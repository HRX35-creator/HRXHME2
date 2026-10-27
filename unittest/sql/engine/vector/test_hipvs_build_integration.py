"""SQL controls for optional hipVS bulk construction on a disposable instance.

Set SEEKDB_TEST_SOCKET or SEEKDB_TEST_HOST/PORT and SEEKDB_TEST_DATABASE.
HIPVS_TEST_EXPECT is cpu (off/auto) or unavailable (required/invalid with no
bridge). Each mode needs a freshly started server; options are process cached.
This suite is a functional control, not GPU performance or recall evidence.
"""
import json
import os
import random
import unittest
import uuid

import pymysql


class HipvsBuildIntegrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        args = dict(user=os.getenv('SEEKDB_TEST_USER', 'root'),
                    password=os.getenv('SEEKDB_TEST_PASSWORD', ''),
                    database=os.getenv('SEEKDB_TEST_DATABASE', 'test'),
                    charset='utf8mb4', autocommit=True, connect_timeout=5,
                    read_timeout=120)
        if os.getenv('SEEKDB_TEST_SOCKET'):
            args['unix_socket'] = os.environ['SEEKDB_TEST_SOCKET']
        else:
            args.update(host=os.getenv('SEEKDB_TEST_HOST', '127.0.0.1'),
                        port=int(os.getenv('SEEKDB_TEST_PORT', '2881')))
        cls.connection = pymysql.connect(**args)
        cls.expect = os.getenv('HIPVS_TEST_EXPECT', 'cpu')
        if cls.expect not in ('cpu', 'unavailable'):
            raise ValueError('HIPVS_TEST_EXPECT must be cpu or unavailable')

    @classmethod
    def tearDownClass(cls):
        cls.connection.close()

    def setUp(self):
        self.table = 'hipvs_control_' + uuid.uuid4().hex[:12]
        with self.connection.cursor() as cursor:
            cursor.execute('SET ob_query_timeout=60000000')
            cursor.execute('SET ob_hnsw_ef_search=128')
            cursor.execute(f'CREATE TABLE {self.table} '
                           '(id BIGINT PRIMARY KEY, embedding VECTOR(16)) ORGANIZATION HEAP')

    def tearDown(self):
        with self.connection.cursor() as cursor:
            cursor.execute(f'DROP TABLE IF EXISTS {self.table}')

    def load_rows(self):
        rng = random.Random(20261027)
        self.rows = [(i - 1024 if i % 2 else (1 << 54) + i,
                      json.dumps([rng.uniform(-1, 1) for _ in range(16)]))
                     for i in range(2048)]
        with self.connection.cursor() as cursor:
            for start in range(0, len(self.rows), 256):
                cursor.executemany(f'INSERT INTO {self.table} VALUES(%s,%s)',
                                   self.rows[start:start + 256])

    def build(self, kind='hnsw'):
        with self.connection.cursor() as cursor:
            cursor.execute(f'CREATE VECTOR INDEX idx_hipvs ON {self.table}(embedding) '
                           f'WITH(distance=l2,type={kind},lib=vsag,m=16,'
                           'ef_construction=128,sync_mode=immediate)')
            cursor.execute(f'ANALYZE TABLE {self.table}')
            cursor.fetchall()

    def nearest(self, vector, explain=False):
        with self.connection.cursor() as cursor:
            cursor.execute(('EXPLAIN ' if explain else '') +
                           f'SELECT id FROM {self.table} ORDER BY '
                           'l2_distance(embedding,%s) APPROXIMATE LIMIT 1', (vector,))
            return cursor.fetchall()

    def check_queries(self):
        self.assertIn('VECTOR INDEX', '\n'.join(str(row[0])
                      for row in self.nearest(self.rows[0][1], explain=True)))
        for identifier, vector in self.rows[::23]:
            self.assertEqual(((identifier,),), self.nearest(vector))
        # New rows and changed vectors use the ordinary native mutation path.
        new_id = -(1 << 62)
        vector = json.dumps([3.0] * 16)
        changed = json.dumps([-3.0] * 16)
        with self.connection.cursor() as cursor:
            cursor.execute(f'INSERT INTO {self.table} VALUES(%s,%s)', (new_id, vector))
            self.assertEqual(((new_id,),), self.nearest(vector))
            cursor.execute(f'UPDATE {self.table} SET embedding=%s WHERE id=%s',
                           (changed, new_id))
            self.assertEqual(((new_id,),), self.nearest(changed))
            cursor.execute(f'DELETE FROM {self.table} WHERE id=%s', (new_id,))
        self.assertNotIn((new_id,), self.nearest(changed))

    def test_empty_snapshot_retains_native_dml(self):
        self.build()
        vector = json.dumps([0.25] * 16)
        with self.connection.cursor() as cursor:
            cursor.execute(f'INSERT INTO {self.table} VALUES(-7,%s)', (vector,))
        self.assertEqual(((-7,),), self.nearest(vector))

    def test_plain_bulk_build_or_explicit_failure(self):
        self.load_rows()
        if self.expect == 'unavailable':
            with self.assertRaises(pymysql.MySQLError) as caught:
                self.build()
            # Connection loss/client timeouts are not a valid explicit rejection.
            self.assertNotIn(caught.exception.args[0], (2006, 2013))
            self.assertNotIn('timeout', str(caught.exception).lower())
            with self.connection.cursor() as cursor:
                cursor.execute(f'SELECT COUNT(*) FROM {self.table}')
                self.assertEqual((2048,), cursor.fetchone())
        else:
            self.build()
            self.check_queries()

    def test_quantized_bulk_build_remains_native(self):
        self.load_rows()
        self.build('hnsw_sq')
        self.check_queries()


if __name__ == '__main__':
    unittest.main()
