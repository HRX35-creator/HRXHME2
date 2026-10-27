"""Native differential coverage for score-free MATCH scalar aggregation.

Use an isolated seekdb instance via SEEKDB_TEST_SOCKET or HOST/PORT. The scored
numeric predicate is deliberately retained as a semantic control; plan checks
verify that observed scores and implicit ranked LIMIT scans remain scored.
"""
import os
import string
import unittest
import uuid

import pymysql


def connect():
    arguments = dict(user=os.getenv('SEEKDB_TEST_USER', 'root'),
                     password=os.getenv('SEEKDB_TEST_PASSWORD', ''),
                     charset='utf8mb4', connect_timeout=5,
                     read_timeout=120, autocommit=True)
    if os.getenv('SEEKDB_TEST_SOCKET'):
        arguments['unix_socket'] = os.environ['SEEKDB_TEST_SOCKET']
    else:
        arguments.update(host=os.getenv('SEEKDB_TEST_HOST', '127.0.0.1'),
                         port=int(os.getenv('SEEKDB_TEST_PORT', '2881')))
    return pymysql.connect(**arguments)


class FTSTruthAggregationIntegrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.connection = connect()
        cls.database = 'fts_truth_' + uuid.uuid4().hex[:12]
        with cls.connection.cursor() as cursor:
            cursor.execute('CREATE DATABASE ' + cls.database)
            cursor.execute('USE ' + cls.database)
            cursor.execute('CREATE TABLE docs (id BIGINT PRIMARY KEY, bucket INT, '
                           'body TEXT, KEY idx_bucket(bucket))')
            patterns = ['quasar nebula quasar', 'quasar comet',
                        'nebula comet', 'dwarf']
            rows = [(i + 1, i % 4, patterns[i % 4]) for i in range(600)]
            rows += [(601, 4, 'QUASAR'), (602, 4, None), (603, 4, '')]
            cursor.executemany('INSERT INTO docs VALUES (%s,%s,%s)', rows)
            cursor.execute('ALTER TABLE docs ADD FULLTEXT INDEX ft_body(body) '
                           'WITH PARSER beng')

    @classmethod
    def tearDownClass(cls):
        try:
            with cls.connection.cursor() as cursor:
                cursor.execute('DROP DATABASE ' + cls.database)
        finally:
            cls.connection.close()

    def rows(self, sql, args=None, connection=None):
        with (connection or self.connection).cursor() as cursor:
            cursor.execute(sql, args)
            return cursor.fetchall()

    def plan(self, sql, args=None):
        return '\n'.join(str(cell) for row in self.rows('EXPLAIN EXTENDED ' + sql, args)
                         for cell in row)

    def compare_count(self, query, suffix='', connection=None):
        direct = self.rows('SELECT COUNT(*) FROM docs WHERE '
                           'MATCH(body) AGAINST(%s)' + suffix, (query,), connection)
        scored = self.rows('SELECT COUNT(*) FROM docs WHERE '
                           'MATCH(body) AGAINST(%s) > 0' + suffix, (query,), connection)
        self.assertEqual(scored, direct)
        return direct[0][0]

    def test_scalar_truth_plan_omits_scores_but_numeric_comparison_keeps_them(self):
        direct = ('SELECT /*+ INDEX(docs ft_body) */ COUNT(*) FROM docs WHERE '
                  'MATCH(body) AGAINST(%s)')
        self.assertIn('calc_relevance=false', self.plan(direct, ('quasar',)))
        self.assertIn('calc_relevance=true', self.plan(direct + ' > 0', ('quasar',)))
        self.assertIn('calc_relevance=true', self.plan(direct + ' > 0.001', ('quasar',)))

    def test_truth_and_scored_predicate_agree_on_input_boundaries(self):
        expected = {'quasar': 301, 'nebula': 300, 'comet': 300, 'dwarf': 150,
                    'quasar nebula': 451, 'quasar quasar': 301, '': 0,
                    'the and of': 0, 'nohitword': 0, None: 0}
        for query, count in expected.items():
            with self.subTest(query=query):
                self.assertEqual(count, self.compare_count(query))

    def test_duplicate_postings_and_multiple_result_batches(self):
        for query in ['quasar nebula comet', 'QUASAR nebula quasar',
                      'comet nebula dwarf', 'quasar,comet', 'quasar 数据库']:
            with self.subTest(query=query):
                self.compare_count(query)
        self.assertEqual(451, self.compare_count('quasar nebula comet'))

    def test_other_column_filter_and_functional_lookup_preserve_truth(self):
        for suffix in [' AND bucket=1', ' AND bucket IN (0,2)',
                       ' AND id BETWEEN 200 AND 520', ' AND body IS NOT NULL']:
            with self.subTest(suffix=suffix):
                self.compare_count('quasar nebula', suffix)
        # Force a normal-index access path so MATCH is evaluated by lookup.
        direct = ('SELECT /*+ INDEX(docs idx_bucket) */ COUNT(*) FROM docs '
                  'WHERE bucket=1 AND MATCH(body) AGAINST(%s)')
        scored = direct + ' > 0'
        self.assertEqual(self.rows(scored, ('quasar',)), self.rows(direct, ('quasar',)))

    def test_long_query_taat_fallback_deduplicates_ids_without_statistics(self):
        alphabet = string.ascii_lowercase
        terms = ['wx' + alphabet[i // (26 * 26)] + alphabet[(i // 26) % 26]
                 + alphabet[i % 26] for i in range(270)]
        query = 'quasar ' + ' '.join(terms)
        direct = 'SELECT COUNT(*) FROM docs WHERE MATCH(body) AGAINST(%s)'
        self.assertIn('calc_relevance=false', self.plan(direct, (query,)))
        self.assertEqual(301, self.compare_count(query))

    def test_projected_aggregate_and_ranked_scores_remain_numeric(self):
        ranked = ("SELECT id,MATCH(body) AGAINST('quasar') AS score FROM docs "
                  "WHERE MATCH(body) AGAINST('quasar') ORDER BY score DESC,id")
        rows = self.rows(ranked)
        self.assertEqual(301, len(rows))
        self.assertTrue(all(float(score) > 0 for _, score in rows))
        self.assertGreater(len({float(score) for _, score in rows}), 1)
        summed = ("SELECT SUM(MATCH(body) AGAINST('quasar')) FROM docs "
                  "WHERE MATCH(body) AGAINST('quasar')")
        self.assertIn('calc_relevance=true', self.plan(summed))
        self.assertAlmostEqual(sum(float(score) for _, score in rows),
                               float(self.rows(summed)[0][0]), places=8)
        limited = ("SELECT COUNT(*) FROM (SELECT id FROM docs WHERE "
                   "MATCH(body) AGAINST('quasar') LIMIT 20) ranked_docs")
        self.assertIn('calc_relevance=true', self.plan(limited))
        self.assertEqual(((20,),), self.rows(limited))

    def test_prepared_query_rescan_uses_current_search_text(self):
        with self.connection.cursor() as cursor:
            cursor.execute('PREPARE fts_truth_stmt FROM %s', (
                'SELECT COUNT(*) FROM docs WHERE MATCH(body) AGAINST(?)',))
            try:
                for query, expected in [('quasar', 301), ('nohitword', 0),
                                        ('nebula', 300), (None, 0), ('quasar', 301)]:
                    cursor.execute('SET @fts_truth_query=%s', (query,))
                    cursor.execute('EXECUTE fts_truth_stmt USING @fts_truth_query')
                    self.assertEqual(((expected,),), cursor.fetchall())
            finally:
                cursor.execute('DEALLOCATE PREPARE fts_truth_stmt')

    def test_transaction_visibility_and_rollback(self):
        reader = connect()
        try:
            with reader.cursor() as cursor:
                cursor.execute('USE ' + self.database)
            self.connection.begin()
            try:
                self.rows("INSERT INTO docs VALUES (9000,1,'quasar')")
                self.assertEqual(302, self.compare_count('quasar'))
                self.assertEqual(301, self.compare_count('quasar', connection=reader))
            finally:
                self.connection.rollback()
            self.assertEqual(301, self.compare_count('quasar'))
            self.assertEqual(301, self.compare_count('quasar', connection=reader))
        finally:
            reader.close()


if __name__ == '__main__':
    unittest.main()
