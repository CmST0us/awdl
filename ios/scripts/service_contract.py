"""Shared discovery name used by device diagnostics and log readers."""
from pathlib import Path
import tomllib

SERVICE = tomllib.loads(
    (Path(__file__).resolve().parents[2] / 'config/services/fileserver.toml').read_text()
)['service']
