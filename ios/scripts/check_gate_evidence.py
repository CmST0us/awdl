#!/usr/bin/env python3
"""Validate the single-session M4 gate observation attachment."""
import argparse
import datetime
import json
from pathlib import Path


def attachment_files(artifact_dir):
    manifest = artifact_dir / "attachments" / "manifest.json"
    if not manifest.exists():
        return {}
    files = {}
    for group in json.loads(manifest.read_text()):
        for item in group.get("attachments", []):
            files[item["suggestedHumanReadableName"]] = item["exportedFileName"]
    return files


def _epoch(value):
    parsed = datetime.datetime.fromisoformat(str(value).replace("Z", "+00:00"))
    assert parsed.tzinfo is not None, "timezone missing"
    return parsed.timestamp()


def _attachment(files, artifact_dir, prefix):
    names = [name for name in files if prefix in name]
    assert names, f"missing attachment {prefix}"
    path = artifact_dir / "attachments" / files[names[0]]
    assert path.is_file(), f"missing exported file {prefix}"


def validate(artifact_dir, run_id, count):
    artifact_dir = Path(artifact_dir)
    files = attachment_files(artifact_dir)
    names = [name for name in files if run_id + "-gate-result" in name]
    errors = []
    if not names:
        return {"valid": False, "errors": ["missing gate-result attachment"]}
    path = artifact_dir / "attachments" / files[names[0]]
    try:
        report = json.loads(path.read_text())
        cycles = report["gate_cycles"]
        ttl_mode = bool(report.get("ttl_mode", False))
        assert report["run_id"] == run_id
        assert report["gate_count"] == count
        assert len(cycles) == count
        assert [c["gate"] for c in cycles] == list(range(1, count + 1))
        assert all(c["target_found"] for c in cycles)
        assert report["failure"] is None
        assert report["device_tap_count"] == 0
        assert report["pairing_flow_started"] is False
        assert report["picker_verified"] is True
        if ttl_mode:
            assert count == 1, "TTL measurement is single-cycle"

        previous_off = None
        for cycle in cycles:
            gate = cycle["gate"]
            on_start = _epoch(cycle["on_wait_started_at_utc"])
            on_observed = _epoch(cycle["on_observed_at_utc"])
            off_start = _epoch(cycle["off_wait_started_at_utc"])
            assert on_start <= on_observed, f"gate {gate} on time not ordered"
            assert on_observed <= off_start, f"gate {gate} off starts before on observed"
            assert cycle["on_timeout_seconds"] > 0 and cycle["off_timeout_seconds"] > 0
            assert 0 <= cycle["on_wait_seconds"] <= cycle["on_timeout_seconds"] + 1
            assert 0 <= cycle["off_wait_seconds"] <= cycle["off_timeout_seconds"] + 1
            if cycle["off_disappeared"]:
                off_observed = _epoch(cycle["off_observed_at_utc"])
                assert off_start <= off_observed, f"gate {gate} off time not ordered"
                if previous_off is not None:
                    assert previous_off <= on_start, f"gate {gate} overlaps previous cycle"
                previous_off = off_observed
            else:
                assert ttl_mode, f"gate {gate} target did not disappear"
                assert cycle["off_observed_at_utc"] is None
                assert cycle["off_observed_monotonic_ns"] is None
                assert cycle["off_wait_seconds"] >= cycle["off_timeout_seconds"] - 1

            for suffix in ("-on", "-on-tree", "-off", "-off-tree"):
                _attachment(files, artifact_dir, f"{run_id}-gate-{gate:02d}{suffix}_")

        if ttl_mode:
            expected_absent = cycles[-1]["off_disappeared"]
            assert report["final_target_absent"] is expected_absent
        else:
            assert all(c["off_disappeared"] for c in cycles)
            assert report["final_target_absent"] is True
    except (OSError, ValueError, KeyError, AssertionError, TypeError) as exc:
        errors.append(str(exc) or "gate evidence assertion failed")
    return {"valid": not errors, "errors": errors, "cycles": count if not errors else 0}


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--artifact-dir", required=True)
    p.add_argument("--run-id", required=True)
    p.add_argument("--count", type=int, required=True)
    a = p.parse_args()
    report = validate(a.artifact_dir, a.run_id, a.count)
    (Path(a.artifact_dir) / "gate-validation.json").write_text(json.dumps(report, indent=2) + "\n")
    return 0 if report["valid"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
