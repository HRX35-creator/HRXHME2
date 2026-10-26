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
"""Native Ollama endpoint lifecycle and SQL protocol regressions."""

import json
import unittest
import uuid

import pymysql

from ai_sql_test_utils import AISQLMockTestCase


class OllamaEndpointIntegrationTest(AISQLMockTestCase):
    @staticmethod
    def default_response(_request):
        return {"embeddings": [[1, 2]]}

    def create_endpoint(self, provider="ollama", **extra):
        endpoint = self.endpoint + "_" + uuid.uuid4().hex[:6]
        model = endpoint + "_model"
        self.execute("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)",
                     (model, '{"type":"dense_embedding","model_name":"native-model"}'))
        self.addCleanup(self.execute, "CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)", (model,))
        config = {"ai_model_name": model, "scope": "all", "provider": provider,
                  "url": f"http://127.0.0.1:{self.server.server_port}/api/embed", **extra}
        self.execute("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)",
                     (endpoint, json.dumps(config)))
        self.addCleanup(self.execute, "CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)", (endpoint,))
        return endpoint

    def test_create_alter_case_and_drop(self):
        endpoint = self.create_endpoint(access_key="")
        for provider in ("Ollama", "OLLAMA", "oLlAmA"):
            self.execute("CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)",
                         (endpoint, json.dumps({"provider": provider})))
            value = self.execute("SELECT provider FROM oceanbase.DBA_OB_AI_MODEL_ENDPOINTS "
                                 "WHERE endpoint_name=%s", (endpoint,))[0][0]
            self.assertEqual("ollama", value.lower())

    def test_omitted_key_is_allowed(self):
        self.create_endpoint()

    def test_alter_provider_and_clear_existing_key(self):
        self.execute("CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)",
                     (self.endpoint, '{"provider":"Ollama","access_key":""}'))
        value = self.execute("SELECT ai_embed(%s,'hello')", (self.model,))[0][0]
        self.assertEqual([1, 2], json.loads(value))
        self.assertIsNone(self.server.request_metadata.get_nowait()["authorization"])

    def test_other_providers_still_require_keys(self):
        for provider in ("openai", "siliconflow"):
            with self.subTest(provider=provider):
                with self.assertRaises(pymysql.MySQLError) as error:
                    self.create_endpoint(provider=provider, access_key="")
                self.assertIn("access_key", str(error.exception))

    def test_altering_to_key_required_provider_preserves_definition_on_failure(self):
        endpoint = self.create_endpoint()
        with self.assertRaises(pymysql.MySQLError):
            self.execute("CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)",
                         (endpoint, '{"provider":"openai"}'))
        value = self.execute("SELECT provider FROM oceanbase.DBA_OB_AI_MODEL_ENDPOINTS "
                             "WHERE endpoint_name=%s", (endpoint,))[0][0]
        self.assertEqual("ollama", value.lower())


class OllamaCompleteIntegrationTest(AISQLMockTestCase):
    MODEL_TYPE = "completion"
    PROVIDER = "oLlAmA"
    ACCESS_KEY = ""
    ENDPOINT_PATH = "/api/generate"

    @staticmethod
    def default_response(_request):
        return {"response": "native answer", "done": True}

    def test_native_generate_without_key(self):
        self.assertEqual((("native answer",),), self.execute("SELECT ai_complete(%s,'hello')", (self.model,)))
        request = self.server.requests.get_nowait()
        self.assertEqual("local-mock", request["model"])
        self.assertEqual("hello", request["prompt"])
        self.assertIs(request["stream"], False)
        self.assertNotIn("messages", request)
        metadata = self.server.request_metadata.get_nowait()
        self.assertEqual("/api/generate", metadata["path"])
        self.assertIsNone(metadata["authorization"])
        self.assertEqual("application/json", metadata["content_type"])

    def test_config_cannot_enable_streaming(self):
        self.execute("SELECT ai_complete(%s,'hello',json_object('stream',true,"
                     "'options',json_object('temperature',0.3,'num_predict',12)))", (self.model,))
        request = self.server.requests.get_nowait()
        self.assertIs(request["stream"], False)
        self.assertEqual({"temperature": 0.3, "num_predict": 12}, request["options"])

    def test_structured_prompt_and_null_config(self):
        self.execute("SELECT ai_complete(%s,ai_prompt('hello {0}','世界'),NULL)", (self.model,))
        self.assertEqual("hello 世界", self.server.requests.get_nowait()["prompt"])

    def test_invalid_native_response(self):
        for response in ({}, {"response": ""}, {"response": None}, {"response": []},
                         {"response": 1}, {"response": {}}, []):
            with self.subTest(response=response):
                self.respond(response)
                with self.assertRaises(pymysql.MySQLError) as error:
                    self.execute("SELECT ai_complete(%s,'hello')", (self.model,))
                self.assertNotIn(error.exception.args[0], (2003, 2006, 2013))
                self.assertEqual(((1,),), self.execute("SELECT 1"))


class OllamaEmbedIntegrationTest(AISQLMockTestCase):
    PROVIDER = "OLLAMA"
    ACCESS_KEY = ""
    ENDPOINT_PATH = "/api/embed"

    @staticmethod
    def vector(text, dimension):
        return [sum(text.encode()) + i / 8 for i in range(dimension)]

    @classmethod
    def default_response(cls, request):
        return {"embeddings": [cls.vector(s, request.get("dimensions", 7)) for s in request["input"]]}

    def test_native_embedding_without_key(self):
        value = self.execute("SELECT ai_embed(%s,'hello')", (self.model,))[0][0]
        self.assertEqual(self.vector("hello", 7), json.loads(value))
        self.assertEqual({"model": "local-mock", "input": ["hello"]}, self.server.requests.get_nowait())
        metadata = self.server.request_metadata.get_nowait()
        self.assertEqual("/api/embed", metadata["path"])
        self.assertIsNone(metadata["authorization"])

    def test_explicit_embedding_dimension(self):
        value = self.execute("SELECT ai_embed(%s,'hello',3)", (self.model,))[0][0]
        self.assertEqual(self.vector("hello", 3), json.loads(value))
        self.assertEqual(3, self.server.requests.get_nowait()["dimensions"])

    def test_table_batch_and_null_mapping(self):
        table = self.model + "_rows"
        self.execute(f"CREATE TABLE `{table}` (id INT PRIMARY KEY,s TEXT)")
        self.addCleanup(self.execute, f"DROP TABLE `{table}`")
        self.execute(f"INSERT INTO `{table}` VALUES (1,'first'),(2,NULL),(3,'世界')")
        rows = self.execute(f"SELECT id,ai_embed(%s,s) FROM `{table}` ORDER BY id", (self.model,))
        self.assertEqual([(1,self.vector("first",7)),(2,None),(3,self.vector("世界",7))],
                         [(i,json.loads(s) if s is not None else None) for i,s in rows])
        self.assertEqual(1, self.server.requests.qsize())
        self.assertEqual(["first","世界"], self.server.requests.get_nowait()["input"])

    def test_invalid_embedding_responses(self):
        for response in ({}, {"embeddings": []}, {"embeddings": [None]},
                         {"embeddings": [[True]]}, {"embeddings": [[1],[2]]}, {"embeddings": {}}):
            with self.subTest(response=response):
                self.respond(response)
                with self.assertRaises(pymysql.MySQLError) as error:
                    self.execute("SELECT ai_embed(%s,'hello')", (self.model,))
                self.assertNotIn(error.exception.args[0], (2003,2006,2013))
                self.assertEqual(((1,),), self.execute("SELECT 1"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
