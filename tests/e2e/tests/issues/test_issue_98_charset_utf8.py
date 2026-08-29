"""Issue #98: キャラクターセット UTF-8 対応（移植版のみ）。

原版に UTF-8 は無いためゴールデン比較は行わない。確認する内容:
  - 文字セットを UTF-8 へ切り替えられ、ステータスバーに反映される
  - 文字欄が UTF-8 として描画される（Shift-JIS 表示と異なる）
  - CP932 に無い文字（ハングル）を含む文字列で検索できる
  - 文字ペインへの入力が UTF-8 で書き込まれる
  - ダンプ保存の文字欄が UTF-8 で出力される
  - 既定文字セット（設定値 6）が反映される
"""

import contextlib
import re
import time

import pytest
import win32con
import win32gui
from pywinauto import timings

from drivers.settings_context import settings_value
from drivers.stirling_driver import (
    StirlingDriver,
    ID_CHARSET_SJIS,
    ID_CHARSET_UTF8,
    ID_JUMP,
    safe_set_focus,
)

# 既定文字セットは拡張子別設定のレコード側に入る（環境設定の Env ではない）。
# 拡張子別設定 Rec0（既定レコード）。StirHex は設定ファイルへ保存するため、この表記は
# settings_context がセクション名へ読み替える（Issue #96）。
PORT_REC0 = r"Software\StirHex\StirHex\Rec0"

ID_EDIT_FIND = 57636          # MFC 標準 ID_EDIT_FIND（drivers の CMD_EDIT_FIND は別コマンド）
IDC_FIND_TYPE_TEXT = 1017     # 検索データ種別: 文字列
IDC_FIND_RANGE_TOP = 1019     # 検索範囲: 先頭から
IDC_FIND_COMBO = 1026         # 検索データ入力コンボ
IDC_FIND_NEXT = 1042          # 次検索ボタン
IDC_JUMP_HINT_CURRENT = 1019  # ジャンプダイアログの「現在アドレス」表示

# 日本語・ハングル・簡体字・ラテン拡張を含む UTF-8 データ。
SAMPLE_TEXT = "ABC あいうえお 한국어 中文简体 éàü"
SAMPLE = SAMPLE_TEXT.encode("utf-8")


def _statusbar_texts(drv):
    """ステータスバーの全パートの文字列（driver 側のプロセス跨ぎ対応版を使う）。"""
    return drv.get_all_statusbar_text()


def _wait_no_dialog(drv, timeout=10.0):
    def _check():
        for h, cls, _title in drv._get_process_windows():
            if cls == "#32770" and win32gui.IsWindowVisible(h):
                raise RuntimeError("dialog still open")
        return True

    timings.wait_until_passes(timeout, 0.2, _check)


def _visible_dialog_with(drv, control_id):
    def _find():
        for h, cls, _title in drv._get_process_windows():
            if cls == "#32770" and win32gui.IsWindowVisible(h):
                if win32gui.GetDlgItem(h, control_id):
                    return h
        raise RuntimeError("dialog not found yet")

    return timings.wait_until_passes(10, 0.2, _find)


def _find_text_from_top(drv, text):
    """検索ダイアログで文字列を先頭から前方検索する。"""
    _wait_no_dialog(drv)
    win32gui.PostMessage(drv.hwnd, win32con.WM_COMMAND, ID_EDIT_FIND, 0)
    dlg = _visible_dialog_with(drv, IDC_FIND_COMBO)
    win32gui.SendMessage(win32gui.GetDlgItem(dlg, IDC_FIND_TYPE_TEXT), win32con.BM_CLICK, 0, 0)
    win32gui.SendMessage(win32gui.GetDlgItem(dlg, IDC_FIND_RANGE_TOP), win32con.BM_CLICK, 0, 0)
    time.sleep(0.2)
    # WM_SETTEXT はシステムがプロセス間をマーシャリングするのでコンボへ直接渡せる。
    win32gui.SendMessage(win32gui.GetDlgItem(dlg, IDC_FIND_COMBO), win32con.WM_SETTEXT, 0, text)
    time.sleep(0.2)
    win32gui.PostMessage(dlg, win32con.WM_COMMAND, IDC_FIND_NEXT, 0)
    time.sleep(1.0)
    if win32gui.IsWindow(dlg):
        # 検索後もダイアログは開いたまま（原と同じ）。閉じてキャレットを読める状態に戻す。
        win32gui.PostMessage(dlg, win32con.WM_CLOSE, 0, 0)
    _wait_no_dialog(drv)
    time.sleep(0.3)


def _caret_address(drv):
    """ジャンプダイアログの「現在アドレス」からキャレット位置を読む。"""
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



