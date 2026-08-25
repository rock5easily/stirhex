import time
import pytest
from pathlib import Path
import win32gui
from drivers.stirling_driver import StirlingDriver, safe_set_focus


class TestIssue14MarkDraw:
    """Tests for Issue #14: Mark background color rendering and structure edit highlight bounds."""

    @pytest.mark.original
    def test_original_mark_in_struct_edit_range(self, original_exe_path, tmp_path):
        """Verify Original Stirling supports setting and navigating marks inside structure edit bounds."""
        test_file = tmp_path / "mark_draw_orig.dat"
        test_file.write_bytes(bytes(range(256)))

        with StirlingDriver(original_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)

            # Show struct bar and select LOGFONT
            drv.toggle_struct_bar(show=True)
            time.sleep(0.3)
            drv.select_struct_type("LOGFONT")
            time.sleep(0.3)

            # Set mark at offset 0x00 and 0x01 (adjacent marks within struct range)
            drv.jump_to_address("0", is_hex=True)
            time.sleep(0.2)
            drv.mark_toggle()
            time.sleep(0.2)

            drv.jump_to_address("1", is_hex=True)
            time.sleep(0.2)
            drv.mark_toggle()
            time.sleep(0.2)

            # Jump to mark via dialog
            drv.struct_goto_dialog(mode="mark", mark_index=0)
            time.sleep(0.3)
            assert drv.get_struct_address() == "00000000"

    @pytest.mark.ported
    def test_ported_mark_in_struct_edit_range(self, ported_exe_path, tmp_path):
        """Verify Ported Stirling supports setting and navigating marks inside structure edit bounds."""
        test_file = tmp_path / "mark_draw_port.dat"
        test_file.write_bytes(bytes(range(256)))

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)

            drv.toggle_struct_bar(show=True)
            time.sleep(0.3)
            drv.select_struct_type("LOGFONT")
            time.sleep(0.3)

            # Set mark at offset 0x00 and 0x01 (adjacent marks within struct range)
            drv.jump_to_address("0", is_hex=True)
            time.sleep(0.2)
            drv.mark_toggle()
            time.sleep(0.2)

            drv.jump_to_address("1", is_hex=True)
            time.sleep(0.2)
            drv.mark_toggle()
            time.sleep(0.2)

            # Jump to mark via struct goto dialog
            drv.struct_goto_dialog(mode="mark", mark_index=0)
            time.sleep(0.3)
            assert drv.get_struct_address() == "00000000"

            # Clear marks
            drv.mark_clear_all()
            time.sleep(0.2)
