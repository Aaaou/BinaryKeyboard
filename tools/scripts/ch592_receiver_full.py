#!/usr/bin/env python3
"""Build and package the CH592F receiver image for first-time ISP flashing."""

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


def main() -> int:
    cmake = find_cmake()
    if not cmake:
        raise SystemExit("CMake not found")

    firmware_dir = PROJECT_ROOT / "firmware" / "CH592F"
    receiver_dir = firmware_dir / "build" / "receiver"
    subprocess.run([str(cmake), "--build", str(receiver_dir)], check=True)

    diagnostic_dir = firmware_dir / "build" / "receiver-usb-diagnostic"
    subprocess.run([
        str(cmake), "-S", str(firmware_dir), "-B", str(diagnostic_dir), "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=MinSizeRel", "-DKBD_RECEIVER_BUILD=ON",
        "-DKBD_RECEIVER_USB_DIAGNOSTIC=ON",
    ], check=True)
    subprocess.run([str(cmake), "--build", str(diagnostic_dir)], check=True)

    jumpiap_build()
    bootloader_build()

    full_hex, full_bin = package(receiver_dir, "CH592F-RECEIVER")
    diagnostic_hex, diagnostic_bin = package(
        diagnostic_dir, "CH592F-RECEIVER-USB-DIAGNOSTIC"
    )
    print(f"Receiver FULL HEX (ISP): {full_hex}")
    print(f"Receiver FULL BIN (ISP): {full_bin}")
    print(f"Receiver USB diagnostic FULL HEX (ISP): {diagnostic_hex}")
    print(f"Receiver USB diagnostic FULL BIN (ISP): {diagnostic_bin}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
