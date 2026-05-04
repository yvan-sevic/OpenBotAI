#!/usr/bin/env python3
"""
Append grenade_human_events.csv rows into maps/<map>.grenade.json (BotGrenadeSpots format).

Usage:
  ./scripts/grenade_human_log_to_json.py --csv grenade_human_events.csv --maps-dir /path/to/cstrike/maps

If a .grenade.json exists, new spots are merged in (dedupe by from+target+type within 2 units).
"""

from __future__ import annotations

import argparse
import csv
import json
import os
from collections import defaultdict
from typing import Any


def _spot_key(spot: dict[str, Any]) -> tuple:
    return (
        round(float(spot["from_x"]), 1),
        round(float(spot["from_y"]), 1),
        round(float(spot["from_z"]), 1),
        round(float(spot["target_x"]), 1),
        round(float(spot["target_y"]), 1),
        round(float(spot["target_z"]), 1),
        str(spot.get("type", "any")),
    )


def load_existing(path: str, map_name: str) -> dict[str, Any]:
    if not os.path.isfile(path):
        return {"map": map_name, "version": 1, "spots": []}
    with open(path, "r", encoding="utf-8") as fp:
        return json.load(fp)


def main() -> int:
    ap = argparse.ArgumentParser(description="Merge human grenade throw CSV into .grenade.json files")
    ap.add_argument("--csv", default="grenade_human_events.csv", help="Path to grenade_human_events.csv")
    ap.add_argument(
        "--maps-dir",
        required=True,
        help="Game maps directory (e.g. .../cstrike/maps) where <map>.grenade.json is written",
    )
    ap.add_argument("--prefix", default="human_log", help="Name prefix for generated spot entries")
    args = ap.parse_args()

    if not os.path.isfile(args.csv):
        print(f"Missing CSV: {args.csv}")
        return 1

    by_map: dict[str, list[dict[str, str]]] = defaultdict(list)
    with open(args.csv, newline="", encoding="utf-8") as fp:
        reader = csv.DictReader(fp)
        for row in reader:
            m = (row.get("map") or "").strip()
            if not m:
                continue
            by_map[m].append(row)

    os.makedirs(args.maps_dir, exist_ok=True)

    for map_name, rows in sorted(by_map.items()):
        out_path = os.path.join(args.maps_dir, f"{map_name}.grenade.json")
        doc = load_existing(out_path, map_name)
        initial = len(doc.get("spots", []))
        existing_keys = {_spot_key(s) for s in doc.get("spots", [])}
        next_idx = initial + 1

        for row in rows:
            try:
                fx = float(row["origin_x"])
                fy = float(row["origin_y"])
                fz = float(row["origin_z"])
                tx = float(row["trace_x"])
                ty = float(row["trace_y"])
                tz = float(row["trace_z"]) + 4.0
            except (KeyError, ValueError):
                continue

            gtype = (row.get("grenade_type") or "he").strip().lower()
            if gtype not in ("he", "flash", "smoke"):
                gtype = "he"

            team = (row.get("team") or "any").strip()
            if team not in ("CT", "T"):
                team = "any"

            name = f"{args.prefix}_{next_idx:03d}"
            spot: dict[str, Any] = {
                "name": name,
                "type": gtype,
                "team": team,
                "from_x": fx,
                "from_y": fy,
                "from_z": fz,
                "target_x": tx,
                "target_y": ty,
                "target_z": tz,
            }

            key = _spot_key(spot)
            if key in existing_keys:
                continue
            existing_keys.add(key)
            doc.setdefault("spots", []).append(spot)
            next_idx += 1

        doc["map"] = map_name
        doc["version"] = 1
        with open(out_path, "w", encoding="utf-8") as fp:
            json.dump(doc, fp, indent=2)
            fp.write("\n")
        added = len(doc["spots"]) - initial
        print(f"{out_path}: {len(doc['spots'])} spots ({added} new from CSV)")

    print("Done.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
