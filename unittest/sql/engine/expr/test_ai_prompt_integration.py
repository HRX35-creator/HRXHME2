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
"""SQL regressions against a running seekdb and a local, deterministic HTTP mock.

Requires PyMySQL. Set SEEKDB_TEST_SOCKET, or SEEKDB_TEST_HOST/SEEKDB_TEST_PORT,
and optionally SEEKDB_TEST_USER/SEEKDB_TEST_PASSWORD/SEEKDB_TEST_DATABASE.
No external model or real API key is used.
"""

import json
import os
import queue
import threading
import unittest
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import pymysql


class PromptEchoHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        if not 0 < length <= 4 * 1024 * 1024:
            self.send_error(400)
            return
        request = json.loads(self.rfile.read(length))
        self.server.requests.put(request)
        response = json.dumps({"choices": [{"message": {"content": "accepted"}}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(response)))
        self.end_headers()
        self.wfile.write(response)

    def log_message(self, *_args):
        pass


class AIPromptIntegrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        options = {
            "host": os.environ.get("SEEKDB_TEST_HOST", "127.0.0.1"),
            "port": int(os.environ.get("SEEKDB_TEST_PORT", "2881")),
            "user": os.environ.get("SEEKDB_TEST_USER", "root"),
            "password": os.environ.get("SEEKDB_TEST_PASSWORD", ""),
            "database": os.environ.get("SEEKDB_TEST_DATABASE", "test"),
            "charset": "utf8mb4",
            "autocommit": True,
            "read_timeout": 60,
        }
        if os.environ.get("SEEKDB_TEST_SOCKET"):
            options["unix_socket"] = os.environ["SEEKDB_TEST_SOCKET"]
        cls.connection = pymysql.connect(**options)
        cls.addClassCleanup(cls.connection.close)
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), PromptEchoHandler)
        cls.server.requests = queue.Queue()
        cls.addClassCleanup(cls.server.server_close)
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()
        cls.addClassCleanup(cls.server.shutdown)
        suffix = uuid.uuid4().hex[:12]
        cls.model = "prompt_regression_" + suffix
        cls.endpoint = cls.model + "_ep"
        cls.table = cls.model + "_lob"
        cls.execute("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s, %s)", (
            cls.model, json.dumps({"type": "completion", "model_name": "local-mock"}),
        ))
        cls.addClassCleanup(cls.execute, "CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)", (cls.model,))
        cls.execute("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s, %s)", (
            cls.endpoint, json.dumps({
                "ai_model_name": cls.model,
                "scope": "all",
                "url": f"http://127.0.0.1:{cls.server.server_port}/v1/chat/completions",
                "access_key": "local-mock-key",
                "provider": "openai",
            }),
        ))
        cls.addClassCleanup(cls.execute, "CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)", (cls.endpoint,))

    @classmethod
    def execute(cls, statement, args=()):
        with cls.connection.cursor() as cursor:
            cursor.execute(statement, args)
            return cursor.fetchall() if cursor.description else ()

    def rendered(self, expression, args=()):
        while not self.server.requests.empty():
            self.server.requests.get_nowait()
        rows = self.execute("SELECT ai_complete(%s, " + expression + ")", (self.model, *args))
        self.assertTrue(rows[0][0])
        request = self.server.requests.get(timeout=5)
        return request["messages"][-1]["content"]

    def test_json_arguments_retain_structure(self):
        value = self.execute("SELECT ai_prompt(%s, json_array(1,true), json_object('k',2))",
                             ("{0}/{0}/{1}",))[0][0]
        self.assertEqual({"template": "{0}/{0}/{1}", "args": [[1, True], {"k": 2}]},
                         json.loads(value))

    def test_json_arguments_render_as_json(self):
        value = self.rendered("ai_prompt(%s, json_array(1,true), json_object('k',2))",
                              ("{0}|{1}",))
        array, obj = value.split("|")
        self.assertEqual([1, True], json.loads(array))
        self.assertEqual({"k": 2}, json.loads(obj))

    def test_json_column_and_json_null(self):
        table = self.model + "_json"
        self.execute(f"CREATE TABLE `{table}` (id INT PRIMARY KEY, j JSON)")
        try:
            self.execute(f"INSERT INTO `{table}` VALUES (1, %s), (2, %s), (3, NULL)",
                         ('{"k":[1,true]}', 'null'))
            values = self.execute(f"SELECT id, ai_prompt('value:{{0}}', j) FROM `{table}` "
                                  "WHERE id < 3 ORDER BY id")
            self.assertEqual([{"k": [1, True]}], json.loads(values[0][1])["args"])
            self.assertEqual([None], json.loads(values[1][1])["args"])
            with self.assertRaises(pymysql.MySQLError) as error:
                self.execute(f"SELECT ai_prompt('value:{{0}}', j) FROM `{table}` WHERE id = 3")
            self.assertEqual(5083, error.exception.args[0])
        finally:
            self.execute(f"DROP TABLE `{table}`")

    def test_repeated_placeholder_and_no_rescanning(self):
        argument = "x" * 4096 + "{1}"
        value = self.rendered("ai_prompt(%s, %s, %s)", ("{0}|{0}|{0}", argument, "unused"))
        self.assertEqual("|".join([argument] * 3), value)

    def test_nested_prompt(self):
        value = self.rendered("ai_prompt(%s, ai_prompt(%s, ai_prompt(%s, %s)))",
                              ("outer[{0}]", "middle[{0}]", "inner[{0}]", "value"))
        self.assertEqual("outer[middle[inner[value]]]", value)

    def test_unicode_and_escaped_bytes(self):
        argument = '世界\\quoted"\n\x00end'
        value = self.rendered("ai_prompt(%s, %s)", ("你好:{0}/{0}", argument))
        self.assertEqual("你好:" + argument + "/" + argument, value)

    def test_literal_braces_and_leading_zero_index(self):
        value = self.rendered("ai_prompt(%s, %s)", ("{name}:{}:{0:{000}:}", "ok"))
        self.assertEqual("{name}:{}:{0:ok:}", value)

    def test_invalid_numeric_index_fails_before_http(self):
        for template in ("{1}", "{18446744073709551616}", "{99999999999999999999999999999999}"):
            with self.subTest(template=template):
                with self.assertRaises(pymysql.MySQLError) as error:
                    self.rendered("ai_prompt(%s, %s)", (template, "value"))
                self.assertEqual(1210, error.exception.args[0])
                self.assertTrue(self.server.requests.empty())

    def test_longtext_template_and_text_argument(self):
        template = "t" * 70000 + "{0}"
        argument = "世" * 20000
        self.execute(f"CREATE TABLE `{self.table}` (t LONGTEXT, a TEXT)")
        try:
            self.execute(f"INSERT INTO `{self.table}` VALUES (%s, %s)", (template, argument))
            value = self.execute(f"SELECT ai_prompt(t,a) FROM `{self.table}`")[0][0]
            self.assertEqual({"template": template, "args": [argument]}, json.loads(value))
            rendered = self.rendered(f"(SELECT ai_prompt(t,a) FROM `{self.table}`)")
            self.assertEqual("t" * 70000 + argument, rendered)
        finally:
            self.execute(f"DROP TABLE `{self.table}`")

    def test_sql_null_stays_invalid(self):
        for expression in ("ai_prompt(NULL)", "ai_prompt('text', NULL)"):
            with self.subTest(expression=expression):
                with self.assertRaises(pymysql.MySQLError) as error:
                    self.execute("SELECT " + expression)
                self.assertEqual(5083, error.exception.args[0])


if __name__ == "__main__":
    unittest.main(verbosity=2)
