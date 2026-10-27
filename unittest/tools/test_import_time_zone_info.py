import importlib.util
import sys
import types
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "import_time_zone_info.py"


class FakeConnectorError(Exception):
    pass


class FakeCursor:
    def __init__(self, result_count=1, fail_on=None):
        self.executed = []
        self.result_count = result_count
        self.fail_on = fail_on

    def execute(self, sql):
        if self.fail_on is not None and self.fail_on in sql:
            raise FakeConnectorError("simulated SQL failure")
        self.executed.append(sql)

    def fetchone(self):
        return (self.result_count,)


class FakeConnection:
    def __init__(self, result_count=1, fail_on=None):
        self.cursor_instance = FakeCursor(result_count, fail_on)
        self.commit_count = 0

    def cursor(self, buffered=False):
        return self.cursor_instance

    def commit(self):
        self.commit_count += 1


def load_importer(connection):
    connector = types.ModuleType("mysql.connector")
    connector.Error = FakeConnectorError
    connector.errorcode = types.SimpleNamespace()
    connector.connect = lambda **kwargs: connection
    mysql = types.ModuleType("mysql")
    mysql.connector = connector
    old_mysql = sys.modules.get("mysql")
    old_connector = sys.modules.get("mysql.connector")
    sys.modules["mysql"] = mysql
    sys.modules["mysql.connector"] = connector
    try:
        spec = importlib.util.spec_from_file_location("import_time_zone_info", SCRIPT)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module
    finally:
        if old_mysql is None:
            sys.modules.pop("mysql", None)
        else:
            sys.modules["mysql"] = old_mysql
        if old_connector is None:
            sys.modules.pop("mysql.connector", None)
        else:
            sys.modules["mysql.connector"] = old_connector


class TimeZoneImporterTest(unittest.TestCase):
    def create_importer(self, connection):
        importer_module = load_importer(connection)
        importer = importer_module.TimeZoneInfoImporter()
        importer.conn = connection
        importer.cur = connection.cursor_instance
        importer.sql_list = ["INSERT INTO oceanbase.__all_time_zone_name VALUES (1)"]
        importer.tz_version_sql_list = [
            "REPLACE INTO oceanbase.__all_sys_stat VALUES (5, 'current_timezone_version', 1, 'current time zone version')"
        ]
        importer.expect_count = [0, 1, 1, 1]
        return importer

    def test_successful_import_commits_transaction_once(self):
        connection = FakeConnection()
        importer = self.create_importer(connection)

        importer.execute_sql()
        importer.check_result()

        self.assertEqual(1, connection.commit_count)

    def test_count_mismatch_does_not_commit_transaction(self):
        connection = FakeConnection(result_count=0)
        importer = self.create_importer(connection)

        with self.assertRaises(RuntimeError):
            importer.check_result()

        self.assertEqual(0, connection.commit_count)

    def test_version_sql_failure_does_not_commit_transaction(self):
        connection = FakeConnection(fail_on="REPLACE INTO")
        importer = self.create_importer(connection)

        with self.assertRaises(FakeConnectorError):
            importer.check_result()

        self.assertEqual(0, connection.commit_count)


if __name__ == "__main__":
    unittest.main()
