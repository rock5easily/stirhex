import ctypes
import time
import winreg
from contextlib import ExitStack

import pytest
import win32api
import win32con
import win32event
import win32gui

from drivers.settings_context import (
    read_reg_values,
    registry_section,
    write_ansi_string_values,
)
from drivers.stirling_driver import StirlingDriver

LEGACY_ROOT = r"Software\StirlingPort\Stirling"
ENV_SECTION = rf"{LEGACY_ROOT}\Env"
EXT_SECTION = rf"{LEGACY_ROOT}\Extensions"
REC0_SECTION = rf"{LEGACY_ROOT}\Rec0"
REC1_SECTION = rf"{LEGACY_ROOT}\Rec1"
STIRHEX_ROOT = r"Software\StirHex\StirHex"
STIRHEX_ENV_SECTION = rf"{STIRHEX_ROOT}\Env"
STIRHEX_EXT_SECTION = rf"{STIRHEX_ROOT}\Extensions"
STIRHEX_REC0_SECTION = rf"{STIRHEX_ROOT}\Rec0"
STIRHEX_REC1_SECTION = rf"{STIRHEX_ROOT}\Rec1"

# Japanese settings a user of the MBCS build could have entered. Every one of them is
# representable in CP932, which is what the MBCS build was limited to.
JP_BACKUP_FOLDER = r"C:\作業\バックアップ"
JP_DEFAULT_FOLDER = r"C:\データ\日本語フォルダ"
JP_COMMENT_ALL = "すべてのファイル（日本語コメント）"
JP_COMMENT_BIN = "バイナリデータ・テスト用"
JP_FONT_FACE = "ＭＳ ゴシック"
EXT_BIN = "BIN;DAT"


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


def _run_once(exe, test_file):
    """Open `test_file` and exit cleanly so the settings are loaded and written back."""
    with StirlingDriver(exe) as drv:
        drv.start(test_file)
        time.sleep(0.5)
        assert _close_and_wait(drv), "Stirling did not exit cleanly; settings were not saved"


def _seed_mbcs_settings(stack: ExitStack) -> None:
    """Recreate the registry contents a pre-Unicode (MBCS) installation would leave behind.

    The numeric values go through the wide API - only REG_SZ is affected by the ANSI
    conversion. Wiping the sections first also guarantees the Env\\SettingsEncoding marker
    is absent, so the migration actually runs.
    """
    stack.enter_context(registry_section(ENV_SECTION))
    stack.enter_context(registry_section(EXT_SECTION, {"Count": (2, winreg.REG_DWORD)}))
    stack.enter_context(registry_section(REC0_SECTION))
    stack.enter_context(registry_section(REC1_SECTION))

    write_ansi_string_values(ENV_SECTION, {
        "BackupFolder": JP_BACKUP_FOLDER,
        "DefaultFolder": JP_DEFAULT_FOLDER,
    })
    write_ansi_string_values(EXT_SECTION, {
        "Ext0": "*",
        "Comment0": JP_COMMENT_ALL,
        "Ext1": EXT_BIN,
        "Comment1": JP_COMMENT_BIN,
    })
    write_ansi_string_values(REC0_SECTION, {"FontFace": JP_FONT_FACE})
    write_ansi_string_values(REC1_SECTION, {"FontFace": JP_FONT_FACE})


def _text(section: str, name: str) -> str | None:
    """Read one REG_SZ value through the wide API (what the Unicode build sees)."""
    entry = read_reg_values(section).get(name)
    return None if entry is None else str(entry[0])


def _assert_seeding_matches_the_acp() -> None:
    """Check the seeded state against what the running system's ACP implies.

    The ANSI seeding is the identity on a Japanese system and mangles the values anywhere
    else, so a single unconditional expectation would be wrong in one of the two. Asserting
    the right one before the app starts keeps the test honest in both: on ACP=932 there is
    nothing to repair, and on any other ACP the values really are broken at rest, so the
    run that follows has to be what fixes them.
    """
    stored = _text(ENV_SECTION, "BackupFolder")
    if ctypes.windll.kernel32.GetACP() == 932:
        assert stored == JP_BACKUP_FOLDER, (
            f"ACP=932 converts the CP932 bytes identically, so the seed must read back "
            f"unchanged: {stored!r}"
        )
    else:
        assert stored != JP_BACKUP_FOLDER, (
            f"a non-932 ACP must mangle the CP932 bytes at rest, otherwise this test "
            f"cannot exercise the migration at all: {stored!r}"
        )


