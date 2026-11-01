#!/usr/bin/env python3
"""Validate M5 AccessorySetupKit evidence produced by `run_tests.py --profile m5`.

The validator proves that the App record, the ask-session record and the
XCUITest attachments exist and agree with the M5 contract. It does not decide
whether the board was listed, whether a PIN flow appeared or whether the system
paired anything; those remain device-run judgments.
"""
from __future__ import annotations

import argparse
import datetime
import json
import os
import re
import sys
from pathlib import Path

from pin_safety import pin_hits

SCHEMA = "awarelab.m5-validation"
SCHEMA_VERSION = 1
from service_contract import SERVICE
DIRECTION = "NAN-PAIRING-DUAL"
APP_ACTIONS = ["launch", "m5_target_expected", "capability_check"]
ACCESSORY_STATES = {"unauthorized", "awaitingAuthorization", "authorized", "unknown"}
TEST_KINDS = {"accessory_picker", "paired_device_snapshot", "stage_a_pairing"}
STAGE_A_CATEGORIES = {
    "picker_not_presented", "ask_failed", "target_not_listed",
    "target_not_hittable", "pin_ui_not_found", "pin_rejected",
    "authorized_timeout", "authorized_without_paired_device_id",
}
STAGE_A_PRE_PIN_CATEGORIES = {
    "picker_not_presented", "ask_failed", "target_not_listed",
    "target_not_hittable", "pin_ui_not_found",
}

OUTCOMES = {
    "not_started", "session_ready", "picker_requested", "picker_presented",
    "picker_failed", "picker_dismissed_without_accessory",
    "picker_dismissed_with_accessory", "accessory_observed", "authorized",
}


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


def _epoch(value):
    parsed = datetime.datetime.fromisoformat(str(value).replace("Z", "+00:00"))
    assert parsed.tzinfo is not None, "timezone missing"
    return parsed.timestamp()


def _derive_outcome(record):
    accessories = record.get("accessories") or []
    session = record.get("session") or {}
    if any(a.get("state") == "authorized" for a in accessories):
        return "authorized"
    if session.get("pickerCompletionError") is not None:
        return "picker_failed"
    if session.get("pickerDismissed"):
        return ("picker_dismissed_with_accessory" if session.get("accessoryAddedCount", 0)
                else "picker_dismissed_without_accessory")
    if accessories:
        return "accessory_observed"
    if session.get("pickerPresented"):
        return "picker_presented"
    if session.get("pickerRequested"):
        return "picker_requested"
    if session.get("activated"):
        return "session_ready"
    return "not_started"


