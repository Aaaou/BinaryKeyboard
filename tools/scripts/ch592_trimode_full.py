#!/usr/bin/env python3
"""Build and package CH592F USB/BLE/2.4G trimode keyboard images."""
from pathlib import Path
import argparse
import shutil
import subprocess
from common import PROJECT_ROOT, find_cmake
from targets.ch592.build import _hex_to_bin, bootloader_build, bootloader_hex_path, jumpiap_build, jumpiap_hex_path, merge_hex

FIRMWARE = PROJECT_ROOT / "firmware" / "CH592F"
PARTITION_SIZE = {"2G4": 64 * 1024, "BLE": 184 * 1024}

def build_image(cmake: Path, keyboard: str, image: str) -> Path:
    build_dir = FIRMWARE / "build" / f"trimode-{keyboard.lower()}-{image.lower()}"
    subprocess.run([str(cmake), "-S", str(FIRMWARE), "-B", str(build_dir), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=MinSizeRel", f"-DKEYBOARD={keyboard}", f"-DKBD_TRIMODE_IMAGE={image}", "-DKBD_RECEIVER_BUILD=OFF"], check=True)
    subprocess.run([str(cmake), "--build", str(build_dir)], check=True)
    data = (build_dir / "CH592F.bin").read_bytes()
    if len(data) > PARTITION_SIZE[image]:
        raise SystemExit(f"{keyboard} {image} exceeds partition")
    return build_dir

def package(cmake: Path, keyboard: str) -> tuple[Path, Path]:
    rf, ble = build_image(cmake, keyboard, "2G4"), build_image(cmake, keyboard, "BLE")
    out = FIRMWARE / "build" / f"trimode-{keyboard.lower()}"
    out.mkdir(parents=True, exist_ok=True)
    stem = f"CH592F-2G4-{keyboard}-TRIMODE"
    shutil.copy2(rf / "CH592F.bin", out / f"{stem}-radio-app.bin")
    shutil.copy2(rf / "CH592F.hex", out / f"{stem}-radio-app.hex")
    shutil.copy2(ble / "CH592F.bin", out / f"{stem}-ble-app.bin")
    shutil.copy2(ble / "CH592F.hex", out / f"{stem}-ble-app.hex")
    stage1, stage2 = out / ".trimode-jump-rf.hex", out / ".trimode-apps.hex"
    full_hex, full_bin = out / f"{stem}-full.hex", out / f"{stem}-full.bin"
    merge_hex(jumpiap_hex_path(), rf / "CH592F.hex", stage1)
    merge_hex(stage1, ble / "CH592F.hex", stage2)
    merge_hex(stage2, FIRMWARE / "bootloader" / "build-trimode" / "CH592F_IAP.hex", full_hex, fill_gaps_with=0xFF)
    stage1.unlink(missing_ok=True); stage2.unlink(missing_ok=True)
    _hex_to_bin(full_hex, full_bin)
    return full_hex, full_bin

def main() -> int:
    parser = argparse.ArgumentParser(); parser.add_argument("--keyboard", choices=("5KEY", "KNOB")); args = parser.parse_args()
    cmake = find_cmake()
    if not cmake: raise SystemExit("CMake not found")
    jumpiap_build()
    boot_dir = FIRMWARE / "bootloader" / "build-trimode"
    subprocess.run([str(cmake), "-S", str(FIRMWARE / "bootloader"), "-B", str(boot_dir), "-G", "Ninja", "-DKBD_TRIMODE_DISPATCHER=ON"], check=True)
    subprocess.run([str(cmake), "--build", str(boot_dir)], check=True)
    for keyboard in ((args.keyboard,) if args.keyboard else ("5KEY", "KNOB")):
        full_hex, full_bin = package(cmake, keyboard)
        print(f"{keyboard} TRIMODE FULL HEX: {full_hex}"); print(f"{keyboard} TRIMODE FULL BIN: {full_bin}")
    return 0

if __name__ == "__main__": raise SystemExit(main())
