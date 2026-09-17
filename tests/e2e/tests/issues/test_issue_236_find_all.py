"""Issue #236: ファイル内の全て検索と検索結果一覧。

[検索] ダイアログの [全て検索] で、一致箇所をすべて検索結果一覧（モードレス）に並べ、
一覧からジャンプできる。原版に無い機能のためゴールデン比較は行わない。確認する内容:
  - 一致の件数・アドレス・データが一覧に並び、重なった一致も数える
  - ワイルドカード `??` と検索範囲（カーソル位置から / 選択範囲内）が条件に効く
  - [ジャンプ] で一致範囲が選択され、キャレットが一致の先頭へ移る
  - 文書を編集すると「内容が変更されました」になりジャンプできない。[再検索] で更新される
  - 保存（別名保存）だけでは結果は古い扱いにならない
  - 上限 10,000 件を超えると打ち切る
  - 見つからない場合の表示、文書を閉じると一覧も閉じる
"""

import re
import time

import pytest
import win32clipboard
import win32con
import win32gui
from pywinauto import timings

from drivers.stirling_driver import (
    CMD_FILE_CLOSE,
    ID_JUMP,
    LVM_GETITEMCOUNT,
    StirlingDriver,
    _control_text,
    _listview_item_text,
    _send_message_w,
    _set_control_text,
)

ID_EDIT_FIND = 57636          # MFC 標準 ID_EDIT_FIND
IDC_FIND_TYPE_HEX = 1016
IDC_FIND_TYPE_TEXT = 1017
IDC_FIND_RANGE_CURSOR = 1018
IDC_FIND_RANGE_ALL = 1019
IDC_FIND_RANGE_SEL = 1044
IDC_FIND_COMBO = 1026
IDC_FIND_ALL = 1410
IDC_FINDRESULT_CONDITION = 1411
IDC_FINDRESULT_STATUS = 1412
IDC_FINDRESULT_LIST = 1413
IDC_FINDRESULT_RESEARCH = 1414
IDC_FINDRESULT_STOP = 1415
IDC_JUMP_HINT_CURRENT = 1019  # ジャンプダイアログの「現在アドレス」表示

RESULT_TITLE_PREFIX = "検索結果一覧 - "


def _visible_dialogs(drv):
    return [(h, title) for h, cls, title in drv._get_process_windows()
            if cls == "#32770" and win32gui.IsWindowVisible(h)]


def _find_result_dialog(drv, timeout=10.0):
    def _find():
        for h, title in _visible_dialogs(drv):
            if title.startswith(RESULT_TITLE_PREFIX):
                return h
        raise RuntimeError("find result dialog not found yet")

    return timings.wait_until_passes(timeout, 0.2, _find)


def _visible_dialog_with(drv, control_id, exclude=()):
    def _find():
        for h, _title in _visible_dialogs(drv):
            if h in exclude:
                continue
            try:
                if win32gui.GetDlgItem(h, control_id):
                    return h
            except win32gui.error:
                continue
        raise RuntimeError("dialog not found yet")

    return timings.wait_until_passes(10, 0.2, _find)


def _find_all(drv, text, is_hex=True, range_id=IDC_FIND_RANGE_ALL, result_dialog=None):
    """[検索] ダイアログを開いて [全て検索] を押し、結果一覧のハンドルを返す。"""
    exclude = (result_dialog,) if result_dialog else ()
    win32gui.PostMessage(drv.hwnd, win32con.WM_COMMAND, ID_EDIT_FIND, 0)
    dlg = _visible_dialog_with(drv, IDC_FIND_ALL, exclude)
    type_id = IDC_FIND_TYPE_HEX if is_hex else IDC_FIND_TYPE_TEXT
    win32gui.SendMessage(win32gui.GetDlgItem(dlg, type_id), win32con.BM_CLICK, 0, 0)
    win32gui.SendMessage(win32gui.GetDlgItem(dlg, range_id), win32con.BM_CLICK, 0, 0)
    _set_control_text(win32gui.GetDlgItem(dlg, IDC_FIND_COMBO), text)
    time.sleep(0.2)
    win32gui.PostMessage(dlg, win32con.WM_COMMAND, IDC_FIND_ALL, 0)
    deadline = time.time() + 5.0
    while win32gui.IsWindow(dlg) and time.time() < deadline:
        time.sleep(0.1)
    assert not win32gui.IsWindow(dlg), "find dialog should close after [全て検索]"
    return _find_result_dialog(drv)


def _status(result):
    return _control_text(win32gui.GetDlgItem(result, IDC_FINDRESULT_STATUS))


