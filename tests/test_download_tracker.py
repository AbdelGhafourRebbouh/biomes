import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import io

spec = importlib.util.spec_from_file_location("tracker", Path(__file__).parents[1] / "scripts/track_downloads.py")
tracker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tracker)


class TrackerTests(unittest.TestCase):
    def test_replacement_deletion_and_temporary_decrease(self):
        state = {"schemaVersion": 1, "assets": {}}
        asset = dict(id="1", name="biomesSetup.exe", tag="beta", count=10)
        state = tracker.merge(state, [asset], "2026-09-29")
        state = tracker.merge(state, [dict(asset, count=8)], "2026-09-30")
        self.assertEqual(state["total"], 10)
        state = tracker.merge(state, [dict(asset, id="2", count=3)], "2026-10-01")
        self.assertEqual(state["total"], 13)
        self.assertFalse(state["assets"]["1"]["present"])
        self.assertEqual(tracker.merge(state, [], "2026-10-02")["total"], 13)

    def test_daily_baseline_and_repeated_run(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            asset = dict(id="1", name="biomesSetup.exe", tag="beta", count=10)
            tracker.save(root, [asset], "2026-09-29T01:00:00Z")
            self.assertIsNone(json.loads((root / "daily/2026-09-29.json").read_text())["increase"])
            tracker.save(root, [dict(asset, count=14)], "2026-09-30T01:00:00Z")
            tracker.save(root, [dict(asset, count=15)], "2026-09-30T02:00:00Z")
            self.assertEqual(json.loads((root / "daily/2026-09-30.json").read_text())["increase"], 5)

    def test_filter_and_pagination(self):
        names = ["biomesSetup.exe", "Biomes-1.0.0-beta.8-windows-x64.zip", "update.json", "biomesSetup.exe.sha256", "source.zip"]
        release = dict(draft=False, tag_name="beta", assets=[dict(id=i, name=n, download_count=2) for i, n in enumerate(names)])
        pages = [io.BytesIO(json.dumps([release] + [dict(draft=True)] * 99).encode()), io.BytesIO(b'[]')]
        with patch.dict("os.environ", GH_TOKEN="fixture"), patch.object(tracker.urllib.request, "urlopen", side_effect=pages) as fetch:
            self.assertEqual(len(tracker.collect()), 2)
            self.assertEqual(fetch.call_count, 2)

    def test_corrupt_history_is_not_reset(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "downloads.json"
            path.write_text("broken")
            with self.assertRaises(ValueError):
                tracker.save(Path(folder), [], "2026-09-29T01:00:00Z")
            self.assertEqual(path.read_text(), "broken")

    def test_network_failure_is_not_an_empty_snapshot(self):
        with patch.dict("os.environ", GH_TOKEN="fixture"), patch.object(tracker.urllib.request, "urlopen", side_effect=OSError("offline")):
            with self.assertRaises(OSError):
                tracker.collect()
