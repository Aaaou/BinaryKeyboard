#!/usr/bin/env python3
"""Build first-flash CH592F 2.4G keyboard images for both layouts."""

from pathlib import Path
import subprocess

from common import PROJECT_ROOT, find_cmake
from targets.ch592.build import (
    _hex_to_bin,
    bootloader_build,
    bootloader_hex_path,
    jumpiap_build,
    jumpiap_hex_path,
    merge_hex,
)


def package(build_dir: Path, stem: str) -> tuple[Path, Path]:
    app_hex = build_dir / "CH592F.hex"
    stage_hex = build_dir / f".{stem}-merge-stage.hex"
    full_hex = build_dir / f"{stem}-full.hex"
    full_bin = build_dir / f"{stem}-full.bin"
    merge_hex(jumpiap_hex_path(), app_hex, stage_hex)
    merge_hex(stage_hex, bootloader_hex_path(), full_hex, fill_gaps_with=0xFF)
    stage_hex.unlink(missing_ok=True)
    _hex_to_bin(full_hex, full_bin)
    return full_hex, full_bin


def build_layout(cmake: Path, keyboard: str, directory: str, stem: str) -> tuple[Path, Path]:
    firmware_dir = PROJECT_ROOT / "firmware" / "CH592F"
    build_dir = firmware_dir / "build" / directory
    subprocess.run([
        str(cmake), "-S", str(firmware_dir), "-B", str(build_dir), "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=MinSizeRel", f"-DKEYBOARD={keyboard}",
        "-DKBD_RADIO_2G4_ENABLED=ON",
        "-DKBD_RECEIVER_BUILD=OFF",
        "-DKBD_RECEIVER_USB_DIAGNOSTIC=OFF",
    ], check=True)
    subprocess.run([str(cmake), "--build", str(build_dir)], check=True)
    return package(build_dir, stem)


def main() -> int:
    cmake = find_cmake()
    if not cmake:
        raise SystemExit("CMake not found")

    jumpiap_build()
    bootloader_build()
    five_hex, five_bin = build_layout(cmake, "5KEY", "2g4-5key", "CH592F-2G4-5KEY")
    knob_hex, knob_bin = build_layout(cmake, "KNOB", "2g4-knob", "CH592F-2G4-KNOB")
    print(f"5KEY 2.4G FULL HEX (ISP): {five_hex}")
    print(f"5KEY 2.4G FULL BIN (ISP): {five_bin}")
    print(f"KNOB 2.4G FULL HEX (ISP): {knob_hex}")
    print(f"KNOB 2.4G FULL BIN (ISP): {knob_bin}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
