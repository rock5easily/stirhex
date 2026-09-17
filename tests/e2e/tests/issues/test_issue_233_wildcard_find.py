"""Issue #233: 16進データ検索のワイルドカード `??`。

`??` は任意の1バイトに一致する（VS Code Hex Editor 形式）。使えるのは [検索] ダイアログの
16進データと、[置換] ダイアログの検索データ（16進）だけで、置換データや文字列では使えない。

原版に無い機能のためゴールデン比較は行わない。確認する内容:
  - [検索] で `??` を含むパターンが一致し、検索欄が `41 ?? 43` へ整形される
  - 続けて [次検索] / 繰り返し検索（前検索）がワイルドカード位置を含めて条件を引き継ぐ
  - `4?` などの不正な入力、すべてワイルドカードの入力はエラーになる
  - 文字列検索では `?` が文字として扱われる
  - [置換] の一括置換で `??` を含む検索データが置き換わる
  - 置換データに `??` を入れるとエラーになり、データは変わらない
"""

import re
import time

import pytest
import win32clipboard
import win32con
import win32gui
from pywinauto import timings

from drivers.stirling_driver import (
    CMD_EDIT_REPLACE,
    IDC_REPL_ALL,
    IDC_REPL_RANGE_ALL,
    IDC_REPL_REPLACE_COMBO,
    IDC_REPL_SEARCH_COMBO,
    ID_FIND_PREV,
    ID_JUMP,
    StirlingDriver,
    _control_text,
    _set_control_text,
)

ID_EDIT_FIND = 57636          # MFC 標準 ID_EDIT_FIND
IDC_FIND_TYPE_HEX = 1016      # 検索データ種別: 16進
IDC_FIND_TYPE_TEXT = 1017     # 検索データ種別: 文字列
IDC_FIND_RANGE_ALL = 1019     # 検索範囲: データ全体
IDC_FIND_COMBO = 1026         # 検索データ入力コンボ
IDC_FIND_NEXT = 1042          # 次検索ボタン
IDC_JUMP_HINT_CURRENT = 1019  # ジャンプダイアログの「現在アドレス」表示

INVALID_DATA_MESSAGE = "データの指定が不正です"


def _visible_dialogs(drv):
    return [h for h, cls, _title in drv._get_process_windows()
            if cls == "#32770" and win32gui.IsWindowVisible(h)]


def _wait_no_dialog(drv, timeout=10.0):
    def _check():
        if _visible_dialogs(drv):
            raise RuntimeError("dialog still open")
        return True

    timings.wait_until_passes(timeout, 0.2, _check)


def _visible_dialog_with(drv, control_id):
    def _find():
        for h in _visible_dialogs(drv):
            if win32gui.GetDlgItem(h, control_id):
                return h
        raise RuntimeError("dialog not found yet")

    return timings.wait_until_passes(10, 0.2, _find)


def _dismiss_message(drv, owner):
    """owner 以外に開いたメッセージボックスの本文を読み、閉じる。"""
    def _find():
        for h in _visible_dialogs(drv):
            if h != owner:
                return h
        raise RuntimeError("message box not found yet")

    box = timings.wait_until_passes(5, 0.2, _find)
    texts = []
    buttons = []

    def _collect(hwnd, _):
        text = _control_text(hwnd)
        if text:
            texts.append(text)
        if win32gui.GetClassName(hwnd) == "Button":
            buttons.append(hwnd)
        return True

    win32gui.EnumChildWindows(box, _collect, None)
    assert buttons, "message box has no button"
    win32gui.PostMessage(buttons[0], win32con.BM_CLICK, 0, 0)
    deadline = time.time() + 5.0
    while win32gui.IsWindow(box) and time.time() < deadline:
        time.sleep(0.1)
    assert not win32gui.IsWindow(box), "message box did not close"
    time.sleep(0.3)
    return "\n".join(texts)


def _no_message_box(drv, owner):
    return all(h == owner for h in _visible_dialogs(drv))


