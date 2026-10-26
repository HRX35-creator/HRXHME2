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
"""Embedding SQL regressions. Run against an isolated source test instance.

Uses PyMySQL and the SEEKDB_TEST_* connection settings. Malformed responses are
intentional; every error case also verifies that the test server remains usable.
"""

import json
import unittest

import pymysql

from ai_sql_test_utils import AISQLMockTestCase


class AIEmbedIntegrationTest(AISQLMockTestCase):
    @staticmethod
    def vector(text, dimension):
        return [len(text.encode()) + i / 8 for i in range(dimension)]

    @classmethod
    def default_response(cls, request):
        dimension = request.get("dimensions", 5)
        if not isinstance(dimension, int) or not 0 < dimension <= 32768:
            dimension = 5
        return {"data": [{"index": i, "embedding": cls.vector(text, dimension)}
                         for i, text in enumerate(request["input"])]}

    def embed(self, content="sample", dimension=None):
        expression = "SELECT ai_embed(%s, %s" + (", %s)" if dimension is not None else ")")
        args = (self.model, content, dimension) if dimension is not None else (self.model, content)
        result = self.execute(expression, args)[0][0]
        return json.loads(result) if result is not None else None

    def assert_response_error(self, response):
        self.setUp()
        self.respond(response)
        with self.assertRaises(pymysql.MySQLError) as error:
            self.embed()
        self.assertNotIn(error.exception.args[0], (2003, 2006, 2013))
        self.assertEqual(((1,),), self.execute("SELECT 1"))

    def test_default_dimension_is_provider_defined(self):
        self.assertEqual(self.vector("sample", 5), self.embed())
        request = self.server.requests.get_nowait()
        self.assertNotIn("dimensions", request)
        self.assertEqual(["sample"], request["input"])

    def test_explicit_dimension(self):
        self.assertEqual(self.vector("sample", 3), self.embed(dimension=3))
        self.assertEqual(3, self.server.requests.get_nowait()["dimensions"])

    def test_large_vector_is_not_truncated(self):
        self.assertEqual(self.vector("sample", 20000), self.embed(dimension=20000))

    def test_null_content_does_not_call_model(self):
        self.assertIsNone(self.embed(content=None))
        self.assertTrue(self.server.requests.empty())

    def test_invalid_dimension_is_rejected_before_http(self):
        for dimension in (0, -1, 1.5, "3", 18446744073709551615):
            with self.subTest(dimension=dimension):
                self.setUp()
                with self.assertRaises(pymysql.MySQLError):
                    self.embed(dimension=dimension)
                self.assertTrue(self.server.requests.empty())

    def test_null_dimension_is_invalid(self):
        with self.assertRaises(pymysql.MySQLError):
            self.execute("SELECT ai_embed(%s, 'sample', NULL)", (self.model,))
        self.assertTrue(self.server.requests.empty())

    def test_nullable_integer_dimension_is_invalid(self):
        with self.assertRaises(pymysql.MySQLError):
            self.execute("SELECT ai_embed(%s, 'sample', d) FROM "
                         "(SELECT CAST(NULL AS SIGNED) AS d) x", (self.model,))
        self.assertTrue(self.server.requests.empty())

    def test_dimension_mismatch(self):
        self.respond({"data": [{"index": 0, "embedding": [1, 2]}]})
        with self.assertRaises(pymysql.MySQLError):
            self.embed(dimension=3)
        self.assertEqual(((1,),), self.execute("SELECT 1"))

    def test_longtext_input_is_preserved(self):
        table = self.model + "_lob"
        content = "世界" * 20000
        self.execute(f"CREATE TABLE `{table}` (s LONGTEXT)")
        try:
            self.execute(f"INSERT INTO `{table}` VALUES (%s)", (content,))
            value = self.execute(f"SELECT ai_embed(%s,s) FROM `{table}`", (self.model,))[0][0]
            self.assertEqual(self.vector(content, 5), json.loads(value))
            self.assertEqual([content], self.server.requests.get_nowait()["input"])
        finally:
            self.execute(f"DROP TABLE `{table}`")

    def test_multiple_rows_and_null_mapping(self):
        table = self.model + "_rows"
        self.execute(f"CREATE TABLE `{table}` (id INT PRIMARY KEY, s TEXT)")
        try:
            self.execute(f"INSERT INTO `{table}` VALUES (1,'a'),(2,NULL),(3,'世界')")
            rows = self.execute(f"SELECT id, ai_embed(%s,s) FROM `{table}` ORDER BY id", (self.model,))
            self.assertEqual([(1, self.vector('a',5)), (2,None), (3,self.vector('世界',5))],
                             [(i,json.loads(value) if value is not None else None) for i,value in rows])
            requested = []
            while not self.server.requests.empty():
                requested.extend(self.server.requests.get_nowait()["input"])
            self.assertEqual(["a", "世界"], requested)
        finally:
            self.execute(f"DROP TABLE `{table}`")

    def test_missing_index(self):
        self.assert_response_error({"data": [{"embedding": [1, 2]}]})

    def test_invalid_index(self):
        for index in (-1, 1, "0", 0.5, None, True):
            with self.subTest(index=index):
                self.assert_response_error({"data": [{"index": index, "embedding": [1, 2]}]})

    def test_wrong_result_count(self):
        self.assert_response_error({"data": [{"index": 0,"embedding": [1]},
                                            {"index": 1,"embedding": [2]}]})

    def test_duplicate_indices(self):
        self.assert_response_error({"data": [{"index": 0,"embedding": [1]},
                                            {"index": 0,"embedding": [2]}]})

    def test_empty_vector(self):
        self.assert_response_error({"data": [{"index": 0, "embedding": []}]})

    def test_non_numeric_elements(self):
        for value in ("1.2", True, None, [1], {"n": 1}):
            with self.subTest(value=value):
                self.assert_response_error({"data": [{"index": 0, "embedding": [value]}]})

    def test_non_array_embedding(self):
        for value in (None, "vector", 2, {"x": 1}):
            with self.subTest(value=value):
                self.assert_response_error({"data": [{"index": 0, "embedding": value}]})

    def test_invalid_data_shape(self):
        for response in ({}, {"data": None}, {"data": {}}, {"data": "bad"}, {"data": [None]},
                         {"data": [1]}, {"data": [{}]}):
            with self.subTest(response=response):
                self.assert_response_error(response)

    def test_non_object_response(self):
        for response in ([], [1], "bad", None, True):
            with self.subTest(response=response):
                self.assert_response_error(response)

    def test_invalid_json_and_non_finite_numbers(self):
        for response in (b'{', b'{"data":[{"index":0,"embedding":[NaN]}]}',
                         b'{"data":[{"index":0,"embedding":[1e9999]}]}'):
            with self.subTest(response=response):
                self.assert_response_error(response)

    def test_non_object_response_does_not_crash_completion(self):
        model = self.model + "_complete"
        endpoint = model + "_ep"
        self.execute("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)",
                     (model, json.dumps({"type":"completion","model_name":"local-mock"})))
        try:
            self.execute("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)", (
                endpoint, json.dumps({"ai_model_name":model,"scope":"all","provider":"openai",
                                      "url":f"http://127.0.0.1:{self.server.server_port}/completion",
                                      "access_key":"local-mock-key"})))
            try:
                self.respond([])
                with self.assertRaises(pymysql.MySQLError) as error:
                    self.execute("SELECT ai_complete(%s,'sample')", (model,))
                self.assertNotIn(error.exception.args[0], (2003,2006,2013))
                self.assertEqual(((1,),), self.execute("SELECT 1"))
            finally:
                self.execute("CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)",(endpoint,))
        finally:
            self.execute("CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)",(model,))


if __name__ == "__main__":
    unittest.main(verbosity=2)
