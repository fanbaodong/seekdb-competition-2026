"""Additional AI-column runtime regressions; run against a disposable local DB.

Usage: python test_ai_column_runtime.py --port 2895
Requires PyMySQL. Model calls use local HTTP mocks only.
"""
import argparse
from collections import Counter
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import threading
import time

import pymysql


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', type=int, default=2895)
    args = parser.parse_args()
    calls = Counter()
    lock = threading.Lock()
    servers = []
    threads = []
    constant_prompt = "literal {0}, quote ', backslash \\ and 中文"
    block_constant_request = threading.Event()
    constant_request_started = threading.Event()
    release_constant_request = threading.Event()

    def handler(name):
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                prompt = next(m['content'] for m in body['messages'] if m['role'] == 'user')
                with lock:
                    calls[(name, prompt)] += 1
                    attempt = calls[(name, prompt)]
                if prompt == constant_prompt and block_constant_request.is_set():
                    constant_request_started.set()
                    if not release_constant_request.wait(15):
                        raise TimeoutError('constant request was not released by the test')
                time.sleep(0.12)
                if name == 'b' and attempt == 1:
                    status, response = 400, {'error': {'message': 'transient test failure'}}
                else:
                    status = 200
                    content = 'invalid-number' if prompt == 'invalid-number' else name + ':' + prompt
                    response = {'choices': [{'message': {'content': content}}]}
                payload = json.dumps(response).encode()
                self.send_response(status)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
        return Handler

    db = pymysql.connect(host='127.0.0.1', port=args.port, user='root', database='oceanbase',
                         autocommit=True, read_timeout=30, charset='utf8mb4')
    cur = db.cursor()
    try:
        cur.execute('DROP DATABASE IF EXISTS ai_column_extra_regression')
        cur.execute('CREATE DATABASE ai_column_extra_regression')
        for name in ('a', 'b'):
            server = ThreadingHTTPServer(('127.0.0.1', 0), handler(name))
            servers.append(server)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            threads.append(thread)
            model, endpoint = 'ai_extra_' + name, 'ai_extra_' + name + '_ep'
            for statement in ('DROP_AI_MODEL_ENDPOINT', 'DROP_AI_MODEL'):
                try:
                    cur.execute(f'CALL DBMS_AI_SERVICE.{statement}(%s)',
                                (endpoint if statement.endswith('ENDPOINT') else model,))
                except pymysql.MySQLError as e:
                    if e.args[0] not in (11112, 11118):
                        raise
            cur.execute('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)',
                        (model, json.dumps({'type': 'completion', 'model_name': model})))
            cur.execute('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)',
                        (endpoint, json.dumps(dict(ai_model_name=model, provider='openai',
                         access_key='local-test-only', request_model_name=model,
                         url=f'http://127.0.0.1:{server.server_port}/v1/chat/completions'))))
        cur.execute('''CREATE TABLE ai_column_extra_regression.docs(
            id BIGINT PRIMARY KEY, content VARCHAR(200),
            a VARCHAR(400) AI COLUMN (AI_COMPLETE('ai_extra_a', content))
                WITH(sync_mode='async',max_retries=2),
            b VARCHAR(400) AI COLUMN (AI_COMPLETE('ai_extra_b', content))
                WITH(sync_mode='async',max_retries=2))''')
        prompt = "backslash \\ and quote ' and newline\n中文"
        cur.execute('INSERT INTO ai_column_extra_regression.docs(id,content) VALUES(1,%s)', (prompt,))
        deadline = time.monotonic() + 20
        while True:
            cur.execute('SELECT a,b FROM ai_column_extra_regression.docs WHERE id=1')
            a, b = cur.fetchone()
            assert (a is None) == (b is None), ('partial group publication', a, b)
            if a is not None:
                assert a == 'a:' + prompt and b == 'b:' + prompt, (a, b)
                break
            assert time.monotonic() < deadline, 'mixed endpoint group did not finish'
            time.sleep(0.03)
        assert calls[('a', prompt)] == 1, dict(calls)
        assert calls[('b', prompt)] == 2, dict(calls)
        print('PASS: mixed endpoints, atomic publication, cached success, byte-preserving result', flush=True)

        for sql in ["UPDATE ai_column_extra_regression.docs SET a='manual' WHERE id=1",
                    "INSERT INTO ai_column_extra_regression.docs(id,content,a) VALUES(2,'x','manual')"]:
            try:
                cur.execute(sql)
            except pymysql.MySQLError as e:
                assert e.args[0] == 1235, e
            else:
                raise AssertionError('manual AI-result assignment was accepted')
        print('PASS: direct AI-result assignments rejected', flush=True)

        cur.execute('START TRANSACTION')
        cur.execute("INSERT INTO ai_column_extra_regression.docs(id,content) VALUES(3,'rolled-back')")
        cur.execute('ROLLBACK')
        time.sleep(0.8)
        assert calls[('a', 'rolled-back')] == 0 and calls[('b', 'rolled-back')] == 0
        print('PASS: rolled-back writes do not call models', flush=True)

        cur.execute('''CREATE TABLE ai_column_extra_regression.constant_prompts(
            id BIGINT PRIMARY KEY,
            answer VARCHAR(400) AI COLUMN (AI_COMPLETE('ai_extra_a',%s))
                WITH(sync_mode='async',max_retries=1))''', (constant_prompt,))
        cur.execute('INSERT INTO ai_column_extra_regression.constant_prompts(id) VALUES(1)')
        deadline = time.monotonic() + 20
        while True:
            cur.execute('SELECT answer FROM ai_column_extra_regression.constant_prompts WHERE id=1')
            answer = cur.fetchone()[0]
            if answer is not None:
                assert answer == 'a:' + constant_prompt, answer
                break
            assert time.monotonic() < deadline, 'constant prompt did not finish'
            time.sleep(0.1)
        assert calls[('a', constant_prompt)] == 1, dict(calls)
        # While the first model request is blocked, delete both rows. Any
        # already-picked sibling must be dropped after checking row existence,
        # and the first response cannot publish into either deleted row.
        block_constant_request.set()
        cur.execute('INSERT INTO ai_column_extra_regression.constant_prompts(id) VALUES(2),(3)')
        try:
            assert constant_request_started.wait(10), 'constant request did not reach mock'
            cur.execute('DELETE FROM ai_column_extra_regression.constant_prompts WHERE id IN (2,3)')
        finally:
            release_constant_request.set()
        # A new task is a completion barrier for the single worker: observing
        # its result proves the earlier in-flight response has been handled.
        cur.execute('INSERT INTO ai_column_extra_regression.constant_prompts(id) VALUES(4)')
        deadline = time.monotonic() + 20
        while True:
            cur.execute('SELECT answer FROM ai_column_extra_regression.constant_prompts WHERE id=4')
            answer = cur.fetchone()[0]
            with lock:
                request_count = calls[('a', constant_prompt)]
            assert request_count <= 3, ('deleted row caused a model request', dict(calls))
            if answer is not None:
                assert answer == 'a:' + constant_prompt, answer
                break
            assert time.monotonic() < deadline, 'constant worker barrier did not finish'
            time.sleep(0.1)
        assert calls[('a', constant_prompt)] == 3, dict(calls)
        cur.execute('SELECT id,answer FROM ai_column_extra_regression.constant_prompts ORDER BY id')
        assert cur.fetchall() == ((1, 'a:' + constant_prompt), (4, 'a:' + constant_prompt))
        print('PASS: literal constant prompt, stale deleted task and late response', flush=True)

        cur.execute('''CREATE TABLE ai_column_extra_regression.bad_types(
            id BIGINT PRIMARY KEY, content VARCHAR(100),
            score BIGINT AI COLUMN (AI_COMPLETE('ai_extra_a',content))
                WITH(sync_mode='async',max_retries=1))''')
        cur.execute("INSERT INTO ai_column_extra_regression.bad_types(id,content) VALUES(1,'invalid-number')")
        deadline = time.monotonic() + 20
        while True:
            cur.execute('''SELECT count(*) FROM oceanbase.__all_ai_column_task task
                JOIN oceanbase.__all_table t ON task.table_id=t.table_id
                JOIN oceanbase.__all_database d ON t.database_id=d.database_id
                WHERE d.database_name='ai_column_extra_regression' AND t.table_name='bad_types' ''')
            if cur.fetchone()[0] == 0:
                break
            assert time.monotonic() < deadline, 'conversion failure was not bounded'
            time.sleep(0.1)
        cur.execute('SELECT score FROM ai_column_extra_regression.bad_types')
        assert cur.fetchone() == (None,)
        assert calls[('a', 'invalid-number')] == 2, dict(calls)
        print('PASS: invalid typed output exhausts bounded retries and stays NULL', flush=True)
    finally:
        release_constant_request.set()
        cur.execute('DROP DATABASE IF EXISTS ai_column_extra_regression')
        for name in ('a', 'b'):
            for method, obj in [('DROP_AI_MODEL_ENDPOINT', 'ai_extra_' + name + '_ep'),
                                ('DROP_AI_MODEL', 'ai_extra_' + name)]:
                try:
                    cur.execute(f'CALL DBMS_AI_SERVICE.{method}(%s)', (obj,))
                except pymysql.MySQLError:
                    pass
        db.close()
        for server in servers:
            server.shutdown()
            server.server_close()
        for thread in threads:
            thread.join(timeout=2)


if __name__ == '__main__':
    main()
