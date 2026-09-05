"""Issue #173: キャラクターセット Unicode で CP932 外の文字も表示できるようにする。

原版は UTF-16 のコード単位を 1 つずつ CP932 へ変換して描画するため、ハングルや
キリル文字は `..`、サロゲートペアも表示できなかった。移植版では UTF-8 対応
（Issue #98 / #107）と同じくワイド描画へ移行する。

原版に同じ表示は無いためゴールデン比較は行わない。確認する内容:
  - CP932 に無い文字がダンプ保存（＝画面と同じセル割り当て）に現れる
  - サロゲートペアが 1 文字として扱われる
  - ペアになっていないサロゲートは '.' になる
  - CP932 に無い文字で検索できる
  - 文字ペインへの入力が UTF-16 で書き込まれる
  - 文字欄の描画が Shift-JIS 表示と異なる
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
    ID_CHARSET_UNICODE,
    ID_JUMP,
    safe_set_focus,
)

PORT_REC0 = r"Software\StirHex\StirHex\Rec0"

ID_EDIT_FIND = 57636          # MFC 標準 ID_EDIT_FIND
IDC_FIND_TYPE_TEXT = 1017     # 検索データ種別: 文字列
IDC_FIND_RANGE_TOP = 1019     # 検索範囲: 先頭から
IDC_FIND_COMBO = 1026         # 検索データ入力コンボ
IDC_FIND_NEXT = 1042          # 次検索ボタン
IDC_JUMP_HINT_CURRENT = 1019  # ジャンプダイアログの「現在アドレス」表示

# 日本語・ハングル・キリル文字・サロゲートペアを含む UTF-16LE データ。
SAMPLE_TEXT = "ABC あいう 한국어 Жд \U0001F600 end"
SAMPLE = SAMPLE_TEXT.encode("utf-16-le")


def _statusbar_texts(drv):
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
    """検索ダイアログで文字列を先頭から前方検索する（Issue #98 のテストと同じ手順）。"""
    _wait_no_dialog(drv)
    win32gui.PostMessage(drv.hwnd, win32con.WM_COMMAND, ID_EDIT_FIND, 0)
    dlg = _visible_dialog_with(drv, IDC_FIND_COMBO)
    win32gui.SendMessage(win32gui.GetDlgItem(dlg, IDC_FIND_TYPE_TEXT), win32con.BM_CLICK, 0, 0)
    win32gui.SendMessage(win32gui.GetDlgItem(dlg, IDC_FIND_RANGE_TOP), win32con.BM_CLICK, 0, 0)
    time.sleep(0.2)
    win32gui.SendMessage(win32gui.GetDlgItem(dlg, IDC_FIND_COMBO), win32con.WM_SETTEXT, 0, text)
    time.sleep(0.2)
    win32gui.PostMessage(dlg, win32con.WM_COMMAND, IDC_FIND_NEXT, 0)
    time.sleep(1.0)
    if win32gui.IsWindow(dlg):
        win32gui.PostMessage(dlg, win32con.WM_CLOSE, 0, 0)
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


@contextlib.contextmanager
def _default_charset(value):
    with settings_value(PORT_REC0, "CharSet", value):
        yield


def _dump_text(drv, dump_file):
    """ダンプ保存の本文を返す。文字欄は画面と同じセル割り当てで書き出される。"""
    drv.save_dump_via_dialog(dump_file)
    return dump_file.read_bytes().decode("utf-8")


# Issue #42 のゴールデン比較で使っていた 64 バイト。文字欄の分岐を網羅するデータで、
#   UTF-16 として読むと大半が CP932 に無い文字になる。Issue #173 で移植版の表示が原版と
#   意図的に変わったため、原版との突合はやめて移植版の出力そのものをここで固定する
#   （test_issue_42_char_pane.py から移した検査）。
CHAR_PANE_DATA = bytes([
    0x41, 0x42, 0x43, 0x82, 0xA0, 0x8A, 0xBF, 0xB1, 0xB2, 0x82, 0x20, 0x82, 0x7F, 0x00, 0x1F, 0x82,
    0xA2, 0xE0, 0x40, 0xA1, 0xDF, 0xFD, 0xFE, 0xFF, 0x20, 0x7E, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35,
    0xA4, 0xA2, 0xA4, 0xA4, 0x8E, 0xB1, 0xA4, 0x20, 0x41, 0x42, 0xA1, 0xA1, 0xFE, 0xFE, 0xA4, 0x0A,
    0x42, 0x30, 0x44, 0x30, 0x41, 0x00, 0xAC, 0x20, 0x3D, 0xD8, 0x00, 0xDE, 0x42, 0x00, 0x0A, 0x00,
])