def _open_find_dialog(drv, is_hex=True):
    _wait_no_dialog(drv)
    win32gui.PostMessage(drv.hwnd, win32con.WM_COMMAND, ID_EDIT_FIND, 0)
    dlg = _visible_dialog_with(drv, IDC_FIND_COMBO)
    type_id = IDC_FIND_TYPE_HEX if is_hex else IDC_FIND_TYPE_TEXT
    win32gui.SendMessage(win32gui.GetDlgItem(dlg, type_id), win32con.BM_CLICK, 0, 0)
    win32gui.SendMessage(win32gui.GetDlgItem(dlg, IDC_FIND_RANGE_ALL), win32con.BM_CLICK, 0, 0)
    time.sleep(0.2)
    return dlg


def _find_next(dlg, text=None):
    if text is not None:
        _set_control_text(win32gui.GetDlgItem(dlg, IDC_FIND_COMBO), text)
        time.sleep(0.2)
    win32gui.PostMessage(dlg, win32con.WM_COMMAND, IDC_FIND_NEXT, 0)
    time.sleep(0.8)


def _close_dialog(drv, dlg):
    if win32gui.IsWindow(dlg):
        win32gui.PostMessage(dlg, win32con.WM_COMMAND, win32con.IDCANCEL, 0)
    _wait_no_dialog(drv)
    time.sleep(0.3)


def _caret_address(drv):
    _wait_no_dialog(drv)
    drv.post_command(ID_JUMP)
    dlg = _visible_dialog_with(drv, IDC_JUMP_HINT_CURRENT)
    try:
        text = win32gui.GetDlgItemText(dlg, IDC_JUMP_HINT_CURRENT).strip()
        m = re.fullmatch(r"[^:：]*[:：]\s*([0-9A-Fa-f]+)", text)
        assert m, "cannot read the current address: %r" % text
        return int(m.group(1), 16)
    finally:
        win32gui.PostMessage(dlg, win32con.WM_COMMAND, win32con.IDCANCEL, 0)
        _wait_no_dialog(drv)


def _clipboard_hex(drv):
    """選択範囲をコピーし、クリップボードの16進テキストを空白なしの大文字で返す。"""
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


