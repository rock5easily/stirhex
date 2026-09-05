import os
import shutil
import time
import warnings
from pathlib import Path
import pytest
from drivers.process_guard import (
    StirlingProcess,
    describe_processes,
    find_stirling_processes,
    stop_command,
    terminate_processes,
)
from drivers.settings_context import settings_value
from drivers.stirling_driver import StirlingDriver

WORKSPACE_ROOT = Path(__file__).resolve().parents[3]
ORIGINAL_EXE = WORKSPACE_ROOT / "analysis_target" / "Stirling.exe"
# Ported exe candidates for Win32 and x64
PORTED_EXE_WIN32_RELEASE = WORKSPACE_ROOT / "porting" / "StirHex" / "Release" / "bin" / "StirHex.exe"
PORTED_EXE_WIN32_DEBUG = WORKSPACE_ROOT / "porting" / "StirHex" / "Debug" / "bin" / "StirHex.exe"
PORTED_EXE_X64_RELEASE = WORKSPACE_ROOT / "porting" / "StirHex" / "x64" / "Release" / "bin" / "StirHex.exe"
PORTED_EXE_X64_DEBUG = WORKSPACE_ROOT / "porting" / "StirHex" / "x64" / "Debug" / "bin" / "StirHex.exe"


class StaleStirlingProcessWarning(UserWarning):
    """A test left a Stirling / StirHex process behind."""


# Stirling and StirHex refuse to run twice, so one leaked process makes every later launch
# hand its command line to that process and exit - the driver then waits for a window that
# never appears. Guarding the session start, every test, and the session end keeps a single
# leak from poisoning the rest of the run (Issue #113).
_SESSION_BASELINE: set[tuple[int, int]] = set()


def pytest_addoption(parser):
    parser.addoption(
        "--stale-processes",
        action="store",
        default="error",
        choices=("error", "kill"),
        help="What to do about Stirling / StirHex processes that are already running when "
             "the session starts: 'error' (default) stops before any test runs, 'kill' "
             "terminates them and continues. The default does not kill, because a process "
             "found here may be an editor you have open with unsaved edits.",
    )


def pytest_sessionstart(session):
    stale = find_stirling_processes()
    if stale and session.config.getoption("--stale-processes") == "kill":
        survivors = terminate_processes(stale)
        killed = [p for p in stale if p not in survivors]
        if killed:
            print(f"\nTerminated stale Stirling / StirHex processes:\n"
                  f"{describe_processes(killed)}")
        stale = survivors

    if stale:
        pytest.exit(
            "Stirling / StirHex is already running. Every golden test would fail with a\n"
            "main-window timeout, because a second launch is handed to the running instance\n"
            f"and exits immediately.\n{describe_processes(stale)}\n"
            f"Close it, or run it down with:\n  {stop_command(stale)}\n"
            "Pass --stale-processes=kill to have the test session do that for you.",
            returncode=pytest.ExitCode.USAGE_ERROR,
        )

    _SESSION_BASELINE.clear()
    _SESSION_BASELINE.update(p.key for p in find_stirling_processes())


def _processes_since(baseline: set[tuple[int, int]],
                     settle: float = 2.0) -> list[StirlingProcess]:
    """Return processes that appeared after `baseline` and are still alive.

    A process that is shutting down normally can outlive the test by a moment, so give the
    set a short chance to drain before calling anything a leak.
    """
    deadline = time.time() + settle
    while True:
        extra = [p for p in find_stirling_processes() if p.key not in baseline]
        if not extra or time.time() >= deadline:
            return extra
        time.sleep(0.2)


# The port restores the bit image window's visibility and placement on start (Issue #121).
# The suite shares %APPDATA%\StirHex\StirHex.ini with whatever else drives StirHex on this
# machine, so a session that was left with the pane open would otherwise start every test
# with a floating window on screen - covering dialogs the tests click. Pin it hidden and
# restore the previous value afterwards; a test that needs it shown sets its own value,
# which is written later and therefore wins.
PORT_ENV_ROOT = r"Software\StirHex\StirHex\Env"


@pytest.fixture(autouse=True)
def hidden_bit_image_by_default():
    with settings_value(PORT_ENV_ROOT, "BitImageShow", 0):
        yield


@pytest.fixture(autouse=True)
def stirling_process_guard(request):
    """Terminate - and blame - any Stirling / StirHex process a test leaves behind.

    Only processes that appear during the test are touched: anything already running when
    the test began is somebody else's, and killing it could discard unsaved edits.
    """
    before = {p.key for p in find_stirling_processes()}
    yield
    leaked = _processes_since(before)
    if not leaked:
        return
    survivors = terminate_processes(leaked)
    detail = describe_processes(leaked)
    if survivors:
        detail += f"\nCould not terminate:\n{describe_processes(survivors)}"
    warnings.warn(
        f"{request.node.nodeid} left {len(leaked)} Stirling / StirHex process(es) running; "
        f"they were terminated so the rest of the session can launch normally.\n{detail}",
        StaleStirlingProcessWarning,
    )


