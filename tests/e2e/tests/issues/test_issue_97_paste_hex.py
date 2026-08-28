"""Issue #97: 16進テキスト貼り付け（移植版のみ）。

Windows クリップボードのテキストを16進表記として読み取り、バイトデータとして
貼り付ける。原版に対応する機能が無いためゴールデン比較は行わない。

確認する内容:
  - 挿入（選択なし・既定設定）
  - 選択範囲の置換
  - 寛容な書式（0x 接頭辞 / カンマ / CRLF / 前後の余分な区切り）
  - 不正なテキストはメッセージを出してデータを変更しない
"""

import time
import pytest
import win32clipboard

from drivers.stirling_driver import StirlingDriver

IDOK = 1

TEST_DATA = bytes(range(16))


def _open_clipboard():
    """他プロセスがロックしている間は待って開く。"""
    last_error = None
    for _ in range(20):
        try:
            win32clipboard.OpenClipboard()
            return
        except Exception as exc:
            last_error = exc
            time.sleep(0.1)
    raise AssertionError(f"Could not open the clipboard: {last_error}")


def _set_clipboard_text(text: str):
    _open_clipboard()
    try:
        win32clipboard.EmptyClipboard()
        # SetClipboardData へ str を直接渡すとヒープを壊すため、テキスト用の API を使う。
        win32clipboard.SetClipboardText(text, win32clipboard.CF_UNICODETEXT)
    finally:
        win32clipboard.CloseClipboard()


@pytest.mark.ported
class TestIssue97PasteHexText:

    def _run(self, exe_path, tmp_path, name, clipboard_text, action=None):
        """クリップボードへ text を置き、16進テキスト貼り付けを実行して保存内容を返す。"""
        test_file = tmp_path / f"{name}.dat"
        out_file = tmp_path / f"{name}_out.dat"
        test_file.write_bytes(TEST_DATA)

        _set_clipboard_text(clipboard_text)

        with StirlingDriver(exe_path) as drv:
            drv.start(test_file)
            drv.focus_view()
            time.sleep(0.3)
            if action is not None:
                action(drv)
            # 解析に失敗するとモーダルのメッセージボックスが出るため、
            # プロセス跨ぎの SendMessage ではなく PostMessage 経由で実行する。
            drv.paste_hex()
            time.sleep(0.5)
            drv.save_as_via_dialog(out_file)

        assert out_file.exists(), "save did not produce a file"
        return out_file.read_bytes()

    def test_paste_inserts_at_caret(self, ported_exe_path, tmp_path):
        """選択なしの既定設定では、キャレット位置へ挿入される。"""
        data = self._run(ported_exe_path, tmp_path, "paste_hex_insert", "41 42 43")
        assert data == b"\x41\x42\x43" + TEST_DATA, data.hex()

    def test_paste_replaces_selection(self, ported_exe_path, tmp_path):
        """選択範囲がある場合は、その範囲が貼り付けデータで置き換わる。"""
        data = self._run(ported_exe_path, tmp_path, "paste_hex_replace", "414243",
                         action=lambda drv: drv.select_all())
        assert data == b"\x41\x42\x43", data.hex()

    def test_paste_accepts_lenient_forms(self, ported_exe_path, tmp_path):
        """0x 接頭辞・カンマ・改行・前後の余分な区切りを受け付ける。"""
        text = "\r\n 0x41,0x42\r\n43 , 44 \r\n"
        data = self._run(ported_exe_path, tmp_path, "paste_hex_lenient", text)
        assert data == b"\x41\x42\x43\x44" + TEST_DATA, data.hex()

    def test_invalid_text_keeps_document(self, ported_exe_path, tmp_path):
        """16進として解釈できないテキストは、メッセージを出してデータを変えない。"""
        test_file = tmp_path / "paste_hex_invalid.dat"
        out_file = tmp_path / "paste_hex_invalid_out.dat"
        test_file.write_bytes(TEST_DATA)

        _set_clipboard_text("0000: 41 42  AB")

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            drv.focus_view()
            time.sleep(0.3)
            # メッセージボックスはモーダルのため PostMessage 経由のコマンドで実行する。
            drv.paste_hex()

            text = drv.answer_message_box(IDOK, timeout=15.0)
            assert "16進" in text, f"hex parse error message not shown: {text!r}"

            time.sleep(0.3)
            drv.save_as_via_dialog(out_file)

        assert out_file.exists()
        assert out_file.read_bytes() == TEST_DATA, "failed parse must not modify the document"
