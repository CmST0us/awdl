"""Shared discovery name used by device diagnostics and log readers."""
from pathlib import Path
import os
import tomllib

CONFIG = Path(__file__).resolve().parents[2] / 'config'
SERVICE = tomllib.loads((CONFIG / 'fileserver-discovery.toml').read_text())['service']
IDENTITY = tomllib.loads((CONFIG / 'wifiawared.toml').read_text())['identity']
TARGET_NAME = os.environ.get('AWARE_TARGET_NAME', IDENTITY['pairing_name'])
