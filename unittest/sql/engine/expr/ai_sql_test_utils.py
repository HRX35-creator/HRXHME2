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

import json
import os
import queue
import threading
import unittest
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import pymysql


class AIServiceHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        if not 0 < length <= 4 * 1024 * 1024:
            self.send_error(400)
            return
        request = json.loads(self.rfile.read(length))
        self.server.requests.put(request)
        self.server.request_metadata.put({"path": self.path,
                                          "authorization": self.headers.get("Authorization"),
                                          "content_type": self.headers.get("Content-Type")})
        try:
            status, response = self.server.responses.get_nowait()
        except queue.Empty:
            status, response = 200, self.server.default_response(request)
        payload = response if isinstance(response, bytes) else json.dumps(response).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *_args):
        pass


class AISQLMockTestCase(unittest.TestCase):
    MODEL_TYPE = "dense_embedding"
    PROVIDER = "openai"
    ACCESS_KEY = "local-mock-key"
    ENDPOINT_PATH = "/embeddings"

    @classmethod
    def setUpClass(cls):
        options = {
            "host": os.environ.get("SEEKDB_TEST_HOST", "127.0.0.1"),
            "port": int(os.environ.get("SEEKDB_TEST_PORT", "2881")),
            "user": os.environ.get("SEEKDB_TEST_USER", "root"),
            "password": os.environ.get("SEEKDB_TEST_PASSWORD", ""),
            "database": os.environ.get("SEEKDB_TEST_DATABASE", "test"),
            "charset": "utf8mb4", "autocommit": True, "read_timeout": 60,
        }
        if os.environ.get("SEEKDB_TEST_SOCKET"):
            options["unix_socket"] = os.environ["SEEKDB_TEST_SOCKET"]
        cls.connection = pymysql.connect(**options)
        cls.addClassCleanup(cls.connection.close)
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), AIServiceHandler)
        cls.server.requests = queue.Queue()
        cls.server.request_metadata = queue.Queue()
        cls.server.responses = queue.Queue()
        cls.server.default_response = cls.default_response
        cls.addClassCleanup(cls.server.server_close)
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()
        cls.addClassCleanup(cls.server.shutdown)
        cls.model = "ai_regression_" + uuid.uuid4().hex[:12]
        cls.endpoint = cls.model + "_ep"
        cls.execute("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s, %s)", (
            cls.model, json.dumps({"type": cls.MODEL_TYPE, "model_name": "local-mock"}),
        ))
        cls.addClassCleanup(cls.execute, "CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)", (cls.model,))
        cls.execute("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s, %s)", (
            cls.endpoint, json.dumps({
                "ai_model_name": cls.model, "scope": "all", "provider": cls.PROVIDER,
                "url": f"http://127.0.0.1:{cls.server.server_port}{cls.ENDPOINT_PATH}",
                "access_key": cls.ACCESS_KEY,
            }),
        ))
        cls.addClassCleanup(cls.execute, "CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)", (cls.endpoint,))

    @classmethod
    def execute(cls, statement, args=()):
        with cls.connection.cursor() as cursor:
            cursor.execute(statement, args)
            return cursor.fetchall() if cursor.description else ()

    def setUp(self):
        for pending in (self.server.requests, self.server.responses, self.server.request_metadata):
            while not pending.empty():
                pending.get_nowait()

    def respond(self, response, status=200):
        self.server.responses.put((status, response))