# 上のデータを Unicode（UTF-16LE）で表示したときの文字欄。各行 16 バイト = 16 セル
#   （＋行末の空白 1 つ）。二倍幅グリフは 2 セル、一倍幅グリフは 1 セル + 空白 1 セル、
#   制御文字は '.' + 空白、サロゲートペアは 4 セル（グリフ 2 + 空白 2）を占める。
EXPECTED_CHAR_FIELDS = [
    "䉁艃誠놿芲舠. 舟 ",
    "ꅀ ﷟ ￾ 縠㄰ ㌲㔴 ",
    "ꊤ ꒤ 놎₤ 䉁ꆡ ﻾ ત  ",
    "あいA € 😀  B .  ",
]


# ダンプ 1 行の文字欄が始まる桁。アドレス欄 11 桁 ＋ 16進欄（16*3-1 桁）＋ 区切り 3 桁。
#   範囲指定のダンプでは 16進欄にも空白が入るため、区切りを探さず固定桁で切り出す。
CHAR_FIELD_COLUMN = 11 + (16 * 3 - 1) + 3


def _char_fields(text):
    """ダンプ本文から各データ行の文字欄だけを取り出す。"""
    fields = []
    for line in text.split("\r\n"):
        if not line.startswith(" 000000"):
            continue
        fields.append(line[CHAR_FIELD_COLUMN:])
    return fields