def _validate_ask_record(record, run_id, target_name, service):
    assert record["schemaVersion"] == 1, "ask-session schemaVersion is not 1"
    assert record["runID"] == run_id, "ask-session runID mismatch"
    assert record["source"] == "ipad.app", "ask-session source mismatch"
    assert record["directionID"] == DIRECTION, "ask-session directionID mismatch"
    assert isinstance(record["appVersion"], str) and record["appVersion"], "appVersion missing"
    assert "WiFi" in record["supportedTechnologies"], "NSAccessorySetupKitSupports is not WiFi"
    assert record["pinHandling"] == "system_picker", \
        "the App must leave PIN handling to the system picker"
    assert "pin" not in record and "pinCode" not in record, "ask-session contains a PIN field"

    descriptor = record["descriptor"]
    assert descriptor["wifiAwareServiceName"] == service, \
        "descriptor service name does not match"
    assert descriptor["wifiAwareServiceRole"] == "subscriber", \
        "descriptor role is not subscriber"
    assert descriptor["expectedPairingName"] == target_name, \
        "descriptor expected pairing name does not match the test target"
    for key in ("wifiAwareVendorName", "wifiAwareModelName", "displayItemName"):
        assert isinstance(descriptor[key], str) and descriptor[key], f"descriptor {key} missing"

    events = record["events"]
    assert isinstance(events, list) and events, "ask-session has no events"
    assert [e["sequence"] for e in events] == list(range(1, len(events) + 1)), \
        "ask-session event sequence is not contiguous"
    assert all(isinstance(e["monotonicNS"], int) and e["monotonicNS"] >= 0 for e in events), \
        "ask-session event monotonicNS is invalid"
    assert [e["monotonicNS"] for e in events] == sorted(e["monotonicNS"] for e in events), \
        "ask-session events are not monotonic"
    for event in events:
        assert isinstance(event["event"], str) and event["event"].startswith("ask_")
        assert isinstance(event["detail"], str)
        _epoch(event["wallTime"])

    session = record["session"]
    assert session["activated"] is True, "ASK session was never activated"
    assert session.get("activationMonotonicNS") is not None, \
        "ASK activation time is missing"
    assert any(e["event"] == "ask_session_activated" for e in events), \
        "ask_session_activated event is missing"

    def count(name):
        return sum(1 for e in events if e["event"] == name)

    assert count("ask_accessory_added") == session.get("accessoryAddedCount", 0), \
        "accessory-added count does not match the session"
    assert count("ask_accessory_changed") == session.get("accessoryChangedCount", 0), \
        "accessory-changed count does not match the session"
    assert count("ask_accessory_removed") == session.get("accessoryRemovedCount", 0), \
        "accessory-removed count does not match the session"
    if session.get("pickerRequested"):
        assert count("ask_picker_requested") == 1, "ask_picker_requested event is missing"
        assert session.get("pickerRequestedMonotonicNS") is not None, \
            "picker request time is missing"
    if session.get("pickerPresented"):
        assert count("ask_picker_presented") == 1, "ask_picker_presented event is missing"
        assert session.get("pickerPresentedMonotonicNS") is not None, \
            "picker presented time is missing"
    if session.get("pickerDismissed"):
        assert count("ask_picker_dismissed") == 1, "ask_picker_dismissed event is missing"
        assert session.get("pickerDismissedMonotonicNS") is not None, \
            "picker dismissed time is missing"
    order = [session.get(key) for key in (
        "activationMonotonicNS", "pickerRequestedMonotonicNS",
        "pickerPresentedMonotonicNS", "pickerDismissedMonotonicNS")
        if session.get(key) is not None]
    assert order == sorted(order), "ASK session timestamps are not ordered"
    if session.get("pickerCompletionError") is not None:
        assert isinstance(session["pickerCompletionError"], str)
        assert any(e["event"] == "ask_picker_completion" for e in events)

    assert record["outcome"] in OUTCOMES
    assert record["outcome"] == _derive_outcome(record), "outcome does not match observed flags"

    accessories = record["accessories"]
    assert isinstance(accessories, list)
    accessory_ids = set()
    for accessory in accessories:
        assert accessory["state"] in ACCESSORY_STATES
        assert isinstance(accessory["displayName"], str) and accessory["displayName"]
        assert isinstance(accessory["wifiAwarePairedDeviceID"], int)
        assert accessory["wifiAwarePairedDeviceID"] >= 0
        assert isinstance(accessory["hasSSID"], bool)
        assert isinstance(accessory["hasBluetoothIdentifier"], bool)
        accessory_ids.add(accessory["wifiAwarePairedDeviceID"])

    paired = record["pairedDevices"]
    assert isinstance(paired, list)
    paired_ids = [device["id"] for device in paired]
    assert paired_ids == sorted(set(paired_ids)), "paired devices are not unique and sorted"
    for device in paired:
        assert isinstance(device["id"], int) and device["id"] >= 0
        for key in ("name", "pairingName", "vendorName", "modelName"):
            assert device.get(key) is None or isinstance(device[key], str)
    if not paired:
        assert record.get("pairedDeviceSnapshotError") is None or isinstance(
            record["pairedDeviceSnapshotError"], str)

    authorized = [a for a in accessories if a["state"] == "authorized"]
    for accessory in authorized:
        assert accessory["wifiAwarePairedDeviceID"] > 0, \
            "authorized accessory has no Wi-Fi Aware paired device ID"
        assert accessory["wifiAwarePairedDeviceID"] in paired_ids, \
            "authorized accessory ID is missing from the paired-device snapshot"
    return {
        "outcome": record["outcome"],
        "authorized_count": len(authorized),
        "accessory_ids": sorted(accessory_ids),
        "paired_device_ids": paired_ids,
        "paired_device_snapshot_error": record.get("pairedDeviceSnapshotError"),
    }


