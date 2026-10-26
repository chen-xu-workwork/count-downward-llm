import json
import pathlib
import tempfile
import unittest
from unittest import mock

from hybrid_planner.extrapolation_selection import SCALES, prepare_selection


class ExtrapolationSelectionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.dataset = self.root / "dataset"
        self.problems = self.dataset / "problems"
        self.problems.mkdir(parents=True)
        (self.dataset / "domain.pddl").write_text("(define (domain test))", encoding="utf-8")
        for scale in (10, 20, 40, 45, 50):
            for numeric_id in range(1, 41):
                self.problem(scale, numeric_id).write_text(
                    "(define (problem p%d-%d))" % (scale, numeric_id), encoding="utf-8"
                )
        self.output = self.root / "results" / "selection"

    def problem(self, scale, numeric_id):
        return self.problems / ("problem_scale_%d_id_%s.pddl" % (scale, numeric_id))

    def prepare(self, **kwargs):
        return prepare_selection(self.dataset, self.output, **kwargs)

    def read_manifest(self, selection, mode):
        return json.loads(pathlib.Path(selection["manifest_paths"][mode]).read_text(encoding="utf-8"))

    def test_paired_sample_and_fixed_budgets(self):
        before = {p.name: p.read_bytes() for p in self.problems.iterdir()}
        selection = self.prepare()
        self.assertEqual(len(selection["selected"]), 40)
        for scale in SCALES:
            chosen = [item for item in selection["selected"] if item["scale"] == scale]
            self.assertEqual(len(chosen), 20)
            self.assertEqual(len({item["id"] for item in chosen}), 20)
            self.assertEqual([item["id"] for item in chosen], sorted(item["id"] for item in chosen))
            self.assertEqual(selection["pool"][str(scale)]["count"], 40)
        self.assertEqual(set(selection["pool"]), {"45", "50"})
        off = self.read_manifest(selection, "off")
        live = self.read_manifest(selection, "live")
        self.assertEqual([j["problem"] for j in off["jobs"]], [j["problem"] for j in live["jobs"]])
        for mode, manifest, budget in (("off", off, 0), ("live", live, 15)):
            self.assertEqual(manifest["default_mode"], mode)
            self.assertTrue(pathlib.Path(manifest["domain"]).is_absolute())
            self.assertTrue(pathlib.Path(manifest["problem_dir"]).is_absolute())
            for job in manifest["jobs"]:
                self.assertEqual(job["time_limit_seconds"], 3600)
                self.assertEqual(job["max_requests_per_iteration"], budget)
                self.assertEqual(job["mode"], mode)
                self.assertIn(job["scale"], SCALES)
                self.assertTrue(job["id"].endswith("-" + mode))
        self.assertEqual(before, {p.name: p.read_bytes() for p in self.problems.iterdir()})

    def test_seed_determinism_and_natural_pool_order(self):
        first = self.prepare()
        second = prepare_selection(self.dataset, self.root / "another")
        self.assertEqual(first["selected"], second["selected"])
        self.assertEqual([item["id"] for item in first["pool"]["45"]["files"]], list(range(1, 41)))
        changed = prepare_selection(self.dataset, self.root / "changed", sample_seed=17)
        self.assertNotEqual(first["selected"], changed["selected"])

    def test_reentry_reuses_without_sampling_or_writes(self):
        selection = self.prepare()
        paths = [self.output / "selection.json"] + [pathlib.Path(p) for p in selection["manifest_paths"].values()]
        initial = {p: (p.read_bytes(), p.stat().st_mtime_ns) for p in paths}
        with mock.patch("hybrid_planner.extrapolation_selection.random.Random", side_effect=AssertionError("resampled")):
            self.assertEqual(self.prepare(), selection)
        self.assertEqual(initial, {p: (p.read_bytes(), p.stat().st_mtime_ns) for p in paths})

    def test_insufficient_pool_and_duplicate_numeric_id(self):
        with self.assertRaisesRegex(ValueError, "fewer than requested"):
            self.prepare(sample_per_scale=41)
        self.problem(45, "01").write_text("duplicate", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Duplicate numeric"):
            self.prepare()
        self.assertFalse(self.output.exists())

    def test_changed_settings_rejected(self):
        self.prepare()
        with self.assertRaisesRegex(ValueError, "sample_seed"):
            self.prepare(sample_seed=99)
        with self.assertRaisesRegex(ValueError, "sample_per_scale"):
            self.prepare(sample_per_scale=19)

    def test_changed_domain_or_pool_rejected(self):
        self.prepare()
        domain = self.dataset / "domain.pddl"
        original = domain.read_bytes()
        domain.write_text("changed domain", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "domain_sha256"):
            self.prepare()
        domain.write_bytes(original)
        self.problem(45, 40).write_text("changed problem", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "pool"):
            self.prepare()

    def test_changed_dataset_path_rejected(self):
        self.prepare()
        other = self.root / "other_dataset"
        other.mkdir()
        (other / "domain.pddl").write_bytes((self.dataset / "domain.pddl").read_bytes())
        (other / "problems").mkdir()
        for scale in SCALES:
            for numeric_id in range(1, 41):
                source = self.problem(scale, numeric_id)
                (other / "problems" / source.name).write_bytes(source.read_bytes())
        with self.assertRaisesRegex(ValueError, "dataset_root"):
            prepare_selection(other, self.output)

    def test_missing_manifest_rebuilt_from_frozen_selection(self):
        selection = self.prepare()
        off_path = pathlib.Path(selection["manifest_paths"]["off"])
        expected = off_path.read_bytes()
        off_path.unlink()
        with mock.patch("hybrid_planner.extrapolation_selection.random.Random", side_effect=AssertionError("resampled")):
            self.prepare()
        self.assertEqual(off_path.read_bytes(), expected)

    def test_existing_manifest_tampering_rejected_without_overwrite(self):
        selection = self.prepare()
        live_path = pathlib.Path(selection["manifest_paths"]["live"])
        live = self.read_manifest(selection, "live")
        live["jobs"][0]["mode"] = "off"
        live_path.write_text(json.dumps(live), encoding="utf-8")
        before = live_path.read_bytes()
        with self.assertRaisesRegex(ValueError, "live manifest"):
            self.prepare()
        self.assertEqual(live_path.read_bytes(), before)

    def test_selected_hash_tampering_rejected(self):
        self.prepare()
        path = self.output / "selection.json"
        selection = json.loads(path.read_text(encoding="utf-8"))
        selection["selected"][0]["sha256"] = "bad"
        path.write_text(json.dumps(selection), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "invalid or changed problem"):
            self.prepare()

    def test_outputs_cannot_be_inside_source_dataset(self):
        with self.assertRaisesRegex(ValueError, "outside dataset_root"):
            prepare_selection(self.dataset, self.dataset / "generated_selection")
        self.assertFalse((self.dataset / "generated_selection").exists())


if __name__ == "__main__":
    unittest.main()
