import pytest
from pathlib import Path
from drivers.stirling_driver import StirlingDriver


class TestGoldenClipboard:
    """Golden comparison tests for Copy, Paste, Undo, and Redo operations."""

    @pytest.mark.golden
    @pytest.mark.ported
    @pytest.mark.original
    def test_golden_copy_paste_all(self, run_both_stirling):
        """Select all, copy to clipboard, delete content, paste, and verify perfect restoration."""
        test_data = bytes([0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0] * 4)

        def action(drv: StirlingDriver, out_path: Path):
            # 1. Select all
            drv.select_all()
            # 2. Copy
            drv.copy()
            # 3. Delete selected range
            drv.press_delete()
            # 4. Paste back
            drv.paste()
            # 5. Save
            drv.save_as_via_dialog(out_path)

        orig_out, port_out = run_both_stirling(action, test_data)

        assert orig_out == port_out, "Ported copy-paste output does not match Original Stirling output!"
        assert orig_out == test_data, f"Data was not restored identically: {orig_out.hex()}"

    @pytest.mark.golden
    @pytest.mark.ported
    @pytest.mark.original
    def test_golden_cut_paste_all(self, run_both_stirling):
        """Select all, cut to clipboard, paste back, and verify identical data restoration."""
        test_data = bytes(range(32))

        def action(drv: StirlingDriver, out_path: Path):
            # 1. Select all
            drv.select_all()
            # 2. Cut
            drv.cut()
            # 3. Paste back
            drv.paste()
            # 4. Save
            drv.save_as_via_dialog(out_path)

        orig_out, port_out = run_both_stirling(action, test_data)

        assert orig_out == port_out, "Ported cut-paste output does not match Original Stirling output!"
        assert orig_out == test_data, f"Data was not restored identically: {orig_out.hex()}"

    @pytest.mark.golden
    @pytest.mark.ported
    @pytest.mark.original
    def test_golden_undo_single_and_multiple(self, run_both_stirling):
        """Perform multiple byte edits, execute Undo, and verify exact byte restoration."""
        test_data = bytes([0xFF] * 16)

        def action(drv: StirlingDriver, out_path: Path):
            # Edit 1: change byte 0 to 0x11
            drv.jump_to_address(0)
            drv.type_hex_chars("11")
            # Edit 2: change byte 4 to 0x33
            drv.jump_to_address(4)
            drv.type_hex_chars("33")
            # Undo Edit 2 (restores byte 4 to 0xFF)
            drv.undo()
            # Save
            drv.save_as_via_dialog(out_path)

        orig_out, port_out = run_both_stirling(action, test_data)

        assert orig_out == port_out, "Ported undo output does not match Original Stirling output!"
        expected = bytes([0x11, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF] + [0xFF] * 10)
        assert orig_out == expected, f"Output mismatch: {orig_out.hex()}"
