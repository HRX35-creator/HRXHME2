"""End-to-end file SQL tests against a local seekdb (requires pymysql).

Set FILE_SQL_TEST_DIR to the exact secure_file_priv directory configured at
server startup. FILE_SQL_PORT defaults to 2881; FILE_SQL_PASSWORD is optional.
The suite creates and removes its own fixtures beneath that directory.
"""
import datetime
import json
import os
from pathlib import Path
import shutil
import tempfile
import unittest
import uuid

import pymysql


class FileSql(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.allowed = Path(os.environ["FILE_SQL_TEST_DIR"]).resolve()
        cls.temp = tempfile.TemporaryDirectory(prefix="regression_", dir=cls.allowed)
        cls.root = Path(cls.temp.name)
        cls.conn = pymysql.connect(host="127.0.0.1", port=int(os.getenv("FILE_SQL_PORT", "2881")),
                                   user="root", password=os.getenv("FILE_SQL_PASSWORD", ""),
                                   charset="utf8mb4", autocommit=True)
        cls.cur = cls.conn.cursor()
        cls.database = "file_sql_" + uuid.uuid4().hex
        cls.cur.execute(f"CREATE DATABASE {cls.database}")
        cls.cur.execute(f"USE {cls.database}")
        cls.cur.execute("SHOW VARIABLES LIKE 'secure_file_priv'")
        configured = cls.cur.fetchone()[1]
        if Path(configured).resolve() != cls.allowed:
            raise RuntimeError("FILE_SQL_TEST_DIR must equal secure_file_priv")
        fixtures = Path(__file__).resolve().parents[4] / "tools/deploy/mysql_test/test_suite/file_sql/data"
        for source in fixtures.iterdir():
            shutil.copyfile(source, cls.root / source.name)

    @classmethod
    def tearDownClass(cls):
        cls.cur.execute(f"DROP DATABASE {cls.database}")
        cls.conn.close()
        cls.temp.cleanup()

    def setUp(self):
        self.cur.execute("SET enable_file_sql=1")

    def path(self, name):
        return "'" + (self.root / name).as_posix().replace("'", "''") + "'"

    def query(self, sql):
        self.cur.execute(sql)
        return self.cur.fetchall()

    def test_disabled(self):
        self.query("SET enable_file_sql=0")
        for function, name in [("read_csv", "people.csv"), ("read_json", "cities.jsonl"),
                               ("read_parquet", "values.parquet"), ("glob", "*.csv")]:
            with self.assertRaises(pymysql.MySQLError):
                self.query(f"SELECT * FROM {function}({self.path(name)})")
        with self.assertRaises(pymysql.MySQLError):
            self.query(f"COPY (SELECT 1) TO {self.path('disabled.csv')}")
        self.assertFalse((self.root / "disabled.csv").exists())
        self.assertEqual(self.query("SELECT 7*6"), ((42,),))

    def test_csv_and_native_plan(self):
        table = f"read_csv({self.path('people.csv')})"
        rows = self.query(f"SELECT id,name,active,day,missing FROM {table} ORDER BY id")
        self.assertEqual(rows[0], (1, "\u4e0a\u6d77", 1, datetime.date(2024, 2, 29), None))
        self.assertEqual(rows[1][1], 'quote "two"')
        self.assertEqual(rows[2][1], "line\nbreak")
        self.assertEqual(self.query(f"SELECT count(*),sum(amount),avg(amount),min(amount),max(amount) FROM {table}"),
                         ((3, 30.5, 15.25, 10.5, 20.0),))
        self.assertEqual(self.query(f"SELECT active,count(*) FROM {table} GROUP BY active HAVING count(*)>1"), ((1, 2),))
        self.assertEqual(self.query(f"SELECT id+10 AS n FROM {table} WHERE id>1 ORDER BY id DESC LIMIT 1"), ((13,),))
        schema = self.query(f"DESCRIBE SELECT * FROM {table}")
        self.assertEqual([r[1] for r in schema], ["BIGINT", "VARCHAR", "DOUBLE", "BOOLEAN", "DATE", "NULL"])
        plan = "\n".join(r[0] for r in self.query(f"EXPLAIN SELECT id FROM {table} WHERE id>1"))
        self.assertIn("JSON_TABLE", plan)
        self.assertIn("filter", plan)
        self.assertEqual(self.query("SELECT * FROM JSON_TABLE('[1,2]', '$[*]' COLUMNS (n BIGINT PATH '$')) t"), ((1,), (2,)))

    def test_json_union_and_join(self):
        csv = f"read_csv({self.path('people.csv')})"
        js = f"read_ndjson({self.path('cities.jsonl')})"
        self.assertEqual(self.query(f"SELECT id,extra FROM {js} ORDER BY id"), ((1, None), (3, 1), (4, None)))
        self.assertEqual(self.query(f"SELECT a.id,b.city FROM {csv} a LEFT JOIN {js} b ON a.id=b.id ORDER BY a.id"),
                         ((1, "\u5317\u4eac"), (2, None), (3, "Paris")))
        self.assertEqual(self.query(f"SELECT /*+ USE_NL(a b) */ a.id,b.id FROM {csv} a INNER JOIN {js} b ON a.id=b.id ORDER BY a.id"), ((1, 1), (3, 3)))
        self.assertEqual(self.query(f"SELECT count(*) FROM {csv} a CROSS JOIN {js} b"), ((9,),))

    def test_parquet(self):
        table = f"read_parquet({self.path('values.parquet')})"
        self.assertEqual(self.query(f"SELECT count(*),sum(id),sum(amount) FROM {table}"), ((5, 15, 3.75),))
        rows = self.query(f"SELECT id,name,day,stamp FROM {table} ORDER BY id")
        self.assertEqual(rows[1], (2, None, None, None))
        self.assertEqual(rows[3][3], datetime.datetime(1969, 12, 31, 23, 59, 59, 999999))
        self.assertEqual(self.query(f"SELECT a.id FROM read_csv({self.path('people.csv')}) a JOIN {table} b ON a.id=b.id ORDER BY a.id"), ((1,), (2,), (3,)))
        self.assertEqual(self.query(f"SELECT count(*) FROM glob({self.path('*.parquet')})"), ((1,),))

    def test_inference_and_options(self):
        p = self.root / "types.csv"
        p.write_text("n,d,b,day,stamp,empty\n9223372036854775808,1e100,true,2024-02-29,2024-02-29T23:59:59.123456,\n", encoding="utf8")
        schema = self.query(f"DESCRIBE SELECT * FROM read_csv({self.path('types.csv')},sample_size=-1)")
        self.assertEqual([r[1] for r in schema], ["VARCHAR", "DOUBLE", "BOOLEAN", "DATE", "DATETIME", "NULL"])
        self.assertEqual(self.query(f"SELECT n FROM read_csv({self.path('types.csv')})"), (("9223372036854775808",),))
        p.write_text("n\n" + "1\n" * 1000 + "text\n", encoding="utf8")
        self.assertEqual(self.query(f"SELECT count(*) FROM read_csv({self.path('types.csv')})"), ((1001,),))
        self.assertEqual(self.query(f"DESCRIBE SELECT * FROM read_csv({self.path('types.csv')})")[0][1], "VARCHAR")
        (self.root / "options.csv").write_bytes(b'1|"a\\"b"|NULL|"NULL"\r\n')
        self.assertEqual(self.query(f"SELECT * FROM read_csv({self.path('options.csv')},header=false,delimiter='|',escape='\\\\',nullstr='NULL')"), ((1, 'a"b', None, 'NULL'),))
        # Literal-only options intentionally reject expressions and sample limits.
        for option in ["sample_size=1", "header=2", "delimiter='|' ,delim=','", "escape=CHAR(92)"]:
            with self.assertRaises(pymysql.MySQLError):
                self.query(f"SELECT * FROM read_csv({self.path('people.csv')},{option})")
        (self.root / "dates.jsonl").write_text(json.dumps({"day": "2024-02-29", "number": "123"}) + "\n")
        self.assertEqual([r[1] for r in self.query(f"DESCRIBE SELECT * FROM read_json({self.path('dates.jsonl')})")], ["DATE", "VARCHAR"])

    def test_errors_and_security(self):
        for fn, name in [("read_csv", "bad.csv"), ("read_json", "nested.jsonl"), ("read_parquet", "bad.csv")]:
            with self.assertRaises(pymysql.MySQLError):
                self.query(f"SELECT * FROM {fn}({self.path(name)})")
        with self.assertRaisesRegex(pymysql.MySQLError, r"row 2"):
            self.query(f"SELECT * FROM read_csv({self.path('bad.csv')})")
        with tempfile.TemporaryDirectory() as outside:
            secret = Path(outside) / "secret.csv"
            secret.write_text("secret\n123\n")
            for sql in [f"SELECT * FROM read_csv('{secret.as_posix()}')",
                        f"SELECT * FROM glob('{Path(outside).as_posix()}/*.csv')",
                        f"COPY (SELECT 1) TO '{Path(outside).as_posix()}/output.csv'"]:
                with self.assertRaises(pymysql.MySQLError):
                    self.query(sql)
            link = self.root / "escape.csv"
            try:
                link.symlink_to(secret)
            except OSError:
                pass
            else:
                try:
                    with self.assertRaises(pymysql.MySQLError):
                        self.query(f"SELECT * FROM read_csv({self.path('escape.csv')})")
                finally:
                    link.unlink()
            self.assertEqual(secret.read_text(), "secret\n123\n")
            self.assertFalse((Path(outside) / "output.csv").exists())
        self.assertEqual(self.query("SELECT 1"), ((1,),))

    def test_copy(self):
        out = self.path("output.csv")
        self.query(f"COPY (SELECT id,name,missing FROM read_csv({self.path('people.csv')}) ORDER BY id) TO {out} (FORMAT CSV, HEADER TRUE, DELIMITER '|', NULL 'NULL')")
        self.assertEqual(self.query(f"SELECT * FROM read_csv({out},delimiter='|',nullstr='NULL') ORDER BY id"),
                         ((1, "\u4e0a\u6d77", None), (2, 'quote "two"', None), (3, "line\nbreak", None)))
        before = (self.root / "output.csv").read_bytes()
        with self.assertRaises(pymysql.MySQLError):
            self.query(f"COPY (SELECT 2) TO {out}")
        self.assertEqual((self.root / "output.csv").read_bytes(), before)
        with self.assertRaises(pymysql.MySQLError):
            self.query(f"COPY (SELECT id,JSON_EXTRACT(IF(id=3,'invalid','{{}}'),'$.x') FROM read_csv({self.path('people.csv')})) TO {self.path('failed.csv')}")
        self.assertFalse((self.root / "failed.csv").exists())
        self.assertFalse(list(self.root.glob("*.seekdb-tmp-*")))
        self.query("CREATE TABLE copy_source(id INT, name VARCHAR(30))")
        self.query("INSERT INTO copy_source VALUES (42,'table')")
        self.query(f"COPY copy_source TO {self.path('table.csv')}")
        self.assertEqual(self.query(f"SELECT * FROM read_csv({self.path('table.csv')})"), ((42, "table"),))
        self.query("DROP TABLE copy_source")
        self.query(f"COPY (SELECT id,name FROM read_csv({self.path('people.csv')})) TO {self.path('conventional.csv')} (HEADER TRUE, DELIMITER '|', NULL 'NULL')")
        self.query(f"COPY (SELECT 1 AS id UNION ALL SELECT 2) TO {self.path('union.csv')}")
        self.assertEqual(self.query(f"SELECT id FROM read_csv({self.path('union.csv')}) ORDER BY id"), ((1,), (2,)))



if __name__ == "__main__":
    unittest.main(verbosity=2)
