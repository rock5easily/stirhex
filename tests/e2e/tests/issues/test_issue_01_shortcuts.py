import time
import pytest
from pathlib import Path
from pywinauto import keyboard
from drivers.stirling_driver import StirlingDriver


class TestIssue01Shortcuts:
    """Tests for Issue #1: Default keyboard shortcuts verified from Original Stirling help & binary.
    
    Default Shortcut Mappings (Stirling 1.31):
    - Navigation: Ctrl+Home (top), Ctrl+End (bottom), Tab (toggle hex/text pane)
    - Edit: Delete (delete byte/selection), Backspace (delete prev byte in insert mode / text), Insert (toggle mode)
    - History: Ctrl+Z (undo), Ctrl+Shift+Z (redo)  [Note: Ctrl+Y is not default in Original Stirling]
    """

    @pytest.mark.original
    def test_original_shortcuts_navigation_and_delete(self, original_exe_path, tmp_path):
        """Verify default keyboard shortcuts (Ctrl+Home, Delete, Insert, Backspace) on Original Stirling."""
        test_file = tmp_path / "nav_del_test_orig.dat"
        out_file = tmp_path / "nav_del_out_orig.dat"
        test_data = b"0123456789ABCDEF"
        test_file.write_bytes(test_data)

        with StirlingDriver(original_exe_path) as drv:
            drv.start(test_file)
            drv.focus_view()

            # 1. Ctrl+Home to ensure at top (offset 0)
            keyboard.send_keys("^{HOME}")
            time.sleep(0.2)

            # 2. Delete key (Delete '0' at offset 0) -> "123456789ABCDEF"
            drv.press_delete()
            time.sleep(0.2)

            # 3. Switch to Insert mode
            drv.press_insert()
            time.sleep(0.2)

            # 4. Move right 4 bytes
            drv.press_arrow_right(4)
            time.sleep(0.2)

            # 5. Backspace key in Insert mode (Delete '4' before cursor) -> "12356789ABCDEF"
            drv.press_backspace()
            time.sleep(0.2)

            drv.save_as_via_dialog(out_file)

        assert out_file.exists()
        expected = b"12356789ABCDEF"
        assert out_file.read_bytes() == expected

    @pytest.mark.original
    def test_original_shortcut_undo_and_redo_ctrl_shift_z(self, original_exe_path, tmp_path):
        """Verify Ctrl+Z (Undo) and Ctrl+Shift+Z (Redo) on Original Stirling."""
        test_file = tmp_path / "redo_test_orig.dat"
        out_file = tmp_path / "redo_out_orig.dat"
        test_data = b"ORIGINAL_REDO_TEST_DATA"
        test_file.write_bytes(test_data)

        with StirlingDriver(original_exe_path) as drv:
            drv.start(test_file)
            drv.focus_view()

            keyboard.send_keys("^{HOME}")
            time.sleep(0.2)

            # Type 'AA' over top byte
            drv.type_hex_chars("AA")
            time.sleep(0.2)

            # Undo via Ctrl+Z
            keyboard.send_keys("^z")
            time.sleep(0.2)

            # Redo via Ctrl+Shift+Z (Original Stirling default Redo shortcut)
            keyboard.send_keys("^+z")
            time.sleep(0.2)

            drv.save_as_via_dialog(out_file)

        assert out_file.exists()
        saved_bytes = out_file.read_bytes()
        assert saved_bytes[0] == 0xAA

    @pytest.mark.original
    def test_original_shortcut_pane_and_mode_toggle(self, original_exe_path, tmp_path):
        """Verify Tab (switch to text pane) and Insert (insert mode) on Original Stirling."""
        test_file = tmp_path / "toggle_test_orig.dat"
        out_file = tmp_path / "toggle_out_orig.dat"
        test_data = b"HELLO_WORLD_1234"
        test_file.write_bytes(test_data)

        with StirlingDriver(original_exe_path) as drv:
            drv.start(test_file)
            drv.focus_view()

            keyboard.send_keys("^{HOME}")
            time.sleep(0.2)

            # Toggle to text pane via Tab
            drv.press_tab()
            time.sleep(0.2)

            # Toggle to insert mode via Insert
            drv.press_insert()
            time.sleep(0.2)

            # Type text 'X' in text pane
            drv.type_text_chars("X")
            time.sleep(0.2)

            drv.save_as_via_dialog(out_file)

        assert out_file.exists()
        assert out_file.read_bytes() == b"XHELLO_WORLD_1234"

    @pytest.mark.ported
    def test_ported_shortcuts_navigation_and_delete(self, ported_exe_path, tmp_path):
        """Verify default keyboard shortcuts on Ported Stirling."""
        test_file = tmp_path / "nav_del_test_port.dat"
        out_file = tmp_path / "nav_del_out_port.dat"
        test_data = b"0123456789ABCDEF"
        test_file.write_bytes(test_data)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            drv.focus_view()

            keyboard.send_keys("^{HOME}")
            time.sleep(0.2)

            drv.press_delete()
            time.sleep(0.2)

            drv.press_insert()
            time.sleep(0.2)

            drv.press_arrow_right(4)
            time.sleep(0.2)

            drv.press_backspace()
            time.sleep(0.2)

            drv.save_as_via_dialog(out_file)

        assert out_file.exists()
        assert out_file.read_bytes() == b"12356789ABCDEF"

    @pytest.mark.ported
    def test_ported_shortcut_undo_and_redo_ctrl_shift_z(self, ported_exe_path, tmp_path):
        """Verify Ctrl+Z and Ctrl+Shift+Z (Redo) on Ported Stirling."""
        test_file = tmp_path / "p_redo_test.dat"
        out_file = tmp_path / "p_redo_out.dat"
        test_data = b"PORTED_REDO_TEST_DATA"
        test_file.write_bytes(test_data)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            drv.focus_view()

            keyboard.send_keys("^{HOME}")
            time.sleep(0.2)

            drv.type_hex_chars("BB")
            time.sleep(0.2)

            keyboard.send_keys("^z")
            time.sleep(0.2)

            keyboard.send_keys("^+z")
            time.sleep(0.2)

            drv.save_as_via_dialog(out_file)

        assert out_file.exists()
        saved_bytes = out_file.read_bytes()
        assert saved_bytes[0] == 0xBB

    @pytest.mark.ported
    def test_ported_shortcut_pane_and_mode_toggle(self, ported_exe_path, tmp_path):
        """Verify Tab (switch to text pane) and Insert (insert mode) on Ported Stirling."""
        test_file = tmp_path / "p_toggle_test.dat"
        out_file = tmp_path / "p_toggle_out.dat"
        test_data = b"HELLO_WORLD_1234"
        test_file.write_bytes(test_data)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            drv.focus_view()

            keyboard.send_keys("^{HOME}")
            time.sleep(0.2)

            drv.press_tab()
            time.sleep(0.2)

            drv.press_insert()
            time.sleep(0.2)

            drv.type_text_chars("X")
            time.sleep(0.2)

            drv.save_as_via_dialog(out_file)

        assert out_file.exists()
        assert out_file.read_bytes() == b"XHELLO_WORLD_1234"
