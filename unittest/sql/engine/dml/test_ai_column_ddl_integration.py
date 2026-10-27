#!/usr/bin/env python3
# Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
"""AI column definition regressions against an isolated native SQL instance."""
import unittest
import os

import pymysql

from ai_sql_test_utils import AISQLMockTestCase


class AIColumnDDLIntegrationTest(AISQLMockTestCase):
    MODEL_TYPE = "completion"

    @staticmethod
    def default_response(request):
        return {"choices": [{"message": {"content": "unused"}}]}

    def create(self, suffix, columns):
        table = self.model + "_" + suffix
        self.execute(f"CREATE TABLE `{table}` ({columns})")
        self.addCleanup(self.execute, f"DROP TABLE `{table}`")
        return table

    def test_show_definition_keeps_user_input_separate_from_type_instruction(self):
        table = self.create("ddl", "id INT PRIMARY KEY,content VARCHAR(50),"
                            f"tags JSON AI COLUMN(AI_COMPLETE('{self.model}',content)) "
                            "WITH(sync_mode='async',max_retries=0)")
        rows = self.execute(f"SHOW AI COLUMNS FROM `{table}`")
        self.assertEqual(1, len(rows))
        self.assertEqual("tags", rows[0][0])
        self.assertIn("AI_COMPLETE", rows[0][2].upper())
        self.assertIn("Return exactly one valid JSON value", rows[0][3])
        self.assertIn("system_prompt", rows[0][3])
        self.assertEqual(("async", 0), rows[0][4:])
        self.assertTrue(self.server.requests.empty())

    def test_alter_add_persists_definition_and_three_hidden_columns(self):
        table = self.create("add", "id INT PRIMARY KEY,content VARCHAR(50)")
        self.execute(f"ALTER TABLE `{table}` ADD COLUMN tags JSON AI COLUMN("
                     f"AI_COMPLETE('{self.model}',content)) WITH(max_retries=1)")
        self.assertEqual(1, self.execute(f"SHOW AI COLUMNS FROM `{table}`")[0][-1])
        rows = self.execute("SELECT COUNT(*) FROM oceanbase.__all_ai_column a "
                            "JOIN oceanbase.__all_table t ON t.table_id=a.table_id "
                            "JOIN oceanbase.__all_column c ON c.table_id=a.table_id "
                            "AND c.column_id IN(a.state_column_id,a.request_column_id,a.result_spec_column_id) "
                            "WHERE t.table_name=%s AND c.is_hidden=1", (table,))
        self.assertEqual(((3,),), rows)
        self.assertTrue(self.server.requests.empty())

    def test_invalid_options_and_forward_ai_dependency_fail_without_inference(self):
        for options in ("sync_mode='sync'", "max_retries=-1", "max_retries=1,max_retries=2",
                        "unrecognized=1", "max_retries=1.5"):
            with self.subTest(options=options):
                with self.assertRaises(pymysql.MySQLError):
                    self.create("bad", "content VARCHAR(50),"
                                f"tags JSON AI COLUMN(AI_COMPLETE('{self.model}',content)) WITH({options})")
        with self.assertRaises(pymysql.MySQLError):
            self.create("forward", "content VARCHAR(50),"
                        f"first_ai VARCHAR(50) AI COLUMN(AI_COMPLETE('{self.model}',second_ai)),"
                        f"second_ai VARCHAR(50) AI COLUMN(AI_COMPLETE('{self.model}',content))")
        self.assertTrue(self.server.requests.empty())

    def test_show_create_round_trip_preserves_ai_definition(self):
        table = self.create("source", "id INT PRIMARY KEY,content VARCHAR(50),"
                            f"summary VARCHAR(100) AI COLUMN(AI_COMPLETE('{self.model}',content)) "
                            "WITH(max_retries=1)")
        ddl = self.execute(f"SHOW CREATE TABLE `{table}`")[0][1]
        self.assertIn("AI COLUMN", ddl.upper())
        clone = self.model + "_roundtrip"
        self.execute(ddl.replace(f"`{table}`", f"`{clone}`", 1))
        self.addCleanup(self.execute, f"DROP TABLE `{clone}`")
        self.assertEqual(1, len(self.execute(f"SHOW AI COLUMNS FROM `{clone}`")))
        self.assertEqual(1, self.execute(f"SHOW AI COLUMNS FROM `{clone}`")[0][-1])
        self.assertTrue(self.server.requests.empty())

    def test_create_like_owns_each_definition_as_the_new_creator(self):
        source = self.create("like_source", "id INT PRIMARY KEY,content VARCHAR(50),"
                             f"first_ai VARCHAR(100) AI COLUMN(AI_COMPLETE('{self.model}',content)),"
                             f"second_ai VARCHAR(100) AI COLUMN(AI_COMPLETE('{self.model}',content))")
        clone = self.model + "_like_clone"
        username = self.model + "_creator"
        database = self.connection.db.decode()
        self.execute("CREATE USER %s@%s IDENTIFIED BY %s", (username, "%", "local-mock-only"))
        self.addCleanup(self.execute, "DROP USER %s@%s", (username, "%"))
        self.execute(f"GRANT ALL ON `{database}`.* TO %s@%s", (username, "%"))
        source_owners = self.execute(
            "SELECT a.owner_user_id FROM oceanbase.__all_ai_column a "
            "JOIN oceanbase.__all_table t ON t.table_id=a.table_id "
            "WHERE t.table_name=%s ORDER BY a.column_id", (source,))
        with pymysql.connect(unix_socket=os.environ["SEEKDB_TEST_SOCKET"], user=username,
                             password="local-mock-only", database=database,
                             autocommit=True) as connection:
            with connection.cursor() as cursor:
                cursor.execute(f"CREATE TABLE `{clone}` LIKE `{source}`")
        self.addCleanup(self.execute, f"DROP TABLE `{clone}`")
        new_owner = self.execute("SELECT user_id FROM oceanbase.__all_user WHERE user_name=%s",
                                 (username,))[0][0]
        self.assertNotEqual(source_owners[0][0], new_owner)
        self.assertEqual(((new_owner,), (new_owner,)), self.execute(
            "SELECT a.owner_user_id FROM oceanbase.__all_ai_column a "
            "JOIN oceanbase.__all_table t ON t.table_id=a.table_id "
            "WHERE t.table_name=%s ORDER BY a.column_id", (clone,)))
        self.assertEqual(source_owners, self.execute(
            "SELECT a.owner_user_id FROM oceanbase.__all_ai_column a "
            "JOIN oceanbase.__all_table t ON t.table_id=a.table_id "
            "WHERE t.table_name=%s ORDER BY a.column_id", (source,)))
        self.assertTrue(self.server.requests.empty())

    def test_alter_cannot_leave_dangling_ai_expression_bindings(self):
        table = self.create("binding", "id INT PRIMARY KEY,content VARCHAR(50),note VARCHAR(50),"
                            f"result VARCHAR(100) AI COLUMN(AI_COMPLETE('{self.model}',content))")
        original = self.execute(f"SHOW AI COLUMNS FROM `{table}`")
        for action in ("DROP COLUMN result", "DROP COLUMN content",
                       "CHANGE COLUMN content renamed VARCHAR(50)",
                       "MODIFY COLUMN content VARCHAR(100)",
                       "CHANGE COLUMN result renamed VARCHAR(100)",
                       f"MODIFY COLUMN note VARCHAR(50) AI COLUMN(AI_COMPLETE('{self.model}',content))"):
            with self.subTest(action=action):
                with self.assertRaises(pymysql.MySQLError) as raised:
                    self.execute(f"ALTER TABLE `{table}` {action}")
                self.assertEqual(1235, raised.exception.args[0])
                self.assertEqual(original, self.execute(f"SHOW AI COLUMNS FROM `{table}`"))
        self.execute(f"ALTER TABLE `{table}` CHANGE COLUMN note renamed_note VARCHAR(100)")
        self.execute(f"ALTER TABLE `{table}` ADD COLUMN extra INT")
        self.assertEqual(original, self.execute(f"SHOW AI COLUMNS FROM `{table}`"))
        self.assertTrue(self.server.requests.empty())


if __name__ == "__main__":
    unittest.main(verbosity=2)
