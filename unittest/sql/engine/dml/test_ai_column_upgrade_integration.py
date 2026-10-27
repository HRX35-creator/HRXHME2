#!/usr/bin/env python3
# Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
"""Run separately with SEEKDB_PREVIOUS_BINARY and SEEKDB_RECOVERY_BINARY.

Build an older private store, then check new variable defaults and persistence
without resetting any of its existing global settings.
"""
import os
from pathlib import Path
import signal
import unittest

import test_ai_column_recovery_integration as fixture


class AIColumnUpgradeIntegrationTest(fixture.AIColumnRecoveryIntegrationTest):
    test_completed_sibling_survives_crash_and_inflight_task_is_reclaimed = None

    @classmethod
    def setUpClass(cls):
        previous = os.environ.get("SEEKDB_PREVIOUS_BINARY")
        current = os.environ.get("SEEKDB_RECOVERY_BINARY")
        if not previous or not current:
            raise unittest.SkipTest("provide both native binaries to run the upgrade fixture")
        cls.current_binary = Path(current).resolve(strict=True)
        os.environ["SEEKDB_RECOVERY_BINARY"] = previous
        try:
            super().setUpClass()
        finally:
            os.environ["SEEKDB_RECOVERY_BINARY"] = current

    def reboot(self, phase):
        cls = type(self)
        previous_connection = cls.connection
        cls.process.kill()
        cls.process.wait(timeout=10)
        self.assertEqual(-signal.SIGKILL, cls.process.returncode)
        cls.stop_native()
        # The base fixture already owns the original connection's cleanup.
        if previous_connection is not self.original_connection:
            previous_connection.close()
        cls.binary = cls.current_binary
        cls.start_native(phase)
        cls.connection = cls.connect()

    def test_missing_default_preserves_existing_settings_and_new_global_value(self):
        self.original_connection = self.connection
        original = self.execute("SELECT @@global.sql_mode,@@global.lower_case_table_names,@@global.max_allowed_packet")[0]
        packet = 33554432 if original[2] != 33554432 else 67108864
        self.execute("SET GLOBAL max_allowed_packet=%s", (packet,))
        expected = (original[0], original[1], packet)
        self.assertEqual((expected,), self.execute(
            "SELECT @@global.sql_mode,@@global.lower_case_table_names,@@global.max_allowed_packet"))
        self.reboot("upgraded")
        self.assertEqual((expected,), self.execute(
            "SELECT @@global.sql_mode,@@global.lower_case_table_names,@@global.max_allowed_packet"))
        self.assertEqual(((0,),), self.execute("SELECT @@global.ai_column_strict_read"))
        self.execute("SET GLOBAL ai_column_strict_read=ON")
        self.assertEqual(((1,),), self.execute("SELECT @@global.ai_column_strict_read"))
        self.reboot("persisted")
        self.assertEqual((expected,), self.execute(
            "SELECT @@global.sql_mode,@@global.lower_case_table_names,@@global.max_allowed_packet"))
        self.assertEqual(((1, 1),), self.execute("SELECT @@global.ai_column_strict_read,@@session.ai_column_strict_read"))
        print("older-store upgrade preserved global overrides and persisted the new strict-read setting", flush=True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
