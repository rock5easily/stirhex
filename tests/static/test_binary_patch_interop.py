"""Issue #265: gate IPS/BPS patches on an independent decoder and external patchers.

binary_patch_interop.py used to write a JSON report only, so nothing failed when a
StirHex patch stopped being readable by other tools.  These checks build the standalone
binary_patch_cli adapter (not StirHex.exe) and fail when

- a StirHex-generated patch does not decode with the independent reference decoders,
- StirHex does not apply independently encoded BPS patches (negative deltas included),
- or, when Flips v198 and RomPatcher.js v3.2.1 are installed, either direction of
  external interoperability breaks.  Without them that check is skipped; point
  STIRHEX_FLIPS / STIRHEX_ROMPATCHER at flips.exe / RomPatcher.js's index.js to run it.
"""
from argparse import Namespace
import os
from pathlib import Path
import subprocess
import sys

import pytest

from conftest import WORKSPACE_ROOT

CORE_DIR = WORKSPACE_ROOT / "porting" / "tests" / "core"
sys.path.insert(0, str(CORE_DIR))
import binary_patch_interop as interop  # noqa: E402


@pytest.fixture(scope="module", params=["x64", "x86"])
def core_cli(request, tmp_path_factory):
    if os.name != "nt":
        pytest.skip("Requires the Windows C++ toolchain")
    out_dir = tmp_path_factory.mktemp("binary_patch_cli_" + request.param)
    build = subprocess.run(
        ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
         str(CORE_DIR / "build_binary_patch_interop.ps1"),
         "-Arch", request.param, "-OutDir", str(out_dir)],
        capture_output=True, text=True, errors="replace")
    exe = out_dir / "binary_patch_cli.exe"
    assert build.returncode == 0 and exe.is_file(), build.stdout + build.stderr
    return exe


def run_harness(tmp_path, core_cli, **overrides):
    args = Namespace(
        root=str(WORKSPACE_ROOT), flips=str(interop.FLIPS_DEFAULT),
        node=str(interop.NODE_DEFAULT), rompatcher=str(interop.ROMPATCHER_DEFAULT),
        rompatcher_adapter=str(interop.ROMPATCHER_ADAPTER_DEFAULT),
        core_cli=str(core_cli), performance=None, work=str(tmp_path / "work"),
        keep_work=False, output=None, core_only=False, strict=True)
    for key, value in overrides.items():
        setattr(args, key, value)
    return interop.run_interop(args)


def test_reference_decoders_match_the_fixture_encoders():
    """The reference decoders must agree with the independent fixture encoders."""
    for _name, source, target, patch in interop.independent_bps_fixtures():
        assert interop.apply_bps_reference(source, patch) == target
    for case in interop.make_cases():
        if len(case.source) == len(case.target):
            assert interop.apply_ips_reference(case.source, interop.make_ips(case.source, case.target)) == case.target


def test_reference_bps_decoder_rejects_corrupted_crc_fields():
    _name, source, _target, patch = interop.independent_bps_fixtures()[1]
    for offset, message in ((-12, "source CRC"), (-8, "target CRC"), (-4, "patch CRC")):
        corrupted = bytearray(patch)
        corrupted[offset] ^= 0x01
        with pytest.raises(ValueError, match=message):
            interop.apply_bps_reference(source, bytes(corrupted))


def test_core_cli_patches_decode_independently(core_cli, tmp_path):
    report = run_harness(tmp_path, core_cli, core_only=True)
    assert len(report["core_cli"]["cases"]) == len(interop.make_cases())
    assert interop.evaluate_report(report) == []


def test_external_patchers_interoperate(core_cli, tmp_path):
    flips = Path(os.environ.get("STIRHEX_FLIPS", interop.FLIPS_DEFAULT))
    rompatcher = Path(os.environ.get("STIRHEX_ROMPATCHER", interop.ROMPATCHER_DEFAULT))
    node = Path(os.environ.get("STIRHEX_NODE", interop.NODE_DEFAULT))
    missing = [str(path) for path in (flips, rompatcher, node) if not path.is_file()]
    if missing:
        pytest.skip("external patchers are not installed: " + ", ".join(missing))
    report = run_harness(tmp_path, core_cli, flips=str(flips),
                         rompatcher=str(rompatcher), node=str(node))
    assert report["core_cli"]["external_tools"] == ["flips", "rompatcher"]
    limitations = []
    assert interop.evaluate_report(report, limitations) == []
    # Cases the pinned external tools cannot handle themselves are not StirHex
    # failures; list them so a run shows what was not judged.
    for limitation in limitations:
        print("external limitation:", limitation)
