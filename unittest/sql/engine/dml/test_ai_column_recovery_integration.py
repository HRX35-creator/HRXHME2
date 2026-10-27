#!/usr/bin/env python3
# Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
"""Kill a private seekdb process during HTTP, then verify durable group recovery.

Run separately from other database suites with SEEKDB_RECOVERY_BINARY set to a
built native binary. The fixture owns its temporary database and child processes.
"""
import os
from pathlib import Path
import signal
import socket
import shutil
import subprocess
import tempfile
import threading
import time
import unittest

import pymysql

from ai_sql_test_utils import AISQLMockTestCase


class AIColumnRecoveryIntegrationTest(AISQLMockTestCase):
    MODEL_TYPE = "completion"
    ENDPOINT_PATH = "/chat/completions"
    process = None
    startup = None

    @staticmethod
    def default_response(request):
        return {"choices": [{"message": {"content": "unused"}}]}

    @classmethod
    def setUpClass(cls):
        binary = os.environ.get("SEEKDB_RECOVERY_BINARY")
        if not binary:
            raise unittest.SkipTest("set SEEKDB_RECOVERY_BINARY to run an isolated crash-recovery fixture")
        cls.binary = Path(binary).resolve(strict=True)
        cls.workspace = tempfile.TemporaryDirectory(prefix="seekdb-ai-recovery-")
        cls.addClassCleanup(cls.workspace.cleanup)
        cls.base = Path(cls.workspace.name)
        cls.socket = cls.base / "run/sql.sock"
        cls.addClassCleanup(cls.stop_native)
        cls.addClassCleanup(lambda: getattr(cls, "connection", None) and cls.connection.close())
        old_socket = os.environ.get("SEEKDB_TEST_SOCKET")
        old_database = os.environ.get("SEEKDB_TEST_DATABASE")
        def restore_environment():
            for name, value in (("SEEKDB_TEST_SOCKET", old_socket), ("SEEKDB_TEST_DATABASE", old_database)):
                if value is None:
                    os.environ.pop(name, None)
                else:
                    os.environ[name] = value
        cls.addClassCleanup(restore_environment)
        os.environ["SEEKDB_TEST_SOCKET"] = str(cls.socket)
        os.environ["SEEKDB_TEST_DATABASE"] = "ai_recovery_regression"
        cls.start_native("initial")
        super().setUpClass()

    @classmethod
    def connect(cls):
        return pymysql.connect(unix_socket=str(cls.socket), user="root", autocommit=True,
                               database="ai_recovery_regression", charset="utf8mb4", read_timeout=15)

    @classmethod
    def start_native(cls, phase):
        if cls.process is not None and cls.process.poll() is None:
            raise RuntimeError("the fixture already owns a live server")
        cls.startup = (cls.base / (phase + "-startup.log")).open("w")
        command = [str(cls.binary), "--nodaemon", "--base-dir", str(cls.base), "--log-level", "DEBUG" if os.environ.get("SEEKDB_RECOVERY_DEBUG") else "ERROR"]
        for setting in ("mysql_port_mode=disabled", "memory_budget=1G", "datafile_size=512M",
                        "datafile_maxsize=2G", "log_disk_size=2G", "net_thread_count=2"):
            command.extend(("--parameter", setting))
        cls.process = subprocess.Popen(command, stdout=cls.startup, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 180
        last_error = None
        while time.monotonic() < deadline:
            if cls.process.poll() is not None:
                raise RuntimeError(f"seekdb exited at {phase}: {cls.process.returncode}")
            connection = pymysql.Connection(unix_socket=str(cls.socket), user="root", autocommit=True,
                                            connect_timeout=2, read_timeout=5, defer_connect=True)
            stream = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                stream.settimeout(2)
                stream.connect(str(cls.socket))
                connection.connect(sock=stream)
                with connection.cursor() as cursor:
                    cursor.execute("CREATE DATABASE IF NOT EXISTS ai_recovery_regression")
                return
            except (OSError, pymysql.MySQLError) as error:
                last_error = error
                time.sleep(0.1)
            finally:
                if connection.open:
                    connection.close()
                stream.close()
        raise RuntimeError(f"seekdb startup timed out at {phase}: {last_error}")

    @classmethod
    def stop_native(cls):
        if cls.process is not None and cls.process.poll() is None:
            cls.process.send_signal(signal.SIGTERM)
            try:
                cls.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                cls.process.kill()
                cls.process.wait(timeout=10)
        if cls.startup is not None:
            cls.startup.close()
            cls.startup = None

    def test_completed_sibling_survives_crash_and_inflight_task_is_reclaimed(self):
        entered, release = threading.Event(), threading.Event()
        self.addCleanup(release.set)
        attempts = {"First:source": 0, "Second:source": 0}
        lock = threading.Lock()

        def response(request):
            content = request["messages"][-1]["content"]
            with lock:
                attempts[content] += 1
                attempt = attempts[content]
            if content == "Second:source" and attempt == 1:
                entered.set()
                release.wait(30)
            value = "first-complete" if content.startswith("First:") else "recovered-second"
            return {"choices": [{"message": {"content": value}}]}

        self.server.default_response = response
        table = self.model + "_durable"
        self.execute(f"CREATE TABLE `{table}`(id INT PRIMARY KEY,content VARCHAR(30),"
                     f"first_result VARCHAR(100) AI COLUMN(AI_COMPLETE('{self.model}',CONCAT('First:',content))),"
                     f"second_result VARCHAR(100) AI COLUMN(AI_COMPLETE('{self.model}',CONCAT('Second:',content))))")
        self.addCleanup(self.execute, f"DROP TABLE `{table}`")
        if os.environ.get("SEEKDB_RECOVERY_DEBUG"):
            self.execute("SET GLOBAL ob_log_level='DEBUG'")
        self.execute(f"INSERT INTO `{table}`(id,content) VALUES(1,'source')")
        self.assertTrue(entered.wait(10), "second HTTP request did not begin")
        query = ("SELECT task.request_id,task.column_id,task.definition_version,task.state "
                 "FROM oceanbase.__all_ai_column_task task JOIN oceanbase.__all_table t "
                 "ON t.table_id=task.table_id WHERE t.table_name=%s ORDER BY task.column_id")
        before = self.execute(query, (table,))
        self.assertEqual(2, len(before))
        self.assertEqual([1, 0], [row[3] for row in before])
        self.assertEqual(before[0][0], before[1][0])
        self.assertEqual(((None, None),), self.execute(f"SELECT first_result,second_result FROM `{table}`"))
        type(self).process.kill()
        type(self).process.wait(timeout=10)
        self.assertEqual(-signal.SIGKILL, type(self).process.returncode)
        type(self).stop_native()
        restarted = time.monotonic()
        type(self).start_native("recovered")
        type(self).connection = type(self).connect()
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            result = self.execute(f"SELECT first_result,second_result FROM `{table}`")
            self.assertEqual(result[0][0] is None, result[0][1] is None, "group was partially published after recovery")
            if result == (("first-complete", "recovered-second"),):
                break
            remaining = self.execute(query, (table,))
            for row in remaining:
                self.assertIn(row[:3], [entry[:3] for entry in before])
            time.sleep(0.05)
        else:
            print("RECOVERY_ROW", result, flush=True)
            print("RECOVERY_DEFINITIONS",self.execute("SELECT a.column_id,a.definition_version,a.state_column_id,a.request_column_id,a.result_spec_column_id FROM oceanbase.__all_ai_column a JOIN oceanbase.__all_table t ON t.table_id=a.table_id WHERE t.table_name=%s",(table,)),flush=True)
            print("RECOVERY_PENDING", self.execute(query, (table,)), attempts, self.execute(
                "SELECT g.lease_owner,g.lease_epoch,g.eligible_ts,g.lease_until FROM oceanbase.__all_ai_column_group g "
                "JOIN oceanbase.__all_table t ON t.table_id=g.table_id WHERE t.table_name=%s", (table,)), flush=True)
            output = os.environ.get("SEEKDB_RECOVERY_ARTIFACT_DIR")
            if output:
                destination = Path(output)
                destination.mkdir(parents=True, exist_ok=True)
                for log in type(self).base.rglob("*.log"):
                    shutil.copyfile(log, destination / log.name)
            self.fail("inflight request was not recovered within the new boot")
        elapsed = time.monotonic() - restarted
        self.assertLess(elapsed, 30, "same-address restart waited for the old 90-second lease")
        self.assertEqual((), self.execute(query, (table,)))
        self.assertEqual(((0,),), self.execute(
            "SELECT COUNT(*) FROM oceanbase.__all_ai_column_group g JOIN oceanbase.__all_table t "
            "ON t.table_id=g.table_id WHERE t.table_name=%s", (table,)))
        with lock:
            self.assertEqual({"First:source": 1, "Second:source": 2}, attempts)
        release.set()
        print(f"recovered_group_in_seconds={elapsed:.3f}; first HTTP calls=1; inflight HTTP calls=2", flush=True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
