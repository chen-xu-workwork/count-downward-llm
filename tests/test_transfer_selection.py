import collections
import json
import pathlib
import tempfile
import unittest

from hybrid_planner.transfer_selection import prepare_selection


class TransferSelectionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.dataset = self.root / "Zenotravel input with spaces"
        self.problems = self.dataset / "problems"
        self.problems.mkdir(parents=True)
        self.domain = self.dataset / "domain.pddl"
        self.domain.write_text("; (define (domain depot))\n(DEFINE (DOMAIN ZENO-TRAVEL))", encoding="utf-8")
        for scale, count in ((10, 2), (20, 2), (30, 25), (40, 12)):
            for numeric_id in range(1, count + 1):
                (self.problems / ("problem_scale_%d_id_%d.pddl" % (scale, numeric_id))).write_text(
                    "(define (problem p%d_%d) (:domain zeno-travel))" % (scale, numeric_id), encoding="utf-8"
                )
        self.output = self.root / "results with spaces"

    def prepare(self, **overrides):
        return prepare_selection(self.dataset, self.output, **overrides)

    def manifest(self):
        return json.loads((self.output / "transfer_manifest.json").read_text(encoding="utf-8"))

    def availability(self, excluded=()):
        return {
            "schema_version": 1,
            "target_domain": "zeno-travel",
            "baseline_archive": "zenotravel-validation-all-off-r1",
            "eligible_problem_names": sorted(
                path.name for path in self.problems.iterdir()
                if path.name not in excluded
                and (path.name.startswith("problem_scale_30_") or path.name.startswith("problem_scale_40_"))
            ),
        }

    def test_defaults_select_only_30_and_40_with_live_budget_policy(self):
        selection = self.prepare()
        self.assertEqual(selection["target_domain"], "zeno-travel")
        self.assertEqual(collections.Counter(item["scale"] for item in selection["selected"]), {30: 20, 40: 10})
        self.assertEqual({k: v["count"] for k, v in selection["pool"].items()}, {"30": 25, "40": 12})
        manifest = self.manifest()
        self.assertEqual(manifest["default_mode"], "live")
        self.assertEqual(pathlib.Path(manifest["domain"]), self.domain.resolve())
        self.assertEqual(pathlib.Path(manifest["problem_dir"]), self.problems.resolve())
        self.assertEqual([job["problem"] for job in manifest["jobs"]], [item["name"] for item in selection["selected"]])
        for job in manifest["jobs"]:
            self.assertEqual(job["mode"], "live")
            expected = (1800, 10) if job["scale"] == 30 else (3600, 15)
            self.assertEqual((job["time_limit_seconds"], job["max_requests_per_iteration"]), expected)
            numeric_id = int(job["problem"].split("_id_")[1].split(".")[0])
            self.assertEqual(job["id"], "scale%d-id%d-live" % (job["scale"], numeric_id))
        self.assertEqual(len({item["name"] for item in selection["selected"]}), 30)
        self.assertEqual(sorted(p.name for p in self.output.iterdir()), ["selection.json", "transfer_manifest.json"])

    def test_reentry_reuses_exact_bytes_and_missing_manifest_rebuilds(self):
        first = self.prepare()
        before = {p.name: p.read_bytes() for p in self.output.iterdir()}
        self.assertEqual(self.prepare(), first)
        self.assertEqual({p.name: p.read_bytes() for p in self.output.iterdir()}, before)
        (self.output / "transfer_manifest.json").unlink()
        self.assertEqual(self.prepare(), first)
        self.assertEqual({p.name: p.read_bytes() for p in self.output.iterdir()}, before)

    def test_sampling_is_deterministic_in_another_output_location(self):
        a = self.prepare()
        b = prepare_selection(self.dataset, self.root / "another result")
        self.assertEqual(a["selected"], b["selected"])
        c = prepare_selection(self.dataset, self.root / "other seed", sample_seed=11)
        self.assertNotEqual(a["selected"], c["selected"])

    def test_changed_seed_and_counts_are_rejected_without_overwriting(self):
        self.prepare()
        before = {p.name: p.read_bytes() for p in self.output.iterdir()}
        for overrides in ({"sample_seed": 1}, {"sample_30": 19}, {"sample_40": 9}):
            with self.subTest(overrides=overrides):
                with self.assertRaisesRegex(ValueError, "new output directory"):
                    self.prepare(**overrides)
        self.assertEqual({p.name: p.read_bytes() for p in self.output.iterdir()}, before)

    def test_changed_unselected_problem_is_rejected(self):
        selection = self.prepare()
        selected = {item["name"] for item in selection["selected"]}
        unselected = next(p for p in self.problems.glob("problem_scale_30_*.pddl") if p.name not in selected)
        unselected.write_text(unselected.read_text(encoding="utf-8") + "\n; changed", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "new output directory"):
            self.prepare()

    def test_changed_domain_contents_are_rejected(self):
        self.prepare()
        self.domain.write_text(self.domain.read_text(encoding="utf-8") + "\n; changed", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "new output directory"):
            self.prepare()

    def test_wrong_domain_is_rejected_before_any_output(self):
        self.domain.write_text("(define (domain depots))", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Expected target domain zeno-travel"):
            self.prepare()
        self.assertFalse(self.output.exists())

    def test_problem_declaring_other_domain_is_rejected(self):
        p = self.problems / "problem_scale_30_id_1.pddl"
        p.write_text("(define (problem wrong) (:domain depot))", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "must declare domain zeno-travel"):
            self.prepare()
        self.assertFalse(self.output.exists())

    def test_insufficient_pool_is_rejected_before_any_output(self):
        with self.assertRaisesRegex(ValueError, "fewer than requested 13"):
            self.prepare(sample_40=13)
        self.assertFalse(self.output.exists())

    def test_invalid_sampling_types_and_counts_are_rejected(self):
        for overrides in ({"sample_30": 0}, {"sample_40": -1}, {"sample_30": True}, {"sample_40": 2.0}, {"sample_seed": False}):
            with self.subTest(overrides=overrides):
                with self.assertRaisesRegex(ValueError, "must be"):
                    self.prepare(**overrides)
        self.assertFalse(self.output.exists())

    def test_output_inside_input_is_rejected_without_creating_it(self):
        nested = self.dataset / "new results"
        for output in (self.dataset, nested):
            with self.subTest(output=output):
                with self.assertRaisesRegex(ValueError, "outside dataset_root"):
                    prepare_selection(self.dataset, output)
        self.assertFalse(nested.exists())

    def test_duplicate_numeric_ids_are_rejected(self):
        original = self.problems / "problem_scale_30_id_1.pddl"
        (self.problems / "problem_scale_30_id_01.pddl").write_bytes(original.read_bytes())
        with self.assertRaisesRegex(ValueError, "Duplicate numeric problem ID"):
            self.prepare()
        self.assertFalse(self.output.exists())

    def test_changed_manifest_is_rejected_without_replacing_selection(self):
        self.prepare()
        selection = (self.output / "selection.json").read_bytes()
        manifest = self.manifest()
        manifest["jobs"][0]["mode"] = "off"
        path = self.output / "transfer_manifest.json"
        path.write_text(json.dumps(manifest), encoding="utf-8")
        altered = path.read_bytes()
        with self.assertRaisesRegex(ValueError, "live manifest differs"):
            self.prepare()
        self.assertEqual((self.output / "selection.json").read_bytes(), selection)
        self.assertEqual(path.read_bytes(), altered)

    def test_changed_selection_is_rejected_even_if_names_still_belong_to_pool(self):
        selection = self.prepare()
        chosen = {item["name"] for item in selection["selected"]}
        alternate = next(item for item in selection["pool"]["30"]["files"] if item["name"] not in chosen)
        selection["selected"][0] = dict(alternate, scale=30)
        (self.output / "selection.json").write_text(json.dumps(selection), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "deterministic sample"):
            self.prepare()

    def test_baseline_replacement_preserves_the_other_29_problems(self):
        original = self.prepare()
        removed = next(item["name"] for item in original["selected"] if item["scale"] == 40)
        availability = self.availability(excluded=[removed])
        selected = prepare_selection(self.dataset, self.root / "with baseline", baseline_availability=availability)
        before = {item["name"] for item in original["selected"]}
        after = {item["name"] for item in selected["selected"]}
        self.assertEqual(len(before & after), 29)
        self.assertEqual(before - after, {removed})
        self.assertEqual(len(after), 30)
        self.assertEqual(collections.Counter(item["scale"] for item in selected["selected"]), {30: 20, 40: 10})
        self.assertEqual(selected["baseline_eligible_counts"], {"30": 25, "40": 11})
        self.assertEqual(selected["pool"], original["pool"])
        self.assertEqual(selected["baseline_availability"], availability)
        self.assertEqual(selected["replacements"], [{
            "scale": 40, "removed": removed, "added": next(iter(after - before)),
            "reason": "missing_baseline_record",
        }])
        again = prepare_selection(self.dataset, self.root / "with baseline", baseline_availability=availability)
        self.assertEqual(again, selected)

    def test_small_subset_can_replace_multiple_missing_baseline_records(self):
        original = self.prepare(sample_30=3, sample_40=2)
        missing = [item["name"] for item in original["selected"][:2]]
        availability = self.availability(excluded=missing)
        selected = prepare_selection(self.dataset, self.root / "small with baseline", 3, 2,
                                     baseline_availability=availability)
        before = {item["name"] for item in original["selected"]}
        after = {item["name"] for item in selected["selected"]}
        self.assertEqual(before - after, set(missing))
        self.assertEqual(len(after - before), 2)
        self.assertEqual({item["scale"] for item in selected["replacements"]}, {30})
        self.assertEqual(collections.Counter(item["scale"] for item in selected["selected"]), {30: 3, 40: 2})
        self.assertTrue(after <= set(availability["eligible_problem_names"]))

    def test_baseline_eligibility_needs_no_success_outcomes(self):
        # Complete unsuccessful baseline runs are represented by names exactly
        # like successful runs. Selection never asks for an outcome or a cost.
        availability = self.availability()
        availability["eligible_problem_names"].append("problem_scale_40_id_99999.pddl")
        selected = self.prepare(baseline_availability=availability)
        self.assertEqual(selected["replacements"], [])
        self.assertEqual(selected["baseline_eligible_counts"], {"30": 25, "40": 12})
        self.assertEqual(len(selected["selected"]), 30)

    def test_insufficient_baseline_availability_is_rejected_before_any_output(self):
        excluded = ["problem_scale_40_id_%d.pddl" % numeric_id for numeric_id in (1, 2, 3)]
        with self.assertRaisesRegex(ValueError, "9 problems with baseline records, fewer than requested 10"):
            self.prepare(baseline_availability=self.availability(excluded))
        self.assertFalse(self.output.exists())

    def test_changed_baseline_metadata_is_rejected_without_overwriting(self):
        availability = self.availability()
        self.prepare(baseline_availability=availability)
        before = {p.name: p.read_bytes() for p in self.output.iterdir()}
        availability["baseline_archive"] = "different baseline archive"
        with self.assertRaisesRegex(ValueError, "new output directory"):
            self.prepare(baseline_availability=availability)
        self.assertEqual({p.name: p.read_bytes() for p in self.output.iterdir()}, before)

    def test_invalid_baseline_metadata_is_rejected(self):
        invalid = [[], {}, dict(self.availability(), target_domain="depot"),
                   dict(self.availability(), eligible_problem_names=[1]),
                   dict(self.availability(), eligible_problem_names=["duplicate", "duplicate"])]
        for availability in invalid:
            with self.subTest(availability=availability):
                with self.assertRaisesRegex(ValueError, "baseline_availability"):
                    self.prepare(baseline_availability=availability)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
