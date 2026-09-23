#!/usr/bin/env python3
"""External IPS/BPS interoperability and performance harness (Issue #259).

The OSS patchers are deliberately kept outside the product tree.  This harness
creates deterministic fixtures, invokes Flips and RomPatcher.js, and optionally
invokes a StirHex binary-patch CLI supplied by the implementation agent.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence


ROOT = Path(__file__).resolve().parents[3]
NODE_DEFAULT = Path(r"C:\Program Files\nodejs\node.exe")
FLIPS_DEFAULT = ROOT / "reports" / "Flips-v198-windows" / "flips.exe"
ROMPATCHER_DEFAULT = ROOT / "reports" / "RomPatcher.js" / "index.js"
ROMPATCHER_ADAPTER_DEFAULT = ROOT / "porting" / "tests" / "core" / "rompatcher_adapter.js"


@dataclass(frozen=True)
class CommandResult:
    argv: tuple[str, ...]
    returncode: int
    stdout: str
    stderr: str
    elapsed_ms: float


@dataclass(frozen=True)
class Case:
    name: str
    source: bytes
    target: bytes
    note: str


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def bps_vli(value: int) -> bytes:
    """Encode a BPS variable-length integer (the byuu/Flips encoding)."""
    if value < 0:
        raise ValueError("BPS VLI cannot encode a negative value")
    out = bytearray()
    while True:
        x = value & 0x7F
        value >>= 7
        if value == 0:
            out.append(x | 0x80)
            return bytes(out)
        out.append(x)
        value -= 1


def make_bps(source: bytes, target: bytes, actions: Iterable[tuple[int, int, object]]) -> bytes:
    """Build a valid BPS patch from explicit (type, length, payload) actions.

    Action 0 is SourceRead, 1 is TargetRead (payload bytes), 2 is SourceCopy,
    and 3 is TargetCopy.  Copy payloads are signed relative offsets.
    """
    body = bytearray(b"BPS1")
    body += bps_vli(len(source))
    body += bps_vli(len(target))
    body += bps_vli(0)  # metadata length
    produced = bytearray()
    source_relative = 0
    target_relative = 0
    for action_type, length, payload in actions:
        if length <= 0:
            raise ValueError("BPS actions must have positive lengths")
        body += bps_vli(((length - 1) << 2) | action_type)
        if action_type == 1:
            literal = bytes(payload)  # type: ignore[arg-type]
            if len(literal) != length:
                raise ValueError("TargetRead payload length mismatch")
            body += literal
            produced += literal
        elif action_type == 0:
            start = len(produced)
            produced += source[start:start + length]
        elif action_type == 2:
            delta = int(payload)
            start = source_relative + delta
            if start < 0 or start + length > len(source):
                raise ValueError("SourceCopy outside source")
            body += bps_vli((abs(delta) << 1) | (1 if delta < 0 else 0))
            produced += source[start:start + length]
            source_relative = start + length
        elif action_type == 3:
            delta = int(payload)
            start = target_relative + delta
            # TargetCopy may overlap the bytes it is appending; only the
            # initial source position must already exist in the target.
            if start < 0 or start >= len(produced):
                raise ValueError("TargetCopy outside target prefix")
            body += bps_vli((abs(delta) << 1) | (1 if delta < 0 else 0))
            for i in range(length):
                produced.append(produced[start + i])
            target_relative = start + length
        else:
            raise ValueError(f"unknown BPS action {action_type}")
    if bytes(produced) != target:
        raise ValueError("explicit BPS actions do not produce target")
    # The patch CRC covers the complete patch before its final four bytes,
    # including the source and target CRC fields (BPS spec §Footer).
    footer_prefix = struct.pack("<II", crc32(source), crc32(target))
    body += footer_prefix
    body += struct.pack("<I", crc32(body))
    return bytes(body)


def make_ips(source: bytes, target: bytes) -> bytes:
    """Build a basic IPS patch for same-size test fixtures."""
    if len(source) != len(target):
        raise ValueError("basic IPS fixture requires equal-sized files")
    out = bytearray(b"PATCH")
    i = 0
    while i < len(source):
        if source[i] == target[i]:
            i += 1
            continue
        start = i
        while i < len(source) and source[i] != target[i] and i - start < 0xFFFF:
            i += 1
        length = i - start
        if start > 0xFFFFFF:
            raise ValueError("IPS fixture offset exceeds 24-bit range")
        out += start.to_bytes(3, "big")
        out += length.to_bytes(2, "big")
        out += target[start:i]
    out += b"EOF"
    return bytes(out)


def read_bps_vli(data: bytes, pos: int, end: int) -> tuple[int, int]:
    """Decode one BPS variable-length integer; return (value, next position)."""
    value = 0
    shift = 1
    while True:
        if pos >= end:
            raise ValueError("truncated BPS variable-length integer")
        byte = data[pos]
        pos += 1
        value += (byte & 0x7F) * shift
        if byte & 0x80:
            return value, pos
        shift <<= 7
        value += shift


def apply_bps_reference(source: bytes, patch: bytes) -> bytes:
    """Apply a BPS patch with a decoder written from the specification (Issue #265).

    It deliberately shares no code with StirHex, so an encoder/decoder pair that
    is wrong in the same way (for example the sign of relative offsets) cannot
    pass a StirHex-only round trip unnoticed.
    """
    if len(patch) < 16 or patch[:4] != b"BPS1":
        raise ValueError("not a BPS patch")
    end = len(patch) - 12
    pos = 4
    source_size, pos = read_bps_vli(patch, pos, end)
    target_size, pos = read_bps_vli(patch, pos, end)
    metadata_size, pos = read_bps_vli(patch, pos, end)
    if source_size != len(source):
        raise ValueError("BPS source size mismatch")
    if metadata_size > end - pos:
        raise ValueError("BPS metadata exceeds the patch")
    pos += metadata_size
    out = bytearray()
    source_relative = 0
    target_relative = 0
    while pos < end:
        instruction, pos = read_bps_vli(patch, pos, end)
        action = instruction & 3
        length = (instruction >> 2) + 1
        if len(out) + length > target_size:
            raise ValueError("BPS instruction exceeds the target size")
        if action == 0:
            start = len(out)
            if start + length > len(source):
                raise ValueError("BPS SourceRead outside the source")
            out += source[start:start + length]
        elif action == 1:
            if pos + length > end:
                raise ValueError("BPS TargetRead exceeds the patch")
            out += patch[pos:pos + length]
            pos += length
        else:
            encoded, pos = read_bps_vli(patch, pos, end)
            delta = -(encoded >> 1) if encoded & 1 else encoded >> 1
            if action == 2:
                source_relative += delta
                if source_relative < 0 or source_relative + length > len(source):
                    raise ValueError("BPS SourceCopy outside the source")
                out += source[source_relative:source_relative + length]
                source_relative += length
            else:
                target_relative += delta
                if target_relative < 0 or target_relative >= len(out):
                    raise ValueError("BPS TargetCopy outside the written target")
                # Byte-by-byte copying implements overlapping copies as specified.
                for _ in range(length):
                    out.append(out[target_relative])
                    target_relative += 1
    if len(out) != target_size:
        raise ValueError("BPS instructions do not produce the declared target size")
    source_crc, target_crc, patch_crc = struct.unpack("<III", patch[end:])
    if source_crc != crc32(source):
        raise ValueError("BPS source CRC mismatch")
    if target_crc != crc32(bytes(out)):
        raise ValueError("BPS target CRC mismatch")
    if patch_crc != crc32(patch[:-4]):
        raise ValueError("BPS patch CRC mismatch")
    return bytes(out)


def apply_ips_reference(source: bytes, patch: bytes) -> bytes:
    """Apply an IPS patch (normal/RLE records and the EOF size extension)."""
    if len(patch) < 8 or patch[:5] != b"PATCH":
        raise ValueError("not an IPS patch")
    out = bytearray(source)
    pos = 5
    while True:
        if pos + 3 > len(patch):
            raise ValueError("IPS patch has no EOF marker")
        offset_bytes = patch[pos:pos + 3]
        pos += 3
        if offset_bytes == b"EOF":
            rest = patch[pos:]
            if len(rest) == 3:
                size = int.from_bytes(rest, "big")
                if size < len(out):
                    del out[size:]
                else:
                    out.extend(bytes(size - len(out)))
            elif rest:
                raise ValueError("unexpected bytes after the IPS EOF marker")
            return bytes(out)
        offset = int.from_bytes(offset_bytes, "big")
        if pos + 2 > len(patch):
            raise ValueError("truncated IPS record length")
        length = int.from_bytes(patch[pos:pos + 2], "big")
        pos += 2
        if length:
            data = patch[pos:pos + length]
            if len(data) != length:
                raise ValueError("truncated IPS record data")
            pos += length
        else:
            if pos + 3 > len(patch):
                raise ValueError("truncated IPS RLE record")
            run = int.from_bytes(patch[pos:pos + 2], "big")
            if run == 0:
                raise ValueError("IPS RLE record has zero length")
            data = bytes([patch[pos + 2]]) * run
            pos += 3
        record_end = offset + len(data)
        if record_end > len(out):
            out.extend(bytes(record_end - len(out)))
        out[offset:record_end] = data


def apply_reference(source: bytes, patch: bytes) -> bytes:
    if patch[:5] == b"PATCH":
        return apply_ips_reference(source, patch)
    return apply_bps_reference(source, patch)


def reference_decode_row(source: bytes, patch: Path, expected: bytes) -> dict[str, object]:
    try:
        decoded = apply_reference(source, patch.read_bytes())
    except (OSError, ValueError) as exc:
        return {"matches_target": False, "error": str(exc)}
    return {"matches_target": decoded == expected}


def write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def run_command(argv: Sequence[os.PathLike[str] | str], cwd: Path) -> CommandResult:
    args = tuple(os.fspath(x) for x in argv)
    start = time.perf_counter()
    proc = subprocess.run(args, cwd=cwd, capture_output=True, text=True, errors="replace")
    elapsed_ms = (time.perf_counter() - start) * 1000.0
    return CommandResult(args, proc.returncode, proc.stdout, proc.stderr, elapsed_ms)


def display_output(result: CommandResult) -> str:
    return (result.stdout + ("\n" + result.stderr if result.stderr else "")).strip()


def require_file(path: Path, label: str) -> None:
    if not path.is_file():
        raise FileNotFoundError(f"{label} not found: {path}")


def make_cases() -> list[Case]:
    source = bytes((i * 37 + 11) & 0xFF for i in range(4096))
    changed = bytearray(source)
    changed[37:61] = bytes((0xD0 + i) & 0xFF for i in range(24))
    changed[2048:2080] = b"StirHex/Issue259/IPS-BPS" + bytes(8)

    repeated = (b"ABCD" * 4096) + bytes(range(64))
    shifted = repeated[:8192] + b"INSERTED-259" + repeated[8192:]

    variable_source = b"header:" + (b"0123456789" * 512) + b":tail"
    variable_target = b"header:" + (b"0123456789" * 128) + b"BPS-SHIFT" + (b"0123456789" * 640) + b":tail"

    # Non-periodic data makes every 4-byte window (almost) unique, so a
    # reordered target must be encoded with a backward (negative) SourceCopy.
    nonperiodic = random.Random(0x265).randbytes(4096)

    return [
        Case("normal_same_size", source, bytes(changed), "same-size modifications"),
        Case("empty_identical", b"", b"", "empty identical input"),
        Case("small_identical", b"same bytes\x00\xFF", b"same bytes\x00\xFF", "non-empty identical input"),
        Case("position_shift_repeated", repeated, shifted, "repeated data with insertion and shifted positions"),
        Case("variable_size", variable_source, variable_target, "target-size growth"),
        Case("shrink_tail", nonperiodic, nonperiodic[:3000], "target-size shrink"),
        Case("swap_halves", nonperiodic, nonperiodic[2048:] + nonperiodic[:2048],
             "reordered blocks (backward SourceCopy)"),
        Case("empty_to_data", b"", b"new data for Issue 265", "empty source, non-empty target"),
        Case("data_to_empty", nonperiodic[:64], b"", "non-empty source, empty target"),
        # Content repeated in the target but absent from the source: StirHex
        # encodes it with (overlapping) TargetCopy (Issue #279).
        Case("repeated_new_block", nonperiodic[:1024], nonperiodic[2048:3072] * 64,
             "block not in the source, repeated in the target (TargetCopy)"),
        Case("zero_fill_inserted", nonperiodic,
             nonperiodic[:2000] + bytes(20000) + nonperiodic[2000:],
             "zero fill inserted (overlapping TargetCopy)"),
    ]


def invoke_flips(flips: Path, mode: str, source: Path, target_or_patch: Path, output: Path, patch_format: str | None = None, exact: bool = False) -> CommandResult:
    if mode == "create":
        args: list[str] = [str(flips), "--create"]
        if patch_format == "ips":
            args += ["--ips"]
        elif patch_format == "bps":
            args += ["--bps"]
        args += [str(source), str(target_or_patch), str(output)]
    elif mode == "apply":
        args = [str(flips), "--apply"]
        if exact:
            args.append("--exact")
        args += [str(target_or_patch), str(source), str(output)]
    else:
        raise ValueError(mode)
    return run_command(args, flips.parent)


def invoke_rompatcher(node: Path, entry: Path, mode: str, source: Path, target_or_patch: Path, output: Path, patch_format: str | None = None, validate: bool = False) -> CommandResult:
    # RomPatcher.js v3.2.1's CLI chooses output next to the patch/ROM based on
    # filenames; callers place each input in a private directory and rename the
    # resulting file to the requested path after the process returns.
    if mode == "create":
        args: list[str] = [str(node), str(entry), "create", str(source), str(target_or_patch)]
        if patch_format:
            args += ["--format", patch_format]
    elif mode == "apply":
        args = [str(node), str(entry), "patch", str(source), str(target_or_patch)]
        if validate:
            args.append("--validate-checksum")
    else:
        raise ValueError(mode)
    # BinFile deliberately stores only basenames in Node mode.  Therefore the
    # CLI's save() writes into its process cwd; isolate every invocation in the
    # input directory so runs cannot overwrite one another or the checked-out
    # RomPatcher.js tree.
    return run_command(args, source.parent)


def invoke_rompatcher_adapter(node: Path, adapter: Path, rompatcher_root: Path,
                              mode: str, source: Path, target_or_patch: Path,
                              output: Path, patch_format: str | None = None,
                              validate: bool = False) -> CommandResult:
    """Call RomPatcher.js's library API through the tracked Node adapter."""
    args: list[str] = [str(node), str(adapter), "--root", str(rompatcher_root), mode]
    if mode == "apply":
        args += [str(source), str(target_or_patch), str(output)]
        if validate:
            args.append("--validate")
    elif mode == "create":
        if not patch_format:
            raise ValueError("adapter create requires patch format")
        args += [str(source), str(target_or_patch), patch_format, str(output)]
    else:
        raise ValueError(mode)
    return run_command(args, source.parent)


def copy_rompatcher_output(directory: Path, expected_stem: str,
                           destination: Path, suffixes: Iterable[str],
                           allow_original: bool = True) -> Path | None:
    candidates: list[Path] = []
    for suffix in suffixes:
        if allow_original:
            candidates.extend(directory.glob(expected_stem + suffix))
        # The v3.2.1 CLI defaults to `outputSuffix: true` and writes
        # `<rom> (patched).ext` for an apply operation.
        candidates.extend(directory.glob(expected_stem + " (patched)" + suffix))
    if not candidates:
        return None
    candidates.sort(key=lambda p: p.stat().st_mtime_ns, reverse=True)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(candidates[0], destination)
    return candidates[0]


def prepare_external_patchers(root: Path, flips: Path, node: Path,
                              rompatcher: Path, adapter: Path) -> dict[str, str]:
    flips_checkout = root / "reports" / "Flips"
    flips_source_commit = "unknown"
    flips_tag_commit = "359d414cff73b3e0400871d3c288a59b36564834"
    if (flips_checkout / ".git").exists():
        source_result = run_command(["git", "-C", flips_checkout, "rev-parse", "HEAD"], root)
        if source_result.returncode == 0:
            flips_source_commit = source_result.stdout.strip()
        tag_result = run_command(["git", "-C", flips_checkout, "rev-list", "-n", "1", "v198"], root)
        if tag_result.returncode == 0:
            flips_tag_commit = tag_result.stdout.strip()
    package_version = "unknown"
    package_json = rompatcher.parent / "package.json"
    if package_json.is_file():
        try:
            package_version = str(json.loads(package_json.read_text(encoding="utf-8")).get("version", "unknown"))
        except (OSError, ValueError):
            package_version = "unreadable"
    return {
        "flips_repository": "https://github.com/Sir-Walrus/Flips.git",
        "flips_tag": "v198",
        "flips_commit": flips_tag_commit,
        "flips_source_clone_commit": flips_source_commit,
        "flips": str(flips),
        "flips_sha256": sha256(flips),
        "flips_version": display_output(run_command([flips, "--version"], root)),
        "rompatcher_repository": "https://github.com/marcrobledo/RomPatcher.js.git",
        "rompatcher_tag": "v3.2.1",
        "rompatcher_package_json_version": package_version,
        "rompatcher_version_note": "The fixed Git tag is v3.2.1; its package.json version field is 3.0.0.",
        "rompatcher_entry": str(rompatcher),
        "rompatcher_commit": "91e522e247f709e894761157ccba3189004d0859",
        "rompatcher_adapter": str(adapter),
        "rompatcher_validation_api": "RomPatcher.applyPatch(..., {requireValidation:true, outputSuffix:false})",
        "node": str(node),
        "node_version": display_output(run_command([node, "--version"], root)),
    }


def run_core_only(args: argparse.Namespace) -> dict[str, object]:
    """Check the StirHex CLI without any external patcher (Issue #265).

    StirHex-generated patches are decoded by the independent reference
    decoders, and hand-authored BPS fixtures are applied by StirHex.
    """
    if not args.core_cli:
        raise ValueError("--core-only requires --core-cli")
    core_cli = Path(args.core_cli).resolve()
    cases_root = Path(args.work).resolve() if args.work else Path(tempfile.mkdtemp(prefix="issue265-core-"))
    if args.work and cases_root.exists() and any(cases_root.iterdir()):
        raise ValueError(f"explicit interop work directory must be empty: {cases_root}")
    cases_root.mkdir(parents=True, exist_ok=True)
    report: dict[str, object] = {"tools": {}, "cases": []}
    try:
        report["core_cli"] = run_core_cli(core_cli, None, None, None, None, cases_root)
        report["ips_extension_record_beyond_target"] = run_ips_extension_case(
            None, None, None, None, core_cli, cases_root)
    finally:
        if not args.keep_work and not args.work:
            shutil.rmtree(cases_root, ignore_errors=True)
    report["summary"] = summarize_report(report)
    return report


def run_interop(args: argparse.Namespace) -> dict[str, object]:
    if getattr(args, "core_only", False):
        return run_core_only(args)
    root = Path(args.root).resolve()
    flips = Path(args.flips).resolve()
    node = Path(args.node).resolve()
    rompatcher = Path(args.rompatcher).resolve()
    adapter = Path(args.rompatcher_adapter).resolve()
    require_file(flips, "Flips executable")
    require_file(node, "Node executable")
    require_file(rompatcher, "RomPatcher.js entry")
    require_file(adapter, "RomPatcher.js API adapter")

    report: dict[str, object] = {"tools": prepare_external_patchers(root, flips, node, rompatcher, adapter), "cases": []}
    cases_root = Path(args.work).resolve() if args.work else Path(tempfile.mkdtemp(prefix="issue259-interop-", dir=root / "reports"))
    if args.work and cases_root.exists() and any(cases_root.iterdir()):
        raise ValueError(f"explicit interop work directory must be empty: {cases_root}")
    cases_root.mkdir(parents=True, exist_ok=True)
    keep_work = bool(args.keep_work)
    try:
        for case in make_cases():
            case_dir = cases_root / case.name
            case_dir.mkdir(parents=True, exist_ok=True)
            source = case_dir / "source.bin"
            target = case_dir / "target.bin"
            write(source, case.source)
            write(target, case.target)
            case_result: dict[str, object] = {
                "name": case.name,
                "note": case.note,
                "source_size": len(case.source),
                "target_size": len(case.target),
                "tools": {},
            }

            for tool_name in ("flips", "rompatcher"):
                tool_result: dict[str, object] = {}
                for fmt in ("ips", "bps"):
                    patch = case_dir / f"{tool_name}-generated.{fmt}"
                    if fmt == "ips" and len(case.source) != len(case.target):
                        tool_result[fmt] = {"status": "not_applicable", "reason": "IPS fixture is same-size only"}
                        continue
                    if tool_name == "flips":
                        result = invoke_flips(flips, "create", source, target, patch, fmt)
                    else:
                        # RomPatcher's create command uses the modified ROM stem
                        # as output; run in a private directory and copy the
                        # produced extension back to our deterministic path.
                        rp_dir = case_dir / f"rompatcher-create-{fmt}"
                        rp_dir.mkdir(exist_ok=True)
                        rp_source = rp_dir / source.name
                        rp_target = rp_dir / target.name
                        shutil.copyfile(source, rp_source)
                        shutil.copyfile(target, rp_target)
                        result = invoke_rompatcher(node, rompatcher, "create", rp_source, rp_target, patch, fmt)
                        produced = copy_rompatcher_output(rp_dir, target.stem, patch, (f".{fmt}",))
                        if produced is None:
                            result = CommandResult(result.argv, 1, result.stdout, result.stderr + "\noutput patch not found", result.elapsed_ms)
                    tool_result[fmt] = {
                        "returncode": result.returncode,
                        "elapsed_ms": result.elapsed_ms,
                        "stdout": result.stdout.strip(),
                        "stderr": result.stderr.strip(),
                        "patch_exists": patch.is_file(),
                        "patch_size": patch.stat().st_size if patch.is_file() else None,
                    }
                    if patch.is_file():
                        applied = case_dir / f"{tool_name}-generated-{fmt}-applied.bin"
                        if tool_name == "flips":
                            apply_result = invoke_flips(flips, "apply", source, patch, applied)
                        else:
                            rp_dir = case_dir / f"rompatcher-apply-{fmt}"
                            rp_dir.mkdir(exist_ok=True)
                            rp_source = rp_dir / source.name
                            rp_patch = rp_dir / patch.name
                            shutil.copyfile(source, rp_source)
                            shutil.copyfile(patch, rp_patch)
                            apply_result = invoke_rompatcher(node, rompatcher, "apply", rp_source, rp_patch, applied, validate=True)
                            produced = copy_rompatcher_output(rp_dir, source.stem, applied, (source.suffix, ".bin"), allow_original=False)
                            if produced is None:
                                apply_result = CommandResult(apply_result.argv, 1, apply_result.stdout, apply_result.stderr + "\noutput ROM not found", apply_result.elapsed_ms)
                        tool_result[fmt]["apply"] = {
                            "returncode": apply_result.returncode,
                            "elapsed_ms": apply_result.elapsed_ms,
                            "stdout": apply_result.stdout.strip(),
                            "stderr": apply_result.stderr.strip(),
                            "output_exists": applied.is_file(),
                            "matches_target": applied.is_file() and applied.read_bytes() == case.target,
                        }
                        if tool_name == "rompatcher":
                            validated_output = case_dir / f"rompatcher-api-{fmt}-applied.bin"
                            validated_result = invoke_rompatcher_adapter(
                                node, adapter, rompatcher.parent, "apply",
                                source, patch, validated_output, validate=True)
                            tool_result[fmt]["validated_adapter"] = {
                                "returncode": validated_result.returncode,
                                "elapsed_ms": validated_result.elapsed_ms,
                                "stdout": validated_result.stdout.strip(),
                                "stderr": validated_result.stderr.strip(),
                                "output_exists": validated_output.is_file(),
                                "matches_target": validated_output.is_file() and
                                validated_output.read_bytes() == case.target,
                            }
                        # A valid BPS carries a source CRC; IPS intentionally
                        # has no source identity field.  Exercise both paths
                        # and retain warnings/errors instead of treating a
                        # process exit code alone as success (RomPatcher.js's
                        # CLI catches errors and still exits zero).
                        wrong_source = case_dir / "wrong-source.bin"
                        wrong_bytes = bytearray(case.source)
                        if wrong_bytes:
                            wrong_bytes[0] ^= 0x01
                        else:
                            wrong_bytes = bytearray(b"wrong-source")
                        write(wrong_source, bytes(wrong_bytes))
                        mismatch_output = case_dir / f"{tool_name}-generated-{fmt}-mismatch.bin"
                        if tool_name == "flips":
                            mismatch_result = invoke_flips(flips, "apply", wrong_source, patch, mismatch_output)
                        else:
                            mismatch_dir = case_dir / f"rompatcher-mismatch-{fmt}"
                            mismatch_dir.mkdir(exist_ok=True)
                            mismatch_input = mismatch_dir / wrong_source.name
                            mismatch_patch = mismatch_dir / patch.name
                            shutil.copyfile(wrong_source, mismatch_input)
                            shutil.copyfile(patch, mismatch_patch)
                            mismatch_result = invoke_rompatcher(node, rompatcher, "apply", mismatch_input, mismatch_patch, mismatch_output, validate=True)
                            produced = copy_rompatcher_output(mismatch_dir, mismatch_input.stem, mismatch_output, (".bin",), allow_original=False)
                            if produced is None:
                                mismatch_result = CommandResult(mismatch_result.argv, 1, mismatch_result.stdout, mismatch_result.stderr + "\noutput ROM not found", mismatch_result.elapsed_ms)
                        tool_result[fmt]["source_mismatch"] = {
                            "returncode": mismatch_result.returncode,
                            "elapsed_ms": mismatch_result.elapsed_ms,
                            "stdout": mismatch_result.stdout.strip(),
                            "stderr": mismatch_result.stderr.strip(),
                            "output_exists": mismatch_output.is_file(),
                            "matches_target": mismatch_output.is_file() and mismatch_output.read_bytes() == case.target,
                        }
                        if tool_name == "rompatcher":
                            validated_mismatch = case_dir / f"rompatcher-api-{fmt}-mismatch.bin"
                            validated_mismatch_result = invoke_rompatcher_adapter(
                                node, adapter, rompatcher.parent, "apply",
                                wrong_source, patch, validated_mismatch,
                                validate=True)
                            tool_result[fmt]["source_mismatch"]["validated_adapter"] = {
                                "returncode": validated_mismatch_result.returncode,
                                "elapsed_ms": validated_mismatch_result.elapsed_ms,
                                "stdout": validated_mismatch_result.stdout.strip(),
                                "stderr": validated_mismatch_result.stderr.strip(),
                                "output_exists": validated_mismatch.is_file(),
                                "matches_target": validated_mismatch.is_file() and
                                validated_mismatch.read_bytes() == case.target,
                            }
                case_result["tools"][tool_name] = tool_result  # type: ignore[index]

            # Hand-authored four-command BPS is independent of each creator's
            # heuristics and specifically exercises SourceRead/TargetRead/
            # SourceCopy/TargetCopy in every implementation.
            # This is intentionally the same compact fixture used by the
            # StirHex core test: SourceRead(AB), TargetRead(X),
            # SourceCopy(AB), and overlapping TargetCopy(AB).
            four_source = b"ABCDEFGH"
            four_target = b"ABXABABABA"
            four_patch = make_bps(
                four_source, four_target,
                [(0, 2, None), (1, 1, b"X"), (2, 2, 0), (3, 5, 3)])
            four_source_path = case_dir / "four-command-source.bin"
            four_target_path = case_dir / "four-command-target.bin"
            four_patch_path = case_dir / "four-command.bps"
            write(four_source_path, four_source)
            write(four_target_path, four_target)
            write(four_patch_path, four_patch)
            four: dict[str, object] = {"patch_sha256": hashlib.sha256(four_patch).hexdigest(), "tools": {}}
            for tool_name in ("flips", "rompatcher"):
                output = case_dir / f"four-command-{tool_name}.bin"
                if tool_name == "flips":
                    result = invoke_flips(flips, "apply", four_source_path, four_patch_path, output)
                else:
                    rp_dir = case_dir / "rompatcher-four-command"
                    rp_dir.mkdir(exist_ok=True)
                    rp_source = rp_dir / four_source_path.name
                    rp_patch = rp_dir / four_patch_path.name
                    shutil.copyfile(four_source_path, rp_source)
                    shutil.copyfile(four_patch_path, rp_patch)
                    result = invoke_rompatcher(node, rompatcher, "apply", rp_source, rp_patch, output, validate=True)
                    produced = copy_rompatcher_output(rp_dir, rp_source.stem, output, (".bin",), allow_original=False)
                    if produced is None:
                        result = CommandResult(result.argv, 1, result.stdout, result.stderr + "\noutput ROM not found", result.elapsed_ms)
                four["tools"][tool_name] = {  # type: ignore[index]
                    "returncode": result.returncode,
                    "elapsed_ms": result.elapsed_ms,
                    "stdout": result.stdout.strip(),
                    "stderr": result.stderr.strip(),
                    "matches_target": output.is_file() and output.read_bytes() == four_target,
                }
                if tool_name == "rompatcher":
                    adapter_output = case_dir / "four-command-rompatcher-api.bin"
                    adapter_result = invoke_rompatcher_adapter(
                        node, adapter, rompatcher.parent, "apply",
                        four_source_path, four_patch_path, adapter_output,
                        validate=True)
                    four["tools"][tool_name]["validated_adapter"] = {  # type: ignore[index]
                        "returncode": adapter_result.returncode,
                        "elapsed_ms": adapter_result.elapsed_ms,
                        "stdout": adapter_result.stdout.strip(),
                        "stderr": adapter_result.stderr.strip(),
                        "matches_target": adapter_output.is_file() and
                        adapter_output.read_bytes() == four_target,
                    }
            case_result["four_command_bps"] = four
            report["cases"].append(case_result)  # type: ignore[union-attr]

        if args.core_cli:
            report["core_cli"] = run_core_cli(
                Path(args.core_cli).resolve(), flips, node, rompatcher, adapter,
                cases_root)
        report["ips_extension_record_beyond_target"] = run_ips_extension_case(
            flips, node, rompatcher, adapter,
            Path(args.core_cli).resolve() if args.core_cli else None,
            cases_root)
        if args.performance:
            report["performance"] = run_performance(
                flips, node, rompatcher, adapter,
                Path(args.core_cli).resolve() if args.core_cli else None,
                cases_root, args.performance)
    finally:
        if not keep_work and not args.work:
            shutil.rmtree(cases_root, ignore_errors=True)
    report["commands"] = {
        "flips_create": "flips.exe --create --ips|--bps SOURCE TARGET PATCH",
        "flips_apply": "flips.exe --apply PATCH SOURCE OUTPUT",
        "rompatcher_cli_create": "node index.js create SOURCE TARGET --format ips|bps",
        "rompatcher_cli_apply": "node index.js patch SOURCE PATCH --validate-checksum",
        "rompatcher_api_create": "node rompatcher_adapter.js --root ROOT create SOURCE TARGET FORMAT PATCH",
        "rompatcher_api_apply": "node rompatcher_adapter.js --root ROOT apply SOURCE PATCH OUTPUT --validate",
        "stirhex_core_generate": "binary_patch_cli.exe generate ips|bps SOURCE TARGET PATCH",
        "stirhex_core_apply": "binary_patch_cli.exe apply SOURCE PATCH TARGET",
    }
    report["summary"] = summarize_report(report)
    return report


def independent_bps_fixtures() -> list[tuple[str, bytes, bytes, bytes]]:
    """Hand-authored BPS patches that do not depend on any creator's heuristics.

    four_command: SourceRead(AB), TargetRead(X), SourceCopy(AB), overlapping
    TargetCopy (the same compact fixture used by the StirHex core test).
    negative_delta: SourceCopy and TargetCopy with negative relative offsets.
    """
    four_source = b"ABCDEFGH"
    four_target = b"ABXABABABA"
    negative_source = b"ABCDEFGH"
    negative_target = b"EFCDZCDZFC"
    return [
        ("four_command", four_source, four_target,
         make_bps(four_source, four_target,
                  [(0, 2, None), (1, 1, b"X"), (2, 2, 0), (3, 5, 3)])),
        ("negative_delta", negative_source, negative_target,
         make_bps(negative_source, negative_target,
                  [(2, 2, 4), (2, 2, -4), (1, 1, b"Z"), (3, 3, 2), (3, 2, -4)])),
    ]


def run_core_cli(core_cli: Path, flips: Path | None, node: Path | None,
                 rompatcher: Path | None, adapter: Path | None,
                 cases_root: Path) -> dict[str, object]:
    """Invoke the agreed StirHex helper API when supplied.

    Contract (kept independent from core_test.cpp):
      binary_patch_cli.exe generate <ips|bps> SOURCE TARGET PATCH
      binary_patch_cli.exe apply SOURCE PATCH TARGET
    Both commands return zero on success and nonzero on a meaningful failure.

    External patchers are optional (pass None): without them the StirHex
    patches are still checked against the independent reference decoders.
    """
    require_file(core_cli, "StirHex binary-patch CLI")
    tool_names = tuple(name for name, available in (
        ("flips", flips is not None),
        ("rompatcher", node is not None and rompatcher is not None and adapter is not None))
        if available)
    result: dict[str, object] = {
        "path": str(core_cli),
        "external_tools": list(tool_names),
        "contract": {
            "generate": "binary_patch_cli.exe generate <ips|bps> SOURCE TARGET PATCH",
            "apply": "binary_patch_cli.exe apply SOURCE PATCH TARGET",
            "success": "exit code 0 and target bytes equal the fixture",
        },
        "cases": [],
        "independent_bps_fixtures": [],
    }
    for case in make_cases():
        case_dir = cases_root / f"core-{case.name}"
        case_dir.mkdir(parents=True, exist_ok=True)
        source = case_dir / "source.bin"
        target = case_dir / "target.bin"
        write(source, case.source)
        write(target, case.target)
        item: dict[str, object] = {"name": case.name, "formats": {}}
        for fmt in ("ips", "bps"):
            patch = case_dir / f"core.{fmt}"
            if fmt == "ips" and len(case.source) != len(case.target):
                item["formats"][fmt] = {"status": "not_applicable"}  # type: ignore[index]
                continue
            generated = run_command([core_cli, "generate", fmt, source, target, patch], core_cli.parent)
            applied = case_dir / f"applied-{fmt}.bin"
            apply_result = run_command([core_cli, "apply", source, patch, applied], core_cli.parent) if patch.is_file() else None
            core_round_trip: dict[str, object] = {
                "generate_returncode": generated.returncode,
                "generate_stdout": generated.stdout.strip(),
                "generate_stderr": generated.stderr.strip(),
                "apply_returncode": apply_result.returncode if apply_result else None,
                "apply_stdout": apply_result.stdout.strip() if apply_result else None,
                "apply_stderr": apply_result.stderr.strip() if apply_result else None,
                "matches_target": applied.is_file() and applied.read_bytes() == case.target,
            }
            # Direction 0: the StirHex patch decoded by the independent
            # reference decoder, which needs no external tool.
            core_round_trip["reference_decoder"] = (
                reference_decode_row(case.source, patch, case.target) if patch.is_file()
                else {"matches_target": False, "error": "patch not generated"})
            # BPS carries the source CRC: a modified source must be rejected
            # without producing an output file.
            if fmt == "bps" and patch.is_file():
                wrong_source = case_dir / "wrong-source.bin"
                wrong_bytes = bytearray(case.source)
                if wrong_bytes:
                    wrong_bytes[0] ^= 0x01
                else:
                    wrong_bytes = bytearray(b"wrong-source")
                write(wrong_source, bytes(wrong_bytes))
                mismatch_output = case_dir / "core-bps-mismatch.bin"
                mismatch = run_command([core_cli, "apply", wrong_source, patch, mismatch_output],
                                       core_cli.parent)
                core_round_trip["source_mismatch"] = {
                    "returncode": mismatch.returncode,
                    "stdout": mismatch.stdout.strip(),
                    "stderr": mismatch.stderr.strip(),
                    "output_exists": mismatch_output.is_file(),
                }
            # Direction 1: StirHex-created patch consumed by each external
            # patcher.  This catches format details which a StirHex self-round
            # trip cannot reveal.
            if patch.is_file():
                for tool_name in tool_names:
                    external_output = case_dir / f"core-generated-{tool_name}-{fmt}.bin"
                    if tool_name == "flips":
                        external_result = invoke_flips(flips, "apply", source, patch, external_output)  # type: ignore[arg-type]
                    else:
                        rp_dir = case_dir / f"core-generated-rompatcher-{fmt}"
                        rp_dir.mkdir(exist_ok=True)
                        rp_source = rp_dir / source.name
                        rp_patch = rp_dir / patch.name
                        shutil.copyfile(source, rp_source)
                        shutil.copyfile(patch, rp_patch)
                        external_result = invoke_rompatcher(
                            node, rompatcher, "apply", rp_source, rp_patch,  # type: ignore[arg-type]
                            external_output, fmt, validate=True)
                        produced = copy_rompatcher_output(
                            rp_dir, rp_source.stem, external_output, (".bin",), allow_original=False)
                        if produced is None:
                            external_result = CommandResult(
                                external_result.argv, 1, external_result.stdout,
                                external_result.stderr + "\noutput ROM not found",
                                external_result.elapsed_ms)
                    core_round_trip[f"external_{tool_name}"] = {
                        "returncode": external_result.returncode if external_result else None,
                        "stdout": external_result.stdout.strip() if external_result else None,
                        "stderr": external_result.stderr.strip() if external_result else None,
                        "matches_target": external_output.is_file() and external_output.read_bytes() == case.target,
                    }
                    if tool_name == "rompatcher":
                        adapter_output = case_dir / f"core-generated-rompatcher-api-{fmt}.bin"
                        adapter_result = invoke_rompatcher_adapter(
                            node, adapter, rompatcher.parent, "apply",  # type: ignore[arg-type,union-attr]
                            source, patch, adapter_output, validate=True)
                        core_round_trip["external_rompatcher_validated_adapter"] = {
                            "returncode": adapter_result.returncode,
                            "elapsed_ms": adapter_result.elapsed_ms,
                            "stdout": adapter_result.stdout.strip(),
                            "stderr": adapter_result.stderr.strip(),
                            "matches_target": adapter_output.is_file() and
                            adapter_output.read_bytes() == case.target,
                        }
            item["formats"][fmt] = core_round_trip  # type: ignore[index]
            # Direction 2: external creator -> StirHex applier.  Keep this
            # separate from the self-round-trip above so a shared encoder and
            # decoder bug cannot cancel out.
            for tool_name in tool_names:
                external_patch = case_dir / f"external-{tool_name}.{fmt}"
                if tool_name == "flips":
                    external_create = invoke_flips(
                        flips, "create", source, target, external_patch, fmt)  # type: ignore[arg-type]
                else:
                    rp_dir = case_dir / f"external-create-{tool_name}-{fmt}"
                    rp_dir.mkdir(exist_ok=True)
                    rp_source = rp_dir / source.name
                    rp_target = rp_dir / target.name
                    shutil.copyfile(source, rp_source)
                    shutil.copyfile(target, rp_target)
                    external_create = invoke_rompatcher(
                        node, rompatcher, "create", rp_source, rp_target,  # type: ignore[arg-type]
                        external_patch, fmt)
                    produced = copy_rompatcher_output(
                        rp_dir, rp_target.stem, external_patch, (f".{fmt}",))
                    if produced is None:
                        external_create = CommandResult(
                            external_create.argv, 1, external_create.stdout,
                            external_create.stderr + "\noutput patch not found",
                            external_create.elapsed_ms)
                external_apply_output = case_dir / f"external-{tool_name}-{fmt}-core.bin"
                # The reference decoder is the arbiter for external patches:
                # StirHex must accept what it accepts and reject what it rejects.
                external_reference = (
                    reference_decode_row(case.source, external_patch, case.target)
                    if external_patch.is_file() else None)
                if external_patch.is_file():
                    core_apply_external = run_command(
                        [core_cli, "apply", source, external_patch,
                         external_apply_output], core_cli.parent)
                else:
                    core_apply_external = None
                core_round_trip[f"external_generated_{tool_name}"] = {
                    "create_returncode": external_create.returncode,
                    "create_stdout": external_create.stdout.strip(),
                    "create_stderr": external_create.stderr.strip(),
                    "patch_exists": external_patch.is_file(),
                    "apply_returncode": core_apply_external.returncode if core_apply_external else None,
                    "apply_stdout": core_apply_external.stdout.strip() if core_apply_external else None,
                    "apply_stderr": core_apply_external.stderr.strip() if core_apply_external else None,
                    "output_exists": external_apply_output.is_file(),
                    "matches_target": external_apply_output.is_file() and
                    external_apply_output.read_bytes() == case.target,
                    "reference_decoder": external_reference,
                }
        result["cases"].append(item)  # type: ignore[union-attr]
    # Hand-authored BPS fixtures consumed by StirHex.  They are built here so
    # this path does not depend on whichever creator happens to be installed.
    fixtures_dir = cases_root / "core-independent-bps"
    fixtures_dir.mkdir(parents=True, exist_ok=True)
    for name, fixture_source, fixture_target, fixture_patch in independent_bps_fixtures():
        source_path = fixtures_dir / f"{name}-source.bin"
        patch_path = fixtures_dir / f"{name}.bps"
        output_path = fixtures_dir / f"{name}-applied.bin"
        write(source_path, fixture_source)
        write(patch_path, fixture_patch)
        applied = run_command([core_cli, "apply", source_path, patch_path, output_path], core_cli.parent)
        result["independent_bps_fixtures"].append({  # type: ignore[union-attr]
            "name": name,
            "returncode": applied.returncode,
            "stdout": applied.stdout.strip(),
            "stderr": applied.stderr.strip(),
            "matches_target": output_path.is_file() and output_path.read_bytes() == fixture_target,
        })
    return result


def run_ips_extension_case(flips: Path | None, node: Path | None,
                           rompatcher: Path | None, adapter: Path | None,
                           core_cli: Path | None,
                           cases_root: Path) -> dict[str, object]:
    """Compare EOF-after-3-byte IPS target-size semantics across tools.

    The record deliberately writes offset 10..13 while the optional target
    size after EOF is 4.  This is an interoperability boundary: tools may
    reject the patch, honor the record extent, or truncate the final output.
    The harness records each behavior without converting a divergence into a
    false pass.
    """
    directory = cases_root / "ips-extension-record-beyond-target"
    directory.mkdir(parents=True, exist_ok=True)
    source_data = b"0123456789abcdefghijkl"
    patch_data = bytearray(b"PATCH")
    patch_data += (10).to_bytes(3, "big")
    patch_data += (4).to_bytes(2, "big")
    patch_data += b"WXYZ"
    patch_data += b"EOF"
    patch_data += (4).to_bytes(3, "big")
    source = directory / "source.bin"
    patch = directory / "offset10-len4-target4.ips"
    write(source, source_data)
    write(patch, bytes(patch_data))
    result: dict[str, object] = {
        "source_size": len(source_data),
        "patch_size": len(patch_data),
        "patch_sha256": hashlib.sha256(bytes(patch_data)).hexdigest(),
        "record": {"offset": 10, "length": 4, "eof_target_size": 4},
        "tools": {},
    }

    def classify(output: Path) -> dict[str, object]:
        if not output.is_file():
            return {"output_exists": False, "behavior": "no_output"}
        data = output.read_bytes()
        if len(data) == 4:
            behavior = "truncate_to_eof_target_size"
        elif len(data) == 14:
            behavior = "honor_record_extent"
        elif len(data) == len(source_data):
            behavior = "preserve_source_size"
        else:
            behavior = "other_output_size"
        return {
            "output_exists": True,
            "output_size": len(data),
            "output_sha256": hashlib.sha256(data).hexdigest(),
            "behavior": behavior,
        }

    if flips is not None:
        flips_output = directory / "flips.bin"
        flips_result = invoke_flips(flips, "apply", source, patch, flips_output)
        flips_row = {
            "returncode": flips_result.returncode,
            "elapsed_ms": flips_result.elapsed_ms,
            "stdout": flips_result.stdout.strip(),
            "stderr": flips_result.stderr.strip(),
        }
        flips_row.update(classify(flips_output))
        result["tools"]["flips"] = flips_row  # type: ignore[index]

    if node is not None and rompatcher is not None and adapter is not None:
        rp_dir = directory / "rompatcher"
        rp_dir.mkdir(exist_ok=True)
        rp_source = rp_dir / source.name
        rp_patch = rp_dir / patch.name
        rom_output = directory / "rompatcher.bin"
        shutil.copyfile(source, rp_source)
        shutil.copyfile(patch, rp_patch)
        rom_result = invoke_rompatcher(node, rompatcher, "apply", rp_source, rp_patch, rom_output)
        produced = copy_rompatcher_output(rp_dir, rp_source.stem, rom_output, (".bin",), allow_original=False)
        if produced is None:
            rom_result = CommandResult(rom_result.argv, 1, rom_result.stdout,
                                        rom_result.stderr + "\noutput ROM not found",
                                        rom_result.elapsed_ms)
        rom_row = {
            "returncode": rom_result.returncode,
            "elapsed_ms": rom_result.elapsed_ms,
            "stdout": rom_result.stdout.strip(),
            "stderr": rom_result.stderr.strip(),
        }
        rom_row.update(classify(rom_output))
        result["tools"]["rompatcher"] = rom_row  # type: ignore[index]

        adapter_output = directory / "rompatcher-api.bin"
        adapter_result = invoke_rompatcher_adapter(
            node, adapter, rompatcher.parent, "apply", source, patch,
            adapter_output, validate=True)
        adapter_row = {
            "returncode": adapter_result.returncode,
            "elapsed_ms": adapter_result.elapsed_ms,
            "stdout": adapter_result.stdout.strip(),
            "stderr": adapter_result.stderr.strip(),
        }
        adapter_row.update(classify(adapter_output))
        result["tools"]["rompatcher_validated_adapter"] = adapter_row  # type: ignore[index]

    if core_cli is not None:
        core_output = directory / "stirhex-core.bin"
        core_result = run_command([core_cli, "apply", source, patch, core_output], core_cli.parent)
        core_row = {
            "returncode": core_result.returncode,
            "elapsed_ms": core_result.elapsed_ms,
            "stdout": core_result.stdout.strip(),
            "stderr": core_result.stderr.strip(),
        }
        core_row.update(classify(core_output))
        result["tools"]["stirhex_core"] = core_row  # type: ignore[index]
    return result


def run_performance(flips: Path, node: Path, rompatcher: Path,
                    adapter: Path, core_cli: Path | None, cases_root: Path,
                    size_mib: int) -> list[dict[str, object]]:
    """Measure creator/apply wall time for deterministic 32/64/128 MiB data."""
    sizes = [size_mib]
    rows: list[dict[str, object]] = []
    for mib in sizes:
        size = mib * 1024 * 1024
        source_data = bytes((i * 17 + 3) & 0xFF for i in range(size))
        target_data = bytearray(source_data)
        for start in (123, size // 3, size - 8192):
            target_data[start:start + 4096] = bytes((0xA5 + i) & 0xFF for i in range(4096))
        perf_dir = cases_root / f"performance-{mib}mib"
        perf_dir.mkdir(parents=True, exist_ok=True)
        source = perf_dir / "source.bin"
        target = perf_dir / "target.bin"
        write(source, source_data)
        write(target, bytes(target_data))
        row: dict[str, object] = {"size_mib": mib, "source_size": size, "tools": {}}
        for tool_name in ("flips", "rompatcher"):
            patch = perf_dir / f"{tool_name}.bps"
            if tool_name == "flips":
                create = invoke_flips(flips, "create", source, target, patch, "bps")
            else:
                rp_dir = perf_dir / "rompatcher-create"
                rp_dir.mkdir(exist_ok=True)
                rp_source = rp_dir / source.name
                rp_target = rp_dir / target.name
                shutil.copyfile(source, rp_source)
                shutil.copyfile(target, rp_target)
                create = invoke_rompatcher(node, rompatcher, "create", rp_source, rp_target, patch, "bps")
                produced = copy_rompatcher_output(rp_dir, target.stem, patch, (".bps",))
                if produced is None:
                    create = CommandResult(create.argv, 1, create.stdout, create.stderr + "\noutput patch not found", create.elapsed_ms)
            applied = perf_dir / f"{tool_name}-applied.bin"
            if tool_name == "flips":
                apply = invoke_flips(flips, "apply", source, patch, applied)
            else:
                rp_dir = perf_dir / "rompatcher-apply"
                rp_dir.mkdir(exist_ok=True)
                rp_source = rp_dir / source.name
                rp_patch = rp_dir / patch.name
                shutil.copyfile(source, rp_source)
                shutil.copyfile(patch, rp_patch)
                apply = invoke_rompatcher(node, rompatcher, "apply", rp_source, rp_patch, applied, validate=True)
                produced = copy_rompatcher_output(rp_dir, rp_source.stem, applied, (".bin",), allow_original=False)
                if produced is None:
                    apply = CommandResult(apply.argv, 1, apply.stdout, apply.stderr + "\noutput ROM not found", apply.elapsed_ms)
            row["tools"][tool_name] = {  # type: ignore[index]
                "create_ms": create.elapsed_ms,
                "create_rc": create.returncode,
                "patch_size": patch.stat().st_size if patch.is_file() else None,
                "apply_ms": apply.elapsed_ms,
                "apply_rc": apply.returncode,
                "matches_target": applied.is_file() and applied.read_bytes() == bytes(target_data),
            }
            if tool_name == "rompatcher":
                adapter_applied = perf_dir / "rompatcher-api-applied.bin"
                adapter_apply = invoke_rompatcher_adapter(
                    node, adapter, rompatcher.parent, "apply", source, patch,
                    adapter_applied, validate=True)
                row["tools"]["rompatcher_validated_adapter"] = {  # type: ignore[index]
                    "apply_ms": adapter_apply.elapsed_ms,
                    "apply_rc": adapter_apply.returncode,
                    "apply_stdout": adapter_apply.stdout.strip(),
                    "apply_stderr": adapter_apply.stderr.strip(),
                    "matches_target": adapter_applied.is_file() and
                    adapter_applied.read_bytes() == bytes(target_data),
                }
        if core_cli is not None:
            patch = perf_dir / "core.bps"
            applied = perf_dir / "core-applied.bin"
            create = run_command(
                [core_cli, "generate", "bps", source, target, patch], core_cli.parent)
            apply = run_command(
                [core_cli, "apply", source, patch, applied], core_cli.parent) if patch.is_file() else None
            row["tools"]["stirhex_core"] = {  # type: ignore[index]
                "create_ms": create.elapsed_ms,
                "create_rc": create.returncode,
                "create_stdout": create.stdout.strip(),
                "create_stderr": create.stderr.strip(),
                "patch_size": patch.stat().st_size if patch.is_file() else None,
                "apply_ms": apply.elapsed_ms if apply else None,
                "apply_rc": apply.returncode if apply else None,
                "apply_stdout": apply.stdout.strip() if apply else None,
                "apply_stderr": apply.stderr.strip() if apply else None,
                "matches_target": applied.is_file() and applied.read_bytes() == bytes(target_data),
            }
        rows.append(row)
    return rows


def summarize_report(report: dict[str, object]) -> dict[str, object]:
    """Add a compact, machine-readable count/table beside raw command rows."""
    cases = report.get("cases", [])
    summary: dict[str, object] = {
        "fixture_case_count": len(cases) if isinstance(cases, list) else 0,
        "formats": {},
        "four_command_bps": {"case_count": 0, "flips_pass": 0, "rompatcher_cli_pass": 0,
                              "rompatcher_api_pass": 0},
        "ips_extension": {},
    }
    if isinstance(cases, list):
        for case in cases:
            if not isinstance(case, dict):
                continue
            tools = case.get("tools", {})
            if isinstance(tools, dict):
                for tool_name in ("flips", "rompatcher"):
                    tool = tools.get(tool_name, {})
                    if not isinstance(tool, dict):
                        continue
                    for fmt in ("ips", "bps"):
                        row = tool.get(fmt)
                        if not isinstance(row, dict) or row.get("status") == "not_applicable":
                            continue
                        key = f"{tool_name}_{fmt}"
                        formats = summary["formats"]  # type: ignore[assignment]
                        bucket = formats.setdefault(key, {"attempted": 0, "create_pass": 0,
                                                           "apply_pass": 0, "validated_api_pass": 0,
                                                           "source_mismatch_rejected": 0})
                        bucket["attempted"] += 1
                        if row.get("returncode") == 0 and row.get("patch_exists"):
                            bucket["create_pass"] += 1
                        apply = row.get("apply")
                        if isinstance(apply, dict) and apply.get("returncode") == 0 and apply.get("matches_target"):
                            bucket["apply_pass"] += 1
                        validated = row.get("validated_adapter")
                        if isinstance(validated, dict) and validated.get("returncode") == 0 and validated.get("matches_target"):
                            bucket["validated_api_pass"] += 1
                        mismatch = row.get("source_mismatch")
                        if fmt == "bps" and isinstance(mismatch, dict):
                            if tool_name == "rompatcher":
                                # The CLI historically returns success for a
                                # checksum failure; use the API adapter's
                                # explicit validation result as the verdict.
                                validation = mismatch.get("validated_adapter")
                                rejected = (isinstance(validation, dict) and
                                            validation.get("returncode") != 0 and
                                            not validation.get("output_exists"))
                            else:
                                rejected = (mismatch.get("returncode") != 0 and
                                            not mismatch.get("output_exists"))
                            if rejected:
                                bucket["source_mismatch_rejected"] += 1
            four = case.get("four_command_bps")
            if isinstance(four, dict):
                summary["four_command_bps"]["case_count"] += 1  # type: ignore[index]
                four_tools = four.get("tools", {})
                if isinstance(four_tools, dict):
                    if four_tools.get("flips", {}).get("matches_target"):
                        summary["four_command_bps"]["flips_pass"] += 1  # type: ignore[index]
                    rp = four_tools.get("rompatcher", {})
                    if isinstance(rp, dict):
                        if rp.get("matches_target"):
                            summary["four_command_bps"]["rompatcher_cli_pass"] += 1  # type: ignore[index]
                        if rp.get("validated_adapter", {}).get("matches_target"):
                            summary["four_command_bps"]["rompatcher_api_pass"] += 1  # type: ignore[index]
    extension = report.get("ips_extension_record_beyond_target")
    if isinstance(extension, dict):
        ext_tools = extension.get("tools", {})
        if isinstance(ext_tools, dict):
            for name, row in ext_tools.items():
                if isinstance(row, dict):
                    summary["ips_extension"][name] = {
                        "returncode": row.get("returncode"),
                        "behavior": row.get("behavior"),
                        "output_size": row.get("output_size"),
                    }
    return summary


# StirHex applies an EOF-after-3-byte IPS size as a final truncate/extend
# (Flips/RomPatcher.js compatibility; see ApplyIps in BinaryPatch.cpp).
STIRHEX_IPS_EXTENSION_BEHAVIOR = "truncate_to_eof_target_size"


def external_self_round_trip(report: dict[str, object], case_name: object,
                             tool_name: str, fmt: str, applier: str) -> bool | None:
    """Whether an external tool reproduces the target from its own patch.

    applier is "apply" (the tool's CLI) or "validated_adapter".  None means the
    report has no such row (the tool was not run for this case).
    """
    for case in report.get("cases", []) or []:
        if not isinstance(case, dict) or case.get("name") != case_name:
            continue
        row = case.get("tools", {}).get(tool_name, {}).get(fmt)
        if not isinstance(row, dict) or row.get("status") == "not_applicable":
            return None
        applied = row.get(applier)
        return isinstance(applied, dict) and bool(applied.get("matches_target"))
    return None


def evaluate_report(report: dict[str, object],
                    limitations: list[str] | None = None) -> list[str]:
    """Return human-readable failures of the StirHex side of a report (Issue #265).

    Only StirHex behavior is judged.  Rows of external tools consuming their
    own patches are informational and never fail the report.  When an external
    tool cannot handle a case at all (it also fails its own round trip, e.g.
    RomPatcher.js v3.2.1 on an empty source), the StirHex->tool row is recorded
    in ``limitations`` instead of failing.  Patches created by external tools
    are judged against the reference decoder, so a creator bug (Flips v198
    writes an invalid BPS for an empty target) requires StirHex to reject it.
    """
    if limitations is None:
        limitations = []
    failures: list[str] = []
    core = report.get("core_cli")
    if not isinstance(core, dict):
        return ["core_cli: the StirHex binary-patch CLI was not exercised"]
    cases = core.get("cases")
    if not isinstance(cases, list) or not cases:
        failures.append("core_cli: no fixture case was exercised")
        cases = []
    for case in cases:
        name = case.get("name")
        for fmt, row in case.get("formats", {}).items():
            if row.get("status") == "not_applicable":
                continue
            label = f"{name}/{fmt}"
            if row.get("generate_returncode") != 0:
                failures.append(f"{label}: StirHex generation failed: {row.get('generate_stdout')} {row.get('generate_stderr')}".rstrip())
            if row.get("apply_returncode") != 0 or not row.get("matches_target"):
                failures.append(f"{label}: StirHex did not reproduce the target from its own patch")
            reference = row.get("reference_decoder", {})
            if not reference.get("matches_target"):
                failures.append(f"{label}: the reference decoder did not reproduce the target "
                                f"from the StirHex patch ({reference.get('error', 'bytes differ')})")
            mismatch = row.get("source_mismatch")
            if isinstance(mismatch, dict) and (mismatch.get("returncode") == 0 or mismatch.get("output_exists")):
                failures.append(f"{label}: StirHex accepted a BPS patch for a modified source")
            for key, tool_name, applier in (
                    ("external_flips", "flips", "apply"),
                    ("external_rompatcher", "rompatcher", "apply"),
                    ("external_rompatcher_validated_adapter", "rompatcher", "validated_adapter")):
                external = row.get(key)
                if not isinstance(external, dict) or external.get("matches_target"):
                    continue
                if external_self_round_trip(report, name, tool_name, fmt, applier) is False:
                    limitations.append(f"{label}: {key} also fails its own round trip "
                                       f"({external.get('stdout') or external.get('stderr')})")
                else:
                    failures.append(f"{label}: {key} did not reproduce the target from the StirHex patch")
            for tool_name in ("flips", "rompatcher"):
                generated = row.get(f"external_generated_{tool_name}")
                if not isinstance(generated, dict):
                    continue
                if not generated.get("patch_exists"):
                    limitations.append(f"{label}: {tool_name} did not create a patch "
                                       f"({generated.get('create_stdout')} {generated.get('create_stderr')})".rstrip())
                    continue
                reference = generated.get("reference_decoder") or {}
                if reference.get("matches_target"):
                    if generated.get("apply_returncode") != 0 or not generated.get("matches_target"):
                        failures.append(f"{label}: StirHex did not reproduce the target from the {tool_name} patch")
                else:
                    if generated.get("apply_returncode") == 0 or generated.get("output_exists"):
                        failures.append(f"{label}: StirHex accepted a {tool_name} patch that the reference "
                                        f"decoder rejects ({reference.get('error', 'bytes differ')})")
                    else:
                        limitations.append(f"{label}: {tool_name} created an invalid patch, rejected by both "
                                           f"StirHex and the reference decoder ({reference.get('error', 'bytes differ')})")
    fixtures = core.get("independent_bps_fixtures")
    if not isinstance(fixtures, list) or not fixtures:
        failures.append("core_cli: no independently encoded BPS fixture was applied")
        fixtures = []
    for fixture in fixtures:
        if fixture.get("returncode") != 0 or not fixture.get("matches_target"):
            failures.append(f"independent BPS {fixture.get('name')}: StirHex did not reproduce the target")
    extension = report.get("ips_extension_record_beyond_target")
    core_extension = extension.get("tools", {}).get("stirhex_core") if isinstance(extension, dict) else None
    if not isinstance(core_extension, dict):
        failures.append("IPS extension case: StirHex was not exercised")
    elif (core_extension.get("returncode") != 0 or
          core_extension.get("behavior") != STIRHEX_IPS_EXTENSION_BEHAVIOR):
        failures.append(f"IPS extension case: StirHex behavior is {core_extension.get('behavior')}, "
                        f"expected {STIRHEX_IPS_EXTENSION_BEHAVIOR}")
    return failures


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=str(ROOT), help="worktree root")
    parser.add_argument("--flips", default=str(FLIPS_DEFAULT), help="Flips v198 executable")
    parser.add_argument("--node", default=str(NODE_DEFAULT), help="Node executable")
    parser.add_argument("--rompatcher", default=str(ROMPATCHER_DEFAULT), help="RomPatcher.js index.js")
    parser.add_argument("--rompatcher-adapter", default=str(ROMPATCHER_ADAPTER_DEFAULT), help="tracked RomPatcher.js API adapter")
    parser.add_argument("--core-cli", help="optional StirHex binary_patch_cli.exe")
    parser.add_argument("--performance", type=int, metavar="MIB", help="measure one deterministic size (e.g. 32 or 128)")
    parser.add_argument("--work", help="keep/use an explicit work directory")
    parser.add_argument("--keep-work", action="store_true", help="retain generated fixtures")
    parser.add_argument("--output", help="JSON report path; default stdout")
    parser.add_argument("--core-only", action="store_true",
                        help="check only the StirHex CLI (with --core-cli); no external patcher is needed")
    parser.add_argument("--strict", action="store_true",
                        help="exit 1 when the StirHex side of the report fails (requires --core-cli)")
    args = parser.parse_args(argv)
    try:
        report = run_interop(args)
    except (FileNotFoundError, OSError, ValueError) as exc:
        print(f"interop harness error: {exc}", file=sys.stderr)
        return 2
    limitations: list[str] = []
    failures = evaluate_report(report, limitations)
    report["failures"] = failures
    report["external_limitations"] = limitations
    encoded = json.dumps(report, indent=2, sort_keys=True)
    if args.output:
        Path(args.output).write_text(encoded + "\n", encoding="utf-8")
    else:
        print(encoded)
    if args.strict and failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