def _assert_japanese_settings_intact(
    where: str,
    env_section: str = ENV_SECTION,
    ext_section: str = EXT_SECTION,
    rec0_section: str = REC0_SECTION,
    rec1_section: str = REC1_SECTION,
) -> None:
    assert _text(env_section, "BackupFolder") == JP_BACKUP_FOLDER, f"backup folder ({where})"
    assert _text(env_section, "DefaultFolder") == JP_DEFAULT_FOLDER, f"default folder ({where})"
    assert _text(ext_section, "Comment0") == JP_COMMENT_ALL, f"default record comment ({where})"
    assert _text(ext_section, "Ext1") == EXT_BIN, f"extension pattern ({where})"
    assert _text(ext_section, "Comment1") == JP_COMMENT_BIN, f"extension comment ({where})"
    assert _text(rec0_section, "FontFace") == JP_FONT_FACE, f"default record font face ({where})"
    assert _text(rec1_section, "FontFace") == JP_FONT_FACE, f"extension record font face ({where})"


class TestIssue43SettingsEncoding:
    """Regression tests for the Issue #66 decision not to migrate old port settings.

    The standalone Issue #43 conversion helper remains covered by the core tests. StirHex
    uses a new registry root, so startup must ignore the old MBCS-era root and preserve
    Unicode strings written directly under the StirHex root.
    """

    @pytest.mark.ported
    def test_stirhex_does_not_migrate_legacy_port_settings(self, ported_exe_path, tmp_path):
        test_file = tmp_path / "issue43_encoding.dat"
        test_file.write_bytes(bytes(range(256)))

        with ExitStack() as stack:
            _seed_mbcs_settings(stack)
            _assert_seeding_matches_the_acp()
            stack.enter_context(registry_section(STIRHEX_ENV_SECTION))
            stack.enter_context(registry_section(STIRHEX_EXT_SECTION))
            stack.enter_context(registry_section(STIRHEX_REC0_SECTION))
            stack.enter_context(registry_section(STIRHEX_REC1_SECTION))
            legacy_before = {
                section: read_reg_values(section)
                for section in (ENV_SECTION, EXT_SECTION, REC0_SECTION, REC1_SECTION)
            }

            _run_once(ported_exe_path, test_file)

            for section, values in legacy_before.items():
                assert read_reg_values(section) == values, f"legacy key was changed: {section}"
            new_env = read_reg_values(STIRHEX_ENV_SECTION)
            assert new_env.get("BackupFolder", (None,))[0] != JP_BACKUP_FOLDER
            assert new_env.get("DefaultFolder", (None,))[0] != JP_DEFAULT_FOLDER
            assert "SettingsEncoding" not in new_env

    @pytest.mark.ported
    def test_stirhex_preserves_unicode_settings_without_migration_marker(
        self, ported_exe_path, tmp_path
    ):
        test_file = tmp_path / "issue43_idempotent.dat"
        test_file.write_bytes(bytes(range(256)))

        with ExitStack() as stack:
            stack.enter_context(registry_section(STIRHEX_ENV_SECTION, {
                "BackupFolder": (JP_BACKUP_FOLDER, winreg.REG_SZ),
                "DefaultFolder": (JP_DEFAULT_FOLDER, winreg.REG_SZ),
            }))
            stack.enter_context(registry_section(STIRHEX_EXT_SECTION, {
                "Count": (2, winreg.REG_DWORD),
                "Ext0": ("*", winreg.REG_SZ),
                "Comment0": (JP_COMMENT_ALL, winreg.REG_SZ),
                "Ext1": (EXT_BIN, winreg.REG_SZ),
                "Comment1": (JP_COMMENT_BIN, winreg.REG_SZ),
            }))
            stack.enter_context(registry_section(STIRHEX_REC0_SECTION, {
                "FontFace": (JP_FONT_FACE, winreg.REG_SZ),
            }))
            stack.enter_context(registry_section(STIRHEX_REC1_SECTION, {
                "FontFace": (JP_FONT_FACE, winreg.REG_SZ),
            }))

            _run_once(ported_exe_path, test_file)
            _assert_japanese_settings_intact(
                "after the first start", STIRHEX_ENV_SECTION, STIRHEX_EXT_SECTION,
                STIRHEX_REC0_SECTION, STIRHEX_REC1_SECTION,
            )

            _run_once(ported_exe_path, test_file)
            _assert_japanese_settings_intact(
                "after the second start", STIRHEX_ENV_SECTION, STIRHEX_EXT_SECTION,
                STIRHEX_REC0_SECTION, STIRHEX_REC1_SECTION,
            )
            assert "SettingsEncoding" not in read_reg_values(STIRHEX_ENV_SECTION)
