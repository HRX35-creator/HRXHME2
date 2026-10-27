#!/usr/bin/env python3
"""Transaction/recovery checks for a dedicated instance; destroys tracelog_regression."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import time

import pymysql


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2881)
    parser.add_argument("--phase", choices=["prepare", "verify", "hold"], required=True)
    parser.add_argument("--snapshot", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--revision", required=True)
    args = parser.parse_args()

    def connect():
        return pymysql.connect(
            host=args.host,
            port=args.port,
            user="root",
            password=os.environ.get("SEEKDB_TEST_PASSWORD", ""),
            charset="utf8mb4",
            autocommit=True,
            read_timeout=120,
        )

    db = connect()
    cursor = db.cursor()

    def sql(query, values=None):
        cursor.execute(query, values)
        return cursor.fetchall()

    def one(query):
        return sql(query)[0][0]

    def snapshot():
        return {
            "accounts": sql("select id,balance from accounts order by id"),
            "journal": sql("select id,amount from journal order by id"),
        }

    if args.phase == "hold":
        sql("use tracelog_regression")
        db.begin()
        sql("update accounts set balance=0")
        sql("insert into accounts values(999999,999999)")
        Path(args.output).write_text(json.dumps({"uncommitted": True}) + "\n")
        # The orchestrator kills this dedicated server after this marker appears.
        while True:
            time.sleep(0.1)
            try:
                sql("select 1")
            except pymysql.MySQLError:
                db.close()
                return

    checks = []
    if args.phase == "verify":
        sql("use tracelog_regression")
        expected = json.loads(Path(args.snapshot).read_text())
        assert json.loads(json.dumps(snapshot())) == expected
        checks.append("committed_state_and_uncommitted_rollback_after_restart")
    else:
        sql("drop database if exists tracelog_regression")
        sql("create database tracelog_regression")
        sql("use tracelog_regression")
        sql("create table accounts(id int primary key,balance bigint not null)")
        sql("create table journal(id int primary key,amount int not null)")
        sql("insert into accounts values(1,10),(2,10)")
        db.begin()
        sql("update accounts set balance=11 where id=1")
        sql("insert into accounts values(3,30)")
        db.commit()
        assert one("select balance from accounts where id=1") == 11
        assert one("select balance from accounts where id=3") == 30
        checks.append("commit_insert_and_update")
        db.begin()
        sql("update accounts set balance=0")
        sql("delete from accounts where id=2")
        sql("insert into accounts values(4,40)")
        db.rollback()
        assert sql("select * from accounts order by id") == (
            (1, 11),
            (2, 10),
            (3, 30),
        )
        checks.append("rollback_insert_update_delete")
        db.begin()
        sql("update accounts set balance=12 where id=1")
        sql("savepoint kept")
        sql("update accounts set balance=999 where id=2")
        sql("insert into accounts values(4,40)")
        sql("rollback to savepoint kept")
        sql("release savepoint kept")
        db.commit()
        assert one("select balance from accounts where id=1") == 12
        assert one("select balance from accounts where id=2") == 10
        assert one("select count(*) from accounts where id=4") == 0
        checks.append("savepoint_partial_rollback_and_commit")
        db.begin()
        sql("update accounts set balance=13 where id=1")
        try:
            sql("insert into accounts values(1,999)")
        except pymysql.IntegrityError as error:
            assert error.args[0] == 1062
        else:
            raise AssertionError("duplicate primary key accepted")
        db.commit()
        assert one("select balance from accounts where id=1") == 13
        checks.append("statement_error_preserves_prior_transaction_write")

        other = connect()
        other_cursor = other.cursor()
        other_cursor.execute("use tracelog_regression")
        db.begin()
        sql("update accounts set balance=999 where id=1")
        other_cursor.execute("select balance from accounts where id=1")
        assert other_cursor.fetchone() == (13,)
        db.rollback()
        checks.append("uncommitted_write_not_visible")
        sql("set session transaction isolation level repeatable read")
        db.begin()
        assert one("select balance from accounts where id=2") == 10
        other_cursor.execute("update accounts set balance=11 where id=2")
        assert one("select balance from accounts where id=2") == 10
        db.commit()
        assert one("select balance from accounts where id=2") == 11
        checks.append("repeatable_read_snapshot")
        sql("set session transaction isolation level read committed")
        db.begin()
        assert one("select balance from accounts where id=2") == 11
        other_cursor.execute("update accounts set balance=12 where id=2")
        assert one("select balance from accounts where id=2") == 12
        db.rollback()
        checks.append("read_committed_refreshes_snapshot")
        other.begin()
        other_cursor.execute("update accounts set balance=999 where id=3")
        other.close()
        assert one("select balance from accounts where id=3") == 30
        sql("update accounts set balance=balance+1 where id=3")
        assert one("select balance from accounts where id=3") == 31
        checks.append("disconnect_rolls_back")

        def worker(number):
            connection = connect()
            cur = connection.cursor()
            cur.execute("use tracelog_regression")
            try:
                for i in range(100):
                    connection.begin()
                    cur.execute("update accounts set balance=balance+1 where id=1")
                    cur.execute(
                        "insert into journal values(%s,1)", (number * 10000 + i,)
                    )
                    if i % 4 == 0:
                        connection.rollback()
                    else:
                        connection.commit()
            finally:
                connection.close()

        with ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(worker, range(4)))
        assert one("select balance from accounts where id=1") == 313
        assert one("select count(*) from journal") == 300
        assert one("select sum(amount) from journal") == 300
        checks.append("400_concurrent_transactions_300_commits_100_rollbacks")
        Path(args.snapshot).write_text(json.dumps(snapshot(), indent=2) + "\n")

    db.close()
    report = {
        "revision": args.revision,
        "phase": args.phase,
        "passed": len(checks),
        "checks": checks,
    }
    Path(args.output).write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report), flush=True)


if __name__ == "__main__":
    main()
