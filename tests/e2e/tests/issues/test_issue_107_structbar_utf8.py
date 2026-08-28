"""Issue #107: 構造体編集バーの文字配列表示を UTF-8 に対応させる（移植版のみ）。

構造体編集バーの char / byte 配列の値は、これまで CP932 バイト列へ写してから
表示していたため、CP932 に無い文字（ハングルなど）が `.` になっていた。
キャラクターセットが UTF-8 のときは復号したワイド文字をそのまま表示する。

原版に UTF-8 は無いためゴールデン比較は行わない。
"""

import time

import pytest

from drivers.stirling_driver import (
    StirlingDriver,
    ID_CHARSET_SJIS,
    ID_CHARSET_UTF8,
    safe_set_focus,
)

# Struct.def の LOGFONT: LONG×5 (20) + BYTE×8 (8) = 28 バイトの後ろが lfFaceName[32]。
FACE_OFFSET = 28
FACE_SIZE = 32
LOGFONT_SIZE = 60


def _logfont_with(face_bytes):
    """lfFaceName に face_bytes を置いた LOGFONT 1 レコード分のデータ。"""
    assert len(face_bytes) <= FACE_SIZE
    data = bytearray(LOGFONT_SIZE)
    data[FACE_OFFSET:FACE_OFFSET + len(face_bytes)] = face_bytes
    return bytes(data)


def _face_value(drv):
    """構造体編集バーの lfFaceName 行の値セルを返す。"""
    for type_name, name, value in drv.get_struct_list_texts():
        if name.startswith("lfFaceName"):
            return value
    raise AssertionError("lfFaceName row not found in the struct bar")


def _open_with_face(drv, exe_path, test_file, charset_cmd):
    drv.start(test_file)
    safe_set_focus(drv.hwnd)
    time.sleep(0.3)
    drv.post_command(charset_cmd)
    time.sleep(0.3)
    drv.toggle_struct_bar(show=True)
    time.sleep(0.4)
    drv.select_struct_type("LOGFONT")
    time.sleep(0.5)


@pytest.mark.ported
class TestIssue107StructBarUtf8:

    def test_utf8_char_array_keeps_characters_outside_cp932(self, ported_exe_path, tmp_path):
        """UTF-8 では CP932 に無い文字も値セルに残ること（この Issue の主眼）。"""
        test_file = tmp_path / "structbar_utf8.dat"
        test_file.write_bytes(_logfont_with("한국어".encode("utf-8")))

        with StirlingDriver(ported_exe_path) as drv:
            _open_with_face(drv, ported_exe_path, test_file, ID_CHARSET_UTF8)
            value = _face_value(drv)

        assert value.startswith("한국어"), "value cell: %r" % value

    def test_utf8_char_array_shows_japanese(self, ported_exe_path, tmp_path):
        """UTF-8 の日本語もそのまま表示されること。"""
        test_file = tmp_path / "structbar_utf8_ja.dat"
        test_file.write_bytes(_logfont_with("あいう".encode("utf-8")))

        with StirlingDriver(ported_exe_path) as drv:
            _open_with_face(drv, ported_exe_path, test_file, ID_CHARSET_UTF8)
            value = _face_value(drv)

        assert value.startswith("あいう"), "value cell: %r" % value

    def test_utf8_broken_sequence_becomes_dots(self, ported_exe_path, tmp_path):
        """不正な列は 1 バイト = 1 文字の '.' になること。"""
        test_file = tmp_path / "structbar_utf8_broken.dat"
        test_file.write_bytes(_logfont_with(b"\xE3\x81" + b"A"))

        with StirlingDriver(ported_exe_path) as drv:
            _open_with_face(drv, ported_exe_path, test_file, ID_CHARSET_UTF8)
            value = _face_value(drv)

        assert value.startswith("..A"), "value cell: %r" % value

    def test_sjis_display_is_unchanged(self, ported_exe_path, tmp_path):
        """Shift-JIS の表示は従来どおり（値列のワイド化で退行していないこと）。"""
        test_file = tmp_path / "structbar_sjis.dat"
        test_file.write_bytes(_logfont_with("あいう".encode("cp932")))

        with StirlingDriver(ported_exe_path) as drv:
            _open_with_face(drv, ported_exe_path, test_file, ID_CHARSET_SJIS)
            value = _face_value(drv)

        assert value.startswith("あいう"), "value cell: %r" % value

    def test_scalar_values_are_unchanged(self, ported_exe_path, tmp_path):
        """スカラ値（数値表記）の表示が変わらないこと。"""
        data = bytearray(_logfont_with(b"A"))
        data[0:4] = (12345).to_bytes(4, "little")     # lfHeight
        test_file = tmp_path / "structbar_scalar.dat"
        test_file.write_bytes(bytes(data))

        with StirlingDriver(ported_exe_path) as drv:
            _open_with_face(drv, ported_exe_path, test_file, ID_CHARSET_UTF8)
            rows = drv.get_struct_list_texts()

        values = {name: value for _type, name, value in rows}
        assert values.get("lfHeight") == "12345", "rows: %r" % (rows,)
