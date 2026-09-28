"""The committed data files agree with their recorded checksums."""

from __future__ import annotations

import hashlib
from pathlib import Path

DATA = Path(__file__).resolve().parents[2] / "data"


def test_equity169_matches_its_sha256_file() -> None:
    # data/equity169.sha256 is in `shasum -a 256` format: "<hex digest>  equity169.bin".
    # A regenerated equity169.bin with a stale checksum file fails here.
    digest, name = (DATA / "equity169.sha256").read_text().split()
    assert name == "equity169.bin"
    assert hashlib.sha256((DATA / "equity169.bin").read_bytes()).hexdigest() == digest
