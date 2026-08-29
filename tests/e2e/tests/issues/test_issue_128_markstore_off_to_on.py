"""Issue #128: マーク自動復元を OFF から ON にしても既存 MarkStore を失わない。

起動時の `LoadMarkStore()` が設定 OFF ではストアを読まずに戻っていたため、
そのセッション中に設定を ON へ変更して終了すると、終了時の `SaveMarkStore()` が
`Count=0` を書いて以前の記録を上書きしていた。
"""

import contextlib

import pytest
import win32con
import win32gui

from drivers.settings_context import read_reg_values, registry_section, settings_value
from drivers.stirling_driver import (
    ID_MARK2_TOGGLE,
    IDC_ED2_MARK_AUTO_RESTORE,
    StirlingDriver,
)

PORT_ENV = r"Software\StirHex\StirHex\Env"
PORT_MARK_STORE = r"Software\StirHex\StirHex\MarkStore"

SAMPLE = bytes(range(256)) * 4      # 1024 バイト
MARK_A = 0x10
MARK_B = 0x20

BM_GETCHECK = 0x00F0
BM_SETCHECK = 0x00F1


@contextlib.contextmanager
def _auto_restore(enabled: bool):
    with settings_value(PORT_ENV, "MarkAutoRestore", 1 if enabled else 0):
        yield


@pytest.fixture(autouse=True)
def _clean_mark_store():
    with registry_section(PORT_MARK_STORE):
        yield


def _mark_addresses(drv) -> list[int]:
    dialog = drv.open_mark_list_dialog()
    try:
        rows = drv.mark_list_entries(dialog)
    finally:
        drv.click_dialog_button(dialog, win32con.IDCANCEL)
    return sorted(int(text.split()[0], 16) for text, _data in rows)


def _put_marks(drv, path):
    drv.start(path)
    drv.jump_to_address("%X" % MARK_A)
    drv.mark_toggle()
    drv.jump_to_address("%X" % MARK_B)
    drv.post_command(ID_MARK2_TOGGLE)


def _set_mark_auto_restore(drv, enabled: bool):
    """環境設定「編集２」でマークの自動復元を切り替えて OK で確定する。"""
    sheet, page = drv.open_edit2_page()
    check = win32gui.GetDlgItem(page, IDC_ED2_MARK_AUTO_RESTORE)
    assert check, "マークの自動復元チェックが見つからない"
    win32gui.SendMessage(check, BM_SETCHECK, 1 if enabled else 0, 0)
    assert win32gui.SendMessage(check, BM_GETCHECK, 0, 0) == (1 if enabled else 0)
    drv.close_settings_sheet(sheet, accept=True)


@pytest.mark.ported
class TestIssue128MarkStoreOffToOn:

    def test_turning_the_setting_on_keeps_existing_records(
        self, ported_exe_path, tmp_path
    ):
        """OFF で起動したセッション中に ON へ変えて終了しても、記録が消えないこと。"""
        target = tmp_path / "off_to_on.dat"
        target.write_bytes(SAMPLE)

        with _auto_restore(True):
            with StirlingDriver(ported_exe_path) as drv:
                _put_marks(drv, target)
            recorded = read_reg_values(PORT_MARK_STORE)
        assert recorded, "ON で記録されていない（前提が崩れている）"

        # OFF で起動 → セッション中に ON へ切り替えて終了する。
        with _auto_restore(False):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start()
                _set_mark_auto_restore(drv, True)

        # 既存記録が残っており、次に ON で開けば復元できること。
        assert read_reg_values(PORT_MARK_STORE) == recorded, (
            "OFF→ON の切り替えで既存の MarkStore が変わっている"
        )
        with _auto_restore(True):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(target)
                addresses = _mark_addresses(drv)
        assert addresses == [MARK_A, MARK_B], (
            "OFF→ON の後にマークが復元されない: %s" % addresses
        )

    def test_staying_off_still_keeps_existing_records(
        self, ported_exe_path, tmp_path
    ):
        """OFF のまま起動・終了した場合は、既存記録を変更しない（既存動作の維持）。"""
        target = tmp_path / "stay_off.dat"
        target.write_bytes(SAMPLE)

        with _auto_restore(True):
            with StirlingDriver(ported_exe_path) as drv:
                _put_marks(drv, target)
            recorded = read_reg_values(PORT_MARK_STORE)
        assert recorded

        with _auto_restore(False):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(target)
                assert not _mark_addresses(drv), "設定 OFF なのに復元されている"

        assert read_reg_values(PORT_MARK_STORE) == recorded, (
            "OFF のまま終了しただけで記録が変わっている"
        )
