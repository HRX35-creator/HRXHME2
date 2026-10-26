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
"""Completion requests and response validation against an isolated SQL instance."""

import json
import unittest

import pymysql

from ai_sql_test_utils import AISQLMockTestCase


class AICompleteIntegrationTest(AISQLMockTestCase):
    MODEL_TYPE = "completion"

    @staticmethod
    def default_response(request):
        return {"choices": [{"message": {"content": "accepted"}}]}

    def complete(self, prompt="hello", config=None, explicit_config=False):
        expression = "SELECT ai_complete(%s,%s" + (",%s)" if explicit_config else ")")
        args = (self.model, prompt, config) if explicit_config else (self.model, prompt)
        return self.execute(expression, args)[0][0]

    def assert_response_error(self, response):
        self.setUp()
        self.respond(response)
        with self.assertRaises(pymysql.MySQLError) as error:
            self.complete()
        self.assertNotIn(error.exception.args[0], (2003, 2006, 2013))
        self.assertEqual(((1,),), self.execute("SELECT 1"))

    def test_system_prompt_preserves_user_content_and_provider_options(self):
        instruction = 'Return JSON only. 世界\n"quoted"'
        config = json.dumps({"system_prompt": instruction, "max_tokens": 13})
        self.assertEqual("accepted", self.complete("unchanged input", config, True))
        request = self.server.requests.get_nowait()
        self.assertEqual([{"role": "system", "content": instruction},
                          {"role": "user", "content": "unchanged input"}], request["messages"])
        self.assertEqual(13, request["max_tokens"])
        self.assertNotIn("system_prompt", request)

    def test_system_prompt_configuration_is_not_consumed_across_rows(self):
        rows = self.execute("SELECT ai_complete(%s,'unchanged',"
                            "JSON_OBJECT('system_prompt','Return JSON only.','max_tokens',9)) "
                            "FROM (SELECT 1 UNION ALL SELECT 2) AS inputs", (self.model,))
        self.assertEqual((("accepted",), ("accepted",)), rows)
        for _ in range(2):
            request = self.server.requests.get_nowait()
            self.assertEqual("Return JSON only.", request["messages"][0]["content"])
            self.assertEqual("unchanged", request["messages"][1]["content"])
            self.assertEqual(9, request["max_tokens"])
            self.assertNotIn("system_prompt", request)

    def test_invalid_system_prompt_fails_before_http(self):
        for value in (None, True, 1, [], {}):
            with self.subTest(value=value):
                with self.assertRaises(pymysql.MySQLError):
                    self.complete(config=json.dumps({"system_prompt": value}), explicit_config=True)
                self.assertTrue(self.server.requests.empty())
        self.assertEqual(((1,),), self.execute("SELECT 1"))

    def test_plain_prompt_request(self):
        self.assertEqual("accepted", self.complete())
        self.assertEqual({"model": "local-mock", "messages": [{"role": "user", "content": "hello"}]},
                         self.server.requests.get_nowait())

    def test_sql_null_config_uses_defaults(self):
        self.assertEqual("accepted", self.complete(explicit_config=True))
        self.assertEqual({"model": "local-mock", "messages": [{"role": "user", "content": "hello"}]},
                         self.server.requests.get_nowait())

    def test_string_json_config(self):
        self.assertEqual("accepted", self.complete(config='{"temperature":0.25,"max_tokens":16}',
                                                    explicit_config=True))
        request = self.server.requests.get_nowait()
        self.assertEqual(0.25, request["temperature"])
        self.assertEqual(16, request["max_tokens"])
        self.assertEqual("hello", request["messages"][-1]["content"])

    def test_native_json_config(self):
        self.execute("SELECT ai_complete(%s,'hello',json_object('temperature',0.5,'max_tokens',17))",
                     (self.model,))
        request = self.server.requests.get_nowait()
        self.assertEqual(0.5, request["temperature"])
        self.assertEqual(17, request["max_tokens"])

    def test_native_json_and_nullable_config_column(self):
        table = self.model + "_config"
        self.execute(f"CREATE TABLE `{table}` (id INT PRIMARY KEY,c JSON)")
        self.addCleanup(self.execute, f"DROP TABLE `{table}`")
        self.execute(f"INSERT INTO `{table}` VALUES (1,NULL),(2,%s)", ('{"max_tokens":19}',))
        rows = self.execute(f"SELECT id,ai_complete(%s,'hello',c) FROM `{table}` ORDER BY id", (self.model,))
        self.assertEqual(((1,"accepted"),(2,"accepted")), rows)
        self.assertNotIn("max_tokens", self.server.requests.get_nowait())
        self.assertEqual(19, self.server.requests.get_nowait()["max_tokens"])

    def test_longtext_prompt_and_config(self):
        table = self.model + "_lob"
        self.execute(f"CREATE TABLE `{table}` (p LONGTEXT,c LONGTEXT)")
        self.addCleanup(self.execute, f"DROP TABLE `{table}`")
        prompt = '世界\\"\n' * 15000
        config = {"temperature":0.4,"user":"u" * 80000}
        self.execute(f"INSERT INTO `{table}` VALUES (%s,%s)", (prompt,json.dumps(config)))
        self.execute(f"SELECT ai_complete(%s,p,c) FROM `{table}`", (self.model,))
        request = self.server.requests.get_nowait()
        self.assertEqual(prompt, request["messages"][-1]["content"])
        self.assertEqual(config["user"], request["user"])

    def test_ai_prompt_and_config(self):
        self.execute("SELECT ai_complete(%s,ai_prompt('answer:{0}',json_array(1,true)),"
                     "json_object('max_tokens',12))", (self.model,))
        request = self.server.requests.get_nowait()
        self.assertEqual([1,True], json.loads(request["messages"][-1]["content"].split(":",1)[1]))
        self.assertEqual(12,request["max_tokens"])

    def test_non_object_config_is_rejected_before_http(self):
        for config in ('[]','[1]','"text"','1','true','null','{'):
            with self.subTest(config=config):
                self.setUp()
                with self.assertRaises(pymysql.MySQLError):
                    self.complete(config=config,explicit_config=True)
                self.assertTrue(self.server.requests.empty())
                self.assertEqual(((1,),),self.execute("SELECT 1"))

    def test_native_json_non_object_config(self):
        for expression in ("json_array(1)","CAST('null' AS JSON)","CAST('1' AS JSON)"):
            with self.subTest(expression=expression):
                with self.assertRaises(pymysql.MySQLError):
                    self.execute("SELECT ai_complete(%s,'hello',"+expression+")",(self.model,))
                self.assertTrue(self.server.requests.empty())

    def test_empty_response_content_is_rejected(self):
        self.assert_response_error({"choices":[{"message":{"content":""}}]})

    def test_non_string_response_content_is_rejected(self):
        for content in (None,True,42,[],{},["text"]):
            with self.subTest(content=content):
                self.assert_response_error({"choices":[{"message":{"content":content}}]})

    def test_malformed_choices_are_rejected(self):
        for response in ({},{"choices":[]},{"choices":None},{"choices":{}},
                         {"choices":[None]},{"choices":[{}]},
                         {"choices":[{"message":"text"}]},
                         {"choices":{"message":{"content":"text"}}}):
            with self.subTest(response=response):
                self.assert_response_error(response)

    def test_invalid_json_and_non_object_response(self):
        for response in (b'{',[],None,"text",True):
            with self.subTest(response=response):
                self.assert_response_error(response)

    def test_long_unicode_response_is_preserved(self):
        content = '世界\\"\n\x00' * 20000
        self.respond({"choices":[{"message":{"content":content}}]})
        self.assertEqual(content,self.complete())

    def test_null_model_and_prompt_fail_before_http(self):
        for args in ((None,"hello"),(self.model,None)):
            with self.subTest(args=args):
                with self.assertRaises(pymysql.MySQLError):
                    self.execute("SELECT ai_complete(%s,%s)",args)
                self.assertTrue(self.server.requests.empty())

    def test_permanent_http_failure_keeps_sql_usable(self):
        self.respond({"error":{"message":"intentional test failure"}},status=400)
        with self.assertRaises(pymysql.MySQLError) as error:
            self.complete()
        self.assertNotIn(error.exception.args[0],(2003,2006,2013))
        self.assertEqual("accepted",self.complete())


if __name__ == "__main__":
    unittest.main(verbosity=2)
