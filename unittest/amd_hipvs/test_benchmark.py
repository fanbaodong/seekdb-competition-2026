"""Numeric correctness and rejection cases; these tests never claim GPU execution."""
import copy
import math
import unittest

import numpy as np

from benchmark import (assess_results, exact_neighbors, fingerprint, generate_data,
                       identifier, match_evidence, parse_options, query_sql,
                       summarize, validate_comparable)


class NumericTests(unittest.TestCase):
    def test_exact_l2_uses_doc_ids_not_rows(self):
        vectors = np.array([[3, 0], [0, 0], [0, 2]], dtype=np.float32)
        ids = np.array([2**53 + 201, 2**53 + 17, 2**53 + 99], dtype=np.int64)
        result = exact_neighbors(vectors, np.array([[0, .5]], dtype=np.float32), ids, 2)
        self.assertEqual(result, [[2**53 + 17, 2**53 + 99]])

    def test_exact_l2_handles_large_offsets_without_dot_product_cancellation(self):
        vectors = np.array([[1e7, 1], [1e7, 2], [1e7, 3]], dtype=np.float32)
        self.assertEqual(exact_neighbors(vectors, np.array([[1e7, 2.9]], np.float32), np.array([9, 20, 31]), 1), [[31]])

    def test_exact_ties_have_deterministic_id_order(self):
        self.assertEqual(exact_neighbors(np.array([[1], [-1]], np.float32), np.array([[0]], np.float32), np.array([9, 3]), 2), [[3, 9]])

    def test_exact_rejects_nonfinite_or_duplicate_identity(self):
        with self.assertRaises(ValueError):
            exact_neighbors(np.array([[math.nan]]), np.array([[0]]), np.array([7]), 1)
        with self.assertRaises(ValueError):
            exact_neighbors(np.array([[0], [1]]), np.array([[0]]), np.array([7, 7]), 1)
        with self.assertRaises(ValueError):
            exact_neighbors(np.array([[0]]), np.array([[0]]), np.array([7]), 2)

    def test_data_seed_and_large_irregular_ids(self):
        a = generate_data(31, 4, 7, 12)
        b = generate_data(31, 4, 7, 12)
        c = generate_data(31, 4, 7, 13)
        self.assertEqual(fingerprint(*a), fingerprint(*b))
        self.assertNotEqual(fingerprint(*a), fingerprint(*c))
        self.assertGreater(int(a[2].min()), 2**53)
        self.assertEqual(len(set(map(int, a[2]))), 31)
        self.assertEqual(a[0].dtype, np.float32)
        self.assertFalse(np.all(np.diff(a[2]) == 1))

    def test_recall_does_not_depend_on_rank_order(self):
        result = assess_results([[(99, .1), (17, .2)], [(99, .1), (21, .2)]], [[17, 99], [17, 21]], [17, 99, 21], 2)
        self.assertEqual(result["recall_at_k"], .75)
        self.assertEqual(result["min_query_recall"], .5)

    def test_rejects_missing_duplicate_unknown_lossy_or_bad_distance_results(self):
        for results in ([], [[(17, 0)]], [[(17, 0), (17, 1)]], [[(17, 0), (42, 1)]],
                        [[(17.0, 0), (99, 1)]], [[(17, math.inf), (99, 1)]],
                        [[(17, -1), (99, 1)]], [[(17, 2), (99, 1)]]):
            with self.subTest(results=results), self.assertRaises(ValueError):
                assess_results(results, [[17, 99]], [17, 99], 2)


