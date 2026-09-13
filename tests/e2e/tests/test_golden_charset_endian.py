import pytest
from pathlib import Path
from drivers.stirling_driver import StirlingDriver


class TestGoldenCharsetEndian:
    """Golden comparison tests for Character Sets (ASCII, SJIS, EUC, Unicode) and Endianness."""

    @pytest.mark.golden
    def test_golden_charset_ascii_text_input(self, run_both_stirling):
        """Switch to ASCII charset, switch to Text pane, type characters, and compare."""
        test_data = bytes([0x00] * 16)

        def action(drv: StirlingDriver, out_path: Path):
            drv.set_charset_ascii()
            drv.press_tab()  # Hex pane -> Text pane
            drv.type_text_chars("ASCII_OK")
            drv.save_as_via_dialog(out_path)

        orig_out, port_out = run_both_stirling(action, test_data)

        assert orig_out == port_out, "Ported ASCII text input does not match Original Stirling output!"
        assert orig_out.startswith(b"ASCII_OK"), f"Unexpected output: {orig_out}"

    @pytest.mark.golden
    def test_golden_charset_euc_text_input(self, run_both_stirling):
        """Switch to EUC-JP charset, switch to Text pane, type characters, and compare."""
        test_data = bytes([0x00] * 16)

        def action(drv: StirlingDriver, out_path: Path):
            drv.set_charset_euc()
            drv.press_tab()  # Hex pane -> Text pane
            drv.type_text_chars("EUC_TEST")
            drv.save_as_via_dialog(out_path)

        orig_out, port_out = run_both_stirling(action, test_data)

        assert orig_out == port_out, "Ported EUC text input does not match Original Stirling output!"
        assert orig_out.startswith(b"EUC_TEST"), f"Unexpected output: {orig_out}"

    # test_golden_byteorder_toggle used to live here. It switched to big endian, switched
    # straight back to little, then typed hex digits - and the digits land on the same
    # bytes either way, so the case passed even if the setting were ignored entirely.
    # Byte order is now verified where it is actually observable, on the status bar detail
    # panes: tests/issues/test_issue_03_statusbar.py (Issue #204).
