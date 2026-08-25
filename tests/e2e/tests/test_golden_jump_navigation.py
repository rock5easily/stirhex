import pytest
from pathlib import Path
from drivers.stirling_driver import StirlingDriver, ID_GOTO_DATA_TOP, ID_GOTO_DATA_END


class TestGoldenJumpNavigation:
    """Golden comparison tests for Jump dialog and Top/End navigation."""

    @pytest.mark.golden
    @pytest.mark.ported
    @pytest.mark.original
    def test_golden_jump_absolute_hex(self, run_both_stirling):
        """Jump to absolute hex address 0x20 and write hex bytes."""
        test_data = bytes([0x00] * 64)

        def action(drv: StirlingDriver, out_path: Path):
            # Jump to offset 0x20 (32) in hex mode
            drv.jump_to_address("20", is_hex=True)
            # Overwrite 4 bytes with DEADBEEF
            drv.type_hex_chars("DEADBEEF")
            drv.save_as_via_dialog(out_path)

        orig_out, port_out = run_both_stirling(action, test_data)

        expected = bytearray([0x00] * 64)
        expected[0x20:0x24] = bytes([0xDE, 0xAD, 0xBE, 0xEF])

        assert orig_out == port_out, "Ported jump output does not match Original Stirling output!"
        assert orig_out == expected, f"Original jump output mismatch: {orig_out.hex()}"

    @pytest.mark.golden
    @pytest.mark.ported
    @pytest.mark.original
    def test_golden_jump_absolute_dec(self, run_both_stirling):
        """Jump to absolute decimal address 32 (0x20) and write hex bytes."""
        test_data = bytes([0x00] * 64)

        def action(drv: StirlingDriver, out_path: Path):
            # Jump to offset 32 (0x20) in decimal mode
            drv.jump_to_address("32", is_hex=False)
            drv.type_hex_chars("CAFEBABE")
            drv.save_as_via_dialog(out_path)

        orig_out, port_out = run_both_stirling(action, test_data)

        expected = bytearray([0x00] * 64)
        expected[0x20:0x24] = bytes([0xCA, 0xFE, 0xBA, 0xBE])

        assert orig_out == port_out, "Ported decimal jump output does not match Original Stirling output!"
        assert orig_out == expected, f"Original decimal jump output mismatch: {orig_out.hex()}"

    @pytest.mark.golden
    @pytest.mark.ported
    @pytest.mark.original
    def test_golden_jump_relative_backward(self, run_both_stirling):
        """Jump with negative relative offset (-) and perform edits."""
        test_data = bytes([0x00] * 64)

        def action(drv: StirlingDriver, out_path: Path):
            # Jump to 0x20 in hex mode
            drv.jump_to_address("20", is_hex=True)
            drv.type_hex_chars("CAFEBABE")
            # Relative backward -10 (offset 0x24 - 0x10 = 0x14)
            drv.jump_to_address("-10", is_hex=True)
            drv.type_hex_chars("FACEFEED")
            drv.save_as_via_dialog(out_path)

        orig_out, port_out = run_both_stirling(action, test_data)

        expected = bytearray([0x00] * 64)
        expected[0x14:0x18] = bytes([0xFA, 0xCE, 0xFE, 0xED])
        expected[0x20:0x24] = bytes([0xCA, 0xFE, 0xBA, 0xBE])

        assert orig_out == port_out, "Ported relative jump does not match Original Stirling output!"
        assert orig_out == expected, f"Original relative jump mismatch: {orig_out.hex()}"

    @pytest.mark.golden
    @pytest.mark.ported
    @pytest.mark.original
    def test_golden_goto_data_top_and_end(self, run_both_stirling):
        """Navigate to Data End and Data Top, and insert bytes at boundaries."""
        test_data = b"MIDDLE_DATA_BLOCK"

        def action(drv: StirlingDriver, out_path: Path):
            # Switch to Insert mode
            drv.press_insert()
            # Goto End and append 2 bytes (0xEE, 0xFF)
            drv.post_command(ID_GOTO_DATA_END)
            drv.type_hex_chars("EEFF")
            # Goto Top and prepend 2 bytes (0xAA, 0xBB)
            drv.post_command(ID_GOTO_DATA_TOP)
            drv.type_hex_chars("AABB")
            drv.save_as_via_dialog(out_path)

        orig_out, port_out = run_both_stirling(action, test_data)

        expected = bytes([0xAA, 0xBB]) + test_data + bytes([0xEE, 0xFF])
        assert orig_out == port_out, "Ported top/end output does not match Original Stirling output!"
        assert orig_out == expected, f"Original top/end mismatch: {orig_out.hex()}"
