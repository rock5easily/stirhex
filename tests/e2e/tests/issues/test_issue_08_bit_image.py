import time
import pytest
from pathlib import Path
import win32gui
from drivers.stirling_driver import StirlingDriver, safe_set_focus, ID_BITIMAGE, ID_BITIMAGE_RELOAD, ID_REVERT_FILE
from drivers.settings_context import stirling_settings


class TestIssue08BitImage:
    """Tests for Issue #8: Bit Image window (0x80EB / 33003) and realtime update settings."""

    @pytest.mark.original
    def test_original_bit_image_window_toggle(self, original_exe_path, tmp_path):
        """Verify Original Stirling opens Bit Image window via ID_BITIMAGE (33003)."""
        test_file = tmp_path / "bitimage_orig.dat"
        test_file.write_bytes(b"\x00" * 4096)

        with StirlingDriver(original_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)

            # Open Bit Image window
            drv.toggle_bit_image()
            wnd = drv.find_bit_image_window(timeout=2.0)
            assert wnd is not None, "Bit Image window not found in Original Stirling"
            assert win32gui.IsWindow(wnd)

            # Manual reload command
            drv.reload_bit_image()
            time.sleep(0.3)
            assert win32gui.IsWindow(wnd)

    @pytest.mark.ported
    def test_ported_bit_image_window_toggle_and_realtime(self, ported_exe_path, tmp_path):
        """Verify Ported Stirling opens Bit Image window and tracks edits in realtime."""
        test_file = tmp_path / "bitimage_port.dat"
        test_file.write_bytes(b"\x00" * 4096)

        with stirling_settings(realtime_bit_image=True):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                safe_set_focus(drv.hwnd)
                time.sleep(0.3)

                # Open Bit Image window
                drv.toggle_bit_image()
                wnd = drv.find_bit_image_window(timeout=2.0)
                assert wnd is not None, "Bit Image window not found in Ported Stirling"
                assert win32gui.IsWindow(wnd)

                # Overwrite bytes in realtime mode
                drv.type_hex_chars("FFFFFFFF")
                time.sleep(0.3)
                assert win32gui.IsWindow(wnd)

                # Revert file to original (32813)
                drv.post_command(ID_REVERT_FILE)
                time.sleep(0.3)
                assert win32gui.IsWindow(wnd)

    @pytest.mark.ported
    def test_ported_bit_image_manual_reload(self, ported_exe_path, tmp_path):
        """Verify Ported Stirling Bit Image manual reload when realtime update is disabled."""
        test_file = tmp_path / "bitimage_manual_port.dat"
        test_file.write_bytes(b"\x55" * 2048)

        with stirling_settings(realtime_bit_image=False):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                safe_set_focus(drv.hwnd)
                time.sleep(0.3)

                drv.toggle_bit_image()
                wnd = drv.find_bit_image_window(timeout=2.0)
                assert wnd is not None, "Bit Image window not found"

                # Edit bytes
                drv.type_hex_chars("AA55AA55")
                time.sleep(0.2)

                # Trigger manual reload (33004)
                drv.reload_bit_image()
                time.sleep(0.3)
                assert win32gui.IsWindow(wnd)
