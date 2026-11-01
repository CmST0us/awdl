#!/usr/bin/env python3
"""PIN evidence hygiene for the M5 Stage A pairing run.

The PIN is injected as an XCTest environment variable and never belongs in an
App record, log, tree or JSON attachment. These helpers find the literal token
in text evidence, redact it before export, and check whether the xcresult bundle
contains it. Screenshots and other binary payloads are not text-scanned here.
"""
from __future__ import annotations

import re
from pathlib import Path

PIN_TEXT_SUFFIXES = {".json", ".jsonl", ".ndjson", ".log", ".txt", ".stderr"}
PIN_ATTACHMENT_MARKERS = ("-tree", "-m5-result", "-m4-result", "-gate-result", "-ui-result")


def text_evidence_paths(root):
    """Yield text-form evidence files; screenshots and xcresult bundles are skipped."""
    root = Path(root)
    attachment_names = set()
    manifest_path = root / "attachments" / "manifest.json"
    if manifest_path.exists():
        try:
            import json
            for group in json.loads(manifest_path.read_text()):
                for item in group.get("attachments", []):
                    name = item.get("suggestedHumanReadableName", "")
                    if any(marker in name for marker in PIN_ATTACHMENT_MARKERS):
                        attachment_names.add(item.get("exportedFileName", ""))
        except (OSError, ValueError):
            pass
    for path in root.rglob("*"):
        if not path.is_file() or "tests.xcresult" in path.parts:
            continue
        if path.suffix.lower() in PIN_TEXT_SUFFIXES or path.name in attachment_names:
            yield path


def _pin_pattern(pin):
    # A PIN that happens to be a substring of a longer number (for example a
    # mach_timestamp) is not PIN plaintext; only standalone tokens count.
    return re.compile(rb"(?<![0-9])" + re.escape(pin.encode()) + rb"(?![0-9])")


def pin_hits(root, pin):
    """Return relative evidence paths that contain the PIN as a standalone token."""
    root = Path(root)
    pattern = _pin_pattern(pin)
    hits = []
    for path in text_evidence_paths(root):
        try:
            if pattern.search(path.read_bytes()):
                hits.append(str(path.relative_to(root)))
        except OSError:
            continue
    return hits


def redact_pin(root, pin):
    """Replace standalone PIN tokens in text evidence; return changed file count."""
    pattern = _pin_pattern(pin)
    changed = 0
    for path in text_evidence_paths(root):
        try:
            data = path.read_bytes()
        except OSError:
            continue
        redacted = pattern.sub(b"<redacted-pin>", data)
        if redacted != data:
            path.write_bytes(redacted)
            changed += 1
    return changed


def xcresult_contains(bundle, pin):
    """Return True when the xcresult bundle contains the PIN as literal bytes."""
    bundle = Path(bundle)
    if not bundle.exists():
        return False
    token = pin.encode()
    for path in bundle.rglob("*"):
        if not path.is_file():
            continue
        try:
            if token in path.read_bytes():
                return True
        except OSError:
            continue
    return False
