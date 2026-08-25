import pytest
from pathlib import Path
from drivers.stirling_driver import StirlingDriver


class TestGoldenEditInsert:
    """Golden comparison tests for Insert mode, Deletion, and Text pane editing."""

    @pytest.mark.golden
    @pytest.mark.ported
    @pytest.mark.original
    def test_golden_insert_mode_hex(self, run_both_stirling):
        """Toggle to Insert mode with VK_INSERT, type hex characters, and verify inserted bytes."""
        test_data = b"0123456789ABCDEF"

        def action(drv: StirlingDriver, out_path: Path):
            # Switch to Insert mode
            drv.press_insert()
            # Insert 4 bytes (0xAA, 0xBB, 0xCC, 0xDD) at position 0
            drv.type_hex_chars("AABBCCDD")
            drv.save_as_via_dialog(out_path)

        orig_out, port_out = run_both_stirling(action, test_data)

        expected = bytes([0xAA, 0xBB, 0xCC, 0xDD]) + test_data
        assert orig_out == expected, f"Original insert mismatch: {orig_out.hex()} != {expected.hex()}"
        assert port_out == orig_out, "Ported insert output does not match Original Stirling output!"
        assert len(port_out) == len(test_data) + 4

    @pytest.mark.golden
    @pytest.mark.ported
    @pytest.mark.original
    def test_golden_delete_key(self, run_both_stirling):
        """Delete bytes at caret with VK_DELETE and verify truncation."""
        test_data = b"PREFIX_1234567890_SUFFIX"

        def action(drv: StirlingDriver, out_path: Path):
            # Delete first 7 bytes ("PREFIX_")
            for _ in range(7):
                drv.press_delete()
            drv.save_as_via_dialog(out_path)

        orig_out, port_out = run_both_stirling(action, test_data)

        expected = b"1234567890_SUFFIX"
        assert orig_out == expected, f"Original delete mismatch: {orig_out} != {expected}"
        assert port_out == orig_out, "Ported delete output does not match Original Stirling output!"
        assert len(port_out) == len(test_data) - 7

    @pytest.mark.golden
    @pytest.mark.ported
    @pytest.mark.original
    def test_golden_backspace_key(self, run_both_stirling):
        """Move caret right then delete with VK_BACK."""
        test_data = b"ABCDEFGHIJKLMN"

        def action(drv: StirlingDriver, out_path: Path):
            # Move caret 4 bytes right (to index 4, pointing at 'E')
            drv.press_arrow_right(4)
            # Backspace twice (deletes 'D', then 'C')
            drv.press_backspace()
            drv.press_backspace()
            drv.save_as_via_dialog(out_path)

        orig_out, port_out = run_both_stirling(action, test_data)

        expected = b"ABEFGHIJKLMN"
        assert orig_out == expected, f"Original backspace mismatch: {orig_out} != {expected}"
        assert port_out == orig_out, "Ported backspace output does not match Original Stirling output!"
        assert len(port_out) == len(test_data) - 2

    @pytest.mark.golden
    @pytest.mark.ported
    @pytest.mark.original
    def test_golden_text_pane_ascii(self, run_both_stirling):
        """Switch to Text pane with Tab and type ASCII characters in overwrite mode."""
        test_data = bytes([0x00] * 16)

        def action(drv: StirlingDriver, out_path: Path):
            # Switch to Text pane
            drv.press_tab()
            # Type ASCII string
            drv.type_text_chars("HELLOWORLD")
            drv.save_as_via_dialog(out_path)

        orig_out, port_out = run_both_stirling(action, test_data)

        expected_prefix = b"HELLOWORLD"
        assert orig_out.startswith(expected_prefix), f"Original text edit failed: {orig_out}"
        assert port_out == orig_out, "Ported text edit output does not match Original Stirling output!"
        assert len(port_out) == len(test_data)
