#!/usr/bin/env python3
# Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
"""Predicate-selected retry and statement-wide strict reads on native SQL."""
import time
import unittest

import pymysql

from ai_sql_test_utils import AISQLMockTestCase


class AIColumnRetryStrictIntegrationTest(AISQLMockTestCase):
    MODEL_TYPE = "completion"
    ENDPOINT_PATH = "/chat/completions"

    @staticmethod
    def default_response(request):
        return {"choices": [{"message": {"content": "reply:" + request["messages"][-1]["content"]}}]}

    def create(self):
        table = self.model + "_retry_strict"
        self.execute(f"CREATE TABLE `{table}`(id INT PRIMARY KEY,category VARCHAR(20),status VARCHAR(20),"
                     f"content VARCHAR(40),result VARCHAR(100) AI COLUMN(AI_COMPLETE('{self.model}',content)),"
                     f"label VARCHAR(100) AI COLUMN(AI_COMPLETE('{self.model}',CONCAT('L:',content))))")
        self.addCleanup(self.execute, f"DROP TABLE `{table}`")
        self.addCleanup(self.execute, "SET ai_column_strict_read=OFF")
        self.addCleanup(self.connection.rollback)
        self.execute(f"INSERT INTO `{table}`(id,category,status,content) "
                     "VALUES(1,'news','active','one'),(2,'sports','inactive','two')")
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            if self.execute(f"SELECT COUNT(*) FROM `{table}` WHERE result IS NOT NULL AND label IS NOT NULL") == ((2,),):
                return table
            time.sleep(0.05)
        self.fail("initial AI values did not become ready")

    def assert_unfinished(self, statement):
        with self.assertRaises(pymysql.MySQLError) as error:
            self.execute(statement)
        self.assertEqual(11120, error.exception.args[0])
        self.assertRegex(error.exception.args[1], r"AI column has unfinished tasks for table_id=\d+, column_id=\d+")

    def test_retry_status_business_predicate_and_rollback(self):
        table = self.create()
        self.connection.begin()
        self.execute(f"ALTER TABLE `{table}` RETRY AI COLUMN result WHERE status='READY' AND category='news'")
        self.assertEqual(((1, 1, 1), (2, 0, 1)), self.execute(
            f"SELECT id,result IS NULL,label IS NOT NULL FROM `{table}` ORDER BY id"))
        self.execute(f"ALTER TABLE `{table}` RETRY AI COLUMN result WHERE status='READY' AND id=1")
        self.assertEqual(((1,),), self.execute(
            "SELECT COUNT(*) FROM oceanbase.__all_ai_column_task task "
            "JOIN oceanbase.__all_table t ON t.table_id=task.table_id WHERE t.table_name=%s", (table,)))
        self.connection.rollback()
        self.assertEqual(((2,),), self.execute(f"SELECT COUNT(*) FROM `{table}` WHERE result IS NOT NULL"))
        self.connection.begin()
        self.execute(f"ALTER TABLE `{table}` RETRY AI COLUMN result WHERE `status`='inactive'")
        self.assertEqual(((1, 0), (2, 1)), self.execute(f"SELECT id,result IS NULL FROM `{table}` ORDER BY id"))
        self.connection.rollback()
        for action in ("RETRY AI COLUMN content", "RETRY AI COLUMN result WHERE id=1, ADD COLUMN extra INT"):
            with self.assertRaises(pymysql.MySQLError) as error:
                self.execute(f"ALTER TABLE `{table}` {action}")
            self.assertEqual(1235, error.exception.args[0])

    def test_strict_reads_track_explicit_columns_and_precede_writes(self):
        table = self.create()
        self.assertEqual(((0,),), self.execute("SELECT @@ai_column_strict_read"))
        self.connection.begin()
        self.execute(f"ALTER TABLE `{table}` RETRY AI COLUMN result WHERE id=1")
        self.execute("SET ai_column_strict_read=ON")
        self.assertEqual(((2,),), self.execute(f"SELECT COUNT(*) FROM `{table}`"))
        self.assertEqual(((2,),), self.execute(f"SELECT COUNT(*) FROM `{table}` WHERE label IS NOT NULL"))
        self.execute(f"UPDATE `{table}` SET category='changed' WHERE id=2")
        for statement in (
                f"SELECT result FROM `{table}` WHERE id=2",
                f"SELECT * FROM `{table}` WHERE id=2",
                f"SELECT id FROM `{table}` WHERE result IS NULL",
                f"SELECT id FROM `{table}` WHERE id IN(SELECT id FROM `{table}` WHERE result IS NULL)",
                f"UPDATE `{table}` SET category='rejected' WHERE result IS NULL",
                f"DELETE FROM `{table}` WHERE result IS NULL"):
            self.assert_unfinished(statement)
        self.assertEqual(((1, 'news'), (2, 'changed')), self.execute(f"SELECT id,category FROM `{table}` ORDER BY id"))
        self.execute(f"DELETE FROM `{table}` WHERE id=2")
        self.assertEqual(((1,),), self.execute(f"SELECT COUNT(*) FROM `{table}`"))
        self.connection.rollback()

    def test_cached_and_prepared_reads_recheck_after_retry(self):
        table = self.create()
        self.execute("SET ai_column_strict_read=ON")
        query = f"SELECT result FROM `{table}` WHERE id=2"
        self.assertEqual((("reply:two",),), self.execute(query))
        self.execute(f"PREPARE ai_column_read FROM '{query}'")
        self.addCleanup(self.execute, "DEALLOCATE PREPARE ai_column_read")
        self.connection.begin()
        self.execute(f"ALTER TABLE `{table}` RETRY AI COLUMN result WHERE id=1")
        self.assert_unfinished(query)
        self.assert_unfinished("EXECUTE ai_column_read")
        self.execute("SET ai_column_strict_read=OFF")
        self.assertEqual((("reply:two",),), self.execute(query))
        self.assertEqual((("reply:two",),), self.execute("EXECUTE ai_column_read"))
        self.connection.rollback()
        self.execute("SET ai_column_strict_read=ON")
        self.assertEqual((("reply:two",),), self.execute(query))


if __name__ == "__main__":
    unittest.main(verbosity=2)
