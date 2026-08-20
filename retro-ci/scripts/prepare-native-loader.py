#!/usr/bin/env python3
"""Copy and deterministically adapt the pinned BFplayer BigApp loader core.

The retained files are GPL-3.0-or-later derivatives of ps5-payload-websrv.
Only the process-launch/ELF-replacement core is copied; no web server code is
included in Retro Papa's payload.

RTPN00001 intentionally remains the single Sony BigApp host. The resident
watcher distinguishes real dashboard launches from Retro Papa's own hbldr
transitions with an explicit transition guard, avoiding any second registered
host title.
"""

from __future__ import annotations

import argparse
import shutil
from pathlib import Path

UPSTREAM_REPOSITORY = "itsblurf/bfplayer"
UPSTREAM_COMMIT = "ad3bdc9d7742606913701dac035f29e2ab49b080"
FILES = (
    "pt.c",
    "pt.h",
    "elfldr.c",
    "elfldr.h",
    "hbldr.c",
    "hbldr.h",
    "standalone_fs.h",
)


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one occurrence, found {count}")
    return text.replace(old, new, 1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path, help="BFplayer src/launcher/core directory")
    parser.add_argument("destination", type=Path)
    parser.add_argument("--commit", default=UPSTREAM_COMMIT)
    args = parser.parse_args()

    if args.commit != UPSTREAM_COMMIT:
        raise RuntimeError(
            f"loader source must be pinned to {UPSTREAM_COMMIT}, got {args.commit}"
        )

    source = args.source.resolve()
    destination = args.destination.resolve()
    destination.mkdir(parents=True, exist_ok=True)

    for name in FILES:
        path = source / name
        if not path.is_file():
            raise FileNotFoundError(path)
        shutil.copyfile(path, destination / name)

    hbldr_path = destination / "hbldr.c"
    hbldr = hbldr_path.read_text(encoding="utf-8")
    hbldr = replace_once(
        hbldr,
        "Modified for BFplayer in 2026:",
        "Modified for Retro Papa in 2026:",
        "attribution heading",
    )
    hbldr = replace_once(
        hbldr,
        '#define HOST_TITLE_ID "PSMC00001"',
        '#define HOST_TITLE_ID "RTPN00001"',
        "host title id",
    )
    hbldr = replace_once(
        hbldr,
        '\\"applicationCategoryType\\": 65536',
        '\\"applicationCategoryType\\": 0',
        "Games category",
    )
    hbldr = replace_once(
        hbldr,
        "IV9999-PSMC00001_00-BFPLAYER00000000",
        "IV9999-RTPN00001_00-RETROPAPA0000000",
        "host content id",
    )
    hbldr = replace_once(
        hbldr,
        '\\"titleName\\": \\"BFplayer\\"',
        '\\"titleName\\": \\"Retro Papa\\"',
        "host title name",
    )
    if "PSMC00001" in hbldr or "BFplayer" in hbldr:
        raise RuntimeError("upstream product identifiers remain in adapted hbldr.c")
    if '#define HOST_TITLE_ID "RTPN00001"' not in hbldr:
        raise RuntimeError("RTPN00001 host identity was not generated")
    if '\\"applicationCategoryType\\": 0' not in hbldr:
        raise RuntimeError("RTPN00001 host is not a Games-category host")
    hbldr_path.write_text(hbldr, encoding="utf-8", newline="\n")

    provenance = destination / "UPSTREAM.txt"
    provenance.write_text(
        "\n".join(
            (
                f"repository={UPSTREAM_REPOSITORY}",
                f"commit={UPSTREAM_COMMIT}",
                "origin=src/launcher/core",
                "license=GPL-3.0-or-later",
                "adaptation=RTPN00001 Games BigApp host for Retro Papa",
                "entry=real-title-no-webkit",
                "",
            )
        ),
        encoding="utf-8",
        newline="\n",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