def _validate_result(evidence, record, run_id, target_name, service):
    assert evidence["run_id"] == run_id, "m5-result run_id mismatch"
    assert evidence["source"] == "xctest.physical", "m5-result source mismatch"
    assert evidence["mode"] == "m5", "m5-result mode mismatch"
    assert evidence["test"] in TEST_KINDS, "unknown m5-result test kind"
    assert evidence["target_name"] == target_name, "m5-result target mismatch"
    assert evidence["service"] == service, "m5-result service mismatch"
    descriptor = record["descriptor"]
    assert evidence["display_item_name"] == descriptor["displayItemName"], \
        "m5-result display item name does not match the App descriptor"
    assert evidence["vendor_name"] == descriptor["wifiAwareVendorName"], \
        "m5-result vendor name does not match the App descriptor"
    assert evidence["model_name"] == descriptor["wifiAwareModelName"], \
        "m5-result model name does not match the App descriptor"
    test_kind = evidence["test"]
    if test_kind == "stage_a_pairing":
        _validate_stage_a_fields(evidence)
    else:
        assert evidence["device_tap_count"] == 0, "XCTest tapped a device row"
        assert evidence["pin_entry_attempted"] is False, "XCTest entered a PIN"
    assert evidence["pin_handling"] == "system_picker"
    for key in ("picker_requested", "picker_presented", "picker_dismissed", "authorized",
                "target_seen_in_picker"):
        assert isinstance(evidence[key], bool), f"{key} is not a boolean"
    assert isinstance(evidence["picker_state_observed"], str)
    assert isinstance(evidence["state_history"], list)
    assert isinstance(evidence["accessory_count"], int) and evidence["accessory_count"] >= 0
    assert isinstance(evidence["authorized_count"], int) and evidence["authorized_count"] >= 0
    assert evidence["authorized"] == (evidence["authorized_count"] > 0)
    for key in ("paired_device_ids", "accessory_paired_device_ids"):
        assert isinstance(evidence[key], list)
        assert all(isinstance(value, int) and value >= 0 for value in evidence[key])
    assert isinstance(evidence["target_candidates_seen"], list)
    assert isinstance(evidence["snapshot_state"], str)
    assert isinstance(evidence["wait_seconds"], (int, float))
    assert isinstance(evidence["wait_timeout_seconds"], (int, float))
    assert evidence["wait_timeout_seconds"] > 0
    assert 0 <= evidence["wait_seconds"] <= evidence["wait_timeout_seconds"] + 1
    _epoch(evidence["observed_at_utc"])
    assert isinstance(evidence["monotonic_ns"], int)
    if test_kind != "stage_a_pairing":
        assert evidence["failure"] is None, f"run reported failure: {evidence['failure']}"

    session = record["session"]
    if evidence["test"] == "paired_device_snapshot":
        assert evidence["picker_requested"] is False
        assert evidence["picker_presented"] is False
    else:
        assert evidence["picker_requested"] is True
    if evidence["picker_presented"]:
        assert session.get("pickerPresented") is True, "UI saw the picker but the App record did not"
    if evidence["picker_dismissed"]:
        assert session.get("pickerDismissed") is True
    if evidence["authorized"]:
        assert record["outcome"] == "authorized"
        assert any(a["state"] == "authorized" for a in record["accessories"])
    paired_ids = {device["id"] for device in record["pairedDevices"]}
    accessory_ids = {a["wifiAwarePairedDeviceID"] for a in record["accessories"]}
    assert set(evidence["paired_device_ids"]) <= paired_ids, \
        "XCUITest paired-device IDs are absent from the App record"
    assert set(evidence["accessory_paired_device_ids"]) <= accessory_ids, \
        "XCUITest accessory IDs are absent from the App record"
    paired_id = evidence["wifi_aware_paired_device_id"]
    assert paired_id is None or isinstance(paired_id, int)
    if paired_id is not None:
        assert paired_id in paired_ids | accessory_ids
    if test_kind == "stage_a_pairing" and evidence["failure"] is not None:
        raise AssertionError(f"Stage A run failed: {evidence['failure_category']}")