class TestIssue233WildcardFind:
    @pytest.mark.ported
    def test_find_dialog_matches_wildcard_and_repeats(self, ported_exe_path, tmp_path):
        """`41??43` で一致し、次検索・前検索がワイルドカード位置を含めて条件を引き継ぐ。"""
        test_file = tmp_path / "wildcard_find.dat"
        #          0     1     2     3     4     5     6     7     8     9
        data = bytes([0x41, 0x10, 0x43, 0x00, 0x41, 0x20, 0x43, 0x41, 0x43, 0xFF])
        test_file.write_bytes(data)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            time.sleep(0.5)

            dlg = _open_find_dialog(drv, is_hex=True)
            _find_next(dlg, "41??43")
            assert _no_message_box(drv, dlg), "valid wildcard pattern was rejected"
            assert _control_text(win32gui.GetDlgItem(dlg, IDC_FIND_COMBO)) == "41 ?? 43", \
                "search text is not normalized"
            _find_next(dlg)   # 2 件目（位置 4）
            _close_dialog(drv, dlg)

            assert _caret_address(drv) == 4, "second wildcard match should be at offset 4"
            assert _clipboard_hex(drv) == "412043", "selection should cover the whole match"

            # 繰り返し検索（前検索）は直前のワイルドカード条件で位置 0 へ戻る。
            #   ワイルドカードが失われて完全一致 41 00 43 を探すと見つからない。
            drv.post_command(ID_FIND_PREV)
            time.sleep(0.8)
            assert _no_message_box(drv, 0), "repeat search reported not found"
            assert _caret_address(drv) == 0, "repeat search should keep the wildcard condition"
            assert _clipboard_hex(drv) == "411043"

            assert test_file.read_bytes() == data, "search must not modify the file"

    @pytest.mark.ported
    @pytest.mark.parametrize("text", ["4?", "?4 41", "41 ? 43", "41 ??? 43", "?? ??", "41 4243"])
    def test_invalid_wildcard_input_is_rejected(self, ported_exe_path, tmp_path, text):
        """ニブル単位の `?`、`?` の個数違い、すべてワイルドカードの入力はエラーになる。"""
        test_file = tmp_path / "wildcard_invalid.dat"
        test_file.write_bytes(bytes([0x41, 0x42, 0x43, 0x44]))

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            time.sleep(0.5)

            dlg = _open_find_dialog(drv, is_hex=True)
            _find_next(dlg, text)
            message = _dismiss_message(drv, dlg)
            assert INVALID_DATA_MESSAGE in message, f"unexpected message for {text!r}: {message!r}"
            assert win32gui.IsWindow(dlg), "find dialog should stay open after the error"
            assert _control_text(win32gui.GetDlgItem(dlg, IDC_FIND_COMBO)) == text, \
                "rejected input must be left as typed"
            _close_dialog(drv, dlg)

    @pytest.mark.ported
    def test_text_search_treats_question_mark_literally(self, ported_exe_path, tmp_path):
        """文字列検索では `?` はワイルドカードではなく文字 0x3F として検索する。"""
        test_file = tmp_path / "wildcard_text.dat"
        test_file.write_bytes(b"abc a?c")

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            time.sleep(0.5)

            dlg = _open_find_dialog(drv, is_hex=False)
            _find_next(dlg, "a?c")
            assert _no_message_box(drv, dlg), "text search reported an error or not found"
            _close_dialog(drv, dlg)

            assert _caret_address(drv) == 4, "'?' in a text search must match only a literal '?'"

    @pytest.mark.ported
    def test_replace_all_with_wildcard_search_data(self, ported_exe_path, tmp_path):
        """[置換] の検索データ（16進）で `??` を使い、一致範囲全体を置換データで置き換える。"""
        test_file = tmp_path / "wildcard_replace.dat"
        test_file.write_bytes(bytes([0x41, 0x10, 0x43, 0x41, 0x20, 0x43, 0x43, 0x41]))
        out_file = tmp_path / "wildcard_replace_out.dat"

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            time.sleep(0.5)
            drv.replace_all_dialog("41 ?? 43", "FF", search_is_hex=True, replace_is_hex=True)
            drv.save_as_via_dialog(out_file)

        assert out_file.read_bytes() == bytes([0xFF, 0xFF, 0x43, 0x41])

    @pytest.mark.ported
    def test_wildcard_in_replace_data_is_rejected(self, ported_exe_path, tmp_path):
        """置換データでは `??` は使えず、エラーになってデータは変わらない。"""
        data = bytes([0x41, 0x42, 0x41, 0x43])
        test_file = tmp_path / "wildcard_replace_invalid.dat"
        test_file.write_bytes(data)
        out_file = tmp_path / "wildcard_replace_invalid_out.dat"

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            time.sleep(0.5)

            _wait_no_dialog(drv)
            drv.post_command(CMD_EDIT_REPLACE)
            dlg = _visible_dialog_with(drv, IDC_REPL_REPLACE_COMBO)
            win32gui.SendMessage(win32gui.GetDlgItem(dlg, IDC_REPL_RANGE_ALL), win32con.BM_CLICK, 0, 0)
            _set_control_text(win32gui.GetDlgItem(dlg, IDC_REPL_SEARCH_COMBO), "41")
            _set_control_text(win32gui.GetDlgItem(dlg, IDC_REPL_REPLACE_COMBO), "??")
            time.sleep(0.2)
            win32gui.PostMessage(dlg, win32con.WM_COMMAND, IDC_REPL_ALL, 0)
            time.sleep(0.5)

            message = _dismiss_message(drv, dlg)
            assert INVALID_DATA_MESSAGE in message, f"unexpected message: {message!r}"
            assert win32gui.IsWindow(dlg), "replace dialog should stay open after the error"
            _close_dialog(drv, dlg)

            # 文書の内容を別名で書き出し、置換が実行されていないことを確かめる。
            drv.save_as_via_dialog(out_file)

        assert out_file.read_bytes() == data, "rejected replace must not modify the document"