def _wait_status(result, pattern, timeout=15.0):
    deadline = time.time() + timeout
    text = ""
    while time.time() < deadline:
        text = _status(result)
        if re.search(pattern, text):
            return text
        time.sleep(0.2)
    raise AssertionError(f"status did not match {pattern!r}: {text!r}")


def _rows(drv, result):
    list_hwnd = win32gui.GetDlgItem(result, IDC_FINDRESULT_LIST)
    list_view = drv.app.window(handle=list_hwnd).wrapper_object()
    count = _send_message_w(list_hwnd, LVM_GETITEMCOUNT)
    return [(_listview_item_text(list_view, row, 0), _listview_item_text(list_view, row, 1))
            for row in range(count)]


def _row_count(result):
    return _send_message_w(win32gui.GetDlgItem(result, IDC_FINDRESULT_LIST), LVM_GETITEMCOUNT)


def _enabled(result, control_id):
    return bool(win32gui.IsWindowEnabled(win32gui.GetDlgItem(result, control_id)))


def _jump_to_row(result, row):
    """一覧の row 行目を選んで [ジャンプ] する（選択はキー操作で動かす）。"""
    list_hwnd = win32gui.GetDlgItem(result, IDC_FINDRESULT_LIST)
    win32gui.SendMessage(list_hwnd, win32con.WM_SETFOCUS, 0, 0)
    win32gui.SendMessage(list_hwnd, win32con.WM_KEYDOWN, win32con.VK_HOME, 0)
    win32gui.SendMessage(list_hwnd, win32con.WM_KEYUP, win32con.VK_HOME, 0)
    for _ in range(row):
        win32gui.SendMessage(list_hwnd, win32con.WM_KEYDOWN, win32con.VK_DOWN, 0)
        win32gui.SendMessage(list_hwnd, win32con.WM_KEYUP, win32con.VK_DOWN, 0)
    time.sleep(0.2)
    win32gui.PostMessage(result, win32con.WM_COMMAND, win32con.IDOK, 0)
    time.sleep(0.5)


def _caret_address(drv, exclude=()):
    drv.post_command(ID_JUMP)
    dlg = _visible_dialog_with(drv, IDC_JUMP_HINT_CURRENT, exclude)
    try:
        text = win32gui.GetDlgItemText(dlg, IDC_JUMP_HINT_CURRENT).strip()
        m = re.fullmatch(r"[^:：]*[:：]\s*([0-9A-Fa-f]+)", text)
        assert m, "cannot read the current address: %r" % text
        return int(m.group(1), 16)
    finally:
        win32gui.PostMessage(dlg, win32con.WM_COMMAND, win32con.IDCANCEL, 0)
        deadline = time.time() + 5.0
        while win32gui.IsWindow(dlg) and time.time() < deadline:
            time.sleep(0.1)


def _clipboard_hex(drv):
    drv.copy()
    for _ in range(20):
        try:
            win32clipboard.OpenClipboard()
            break
        except Exception:
            time.sleep(0.05)
    else:
        raise AssertionError("clipboard remained locked")
    try:
        text = win32clipboard.GetClipboardData(win32clipboard.CF_UNICODETEXT)
    finally:
        win32clipboard.CloseClipboard()
    return re.sub(r"\s", "", text).upper()


def _close_result(result):
    if win32gui.IsWindow(result):
        win32gui.PostMessage(result, win32con.WM_COMMAND, win32con.IDCANCEL, 0)
        deadline = time.time() + 5.0
        while win32gui.IsWindow(result) and time.time() < deadline:
            time.sleep(0.1)