def _validate_stage_a_fields(evidence):
    assert evidence["pin_injected"] is True, "Stage A evidence did not record PIN injection"
    assert evidence["pin_source"] == "AWARE_M5_PIN", "Stage A PIN source mismatch"
    assert evidence["pin_handling"] == "system_picker", "Stage A PIN handling mismatch"
    assert isinstance(evidence["pin_length"], int) and evidence["pin_length"] == 6, \
        "Stage A PIN length must be 6"
    for key in ("pin_entry_attempted", "target_row_found", "pin_ui_found",
                "pin_error_signal", "pairing_flow_started", "picker_search_tapped",
                "screenshot_withheld_due_to_unmasked_pin"):
        assert isinstance(evidence[key], bool), f"{key} is not a boolean"
    assert isinstance(evidence["device_tap_count"], int) and evidence["device_tap_count"] >= 0
    assert evidence["pin_submit_method"] is None or isinstance(evidence["pin_submit_method"], str)
    assert evidence["target_row_query"] is None or isinstance(evidence["target_row_query"], str)
    assert evidence["pin_error_text"] is None or isinstance(evidence["pin_error_text"], str)
    assert isinstance(evidence["system_alert_labels"], list)
    category = evidence.get("failure_category")
    assert category is None or category in STAGE_A_CATEGORIES, \
        f"unknown Stage A failure category: {category}"
    if evidence["failure"] is None:
        assert category is None, "successful Stage A evidence carries a failure category"
        assert evidence["picker_presented"] is True, "Stage A success without a presented picker"
        assert evidence["picker_search_tapped"] is True, \
            "Stage A success without starting the picker discovery"
        assert evidence["pin_entry_attempted"] is True, "Stage A did not attempt PIN entry"
        assert evidence["device_tap_count"] >= 1, "Stage A did not tap the target row"
        assert evidence["pairing_flow_started"] is True
        assert evidence["target_row_found"] is True, "Stage A target row was not found"
        assert evidence["pin_ui_found"] is True, "Stage A PIN UI was not found"
        assert evidence["pin_ui_scope"] in ("app", "springboard"), \
            "Stage A success has no PIN UI scope"
        assert evidence["pin_submit_method"], "Stage A success has no PIN submit method"
        assert evidence["authorized"] is True, "Stage A success without an authorized accessory"
        assert evidence["authorized_count"] >= 1
        assert evidence["accessory_paired_device_ids"], "Stage A has no accessory paired ID"
        assert evidence["paired_device_ids"], "Stage A has no WAPairedDevice ID"
        assert evidence["snapshot_state"] == "complete", "Stage A paired-device snapshot incomplete"
        for key in ("started", "picker_requested", "picker_presented",
                    "picker_search_tapped", "target_found", "target_tap",
                    "pin_ui_found", "pin_typed", "pin_submitted", "result"):
            assert isinstance(evidence[key + "_utc"], str) and evidence[key + "_utc"], \
                f"{key} time missing"
            assert isinstance(evidence[key + "_monotonic_ns"], int)
            assert evidence[key + "_monotonic_ns"] > 0
    else:
        assert category is not None, "failed Stage A run must record a failure category"
        if category not in STAGE_A_PRE_PIN_CATEGORIES:
            assert evidence["pin_entry_attempted"] is True, \
                "post-PIN Stage A failure must record a PIN attempt"
        if category in ("target_not_listed", "target_not_hittable"):
            assert evidence["target_row_found"] is False
        if category == "pin_ui_not_found":
            assert evidence["pin_ui_found"] is False


