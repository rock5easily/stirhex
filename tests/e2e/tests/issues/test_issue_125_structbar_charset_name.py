"""Issue #125: 構造体編集バーの文字セット表示欄に UTF-8 を出す。

`CStructBar::UpdateStructStatus` の文字セット名の表が 0..5 の 6 要素しか無く、
UTF-8（値 6）を選ぶと表示名を解決できずに欄が空になっていた。表示名は
`ui::CharsetNameW`（文字列リソース 6040-6046）へ集約した。
"""

import time

import pytest

from drivers.stirling_driver import (
    StirlingDriver,
    ID_CHARSET_ASCII,
    ID_CHARSET_EBCDIC,
    ID_CHARSET_EBCIDK,
    ID_CHARSET_EUC,
    ID_CHARSET_SJIS,
    ID_CHARSET_UNICODE,
    ID_CHARSET_UTF8,
    safe_set_focus,
)

# (コマンドID, 期待する表示名)。既存6種＋UTF-8。
CHARSETS = [
    (ID_CHARSET_ASCII, "ASCII"),
    (ID_CHARSET_SJIS, "SHIFT-JIS"),
    (ID_CHARSET_EUC, "EUC"),
    (ID_CHARSET_UNICODE, "Unicode"),
    (ID_CHARSET_UTF8, "UTF-8"),
    (ID_CHARSET_EBCDIC, "EBCDIC"),
    (ID_CHARSET_EBCIDK, "EBCIDK"),
]


def _charset_status(drv: StirlingDriver) -> str:
    return drv.struct_status_texts().get("charset", "")


@pytest.mark.ported
class TestIssue125StructBarCharsetName:

    def test_struct_bar_shows_every_charset_name(self, ported_exe_path, tmp_path):
        test_file = tmp_path / "issue125.dat"
        test_file.write_bytes(bytes(range(0x40)))

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)
            drv.toggle_struct_bar(show=True)
            time.sleep(0.5)

            for cmd, expected in CHARSETS:
                drv.post_command(cmd)
                time.sleep(0.4)
                assert _charset_status(drv) == expected, (
                    "charset %d showed %r" % (cmd, _charset_status(drv))
                )

    def test_utf8_name_survives_timer_updates(self, ported_exe_path, tmp_path):
        """タイマーによる定期更新後も UTF-8 の表示が消えないこと。"""
        test_file = tmp_path / "issue125_timer.dat"
        test_file.write_bytes("あいうえお".encode("utf-8"))

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)
            drv.post_command(ID_CHARSET_UTF8)
            time.sleep(0.3)
            drv.toggle_struct_bar(show=True)
            time.sleep(0.5)

            assert _charset_status(drv) == "UTF-8"
            # 構造体バーのステータスはタイマーで再描画される。数周期ぶん待つ。
            time.sleep(2.0)
            assert _charset_status(drv) == "UTF-8"
