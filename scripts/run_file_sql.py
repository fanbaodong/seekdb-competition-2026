#!/usr/bin/env python3
"""Run one seekdb file-SQL statement and print the rows.

Usage:
    python run_file_sql.py "SELECT COUNT(*) FROM read_csv('/data/a.csv')"

Environment:
    SEEKDB_HOST (default 127.0.0.1)
    SEEKDB_PORT (default 2881)
    SEEKDB_USER (default root)
    SEEKDB_PASSWORD (default empty)
    SEEKDB_DATABASE (default oceanbase)

The script enables `enable_file_sql` on the session before running the query so a
plain statement can be tested directly.
"""

from __future__ import annotations

import os
import sys

import pymysql


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__.strip())
        return 2
    connection = pymysql.connect(
        host=os.environ.get("SEEKDB_HOST", "127.0.0.1"),
        port=int(os.environ.get("SEEKDB_PORT", "2881")),
        user=os.environ.get("SEEKDB_USER", "root"),
        password=os.environ.get("SEEKDB_PASSWORD", ""),
        database=os.environ.get("SEEKDB_DATABASE", "oceanbase"),
        charset="utf8mb4",
        autocommit=True,
    )
    try:
        with connection.cursor() as cursor:
            cursor.execute("SET SESSION enable_file_sql = ON")
            cursor.execute(sys.argv[1])
            rows = cursor.fetchall()
            if cursor.description:
                print("\t".join(column[0] for column in cursor.description))
            for row in rows:
                print("\t".join("NULL" if value is None else str(value) for value in row))
            print(f"-- {len(rows)} row(s)")
    finally:
        connection.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