def validate(artifact_dir, run_id, target_name, repeat=1, service=SERVICE, pin=None):
    artifact_dir = Path(artifact_dir)
    attachment_files = _load_attachments(artifact_dir)
    expected = [f"{run_id}-{index:02d}" for index in range(1, repeat + 1)]
    errors = []
    valid = []
    runs = []
    stage_a_seen = False

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
            record = json.loads((folder / "ask-session.json").read_text())
            assert configuration["runID"] == rid, "configuration runID mismatch"
            assert configuration["experimentID"] == run_id, "configuration experimentID mismatch"
            assert configuration["mode"] == "m5", "configuration mode is not m5"
            assert configuration["targetName"] == target_name, "configuration target mismatch"
            assert configuration["service"] == service, "configuration service mismatch"
            assert configuration["directionID"] == DIRECTION, "configuration direction mismatch"
            assert all(e["runID"] == rid and e["experimentID"] == run_id for e in events), \
                "events.jsonl run/experiment ID mismatch"
            assert all(e["directionID"] == DIRECTION for e in events), \
                "events.jsonl direction mismatch"
            assert [e["sequence"] for e in events] == list(range(1, len(events) + 1)), \
                "events.jsonl sequence is not contiguous"
            assert [e["monotonicNS"] for e in events] == sorted(e["monotonicNS"] for e in events), \
                "events.jsonl is not monotonic"
            assert [e["action"] for e in events] == APP_ACTIONS, \
                f"unexpected App events: {[e['action'] for e in events]}"
            assert result["stage"] == "M5-pairing-observation", "result stage mismatch"
            assert result["wirelessVerified"] is False and result["runID"] == rid, \
                "result wirelessVerified/runID mismatch"
            assert isinstance(result["state"], str) and result["state"], "result state missing"
            assert rid in seen_ids, f"run {rid} is missing from the system log"

            record_summary = _validate_ask_record(record, rid, target_name, service)

            result_path = _attachment_path(artifact_dir, attachment_files, rid, "-m5-result")
            evidence = json.loads(result_path.read_text())
            _validate_result(evidence, record, rid, target_name, service)
            if evidence["test"] == "stage_a_pairing":
                stage_a_seen = True

            _attachment_path(artifact_dir, attachment_files, rid, "-ask-observation")
            _attachment_path(artifact_dir, attachment_files, rid, "-ask-observation-tree")
            if evidence["test"] == "accessory_picker":
                _attachment_path(artifact_dir, attachment_files, rid, "-ask-final")
                _attachment_path(artifact_dir, attachment_files, rid, "-ask-final-tree")
            elif evidence["test"] == "stage_a_pairing":
                _attachment_path(artifact_dir, attachment_files, rid, "-ask-final-tree")
                if not evidence["screenshot_withheld_due_to_unmasked_pin"]:
                    _attachment_path(artifact_dir, attachment_files, rid, "-ask-final")
                if evidence["pin_ui_found"]:
                    _attachment_path(artifact_dir, attachment_files, rid, "-ask-pin")
                    _attachment_path(artifact_dir, attachment_files, rid, "-ask-pin-tree")

            runs.append({
                "run_id": rid,
                "test": evidence["test"],
                "outcome": record_summary["outcome"],
                "failure_category": evidence.get("failure_category"),
                "target_row_found": evidence.get("target_row_found"),
                "pin_ui_found": evidence.get("pin_ui_found"),
                "pin_entry_attempted": evidence.get("pin_entry_attempted"),
                "pin_submit_method": evidence.get("pin_submit_method"),
                "device_tap_count": evidence.get("device_tap_count"),
                "picker_presented": evidence["picker_presented"],
                "picker_dismissed": evidence["picker_dismissed"],
                "target_seen_in_picker": evidence["target_seen_in_picker"],
                "authorized": evidence["authorized"],
                "accessory_count": evidence["accessory_count"],
                "accessory_paired_device_ids": evidence["accessory_paired_device_ids"],
                "paired_device_ids": evidence["paired_device_ids"],
                "paired_device_snapshot_error": record_summary["paired_device_snapshot_error"],
                "snapshot_state": evidence["snapshot_state"],
                "observed_at_utc": evidence["observed_at_utc"],
                "wait_seconds": evidence["wait_seconds"],
            })
            valid.append(rid)
        except (OSError, ValueError, KeyError, AssertionError, TypeError) as exc:
            errors.append({"run_id": rid, "error": str(exc) or "evidence assertion failed"})

    pin_leak_check = {"performed": False, "clean": None, "matchCount": 0, "files": []}
    if pin is not None:
        hits = pin_hits(artifact_dir, pin)
        pin_leak_check = {"performed": True, "clean": not hits,
                          "matchCount": len(hits), "files": hits}
        if hits:
            errors.append({"error": "PIN plaintext found in evidence", "files": hits})
    elif stage_a_seen:
        errors.append({"error": "Stage A evidence requires AWARE_M5_PIN for the PIN leak check"})

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
        "pinLeakCheck": pin_leak_check,
        "errors": errors,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifact-dir", required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--target-name", default="ROCK5B-M5")
    parser.add_argument("--repeat", type=int, choices=range(1, 11), default=1)
    parser.add_argument("--service", default=SERVICE)
    args = parser.parse_args(argv)

    artifact_dir = Path(args.artifact_dir)
    if not artifact_dir.is_dir():
        print(f"artifact directory not found: {artifact_dir}", file=sys.stderr)
        return 2
    pin = os.environ.get("AWARE_M5_PIN")
    if pin is not None and not re.fullmatch(r"[0-9]{6}", pin):
        print("AWARE_M5_PIN must be exactly 6 digits", file=sys.stderr)
        return 2
    report = validate(artifact_dir, args.run_id, args.target_name,
                      repeat=args.repeat, service=args.service, pin=pin)
    (artifact_dir / "m5-validation.json").write_text(
        json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    passed = not report["errors"] and len(report["validatedAppRuns"]) == args.repeat
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
