import pytest
from pathlib import Path
from drivers.stirling_driver import StirlingDriver


class TestGoldenPassthrough:
    """Golden comparison tests for loading files and saving without edit (Pass-through)."""

    @pytest.mark.original
    def test_original_smoke(self, original_exe_path):
        """Verify original Stirling launches and closes cleanly."""
        with StirlingDriver(original_exe_path) as drv:
            drv.start()
            assert drv.hwnd != 0

    @pytest.mark.ported
    def test_ported_smoke(self, ported_exe_path):
        """Verify ported Stirling launches and closes cleanly."""
        with StirlingDriver(ported_exe_path) as drv:
            drv.start()
            assert drv.hwnd != 0

    @pytest.mark.golden
    @pytest.mark.ported
    @pytest.mark.original
    def test_golden_load_and_save_as(self, run_both_stirling):
        """Open a binary file and Save As without any edits.
        Verify both original and ported produce byte-identical files to input."""
        test_data = bytes(range(256)) + b"Stirling Golden Verification Data 2026\x00\xFF\xAA\x55"

        def action(drv: StirlingDriver, out_path: Path):
            drv.save_as_via_dialog(out_path)

        orig_out, port_out = run_both_stirling(action, test_data)

        # 1. Output must not be empty
        assert len(orig_out) == len(test_data), f"Original output size mismatch: {len(orig_out)} != {len(test_data)}"
        assert len(port_out) == len(test_data), f"Ported output size mismatch: {len(port_out)} != {len(test_data)}"

        # 2. Golden comparison: Original == Ported
        assert orig_out == port_out, "Ported output does not match Original Stirling output!"
        # 3. Preservation comparison: Output == Input
        assert port_out == test_data, "Ported output corrupted initial binary data!"
