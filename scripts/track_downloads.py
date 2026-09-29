"""Track observed app-asset downloads without discarding retired asset IDs."""
import argparse
import datetime as dt
import json
import os
from pathlib import Path
import urllib.request

REPO = "AbdelGhafourRebbouh/biomes"


def collect():
    assets = []
    headers = {"Accept": "application/vnd.github+json", "User-Agent": "biomes-download-tracker"}
    if os.environ.get("GH_TOKEN"):
        headers["Authorization"] = "Bearer " + os.environ["GH_TOKEN"]
    for page in range(1, 1001):
        request = urllib.request.Request(
            f"https://api.github.com/repos/{REPO}/releases?per_page=100&page={page}",
            headers=headers)
        with urllib.request.urlopen(request, timeout=30) as response:
            releases = json.load(response)
        for release in releases:
            if release["draft"]:
                continue
            for asset in release["assets"]:
                name = asset["name"]
                if ((name.startswith("biomesSetup") and name.endswith(".exe")) or
                        (name.startswith("Biomes-") and name.endswith("-windows-x64.zip"))):
                    assets.append(dict(id=str(asset["id"]), name=name, tag=release["tag_name"],
                                       count=asset["download_count"]))
        if len(releases) < 100:
            return assets
    raise RuntimeError("Release pagination limit exceeded; no statistics written")


def merge(previous, assets, timestamp):
    if previous.get("schemaVersion") != 1:
        raise ValueError("Unsupported statistics schema")
    records = {key: dict(value, present=False) for key, value in previous["assets"].items()}
    for asset in assets:
        if not isinstance(asset["count"], int) or asset["count"] < 0:
            raise ValueError("Invalid download count")
        old = records.get(asset["id"], {})
        records[asset["id"]] = dict(asset, count=max(old.get("count", 0), asset["count"]),
                                   observedCount=asset["count"], present=True,
                                   firstSeen=old.get("firstSeen", timestamp), lastSeen=timestamp)
    return dict(schemaVersion=1, updatedAt=timestamp,
                trackingStarted=previous.get("trackingStarted", timestamp), assets=records,
                total=sum(record["count"] for record in records.values()))


def save(output, assets, timestamp):
    output.mkdir(parents=True, exist_ok=True)
    state_path = output / "downloads.json"
    previous = json.loads(state_path.read_text()) if state_path.exists() else {"schemaVersion": 1, "assets": {}}
    state = merge(previous, assets, timestamp)
    snapshots = output / "daily"
    snapshots.mkdir(exist_ok=True)
    day = timestamp[:10]
    earlier = sorted(path for path in snapshots.glob("*.json") if path.stem < day)
    baseline = json.loads(earlier[-1].read_text())["total"] if earlier else None
    snapshot = dict(date=day, total=state["total"], assets=state["assets"],
                    increase=None if baseline is None else state["total"] - baseline)
    report = ["# biomes app downloads", "", f"Recorded total: **{state['total']}**", "",
              f"Updated: {timestamp}", "", "Installers and distribution ZIPs only. Includes repeat downloads and updates.",
              "Retired assets retain their highest observed counts. Previously lost downloads cannot be reconstructed.",
              "Daily increases are observed differences, not exact download dates; the first snapshot is a baseline.",
              "", "| Version | Recorded downloads |", "| --- | ---: |"]
    totals = {}
    for record in state["assets"].values():
        totals[record["tag"]] = totals.get(record["tag"], 0) + record["count"]
    report += [f"| {tag.replace('|', '/')} | {count} |" for tag, count in sorted(totals.items())]
    report += ["", "| Day (UTC) | Total | Observed increase |", "| --- | ---: | ---: |"]
    daily = [json.loads(path.read_text()) for path in earlier[-29:]] + [snapshot]
    report += [f"| {item['date']} | {item['total']} | {item['increase'] if item['increase'] is not None else 'Baseline'} |" for item in daily]
    report += ["", "Daily snapshots: [daily/](daily/). Asset IDs and history: [downloads.json](downloads.json).", ""]
    # All network reads and validation finish before replacing the local snapshot.
    for path, value in [(state_path, state), (snapshots / f"{day}.json", snapshot),
                        (output / "badge.json", dict(schemaVersion=1, label="App downloads", message=str(state["total"]), color="6f8e73"))]:
        path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
    (output / "README.md").write_text("\n".join(report), encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    save(args.output, collect(), dt.datetime.now(dt.timezone.utc).isoformat())
