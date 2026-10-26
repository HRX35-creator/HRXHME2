#!/usr/bin/env python3
# Copyright (c) 2026 OceanBase.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Native File SQL contract tests. Run only against an isolated local instance.

Requires PyMySQL and PyArrow (fixture generation only). Uses SEEKDB_TEST_*.
Changes secure_file_priv for the duration of the class and restores its value.
"""

import csv
import datetime
import json
import os
from pathlib import Path
import tempfile
import threading
import time
import unittest

import pyarrow as pa
import pyarrow.parquet as pq
import pymysql


class FileSQLIntegrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        options = {"host": os.environ.get("SEEKDB_TEST_HOST", "127.0.0.1"),
                   "port": int(os.environ.get("SEEKDB_TEST_PORT", "2881")),
                   "user": os.environ.get("SEEKDB_TEST_USER", "root"),
                   "password": os.environ.get("SEEKDB_TEST_PASSWORD", ""),
                   "database": os.environ.get("SEEKDB_TEST_DATABASE", "test"),
                   "charset": "utf8mb4", "autocommit": True, "read_timeout": 60}
        if os.environ.get("SEEKDB_TEST_SOCKET"):
            options["unix_socket"] = os.environ["SEEKDB_TEST_SOCKET"]
        cls.connection_options = options
        cls.connection = pymysql.connect(**options)
        cls.addClassCleanup(cls.connection.close)
        cls.directory = tempfile.TemporaryDirectory(prefix="seekdb-file-sql-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.root = Path(cls.directory.name).resolve()
        cls.default_enabled = cls.execute("SELECT @@session.enable_file_sql")[0][0]
        cls.original_secure_path = cls.execute("SELECT @@global.secure_file_priv")[0][0]
        cls.addClassCleanup(cls.execute, "SET GLOBAL secure_file_priv=%s", (cls.original_secure_path,))
        cls.execute("SET GLOBAL secure_file_priv=%s", (str(cls.root),))
        cls.csv_path = cls.root / "data.csv"
        with cls.csv_path.open("w", newline="", encoding="utf-8") as output:
            writer = csv.writer(output)
            writer.writerow(["id", "name", "score", "active", "day", "created", "note", "id"])
            writer.writerows([
                [1,"Alice",98.5,"true","2026-08-01","2026-08-01 10:11:12","hello,\nworld",101],
                [2,"世界",87,"false","","","plain",102],
                [3,"Chen",90.25,"true","2026-08-03","2026-08-03T12:13:14",'say "hi"',103]])
        cls.json_path = cls.root / "data.jsonl"
        cls.json_path.write_text('\n'.join(json.dumps(row,ensure_ascii=False) for row in [
            {"id":1,"name":"Alice","score":98.5,"active":True},
            {"id":2,"name":"世界","active":False,"day":"2026-08-02"},
            {"id":3,"name":"Chen","score":90,"active":True,"note":"ok"}])+"\n")
        cls.parquet_path = cls.root / "data.parquet"
        pq.write_table(pa.table({"id":pa.array([1,2,3,4,5],type=pa.int64()),
                                 "name":["one",None,"世界","four","five"],
                                 "value":pa.array([1.5,None,-2.5,0,4],type=pa.float64()),
                                 "active":[True,False,None,True,False]}),
                       cls.parquet_path,row_group_size=2,compression="snappy",use_dictionary=True)

    @classmethod
    def execute(cls, statement, args=()):
        with cls.connection.cursor() as cursor:
            cursor.execute(statement,args)
            return cursor.fetchall() if cursor.description else ()

    def setUp(self):
        self.execute("SET SESSION enable_file_sql=ON")
        self.execute("SET GLOBAL secure_file_priv=%s", (str(self.root),))

    def write(self,name,content):
        path=self.root/name
        path.write_text(content,encoding="utf-8")
        return str(path)

    def test_default_off_and_gate(self):
        self.assertEqual(0,self.default_enabled)
        self.execute("SET SESSION enable_file_sql=OFF")
        for function in ("read_csv","read_json","read_ndjson","read_parquet","glob"):
            with self.subTest(function=function):
                with self.assertRaises(pymysql.MySQLError) as error:
                    self.execute(f"SELECT * FROM {function}(%s)",(str(self.root/"missing.file"),))
                self.assertEqual(1235,error.exception.args[0])

    def test_csv_types_multiline_unicode_and_duplicate_headers(self):
        rows=self.execute("SELECT id,name,score,active,day,created,note,id__2 FROM read_csv(%s) f ORDER BY id",
                          (str(self.csv_path),))
        self.assertEqual((1,"Alice",98.5,1,datetime.date(2026,8,1),datetime.datetime(2026,8,1,10,11,12),"hello,\nworld",101),rows[0])
        self.assertEqual((2,"世界",87,0,None,None,"plain",102),rows[1])
        self.assertEqual('say "hi"',rows[2][6])

    def test_describe_select(self):
        rows=self.execute("DESCRIBE SELECT * FROM read_csv(%s)",(str(self.csv_path),))
        self.assertEqual(["id","name","score","active","day","created","note","id__2"],[row[0] for row in rows])
        self.assertEqual(["BIGINT","VARCHAR","DOUBLE","BOOLEAN","DATE","DATETIME","VARCHAR","BIGINT"],
                         [row[1] for row in rows])
        self.assertEqual(["NO","NO","NO","NO","YES","YES","NO","NO"],[row[2] for row in rows])

    def test_csv_options_and_headerless_input(self):
        path=self.write("options.csv",'1;"a;b";NULL\r\n2;"line\nnext";3\r\n')
        rows=self.execute("SELECT * FROM read_csv(%s,header=false,delim=';',nullstr='NULL') ORDER BY column0",(path,))
        self.assertEqual(((1,"a;b",None),(2,"line\nnext",3)),rows)

    def test_full_schema_inference_and_numeric_boundaries(self):
        path=self.write("infer.csv","i,n,d,b\n9223372036854775807,,2024-02-29,true\n-9223372036854775808,,2026-02-29,false\n")
        rows=self.execute("SELECT * FROM read_csv(%s)",(path,))
        self.assertEqual(((9223372036854775807,None,"2024-02-29",1),(-9223372036854775808,None,"2026-02-29",0)),rows)
        overflow=self.write("overflow.csv","v\n9223372036854775808\n-9223372036854775809\n")
        self.assertEqual((("9223372036854775808",),("-9223372036854775809",)),self.execute("SELECT v FROM read_csv(%s)",(overflow,)))

    def test_aggregate_filter_having_and_limit(self):
        rows=self.execute("SELECT active,COUNT(*) AS n,SUM(id) AS s FROM read_csv(%s) f "
                          "WHERE score>80 GROUP BY active HAVING COUNT(*)>=1 ORDER BY active DESC LIMIT 1",
                          (str(self.csv_path),))
        self.assertEqual(((1,2,4),),rows)

    def test_json_field_union_and_missing_values(self):
        for function in ("read_json","read_ndjson"):
            with self.subTest(function=function):
                rows=self.execute(f"SELECT id,name,score,active,day,note FROM {function}(%s) ORDER BY id",
                                  (str(self.json_path),))
                self.assertEqual(((1,"Alice",98.5,1,None,None),
                                  (2,"世界",None,0,datetime.date(2026,8,2),None),
                                  (3,"Chen",90,1,None,"ok")),rows)

    def test_json_type_conflict_and_null(self):
        path=self.write("conflict.jsonl",'{"x":1,"n":null}\n{"x":"text"}\n{"x":true,"n":null}\n')
        self.assertEqual((("1",None),("text",None),("true",None)),self.execute("SELECT x,n FROM read_json(%s)",(path,)))

    def test_nested_json_is_rejected(self):
        for content in ('{"x":{"nested":1}}\n','{"x":[1]}\n','[1,2]\n','{"x":1}\n{bad}\n'):
            with self.subTest(content=content):
                path=self.write("bad.jsonl",content)
                with self.assertRaises(pymysql.MySQLError):
                    self.execute("SELECT * FROM read_json(%s)",(path,))
                self.assertEqual(((1,),),self.execute("SELECT 1"))

    def test_parquet_multi_row_group_nullable_and_projection(self):
        rows=self.execute("SELECT id,name,value,active FROM read_parquet(%s) ORDER BY id",(str(self.parquet_path),))
        self.assertEqual(((1,"one",1.5,1),(2,None,None,0),(3,"世界",-2.5,None),(4,"four",0,1),(5,"five",4,0)),rows)
        self.assertEqual(((5,1,5,15),),self.execute("SELECT count(*),min(id),max(id),sum(id) FROM read_parquet(%s)",
                                                   (str(self.parquet_path),)))

    def test_parquet_dates_timestamps_unsigned_boundaries_and_null_type(self):
        path=self.root/'typed.parquet'
        before_epoch=datetime.datetime(1969,12,31,23,59,59,999999)
        recent=datetime.datetime(2026,8,1,10,11,12,123456)
        pq.write_table(pa.table({
            "day":pa.array([datetime.date(2024,2,29),None],type=pa.date32()),
            "micros":pa.array([recent,before_epoch],type=pa.timestamp('us')),
            "nanos":pa.array([recent,before_epoch],type=pa.timestamp('ns')),
            "unsigned":pa.array([2**64-1,0],type=pa.uint64()),
            "empty":pa.nulls(2)}),path,row_group_size=1,compression='snappy')
        self.assertEqual(((datetime.date(2024,2,29),recent,recent,str(2**64-1),None),
                          (None,before_epoch,before_epoch,'0',None)),self.execute(
            "SELECT day,micros,nanos,`unsigned`,`empty` FROM read_parquet(%s)",(str(path),)))

    def test_inner_left_and_cross_format_joins(self):
        rows=self.execute("SELECT c.id,c.name,j.note FROM read_csv(%s) c "
                          "LEFT JOIN read_json(%s) j ON c.id=j.id WHERE c.id>=2 ORDER BY c.id",
                          (str(self.csv_path),str(self.json_path)))
        self.assertEqual(((2,"世界",None),(3,"Chen","ok")),rows)
        self.assertEqual(((3,),),self.execute("SELECT count(*) FROM read_csv(%s) c INNER JOIN read_parquet(%s) p ON c.id=p.id",
                                              (str(self.csv_path),str(self.parquet_path))))

    def test_native_plan_and_rescan(self):
        plan=self.execute("EXPLAIN SELECT sum(c.id) FROM read_csv(%s) c JOIN "
                          "(SELECT 1 AS k UNION ALL SELECT 2) x ON c.id=x.k",(str(self.csv_path),))
        self.assertTrue(plan)
        self.assertEqual(((3,),),self.execute("SELECT sum(c.id) FROM read_csv(%s) c JOIN "
                                              "(SELECT 1 AS k UNION ALL SELECT 2) x ON c.id=x.k",(str(self.csv_path),)))

    def test_glob_only_returns_files(self):
        folder=self.root/"glob-data"
        folder.mkdir(exist_ok=True)
        (folder/"a.csv").write_text("x\n1\n")
        (folder/"b.txt").write_text("text")
        (folder/"subdir").mkdir(exist_ok=True)
        rows=self.execute("SELECT file FROM glob(%s) ORDER BY file",(str(folder/"*"),))
        self.assertEqual(((str(folder/"a.csv"),),(str(folder/"b.txt"),)),rows)

    def test_copy_roundtrip_and_existing_target(self):
        target=self.root/"export.csv"
        target.unlink(missing_ok=True)
        self.execute("COPY (SELECT id,name,note FROM read_csv(%s) ORDER BY id) TO %s "
                     "(FORMAT CSV,HEADER,NULLSTR 'NULL')",(str(self.csv_path),str(target)))
        self.assertEqual(((1,"Alice","hello,\nworld"),(2,"世界","plain"),(3,"Chen",'say "hi"')),
                         self.execute("SELECT * FROM read_csv(%s,nullstr='NULL') ORDER BY id",(str(target),)))
        original=target.read_bytes()
        with self.assertRaises(pymysql.MySQLError):
            self.execute("COPY (SELECT 1) TO %s (FORMAT CSV)",(str(target),))
        self.assertEqual(original,target.read_bytes())

    def test_outside_and_symlink_paths_are_rejected(self):
        with tempfile.TemporaryDirectory(prefix="seekdb-file-outside-") as outside:
            path=Path(outside)/"secret.csv"
            path.write_text("x\n1\n")
            link=self.root/"escape.csv"
            link.unlink(missing_ok=True)
            link.symlink_to(path)
            for candidate in (str(path),str(link)):
                with self.subTest(candidate=candidate):
                    with self.assertRaises(pymysql.MySQLError):
                        self.execute("SELECT * FROM read_csv(%s)",(candidate,))
            with self.assertRaises(pymysql.MySQLError):
                self.execute("COPY (SELECT 1) TO %s (FORMAT CSV)",(str(Path(outside)/"output.csv"),))

    def test_secure_path_disabled_still_blocks(self):
        self.execute("SET GLOBAL secure_file_priv=NULL")
        with self.assertRaises(pymysql.MySQLError):
            self.execute("SELECT * FROM read_csv(%s)",(str(self.csv_path),))

    def test_bad_csv_and_unsupported_sampling(self):
        for content in ('a,b\n1\n','a,b\n1,2,3\n','a,b\n"unterminated,2\n'):
            with self.subTest(content=content):
                path=self.write("bad.csv",content)
                with self.assertRaises(pymysql.MySQLError):
                    self.execute("SELECT * FROM read_csv(%s)",(path,))
                self.assertEqual(((1,),),self.execute("SELECT 1"))
        with self.assertRaises(pymysql.MySQLError):
            self.execute("SELECT * FROM read_csv(%s,sample_size=10)",(str(self.csv_path),))

    def test_many_rows_cross_batch_boundaries_and_nested_loop_rescan(self):
        path=self.write("many.csv","id,name\n"+"".join(f'{i},row-{i:05d}-世界\n' for i in range(2305)))
        self.assertEqual(((2305,2655360,0,2304),),self.execute(
            "SELECT COUNT(*),SUM(id),MIN(id),MAX(id) FROM read_csv(%s)",(path,)))
        rows=self.execute("SELECT id,name FROM read_csv(%s) ORDER BY id DESC LIMIT 3",(path,))
        self.assertEqual(tuple((i,f'row-{i:05d}-世界') for i in (2304,2303,2302)),rows)
        right=self.write("right.csv","id,label\n0,zero\n1,one\n2,two\n")
        rows=self.execute("SELECT /*+ USE_NL(b) */ a.id,b.label FROM read_csv(%s) a "
                          "JOIN read_csv(%s) b ON a.id=b.id ORDER BY a.id",(path,right))
        self.assertEqual(((0,"zero"),(1,"one"),(2,"two")),rows)

    def test_file_join_with_an_ordinary_table(self):
        path=self.write("ordinary-join.csv","id,label\n"+"".join(f'{i},label-{i}\n' for i in range(1500)))
        self.execute("DROP TABLE IF EXISTS file_sql_join_numbers")
        self.addCleanup(self.execute,"DROP TABLE IF EXISTS file_sql_join_numbers")
        self.execute("CREATE TABLE file_sql_join_numbers(id BIGINT PRIMARY KEY)")
        self.execute("INSERT INTO file_sql_join_numbers VALUES "+','.join(f'({i})' for i in range(1500)))
        self.execute("ANALYZE TABLE file_sql_join_numbers")
        self.assertEqual(((1500,1124250),),self.execute(
            "SELECT COUNT(*),SUM(f.id) FROM file_sql_join_numbers t JOIN read_csv(%s) f ON t.id=f.id",(path,)))

    def test_describe_and_explain_ordinary_queries(self):
        self.assertTrue(self.execute("EXPLAIN SELECT 1"))
        self.assertTrue(self.execute("DESCRIBE SELECT 1"))
        self.assertEqual(((1,),),self.execute("SELECT 1"))

    def test_file_change_aborts_an_active_scan(self):
        path=Path(self.write("changing.csv","id\n"+"".join(f'{i}\n' for i in range(5000))))
        self.assertEqual(((5000,),),self.execute("SELECT COUNT(*) FROM read_csv(%s)",(str(path),)))
        ready=threading.Event()
        result=[]
        def query():
            try:
                with pymysql.connect(**self.connection_options) as connection:
                    with connection.cursor() as cursor:
                        cursor.execute("SET SESSION enable_file_sql=ON")
                        ready.set()
                        cursor.execute("SELECT SLEEP(0.001),id FROM read_csv(%s)",(str(path),))
                        result.append(cursor.fetchall())
            except pymysql.MySQLError as error:
                result.append(error)
        worker=threading.Thread(target=query)
        worker.start()
        self.assertTrue(ready.wait(5))
        time.sleep(0.15)
        with path.open("a") as output: output.write("5000\n")
        worker.join(10)
        self.assertFalse(worker.is_alive())
        self.assertEqual(1,len(result))
        self.assertIsInstance(result[0],pymysql.MySQLError)
        self.assertEqual(((1,),),self.execute("SELECT 1"))

    def test_copy_table_options_and_null_text_are_reversible(self):
        self.execute("DROP TABLE IF EXISTS file_copy_values")
        self.addCleanup(self.execute,"DROP TABLE IF EXISTS file_copy_values")
        self.execute("CREATE TABLE file_copy_values(id BIGINT, v VARCHAR(100))")
        self.execute("INSERT INTO file_copy_values VALUES(1,NULL),(2,''),(3,'NULL'),(4,%s),(5,%s)",
                     ("a;b'c",'line\nnext'))
        target=self.root/"options-export.csv"
        target.unlink(missing_ok=True)
        self.execute("COPY file_copy_values TO %s (FORMAT CSV,HEADER TRUE,DELIMITER ';',QUOTE %s,ESCAPE %s,NULLSTR 'NULL')",
                     (str(target),"'","\\"))
        self.assertEqual(((1,None),(2,''),(3,'NULL'),(4,"a;b'c"),(5,'line\nnext')),
            self.execute("SELECT * FROM read_csv(%s,delimiter=';',quote=%s,escape=%s,nullstr='NULL') ORDER BY id",
                         (str(target),"'","\\")))

    def test_copy_query_failure_cleans_up_unpublished_output(self):
        target=self.root/"failed-export.csv"
        target.unlink(missing_ok=True)
        before=set(self.root.glob('.seekdb-copy-*.tmp'))
        with self.assertRaises(pymysql.MySQLError):
            self.execute("COPY (SELECT id,CAST(JSON_EXTRACT(CASE WHEN id=3 THEN 'invalid' ELSE '{\"v\":1}' END,'$.v') AS CHAR) AS v "
                         "FROM read_csv(%s) ORDER BY id) TO %s (FORMAT CSV,HEADER)",(str(self.csv_path),str(target)))
        self.assertFalse(target.exists())
        self.assertEqual(before,set(self.root.glob('.seekdb-copy-*.tmp')))

    def test_nonregular_inputs_and_invalid_utf8_are_rejected(self):
        fifo=self.root/'input.fifo'
        if not fifo.exists(): os.mkfifo(fifo)
        with self.assertRaises(pymysql.MySQLError):
            self.execute("SELECT * FROM read_csv(%s)",(str(fifo),))
        invalid=self.root/'invalid-utf8.csv'
        invalid.write_bytes(b'v\n\xff\n')
        with self.assertRaises(pymysql.MySQLError):
            self.execute("SELECT * FROM read_csv(%s)",(str(invalid),))
        for content in ('{"v":['+'0,'*10000+'0]}\n','{"v":{"nested":1}}\n'):
            path=self.write('nested-large.jsonl',content)
            with self.assertRaises(pymysql.MySQLError):
                self.execute("SELECT * FROM read_json(%s)",(path,))


if __name__ == "__main__":
    unittest.main(verbosity=2)
