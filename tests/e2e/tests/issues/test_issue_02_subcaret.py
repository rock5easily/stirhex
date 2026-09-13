"""Issue #2: the sub-caret drawn in the pane that does not have the caret.

The tracking cases used to move the caret from 0x00 to 0x01 and pass as soon as any pixel
of the whole view changed - which the active caret does on its own, so the sub-caret could
have been frozen or missing entirely.

They now isolate the sub-caret: the same address is captured twice, once with the setting
on and once with it off, and the pixels that differ between those two pictures are the
sub-caret itself. Doing that at 0x00 and at 0x01 shows whether it moved, and how far. Only
the outermost changed column on the side opposite the caret is used, so the blinking
active caret cannot be mistaken for the sub-caret (Issue #204).
"""

import time

import pytest

from drivers.settings_context import stirling_settings
from drivers.stirling_driver import StirlingDriver

DATA = b"A" * 64

# One character cell is far narrower than this; a jump of a whole pane is far wider.
MAX_CELL_ADVANCE_PX = 40


def _changed_columns(first, second) -> set[int]:
    """Columns where two captures of the same view differ."""
    pixels_a, width, height = first
    pixels_b, width_b, height_b = second
    assert (width, height) == (width_b, height_b), "the view must not be resized"

    columns: set[int] = set()
    for row in range(height):
        base = row * width * 4
        for column in range(width):
            offset = base + column * 4
            if pixels_a[offset:offset + 4] != pixels_b[offset:offset + 4]:
                columns.add(column)
    return columns


def _capture_positions(exe_path, test_file, sub_caret: bool, focus_text_pane: bool):
    """Capture the view at 0x00 and at 0x01 with the sub-caret setting as given."""
    with stirling_settings(show_sub_caret=sub_caret):
        with StirlingDriver(exe_path) as drv:
            drv.start(test_file)
            drv.focus_view()
            time.sleep(0.3)
            if focus_text_pane:
                drv.press_tab()
                time.sleep(0.3)

            captures = []
            for address in ("0", "1"):
                drv.jump_to_address(address, is_hex=True)
                time.sleep(0.4)
                captured = drv.capture_view_image()
                assert captured is not None, "the view must be capturable"
                captures.append(captured)
            return captures


def _subcaret_columns(exe_path, test_file, focus_text_pane: bool) -> tuple[int, int]:
    """Column of the sub-caret with the caret at 0x00 and at 0x01."""
    on_zero, on_one = _capture_positions(exe_path, test_file, True, focus_text_pane)
    off_zero, off_one = _capture_positions(exe_path, test_file, False, focus_text_pane)

    at_zero = _changed_columns(on_zero, off_zero)
    at_one = _changed_columns(on_one, off_one)
    assert at_zero, "turning the sub-caret on must draw something at 0x00"
    assert at_one, "turning the sub-caret on must draw something at 0x01"

    # With the caret in the hex pane the sub-caret is in the text pane, on the right of
    # the view, and the other way round. Taking the outermost changed column on that side
    # keeps a blinking caret in the opposite pane out of the measurement.
    if focus_text_pane:
        return min(at_zero), min(at_one)
    return max(at_zero), max(at_one)


def _assert_followed_the_caret(column_zero: int, column_one: int) -> None:
    """One step right, by one character cell rather than by a pane."""
    assert column_one > column_zero, (
        f"the sub-caret stayed at {column_zero}px when the caret moved to 0x01"
    )
    assert column_one - column_zero <= MAX_CELL_ADVANCE_PX, (
        f"the sub-caret moved {column_one - column_zero}px, which is more than one cell"
    )


