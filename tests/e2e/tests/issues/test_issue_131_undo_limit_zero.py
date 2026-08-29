"""Issue #131: `UndoMemoryLimitMB=0` の既存設定でも環境設定を確定できる。

`UndoMemoryLimit` キーが無かった頃の設定ファイルでは `UndoMemoryLimitMB=0` が
「無制限」を表していた。移行後は上限 ON が既定になり、入力欄に 0 が表示されるうえ、
範囲検証はチェックを外して欄が無効でも走るため、値を書き換えない限り確定できなかった。
"""

import time
import winreg

import pytest
import win32con
import win32gui

from drivers.settings_context import read_reg_values, registry_section, settings_value
from drivers.stirling_driver import (
    IDC_ED1_UNDO_LIMIT,
    IDC_ED1_UNDO_MB,
    StirlingDriver,
    _control_text,
    _set_control_text,
)

PORT_ENV = r"Software\StirHex\StirHex\Env"

DEFAULT_MB = 256   # CAppSettings::kUndoMemoryLimitDefaultMB

BM_GETCHECK = 0x00F0


def _click_ok(dialog_hwnd: int):
    """メッセージボックスの OK ボタンを押す（WM_COMMAND だけでは閉じないことがある）。"""
    buttons = []

    def _enum(h, _):
        if win32gui.GetClassName(h) == "Button":
            buttons.append(h)
        return True

    win32gui.EnumChildWindows(dialog_hwnd, _enum, None)
    if buttons:
        win32gui.PostMessage(buttons[0], win32con.BM_CLICK, 0, 0)
    else:
        win32gui.PostMessage(dialog_hwnd, win32con.WM_COMMAND, win32con.IDOK, 0)


def _dialog_count(drv) -> int:
    return sum(1 for _h, cls, _t in drv._get_process_windows() if cls == "#32770")


@pytest.mark.ported
class TestIssue131UndoLimitZero:

    def test_legacy_zero_migrates_to_limit_off(self, ported_exe_path):
        """`UndoMemoryLimit` キーが無く `UndoMemoryLimitMB=0` の設定を移行する。"""
        legacy = {"UndoMemoryLimitMB": (0, winreg.REG_DWORD)}
        with registry_section(PORT_ENV, legacy):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start()
                sheet, page = drv.open_edit1_page()
                check = win32gui.GetDlgItem(page, IDC_ED1_UNDO_LIMIT)
                edit = win32gui.GetDlgItem(page, IDC_ED1_UNDO_MB)
                assert check and edit

                assert win32gui.SendMessage(check, BM_GETCHECK, 0, 0) == 0, (
                    "0=無制限 が上限 OFF へ移行していない"
                )
                assert _control_text(edit) == str(DEFAULT_MB), (
                    "入力欄に有効な表示値が入っていない: %r" % _control_text(edit)
                )

                # そのまま確定できること（これがこの Issue の主眼）。
                drv.close_settings_sheet(sheet, accept=True)
                assert _dialog_count(drv) == 0, "範囲検証のメッセージが出ている"

            saved = read_reg_values(PORT_ENV)
        assert int(saved["UndoMemoryLimit"][0]) == 0, "上限 OFF が保存されていない"
        assert int(saved["UndoMemoryLimitMB"][0]) == DEFAULT_MB, (
            "入力欄の既定値が保存されていない: %s" % (saved.get("UndoMemoryLimitMB"),)
        )

    def test_limit_off_skips_the_range_check(self, ported_exe_path):
        """上限 OFF なら、入力欄が範囲外でも確定できる。"""
        with settings_value(PORT_ENV, "UndoMemoryLimit", 1), \
             settings_value(PORT_ENV, "UndoMemoryLimitMB", DEFAULT_MB):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start()
                sheet, page = drv.open_edit1_page()
                _set_control_text(win32gui.GetDlgItem(page, IDC_ED1_UNDO_MB), "0")
                # BM_CLICK はチェックを反転し、親へ BN_CLICKED も送る。
                win32gui.SendMessage(win32gui.GetDlgItem(page, IDC_ED1_UNDO_LIMIT),
                                     win32con.BM_CLICK, 0, 0)
                time.sleep(0.2)
                drv.close_settings_sheet(sheet, accept=True)
                assert _dialog_count(drv) == 0, "上限 OFF なのに範囲検証が走っている"

            saved = read_reg_values(PORT_ENV)
        assert int(saved["UndoMemoryLimit"][0]) == 0, "上限 OFF が保存されていない"

    def test_limit_on_still_validates_the_range(self, ported_exe_path):
        """上限 ON では従来どおり有効範囲を検証する（既存動作の維持）。"""
        with settings_value(PORT_ENV, "UndoMemoryLimit", 1), \
             settings_value(PORT_ENV, "UndoMemoryLimitMB", DEFAULT_MB):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start()
                sheet, page = drv.open_edit1_page()
                _set_control_text(win32gui.GetDlgItem(page, IDC_ED1_UNDO_MB), "0")
                btn = win32gui.GetDlgItem(sheet, win32con.IDOK)
                win32gui.PostMessage(btn, win32con.BM_CLICK, 0, 0)
                time.sleep(0.8)

                boxes = [h for h, cls, _t in drv._get_process_windows()
                         if cls == "#32770" and h != sheet
                         and win32gui.IsWindowVisible(h)]
                assert boxes, "範囲検証のメッセージが出ていない"
                assert win32gui.IsWindowVisible(sheet), (
                    "範囲外の値のまま確定できてしまっている"
                )
                for box in boxes:
                    _click_ok(box)
                time.sleep(0.5)
                assert not [h for h, cls, _t in drv._get_process_windows()
                            if cls == "#32770" and h != sheet
                            and win32gui.IsWindowVisible(h)], "メッセージが閉じていない"

                _set_control_text(win32gui.GetDlgItem(page, IDC_ED1_UNDO_MB),
                                  str(DEFAULT_MB))
                drv.close_settings_sheet(sheet, accept=False)
