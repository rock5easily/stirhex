import pytest
from pathlib import Path
from drivers.stirling_driver import StirlingDriver


class TestGoldenRevert:
    """Golden comparison tests for Revert File functionality (ID_REVERT_FILE = 32813)."""

    @pytest.mark.golden
    @pytest.mark.ported
    @pytest.mark.original
    def test_golden_revert_after_edit(self, run_both_stirling):
        """Perform modifications, execute Revert File, type new byte, and verify clean state reset."""
        test_data = b"PRE_EDIT_ORIGINAL_DATA_BLOCK_1234567890"

        def action(drv: StirlingDriver, out_path: Path):
            # 1. Dirty document by writing DEADBEEF (moves caret to 4)
            drv.type_hex_chars("DEADBEEF")
            # 2. Revert file (discards edits and reloads from disk)
            drv.revert_file()
            # 3. Explicitly jump to address 0 to ensure uniform caret pos across versions
            drv.jump_to_address(0)
            # 4. Modify single byte at position 0 to 0x77 ('w')
            drv.type_hex_chars("77")
            # 5. Save as
            drv.save_as_via_dialog(out_path)

        orig_out, port_out = run_both_stirling(action, test_data)

        expected = b"w" + test_data[1:]
        assert orig_out == port_out, "Ported revert output does not match Original Stirling output!"
        assert orig_out == expected, f"Revert result mismatch: {orig_out}"
