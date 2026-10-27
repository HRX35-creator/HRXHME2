#!/usr/bin/env python3
# Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
"""Trigger input changes invalidate AI values before the business row is stored."""
import time
import unittest

from ai_sql_test_utils import AISQLMockTestCase


class AIColumnTriggerIntegrationTest(AISQLMockTestCase):
    MODEL_TYPE = "completion"
    ENDPOINT_PATH = "/chat/completions"

    @staticmethod
    def default_response(request):
        return {"choices": [{"message": {"content": "reply:" + request["messages"][-1]["content"]}}]}

    def create(self, suffix):
        table = self.model + "_" + suffix
        self.execute(f"CREATE TABLE `{table}`(id INT PRIMARY KEY,title VARCHAR(40),content VARCHAR(100),"
                     f"result VARCHAR(200) AI COLUMN(AI_COMPLETE('{self.model}',content)))")
        self.addCleanup(self.execute, f"DROP TABLE `{table}`")
        self.addCleanup(self.connection.rollback)
        return table

    def wait_result(self, table, expected):
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            if self.execute(f"SELECT result FROM `{table}` WHERE id=1") == ((expected,),):
                return
            time.sleep(0.05)
        self.fail("trigger-dependent AI result did not publish")

    def initial(self, table):
        self.execute(f"INSERT INTO `{table}`(id,title,content) VALUES(1,'initial','initial')")
        self.wait_result(table, "reply:initial")

    def test_before_update_dependency_change_invalidates_then_rollback_restores(self):
        table = self.create("before")
        self.initial(table)
        self.execute(f"CREATE TRIGGER `{table}_tr` BEFORE UPDATE ON `{table}` FOR EACH ROW "
                     "SET NEW.content=CONCAT('trigger:',NEW.title)")
        self.connection.begin()
        self.execute(f"UPDATE `{table}` SET title='changed' WHERE id=1")
        self.assertEqual((("trigger:changed", None),), self.execute(f"SELECT content,result FROM `{table}`"))
        self.connection.rollback()
        self.assertEqual((("initial", "reply:initial"),), self.execute(f"SELECT content,result FROM `{table}`"))
        self.execute(f"UPDATE `{table}` SET title='changed' WHERE id=1")
        self.wait_result(table, "reply:trigger:changed")

    def test_before_update_reverting_input_keeps_result_and_creates_no_work(self):
        table = self.create("revert")
        self.initial(table)
        calls = self.server.requests.qsize()
        self.execute(f"CREATE TRIGGER `{table}_tr` BEFORE UPDATE ON `{table}` FOR EACH ROW SET NEW.content=OLD.content")
        self.connection.begin()
        self.execute(f"UPDATE `{table}` SET content='discarded' WHERE id=1")
        self.assertEqual((("initial", "reply:initial"),), self.execute(f"SELECT content,result FROM `{table}`"))
        self.assertEqual(((0,),), self.execute(
            "SELECT COUNT(*) FROM oceanbase.__all_ai_column_task task JOIN oceanbase.__all_table t "
            "ON t.table_id=task.table_id WHERE t.table_name=%s", (table,)))
        self.connection.commit()
        self.assertEqual(calls, self.server.requests.qsize())

    def test_insert_trigger_and_internal_maintenance_do_not_repeat_business_triggers(self):
        table = self.create("audit")
        audit = table + "_events"
        self.execute(f"CREATE TABLE `{audit}`(event INT)")
        self.addCleanup(self.execute, f"DROP TABLE `{audit}`")
        for timing, operation, name in (("BEFORE", "INSERT", "bi"), ("BEFORE", "UPDATE", "bu")):
            self.execute(f"CREATE TRIGGER `{table}_{name}` {timing} {operation} ON `{table}` FOR EACH ROW "
                         "SET NEW.content=CONCAT(NEW.content,'!')")
        self.execute(f"CREATE TRIGGER `{table}_au` AFTER UPDATE ON `{table}` FOR EACH ROW INSERT INTO `{audit}` VALUES(1)")
        self.execute(f"INSERT INTO `{table}`(id,title,content) VALUES(1,'initial','initial')")
        self.wait_result(table, "reply:initial!")
        self.assertEqual((("initial!",),), self.execute(f"SELECT content FROM `{table}`"))
        self.assertEqual(((0,),), self.execute(f"SELECT COUNT(*) FROM `{audit}`"))
        self.execute(f"UPDATE `{table}` SET content='next' WHERE id=1")
        self.wait_result(table, "reply:next!")
        self.assertEqual((("next!",),), self.execute(f"SELECT content FROM `{table}`"))
        self.assertEqual(((1,),), self.execute(f"SELECT COUNT(*) FROM `{audit}`"))
        self.connection.begin()
        self.execute(f"ALTER TABLE `{table}` RETRY AI COLUMN result WHERE id=1")
        self.assertEqual((("next!", None),), self.execute(f"SELECT content,result FROM `{table}`"))
        self.assertEqual(((1,),), self.execute(f"SELECT COUNT(*) FROM `{audit}`"))
        self.connection.rollback()
        self.assertEqual((("reply:next!",),), self.execute(f"SELECT result FROM `{table}`"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
