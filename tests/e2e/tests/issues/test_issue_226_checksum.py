"""Issue #226: tools menu and checksums of current document bytes."""
import ctypes
import hashlib
import zlib

import pytest
import win32clipboard
import win32con
import win32gui
from pywinauto import timings

from drivers.stirling_driver import StirlingDriver, _control_text

pytestmark = pytest.mark.ported
COMMAND = 33020
ALL, SELECTION, ALGORITHM, START, LENGTH, RESULT, COPY, STATUS, STOP = range(1400, 1409)


def wait_for(predicate, description, timeout=8):
    def check():
        value = predicate()
        assert value, description
        return value
    return timings.wait_until_passes(timeout, 0.05, check)


def open_checksum(drv):
    drv.post_command(COMMAND)
    return wait_for(lambda: next((h for h, cls, title in drv._get_process_windows()
        if cls == '#32770' and title == 'チェックサム・ハッシュ計算'), None), 'checksum dialog')


def text(dialog, control):
    return _control_text(win32gui.GetDlgItem(dialog, control))


def click(dialog, control):
    win32gui.SendMessage(win32gui.GetDlgItem(dialog, control), win32con.BM_CLICK, 0, 0)


def close(dialog):
    win32gui.PostMessage(dialog, win32con.WM_COMMAND, win32con.IDCANCEL, 0)
    wait_for(lambda: not win32gui.IsWindow(dialog), 'dialog closed')


def choose(dialog, index):
    combo = win32gui.GetDlgItem(dialog, ALGORITHM)
    win32gui.SendMessage(combo, win32con.CB_SETCURSEL, index, 0)
    win32gui.SendMessage(dialog, win32con.WM_COMMAND,
        ALGORITHM | (win32con.CBN_SELCHANGE << 16), combo)


def calculate(dialog):
    click(dialog, win32con.IDOK)
    wait_for(lambda: bool(text(dialog, RESULT)), 'calculation complete')
    return text(dialog, RESULT)


def expected(data, index):
    if index == 0:
        return f'{zlib.crc32(data):08X}'
    return hashlib.new(('md5', 'sha1', 'sha256')[index-1], data).hexdigest().upper()


def menu_info(drv):
    menu = win32gui.GetMenu(drv.hwnd)
    get_string = ctypes.windll.user32.GetMenuStringW
    get_string.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_wchar_p, ctypes.c_int, ctypes.c_uint]
    get_string.restype = ctypes.c_int
    entries = []
    for index in range(win32gui.GetMenuItemCount(menu)):
        buf = ctypes.create_unicode_buffer(128)
        get_string(menu, index, buf, len(buf), win32con.MF_BYPOSITION)
        entries.append(buf.value)
    index = entries.index('ツール(&T)')
    assert entries[index-1] == '検索・移動(&S)'
    assert entries[index+1] == '設定(&O)'
    submenu = win32gui.GetSubMenu(menu, index)
    win32gui.SendMessage(drv.hwnd, win32con.WM_INITMENUPOPUP, submenu, index)
    state = win32gui.GetMenuState(submenu, COMMAND, win32con.MF_BYCOMMAND)
    assert state != -1
    return state


def test_tools_menu_without_document_and_empty_new_document(ported_exe_path):
    with StirlingDriver(ported_exe_path) as drv:
        drv.start()
        assert menu_info(drv) & (win32con.MF_GRAYED | win32con.MF_DISABLED)
        drv.post_command(57600) # File / New
        assert not (menu_info(drv) & (win32con.MF_GRAYED | win32con.MF_DISABLED))
        dialog = open_checksum(drv)
        assert not win32gui.IsWindowEnabled(win32gui.GetDlgItem(dialog, SELECTION))
        assert text(dialog, LENGTH) == '0'
        assert calculate(dialog) == expected(b'', 3)
        close(dialog)


@pytest.mark.parametrize('algorithm', range(4))
def test_whole_selection_copy_and_stale_result(ported_exe_path, tmp_path, algorithm):
    data = bytes(range(256)) * 260
    path = tmp_path / 'hash.dat'
    path.write_bytes(data)
    with StirlingDriver(ported_exe_path) as drv:
        drv.start(path)
        assert not menu_info(drv) & (win32con.MF_GRAYED | win32con.MF_DISABLED)
        drv.select_range_dialog('3FFD', '8007') # inclusive UI endpoints
        titles = drv.get_mdi_child_titles()
        dialog = open_checksum(drv)
        assert win32gui.SendMessage(win32gui.GetDlgItem(dialog, SELECTION), win32con.BM_GETCHECK) == win32con.BST_CHECKED
        assert text(dialog, START) == '0x3FFD'
        assert text(dialog, LENGTH) == str(0x8008 - 0x3FFD)
        choose(dialog, algorithm)
        assert calculate(dialog) == expected(data[0x3FFD:0x8008], algorithm)
        click(dialog, COPY)
        win32clipboard.OpenClipboard()
        try:
            assert win32clipboard.GetClipboardData(win32con.CF_UNICODETEXT) == expected(data[0x3FFD:0x8008], algorithm)
        finally:
            win32clipboard.CloseClipboard()
        click(dialog, ALL)
        assert not text(dialog, RESULT)
        assert not win32gui.IsWindowEnabled(win32gui.GetDlgItem(dialog, COPY))
        assert text(dialog, START) == '0x0' and text(dialog, LENGTH) == str(len(data))
        assert calculate(dialog) == expected(data, algorithm)
        choose(dialog, (algorithm+1) % 4)
        assert not text(dialog, RESULT)
        assert not win32gui.IsWindowEnabled(win32gui.GetDlgItem(dialog, COPY))
        close(dialog)
        assert drv.get_mdi_child_titles() == titles
        dialog = open_checksum(drv)
        assert text(dialog, START) == '0x3FFD' # selection was preserved
        assert text(dialog, LENGTH) == str(0x8008-0x3FFD)
        close(dialog)
        out = tmp_path / 'unchanged.dat'
        drv.save_as_via_dialog(out)
        assert out.read_bytes() == data
    assert path.read_bytes() == data


