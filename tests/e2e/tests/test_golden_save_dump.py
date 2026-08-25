import pytest
from pathlib import Path
from drivers.stirling_driver import StirlingDriver


class TestGoldenSaveDump:
    """Golden comparison tests for Save Dump (Text Dump Output) functionality."""

    @pytest.mark.golden
    @pytest.mark.ported
    @pytest.mark.original
    def test_golden_save_dump_whole(self, run_both_stirling):
        """Save formatted text dump of the entire binary document and compare original vs ported."""
        test_data = bytes(range(64)) + b"HELLO_STIRLING_TEXT_DUMP_GOLDEN_TEST"

        def action(drv: StirlingDriver, out_path: Path):
            drv.save_dump_via_dialog(out_path)

        orig_out, port_out = run_both_stirling(action, test_data)

        # 1. Output must not be empty
        assert len(orig_out) > 0, "Original dump output is empty!"
        assert len(port_out) > 0, "Ported dump output is empty!"

        # 2. Golden comparison: dump text must match identically
        assert orig_out == port_out, "Ported dump text output does not match Original Stirling output!"
