# AI column integration

This branch builds on `b4d90d3a2d37abfb6b70653882c2a50282b8dd4c`, which contains
the previously evaluated AI function and File SQL implementations.

## Source attribution

The initial AI-column implementation was selectively ported from the public
Apache-2.0 contribution by GitHub user `r1ck9-2q`:

- https://github.com/oceanbase/seekdb/pull/2249
- Reference commit: `aed94823356b320ac2012d3db4464a98c2ac8f23`

Original copyright and license notices are retained. This is an adaptation of
that contribution, not a claim that its original code was authored here. The
reference's AI-function, File SQL, performance and unrelated changes were not
imported. Its reported test results are not evidence for this branch.

## Integration changes

- Merge parser tokens and system variables with the existing File SQL feature.
- Register durable AI-column metadata, hidden columns and the task catalog.
- Add DML hooks, asynchronous model evaluation, retries and strict reads.
- Propagate task write errors to the business transaction instead of losing work.
- Use timestamp values for task eligibility and retain microsecond backoff.
- Resolve each column's model endpoint independently and decode completion text
  without adding an extra JSON string layer.
- Keep publication and task cleanup in one transaction, invalidate superseded
  requests, and recheck request ownership during publication.
- Put shared SQL hook declarations in the neutral share module.
- Preserve byte-safe SQL literals and release per-operator task allocations.

## Validation (2026-10-30)

The integrated source was compiled and linked in WSL Ubuntu with Clang 17 and
the project's Release-compatible CMake configuration, using two build jobs.
All five unmodified AI-column mysqltests passed:

- `ai_column_ddl`
- `ai_column_t1_dml`
- `ai_column_worker`
- `ai_column_vector_index`
- `ai_column_retry_strict`

`unittest/sql/engine/basic/test_ai_column_runtime.py` also passed: mixed
endpoints, atomic group publication, cached successful responses during retries,
special-character output, direct result-write rejection, transaction rollback,
and bounded retries for invalid typed responses.

Regression checks passed for `ai_function_enhancement`, `file_sql_basic`, and the
existing CSV/JSONL/Parquet/COPY runtime coverage. The File SQL runner substitutes
local fixture setup for its remote SSH setup; its SQL assertions and expected
results are unchanged. AI-column and AI-function test inputs and expected outputs
are unchanged.

One discovered RETRY defect was fixed after the first test run: retrying a row
again left its older request alive. Manual retry now atomically deletes the old
generation, invalidates the result, and inserts the new task through a dedicated
transactional inner session.

Full logs and the final submission fields are retained in the task folder.
These are local test results; the competition platform has not evaluated this
new branch yet.
