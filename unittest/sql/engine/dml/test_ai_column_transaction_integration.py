#!/usr/bin/env python3
# Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
"""Transactional AI invalidation and durable queue checks; HTTP must not run in T1."""
import unittest

import pymysql

from ai_sql_test_utils import AISQLMockTestCase


class AIColumnTransactionIntegrationTest(AISQLMockTestCase):
    MODEL_TYPE = "completion"

    @staticmethod
    def default_response(request):
        return {"choices": [{"message": {"content": "unused"}}]}

    def create(self, suffix, two=False):
        table = self.model + "_" + suffix
        extra = (f",other VARCHAR(100) AI COLUMN(AI_COMPLETE('{self.model}',title))" if two else "")
        self.execute(f"CREATE TABLE `{table}`(id INT PRIMARY KEY,title VARCHAR(50),"
                     f"content VARCHAR(50) COLLATE utf8mb4_general_ci,"
                     f"result VARCHAR(100) AI COLUMN(AI_COMPLETE('{self.model}',content)){extra})")
        self.addCleanup(self.execute, f"DROP TABLE `{table}`")
        self.connection.begin()
        self.addCleanup(self.connection.rollback)
        return table

    def tasks(self, table):
        return self.execute("SELECT task.column_id,task.request_id,task.rowkey_predicate "
                            "FROM oceanbase.__all_ai_column_task task "
                            "JOIN oceanbase.__all_table t ON t.table_id=task.table_id "
                            "WHERE t.table_name=%s ORDER BY task.rowkey_predicate,task.column_id", (table,))

    def test_multirow_groups_and_rollback(self):
        table = self.create("batch", two=True)
        self.execute(f"INSERT INTO `{table}`(id,title,content) VALUES(1,'t1','c1'),(2,'t2','c2')")
        tasks = self.tasks(table)
        self.assertEqual(4, len(tasks))
        self.assertEqual(2, len({row[1] for row in tasks}))
        self.assertEqual(tasks[0][1], tasks[1][1])
        self.assertEqual(tasks[2][1], tasks[3][1])
        self.assertEqual(((1, 1), (2, 1)), self.execute(
            f"SELECT id,result IS NULL AND other IS NULL FROM `{table}` ORDER BY id"))
        self.assertTrue(self.server.requests.empty())
        self.connection.rollback()
        self.assertEqual((), self.tasks(table))
        self.assertEqual(((0,),), self.execute(f"SELECT COUNT(*) FROM `{table}`"))

    def test_dependency_changes_and_primary_key_relocate_pending_work(self):
        table = self.create("update")
        self.execute(f"INSERT INTO `{table}`(id,title,content) VALUES(1,'title','Alpha')")
        initial = self.tasks(table)
        self.assertEqual(1, len(initial))
        self.execute(f"UPDATE `{table}` SET title='different' WHERE id=1")
        self.assertEqual(initial, self.tasks(table))
        self.execute(f"UPDATE `{table}` SET content='Alpha' WHERE id=1")
        self.assertEqual(initial, self.tasks(table))
        for content in ("alpha", "alpha "):
            self.execute(f"UPDATE `{table}` SET content=%s WHERE id=1", (content,))
            changed = self.tasks(table)
            self.assertEqual(1, len(changed))
            self.assertNotEqual(initial[0][1], changed[0][1])
            initial = changed
        self.execute(f"UPDATE `{table}` SET id=3 WHERE id=1")
        changed = self.tasks(table)
        self.assertEqual(1, len(changed))
        self.assertEqual("`id` = 3", changed[0][2])
        self.assertNotEqual(initial[0][1], changed[0][1])
        self.execute(f"DELETE FROM `{table}` WHERE id=3")
        self.assertEqual((), self.tasks(table))
        self.assertTrue(self.server.requests.empty())

    def test_ignored_insert_preserves_existing_request_and_readonly_result(self):
        table = self.create("ignore")
        self.execute(f"INSERT INTO `{table}`(id,content) VALUES(1,'original')")
        initial = self.tasks(table)
        self.execute(f"INSERT IGNORE INTO `{table}`(id,content) VALUES(1,'ignored')")
        self.assertEqual(initial, self.tasks(table))
        self.assertEqual((("original",),), self.execute(f"SELECT content FROM `{table}`"))
        with self.assertRaises(pymysql.MySQLError):
            self.execute(f"UPDATE `{table}` SET result='forged' WHERE id=1")
        self.assertTrue(self.server.requests.empty())

    def test_partial_supersession_keeps_the_other_columns_original_task(self):
        table = self.create("partial", two=True)
        self.execute(f"INSERT INTO `{table}`(id,title,content) VALUES(1,'title','content')")
        initial = self.tasks(table)
        self.assertEqual(2, len(initial))
        original_request = initial[0][1]
        self.assertEqual(original_request, initial[1][1])
        self.execute(f"UPDATE `{table}` SET content='new content' WHERE id=1")
        partial = self.tasks(table)
        self.assertEqual(2, len(partial))
        self.assertEqual(2, len({row[1] for row in partial}))
        self.assertEqual(1, sum(row[1] == original_request for row in partial))
        self.execute(f"UPDATE `{table}` SET title='new title',content='newest content' WHERE id=1")
        together = self.tasks(table)
        self.assertEqual(2, len(together))
        self.assertEqual(1, len({row[1] for row in together}))
        self.assertNotEqual(original_request, together[0][1])
        self.execute(f"DELETE FROM `{table}`")
        self.assertEqual((), self.tasks(table))
        self.assertEqual(((0,),), self.execute(
            "SELECT COUNT(*) FROM oceanbase.__all_ai_column_group g "
            "JOIN oceanbase.__all_table t ON t.table_id=g.table_id WHERE t.table_name=%s", (table,)))
        self.assertTrue(self.server.requests.empty())

    def test_null_dependency_comparison_and_rollback_keep_request_identity(self):
        table = self.create("nullable")
        self.execute(f"INSERT INTO `{table}`(id,content) VALUES(1,NULL)")
        initial = self.tasks(table)
        self.assertEqual(1, len(initial))
        self.execute(f"UPDATE `{table}` SET content=NULL WHERE id=1")
        self.assertEqual(initial, self.tasks(table))
        self.execute("SAVEPOINT before_dependency_change")
        self.execute(f"UPDATE `{table}` SET content='' WHERE id=1")
        changed = self.tasks(table)
        self.assertEqual(1, len(changed))
        self.assertNotEqual(initial[0][1], changed[0][1])
        self.execute("ROLLBACK TO SAVEPOINT before_dependency_change")
        self.assertEqual(initial, self.tasks(table))
        self.assertTrue(self.server.requests.empty())


if __name__ == "__main__":
    unittest.main(verbosity=2)
