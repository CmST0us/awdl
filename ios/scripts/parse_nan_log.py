#!/usr/bin/env python3
"""Summarize iPad Wi-Fi Aware (NAN) and AccessorySetupKit system log records.

Input is NDJSON from either `pymobiledevice3 syslog live --format json` or
`log show --style ndjson`. Wording that has never been observed on this device
is classified heuristically and labelled as such; unclassified NAN lines are
counted and sampled instead of being interpreted. M5 AccessorySetupKit keywords
are summarized separately so they cannot change the M4 discovery counters.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from datetime import datetime, timezone
from pathlib import Path

SCHEMA = "awarelab.nan-log-summary"
SCHEMA_VERSION = 1
PARSER_VERSION = "1.1.0"
from service_contract import SERVICE as DEFAULT_SERVICE, TARGET_NAME
DEFAULT_TARGET = TARGET_NAME
WINDOW_TOLERANCE_SECONDS = 2.0
MAX_MESSAGE_CHARS = 500

# (id, kind, regex, confidence, source). Order matters: first match wins.
PATTERN_SPECS = [
    ("subscribe-created", "subscribe_created", r"Created NANSubscribe\b",
     "observed", "IOS-M1-001 G1 system.ndjson"),
    ("subscribe-started", "subscribe_started", r"\bNANSubscribe\b[^\n]*\bwas started\b",
     "observed", "IOS-M1-001 G1 system.ndjson"),
    ("subscribe-timeout", "subscribe_timeout", r"\bNANSubscribe\b[^\n]*\btimeout set to\b",
     "observed", "IOS-M1-001 G1 system.ndjson"),
    ("subscribe-stopping", "subscribe_stopping", r"\bNANSubscribe\b[^\n]*\bwill attempt to stop\b",
     "observed", "IOS-M1-001 G1 system.ndjson"),
    ("subscribe-terminated", "subscribe_terminated", r"\bNANSubscribe\b[^\n]*\bwas terminated\b",
     "observed", "IOS-M1-001 G1 system.ndjson"),
    ("subscribe-destroyed", "subscribe_destroyed", r"\bDestroyed NANSubscribe\b",
     "observed", "IOS-M1-001 G1 system.ndjson"),
    ("subscribe-requested", "subscribe_requested", r"\brequesting a subscribe with service name\b",
     "observed", "IOS-M1-001 G1 system.ndjson"),
    ("subscribe-bundle", "subscribe_bundle_bound", r"\bupdated bundleID to: \S+ and pid to \d+\b",
     "observed", "IOS-M1-001 G1 system.ndjson"),
    ("subscribe-pik", "subscribe_pik", r"\bNANSubscribe\b[^\n]*\[PIK\]",
     "observed", "IOS-M1-001 G1 system.ndjson"),
    ("publish-created", "publish_created", r"Created NANPublish\b",
     "heuristic", "symmetric with NANSubscribe; not observed in G1"),
    ("publish-started", "publish_started", r"\bNANPublish\b[^\n]*\bwas started\b",
     "heuristic", "symmetric with NANSubscribe; not observed in G1"),
    ("publish-stopping", "publish_stopping", r"\bNANPublish\b[^\n]*\bwill attempt to stop\b",
     "heuristic", "symmetric with NANSubscribe; not observed in G1"),
    ("publish-terminated", "publish_terminated", r"\bNANPublish\b[^\n]*\bwas terminated\b",
     "heuristic", "symmetric with NANSubscribe; not observed in G1"),
    ("publish-destroyed", "publish_destroyed", r"\bDestroyed NANPublish\b",
     "heuristic", "symmetric with NANSubscribe; not observed in G1"),
    ("engine-update", "engine_update", r"\[WiFiAwareEngine Event\] Update\(subscribedServices:",
     "observed", "IOS-M1-001 G1 system.ndjson"),
    ("pairing-identity-absent", "pairing_identity_absent", r"No NAN identity key stored in the keychain",
     "observed", "IOS-M1-001 G1 system.ndjson"),
    ("pairing-attribute-failed", "pairing_attribute_update_failed",
     r"Failed to update nan\d+ pairing custom attributes",
     "observed", "IOS-M1-001 G1 system.ndjson"),
    ("nan-init", "nan_init", r"\bNAN Init\b",
     "observed", "IOS-M1-001 G1 system.ndjson"),
    ("nan-channel-sequence", "nan_channel_sequence", r"\bNAN Channel Sequence:|\bNAN_CH_SEQ\b",
     "observed", "IOS-M1-001 G1 system.ndjson"),
    ("nan-state", "nan_state", r"\bupdate\(nanState:\)",
     "observed", "IOS-M1-001 G1 system.ndjson"),
    ("discovery-received", "discovery_result", r"\bReceived a new discovery result\b",
     "observed", "IOS-M1-001 G1 system.ndjson (companion-link service)"),
    ("discovery-aged-out", "discovery_aged_out", r"\bAging out discovery result\b",
     "observed", "IOS-M1-001 G1 system.ndjson (companion-link service)"),
    ("discovery-dropped", "discovery_dropped",
     r"(?i)\bdropping (?:the )?(?:event|publish|service|result)\b"
     r"|\bdrop(?:ped)?\b[^\n]*\b(?:discovery|publish|service)\b",
     "heuristic", "inverse of the historical wifip2pd drop line; not observed in G1"),
    ("discovery-result", "discovery_result",
     r"(?i)\b(?:discovery result|discovered (?:publish|service|device|peer|target)"
     r"|found (?:publish|service|device|peer|target)|received (?:publish|sdf|service|publisher)"
     r"|matched (?:publish|service)|publish(?:ed)? service (?:found|received))\b",
     "heuristic", "generic wording; NAN-specific discovery wording still unconfirmed"),
]
PATTERNS = [
    (pid, kind, re.compile(rx), confidence, source)
    for pid, kind, rx, confidence, source in PATTERN_SPECS
]
# AccessorySetupKit wording has not been observed on this iPad yet. These
# patterns only count and sample; they never feed the M4 discovery counters.
ASK_PATTERN_SPECS = [
    ("ask-pin", "ask_pin",
     r"(?i)\bpairing pin\b|\benter (?:the )?pin\b|\bpin (?:code|entry|prompt)\b"
     r"|\bpin is displayed\b",
     "heuristic", "no AccessorySetupKit system-log wording observed on this iPad yet"),
    ("ask-picker", "ask_picker",
     r"\bASPickerDisplayItem\b|\bASDiscoveryDescriptor\b|\baccessory picker\b",
     "heuristic", "no AccessorySetupKit system-log wording observed on this iPad yet"),
    ("ask-pairing", "ask_pairing",
     r"(?i)\bwi-?fi aware pairing\b|\bWAPairedDevice\b|wifiAwarePairedDeviceID"
     r"|paired device (?:id|identifier)",
     "heuristic", "no AccessorySetupKit system-log wording observed on this iPad yet"),
    ("ask-session", "ask_session",
     r"\bASAccessorySession\b|\bAccessorySetupKit\b",
     "heuristic", "no AccessorySetupKit system-log wording observed on this iPad yet"),
]
ASK_PATTERNS = [
    (pid, kind, re.compile(rx), confidence, source)
    for pid, kind, rx, confidence, source in ASK_PATTERN_SPECS
]
SESSION_REF_RE = re.compile(r"\b(NANSubscribe|NANPublish) \[(\d+)\] '([^']+)'")
TERMINATED_RE = re.compile(r"was terminated with reason (.+?)\s*$")
DISCOVERY_SERVICE_RE = re.compile(r"\[([^\]]+)\]\s+at\s+(-?\d+)\s+dBm")
DISCOVERY_PEER_RE = re.compile(r"discovery result ([0-9a-fA-F:]{17})")
ACTION_RE = re.compile(r"action=(\S+) state=(\S+)")
NAN_TOKEN_RE = re.compile(
    r"NAN(?:Subscribe|Publish|Attribute|Init|_CH_SEQ)|com\.apple\.p2p\.nan|NAN Channel Sequence"
)
NAN_SUBSYSTEM_PREFIXES = ("com.apple.nan", "com.apple.wifip2pd")
PIN_DIGITS_RE = re.compile(r"\b\d{4,8}\b")


def _expand_inputs(values):
    paths = []
    for value in values:
        path = Path(value)
        if path.is_dir():
            candidate = path / "system.ndjson"
            paths.extend([candidate] if candidate.is_file() else sorted(path.glob("*.ndjson")))
        else:
            paths.append(path)
    return paths


def _epoch(value):
    if not value:
        return None
    normalized = value.replace("Z", "+00:00")
    # `log show --style ndjson` uses +0800; older datetime.fromisoformat wants +08:00.
    normalized = re.sub(r"([+-]\d{2})(\d{2})$", r"\1:\2", normalized)
    try:
        parsed = datetime.fromisoformat(normalized)
    except ValueError:
        return None
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=datetime.now().astimezone().tzinfo)
    return parsed.timestamp()


def _normalize(record):
    label = record.get("label") or {}
    image = record.get("filename") or record.get("processImagePath") or record.get("image_name") or ""
    message = record.get("message") or record.get("eventMessage") or ""
    if not isinstance(message, str):
        return None
    process = record.get("process") or Path(str(image)).name
    return {
        "timestamp": record.get("timestamp") or "",
        "epoch": _epoch(record.get("timestamp") or ""),
        "message": message,
        "subsystem": label.get("subsystem") or record.get("subsystem") or "",
        "category": label.get("category") or record.get("category") or "",
        "process": process or "",
        "image": str(image),
        "level": record.get("level") or record.get("messageType") or "",
    }


def _is_nan(record):
    if record["subsystem"].startswith(NAN_SUBSYSTEM_PREFIXES):
        return True
    if record["process"] == "wifip2pd" or "wifip2pd" in record["image"]:
        return True
    return bool(NAN_TOKEN_RE.search(record["message"]))


def _classify(message):
    for pattern_id, kind, regex, confidence, source in PATTERNS:
        if regex.search(message):
            return pattern_id, kind, confidence, source
    return None


def _classify_ask(message):
    for pattern_id, kind, regex, confidence, source in ASK_PATTERNS:
        if regex.search(message):
            return pattern_id, kind, confidence, source
    return None


def _redact_pin_digits(message):
    return PIN_DIGITS_RE.sub("<redacted>", message)


def _truncate(message):
    return message if len(message) <= MAX_MESSAGE_CHARS else message[:MAX_MESSAGE_CHARS] + "…"


def _service_names(message):
    names = re.findall(r"'([^']+)'", message)
    for candidate in re.findall(r"\[([^\]]+)\]", message):
        if candidate.isdigit() or candidate in names:
            continue
        names.append(candidate)
    return names


def _inside_window(epoch, window, tolerance):
    if epoch is None or window is None:
        return None
    start = window.get("startEpoch")
    end = window.get("endEpoch")
    if start is None:
        return None
    if end is None:
        end = start
    return start - tolerance <= epoch <= end + tolerance


def summarize(paths, run_ids=(), target_name=DEFAULT_TARGET, service=DEFAULT_SERVICE,
              max_samples=10):
    inputs = []
    totals = {
        "lines": 0, "jsonLines": 0, "parseErrors": 0, "nanRelated": 0,
        "classified": 0, "unclassifiedNan": 0,
        "byKind": {}, "byProcess": {}, "bySubsystem": {},
    }
    parse_errors = []
    sessions = []
    active = {}
    results = []
    aged_out = []
    dropped = []
    other_samples = []
    kernel_samples = []
    engine_updates = 0
    bundle_ids = set()
    nan_services = set()
    ask_total = 0
    ask_counts = {}
    ask_samples = []
    run_timelines = {rid: {"runID": rid, "observed": False, "lines": 0, "actions": [],
                           "firstSeen": None, "lastSeen": None} for rid in run_ids}
    target_observed = False

    def touch_session(record, kind, ref, created):
        family = "subscribe" if ref.group(1) == "NANSubscribe" else "publish"
        instance = int(ref.group(2))
        session_service = ref.group(3)
        key = (family, instance)
        if created and key in active:
            sessions.append(active.pop(key))
        session = active.get(key)
        if session is None:
            session = {
                "family": family, "instance": instance, "service": session_service,
                "createdObserved": created, "firstSeen": record["timestamp"],
                "lastSeen": record["timestamp"], "eventCounts": {},
                "terminatedReason": None, "destroyed": False,
                "startedAt": None, "startedEpoch": None,
            }
            active[key] = session
        session["lastSeen"] = record["timestamp"]
        session["eventCounts"][kind] = session["eventCounts"].get(kind, 0) + 1
        if kind in ("subscribe_started", "publish_started"):
            if session["startedAt"] is None:
                session["startedAt"] = record["timestamp"]
                session["startedEpoch"] = record["epoch"]
        if kind.endswith("_terminated"):
            match = TERMINATED_RE.search(record["message"])
            if match:
                session["terminatedReason"] = match.group(1)
        if kind.endswith("_destroyed"):
            session["destroyed"] = True
        return session

    for path in paths:
        stats = {"path": str(path), "bytes": 0, "lines": 0, "jsonLines": 0, "parseErrors": 0,
                 "nanLines": 0}
        try:
            stats["bytes"] = path.stat().st_size
        except OSError:
            stats["error"] = "unreadable"
        try:
            handle = path.open("r", encoding="utf-8", errors="replace")
        except OSError as exc:
            stats["error"] = str(exc)
            inputs.append(stats)
            continue
        with handle:
            for line_number, line in enumerate(handle, 1):
                stats["lines"] += 1
                totals["lines"] += 1
                line = line.strip()
                if not line:
                    continue
                try:
                    record = json.loads(line)
                except json.JSONDecodeError:
                    stats["parseErrors"] += 1
                    totals["parseErrors"] += 1
                    if len(parse_errors) < 20:
                        parse_errors.append({"path": str(path), "line": line_number})
                    continue
                if not isinstance(record, dict):
                    stats["parseErrors"] += 1
                    totals["parseErrors"] += 1
                    continue
                stats["jsonLines"] += 1
                totals["jsonLines"] += 1
                normalized = _normalize(record)
                if normalized is None or not normalized["message"]:
                    continue
                message = normalized["message"]

                for run_id, timeline in run_timelines.items():
                    if run_id not in message:
                        continue
                    timeline["observed"] = True
                    timeline["lines"] += 1
                    timeline["firstSeen"] = timeline["firstSeen"] or normalized["timestamp"]
                    timeline["lastSeen"] = normalized["timestamp"]
                    action = ACTION_RE.search(message)
                    if action:
                        entry = {"action": action.group(1), "state": action.group(2),
                                 "timestamp": normalized["timestamp"],
                                 "epoch": normalized["epoch"]}
                        if entry not in timeline["actions"]:
                            timeline["actions"].append(entry)

                if target_name and target_name in message:
                    target_observed = True

                ask_classified = _classify_ask(message)
                if ask_classified is not None:
                    _, ask_kind, ask_confidence, _ = ask_classified
                    ask_total += 1
                    ask_counts[ask_kind] = ask_counts.get(ask_kind, 0) + 1
                    if len(ask_samples) < max_samples:
                        redacted = ask_kind == "ask_pin"
                        ask_samples.append({
                            "timestamp": normalized["timestamp"],
                            "process": normalized["process"],
                            "subsystem": normalized["subsystem"],
                            "kind": ask_kind,
                            "confidence": ask_confidence,
                            "redacted": redacted,
                            "message": _truncate(_redact_pin_digits(message) if redacted else message),
                        })

                if not _is_nan(normalized):
                    continue
                stats["nanLines"] += 1
                totals["nanRelated"] += 1
                totals["byProcess"][normalized["process"]] = \
                    totals["byProcess"].get(normalized["process"], 0) + 1
                if normalized["subsystem"]:
                    totals["bySubsystem"][normalized["subsystem"]] = \
                        totals["bySubsystem"].get(normalized["subsystem"], 0) + 1

                classified = _classify(message)
                if classified is None:
                    totals["unclassifiedNan"] += 1
                    sample = {"timestamp": normalized["timestamp"],
                              "process": normalized["process"],
                              "subsystem": normalized["subsystem"],
                              "message": _truncate(message)}
                    bucket = kernel_samples if normalized["process"] == "kernel" else other_samples
                    if len(bucket) < max_samples:
                        bucket.append(sample)
                    continue
                pattern_id, kind, confidence, source = classified
                totals["classified"] += 1
                totals["byKind"][kind] = totals["byKind"].get(kind, 0) + 1

                if kind == "engine_update":
                    engine_updates += 1
                if kind == "subscribe_bundle_bound":
                    bundle_match = re.search(r"bundleID to: (\S+) and pid to (\d+)", message)
                    if bundle_match:
                        bundle_ids.add(bundle_match.group(1))

                created = kind in ("subscribe_created", "publish_created")
                ref = SESSION_REF_RE.search(message)
                if ref:
                    session = touch_session(normalized, kind, ref, created)
                    nan_services.add(session["service"])
                elif kind in ("subscribe_requested",):
                    requested = re.search(r"service name '([^']+)'", message)
                    if requested:
                        nan_services.add(requested.group(1))
                elif kind.startswith("subscribe_") or kind.startswith("publish_"):
                    # Lifecycle wording without an instance reference still counts by kind.
                    pass

                if kind == "discovery_result":
                    bracket = DISCOVERY_SERVICE_RE.search(message)
                    service_names = _service_names(message)
                    peer = DISCOVERY_PEER_RE.search(message)
                    service_expected = bool(service) and (
                        service in message or any(service in name for name in service_names))
                    results.append({
                        "timestamp": normalized["timestamp"],
                        "process": normalized["process"],
                        "category": normalized["category"],
                        "confidence": confidence,
                        "serviceNames": service_names,
                        "expectedService": service_expected,
                        "rssiDbm": int(bracket.group(2)) if bracket else None,
                        "peer": peer.group(1) if peer else None,
                        "targetNameMatched": bool(target_name and target_name in message),
                        "message": _truncate(message),
                    })
                elif kind == "discovery_aged_out":
                    service_names = _service_names(message)
                    peer = DISCOVERY_PEER_RE.search(message)
                    aged_out.append({
                        "timestamp": normalized["timestamp"],
                        "process": normalized["process"],
                        "serviceNames": service_names,
                        "expectedService": bool(service) and (
                            service in message or any(service in name for name in service_names)),
                        "peer": peer.group(1) if peer else None,
                        "message": _truncate(message),
                    })
                elif kind == "discovery_dropped":
                    reason = re.search(r"\breason\b\s*[:=]?\s*([^,;]+)", message)
                    dropped.append({
                        "timestamp": normalized["timestamp"],
                        "process": normalized["process"],
                        "category": normalized["category"],
                        "confidence": confidence,
                        "reason": reason.group(1).strip() if reason else None,
                        "message": _truncate(message),
                    })
        inputs.append(stats)

    sessions.extend(active.values())

    def subscribe_sessions():
        return [s for s in sessions if s["family"] == "subscribe"]

    def publish_sessions():
        return [s for s in sessions if s["family"] == "publish"]

    started = [s for s in subscribe_sessions() if s["startedAt"]]
    started.sort(key=lambda s: (s["startedEpoch"] is None, s["startedEpoch"] or 0))
    first_started = started[0] if started else None
    terminated = [s for s in subscribe_sessions() if s["terminatedReason"]]
    publish_started = [s for s in publish_sessions() if s["startedAt"]]

    timelines = []
    for run_id in run_ids:
        timeline = run_timelines[run_id]
        requested = [a for a in timeline["actions"] if a["action"] == "discovery_requested"]
        closed = [a for a in timeline["actions"] if a["action"] == "discovery_container_closed"]
        window = None
        end_inferred = False
        if requested:
            start = requested[0]
            end = closed[-1] if closed and closed[-1]["epoch"] is not None else None
            end_epoch = end["epoch"] if end else None
            if end_epoch is None:
                end_inferred = True
                end_epoch = _epoch(timeline["lastSeen"])
            window = {"start": start["timestamp"], "startEpoch": start["epoch"],
                      "end": end["timestamp"] if end else timeline["lastSeen"],
                      "endEpoch": end_epoch, "endInferred": end_inferred}
        inside = _inside_window(first_started["startedEpoch"] if first_started else None,
                                window, WINDOW_TOLERANCE_SECONDS)
        timelines.append({
            "runID": run_id,
            "observed": timeline["observed"],
            "lineCount": timeline["lines"],
            "actions": [{k: a[k] for k in ("action", "state", "timestamp")}
                        for a in timeline["actions"]],
            "firstSeen": timeline["firstSeen"],
            "lastSeen": timeline["lastSeen"],
            "pickerWindow": ({k: window[k] for k in
                              ("start", "end", "endInferred")} if window else None),
            "subscribeStartedInsidePickerWindow": inside,
            "toleranceSeconds": WINDOW_TOLERANCE_SECONDS,
        })

    inside_any = None
    window_checks = [t["subscribeStartedInsidePickerWindow"] for t in timelines
                     if t["subscribeStartedInsidePickerWindow"] is not None]
    if window_checks:
        inside_any = any(window_checks)

    publish_services = sorted({s["service"] for s in publish_sessions()})
    expected_results = [r for r in results if r["expectedService"]]
    summary = {
        "schema": SCHEMA,
        "schemaVersion": SCHEMA_VERSION,
        "parserVersion": PARSER_VERSION,
        "generatedAt": datetime.now(timezone.utc).isoformat(),
        "inputs": inputs,
        "filters": {"runIDs": list(run_ids), "targetName": target_name, "service": service},
        "totals": totals,
        "services": sorted(nan_services),
        "sessions": sessions,
        "discovery": {
            "received": results,
            "agedOut": aged_out,
            "dropped": dropped,
            "unclassifiedNanSamples": (other_samples + kernel_samples)[:max_samples],
            "parseErrorSamples": parse_errors,
        },
        "askObservations": {
            "lineCount": ask_total,
            "byKind": ask_counts,
            "pinLineCount": ask_counts.get("ask_pin", 0),
            "samples": ask_samples[:max_samples],
            "confidence": "heuristic",
            "notes": [
                "AccessorySetupKit wording has not been observed on this iPad; counts are heuristic.",
                "PIN-like 4-8 digit groups are redacted from sampled PIN lines.",
                "Absence of ASK lines does not prove the system pairing flow did not run.",
            ],
        },
        "runTimelines": timelines,
        "p2Observations": {
            "subscribeCreated": any(s["createdObserved"] for s in subscribe_sessions()),
            "subscribeStartedCount": sum(s["eventCounts"].get("subscribe_started", 0)
                                         for s in subscribe_sessions()),
            "subscribeStartedAt": first_started["startedAt"] if first_started else None,
            "subscribeTerminatedReasons": sorted({s["terminatedReason"] for s in terminated}),
            "subscribeServices": sorted({s["service"] for s in subscribe_sessions()}),
            "engineUpdateCount": engine_updates,
            "bundleIDs": sorted(bundle_ids),
            "subscribeStartedInsidePickerWindow": inside_any,
            "expectedServiceDiscoveryCount": len(expected_results),
            "otherServiceDiscoveryCount": len(results) - len(expected_results),
        },
        "p3Observations": {
            "publishStartedCount": sum(s["eventCounts"].get("publish_started", 0)
                                       for s in publish_sessions()),
            "publishServices": publish_services,
            "expectedServicePublishObserved": bool(publish_started) and service in publish_services,
            "discoveryReceivedCount": len(results),
            "expectedServiceDiscoveryReceived": bool(expected_results),
            "expectedServiceDiscoveryCount": len(expected_results),
            "discoveryDroppedCount": len(dropped),
            "targetNameObservedInLog": target_observed if target_name else None,
            "targetName": target_name,
            "subscribeStartedInsidePickerWindow": inside_any,
        },
        "interpretation": {
            "confidence": {
                "observed": "wording seen in IOS-M1-001 G1 logs on this iPad",
                "heuristic": "wording not yet observed; must be confirmed by the first P3 log",
            },
            "notes": [
                "G1 saw wifip2pd discovery results for _companion-link._tcp.local, not for the M4 service.",
                "Expected-service discovery must match the service name, not a positive result count.",
                "NAN transport for that discovery-result wording is unconfirmed until P3.",
                "Absence of a publish or discovery line does not prove nothing was sent on air.",
                "Driver lines under com.apple.p2p.nan0 are counted, not treated as discovery results.",
                "Publish fields describe iPad-side sessions only; the board publisher is not in this log.",
                "Window checks use the App run_id lifecycle lines and a ±2 s tolerance.",
                "M5 AccessorySetupKit keywords are counted separately and do not change M4 discovery counters.",
                "ASK PIN samples redact 4-8 digit groups; the App itself never logs a PIN.",
            ],
        },
        "patterns": [
            {"id": pid, "kind": kind, "regex": regex.pattern,
             "confidence": confidence, "source": source}
            for pid, kind, regex, confidence, source in PATTERNS
        ],
        "askPatterns": [
            {"id": pid, "kind": kind, "regex": regex.pattern,
             "confidence": confidence, "source": source}
            for pid, kind, regex, confidence, source in ASK_PATTERNS
        ],
    }
    return summary


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", nargs="+", required=True,
                        help="NDJSON file or directory containing system.ndjson")
    parser.add_argument("--output", help="write summary JSON here instead of stdout")
    parser.add_argument("--run-id", action="append", default=[],
                        help="App run_id whose lifecycle lines bound the picker window; repeatable")
    parser.add_argument("--target-name", default=DEFAULT_TARGET)
    parser.add_argument("--service", default=DEFAULT_SERVICE)
    parser.add_argument("--max-samples", type=int, default=10)
    args = parser.parse_args(argv)

    paths = _expand_inputs(args.input)
    missing = [str(path) for path in paths if not path.is_file()]
    if missing:
        print("input not found: " + ", ".join(missing), file=sys.stderr)
        return 2
    if not paths:
        print("no NDJSON input found", file=sys.stderr)
        return 2

    summary = summarize(paths, run_ids=args.run_id, target_name=args.target_name,
                        service=args.service, max_samples=args.max_samples)
    payload = json.dumps(summary, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        Path(args.output).write_text(payload, encoding="utf-8")
    else:
        sys.stdout.write(payload)
    if summary["totals"]["jsonLines"] == 0:
        print("no JSON records parsed", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
