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
"""Rerank batching, stable global ordering, and malformed-response regressions."""

import json
import unittest

import pymysql

from ai_sql_test_utils import AISQLMockTestCase


class AIRerankIntegrationTest(AISQLMockTestCase):
    MODEL_TYPE = "rerank"
    PROVIDER = "siliconflow"

    @staticmethod
    def score(text):
        return (sum(text.encode()) % 13) / 4

    @classmethod
    def response(cls, results):
        return {"output": {"results": results}} if cls.PROVIDER == "aliyun-dashscope" else {"results": results}

    @classmethod
    def default_response(cls, request):
        payload = request.get("input", request)
        return cls.response([{"index": i, "relevance_score": cls.score(text)}
                             for i, text in reversed(list(enumerate(payload["documents"]) ))])

    def rerank(self, documents, key=None, explicit_key=False):
        statement = "SELECT ai_rerank(%s,'query',%s" + (",%s)" if explicit_key else ")")
        args = (self.model, json.dumps(documents), key) if explicit_key else (self.model, json.dumps(documents))
        return json.loads(self.execute(statement, args)[0][0])

    def requests(self):
        requests = []
        while not self.server.requests.empty():
            requests.append(self.server.requests.get_nowait())
        return [request.get("input", request) for request in requests]

    def expected_indices(self, documents):
        return sorted(range(len(documents)), key=lambda i: (-self.score(documents[i]), i))

    def assert_indices(self, result, documents):
        self.assertEqual(self.expected_indices(documents), [row["index"] for row in result])
        self.assertEqual([self.score(documents[i]) for i in self.expected_indices(documents)],
                         [row["relevance_score"] for row in result])

    def assert_response_error(self, response):
        self.setUp()
        self.respond(response)
        with self.assertRaises(pymysql.MySQLError) as error:
            self.rerank(["one", "two"])
        self.assertNotIn(error.exception.args[0], (2003, 2006, 2013))
        self.assertEqual(((1,),), self.execute("SELECT 1"))

    def test_strings_omitted_key_and_unsorted_response(self):
        documents = ["first", "世界", "third", "fourth"]
        self.assert_indices(self.rerank(documents), documents)
        request = self.requests()[0]
        self.assertEqual(documents, request["documents"])
        self.assertEqual("query", request["query"])

    def test_null_key_uses_string_input(self):
        documents = ["one", "two"]
        self.assert_indices(self.rerank(documents, explicit_key=True), documents)

    def test_string_batches_have_global_indices(self):
        documents = [f"document-{i}" for i in range(47)]
        self.assert_indices(self.rerank(documents), documents)
        requests = self.requests()
        self.assertEqual([20, 20, 7], [len(request["documents"]) for request in requests])
        self.assertEqual(documents, [s for request in requests for s in request["documents"]])

    def test_ties_follow_original_input_order(self):
        documents = ["same"] * 45
        self.assertEqual(list(range(45)), [row["index"] for row in self.rerank(documents)])

    def test_object_batches_preserve_ids_and_original_fields(self):
        texts = [f"text-{i}" for i in range(43)]
        documents = [{"id": "id-" + str(i), "body": text, "extra": {"n": i}} for i, text in enumerate(texts)]
        result = self.rerank(documents, key="body", explicit_key=True)
        expected = [{**documents[i], "_model_score": self.score(texts[i])} for i in self.expected_indices(texts)]
        self.assertEqual(expected, result)
        self.assertEqual([20,20,3], [len(request["documents"]) for request in self.requests()])

    def test_native_json_documents(self):
        rows = self.execute("SELECT ai_rerank(%s,'query',json_array('one','two'))", (self.model,))
        self.assert_indices(json.loads(rows[0][0]), ["one", "two"])

    def test_longtext_query_is_preserved(self):
        table = self.model + "_query"
        self.execute(f"CREATE TABLE `{table}` (q LONGTEXT)")
        self.addCleanup(self.execute, f"DROP TABLE `{table}`")
        query = "世界" * 14000
        self.execute(f"INSERT INTO `{table}` VALUES (%s)", (query,))
        self.execute(f"SELECT ai_rerank(%s,q,'[\"one\",\"two\"]') FROM `{table}`", (self.model,))
        self.assertEqual(query, self.requests()[0]["query"])

    def test_integer_scores_are_accepted(self):
        self.respond(self.response([{"index":1,"relevance_score":2},{"index":0,"relevance_score":1}]))
        result = self.rerank(["one","two"])
        self.assertEqual([1,0], [row["index"] for row in result])

    def test_invalid_input_is_rejected_before_http(self):
        for documents, key, explicit in (([],None,False),([None],None,False),([1],None,False),
                                         ([{}],None,False),(["text"],"body",True),
                                         ([{"body":1}],"body",True),([{}],"body",True),
                                         ([{"body":"text"}],"",True)):
            with self.subTest(documents=documents,key=key):
                with self.assertRaises(pymysql.MySQLError):
                    self.rerank(documents,key,explicit)
                self.assertTrue(self.server.requests.empty())

    def test_response_count_must_match(self):
        for results in ([],[{"index":0,"relevance_score":0}],
                        [{"index":i,"relevance_score":0} for i in range(3)]):
            with self.subTest(results=results):
                self.assert_response_error(self.response(results))

    def test_duplicate_and_invalid_indices(self):
        for index in (-1,2,0,"1",True,None,0.5):
            with self.subTest(index=index):
                self.assert_response_error(self.response([{"index":0,"relevance_score":1},
                                                          {"index":index,"relevance_score":2}]))

    def test_non_numeric_and_non_finite_scores(self):
        for score in (None,True,"1",[],{}):
            with self.subTest(score=score):
                self.assert_response_error(self.response([{"index":0,"relevance_score":score},
                                                          {"index":1,"relevance_score":1}]))
        response = json.dumps(self.response([{"index":0,"relevance_score":0},
                                             {"index":1,"relevance_score":1}])).replace('"relevance_score": 0','"relevance_score": 1e9999')
        self.assert_response_error(response.encode())

    def test_wrong_response_shapes(self):
        for response in ([],None,{},self.response(None),self.response({}),self.response([None,None]),
                         self.response([{"index":0},{"index":1}])):
            with self.subTest(response=response):
                self.assert_response_error(response)

    def test_later_batch_failure_keeps_server_usable(self):
        self.respond(self.response([{"index":i,"relevance_score":0} for i in range(20)]))
        self.respond({"error":{"message":"intentional later batch failure"}},status=400)
        with self.assertRaises(pymysql.MySQLError) as error:
            self.rerank([f"doc-{i}" for i in range(45)])
        self.assertNotIn(error.exception.args[0],(2003,2006,2013))
        self.assertEqual(2,len(self.requests()))
        self.assert_indices(self.rerank(["one","two"]),["one","two"])


class DashscopeRerankIntegrationTest(AIRerankIntegrationTest):
    PROVIDER = "aliyun-dashscope"


if __name__ == "__main__":
    unittest.main(verbosity=2)
