import time
import pytest
from pathlib import Path
from drivers.stirling_driver import StirlingDriver, safe_set_focus


class TestIssue07SelectRange:
    """Tests for Issue #7: Select Range command (0x8065 / 32869) and Dialog.
    
    Prerequisite:
    - Document is opened.
    - Dialog allows entering start address and end address (in Hex or Dec) and selects the specified range upon OK.
    """

    @pytest.mark.original
    def test_original_select_range_hex_and_delete(self, original_exe_path, tmp_path):
        """Verify Original Stirling selects hex address range (0x10 to 0x1F) and deletes it."""
        test_file = tmp_path / "range_test_hex_orig.dat"
        out_file = tmp_path / "range_out_hex_orig.dat"
        # 64 bytes: 0x00 .. 0x3F
        test_data = bytes(range(64))
        test_file.write_bytes(test_data)

        with StirlingDriver(original_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)

            # Select range 0x10 .. 0x1F (16 bytes) in Hex
            drv.select_range_dialog(start_addr="10", end_addr="1F", is_hex=True)
            time.sleep(0.3)

            # Delete the selected range
            drv.press_delete()
            time.sleep(0.3)

            drv.save_as_via_dialog(out_file)

        assert out_file.exists()
        saved_bytes = out_file.read_bytes()
        # Original: 64 bytes - 16 bytes deleted = 48 bytes
        expected_bytes = bytes(range(0, 16)) + bytes(range(32, 64))
        assert len(saved_bytes) == 48, f"Expected 48 bytes after range delete, got {len(saved_bytes)}"
        assert saved_bytes == expected_bytes, "Deleted data range does not match selected range"

    @pytest.mark.original
    def test_original_select_range_dec_and_fill(self, original_exe_path, tmp_path):
        """Verify Original Stirling selects decimal address range (10 to 19) and fills it."""
        test_file = tmp_path / "range_test_dec_orig.dat"
        out_file = tmp_path / "range_out_dec_orig.dat"
        test_data = bytes([0x00] * 32)
        test_file.write_bytes(test_data)

        with StirlingDriver(original_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)

            # Select decimal range 10 .. 19 (10 bytes)
            drv.select_range_dialog(start_addr="10", end_addr="19", is_hex=False)
            time.sleep(0.3)

            # Fill selected range with 0xFF
            drv.fill_range_dialog("FF")
            time.sleep(0.3)

            drv.save_as_via_dialog(out_file)

        assert out_file.exists()
        saved_bytes = out_file.read_bytes()
        expected = bytes([0x00] * 10) + bytes([0xFF] * 10) + bytes([0x00] * 12)
        assert saved_bytes == expected, "Filled data range does not match selected decimal range"

    @pytest.mark.ported
    def test_ported_select_range_hex_and_delete(self, ported_exe_path, tmp_path):
        """Verify Ported Stirling selects hex address range (0x10 to 0x1F) and deletes it."""
        test_file = tmp_path / "range_test_hex_port.dat"
        out_file = tmp_path / "range_out_hex_port.dat"
        test_data = bytes(range(64))
        test_file.write_bytes(test_data)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)

            # Select range 0x10 .. 0x1F (16 bytes) in Hex
            drv.select_range_dialog(start_addr="10", end_addr="1F", is_hex=True)
            time.sleep(0.3)

            # Delete the selected range
            drv.press_delete()
            time.sleep(0.3)

            drv.save_as_via_dialog(out_file)

        assert out_file.exists()
        saved_bytes = out_file.read_bytes()
        expected_bytes = bytes(range(0, 16)) + bytes(range(32, 64))
        assert len(saved_bytes) == 48, f"Expected 48 bytes after range delete, got {len(saved_bytes)}"
        assert saved_bytes == expected_bytes, "Deleted data range does not match selected range"

    @pytest.mark.ported
    def test_ported_select_range_dec_and_fill(self, ported_exe_path, tmp_path):
        """Verify Ported Stirling selects decimal address range (10 to 19) and fills it."""
        test_file = tmp_path / "range_test_dec_port.dat"
        out_file = tmp_path / "range_out_dec_port.dat"
        test_data = bytes([0x00] * 32)
        test_file.write_bytes(test_data)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)

            # Select decimal range 10 .. 19 (10 bytes)
            drv.select_range_dialog(start_addr="10", end_addr="19", is_hex=False)
            time.sleep(0.3)

            # Fill selected range with 0xFF
            drv.fill_range_dialog("FF")
            time.sleep(0.3)

            drv.save_as_via_dialog(out_file)

        assert out_file.exists()
        saved_bytes = out_file.read_bytes()
        expected = bytes([0x00] * 10) + bytes([0xFF] * 10) + bytes([0x00] * 12)
        assert saved_bytes == expected, "Filled data range does not match selected decimal range"