@pytest.mark.ported
class TestIssue173CharsetUnicode:

    def test_dump_matches_char_pane_sample(self, ported_exe_path, tmp_path):
        """文字欄の分岐を網羅するデータで、移植版の文字欄が期待どおりであること。

        Issue #42 のゴールデン比較（原版との突合）から Unicode を外した代わりに、
        移植版自身の出力を固定する。桁揃え（1 バイト = 1 セル）と、CP932 を経由しない
        表示の両方をここで押さえる。
        """
        test_file = tmp_path / "utf16_sample_all.bin"
        dump_file = tmp_path / "utf16_sample_all.txt"
        test_file.write_bytes(CHAR_PANE_DATA)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)
            drv.post_command(ID_CHARSET_UNICODE)
            time.sleep(0.5)
            text = _dump_text(drv, dump_file)

        assert _char_fields(text) == EXPECTED_CHAR_FIELDS, text

    def test_dump_range_from_odd_offset(self, ported_exe_path, tmp_path):
        """奇数オフセット（コード単位の 2 バイト目）から始まる範囲でも桁が揃うこと。

        Issue #42 のゴールデン比較 unicode-odd-offset を移したもの。範囲外の先頭列と、
        読み飛ばす半端な 1 バイトが、それぞれ空白 1 セルになる。
        """
        test_file = tmp_path / "utf16_odd.bin"
        dump_file = tmp_path / "utf16_odd.txt"
        test_file.write_bytes(CHAR_PANE_DATA)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)
            drv.post_command(ID_CHARSET_UNICODE)
            time.sleep(0.5)
            drv.select_range_dialog(start_addr="31", end_addr="3F", is_hex=True)
            time.sleep(0.3)
            text = _dump_text(drv, dump_file)

        fields = _char_fields(text)
        assert len(fields) == 1, text
        # 先頭 2 セルは「範囲外の 0x30」と「読み飛ばした 0x31」。以降は 0x32 から。
        assert fields[0] == "  いA € 😀  B .  ", ascii(fields[0])

    def test_surrogate_pairs_survive_odd_line_size(self, ported_exe_path, tmp_path):
        """1 行バイト数が奇数でも、行の境界でサロゲートペアが割れないこと。

        1 行バイト数は 2..256 で奇数も設定できる。奇数だと行末がコード単位の途中に
        なるため、ペアが行をまたぐ位置関係が偶数のときと変わる。

        なお、レビューで指摘された先読み不足（WideReadAhead）は画面と印刷の経路の
        ものなので、ダンプ保存を見るこのテストでは再現しない（ダンプは以前から
        kCharLookahead=3 を使っていた）。修正では両者を同じ定数に統一した。
        """
        pairs = 20
        test_file = tmp_path / "utf16_odd_linesize.bin"
        dump_file = tmp_path / "utf16_odd_linesize.txt"
        test_file.write_bytes("😀".encode("utf-16-le") * pairs)

        with settings_value(PORT_REC0, "LineSize", 15):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                safe_set_focus(drv.hwnd)
                time.sleep(0.3)
                drv.post_command(ID_CHARSET_UNICODE)
                time.sleep(0.5)
                text = _dump_text(drv, dump_file)

        assert text.count("😀") == pairs, (
            "%d/%d surrogate pairs survived:\n%s" % (text.count("😀"), pairs, text)
        )

    def test_dump_shows_characters_outside_cp932(self, ported_exe_path, tmp_path):
        """CP932 に無い文字が '..' に潰れず、そのまま現れること（この Issue の主眼）。"""
        test_file = tmp_path / "utf16_outside.bin"
        dump_file = tmp_path / "utf16_outside.txt"
        test_file.write_bytes(SAMPLE)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)
            drv.post_command(ID_CHARSET_UNICODE)
            time.sleep(0.5)
            text = _dump_text(drv, dump_file)

        # 2 バイト = 2 セル。二倍幅グリフはそのまま 2 セルを占める。
        assert "한국어" in text, text
        # キリル文字も表示できる。セル数はフォントに実測させるため、この字が
        #   一倍幅か二倍幅かはフォント次第（既定の ＭＳ ゴシックでは二倍幅）。
        assert "Ж" in text and "д" in text, text
        # 日本語（CP932 にもある文字）は従来どおり表示される。
        assert "あいう" in text, text

    def test_dump_keeps_surrogate_pair_as_one_character(self, ported_exe_path, tmp_path):
        """サロゲートペア（4 バイト）が 1 文字として復号されること。"""
        test_file = tmp_path / "utf16_emoji.bin"
        dump_file = tmp_path / "utf16_emoji.txt"
        test_file.write_bytes("\U0001F600".encode("utf-16-le"))

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)
            drv.post_command(ID_CHARSET_UNICODE)
            time.sleep(0.5)
            text = _dump_text(drv, dump_file)

        assert "\U0001F600" in text, text

    def test_dump_marks_unpaired_surrogate_with_dots(self, ported_exe_path, tmp_path):
        """ペアになっていないサロゲートは '.' になり、桁が崩れないこと。"""
        test_file = tmp_path / "utf16_lone.bin"
        dump_file = tmp_path / "utf16_lone.txt"
        # 上位サロゲート単独 + "AB"
        test_file.write_bytes(b"\x3d\xd8" + "AB".encode("utf-16-le"))

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)
            drv.post_command(ID_CHARSET_UNICODE)
            time.sleep(0.5)
            text = _dump_text(drv, dump_file)

        assert "\U0001F600" not in text, text
        # 単独サロゲート 2 バイトが '..'、続く "A"/"B" が各 1 セル + 空白。
        assert "..A B" in text, text

    def test_search_finds_text_outside_cp932(self, ported_exe_path, tmp_path):
        """CP932 に無い文字（ハングル）でも検索できること（ワイドから直接 UTF-16 へ）。"""
        test_file = tmp_path / "utf16_search.bin"
        test_file.write_bytes(SAMPLE)
        expected = SAMPLE.index("한국어".encode("utf-16-le"))

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)
            drv.post_command(ID_CHARSET_UNICODE)
            time.sleep(0.5)

            _find_text_from_top(drv, "한국어")
            pos = _caret_address(drv)

        assert pos == expected, "caret is at 0x%X, expected 0x%X" % (pos, expected)

    def test_char_input_writes_utf16_bytes(self, ported_exe_path, tmp_path):
        """文字ペインへの入力が UTF-16 で書き込まれること（CP932 を経由しない）。"""
        test_file = tmp_path / "utf16_input.bin"
        out_file = tmp_path / "utf16_input_out.bin"
        test_file.write_bytes(b"")

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)
            drv.post_command(ID_CHARSET_UNICODE)
            time.sleep(0.5)
            drv.press_tab()          # 文字ペインへ
            time.sleep(0.2)
            drv.type_ime_chars("한")  # CP932 に無い文字
            time.sleep(0.3)
            drv.save_as_via_dialog(out_file)

        # 格納バイト順は原版同様つねにリトルエンディアン。
        assert out_file.read_bytes() == "한".encode("utf-16-le"), out_file.read_bytes().hex()

    def test_char_pane_rendering_differs_from_sjis(self, ported_exe_path, tmp_path):
        """Unicode の文字欄は Shift-JIS 表示と異なる（UTF-16 として解釈されている）。"""
        test_file = tmp_path / "utf16_render.bin"
        test_file.write_bytes(SAMPLE)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)

            drv.post_command(ID_CHARSET_SJIS)
            time.sleep(0.6)
            sjis_px = drv.capture_view_pixels()

            drv.post_command(ID_CHARSET_UNICODE)
            time.sleep(0.6)
            uni_px = drv.capture_view_pixels()

        assert sjis_px and uni_px, "could not capture the view"
        assert len(sjis_px) == len(uni_px), "capture size changed between charsets"
        assert sjis_px != uni_px, "the char pane did not change when switching to Unicode"

    def test_default_charset_setting_is_honored(self, ported_exe_path, tmp_path):
        """既定文字セットの設定値 3（Unicode）でファイルが開かれること（非退行）。"""
        test_file = tmp_path / "utf16_default.bin"
        test_file.write_bytes(SAMPLE)

        with _default_charset(3):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                safe_set_focus(drv.hwnd)
                time.sleep(0.5)
                texts = _statusbar_texts(drv)

        assert any("UNICODE" in t.upper() for t in texts), "status bar: %r" % (texts,)
