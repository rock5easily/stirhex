import time
import winreg

import pytest
import win32api
import win32con
import win32event
import win32gui

from conftest import (
    PORTED_EXE_WIN32_DEBUG,
    PORTED_EXE_WIN32_RELEASE,
    PORTED_EXE_X64_DEBUG,
    PORTED_EXE_X64_RELEASE,
)
from drivers.settings_context import caret_store, read_caret_store, stirling_settings
from drivers.stirling_driver import StirlingDriver

# Path of an entry that is only ever seeded and read back through the registry.
# It never has to exist: the app must carry unrelated entries through unchanged.
HUGE_FILE_PATH = r"C:\stirling-issue22-huge.bin"
HUGE_FILE_ADDR = "1FFFFFFFF"   # 8GB-1, i.e. beyond what the legacy 32-bit format could hold


def _first_existing(*candidates):
    for c in candidates:
        if c.exists():
            return c
    return None


def _close_and_wait(drv: StirlingDriver, timeout: float = 20.0) -> bool:
    """Close the app with WM_CLOSE and wait for the process to terminate.

    Settings are persisted in ExitInstance(), so the process must be allowed to exit
    on its own - StirlingDriver.close() kills it after a short delay.
    """
    handle = win32api.OpenProcess(win32con.SYNCHRONIZE, False, drv.pid)
    try:
        win32gui.PostMessage(drv.hwnd, win32con.WM_CLOSE, 0, 0)
        return win32event.WaitForSingleObject(handle, int(timeout * 1000)) == win32event.WAIT_OBJECT_0
    finally:
        win32api.CloseHandle(handle)


def _store_entries(values: dict[str, str]) -> dict[str, str]:
    """Reduce a raw caret store dump to {path: address text} for readable assertions."""
    count = int(values.get("Count", 0))
    entries = {}
    for i in range(count):
        path = values.get(f"Path{i}")
        addr = values.get(f"Addr{i}")
        if path is None or addr is None:
            continue
        entries[path] = addr
    return entries


def _run_once(exe, test_file, jump_to: int | None = None):
    """Open `test_file`, optionally move the caret, and exit cleanly so settings are saved.

    Note: focus_view() clicks into the view, which moves the caret to the top of the
    window. It is therefore only used when the caret is moved explicitly afterwards.
    """
    with StirlingDriver(exe) as drv:
        drv.start(test_file)
        time.sleep(0.5)
        if jump_to is not None:
            drv.focus_view()
            drv.jump_to_address(f"{jump_to:X}", is_hex=True)
            time.sleep(0.3)
        assert _close_and_wait(drv), "Stirling did not exit cleanly; settings were not saved"


