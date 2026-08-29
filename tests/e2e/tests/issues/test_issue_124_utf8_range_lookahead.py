"""Issue #124: UTF-8 の指定範囲ダンプで、範囲外の後続バイトを文字欄へ出さない。

UTF-8 用の先読みが指定範囲ではなく文書末尾まで許可されていたため、範囲終端が
有効な UTF-8 シーケンスの途中にあると、BuildCharCellsUtf8 が範囲外のバイトまで
消費して完全な文字を描いていた。16進欄は範囲内のバイトしか出さないので、両欄の
内容が食い違う。
"""

import time

import pytest

from drivers.stirling_driver import (
    StirlingDriver,
    ID_CHARSET_UTF8,
    safe_set_focus,
)

# 2/3/4 バイトの UTF-8 シーケンスを 1 バイト文字で挟んだデータ。
#   00      : 'A'
#   01..02  : C3 A9        -> é   (2 バイト)
#   03      : 'B'
#   04..06  : E3 81 82     -> あ  (3 バイト)
#   07      : 'C'
#   08..0B  : F0 9F 98 80  -> 😀  (4 バイト)
#   0C      : 'D'
SAMPLE = (b"A" + "é".encode("utf-8") + b"B" + "あ".encode("utf-8")
          + b"C" + "😀".encode("utf-8") + b"D")

MULTIBYTE_CHARS = ("é", "あ", "😀")

# (範囲終端, 途切れる文字) — 各シーケンスのあらゆる途中位置で範囲を切る。
TRUNCATING_ENDS = [
    (0x01, "é"),
    (0x04, "あ"),
    (0x05, "あ"),
    (0x08, "😀"),
    (0x09, "😀"),
    (0x0A, "😀"),
]


def _dump_range(drv: StirlingDriver, dump_file, end_addr: int) -> str:
    """0 から end_addr（両端含む）を選択してダンプ保存し、本文を返す。"""
    drv.select_range_dialog("0", "%X" % end_addr)
    drv.save_dump_via_dialog(dump_file)
    return dump_file.read_bytes().decode("utf-8")


def _hex_bytes_in_dump(text: str) -> list[int]:
    """ダンプの16進欄に現れるバイト列を取り出す（ヘッダと区切り線は除く）。"""
    out: list[int] = []
    for line in text.splitlines():
        if not line.startswith(" ") or line.startswith(" ADDRESS"):
            continue
        body = line[11:11 + 16 * 3]
        for i in range(0, len(body), 3):
            cell = body[i:i + 2]
            if cell.strip():
                out.append(int(cell, 16))
    return out


class TestIssue124Utf8RangeLookahead:
    @pytest.mark.ported
    def test_dump_range_does_not_emit_bytes_past_the_end(
        self, ported_exe_path, tmp_path
    ):
        test_file = tmp_path / "issue124_utf8.bin"
        test_file.write_bytes(SAMPLE)
        dump_file = tmp_path / "issue124_dump.txt"

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)
            drv.post_command(ID_CHARSET_UTF8)
            time.sleep(0.5)

            # 正常系: 範囲がシーケンス全体を含むときは従来どおり文字が出る。
            whole = _dump_range(drv, dump_file, len(SAMPLE) - 1)
            for ch in MULTIBYTE_CHARS:
                assert ch in whole, "whole-range dump lost %r:\n%s" % (ch, whole)
            assert _hex_bytes_in_dump(whole) == list(SAMPLE)

            for end_addr, cut_char in TRUNCATING_ENDS:
                text = _dump_range(drv, dump_file, end_addr)
                # 途切れた文字は範囲外バイトから復元してはならない。
                assert cut_char not in text, (
                    "end=%02X leaked %r from outside the range:\n%s"
                    % (end_addr, cut_char, text)
                )
                # 16進欄と文字欄の対象範囲が一致すること（範囲外バイトを出さない）。
                assert _hex_bytes_in_dump(text) == list(SAMPLE[:end_addr + 1]), (
                    "end=%02X hex column mismatch:\n%s" % (end_addr, text)
                )
                # 範囲内に完全に収まっている文字は残る。
                for ch in MULTIBYTE_CHARS:
                    if ch.encode("utf-8") in SAMPLE[:end_addr + 1]:
                        assert ch in text, (
                            "end=%02X dropped in-range %r:\n%s"
                            % (end_addr, ch, text)
                        )