class TestIssue236FindAll:
    @pytest.mark.ported
    def test_find_all_lists_matches_and_jumps(self, ported_exe_path, tmp_path):
        """`41 ?? 43` の一致が一覧に並び、[ジャンプ] で一致範囲が選択される。"""
        test_file = tmp_path / "find_all.dat"
        data = bytes([0x41, 0x10, 0x43, 0x00, 0x41, 0x20, 0x43, 0x41, 0x43, 0xFF,
                      0x41, 0x41, 0x43, 0x43])
        test_file.write_bytes(data)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            time.sleep(0.5)

            result = _find_all(drv, "41??43")
            _wait_status(result, r"4 件見つかりました")
            title = win32gui.GetWindowText(result)
            assert title == RESULT_TITLE_PREFIX + "find_all.dat", title
            condition = _control_text(win32gui.GetDlgItem(result, IDC_FINDRESULT_CONDITION))
            assert "41 ?? 43" in condition and "16進データ" in condition and "データ全体" in condition, condition
            assert _rows(drv, result) == [
                ("00000000", "41 10 43"),
                ("00000004", "41 20 43"),
                ("0000000A", "41 41 43"),
                ("0000000B", "41 43 43"),
            ]
            assert _enabled(result, win32con.IDOK), "first row is selected, so jump is enabled"
            assert _enabled(result, IDC_FINDRESULT_RESEARCH)
            assert not _enabled(result, IDC_FINDRESULT_STOP)

            _jump_to_row(result, 1)
            assert win32gui.IsWindow(result), "result dialog stays open after jump"
            assert _caret_address(drv) == 4, "jump places the caret at the match"
            assert _clipboard_hex(drv) == "412043", "jump selects the whole match"

            _jump_to_row(result, 2)
            assert _caret_address(drv) == 0x0A
            assert _clipboard_hex(drv) == "414143"
            assert test_file.read_bytes() == data

    @pytest.mark.ported
    def test_overlap_and_ranges(self, ported_exe_path, tmp_path):
        """重なった一致も数え、[カーソル位置から] と [選択範囲内] が範囲を絞る。"""
        test_file = tmp_path / "find_all_range.dat"
        test_file.write_bytes(bytes([0xAA] * 6 + [0x00] + [0xAA] * 3))

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            time.sleep(0.5)

            result = _find_all(drv, "AA AA")
            _wait_status(result, r"7 件見つかりました")   # 0..4 と 7,8
            assert [addr for addr, _ in _rows(drv, result)] == [
                "00000000", "00000001", "00000002", "00000003", "00000004", "00000007", "00000008"]

            _close_result(result)
            drv.jump_to_address("5")
            result = _find_all(drv, "AA AA", range_id=IDC_FIND_RANGE_CURSOR)
            _wait_status(result, r"2 件見つかりました")
            assert [addr for addr, _ in _rows(drv, result)] == ["00000007", "00000008"]
            condition = _control_text(win32gui.GetDlgItem(result, IDC_FINDRESULT_CONDITION))
            assert "カーソル位置から" in condition, condition

            # [再検索] は最初の検索のキャレット位置 (5) から探す。ジャンプでキャレットが 8 へ
            #   動いても範囲は縮まない（現在のキャレットから探すと 1 件になる）。
            _jump_to_row(result, 1)
            assert _caret_address(drv) == 8
            win32gui.PostMessage(result, win32con.WM_COMMAND, IDC_FINDRESULT_RESEARCH, 0)
            _wait_status(result, r"2 件見つかりました")
            assert [addr for addr, _ in _rows(drv, result)] == ["00000007", "00000008"]

            _close_result(result)
            drv.select_range_dialog("2", "5")   # [2, 5] = 4 バイト
            result = _find_all(drv, "AA AA", range_id=IDC_FIND_RANGE_SEL)
            _wait_status(result, r"3 件見つかりました")   # 2,3,4（4-5 は範囲内に収まる）
            assert [addr for addr, _ in _rows(drv, result)] == ["00000002", "00000003", "00000004"]

    @pytest.mark.ported
    def test_edit_marks_results_outdated_and_research_updates(self, ported_exe_path, tmp_path):
        """編集で結果が古い扱いになりジャンプできない。[再検索] で最新の内容に更新される。
        別名保存だけでは古い扱いにならない。"""
        test_file = tmp_path / "find_all_edit.dat"
        test_file.write_bytes(bytes([0x11, 0x22, 0x00, 0x11, 0x22, 0x00, 0x11, 0x22]))
        saved = tmp_path / "find_all_saved.dat"

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            time.sleep(0.5)

            result = _find_all(drv, "11 22")
            _wait_status(result, r"3 件見つかりました")

            drv.save_as_via_dialog(saved)
            time.sleep(1.0)
            assert re.search(r"3 件見つかりました", _status(result)), \
                "saving without changing data must not mark the results outdated"
            assert _enabled(result, win32con.IDOK)
            assert win32gui.GetWindowText(result) == RESULT_TITLE_PREFIX + "find_all_saved.dat", \
                "title follows the renamed document"

            _jump_to_row(result, 1)            # キャレットを 3 へ
            drv.type_hex_chars("FF")           # ジャンプで選択した一致 [3, 5) を FF 1 バイトで置き換える
            _wait_status(result, r"内容が変更されました")
            assert not _enabled(result, win32con.IDOK), "jump is disabled for outdated results"
            assert _row_count(result) == 3, "outdated results stay listed"

            win32gui.PostMessage(result, win32con.WM_COMMAND, IDC_FINDRESULT_RESEARCH, 0)
            _wait_status(result, r"2 件見つかりました")   # 11 22 00 FF 00 11 22
            assert [addr for addr, _ in _rows(drv, result)] == ["00000000", "00000005"]
            assert _enabled(result, win32con.IDOK)

            # Undo もデータの変更として扱う。
            drv.undo()
            _wait_status(result, r"内容が変更されました")
            assert not _enabled(result, win32con.IDOK)
            win32gui.PostMessage(result, win32con.WM_COMMAND, IDC_FINDRESULT_RESEARCH, 0)
            _wait_status(result, r"3 件見つかりました")

    @pytest.mark.ported
    def test_truncates_over_limit(self, ported_exe_path, tmp_path):
        """一致が 10,000 件を超えると打ち切り、10,000 件を一覧にする。"""
        test_file = tmp_path / "find_all_many.dat"
        test_file.write_bytes(bytes(20000))

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            time.sleep(0.5)

            result = _find_all(drv, "00")
            _wait_status(result, r"10000 件を超えたため、検索を打ち切りました")
            assert _row_count(result) == 10000
            list_hwnd = win32gui.GetDlgItem(result, IDC_FINDRESULT_LIST)
            list_view = drv.app.window(handle=list_hwnd).wrapper_object()
            assert _listview_item_text(list_view, 9999, 0) == "0000270F", "last listed hit is the 10,000th"

    @pytest.mark.ported
    def test_stop_running_search(self, ported_exe_path, tmp_path):
        """[中止] で検索を止めると「中止」の状態になり、[再検索] が使えるようになる。"""
        test_file = tmp_path / "find_all_large.dat"
        # 0 が続くデータで末尾が一致しないパターンを探すと、1バイトずつしか進めず時間がかかる
        #   （開発機で 128MB に約 2.5 秒）。一覧が開いたら状態表示を待たずに [中止] を送り、
        #   検索が先に終わる競合を避ける。「中止」の状態は走査中にしか到達しない。
        with open(test_file, "wb") as f:
            f.truncate(128 * 1024 * 1024)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            time.sleep(1.0)

            win32gui.PostMessage(drv.hwnd, win32con.WM_COMMAND, ID_EDIT_FIND, 0)
            dlg = _visible_dialog_with(drv, IDC_FIND_ALL)
            _set_control_text(win32gui.GetDlgItem(dlg, IDC_FIND_COMBO), "00 01")
            win32gui.SendMessage(win32gui.GetDlgItem(dlg, IDC_FIND_RANGE_ALL), win32con.BM_CLICK, 0, 0)
            win32gui.PostMessage(dlg, win32con.WM_COMMAND, IDC_FIND_ALL, 0)
            result = 0
            deadline = time.time() + 10.0
            while not result and time.time() < deadline:
                for h, title in _visible_dialogs(drv):
                    if title.startswith(RESULT_TITLE_PREFIX):
                        result = h
                        break
                else:
                    time.sleep(0.02)
            assert result, "find result dialog did not open"
            win32gui.PostMessage(result, win32con.WM_COMMAND, IDC_FINDRESULT_STOP, 0)

            status = _wait_status(result, r"検索を中止しました|見つかりませんでした")
            assert "検索を中止しました（0 件）" in status,                 f"search finished before [中止] was processed: {status!r}"
            assert not _enabled(result, IDC_FINDRESULT_STOP)
            assert _enabled(result, IDC_FINDRESULT_RESEARCH)

    @pytest.mark.ported
    def test_not_found_text_search_and_close_with_document(self, ported_exe_path, tmp_path):
        """見つからない場合の表示。文字列の `?` は文字として検索する。文書を閉じると一覧も閉じる。"""
        test_file = tmp_path / "find_all_text.dat"
        test_file.write_bytes(b"abc a?c abc")

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            time.sleep(0.5)

            result = _find_all(drv, "zz", is_hex=False)
            _wait_status(result, r"見つかりませんでした")
            assert _row_count(result) == 0
            assert not _enabled(result, win32con.IDOK)

            result = _find_all(drv, "a?c", is_hex=False, result_dialog=result)
            _wait_status(result, r"1 件見つかりました")
            assert _rows(drv, result) == [("00000004", "61 3F 63")]
            condition = _control_text(win32gui.GetDlgItem(result, IDC_FINDRESULT_CONDITION))
            assert "文字列" in condition, condition

            drv.post_command(CMD_FILE_CLOSE)
            deadline = time.time() + 5.0
            while win32gui.IsWindow(result) and time.time() < deadline:
                time.sleep(0.1)
            assert not win32gui.IsWindow(result), "closing the document closes its result dialog"
