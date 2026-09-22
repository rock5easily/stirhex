#!/usr/bin/env python3
"""Measure StirHex binary-patch resource usage at the product size limit.

This probe intentionally exercises only the standalone core CLI.  It records
wall time, peak working set, STP/STT temporary-file peak, timeout, exit status,
and byte/hash validation.  Reports and large fixtures are written below
reports/, which is ignored by the product repository.
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import random
import subprocess
import sys
import time
import zlib
from pathlib import Path
from typing import Sequence

from binary_patch_interop import bps_vli, crc32


ROOT = Path(__file__).resolve().parents[3]
DEFAULT_EXE = ROOT / "porting" / "tests" / "core" / "bin" / "x64" / "binary_patch_cli.exe"


class PROCESS_MEMORY_COUNTERS_EX(ctypes.Structure):
    _fields_ = [
        ("cb", ctypes.c_ulong),
        ("PageFaultCount", ctypes.c_ulong),
        ("PeakWorkingSetSize", ctypes.c_size_t),
        ("WorkingSetSize", ctypes.c_size_t),
        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
        ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
        ("PagefileUsage", ctypes.c_size_t),
        ("PeakPagefileUsage", ctypes.c_size_t),
        ("PrivateUsage", ctypes.c_size_t),
    ]


def process_peak_working_set(process: subprocess.Popen[str]) -> int:
    if os.name != "nt":
        return 0
    try:
        handle = ctypes.windll.kernel32.OpenProcess(0x1010, False, process.pid)
        if not handle:
            return 0
        counters = PROCESS_MEMORY_COUNTERS_EX()
        counters.cb = ctypes.sizeof(counters)
        ok = ctypes.windll.psapi.GetProcessMemoryInfo(
            handle, ctypes.byref(counters), counters.cb)
        ctypes.windll.kernel32.CloseHandle(handle)
        return int(counters.PeakWorkingSetSize) if ok else 0
    except (AttributeError, OSError):
        return 0


def temporary_bytes(directory: Path) -> int:
    total = 0
    for path in directory.iterdir():
        if path.is_file() and path.name.startswith(("STP", "STT", "STH")):
            try:
                total += path.stat().st_size
            except OSError:
                pass
    return total


def run_monitored(exe: Path, args: Sequence[os.PathLike[str] | str],
                  directory: Path, timeout_s: float) -> dict[str, object]:
    command = [str(exe), *(os.fspath(value) for value in args)]
    start = time.perf_counter()
    process = subprocess.Popen(command, cwd=directory, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True,
                               errors="replace")
    peak_memory = 0
    peak_temp = 0
    timed_out = False
    while process.poll() is None:
        peak_memory = max(peak_memory, process_peak_working_set(process))
        peak_temp = max(peak_temp, temporary_bytes(directory))
        if time.perf_counter() - start > timeout_s:
            timed_out = True
            process.kill()
            break
        time.sleep(0.2)
    stdout, stderr = process.communicate()
    elapsed_ms = (time.perf_counter() - start) * 1000.0
    peak_memory = max(peak_memory, process_peak_working_set(process))
    peak_temp = max(peak_temp, temporary_bytes(directory))
    return {
        "command": command,
        "elapsed_ms": elapsed_ms,
        "returncode": process.returncode,
        "timed_out": timed_out,
        "peak_working_set_bytes": peak_memory,
        "peak_temporary_bytes": peak_temp,
        "temporary_prefixes": ["STP", "STT", "STH"],
        "stdout": stdout.strip(),
        "stderr": stderr.strip(),
    }


def write_random(path: Path, size: int, seed: int) -> str:
    digest = hashlib.sha256()
    generator = random.Random(seed)
    with path.open("wb") as stream:
        remaining = size
        while remaining:
            block = generator.randbytes(min(1024 * 1024, remaining))
            stream.write(block)
            digest.update(block)
            remaining -= len(block)
    return digest.hexdigest()


def hash_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def prepend_file(source: Path, target: Path, prefix: bytes) -> str:
    digest = hashlib.sha256()
    with target.open("wb") as out:
        out.write(prefix)
        digest.update(prefix)
        with source.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                out.write(block)
                digest.update(block)
    return digest.hexdigest()


def scattered_copy(source: Path, target: Path, positions: set[int]) -> str:
    digest = hashlib.sha256()
    offset = 0
    with source.open("rb") as inp, target.open("wb") as out:
        for block in iter(lambda: bytearray(inp.read(1024 * 1024)), bytearray()):
            for index in range(len(block)):
                if offset + index in positions:
                    block[index] ^= 0x5A
            out.write(block)
            digest.update(block)
            offset += len(block)
    return digest.hexdigest()


def repeated_crc(pattern: bytes, size: int) -> int:
    value = 0
    block = pattern * (1024 * 1024 // len(pattern))
    remaining = size
    while remaining:
        chunk = block if remaining >= len(block) else (pattern * ((remaining + len(pattern) - 1) // len(pattern)))[:remaining]
        value = zlib.crc32(chunk, value)
        remaining -= len(chunk)
    return value & 0xFFFFFFFF


def repeated_sha256(pattern: bytes, size: int) -> str:
    digest = hashlib.sha256()
    block = pattern * (1024 * 1024 // len(pattern))
    remaining = size
    while remaining:
        chunk = block if remaining >= len(block) else (pattern * ((remaining + len(pattern) - 1) // len(pattern)))[:remaining]
        digest.update(chunk)
        remaining -= len(chunk)
    return digest.hexdigest()


def make_repeat_bps(source: bytes, target_size: int, pattern: bytes) -> bytes:
    if target_size < len(pattern):
        raise ValueError("target is smaller than repeat seed")
    body = bytearray(b"BPS1")
    body += bps_vli(len(source))
    body += bps_vli(target_size)
    body += bps_vli(0)
    body += bps_vli(((len(pattern) - 1) << 2) | 1)
    body += pattern
    repeat_length = target_size - len(pattern)
    if repeat_length:
        body += bps_vli(((repeat_length - 1) << 2) | 3)
        body += bps_vli(0)
    footer_prefix = source_crc = crc32(source)
    target_crc = repeated_crc(pattern, target_size)
    del footer_prefix
    body += source_crc.to_bytes(4, "little")
    body += target_crc.to_bytes(4, "little")
    body += crc32(body).to_bytes(4, "little")
    return bytes(body)


def probe_arch(exe: Path, arch: str, size_mib: int, shift_size_mib: int,
               timeout_s: float, root: Path) -> dict[str, object]:
    size = size_mib * 1024 * 1024
    directory = root / f"resource-{arch}-{size_mib}mib"
    directory.mkdir(parents=True, exist_ok=True)
    result: dict[str, object] = {
        "arch": arch,
        "size_mib": size_mib,
        "size_bytes": size,
        "timeout_s": timeout_s,
        "limits_under_test": {
            "bpsMaxSourceBytes": 512 * 1024 * 1024,
            "bpsMaxTargetBytes": 512 * 1024 * 1024,
            "maxTemporaryBytes": 1024 * 1024 * 1024,
        },
        "cases": {},
    }

    unrelated = directory / "unrelated"
    unrelated.mkdir(exist_ok=True)
    source = unrelated / "source.bin"
    target = unrelated / "target.bin"
    patch = unrelated / "patch.bps"
    output = unrelated / "output.bin"
    source_hash = write_random(source, size, 0x25901)
    target_hash = write_random(target, size, 0x25902)
    create = run_monitored(exe, ["generate", "bps", source, target, patch], unrelated, timeout_s)
    apply = run_monitored(exe, ["apply", source, patch, output], unrelated, timeout_s) if patch.is_file() else {
        "skipped": "patch was not created"
    }
    result["cases"]["unrelated_data"] = {
        "source_sha256": source_hash,
        "target_sha256": target_hash,
        "create": create,
        "apply": apply,
        "output_matches": output.is_file() and hash_file(output) == target_hash,
        "patch_size": patch.stat().st_size if patch.is_file() else None,
    }

    # Random, non-periodic sources expose the source-index stride behavior:
    # inserting one or seven bytes at the front should not be mistaken for a
    # periodic/repeated-data success. Keep this differential fixture at 16 MiB
    # so a 512 MiB limit probe remains bounded while still recording patch
    # sizes and byte-correctness for all three shapes.
    shift_size = min(size, shift_size_mib * 1024 * 1024)
    shifted = directory / "nonperiodic-shifts"
    shifted.mkdir(exist_ok=True)
    shift_source = shifted / "source.bin"
    write_random(shift_source, shift_size, 0x25911)
    shift_cases: dict[str, object] = {}
    for name, prefix in (("insert1", b"X"), ("insert7", b"PREFIX7")):
        target_path = shifted / f"{name}-target.bin"
        patch_path = shifted / f"{name}.bps"
        output_path = shifted / f"{name}-output.bin"
        target_hash = prepend_file(shift_source, target_path, prefix)
        create = run_monitored(exe, ["generate", "bps", shift_source, target_path, patch_path], shifted, timeout_s)
        apply = run_monitored(exe, ["apply", shift_source, patch_path, output_path], shifted, timeout_s) if patch_path.is_file() else {"skipped": "patch was not created"}
        shift_cases[name] = {
            "source_size": shift_size,
            "target_size": target_path.stat().st_size,
            "target_sha256": target_hash,
            "create": create,
            "apply": apply,
            "output_matches": output_path.is_file() and hash_file(output_path) == target_hash,
            "patch_size": patch_path.stat().st_size if patch_path.is_file() else None,
        }
    scattered_target = shifted / "scattered-target.bin"
    scattered_patch = shifted / "scattered.bps"
    scattered_output = shifted / "scattered-output.bin"
    scattered_hash = scattered_copy(
        shift_source, scattered_target,
        {0, 1, 7, shift_size // 3, shift_size // 2, shift_size - 1})
    scattered_create = run_monitored(
        exe, ["generate", "bps", shift_source, scattered_target, scattered_patch], shifted, timeout_s)
    scattered_apply = run_monitored(
        exe, ["apply", shift_source, scattered_patch, scattered_output], shifted, timeout_s) if scattered_patch.is_file() else {"skipped": "patch was not created"}
    shift_cases["scattered_changes"] = {
        "source_size": shift_size,
        "target_size": scattered_target.stat().st_size,
        "target_sha256": scattered_hash,
        "create": scattered_create,
        "apply": scattered_apply,
        "output_matches": scattered_output.is_file() and hash_file(scattered_output) == scattered_hash,
        "patch_size": scattered_patch.stat().st_size if scattered_patch.is_file() else None,
    }
    result["cases"]["nonperiodic_shift_and_scattered"] = shift_cases

    repeated = directory / "targetcopy-repeat"
    repeated.mkdir(exist_ok=True)
    repeat_source = repeated / "source.bin"
    repeat_patch = repeated / "repeat.bps"
    repeat_output = repeated / "output.bin"
    cancel_output = repeated / "cancel.bin"
    pattern = b"ABCD"
    repeat_source.write_bytes(pattern)
    repeat_patch.write_bytes(make_repeat_bps(pattern, size, pattern))
    repeat_hash = repeated_sha256(pattern, size)
    apply_repeat = run_monitored(exe, ["apply", repeat_source, repeat_patch, repeat_output], repeated, timeout_s)
    cancel = run_monitored(exe, ["apply", repeat_source, repeat_patch, cancel_output,
                                 "--cancel-after", str(min(size, 16 * 1024 * 1024))], repeated, timeout_s)
    result["cases"]["targetcopy_repeat"] = {
        "pattern": pattern.decode("ascii"),
        "target_sha256": repeat_hash,
        "patch_size": repeat_patch.stat().st_size,
        "apply": apply_repeat,
        "apply_output_matches": repeat_output.is_file() and hash_file(repeat_output) == repeat_hash,
        "cancel": cancel,
        "cancel_output_exists": cancel_output.is_file(),
    }
    return result


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", default=str(DEFAULT_EXE))
    parser.add_argument("--arch", choices=("x86", "x64"), default="x64")
    parser.add_argument("--size-mib", type=int, default=512)
    parser.add_argument("--shift-size-mib", type=int, default=16)
    parser.add_argument("--timeout-s", type=float, default=180.0)
    parser.add_argument("--work", default=str(ROOT / "reports" / "interop" / "resource-probe"))
    parser.add_argument("--output", required=True)
    args = parser.parse_args(argv)
    exe = Path(args.exe).resolve()
    if not exe.is_file():
        print(f"missing executable: {exe}", file=sys.stderr)
        return 2
    report = probe_arch(exe, args.arch, args.size_mib, args.shift_size_mib,
                        args.timeout_s, Path(args.work).resolve())
    Path(args.output).write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
