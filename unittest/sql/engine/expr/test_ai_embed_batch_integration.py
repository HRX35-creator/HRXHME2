#!/usr/bin/env python3
# Copyright (c) 2026 OceanBase.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""SQL batch regressions with a provider that returns embeddings in reverse order."""

import json
import unittest

import pymysql

from ai_sql_test_utils import AISQLMockTestCase


class AIEmbedBatchIntegrationTest(AISQLMockTestCase):
    @staticmethod
    def vector(text, dimension):
        return [sum(text.encode()) + i / 8 for i in range(dimension)]

    @classmethod
    def default_response(cls, request):
        dimension = request.get("dimensions", 5)
        return {"data": [{"index": i, "embedding": cls.vector(text, dimension)}
                         for i, text in reversed(list(enumerate(request["input"])))]}

    def setUp(self):
        super().setUp()
        self.table = self.model + "_batch"
        self.execute(f"CREATE TABLE `{self.table}` (id INT PRIMARY KEY, s TEXT, d INT, m VARCHAR(128))")
        self.addCleanup(self.execute, f"DROP TABLE `{self.table}`")
        self.inputs = ["first", None, "世界", "fourth", "fifth", "sixth", "last"]
        for i, text in enumerate(self.inputs, 1):
            self.execute(f"INSERT INTO `{self.table}` VALUES (%s,%s,3,%s)", (i, text, self.model))

    def rows(self, suffix="", dimension="", batch_size=64):
        return self.execute(
            f"SELECT /*+ OPT_PARAM('ROWSETS_MAX_ROWS', {batch_size}) */ "
            f"id, ai_embed(m,s{dimension}) FROM `{self.table}` {suffix}")

    def assert_rows(self, rows, expected):
        actual = [(i, json.loads(value) if value is not None else None) for i, value in rows]
        self.assertEqual(expected, actual)

    def requests(self):
        requests = []
        while not self.server.requests.empty():
            requests.append(self.server.requests.get_nowait())
        return requests

    def expected(self, dimension=5, ids=None):
        return [(i, self.vector(text, dimension) if text is not None else None)
                for i, text in enumerate(self.inputs, 1) if ids is None or i in ids]

    def test_table_scan_sends_one_request_and_restores_order(self):
        self.assert_rows(self.rows("ORDER BY id"), self.expected())
        requests = self.requests()
        self.assertEqual(1, len(requests))
        self.assertEqual([s for s in self.inputs if s is not None], requests[0]["input"])

    def test_filter_and_limit(self):
        self.assert_rows(self.rows("WHERE id >= 3 ORDER BY id LIMIT 2"), self.expected(ids={3, 4}))
        requests = self.requests()
        self.assertEqual(1, len(requests))
        self.assertEqual(self.inputs[2:4], requests[0]["input"])

    def test_all_null_rows_skip_http(self):
        self.execute(f"UPDATE `{self.table}` SET s=NULL")
        self.assert_rows(self.rows("ORDER BY id"), [(i, None) for i in range(1, 8)])
        self.assertEqual([], self.requests())

    def test_case_skip_mask(self):
        rows = self.execute(
            f"SELECT /*+ OPT_PARAM('ROWSETS_MAX_ROWS',64) */ id, "
            f"CASE WHEN MOD(id,2)=0 THEN ai_embed(m,s) ELSE NULL END "
            f"FROM `{self.table}` ORDER BY id")
        self.assert_rows(rows, [(i, self.vector(s, 5) if i % 2 == 0 and s is not None else None)
                                for i, s in enumerate(self.inputs, 1)])
        requests = self.requests()
        self.assertEqual(1, len(requests))
        self.assertEqual(["fourth", "sixth"], requests[0]["input"])

    def test_multiple_execution_batches(self):
        self.assert_rows(self.rows("ORDER BY id", dimension=",3", batch_size=3), self.expected(3))
        requests = self.requests()
        self.assertGreater(len(requests), 1)
        self.assertTrue(any(len(request["input"]) > 1 for request in requests))
        self.assertTrue(all(1 <= len(request["input"]) <= 3 for request in requests))
        self.assertEqual([s for s in self.inputs if s is not None],
                         [s for request in requests for s in request["input"]])

    def test_per_row_dimensions_are_grouped(self):
        self.execute(f"UPDATE `{self.table}` SET d=1 WHERE MOD(id,2)=1")
        self.assert_rows(self.rows("ORDER BY id", dimension=",d"),
                         [(i, self.vector(s, 1 if i % 2 else 3) if s is not None else None)
                          for i, s in enumerate(self.inputs, 1)])
        requests = self.requests()
        self.assertEqual(2, len(requests))
        self.assertEqual({1, 3}, {request["dimensions"] for request in requests})
        self.assertEqual(6, sum(len(request["input"]) for request in requests))

    def test_invalid_dimension_prevents_batch_http(self):
        self.execute(f"UPDATE `{self.table}` SET d=0 WHERE id=7")
        with self.assertRaises(pymysql.MySQLError):
            self.rows("ORDER BY id", dimension=",d")
        self.assertEqual([], self.requests())
        self.assertEqual(((1,),), self.execute("SELECT 1"))

    def test_per_row_models_use_separate_requests(self):
        model = self.model + "_other"
        endpoint = model + "_ep"
        self.execute("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)",
                     (model, json.dumps({"type": "dense_embedding", "model_name": "other-mock"})))
        self.addCleanup(self.execute, "CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)", (model,))
        self.execute("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)", (endpoint, json.dumps({
            "ai_model_name": model, "scope": "all", "provider": "openai", "access_key": "mock-key",
            "url": f"http://127.0.0.1:{self.server.server_port}/other-embeddings"})))
        self.addCleanup(self.execute, "CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)", (endpoint,))
        self.execute(f"UPDATE `{self.table}` SET m=%s WHERE MOD(id,2)=0", (model,))
        self.assert_rows(self.rows("ORDER BY id"), self.expected())
        requests = self.requests()
        self.assertEqual(2, len(requests))
        by_model = {request["model"]: request["input"] for request in requests}
        self.assertEqual(["fourth", "sixth"], by_model["other-mock"])
        self.assertEqual(["first", "世界", "fifth", "last"], by_model["local-mock"])

    def test_duplicate_batch_indices_are_rejected(self):
        self.respond({"data": [{"index": 0, "embedding": [i, 2]}
                               for i in range(6)]})
        with self.assertRaises(pymysql.MySQLError) as error:
            self.rows()
        self.assertNotIn(error.exception.args[0], (2003, 2006, 2013))
        self.assertEqual(((1,),), self.execute("SELECT 1"))

    def test_inconsistent_default_dimensions_are_rejected(self):
        texts = [s for s in self.inputs if s is not None]
        self.respond({"data": [{"index": i, "embedding": self.vector(s, 2 if i == 0 else 3)}
                               for i, s in enumerate(texts)]})
        with self.assertRaises(pymysql.MySQLError) as error:
            self.rows()
        self.assertNotIn(error.exception.args[0], (2003, 2006, 2013))
        self.assertEqual(((1,),), self.execute("SELECT 1"))

    def test_failed_batch_does_not_poison_next_query(self):
        self.respond({"error": {"message": "intentional mock failure"}}, status=400)
        with self.assertRaises(pymysql.MySQLError) as error:
            self.rows()
        self.assertNotIn(error.exception.args[0], (2003, 2006, 2013))
        self.requests()
        self.assert_rows(self.rows("ORDER BY id"), self.expected())
        self.assertEqual(1, len(self.requests()))

    def test_transient_http_error_retries_the_same_batch(self):
        self.respond({"error": {"message": "temporary mock failure"}}, status=500)
        self.assert_rows(self.rows("ORDER BY id"), self.expected())
        requests = self.requests()
        self.assertEqual(2, len(requests))
        self.assertEqual(requests[0], requests[1])
        self.assertEqual([s for s in self.inputs if s is not None], requests[0]["input"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
