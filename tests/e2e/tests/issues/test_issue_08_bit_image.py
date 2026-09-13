"""Issue #8: the bit image window and its realtime / manual update settings.

These cases used to assert only `IsWindow(wnd)` after editing or reloading, so a build
whose image never updated again still passed. They now compare the pixels of the bit
image itself: with realtime updates on the picture must follow an edit, with them off it
must stay put until the manual reload command, and reverting the file must bring the
original picture back (Issue #204).
"""

import time

import pytest
import win32gui

from drivers.settings_context import stirling_settings
from drivers.stirling_driver import ID_BITIMAGE_RELOAD, StirlingDriver, safe_set_focus

# A uniform file gives a flat picture, so any edit shows up as a clear difference.
FLAT_FILE = b"\x00" * 4096
PATTERN_FILE = b"\x55" * 2048


def _image_of(drv: StirlingDriver, wnd: int):
    """Pixels of the bit image window, waited for so a pending repaint is included.

    The caret is parked at a fixed address first: the bit image marks the current
    position, so captures taken at different addresses would differ for that reason
    alone - and an edit would look like an update even if the data were never redrawn.
    """
    drv.jump_to_address("0", is_hex=True)
    time.sleep(0.5)
    captured = drv.capture_window_image(wnd)
    assert captured is not None, "the bit image window must be capturable"
    return captured


def _open_bit_image(drv: StirlingDriver) -> int:
    drv.toggle_bit_image()
    wnd = drv.find_bit_image_window(timeout=3.0)
    assert wnd is not None, "Bit Image window not found"
    assert win32gui.IsWindow(wnd)
    return wnd


class TestIssue08BitImage:
    """Issue #8: Bit Image window (0x80EB / 33003) and realtime update settings."""

    @pytest.mark.original
    def test_original_bit_image_window_toggle(self, original_exe_path, tmp_path):
        """Verify Original Stirling opens Bit Image window via ID_BITIMAGE (33003)."""
        test_file = tmp_path / "bitimage_orig.dat"
        test_file.write_bytes(FLAT_FILE)

        with StirlingDriver(original_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)

            wnd = _open_bit_image(drv)
            drv.reload_bit_image()
            time.sleep(0.3)
            assert win32gui.IsWindow(wnd)

    @pytest.mark.ported
    def test_ported_bit_image_follows_edits_in_realtime(self, ported_exe_path, tmp_path):
        """With realtime updates on, editing changes the picture and reverting restores it."""
        test_file = tmp_path / "bitimage_port.dat"
        test_file.write_bytes(FLAT_FILE)

        with stirling_settings(realtime_bit_image=True):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                safe_set_focus(drv.hwnd)
                time.sleep(0.3)

                wnd = _open_bit_image(drv)
                original = _image_of(drv, wnd)

                drv.type_hex_chars("FFFFFFFF")
                edited = _image_of(drv, wnd)
                assert edited != original, "the realtime image must follow the edit"

                # Reverting discards the edit; revert_file answers the confirmation.
                drv.revert_file()
                time.sleep(0.5)
                assert win32gui.IsWindow(wnd)
                assert _image_of(drv, wnd) == original, (
                    "after reverting, the image must show the file again"
                )

    @pytest.mark.ported
    def test_ported_bit_image_manual_reload(self, ported_exe_path, tmp_path):
        """With realtime updates off, the picture only changes on the reload command."""
        test_file = tmp_path / "bitimage_manual_port.dat"
        test_file.write_bytes(PATTERN_FILE)

        with stirling_settings(realtime_bit_image=False):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                safe_set_focus(drv.hwnd)
                time.sleep(0.3)

                wnd = _open_bit_image(drv)
                original = _image_of(drv, wnd)

                drv.type_hex_chars("AA55AA55")
                assert _image_of(drv, wnd) == original, (
                    "with realtime updates off the image must not change on its own"
                )

                drv.post_command(ID_BITIMAGE_RELOAD)
                time.sleep(0.5)
                assert _image_of(drv, wnd) != original, (
                    "the manual reload must redraw the image with the edited data"
                )
