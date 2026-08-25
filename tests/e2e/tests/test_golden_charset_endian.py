import pytest
from pathlib import Path
from drivers.stirling_driver import StirlingDriver


class TestGoldenCharsetEndian:
    """Golden comparison tests for Character Sets (ASCII, SJIS, EUC, Unicode) and Endianness."""

    @pytest.mark.golden
    @pytest.mark.ported
    @pytest.mark.original
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
    @pytest.mark.ported
    @pytest.mark.original
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

    @pytest.mark.golden
    @pytest.mark.ported
    @pytest.mark.original
    def test_golden_byteorder_toggle(self, run_both_stirling):
        """Toggle byte order Big/Little endian, type hex, and verify consistency."""
        test_data = bytes([0xAA] * 16)

        def action(drv: StirlingDriver, out_path: Path):
            drv.set_byteorder_big()
            drv.set_byteorder_little()
            drv.type_hex_chars("11223344")
            drv.save_as_via_dialog(out_path)

        orig_out, port_out = run_both_stirling(action, test_data)

        assert orig_out == port_out, "Ported endian toggle output does not match Original Stirling output!"
        expected = bytes([0x11, 0x22, 0x33, 0x44] + [0xAA] * 12)
        assert orig_out == expected, f"Output mismatch: {orig_out.hex()}"
