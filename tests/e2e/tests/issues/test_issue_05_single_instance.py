import time
import subprocess
import pytest
from pathlib import Path
from pywinauto import timings
from drivers.stirling_driver import StirlingDriver


class TestIssue05SingleInstance:
    """Tests for Issue #5: Single-instance mutex and inter-process file passing."""

    @pytest.mark.original
    def test_original_single_instance_behavior(self, original_exe_path, tmp_path):
        """Verify Original Stirling prevents multi-instances and forwards command line file."""
        file1 = tmp_path / "inst1.dat"
        file2 = tmp_path / "inst2.dat"
        file1.write_bytes(b"FILE_1_CONTENT")
        file2.write_bytes(b"FILE_2_CONTENT")

        with StirlingDriver(original_exe_path) as drv1:
            drv1.start(file1)
            time.sleep(0.5)

            # Launch 2nd process targeting file2
            proc2 = subprocess.Popen([str(original_exe_path), str(file2)])

            # The 2nd process should terminate quickly after delegating to the 1st instance.
            # Kill it if it does not: a surviving original Stirling holds the single-instance
            # mutex and makes every later launch in the session fail (Issue #113).
            try:
                proc2.wait(timeout=5)
                assert proc2.returncode is not None, "2nd instance did not terminate"
            finally:
                if proc2.poll() is None:
                    proc2.kill()

            # Check that the 1st instance has opened the new document
            time.sleep(0.5)
            titles = drv1.get_mdi_child_titles()
            assert len(titles) == 2, f"Expected 2 documents in 1st instance, got {titles}"
            assert any("inst1.dat" in t for t in titles)
            assert any("inst2.dat" in t for t in titles)

    @pytest.mark.ported
    def test_ported_single_instance_behavior(self, ported_exe_path, tmp_path):
        """Verify Ported Stirling prevents multi-instances and forwards command line file."""
        file1 = tmp_path / "p_inst1.dat"
        file2 = tmp_path / "p_inst2.dat"
        file1.write_bytes(b"PORTED_FILE_1")
        file2.write_bytes(b"PORTED_FILE_2")

        with StirlingDriver(ported_exe_path) as drv1:
            drv1.start(file1)
            time.sleep(0.5)

            # Launch 2nd process targeting file2
            proc2 = subprocess.Popen([str(ported_exe_path), str(file2)])

            try:
                proc2.wait(timeout=5)
                assert proc2.returncode is not None, "2nd instance should terminate in single-instance mode"
            finally:
                if proc2.poll() is None:
                    proc2.kill()

            time.sleep(0.5)
            titles = drv1.get_mdi_child_titles()
            assert len(titles) == 2, f"Expected 2 documents in 1st instance, got {titles}"
            assert any("p_inst1.dat" in t for t in titles)
            assert any("p_inst2.dat" in t for t in titles)