class TestIssue02SubCaret:
    """Issue #2: sub-caret rendering in the inactive pane.

    - Hex pane active -> sub-caret rendered in the text pane
    - Text pane active -> sub-caret rendered in the hex pane

    Prerequisite setting: 環境設定 - 編集動作1 - サブキャレットを表示する.
    """

    # --- 1. Hex pane active (sub-caret in the text pane) ---

    @pytest.mark.original
    def test_original_subcaret_hex_active_toggle(self, original_exe_path, tmp_path):
        """The setting must change what is drawn in the text pane."""
        test_file = tmp_path / "subcaret_hex_orig.dat"
        test_file.write_bytes(DATA)

        on_zero, _ = _capture_positions(original_exe_path, test_file, True, False)
        off_zero, _ = _capture_positions(original_exe_path, test_file, False, False)
        assert _changed_columns(on_zero, off_zero), "no sub-caret was drawn when enabled"

    @pytest.mark.original
    def test_original_subcaret_hex_active_tracking(self, original_exe_path, tmp_path):
        """The sub-caret in the text pane follows the caret in the hex pane."""
        test_file = tmp_path / "subcaret_hex_track_orig.dat"
        test_file.write_bytes(DATA)

        _assert_followed_the_caret(*_subcaret_columns(original_exe_path, test_file, False))

    @pytest.mark.ported
    def test_ported_subcaret_hex_active_toggle(self, ported_exe_path, tmp_path):
        """The setting must change what is drawn in the text pane."""
        test_file = tmp_path / "subcaret_hex_port.dat"
        test_file.write_bytes(DATA)

        on_zero, _ = _capture_positions(ported_exe_path, test_file, True, False)
        off_zero, _ = _capture_positions(ported_exe_path, test_file, False, False)
        assert _changed_columns(on_zero, off_zero), "no sub-caret was drawn when enabled"

    @pytest.mark.ported
    def test_ported_subcaret_hex_active_tracking(self, ported_exe_path, tmp_path):
        """The sub-caret in the text pane follows the caret in the hex pane."""
        test_file = tmp_path / "subcaret_hex_track_port.dat"
        test_file.write_bytes(DATA)

        _assert_followed_the_caret(*_subcaret_columns(ported_exe_path, test_file, False))

    # --- 2. Text pane active (sub-caret in the hex pane) ---

    @pytest.mark.original
    def test_original_subcaret_text_active_toggle(self, original_exe_path, tmp_path):
        """The setting must change what is drawn in the hex pane."""
        test_file = tmp_path / "subcaret_txt_orig.dat"
        test_file.write_bytes(DATA)

        on_zero, _ = _capture_positions(original_exe_path, test_file, True, True)
        off_zero, _ = _capture_positions(original_exe_path, test_file, False, True)
        assert _changed_columns(on_zero, off_zero), "no sub-caret was drawn when enabled"

    @pytest.mark.original
    def test_original_subcaret_text_active_tracking(self, original_exe_path, tmp_path):
        """The sub-caret in the hex pane follows the caret in the text pane."""
        test_file = tmp_path / "subcaret_txt_track_orig.dat"
        test_file.write_bytes(DATA)

        _assert_followed_the_caret(*_subcaret_columns(original_exe_path, test_file, True))

    @pytest.mark.ported
    def test_ported_subcaret_text_active_toggle(self, ported_exe_path, tmp_path):
        """The setting must change what is drawn in the hex pane."""
        test_file = tmp_path / "subcaret_txt_port.dat"
        test_file.write_bytes(DATA)

        on_zero, _ = _capture_positions(ported_exe_path, test_file, True, True)
        off_zero, _ = _capture_positions(ported_exe_path, test_file, False, True)
        assert _changed_columns(on_zero, off_zero), "no sub-caret was drawn when enabled"

    @pytest.mark.ported
    def test_ported_subcaret_text_active_tracking(self, ported_exe_path, tmp_path):
        """The sub-caret in the hex pane follows the caret in the text pane."""
        test_file = tmp_path / "subcaret_txt_track_port.dat"
        test_file.write_bytes(DATA)

        _assert_followed_the_caret(*_subcaret_columns(ported_exe_path, test_file, True))