class TestIssue22SettingsMigration:
    """Tests for Issue #22: 64-bit settings persistence and migration of 32-bit settings.

    The caret auto-restore store holds file addresses, so it is saved as an uppercase hex
    string in Addr%d. Values written by the 32-bit build live in Pos%d (a decimal number,
    REG_DWORD back when the store was in the registry) and must be migrated on read, then
    removed on save.

    The caret position on exit is recorded from the view, so the address written back to
    the settings file is what the view actually restored.
    """

    @pytest.mark.ported
    def test_ported_migrates_legacy_32bit_caret_position(self, ported_exe_path, tmp_path):
        """A caret position saved by the 32-bit build (Pos%d, decimal) is restored into the
        view, then rewritten in the 64-bit format (Addr%d, hex) with the legacy value gone."""
        test_file = tmp_path / "issue22_legacy.dat"
        test_file.write_bytes(bytes(range(256)))
        legacy_pos = 0x40

        seed = {
            "Count": (1, winreg.REG_DWORD),
            "Path0": (str(test_file), winreg.REG_SZ),
            "Pos0": (legacy_pos, winreg.REG_DWORD),   # legacy 32-bit format
        }
        with stirling_settings(CaretAutoRestore=1), caret_store(seed):
            _run_once(ported_exe_path, test_file)
            values = read_caret_store()

        assert "Pos0" not in values, "legacy Pos0 must be removed once migrated"
        assert values["Addr0"] == f"{legacy_pos:X}", (
            "Addr0 must be written in the 64-bit format: an uppercase hex address, not the\n"
            "decimal a 32-bit REG_DWORD would have produced"
        )
        assert _store_entries(values) == {str(test_file): f"{legacy_pos:X}"}, (
            "legacy caret position was not restored and rewritten in the 64-bit format"
        )

    @pytest.mark.ported
    def test_ported_keeps_caret_at_top_when_auto_restore_is_off(self, ported_exe_path, tmp_path):
        """Negative control for the migration test: with auto restore off the view opens at 0,
        so the address written back is 0 rather than the seeded one."""
        test_file = tmp_path / "issue22_norestore.dat"
        test_file.write_bytes(bytes(range(256)))

        seed = {
            "Count": (1, winreg.REG_DWORD),
            "Path0": (str(test_file), winreg.REG_SZ),
            "Pos0": (0x40, winreg.REG_DWORD),
        }
        with stirling_settings(CaretAutoRestore=0), caret_store(seed):
            _run_once(ported_exe_path, test_file)
            values = read_caret_store()

        assert _store_entries(values) == {str(test_file): "0"}, (
            "caret must stay at the top of the file when auto restore is disabled"
        )

    @pytest.mark.ported
    def test_ported_preserves_64bit_caret_position(self, ported_exe_path, tmp_path):
        """A stored address beyond 32 bits survives a load/save round trip unchanged
        (the 32-bit format silently truncated it)."""
        test_file = tmp_path / "issue22_roundtrip.dat"
        test_file.write_bytes(bytes(range(256)))
        jump_to = 0x20

        seed = {
            "Count": (2, winreg.REG_DWORD),
            "Path0": (HUGE_FILE_PATH, winreg.REG_SZ),
            "Addr0": (HUGE_FILE_ADDR, winreg.REG_SZ),
            "Path1": (str(test_file), winreg.REG_SZ),
            "Addr1": ("0", winreg.REG_SZ),
        }
        with stirling_settings(CaretAutoRestore=1), caret_store(seed):
            _run_once(ported_exe_path, test_file, jump_to=jump_to)
            values = read_caret_store()

        entries = _store_entries(values)
        assert entries.get(HUGE_FILE_PATH) == HUGE_FILE_ADDR, (
            f"64-bit address was not preserved: {entries!r}"
        )
        assert entries.get(str(test_file)) == f"{jump_to:X}", (
            f"caret position of the edited file was not stored: {entries!r}"
        )

    @pytest.mark.ported
    def test_win32_and_x64_builds_share_the_caret_store(self, tmp_path):
        """Both platform builds use one registry key, so each must read back what the other
        wrote - including an address that does not fit in the legacy 32-bit format."""
        win32_exe = _first_existing(PORTED_EXE_WIN32_RELEASE, PORTED_EXE_WIN32_DEBUG)
        x64_exe = _first_existing(PORTED_EXE_X64_RELEASE, PORTED_EXE_X64_DEBUG)
        if win32_exe is None or x64_exe is None:
            pytest.skip("both the Win32 and the x64 build are required for this test")

        file_x64 = tmp_path / "issue22_shared_x64.dat"
        file_win32 = tmp_path / "issue22_shared_win32.dat"
        for f in (file_x64, file_win32):
            f.write_bytes(bytes(range(256)))

        seed = {
            "Count": (1, winreg.REG_DWORD),
            "Path0": (HUGE_FILE_PATH, winreg.REG_SZ),
            "Addr0": (HUGE_FILE_ADDR, winreg.REG_SZ),
        }
        with stirling_settings(CaretAutoRestore=1), caret_store(seed):
            _run_once(x64_exe, file_x64, jump_to=0x10)
            after_x64 = _store_entries(read_caret_store())
            _run_once(win32_exe, file_win32, jump_to=0x20)
            after_win32 = _store_entries(read_caret_store())

        assert after_x64 == {
            str(file_x64): "10",
            HUGE_FILE_PATH: HUGE_FILE_ADDR,
        }, f"x64 build did not keep the shared store intact: {after_x64!r}"
        assert after_win32 == {
            str(file_win32): "20",
            str(file_x64): "10",
            HUGE_FILE_PATH: HUGE_FILE_ADDR,
        }, f"Win32 build did not read back what the x64 build wrote: {after_win32!r}"