class InputTests(unittest.TestCase):
    def test_identifier_rejects_fragments(self):
        self.assertEqual(identifier("ahb_123_cpu_1"), "`ahb_123_cpu_1`")
        for bad in ("test;DROP", "a`", "db.table", "a b", "", "x" * 65):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                identifier(bad)

    def test_with_grammar_rejects_injection_and_duplicates(self):
        for bad in ("lib=vsag; DROP TABLE t", "lib='vsag'", "lib=vsag,lib=other", "x=(select 1)", "lib=vsag --comment", ""):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                parse_options(bad)
        self.assertEqual(parse_options(" TYPE = HNSW ,m=16"), {"type": "hnsw", "m": "16"})

    def test_rejects_quality_parameter_changes(self):
        cpu = parse_options("distance=l2,type=hnsw,lib=vsag,m=16,ef_construction=200,ef_search=200")
        for key, value in (("type", "hnsw_sq"), ("distance", "ip"), ("m", "8"), ("ef_search", "10"), ("ef_construction", "50")):
            with self.subTest(key=key), self.assertRaises(ValueError):
                validate_comparable(cpu, dict(cpu, **{key: value}), 10)

    def test_query_does_not_interpolate_vectors(self):
        query = query_sql("ahb_t", "ahb_idx", 10)
        self.assertEqual(query.count("%s"), 2)
        self.assertIn("INDEX(t ahb_idx)", query)
        self.assertIn("APPROXIMATE LIMIT 10", query)
        with self.assertRaises(ValueError):
            query_sql("ahb_t", "x) */ DROP", 10)


class AcceptanceGateTests(unittest.TestCase):
    @staticmethod
    def report():
        # Deliberately no GPU evidence: fictitious timings are unit-test inputs only.
        return {"config": {"repeats": 3, "cpu_only": False, "min_recall": .9,
                           "rows": 100, "dim": 8, "target_speedup": 2},
                "server_environment": {}, "rounds": [
                    {"mode": mode, "build_sec": duration, "index_name": f"idx_{mode}_{i}",
                     "table_name": f"table_{mode}_{i}", "coverage_found": 100,
                     "quality": {"recall_at_k": .99}, "vector_plan_verified": True}
                    for mode, times in (("cpu", [10., 20., 90.]), ("gpu", [4., 5., 12.]))
                    for i, duration in enumerate(times)]}

    def test_no_gpu_evidence_never_produces_speedup(self):
        result = summarize(self.report(), [])
        self.assertEqual(result["status"], "UNVERIFIED")
        self.assertIsNone(result["speedup"])
        self.assertFalse(result["meets_target"])
        self.assertEqual(result["cpu_median_build_sec"], 20)
        self.assertEqual(result["gpu_median_build_sec"], 5)

    def test_incomplete_run_never_produces_speedup(self):
        report = self.report()
        report["rounds"].pop()
        self.assertEqual(summarize(report, [])["status"], "INCOMPLETE")

    def test_mean_recall_does_not_hide_a_failed_round(self):
        report = self.report()
        report["rounds"][-1]["quality"]["recall_at_k"] = .8
        self.assertEqual(summarize(report, [])["status"], "QUALITY_OR_FUNCTION_FAILED")

    def test_quality_does_not_hide_a_missing_id_or_unused_index(self):
        for key, value in (("coverage_found", 99), ("vector_plan_verified", False)):
            report = self.report()
            report["rounds"][0][key] = value
            self.assertEqual(summarize(report, [])["status"], "QUALITY_OR_FUNCTION_FAILED")

    def test_cpu_only_never_claims_gpu_acceptance(self):
        report = self.report()
        report["config"]["cpu_only"] = True
        report["rounds"] = report["rounds"][:3]
        result = summarize(report, [])
        self.assertEqual(result["status"], "CPU_ONLY")
        self.assertIsNone(result["speedup"])
        self.assertFalse(result["meets_target"])

    def test_invalid_timing_is_rejected(self):
        for invalid in (0, -1, math.nan, math.inf):
            report = self.report()
            report["rounds"][0]["build_sec"] = invalid
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                summarize(report, [])

    def test_fallback_or_wrong_build_evidence_is_rejected(self):
        row = self.report()["rounds"][-1]
        rejected = [
            {"table_name": "another_table", "index_name": row["index_name"]},
            {"table_name": row["table_name"], "index_name": row["index_name"], "backend": "cpu"},
            {"table_name": row["table_name"], "index_name": row["index_name"], "producer": "seekdb-hipvs-build",
             "backend": "hipvs", "device_vendor": "AMD", "gpu_compute": True, "cpu_fallback": True},
        ]
        for evidence in rejected:
            with self.subTest(evidence=evidence):
                verified, reason = match_evidence(row, [evidence], 100, 8)
                self.assertIsNone(verified)
                self.assertTrue(reason)


if __name__ == "__main__":
    unittest.main()
