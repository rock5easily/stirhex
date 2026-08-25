import time
import pytest
from pathlib import Path
from drivers.stirling_driver import StirlingDriver
from drivers.settings_context import stirling_settings


class TestIssue02SubCaret:
    """Tests for Issue #2: Sub-caret rendering in the inactive pane.
    - Hex pane active -> Sub-caret rendered in Text pane
    - Text pane active -> Sub-caret rendered in Hex pane
    
    Prerequisite settings:
    - '環境設定' - '編集動作1' -> 'サブキャレットを表示する':
      - Enabled (show_sub_caret = True, Default): Renders sub-caret underline in inactive pane.
      - Disabled (show_sub_caret = False): Sub-caret is not rendered.
    """

    # --- 1. Hex Pane Active (Sub-caret in Text Pane) ---

    @pytest.mark.original
    def test_original_subcaret_hex_active_toggle(self, original_exe_path, tmp_path):
        """Verify Original Stirling renders sub-caret in Text pane when Hex pane is active."""
        test_file = tmp_path / "subcaret_hex_orig.dat"
        test_file.write_bytes(b"A" * 64)

        # 1. Capture view with SubCaret ON
        with stirling_settings(show_sub_caret=True):
            with StirlingDriver(original_exe_path) as drv:
                drv.start(test_file)
                drv.focus_view()
                time.sleep(0.4)
                bits_on = drv.capture_view_pixels()

        # 2. Capture view with SubCaret OFF
        with stirling_settings(show_sub_caret=False):
            with StirlingDriver(original_exe_path) as drv:
                drv.start(test_file)
                drv.focus_view()
                time.sleep(0.4)
                bits_off = drv.capture_view_pixels()

        assert bits_on is not None and bits_off is not None
        diff_count = sum(1 for b1, b2 in zip(bits_on, bits_off) if b1 != b2)
        assert diff_count > 0, "Original Stirling did not produce visual difference when SubCaret was toggled"

    @pytest.mark.original
    def test_original_subcaret_hex_active_tracking(self, original_exe_path, tmp_path):
        """Verify Original Stirling sub-caret in Text pane follows caret movement in Hex pane."""
        test_file = tmp_path / "subcaret_hex_track_orig.dat"
        test_file.write_bytes(b"A" * 64)

        with stirling_settings(show_sub_caret=True):
            with StirlingDriver(original_exe_path) as drv:
                drv.start(test_file)
                drv.focus_view()
                time.sleep(0.3)

                drv.jump_to_address("0", is_hex=True)
                time.sleep(0.3)
                bits_pos0 = drv.capture_view_pixels()

                drv.jump_to_address("1", is_hex=True)
                time.sleep(0.3)
                bits_pos1 = drv.capture_view_pixels()

        assert bits_pos0 is not None and bits_pos1 is not None
        diff_count = sum(1 for b1, b2 in zip(bits_pos0, bits_pos1) if b1 != b2)
        assert diff_count > 0, "Original Stirling sub-caret did not follow caret movement"

    @pytest.mark.ported
    def test_ported_subcaret_hex_active_toggle(self, ported_exe_path, tmp_path):
        """Verify Ported Stirling renders sub-caret in Text pane when Hex pane is active."""
        test_file = tmp_path / "subcaret_hex_port.dat"
        test_file.write_bytes(b"A" * 64)

        with stirling_settings(show_sub_caret=True):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                drv.focus_view()
                time.sleep(0.4)
                bits_on = drv.capture_view_pixels()

        with stirling_settings(show_sub_caret=False):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                drv.focus_view()
                time.sleep(0.4)
                bits_off = drv.capture_view_pixels()

        assert bits_on is not None and bits_off is not None
        diff_count = sum(1 for b1, b2 in zip(bits_on, bits_off) if b1 != b2)
        assert diff_count > 0, "Ported Stirling did not produce visual difference when SubCaret was toggled"

    @pytest.mark.ported
    def test_ported_subcaret_hex_active_tracking(self, ported_exe_path, tmp_path):
        """Verify Ported Stirling sub-caret in Text pane follows caret movement in Hex pane."""
        test_file = tmp_path / "subcaret_hex_track_port.dat"
        test_file.write_bytes(b"A" * 64)

        with stirling_settings(show_sub_caret=True):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                drv.focus_view()
                time.sleep(0.3)

                drv.jump_to_address("0", is_hex=True)
                time.sleep(0.3)
                bits_pos0 = drv.capture_view_pixels()

                drv.jump_to_address("1", is_hex=True)
                time.sleep(0.3)
                bits_pos1 = drv.capture_view_pixels()

        assert bits_pos0 is not None and bits_pos1 is not None
        diff_count = sum(1 for b1, b2 in zip(bits_pos0, bits_pos1) if b1 != b2)
        assert diff_count > 0, "Ported Stirling sub-caret did not follow caret movement"

    # --- 2. Text Pane Active (Sub-caret in Hex Pane) ---

    @pytest.mark.original
    def test_original_subcaret_text_active_toggle(self, original_exe_path, tmp_path):
        """Verify Original Stirling renders sub-caret in Hex pane when Text pane is active."""
        test_file = tmp_path / "subcaret_txt_orig.dat"
        test_file.write_bytes(b"A" * 64)

        with stirling_settings(show_sub_caret=True):
            with StirlingDriver(original_exe_path) as drv:
                drv.start(test_file)
                drv.focus_view()
                time.sleep(0.3)
                drv.press_tab()
                time.sleep(0.3)
                bits_on = drv.capture_view_pixels()

        with stirling_settings(show_sub_caret=False):
            with StirlingDriver(original_exe_path) as drv:
                drv.start(test_file)
                drv.focus_view()
                time.sleep(0.3)
                drv.press_tab()
                time.sleep(0.3)
                bits_off = drv.capture_view_pixels()

        assert bits_on is not None and bits_off is not None
        diff_count = sum(1 for b1, b2 in zip(bits_on, bits_off) if b1 != b2)
        assert diff_count > 0, "Original Stirling did not produce visual difference when SubCaret was toggled in Text pane"

    @pytest.mark.original
    def test_original_subcaret_text_active_tracking(self, original_exe_path, tmp_path):
        """Verify Original Stirling sub-caret in Hex pane follows caret movement in Text pane."""
        test_file = tmp_path / "subcaret_txt_track_orig.dat"
        test_file.write_bytes(b"A" * 64)

        with stirling_settings(show_sub_caret=True):
            with StirlingDriver(original_exe_path) as drv:
                drv.start(test_file)
                drv.focus_view()
                time.sleep(0.3)
                drv.press_tab()
                time.sleep(0.3)

                drv.jump_to_address("0", is_hex=True)
                time.sleep(0.3)
                bits_pos0 = drv.capture_view_pixels()

                drv.jump_to_address("1", is_hex=True)
                time.sleep(0.3)
                bits_pos1 = drv.capture_view_pixels()

        assert bits_pos0 is not None and bits_pos1 is not None
        diff_count = sum(1 for b1, b2 in zip(bits_pos0, bits_pos1) if b1 != b2)
        assert diff_count > 0, "Original Stirling sub-caret did not follow caret movement in Text pane"

    @pytest.mark.ported
    def test_ported_subcaret_text_active_toggle(self, ported_exe_path, tmp_path):
        """Verify Ported Stirling renders sub-caret in Hex pane when Text pane is active."""
        test_file = tmp_path / "subcaret_txt_port.dat"
        test_file.write_bytes(b"A" * 64)

        with stirling_settings(show_sub_caret=True):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                drv.focus_view()
                time.sleep(0.3)
                drv.press_tab()
                time.sleep(0.3)
                bits_on = drv.capture_view_pixels()

        with stirling_settings(show_sub_caret=False):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                drv.focus_view()
                time.sleep(0.3)
                drv.press_tab()
                time.sleep(0.3)
                bits_off = drv.capture_view_pixels()

        assert bits_on is not None and bits_off is not None
        diff_count = sum(1 for b1, b2 in zip(bits_on, bits_off) if b1 != b2)
        assert diff_count > 0, "Ported Stirling did not produce visual difference when SubCaret was toggled in Text pane"

    @pytest.mark.ported
    def test_ported_subcaret_text_active_tracking(self, ported_exe_path, tmp_path):
        """Verify Ported Stirling sub-caret in Hex pane follows caret movement in Text pane."""
        test_file = tmp_path / "subcaret_txt_track_port.dat"
        test_file.write_bytes(b"A" * 64)

        with stirling_settings(show_sub_caret=True):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                drv.focus_view()
                time.sleep(0.3)
                drv.press_tab()
                time.sleep(0.3)

                drv.jump_to_address("0", is_hex=True)
                time.sleep(0.3)
                bits_pos0 = drv.capture_view_pixels()

                drv.jump_to_address("1", is_hex=True)
                time.sleep(0.3)
                bits_pos1 = drv.capture_view_pixels()

        assert bits_pos0 is not None and bits_pos1 is not None
        diff_count = sum(1 for b1, b2 in zip(bits_pos0, bits_pos1) if b1 != b2)
        assert diff_count > 0, "Ported Stirling sub-caret did not follow caret movement in Text pane"
