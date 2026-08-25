import pytest
from pathlib import Path
from drivers.stirling_driver import (
    StirlingDriver,
    CMD_EDIT_SELECT_ALL,
    ID_DELETE_SELECTION,
    ID_FILL_SELECTION,
    ID_SAVE_SELECTION,
)


class TestGoldenSelectionFill:
    """Golden comparison tests for Range Selection, Fill Selection, and Save Selection."""

    @pytest.mark.golden
    @pytest.mark.ported
    @pytest.mark.original
    def test_golden_select_all_and_fill(self, run_both_stirling):
        """Select all data and fill with 0xFF byte."""
        test_data = bytes(range(64))  # 64 bytes varying 0x00 .. 0x3F

        def action(drv: StirlingDriver, out_path: Path):
            # Select all bytes (CMD_EDIT_SELECT_ALL = 57642)
            drv.post_command(CMD_EDIT_SELECT_ALL)
            # Fill with FF
            drv.fill_range_dialog("FF")
            drv.save_as_via_dialog(out_path)

        orig_out, port_out = run_both_stirling(action, test_data)

        expected = bytes([0xFF] * 64)
        assert orig_out == port_out, "Ported fill selection output does not match Original Stirling output!"
        assert orig_out == expected, f"Original fill selection mismatch: {orig_out.hex()}"

    @pytest.mark.golden
    @pytest.mark.ported
    @pytest.mark.original
    def test_golden_select_all_and_delete(self, run_both_stirling):
        """Select all data and delete the entire selection."""
        test_data = b"DATA_TO_BE_DELETED_COMPLETELY"

        def action(drv: StirlingDriver, out_path: Path):
            # Select all
            drv.post_command(CMD_EDIT_SELECT_ALL)
            # Delete selection (ID_DELETE_SELECTION = 32810)
            drv.post_command(ID_DELETE_SELECTION)
            drv.save_as_via_dialog(out_path)

        orig_out, port_out = run_both_stirling(action, test_data)

        expected = b""
        assert orig_out == port_out, "Ported delete selection output does not match Original Stirling output!"
        assert orig_out == expected, f"Original delete selection mismatch: {orig_out}"

    @pytest.mark.golden
    @pytest.mark.ported
    @pytest.mark.original
    def test_golden_save_selection(self, run_both_stirling, tmp_path):
        """Select all data and save selection to a separate binary file."""
        test_data = b"SELECTED_RANGE_PRESERVATION_TEST_2026"

        def action(drv: StirlingDriver, out_path: Path):
            drv.post_command(CMD_EDIT_SELECT_ALL)
            drv.save_selection_via_dialog(out_path)

        orig_out, port_out = run_both_stirling(action, test_data)

        assert orig_out == port_out, "Ported save selection output does not match Original Stirling output!"
        assert orig_out == test_data, "Saved selection does not match initial binary data!"
