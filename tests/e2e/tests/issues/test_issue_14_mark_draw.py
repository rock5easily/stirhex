"""Issue #14: a mark inside the structure highlight must not colour the separating space.

The hex pane draws "XX XX XX ..."; a marked byte gets its own background for the two
digits, and the space that follows keeps the normal data background. While a structure is
selected the port used to extend the mark colour over that space as well.

The check needs no font metrics: it marks one byte inside the structure highlight and one
outside it, and compares how wide the redrawn area is in each case. Colouring the extra
space makes the highlighted one wider - which is exactly the defect - while a correct
build redraws the same two digit cells in both places.

The jump-to-mark case that used to live here only asserted that the structure address
stayed "00000000", which the initial address satisfies on its own, so it could pass with
the navigation broken; test_issue_06_struct_bar.py covers that navigation with a non-zero
address (Issue #204).
"""

import time

import pytest

from drivers.settings_context import stirling_settings
from drivers.stirling_driver import StirlingDriver, safe_set_focus

# LOGFONT covers 0x00-0x3B, so 0x02 is inside the highlight and 0x42 is outside it.
MARK_INSIDE = "2"
MARK_OUTSIDE = "42"
# Parking the caret away from both rows keeps its blinking out of the comparison.
CARET_PARK = "F0"


def _changed_run_width(before, after) -> int:
    """Width, in pixels, of the widest horizontal run of pixels that differ."""
    pixels_before, width, height = before
    pixels_after, width_after, height_after = after
    assert (width, height) == (width_after, height_after), "the view must not be resized"

    widest = 0
    for row in range(height):
        base = row * width * 4
        run = 0
        for column in range(width):
            offset = base + column * 4
            if pixels_before[offset:offset + 4] != pixels_after[offset:offset + 4]:
                run += 1
                if run > widest:
                    widest = run
            else:
                run = 0
    return widest


def _mark_and_measure(drv: StirlingDriver, address: str, baseline) -> int:
    """Toggle a mark at `address`, park the caret, and report the redrawn width."""
    drv.jump_to_address(address, is_hex=True)
    time.sleep(0.2)
    drv.mark_toggle()
    time.sleep(0.2)
    drv.jump_to_address(CARET_PARK, is_hex=True)
    time.sleep(0.4)
    width = _changed_run_width(baseline, drv.capture_view_image())

    # Undo the mark so the next measurement starts from the same picture.
    drv.jump_to_address(address, is_hex=True)
    time.sleep(0.2)
    drv.mark_toggle()
    time.sleep(0.2)
    drv.jump_to_address(CARET_PARK, is_hex=True)
    time.sleep(0.4)
    return width


def _measure_mark_widths(exe_path, test_file) -> tuple[int, int]:
    """(inside, outside) redraw widths for a mark in and out of the structure highlight."""
    # The structure highlight must stay on 0x00 while the caret moves, and marks must not
    # follow the caret, so both settings are pinned for the measurement.
    with stirling_settings(auto_set_struct_addr=False, dynamic_mark=False):
        with StirlingDriver(exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)

            drv.toggle_struct_bar(show=True)
            time.sleep(0.3)
            drv.select_struct_type("LOGFONT")
            time.sleep(0.3)

            drv.jump_to_address(CARET_PARK, is_hex=True)
            time.sleep(0.4)
            baseline = drv.capture_view_image()
            assert baseline is not None, "the view must be capturable"

            inside = _mark_and_measure(drv, MARK_INSIDE, baseline)
            outside = _mark_and_measure(drv, MARK_OUTSIDE, baseline)
            return inside, outside


class TestIssue14MarkDraw:
    """Issue #14: mark background bounds inside the structure edit highlight."""

    @pytest.mark.original
    def test_original_mark_does_not_paint_the_separating_space(self, original_exe_path, tmp_path):
        """The original paints two digit cells whether or not the byte is highlighted."""
        test_file = tmp_path / "mark_draw_orig.dat"
        test_file.write_bytes(bytes(range(256)))

        inside, outside = _measure_mark_widths(original_exe_path, test_file)
        assert inside > 0 and outside > 0, "marking must change the drawing at all"
        assert inside == outside, (
            f"the highlighted mark is drawn {inside}px wide but a plain mark {outside}px"
        )

    @pytest.mark.ported
    def test_ported_mark_does_not_paint_the_separating_space(self, ported_exe_path, tmp_path):
        """The port must not extend the mark colour over the separator (Issue #14)."""
        test_file = tmp_path / "mark_draw_port.dat"
        test_file.write_bytes(bytes(range(256)))

        inside, outside = _measure_mark_widths(ported_exe_path, test_file)
        assert inside > 0 and outside > 0, "marking must change the drawing at all"
        assert inside == outside, (
            f"the highlighted mark is drawn {inside}px wide but a plain mark {outside}px"
        )
