#!/usr/bin/env python3

"""
1. Get all files with extension .tiff in folder "data".
2. Parse filenames using a regex, the format is "MDT-50cm-<x><y>-07-2025_v02.tiff", where <x> and <y> are 3-digit numbers.
3. Get the min and max values of x and y; for the minimum, round to the nearest decade down, for the maximum, round to the nearest decade up.
4. Create a VRT file named "lidar-<x><y>.vrt" for each decade of x and y, and include in that file all the .tiff files that fall within that decade range of x and y (plus the first row/column of the next decade), using command:
    gdalbuildvrt -resolution highest lidar-<x><y>.vrt <tiff files>
"""

from __future__ import annotations

import math
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


DATA_DIR = Path("data/MDS-50cm")
OUTPUT_PREFIX = "lidar"
FILENAME_RE = re.compile(r"^MDS-50cm-(\d{3})(\d{3})-[\d\-]{7}\.tif$")

FACTOR = 20

def decade_floor(value: int) -> int:
    return (value // FACTOR) * FACTOR

def decade_ceil(value: int) -> int:
    return ((value // FACTOR) + (1 if value % FACTOR != 0 else 0)) * FACTOR

def collect_tiles() -> list[tuple[int, int, Path]]:
    tiles: list[tuple[int, int, Path]] = []
    for tiff in sorted(DATA_DIR.glob("*.tif")):
        match = FILENAME_RE.match(tiff.name)
        if not match:
            continue
        x, y = int(match.group(1)), int(match.group(2))
        tiles.append((x, y, tiff))
    return tiles


def normalized_source_path(source: str, vrt_dir: Path) -> Path:
    path = Path(source)
    if not path.is_absolute():
        path = vrt_dir / path
    return path.resolve()


def existing_vrt_sources(vrt_path: Path) -> set[Path]:
    tree = ET.parse(vrt_path)
    root = tree.getroot()
    return {
        normalized_source_path(elem.text.strip(), vrt_path.parent)
        for elem in root.iter("SourceFilename")
        if elem.text and elem.text.strip()
    }


def main() -> int:
    if not DATA_DIR.exists():
        print(f"error: missing input folder: {DATA_DIR}", file=sys.stderr)
        return 1

    tiles = collect_tiles()
    if not tiles:
        print("error: no matching .tif files found in data/", file=sys.stderr)
        return 1

    xs = [x for x, _, _ in tiles]
    ys = [y for _, y, _ in tiles]

    x_min = decade_floor(min(xs))
    x_max = decade_ceil(max(xs))
    y_min = decade_floor(min(ys))
    y_max = decade_ceil(max(ys))
    
    print(f"X range: {x_min} to {x_max}, Y range: {y_min} to {y_max}")

    built = 0
    skipped = 0
    for x_decade in range(x_min, x_max + 1, FACTOR):
        for y_decade in range(y_min, y_max + 1, FACTOR):
            print(f"Processing decade X: {x_decade} to {x_decade + FACTOR}, Y: {y_decade} to {y_decade + FACTOR}")
            
            selected = [
                path
                for x, y, path in tiles
                if x_decade <= x <= x_decade + FACTOR and y_decade <= y <= y_decade + FACTOR
            ]

            if not selected:
                continue
            
            

            output = Path(f"{OUTPUT_PREFIX}-{x_decade:03d}{y_decade:03d}.vrt")
            selected_sources = {path.resolve() for path in selected}
            if output.exists():
                try:
                    if existing_vrt_sources(output) == selected_sources:
                        print(f"Skipping unchanged VRT: {output}")
                        skipped += 1
                        continue
                except (ET.ParseError, OSError) as exc:
                    print(f"warning: could not parse existing VRT {output}: {exc}; rebuilding")

            cmd = [
                "gdalbuildvrt",
                "-resolution",
                "highest",
                str(output),
                *[str(path) for path in selected],
            ]
            print(" ".join(cmd))
            subprocess.run(cmd, check=True)
            built += 1

            print(f"Built {built} VRT files, skipped {skipped} unchanged")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
