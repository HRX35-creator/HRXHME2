#!/usr/bin/env python3
# Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
"""Real background execution, typed publication and stale-result regressions."""
import json
import threading
import time
import unittest

from ai_sql_test_utils import AISQLMockTestCase


class AIColumnWorkerIntegrationTest(AISQLMockTestCase):
    MODEL_TYPE = "completion"
    ENDPOINT_PATH = "/chat/completions"

    @staticmethod
    def response(text):
        return {"choices": [{"message": {"content": text}}]}

    @staticmethod
    def default_response(request):
        text = request["messages"][-1]["content"]
        value = json.dumps({"input": text}) if text.startswith("JSON:") else (
            "7" if text.startswith("Score:") else "reply:" + text)
        return AIColumnWorkerIntegrationTest.response(value)

    def create(self, suffix, columns=None):
        table = self.model + "_" + suffix
        if columns is None:
            columns = ("id INT PRIMARY KEY,content LONGTEXT,"
                       f"result LONGTEXT AI COLUMN(AI_COMPLETE('{self.model}',content)) WITH(max_retries=2)")
        self.execute(f"CREATE TABLE `{table}`({columns})")
        self.addCleanup(self.execute, f"DROP TABLE `{table}`")
        return table

    def wait_for(self, predicate, timeout=12):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate():
                return
            time.sleep(0.05)
        self.fail("background AI column condition did not become true")

    def tasks(self, table):
        return self.execute("SELECT task.state,task.retry_count,task.request_id "
                            "FROM oceanbase.__all_ai_column_task task "
                            "JOIN oceanbase.__all_table t ON t.table_id=task.table_id "
                            "WHERE t.table_name=%s ORDER BY task.column_id", (table,))

    def wait_value(self, table, expected, row=1):
        self.wait_for(lambda: self.execute(f"SELECT result FROM `{table}` WHERE id=%s", (row,)) == ((expected,),))

    def override_response(self, callback):
        self.server.default_response = callback
        self.addCleanup(setattr, self.server, "default_response", type(self).default_response)

    def test_rollback_makes_no_request_and_commit_starts_execution(self):
        table = self.create("commit")
        self.connection.begin()
        try:
            self.execute(f"INSERT INTO `{table}`(id,content) VALUES(1,'rolled back')")
            self.assertEqual(1, len(self.tasks(table)))
            time.sleep(0.25)
            self.assertTrue(self.server.requests.empty())
        finally:
            self.connection.rollback()
        time.sleep(0.25)
        self.assertTrue(self.server.requests.empty())
        self.assertEqual((), self.tasks(table))
        self.execute(f"INSERT INTO `{table}`(id,content) VALUES(1,'committed')")
        self.wait_value(table, "reply:committed")
        self.assertEqual("committed", self.server.requests.get_nowait()["messages"][-1]["content"])
        self.assertEqual((), self.tasks(table))

    def test_retry_is_persistent_and_transport_does_not_add_attempts(self):
        table = self.create("retry")
        self.respond({"error": {"message": "retry once"}}, status=429)
        self.execute(f"INSERT INTO `{table}`(id,content) VALUES(1,'retry')")
        def retry_waiting():
            rows = self.tasks(table)
            return len(rows) == 1 and rows[0][:2] == (3, 1)
        self.wait_for(retry_waiting)
        self.assertEqual(1, self.server.requests.qsize())
        self.wait_value(table, "reply:retry")
        self.assertEqual(2, self.server.requests.qsize())
        self.assertEqual((), self.tasks(table))

    def test_typed_json_and_integer_share_one_atomic_publication(self):
        table = self.create("typed", "id INT PRIMARY KEY,content VARCHAR(50),"
                            f"tags JSON AI COLUMN(AI_COMPLETE('{self.model}',CONCAT('JSON:',content))),"
                            f"score BIGINT AI COLUMN(AI_COMPLETE('{self.model}',CONCAT('Score:',content)))")
        self.execute(f"INSERT INTO `{table}`(id,content) VALUES(1,'original')")
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            tags, score = self.execute(f"SELECT tags,score FROM `{table}` WHERE id=1")[0]
            self.assertEqual(tags is None, score is None, "request group became partially visible")
            if tags is not None:
                self.assertEqual({"input": "JSON:original"}, json.loads(tags))
                self.assertEqual(7, score)
                break
            time.sleep(0.01)
        else:
            self.fail("typed request group did not publish")
        self.assertEqual((), self.tasks(table))
        requests = [self.server.requests.get_nowait(), self.server.requests.get_nowait()]
        self.assertTrue(self.server.requests.empty())
        for request in requests:
            user = request["messages"][-1]["content"]
            system = request["messages"][0]["content"]
            self.assertIn(user, ("JSON:original", "Score:original"))
            self.assertIn("valid JSON value" if user.startswith("JSON:") else "base-10 integer", system)

    def test_terminal_type_failure_still_publishes_successful_sibling(self):
        self.override_response(lambda request: self.response(
            "7" if request["messages"][-1]["content"].startswith("Score:") else "invalid JSON"))
        table = self.create("mixed", "id INT PRIMARY KEY,content VARCHAR(50),"
                            f"tags JSON AI COLUMN(AI_COMPLETE('{self.model}',content)) WITH(max_retries=0),"
                            f"score BIGINT AI COLUMN(AI_COMPLETE('{self.model}',CONCAT('Score:',content))) WITH(max_retries=0)")
        self.execute(f"INSERT INTO `{table}`(id,content) VALUES(1,'bad')")
        self.wait_for(lambda: self.tasks(table) == ())
        self.assertEqual(((None, 7),), self.execute(f"SELECT tags,score FROM `{table}`"))
        self.assertEqual(2, self.server.requests.qsize())

    def test_slow_old_result_cannot_overwrite_new_request(self):
        table = self.create("stale")
        entered, release = threading.Event(), threading.Event()
        self.addCleanup(release.set)

        def callback(request):
            value = request["messages"][-1]["content"]
            if value == "old":
                entered.set()
                release.wait(10)
            return self.response("reply:" + value)

        self.override_response(callback)
        self.execute(f"INSERT INTO `{table}`(id,content) VALUES(1,'old')")
        self.assertTrue(entered.wait(5), "old request did not begin")
        self.execute(f"UPDATE `{table}` SET content='new' WHERE id=1")
        self.wait_value(table, "reply:new")
        release.set()
        time.sleep(1)
        self.assertEqual((("reply:new",),), self.execute(f"SELECT result FROM `{table}` WHERE id=1"))
        self.assertEqual((), self.tasks(table))

    def test_expired_lease_cannot_publish_over_the_new_owner(self):
        table = self.create("lease")
        entered, release = threading.Event(), threading.Event()
        self.addCleanup(release.set)
        counter, lock = [0], threading.Lock()

        def callback(request):
            with lock:
                counter[0] += 1
                attempt = counter[0]
            if attempt == 1:
                entered.set()
                release.wait(10)
            return self.response("old attempt" if attempt == 1 else "new owner")

        self.override_response(callback)
        self.execute(f"INSERT INTO `{table}`(id,content) VALUES(1,'unchanged input')")
        self.assertTrue(entered.wait(5))
        self.execute("UPDATE oceanbase.__all_ai_column_group SET lease_until=TIMESTAMPADD(SECOND,-1,NOW(6)) "
                     "WHERE table_id IN(SELECT table_id FROM oceanbase.__all_table WHERE table_name=%s)", (table,))
        self.wait_value(table, "new owner")
        release.set()
        time.sleep(1)
        self.assertEqual((("new owner",),), self.execute(f"SELECT result FROM `{table}`"))
        self.assertEqual(2, counter[0])
        self.assertEqual((), self.tasks(table))

    def test_long_input_and_json_config_are_materialized_before_http(self):
        table = self.create("lob", "id INT PRIMARY KEY,content LONGTEXT,config JSON,"
                            f"result LONGTEXT AI COLUMN(AI_COMPLETE('{self.model}',content,config)) WITH(max_retries=0)")
        content = '世界\\"\n\x00' * 5000
        config = {"user": "u" * 12000, "max_tokens": 17}
        self.execute(f"INSERT INTO `{table}`(id,content,config) VALUES(1,%s,%s)", (content, json.dumps(config)))
        self.wait_value(table, "reply:" + content)
        request = self.server.requests.get_nowait()
        self.assertEqual(content, request["messages"][-1]["content"])
        self.assertEqual(config["user"], request["user"])
        self.assertEqual(17, request["max_tokens"])
        self.assertTrue(self.server.requests.empty())

    def test_session_timestamp_override_does_not_delay_durable_work(self):
        table = self.create("clock")
        self.addCleanup(self.execute, "SET timestamp=0")
        self.addCleanup(self.execute, "SET time_zone='SYSTEM'")
        self.execute("SET time_zone='+12:00'")
        self.execute("SET timestamp=2000000000")
        self.execute(f"INSERT INTO `{table}`(id,content) VALUES(1,'future SQL clock')")
        self.wait_value(table, "reply:future SQL clock")
        self.assertEqual((), self.tasks(table))
        self.assertEqual(1, self.server.requests.qsize())


if __name__ == "__main__":
    unittest.main(verbosity=2)
