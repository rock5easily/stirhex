"""The Debug build's CRT leak report, checked over a mixed session (Issue #224).

The Debug build writes the CRT's leak list to the file named by STIRHEX_LEAK_REPORT when
the process exits. This drives a session that touches the parts of the editor that
allocate - documents, bars, dialogs, an edit and its undo - and requires the list to come
back empty.

Unlike the counter loops, this sees the individual allocations that were never freed, so a
failure names the size and contents of each leaked block.
"""

import os
import time
from pathlib import Path

import pytest

from drivers.stirling_driver import (
    CMD_FILE_CLOSE,
    CMD_FILE_NEW,
    ID_BITIMAGE,
    ID_GOTO_DATA_END,
    ID_GOTO_DATA_TOP,
    ID_OUTPUT_PANE,
    ID_STRUCT_EDIT,
    StirlingDriver,
)

PORTING_ROOT = Path(__file__).resolve().parents[4]   # porting/tests/e2e/tests/stability
DEBUG_EXE_CANDIDATES = (
    PORTING_ROOT / "StirHex" / "x64" / "Debug" / "bin" / "StirHex.exe",
    PORTING_ROOT / "StirHex" / "Debug" / "bin" / "StirHex.exe",
)


@pytest.fixture
def debug_exe_path() -> Path:
    """The Debug build, which is the only one that reports leaks."""
    for candidate in DEBUG_EXE_CANDIDATES:
        if candidate.exists():
            return candidate
    pytest.skip(
        "No Debug build found; the CRT leak report exists only there. Build one with: "
        "porting\\build.bat Debug x64"
    )


@pytest.mark.ported
def test_debug_session_reports_no_leaks(debug_exe_path, sample_binary_file, tmp_path):
    """A session that opens documents, bars and dialogs must leak nothing."""
    report = tmp_path / "leaks.txt"
    previous = os.environ.get("STIRHEX_LEAK_REPORT")
    os.environ["STIRHEX_LEAK_REPORT"] = str(report)
    try:
        with StirlingDriver(debug_exe_path) as drv:
            drv.start(sample_binary_file)
            assert drv.hwnd != 0, "StirHex did not open its main window"
            for _ in range(3):
                drv.post_command(ID_GOTO_DATA_END)
                drv.post_command(ID_GOTO_DATA_TOP)
                drv.post_command(ID_STRUCT_EDIT)
                drv.post_command(ID_STRUCT_EDIT)
                drv.post_command(ID_BITIMAGE)
                drv.post_command(ID_BITIMAGE)
                drv.post_command(ID_OUTPUT_PANE)
                drv.post_command(ID_OUTPUT_PANE)
                drv.post_command(CMD_FILE_NEW)
                drv.post_command(CMD_FILE_CLOSE)
            drv.jump_to_address("20")
            drv.type_hex_chars("A5")
            drv.undo()
            time.sleep(0.5)
    finally:
        if previous is None:
            os.environ.pop("STIRHEX_LEAK_REPORT", None)
        else:
            os.environ["STIRHEX_LEAK_REPORT"] = previous

    # The report is written while the process exits, a moment after the driver returns.
    deadline = time.time() + 5.0
    while time.time() < deadline and not report.exists():
        time.sleep(0.2)

    assert report.exists(), (
        "the Debug build wrote no leak report; STIRHEX_LEAK_REPORT was not honoured"
    )
    text = report.read_text(encoding="utf-8", errors="replace").strip()
    assert text == "", f"the Debug build reported memory leaks:\n{text[:4000]}"
