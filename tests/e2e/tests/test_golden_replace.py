import pytest
from pathlib import Path
from drivers.stirling_driver import StirlingDriver


class TestGoldenReplace:
    """Golden comparison tests for Replace dialog operations."""

    @pytest.mark.golden
    @pytest.mark.ported
    @pytest.mark.original
    def test_golden_replace_hex_same_length(self, run_both_stirling):
        """Replace hex pattern with equal-length pattern across whole document."""
        test_data = bytes([0xAA, 0x11, 0xAA, 0x22, 0xAA, 0x33, 0xAA, 0x44])

        def action(drv: StirlingDriver, out_path: Path):
            # Replace all 0xAA with 0xFF
            drv.replace_all_dialog("AA", "FF", search_is_hex=True, replace_is_hex=True)
            drv.save_as_via_dialog(out_path)

        orig_out, port_out = run_both_stirling(action, test_data)

        expected = bytes([0xFF, 0x11, 0xFF, 0x22, 0xFF, 0x33, 0xFF, 0x44])
        assert orig_out == port_out, "Ported replace output does not match Original Stirling output!"
        assert orig_out == expected, f"Original replace mismatch: {orig_out.hex()}"

    @pytest.mark.golden
    @pytest.mark.ported
    @pytest.mark.original
    def test_golden_replace_hex_expand(self, run_both_stirling):
        """Replace hex pattern with longer pattern (data expansion)."""
        test_data = bytes([0x00, 0xAA, 0x00, 0xBB, 0x00])

        def action(drv: StirlingDriver, out_path: Path):
            # Replace single byte 0x00 with 3 bytes (0x11, 0x22, 0x33)
            drv.replace_all_dialog("00", "112233", search_is_hex=True, replace_is_hex=True)
            drv.save_as_via_dialog(out_path)

        orig_out, port_out = run_both_stirling(action, test_data)

        expected = bytes([
            0x11, 0x22, 0x33, 0xAA,
            0x11, 0x22, 0x33, 0xBB,
            0x11, 0x22, 0x33
        ])
        assert orig_out == port_out, "Ported replace expansion output does not match Original Stirling output!"
        assert orig_out == expected, f"Original replace expansion mismatch: {orig_out.hex()}"

    @pytest.mark.golden
    @pytest.mark.ported
    @pytest.mark.original
    def test_golden_replace_hex_shrink(self, run_both_stirling):
        """Replace hex pattern with shorter pattern (data shrinkage)."""
        test_data = bytes([0xAA, 0xBB, 0xCC, 0x01, 0xAA, 0xBB, 0xCC, 0x02])

        def action(drv: StirlingDriver, out_path: Path):
            # Replace 3 bytes (0xAA, 0xBB, 0xCC) with 1 byte (0xFF)
            drv.replace_all_dialog("AABBCC", "FF", search_is_hex=True, replace_is_hex=True)
            drv.save_as_via_dialog(out_path)

        orig_out, port_out = run_both_stirling(action, test_data)

        expected = bytes([0xFF, 0x01, 0xFF, 0x02])
        assert orig_out == port_out, "Ported replace shrinkage output does not match Original Stirling output!"
        assert orig_out == expected, f"Original replace shrinkage mismatch: {orig_out.hex()}"
