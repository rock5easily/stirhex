"""Save As onto an existing file: the overwrite prompt (Issue #203).

`save_as_via_dialog` deletes an existing destination before opening the dialog, so every
test that uses it saves onto a free path and the overwrite confirmation of the common
file dialog is never reached. These cases keep the destination in place and answer the
prompt both ways, checking what survives when it is declined.
"""

import time
from pathlib import Path

import pytest

from drivers.stirling_driver import StirlingDriver, safe_set_focus

SOURCE = bytes(range(64))
DESTINATION = b"KEEP-ME" * 8
# Typing "10" over the first byte of the source document.
EDITED = b"\x10" + SOURCE[1:]


def _titles(drv: StirlingDriver) -> list[str]:
    return [title.rstrip("*").strip() for title in drv.get_mdi_child_titles()]


def _is_dirty(drv: StirlingDriver) -> bool:
    return any(title.endswith("*") for title in drv.get_mdi_child_titles())


class TestSaveAsOverwrite:
    @pytest.mark.ported
    def test_declined_overwrite_keeps_destination_and_document(self, ported_exe_path, tmp_path):
        source = tmp_path / "source.dat"
        destination = tmp_path / "destination.dat"
        source.write_bytes(SOURCE)
        destination.write_bytes(DESTINATION)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(source)
            safe_set_focus(drv.hwnd)
            time.sleep(0.2)
            drv.type_hex_chars("10")
            time.sleep(0.2)
            assert _is_dirty(drv)

            saved = drv.save_as_via_dialog(destination, remove_existing=False, overwrite="no")
            assert saved is False, "declining the prompt must not save"

            assert destination.read_bytes() == DESTINATION, "the destination is untouched"
            assert source.read_bytes() == SOURCE, "the source file is untouched as well"
            assert any(title.startswith(source.name) for title in _titles(drv)), (
                "the document keeps its own path when the save is abandoned"
            )
            assert _is_dirty(drv), "the unsaved edit is still unsaved"

    @pytest.mark.ported
    def test_accepted_overwrite_replaces_destination(self, ported_exe_path, tmp_path):
        source = tmp_path / "source.dat"
        destination = tmp_path / "destination.dat"
        source.write_bytes(SOURCE)
        destination.write_bytes(DESTINATION)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(source)
            safe_set_focus(drv.hwnd)
            time.sleep(0.2)
            drv.type_hex_chars("10")
            time.sleep(0.2)

            saved = drv.save_as_via_dialog(destination, remove_existing=False, overwrite="yes")
            assert saved is True

            assert not _is_dirty(drv), "a completed Save As clears the modified mark"
            assert any(title.startswith(destination.name) for title in _titles(drv)), (
                "the document follows the new path"
            )

        assert destination.read_bytes() == EDITED, "the destination holds the edited document"
        assert source.read_bytes() == SOURCE, "Save As leaves the original file alone"