@contextlib.contextmanager
def _default_charset(value):
    """既定文字セット（拡張子別設定 Rec0 の CharSet）を一時的に差し替える。"""
    with settings_value(PORT_REC0, "CharSet", value):
        yield


@pytest.mark.ported
class TestIssue98CharsetUtf8:

    def test_charset_switches_and_statusbar_shows_utf8(self, ported_exe_path, tmp_path):
        """[設定]→[キャラクターセット]→[UTF-8] でステータスバーの表示が変わること。"""
        test_file = tmp_path / "utf8_status.bin"
        test_file.write_bytes(SAMPLE)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)

            drv.post_command(ID_CHARSET_SJIS)
            time.sleep(0.5)
            before = _statusbar_texts(drv)

            drv.post_command(ID_CHARSET_UTF8)
            time.sleep(0.5)
            after = _statusbar_texts(drv)

        assert "SHIFT-JIS" in before, "status bar before: %r" % (before,)
        assert "UTF-8" in after, "status bar after: %r" % (after,)

    def test_char_pane_rendering_differs_from_sjis(self, ported_exe_path, tmp_path):
        """UTF-8 の文字欄は Shift-JIS 表示と異なる（UTF-8 として解釈されている）。"""
        test_file = tmp_path / "utf8_render.bin"
        test_file.write_bytes(SAMPLE)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)

            drv.post_command(ID_CHARSET_SJIS)
            time.sleep(0.6)
            sjis_px = drv.capture_view_pixels()

            drv.post_command(ID_CHARSET_UTF8)
            time.sleep(0.6)
            utf8_px = drv.capture_view_pixels()

        assert sjis_px and utf8_px, "could not capture the view"
        assert len(sjis_px) == len(utf8_px), "capture size changed between charsets"
        assert sjis_px != utf8_px, "the char pane did not change when switching to UTF-8"

    def test_search_finds_text_outside_cp932(self, ported_exe_path, tmp_path):
        """CP932 に無い文字（ハングル）でも検索できる（ワイドから直接 UTF-8 へ符号化）。"""
        test_file = tmp_path / "utf8_search.bin"
        test_file.write_bytes(SAMPLE)
        expected = SAMPLE.index("한국어".encode("utf-8"))

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)
            drv.post_command(ID_CHARSET_UTF8)
            time.sleep(0.5)

            _find_text_from_top(drv, "한국어")
            pos = _caret_address(drv)

        assert pos == expected, "caret is at 0x%X, expected 0x%X" % (pos, expected)

    def test_char_input_writes_utf8_bytes(self, ported_exe_path, tmp_path):
        """文字ペインへの入力が UTF-8 で書き込まれること（CP932 を経由しない）。"""
        test_file = tmp_path / "utf8_input.bin"
        out_file = tmp_path / "utf8_input_out.bin"
        test_file.write_bytes(b"")

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)
            drv.post_command(ID_CHARSET_UTF8)
            time.sleep(0.5)
            drv.press_tab()          # 文字ペインへ
            time.sleep(0.2)
            drv.type_ime_chars("あ")
            time.sleep(0.3)
            drv.save_as_via_dialog(out_file)

        assert out_file.read_bytes() == "あ".encode("utf-8"), out_file.read_bytes().hex()

    def test_dump_save_writes_utf8(self, ported_exe_path, tmp_path):
        """ダンプ保存の文字欄が UTF-8 で書き出されること。"""
        test_file = tmp_path / "utf8_dump.bin"
        dump_file = tmp_path / "utf8_dump.txt"
        test_file.write_bytes(SAMPLE)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)
            drv.post_command(ID_CHARSET_UTF8)
            time.sleep(0.5)
            drv.save_dump_via_dialog(dump_file)

        raw = dump_file.read_bytes()
        text = raw.decode("utf-8")            # 復号できること自体が UTF-8 出力の確認
        # 文字欄は画面と同じセル割り当てで書き出す。3 バイト文字は 3 セルを占め、
        #   二倍幅グリフが 2 セル、余りの 1 セルが空白になるため 1 文字ごとに空白が入る。
        assert "ABC あ い う え" in text, text
        assert "한 국 어" in text, text       # CP932 に無い文字もそのまま出力される

    def test_default_charset_setting_is_honored(self, ported_exe_path, tmp_path):
        """既定文字セットの設定値 6（UTF-8）でファイルが開かれること。"""
        test_file = tmp_path / "utf8_default.bin"
        test_file.write_bytes(SAMPLE)

        with _default_charset(6):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                safe_set_focus(drv.hwnd)
                time.sleep(0.5)
                texts = _statusbar_texts(drv)

        assert "UTF-8" in texts, "status bar: %r" % (texts,)