@pytest.mark.parametrize('operation', ['overwrite', 'insert', 'delete'])
def test_unsaved_edits_and_undo(ported_exe_path, tmp_path, operation):
    data = bytes(range(32))
    path = tmp_path / 'unsaved.dat'
    path.write_bytes(data)
    with StirlingDriver(ported_exe_path) as drv:
        drv.start(path)
        drv.jump_to_address('0')
        if operation == 'delete':
            drv.press_delete()
            edited = data[1:]
        else:
            if operation == 'insert':
                drv.press_insert()
            drv.type_hex_chars('AB')
            edited = b'\xAB' + (data if operation == 'insert' else data[1:])
        titles = drv.get_mdi_child_titles()
        assert any(t.endswith('*') for t in titles)
        dialog = open_checksum(drv)
        assert calculate(dialog) == expected(edited, 3)
        close(dialog)
        assert drv.get_mdi_child_titles() == titles
        assert path.read_bytes() == data
        drv.undo()
        dialog = open_checksum(drv)
        assert calculate(dialog) == expected(data, 3)
        close(dialog)
        assert all(not t.endswith('*') for t in drv.get_mdi_child_titles())
        drv.redo()
        dialog = open_checksum(drv)
        assert calculate(dialog) == expected(edited, 3)
        close(dialog)


def test_read_only_and_cancel_restart(ported_exe_path, tmp_path):
    data = bytes(range(256)) * (128 * 1024) # 32 MiB: spans many UI turns
    path = tmp_path / 'cancel.dat'
    path.write_bytes(data)
    with StirlingDriver(ported_exe_path) as drv:
        drv.start(path)
        drv.post_command(32805) # Edit prohibited / allowed
        dialog = open_checksum(drv)
        choose(dialog, 0)
        # Python がスケジュールされるまでに完了した場合は、正しい完了結果を
        # 確認して再試行する。中止できたこと自体は必須とし、スキップしない。
        for attempt in range(3):
            click(dialog, 1)
            wait_for(lambda: text(dialog, STATUS).startswith('計算中:') or text(dialog, RESULT),
                     'calculation started')
            stop_enabled = win32gui.IsWindowEnabled(win32gui.GetDlgItem(dialog, STOP))
            algorithm_enabled = win32gui.IsWindowEnabled(win32gui.GetDlgItem(dialog, ALGORITHM))
            if not stop_enabled or algorithm_enabled:
                assert text(dialog, RESULT) == expected(data, 0)
                continue
            assert stop_enabled
            assert not algorithm_enabled
            click(dialog, STOP)
            if text(dialog, STATUS) == '計算が完了しました。':
                assert text(dialog, RESULT) == expected(data, 0)
                continue
            break
        else:
            pytest.fail('Calculation completed before cancellation in all three attempts')
        assert text(dialog, STATUS) == '計算を中止しました。'
        assert not text(dialog, RESULT)
        assert not win32gui.IsWindowEnabled(win32gui.GetDlgItem(dialog, COPY))
        choose(dialog, 3)
        assert calculate(dialog) == expected(data, 3)
        click(dialog, 1)
        close(dialog) # Closing while running also cancels.
        assert path.read_bytes() == data
        assert all(not t.endswith('*') for t in drv.get_mdi_child_titles())



def test_algorithm_validation_and_item_mapping(ported_exe_path, tmp_path):
    data = b'123456789'
    path = tmp_path / 'algorithm.dat'
    path.write_bytes(data)
    with StirlingDriver(ported_exe_path) as drv:
        drv.start(path)
        dialog = open_checksum(drv)
        assert calculate(dialog) == expected(data, 3)
        choose(dialog, -1) # Simulate a combo without a selected item.
        win32gui.PostMessage(dialog, win32con.WM_COMMAND, win32con.IDOK, 0)
        error = wait_for(lambda: next((h for h, cls, title in drv._get_process_windows()
            if cls == '#32770' and h != dialog), None), 'algorithm error dialog')
        messages = []
        buttons = []
        def collect(hwnd, _):
            messages.append(_control_text(hwnd))
            if win32gui.GetClassName(hwnd) == 'Button':
                buttons.append(hwnd)
        win32gui.EnumChildWindows(error, collect, None)
        assert '計算方式を選択できませんでした。計算方式を選び直してください。' in messages
        assert len(buttons) == 1, messages
        win32gui.SendMessage(buttons[0], win32con.BM_CLICK, 0, 0)
        wait_for(lambda: not win32gui.IsWindow(error), 'error dialog closed')
        assert not text(dialog, RESULT)
        assert not win32gui.IsWindowEnabled(win32gui.GetDlgItem(dialog, COPY))
        combo = win32gui.GetDlgItem(dialog, ALGORITHM)
        wait_for(lambda: drv.app.window(handle=dialog).get_focus().handle == combo,
                 'focus returned to algorithm selector')
        # Removing the first entry shifts MD5 to index zero; the displayed method
        # must still select MD5, not the enum value that happens to be zero.
        win32gui.SendMessage(combo, win32con.CB_DELETESTRING, 0, 0)
        choose(dialog, 0)
        assert _control_text(combo) == 'MD5'
        assert calculate(dialog) == expected(data, 1)
        close(dialog)
