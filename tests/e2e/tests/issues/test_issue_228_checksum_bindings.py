"""チェックサム計算のキーアサイン・ユーザーメニュー登録（Issue #228）。"""
import hashlib

import pytest
import win32con
import win32gui

from drivers.stirling_driver import (
    StirlingDriver, safe_set_focus,
    IDC_UM_CATEGORY, IDC_UM_MENUSET, IDC_UM_AVAILABLE, IDC_UM_CURRENT,
)
from tests.issues.test_issue_226_checksum import calculate, close, wait_for

pytestmark = pytest.mark.ported
RAW_CHECKSUM = 0x0710
NAME = 'チェックサム・ハッシュ計算...'
DATA = b'123456789'


def verify_dialog(drv):
    dialog = drv.find_process_dialog('チェックサム・ハッシュ計算')
    assert calculate(dialog) == hashlib.sha256(DATA).hexdigest().upper()
    close(dialog)


def invoke_key(drv):
    safe_set_focus(drv.hwnd)
    view = drv.get_view_hwnd()
    # WM_KEYDOWN が届く F8 を使う（F10 は WM_SYSKEYDOWN の対象）。
    # モーダルダイアログを開くキーなので、SendMessage で待たない。
    win32gui.PostMessage(view, win32con.WM_KEYDOWN, win32con.VK_F8, 0)
    win32gui.PostMessage(view, win32con.WM_KEYUP, win32con.VK_F8, 0)
    verify_dialog(drv)


def select_combo(page, control, index):
    combo = win32gui.GetDlgItem(page, control)
    win32gui.SendMessage(combo, win32con.CB_SETCURSEL, index, 0)
    win32gui.SendMessage(page, win32con.WM_COMMAND,
                         control | (win32con.CBN_SELCHANGE << 16), combo)


def invoke_menu(drv):
    safe_set_focus(drv.hwnd)
    drv.post_command(0x803A + 9) # ユーザーメニュー10
    assert drv.find_popup_menu() is not None
    drv.select_popup_menu_item(down_count=1)
    verify_dialog(drv)


def test_key_assign_checksum_and_restore(ported_exe_path, tmp_path):
    source = tmp_path / 'data.bin'
    source.write_bytes(DATA)
    options = [f'/ini:{tmp_path / "keys.ini"}']
    with StirlingDriver(ported_exe_path) as drv:
        drv.start(source, options=options)
        sheet, page = drv.open_key_assign_page()
        drv.key_assign_select_category(page, 7)
        assert (NAME, RAW_CHECKSUM) in drv.key_assign_functions(page)
        drv.key_assign_set_modifiers(page)
        drv.key_assign_select_key(page, 7) # F8
        assert drv.key_assign_current_function(page) == 0 # 既定割り当ては増やさない
        drv.key_assign_select_function(page, RAW_CHECKSUM)
        drv.close_settings_sheet(sheet, accept=True)
        invoke_key(drv)

    with StirlingDriver(ported_exe_path) as drv:
        drv.start(source, options=options)
        sheet, page = drv.open_key_assign_page()
        drv.key_assign_set_modifiers(page)
        drv.key_assign_select_key(page, 7)
        assert drv.key_assign_current_function(page) == RAW_CHECKSUM
        drv.close_settings_sheet(sheet, accept=False)
        invoke_key(drv)
    assert source.read_bytes() == DATA


def test_user_menu_checksum_and_restore(ported_exe_path, tmp_path):
    source = tmp_path / 'data.bin'
    source.write_bytes(DATA)
    options = [f'/ini:{tmp_path / "menu.ini"}']
    with StirlingDriver(ported_exe_path) as drv:
        drv.start(source, options=options)
        sheet, page = drv.open_user_menu_page()
        select_combo(page, IDC_UM_MENUSET, 9)
        assert drv.listbox_texts(page, IDC_UM_CURRENT) == []
        select_combo(page, IDC_UM_CATEGORY, 7)
        available = drv.listbox_texts(page, IDC_UM_AVAILABLE)
        assert NAME in available
        drv.um_select_available(page, available.index(NAME))
        drv.um_click_add(page)
        accel = drv.find_accel_dialog()
        assert accel is not None
        drv.accel_dialog_type(accel, 'H')
        drv.accel_dialog_close(accel, accept=True)
        wait_for(lambda: any(NAME in item for item in drv.listbox_texts(page, IDC_UM_CURRENT)),
                 'checksum added to user menu')
        drv.close_settings_sheet(sheet, accept=True)
        invoke_menu(drv)

    with StirlingDriver(ported_exe_path) as drv:
        drv.start(source, options=options)
        sheet, page = drv.open_user_menu_page()
        select_combo(page, IDC_UM_MENUSET, 9)
        assert any(NAME in item for item in drv.listbox_texts(page, IDC_UM_CURRENT))
        drv.close_settings_sheet(sheet, accept=False)
        invoke_menu(drv)
    assert source.read_bytes() == DATA


def test_checksum_is_not_a_toolbar_item(ported_exe_path, tmp_path):
    with StirlingDriver(ported_exe_path) as drv:
        drv.start(options=[f'/ini:{tmp_path / "toolbar.ini"}'])
        sheet, page = drv.open_toolbar_page()
        drv.toolbar_select_category(page, 7)
        assert RAW_CHECKSUM not in drv.toolbar_items(page, current=False)
        drv.close_settings_sheet(sheet, accept=False)
