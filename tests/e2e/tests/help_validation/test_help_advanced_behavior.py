import ctypes
import time

import pytest
import win32con
import win32clipboard
import win32gui

from drivers.stirling_driver import (
    IDC_DIFFLIST_HILITE,
    IDC_DIFFLIST_SYNC,
    IDC_MISMATCH_BYTE,
    IDC_MISMATCH_RANGE_SEL,
    IDC_PRINTRANGE_PREVIEW,
    IDC_RANGEBAR_END,
    IDC_RANGEBAR_START,
    IDC_RANGEBAR_USESEL,
    IDC_SYNC_ADD,
    IDC_SYNC_REGISTERED,
    IDC_SYNC_REMOVE,
    IDC_SYNC_RESET,
    ID_SYNC_SCROLL,
    StirlingDriver,
    _control_text,
    _set_control_text,
)


def _wait_for(predicate, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return
        time.sleep(0.1)
    raise AssertionError("condition did not become true")


def _dialog_exists(drv: StirlingDriver, title: str) -> bool:
    return any(cls == "#32770" and caption == title
               for _hwnd, cls, caption in drv._get_process_windows())


def _message_text(drv: StirlingDriver) -> str:
    dialog = drv.find_process_dialog("StirHex", timeout=5.0)
    texts = []
    buttons = []

    def _collect(hwnd, _):
        text = _control_text(hwnd)
        if text:
            texts.append(text)
        if win32gui.GetClassName(hwnd) == "Button":
            buttons.append(hwnd)
        return True

    win32gui.EnumChildWindows(dialog, _collect, None)
    assert buttons, "message box has no button"
    win32gui.PostMessage(buttons[0], win32con.BM_CLICK, 0, 0)
    _wait_for(lambda: not win32gui.IsWindow(dialog))
    time.sleep(0.3)
    return "\n".join(texts)


def _clipboard_unicode_text() -> str:
    for _ in range(20):
        try:
            win32clipboard.OpenClipboard()
            break
        except Exception:
            time.sleep(0.05)
    else:
        raise AssertionError("clipboard remained locked")
    try:
        return win32clipboard.GetClipboardData(win32clipboard.CF_UNICODETEXT)
    finally:
        win32clipboard.CloseClipboard()


class TestHelpAdvancedBehavior:
    @pytest.mark.ported
    def test_hv025_mismatch_normalization_range_and_validation(
        self, ported_exe_path, tmp_path
    ):
        test_file = tmp_path / "mismatch.dat"
        test_file.write_bytes(b"\x0A\x0A\xFF\x0A")

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            dialog = drv.open_find_mismatch_dialog()
            assert not win32gui.IsWindowEnabled(
                win32gui.GetDlgItem(dialog, IDC_MISMATCH_RANGE_SEL)
            )

            drv.configure_find_mismatch(dialog, "a", range_mode="all")
            drv.execute_find_mismatch(dialog, forward=True)
            assert _control_text(win32gui.GetDlgItem(dialog, IDC_MISMATCH_BYTE)) == "0A"

            _set_control_text(win32gui.GetDlgItem(dialog, IDC_MISMATCH_BYTE), "123")
            drv.execute_find_mismatch(dialog, forward=True)
            message = drv.find_process_dialog("StirHex", timeout=5.0)
            assert win32gui.IsWindow(message)
            assert win32gui.IsWindow(dialog)
            _message_text(drv)
            assert win32gui.IsWindow(dialog)
            assert _control_text(
                win32gui.GetDlgItem(dialog, IDC_MISMATCH_BYTE)
            ) == "123"

            drv.click_dialog_button(dialog, win32con.IDCANCEL)
            drv.copy()
            assert _clipboard_unicode_text() == "FF"
            assert test_file.read_bytes() == b"\x0A\x0A\xFF\x0A"

    @pytest.mark.ported
    def test_hv027_compare_diff_list_controls(
        self, ported_exe_path, tmp_path
    ):
        first = tmp_path / "compare_first.dat"
        second = tmp_path / "compare_second.dat"
        first.write_bytes(bytes([0x00, 0x11, 0x22, 0x33, 0x44, 0x55]))
        second.write_bytes(bytes([0x00, 0xAA, 0xBB, 0x33, 0xCC, 0x55]))

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(first)
            drv.open_file_via_dialog(second)
            compare = drv.open_compare_dialog()
            candidates = drv.compare_candidates(compare)
            assert len(candidates) == 1
            assert first.name in candidates[0]
            drv.accept_compare(compare)

            diff = drv.find_diff_list_dialog()
            assert drv.get_diff_list_rows(diff) == [
                ("00000001", "00000002", "00000002"),
                ("00000004", "", "00000001"),
            ]
            assert drv.diff_list_checks(diff) == (True, True)

            active_before = drv.active_mdi_title()
            drv.click_dialog_button(diff, 1000)  # IDC_DIFFLIST_SWITCH
            assert drv.active_mdi_title() != active_before

            drv.click_dialog_button(diff, IDC_DIFFLIST_HILITE)
            drv.click_dialog_button(diff, IDC_DIFFLIST_SYNC)
            assert drv.diff_list_checks(diff) == (False, False)

            drv.click_dialog_button(diff, win32con.IDOK)
            drv.click_dialog_button(diff, win32con.IDCANCEL)
            _wait_for(lambda: not win32gui.IsWindow(diff))
            drv.copy()
            assert _clipboard_unicode_text() == "11 22"

    @pytest.mark.ported
    def test_hv027_compare_identical_and_size_difference_messages(
        self, ported_exe_path, tmp_path
    ):
        same1 = tmp_path / "same1.dat"
        same2 = tmp_path / "same2.dat"
        same1.write_bytes(b"SAME")
        same2.write_bytes(b"SAME")

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(same1)
            drv.open_file_via_dialog(same2)
            compare = drv.open_compare_dialog()
            drv.accept_compare(compare)
            assert "違いはありません" in _message_text(drv)

        short = tmp_path / "short.dat"
        long = tmp_path / "long.dat"
        short.write_bytes(b"ABC")
        long.write_bytes(b"AXCD")
        with StirlingDriver(ported_exe_path) as drv:
            drv.start(short)
            drv.open_file_via_dialog(long)
            compare = drv.open_compare_dialog()
            drv.accept_compare(compare)
            assert "サイズが異なります" in _message_text(drv)
            diff = drv.find_diff_list_dialog()
            assert drv.get_diff_list_rows(diff) == [
                ("00000001", "", "00000001")
            ]
            drv.click_dialog_button(diff, win32con.IDCANCEL)

    @pytest.mark.ported
    def test_hv028_sync_scroll_dialog_and_propagation(
        self, ported_exe_path, tmp_path
    ):
        files = [tmp_path / f"sync_{index}.dat" for index in range(3)]
        for index, path in enumerate(files):
            path.write_bytes(bytes([index]) * 8192)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(files[0])
            drv.post_command(ID_SYNC_SCROLL)
            time.sleep(0.4)
            assert not _dialog_exists(drv, "シンクロスクロール")

            drv.open_file_via_dialog(files[1])
            drv.open_file_via_dialog(files[2])
            dialog = drv.open_sync_scroll_dialog()
            candidates, registered = drv.sync_scroll_lists(dialog)
            assert len(candidates) == 2 and registered == []
            assert win32gui.IsWindowEnabled(win32gui.GetDlgItem(dialog, IDC_SYNC_ADD))
            assert not win32gui.IsWindowEnabled(win32gui.GetDlgItem(dialog, IDC_SYNC_REMOVE))
            assert not win32gui.IsWindowEnabled(win32gui.GetDlgItem(dialog, IDC_SYNC_RESET))

            drv.click_dialog_button(dialog, IDC_SYNC_ADD)
            assert tuple(map(len, drv.sync_scroll_lists(dialog))) == (1, 1)
            drv.click_dialog_button(dialog, IDC_SYNC_REMOVE)
            assert tuple(map(len, drv.sync_scroll_lists(dialog))) == (2, 0)
            drv.click_dialog_button(dialog, IDC_SYNC_ADD)
            registered_title = drv.sync_scroll_lists(dialog)[1][0]
            drv.click_dialog_button(dialog, win32con.IDCANCEL)

            dialog = drv.open_sync_scroll_dialog()
            assert tuple(map(len, drv.sync_scroll_lists(dialog))) == (2, 0)
            drv.click_dialog_button(dialog, IDC_SYNC_ADD)
            drv.click_dialog_button(dialog, win32con.IDOK)

            views = drv.get_mdi_views()
            owner_title = drv.active_mdi_title()
            owner_view = next(view for title, _child, view in views if title == owner_title)
            partner_view = next(
                view for title, _child, view in views
                if registered_title.split(" - ")[0] in title
            )
            other_view = next(
                view for title, _child, view in views
                if view not in (owner_view, partner_view)
            )
            get_scroll_pos = ctypes.windll.user32.GetScrollPos
            assert get_scroll_pos(owner_view, win32con.SB_VERT) == 0
            assert get_scroll_pos(partner_view, win32con.SB_VERT) == 0
            assert get_scroll_pos(other_view, win32con.SB_VERT) == 0

            win32gui.SendMessage(owner_view, win32con.WM_VSCROLL, win32con.SB_PAGEDOWN, 0)
            time.sleep(0.3)
            owner_pos = get_scroll_pos(owner_view, win32con.SB_VERT)
            assert owner_pos > 0
            assert get_scroll_pos(partner_view, win32con.SB_VERT) == owner_pos
            assert get_scroll_pos(other_view, win32con.SB_VERT) == 0

            dialog = drv.open_sync_scroll_dialog()
            drv.click_dialog_button(dialog, IDC_SYNC_RESET)
            assert tuple(map(len, drv.sync_scroll_lists(dialog))) == (2, 0)
            drv.click_dialog_button(dialog, win32con.IDOK)

    @pytest.mark.ported
    def test_hv034_print_range_validation_preview_and_restore(
        self, ported_exe_path, tmp_path
    ):
        test_file = tmp_path / "print_range.dat"
        test_file.write_bytes(bytes(range(256)))

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            child_before = drv.get_mdi_views()[0][1]
            rect_before = win32gui.GetWindowRect(child_before)

            dialog, bar = drv.open_print_range_dialog()
            assert win32gui.SendMessage(
                win32gui.GetDlgItem(dialog, IDC_PRINTRANGE_PREVIEW),
                win32con.BM_GETCHECK,
                0,
                0,
            ) == win32con.BST_UNCHECKED

            drv.set_print_range(dialog, bar, "0", "100", is_hex=True, preview=True)
            drv.click_dialog_button(dialog, win32con.IDOK)
            assert "無効" in _message_text(drv)
            assert win32gui.IsWindow(dialog)

            drv.set_print_range(dialog, bar, "10", "5", is_hex=True, preview=True)
            drv.click_dialog_button(dialog, win32con.IDOK)
            _wait_for(drv.is_print_preview_active, timeout=10.0)
            drv.close_print_preview()
            _wait_for(lambda: not drv.is_print_preview_active(), timeout=10.0)

            views_after = drv.get_mdi_views()
            assert len(views_after) == 1
            assert test_file.name in views_after[0][0]
            assert win32gui.GetWindowRect(views_after[0][1]) == rect_before

            drv.select_range_dialog("2", "4", is_hex=True)
            dialog, bar = drv.open_print_range_dialog()
            use_selection = win32gui.GetDlgItem(bar, IDC_RANGEBAR_USESEL)
            assert win32gui.SendMessage(
                use_selection, win32con.BM_GETCHECK, 0, 0
            ) == win32con.BST_CHECKED
            assert not win32gui.IsWindowEnabled(
                win32gui.GetDlgItem(bar, IDC_RANGEBAR_START)
            )
            assert not win32gui.IsWindowEnabled(
                win32gui.GetDlgItem(bar, IDC_RANGEBAR_END)
            )
            drv.click_dialog_button(dialog, win32con.IDCANCEL)