def pytest_sessionfinish(session, exitstatus):
    leaked = _processes_since(_SESSION_BASELINE, settle=1.0)
    if not leaked:
        return
    survivors = terminate_processes(leaked)
    print(f"\nTerminated Stirling / StirHex processes left over from this session:\n"
          f"{describe_processes(leaked)}")
    if survivors:
        print(f"Could not terminate:\n{describe_processes(survivors)}")


def get_ported_exe() -> Path:
    # 1. Environment variable for exact path (highest priority)
    env_exe = os.environ.get("STIRHEX_PORTED_EXE")
    if env_exe:
        exe_path = Path(env_exe)
        if not exe_path.exists():
            raise FileNotFoundError(
                f"StirHex.exe specified by STIRHEX_PORTED_EXE not found at {exe_path}"
            )
        exe = exe_path
    else:
        # 2. Candidate paths based on STIRLING_PLATFORM or default (x64 preferred)
        platform_env = os.environ.get("STIRLING_PLATFORM", "").strip()
        if platform_env.lower() in ("win32", "x86"):
            candidates = [PORTED_EXE_WIN32_RELEASE, PORTED_EXE_WIN32_DEBUG]
        elif platform_env.lower() in ("x64", "win64", "amd64"):
            candidates = [PORTED_EXE_X64_RELEASE, PORTED_EXE_X64_DEBUG]
        elif platform_env != "":
            raise ValueError(
                f"Unsupported STIRLING_PLATFORM='{platform_env}'. Expected 'Win32' or 'x64'."
            )
        else:
            # 既定は x64（移植版の既定プラットフォーム。Issue #23）。
            #   Win32 を試すときは STIRLING_PLATFORM=Win32 を指定する。
            candidates = [
                PORTED_EXE_X64_RELEASE,
                PORTED_EXE_X64_DEBUG,
                PORTED_EXE_WIN32_RELEASE,
                PORTED_EXE_WIN32_DEBUG,
            ]

        exe = None
        for cand in candidates:
            if cand.exists():
                exe = cand
                break

        if exe is None:
            cand_str = "\n".join(f"  - {p}" for p in candidates)
            raise FileNotFoundError(
                f"StirHex.exe not found. Searched candidates:\n{cand_str}"
            )

    # Ensure Struct.def exists in same directory as StirHex.exe
    target_def = exe.parent / "Struct.def"
    src_def = WORKSPACE_ROOT / "analysis_target" / "struct.def"
    if not target_def.exists() and src_def.exists():
        shutil.copy2(src_def, target_def)

    return exe


@pytest.fixture
def original_exe_path() -> Path:
    if not ORIGINAL_EXE.exists():
        pytest.skip(f"Original Stirling.exe not found at {ORIGINAL_EXE}")
    return ORIGINAL_EXE


@pytest.fixture
def ported_exe_path() -> Path:
    return get_ported_exe()


@pytest.fixture
def sample_binary_file(tmp_path) -> Path:
    """Generate a reproducible sample binary file (256 bytes sequence + ASCII)."""
    p = tmp_path / "sample.bin"
    # Create 256-byte sequence (0x00 .. 0xFF) followed by ASCII text
    data = bytes(range(256)) + b"Hello Stirling Binary Editor 2026! 0123456789\r\n"
    p.write_bytes(data)
    return p


@pytest.fixture
def run_both_stirling(original_exe_path, ported_exe_path, tmp_path):
    """Fixture that helper executes an identical operation sequence on both original and ported Stirling,
    and returns paths to the outputs generated by both."""
    
    def _runner(action_fn, initial_data: bytes) -> tuple[bytes, bytes]:
        # Prepare inputs with .dat extension (uses default Rec0 display setting: LineSize=16, AddressBase=16hex)
        orig_in = tmp_path / "in_orig.dat"
        port_in = tmp_path / "in_port.dat"
        orig_out = tmp_path / "out_orig.dat"
        port_out = tmp_path / "out_port.dat"

        orig_in.write_bytes(initial_data)
        port_in.write_bytes(initial_data)

        # Run on original
        with StirlingDriver(original_exe_path) as drv_orig:
            drv_orig.start(orig_in)
            action_fn(drv_orig, orig_out)
        
        # Run on ported
        with StirlingDriver(ported_exe_path) as drv_port:
            drv_port.start(port_in)
            action_fn(drv_port, port_out)

        orig_result = orig_out.read_bytes() if orig_out.exists() else b""
        port_result = port_out.read_bytes() if port_out.exists() else b""
        return orig_result, port_result

    return _runner
