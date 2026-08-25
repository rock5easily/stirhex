import time
import pytest
import win32clipboard
from drivers.stirling_driver import StirlingDriver, safe_set_focus


def _open_clipboard():
    """Open the clipboard, retrying while another process holds it."""
    last_error = None
    for _ in range(20):
        try:
            win32clipboard.OpenClipboard()
            return
        except Exception as exc:  # clipboard locked by another process
            last_error = exc
            time.sleep(0.1)
    raise AssertionError(f"Could not open the clipboard: {last_error}")


def _read_clipboard(fmt):
    """Read one clipboard format (None when the format is not available)."""
    _open_clipboard()
    try:
        if not win32clipboard.IsClipboardFormatAvailable(fmt):
            return None
        return win32clipboard.GetClipboardData(fmt)
    finally:
        win32clipboard.CloseClipboard()


def _first_clipboard_format():
    """The format that was explicitly set last.

    EnumClipboardFormats lists explicitly set formats before the ones Windows
    synthesizes, so this tells CF_UNICODETEXT-we-set apart from CF_UNICODETEXT
    that the OS derived from a CF_TEXT we published.
    """
    _open_clipboard()
    try:
        return win32clipboard.EnumClipboardFormats(0)
    finally:
        win32clipboard.CloseClipboard()


class TestIssue47Clipboard:
    """Tests for Issue #47: RAII-based clipboard transfer and clipboard formats.

    Layering (analysis_artifacts/docs/20_unicode_layering.md 6.5):
    - Hex pane copy is an ASCII-layer string  -> CF_UNICODETEXT.
    - Char pane copy is the edited byte layer -> CF_TEXT carrying the raw bytes.
    """

    # CP932 bytes for "aiu" in Japanese kana, plus a deliberately broken pair (0x82 0x3F).
    TEST_DATA = bytes([0x82, 0xA0, 0x82, 0xA2, 0x82, 0xA4, 0x82, 0x3F])

    @pytest.mark.ported
    def test_ported_hex_pane_copy_is_unicode_text(self, ported_exe_path, tmp_path):
        """Hex pane copy exposes CF_UNICODETEXT with space separated upper case hex."""
        test_file = tmp_path / "clip_hex.dat"
        test_file.write_bytes(self.TEST_DATA)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)
            drv.select_all()
            drv.copy()
            time.sleep(0.3)

            text = _read_clipboard(win32clipboard.CF_UNICODETEXT)
            first = _first_clipboard_format()

        expected = " ".join(f"{b:02X}" for b in self.TEST_DATA)
        assert text is not None, "CF_UNICODETEXT is not available after copying from the hex pane"
        assert text == expected, f"Hex pane copy mismatch: {text!r} != {expected!r}"
        assert first == win32clipboard.CF_UNICODETEXT, (
            f"Hex pane should publish CF_UNICODETEXT itself, not have it synthesized (first={first})"
        )

    @pytest.mark.ported
    def test_ported_char_pane_copy_keeps_raw_bytes(self, ported_exe_path, tmp_path):
        """Char pane copy exposes CF_TEXT with the raw CP932 bytes (broken pairs included)."""
        test_file = tmp_path / "clip_char.dat"
        test_file.write_bytes(self.TEST_DATA)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)
            drv.press_tab()   # switch to the char pane
            time.sleep(0.2)
            drv.select_all()
            drv.copy()
            time.sleep(0.3)

            raw = _read_clipboard(win32clipboard.CF_TEXT)
            first = _first_clipboard_format()

        assert raw is not None, "CF_TEXT is not available after copying from the char pane"
        if isinstance(raw, str):
            raw = raw.encode("cp932", errors="strict")
        assert raw == self.TEST_DATA, f"Char pane copy mismatch: {raw!r} != {self.TEST_DATA!r}"
        assert first == win32clipboard.CF_TEXT, (
            f"Char pane should publish CF_TEXT itself so the raw bytes survive (first={first})"
        )

    @pytest.mark.ported
    def test_ported_char_pane_copy_pastes_japanese_without_mojibake(self, ported_exe_path, tmp_path):
        """Japanese text copied from the char pane reaches other apps unchanged.

        Other apps commonly ask for CF_UNICODETEXT, which Windows synthesizes from the
        CF_TEXT we publish. Verify the synthesized text decodes back to the same kana.
        """
        kana = "あいう"   # 3 Japanese kana
        test_file = tmp_path / "clip_kana.dat"
        test_file.write_bytes(kana.encode("cp932"))

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)
            drv.press_tab()   # switch to the char pane
            time.sleep(0.2)
            drv.select_all()
            drv.copy()
            time.sleep(0.3)

            text = _read_clipboard(win32clipboard.CF_UNICODETEXT)

        assert text is not None, "CF_UNICODETEXT is not synthesized from CF_TEXT"
        assert text == kana, f"Japanese text was mangled: {text!r} != {kana!r}"
