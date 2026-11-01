#!/usr/bin/env python3
"""Validate M4 discovery artifacts produced by `run_tests.py --profile m4`.

Checks the exported App records, the named xcresult attachments and the
presence of each run ID in the system log. It does not decide whether a
discovery happened; it only proves that the evidence exists and is consistent.
"""
from __future__ import annotations

import argparse
import datetime
import json
import sys
from pathlib import Path

SCHEMA = "awarelab.m4-validation"
SCHEMA_VERSION = 1
APP_ACTIONS = ["launch", "m4_target_expected", "capability_check",
               "discovery_requested", "discovery_container_closed"]
from service_contract import SERVICE, TARGET_NAME


def _load_attachments(artifact_dir):
    manifest_path = artifact_dir / "attachments" / "manifest.json"
    if not manifest_path.exists():
        return {}
    files = {}
    for group in json.loads(manifest_path.read_text()):
        for item in group.get("attachments", []):
            files[item["suggestedHumanReadableName"]] = item["exportedFileName"]
    return files


def _seen_run_ids(system_log, expected):
    seen = set()
    with system_log.open() as handle:
        for line in handle:
            seen.update(run_id for run_id in expected if run_id in line)
    return seen


def _attachment_path(artifact_dir, attachment_files, run_id, suffix):
    names = [name for name in attachment_files if run_id + suffix in name]
    assert names, f"missing {suffix} attachment"
    path = artifact_dir / "attachments" / attachment_files[names[0]]
    assert path.is_file(), f"missing exported file for {suffix}"
    return path


def validate(artifact_dir, run_id, target_name, repeat=1, service=SERVICE):
    artifact_dir = Path(artifact_dir)
    attachment_files = _load_attachments(artifact_dir)
    expected = [f"{run_id}-{index:02d}" for index in range(1, repeat + 1)]
    errors = []
    valid = []
    runs = []

    system_log = artifact_dir / "system.ndjson"
    try:
        seen_ids = _seen_run_ids(system_log, expected)
    except OSError as exc:
        seen_ids = set()
        errors.append({"error": f"system log unreadable: {exc}"})

    for rid in expected:
        folder = artifact_dir / "app-runs" / rid
        try:
            events = [json.loads(line) for line in
                      (folder / "events.jsonl").read_text().splitlines()]
            configuration = json.loads((folder / "configuration.json").read_text())
            result = json.loads((folder / "result.json").read_text())
            assert configuration["runID"] == rid
            assert configuration["experimentID"] == run_id
            assert configuration["mode"] == "m4"
            assert configuration["targetName"] == target_name
            assert all(e["runID"] == rid and e["experimentID"] == run_id for e in events)
            assert [e["sequence"] for e in events] == list(range(1, len(events) + 1))
            assert [e["monotonicNS"] for e in events] == sorted(e["monotonicNS"] for e in events)
            assert [e["action"] for e in events] == APP_ACTIONS
            assert events[-1]["state"] == "cancelled" and result["state"] == "cancelled"
            assert result["stage"] == "M4-discovery-observation"
            assert result["wirelessVerified"] is False and result["runID"] == rid
            assert rid in seen_ids

            result_path = _attachment_path(artifact_dir, attachment_files, rid, "-m4-result")
            evidence = json.loads(result_path.read_text())
            assert evidence["run_id"] == rid and evidence["target_name"] == target_name
            assert evidence["mode"] == "m4" and evidence["service"] == service
            assert evidence["picker_verified"] is True
            assert evidence["device_tap_count"] == 0
            assert evidence["pairing_flow_started"] is False
            assert isinstance(evidence["target_found"], bool)
            assert isinstance(evidence["wait_seconds"], (int, float))
            observed_at = datetime.datetime.fromisoformat(
                str(evidence["observed_at_utc"]).replace("Z", "+00:00"))
            assert observed_at.tzinfo is not None
            _attachment_path(artifact_dir, attachment_files, rid, "-system-discovery")
            _attachment_path(artifact_dir, attachment_files, rid, "-system-tree")
            if evidence["target_found"]:
                _attachment_path(artifact_dir, attachment_files, rid, "-target-found")

            runs.append({
                "run_id": rid,
                "target_found": evidence["target_found"],
                "observation": evidence["observation"],
                "observed_at_utc": evidence["observed_at_utc"],
                "wait_seconds": evidence["wait_seconds"],
                "picker_verified": evidence["picker_verified"],
                "service_seen_in_picker": evidence["service_seen_in_picker"],
                "empty_state_seen": evidence["empty_state_seen"],
            })
            valid.append(rid)
        except (OSError, ValueError, KeyError, AssertionError, TypeError) as exc:
            errors.append({"run_id": rid, "error": str(exc) or "evidence assertion failed"})

    return {
        "schema": SCHEMA,
        "schemaVersion": SCHEMA_VERSION,
        "run_id": run_id,
        "target_name": target_name,
        "service": service,
        "repeat": repeat,
        "observedRunIDsInSystemLog": sorted(seen_ids),
        "validatedAppRuns": valid,
        "runs": runs,
        "errors": errors,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifact-dir", required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--target-name", default=TARGET_NAME)
    parser.add_argument("--repeat", type=int, choices=range(1, 11), default=1)
    parser.add_argument("--service", default=SERVICE)
    args = parser.parse_args(argv)

    artifact_dir = Path(args.artifact_dir)
    if not artifact_dir.is_dir():
        print(f"artifact directory not found: {artifact_dir}", file=sys.stderr)
        return 2
    report = validate(artifact_dir, args.run_id, args.target_name,
                      repeat=args.repeat, service=args.service)
    (artifact_dir / "m4-validation.json").write_text(
        json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    passed = not report["errors"] and len(report["validatedAppRuns"]) == args.repeat
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
