import pytest
from pathlib import Path
from drivers.stirling_driver import StirlingDriver, CMD_EDIT_UNDO


class TestGoldenEditHex:
    """Golden comparison tests for 16-bit hex editing operations."""

    @pytest.mark.golden
    @pytest.mark.ported
    @pytest.mark.original
    def test_golden_overwrite_hex(self, run_both_stirling):
        """Type hex characters in overwrite mode (default), then Save As.
        Verify both original and ported produce byte-identical modified binary."""
        test_data = bytes([0x00] * 32)  # 32 zero bytes

        def action(drv: StirlingDriver, out_path: Path):
            # Overwrite first 4 bytes with 0xAA, 0xBB, 0xCC, 0xDD
            drv.type_hex_chars("AABBCCDD")
            drv.save_as_via_dialog(out_path)

        orig_out, port_out = run_both_stirling(action, test_data)

        expected_prefix = bytes([0xAA, 0xBB, 0xCC, 0xDD])
        assert orig_out.startswith(expected_prefix), "Original edit failed"
        assert port_out.startswith(expected_prefix), "Ported edit failed"

        # Golden comparison: Original == Ported
        assert orig_out == port_out, "Ported edit output does not match Original Stirling output!"
        assert len(port_out) == len(test_data), "Overwrite mode should not alter total length"

    @pytest.mark.golden
    @pytest.mark.ported
    @pytest.mark.original
    def test_golden_undo_hex(self, run_both_stirling):
        """Type hex characters, then Undo, then Save As.
        Verify both original and ported restore exact initial data."""
        test_data = b"STIRLING_UNDO_TEST_INITIAL_DATA_12345678"

        def action(drv: StirlingDriver, out_path: Path):
            drv.type_hex_chars("DEADBEEF")
            # Undo 4 overwrite operations
            for _ in range(4):
                drv.post_command(CMD_EDIT_UNDO)
            drv.save_as_via_dialog(out_path)

        orig_out, port_out = run_both_stirling(action, test_data)

        # Golden comparison
        assert orig_out == port_out, "Ported Undo output does not match Original Stirling output!"
        assert port_out == test_data, "Undo failed to restore initial data exactly!"
