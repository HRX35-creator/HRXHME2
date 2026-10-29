import json
import tempfile
import unittest
from pathlib import Path

import numpy as np

from benchmark import load_dataset, neighbor_record
from make_dataset import generate
from metrics import exact_neighbors, score_results, vector_literal
from pack_results import pack


class RecallTests(unittest.TestCase):
    def setUp(self):
        self.truth = [{"query_id": 0, "ids": [101, 202]}, {"query_id": 1, "ids": [303, 404]}]
        self.ids = [101, 202, 303, 404, 505]

    def result(self, qid, ids):
        return {"query_id": qid, "neighbors": [{"id": x, "distance": 1.0} for x in ids]}

    def score(self, rows):
        return score_results(self.truth, rows, self.ids)

    def test_complete_results(self):
        s = self.score([self.result(0, [202, 101]), self.result(1, [303, 404])])
        self.assertEqual(s["recall_at_k"], 1)
        self.assertTrue(s["valid_results"])

    def test_missing_query_counts_zero(self):
        s = self.score([self.result(0, [101, 202])])
        self.assertEqual(s["recall_at_k"], 0.5)
        self.assertEqual(s["denominator"], 4)
        self.assertFalse(s["valid_results"])

    def test_short_response_cannot_improve_recall(self):
        s = self.score([self.result(0, [101]), self.result(1, [303])])
        self.assertEqual(s["recall_at_k"], 0.5)
        self.assertFalse(s["valid_results"])

    def test_duplicate_document_not_double_counted(self):
        s = self.score([self.result(0, [101, 101]), self.result(1, [303, 404])])
        self.assertEqual(s["recall_at_k"], 0.75)
        self.assertFalse(s["valid_results"])

    def test_duplicate_query_not_double_counted(self):
        s = self.score([self.result(0, [101, 202]), self.result(0, [101, 202])])
        self.assertEqual(s["recall_at_k"], 0.5)
        self.assertFalse(s["valid_results"])

    def test_unknown_ids_and_nan_rejected(self):
        row = self.result(0, [9999, 101])
        row["neighbors"][1]["distance"] = float("nan")
        s = self.score([row])
        self.assertEqual(s["recall_at_k"], 0)
        self.assertFalse(s["valid_results"])

    def test_padding_beyond_k_does_not_score(self):
        s = self.score([self.result(0, [505, 303, 101, 202])])
        self.assertEqual(s["recall_at_k"], 0)
        self.assertFalse(s["valid_results"])

    def test_malformed_rows_and_neighbors_fail_cleanly(self):
        s = self.score([None, {"query_id": 0, "neighbors": [None, {"id": True, "distance": 0}]}])
        self.assertFalse(s["valid_results"])
        self.assertEqual(s["recall_at_k"], 0)

    def test_invalid_server_distance_preserves_json_evidence(self):
        record = neighbor_record(101, float("nan"))
        self.assertEqual(record["distance"], "nan")
        json.dumps(record, allow_nan=False)
        self.assertFalse(self.score([{"query_id": 0, "neighbors": [record]}])["valid_results"])


class DataTests(unittest.TestCase):
    def test_exhaustive_truth_and_large_ids(self):
        ids = np.array([8589934627, 8589934593, 8589934610], dtype=np.int64)
        base = np.array([[0, 0], [3, 4], [10, 10]], dtype=np.float32)
        truth, distances, ties = exact_neighbors(base, np.array([[1, 0]], dtype=np.float32), ids, 2)
        self.assertEqual(truth.tolist(), [[8589934627, 8589934593]])
        self.assertEqual(distances.tolist(), [[1.0, 20.0]])
        self.assertEqual(ties, [False])

    def test_tie_reported_and_deterministic(self):
        ids = np.array([20, 10, 30])
        base = np.array([[1], [-1], [4]], dtype=np.float32)
        truth, _, ties = exact_neighbors(base, np.array([[0]], dtype=np.float32), ids, 1)
        self.assertEqual(truth.tolist(), [[10]])
        self.assertEqual(ties, [True])

    def test_nonfinite_input_refused(self):
        with self.assertRaises(ValueError):
            exact_neighbors(np.array([[np.inf]], dtype=np.float32), np.array([[0]], dtype=np.float32), np.array([1]), 1)

    def test_sql_float32_roundtrip(self):
        values = np.random.default_rng(42).normal(size=5000).astype(np.float32)
        actual = np.asarray(json.loads(vector_literal(values)), dtype=np.float32)
        np.testing.assert_array_equal(values, actual)

    def test_generation_and_hash_validation(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "data"
            generate(path, 40, 5, 4, 3, 321)
            manifest, base, queries, ids, truth = load_dataset(path)
            self.assertEqual(base.shape, (40, 5))
            self.assertGreater(int(ids.min()), 2**32)
            content = path / "truth.json"
            content.write_text("[]", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "hash mismatch"):
                load_dataset(path)

    def test_existing_dataset_never_overwritten(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(FileExistsError):
                generate(tmp, 20, 3, 2, 2)


class BackupTests(unittest.TestCase):
    def test_only_evidence_files_packaged_and_verified(self):
        import hashlib
        import tarfile
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "run"
            root.mkdir()
            (root / "result.json").write_text('{"ok":true}')
            (root / ".env").write_text("not-for-archive")
            (root / "private.pem").write_text("not-for-archive")
            target = Path(tmp) / "backup.tar.gz"
            hashes = pack(root, target)
            self.assertEqual(set(hashes), {"result.json"})
            with tarfile.open(target) as archive:
                self.assertEqual(archive.getnames(), ["evidence/result.json"])
                self.assertEqual(hashlib.sha256(archive.extractfile("evidence/result.json").read()).hexdigest(), hashes["result.json"])
            with self.assertRaises(ValueError):
                pack(root, target)

    def test_archive_must_be_outside_source(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(ValueError):
                pack(tmp, Path(tmp) / "recursive.tar.gz")


if __name__ == "__main__":
    unittest.main(verbosity=2)
