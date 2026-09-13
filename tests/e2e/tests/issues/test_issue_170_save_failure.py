"""Save failure paths of CStirlingDoc::OnSaveDocument (Issue #203, related #170 / #186).

The core suite already covers the file-level contract of the temp-then-replace save
(`TestAtomicSave`: the target survives a failed replacement, temporaries are cleaned up).
What no test covered is the application side of the same failures: the notification, the
dirty state, the undo history and the exclusive lock that OnSaveDocument releases before
saving and re-acquires afterwards.

These cases therefore make the save fail from outside the process:

* backup failure - a directory occupies the ".bak" path, so CopyFile cannot write it
* save failure   - another handle holds the target with writing denied, so the replacement
                   probe in StreamFileWriter::Open reports a sharing violation

Both are reversible, so each case also checks that saving succeeds once the obstacle is
removed - that is what proves the document still held the edited data.
"""

import time
from pathlib import Path

import pytest
import pywintypes
import win32con
import win32file
import win32gui

from drivers.settings_context import stirling_settings
from drivers.stirling_driver import CMD_FILE_SAVE, StirlingDriver, safe_set_focus

IDYES = 6
IDNO = 7

ORIGINAL = bytes(range(64))
# Typing "10" over the first byte turns 0x00 into 0x10; nothing else changes.
EDITED = b"\x10" + ORIGINAL[1:]


def _is_dirty(drv: StirlingDriver) -> bool:
    """The frame title of a modified document ends with '*' (CStirlingDoc::SetModifiedFlag)."""
    return any(title.endswith("*") for title in drv.get_mdi_child_titles())


def _can_open(path: Path, access: int) -> bool:
    try:
        handle = win32file.CreateFile(
            str(path),
            access,
            win32con.FILE_SHARE_READ | win32con.FILE_SHARE_WRITE,
            None,
            win32con.OPEN_EXISTING,
            win32con.FILE_ATTRIBUTE_NORMAL,
            None,
        )
    except pywintypes.error:
        return False
    handle.Close()
    return True


def _hold_denying_write(path: Path):
    """Open the file so that others may read it but not write or replace it."""
    return win32file.CreateFile(
        str(path),
        win32con.GENERIC_READ,
        win32con.FILE_SHARE_READ,
        None,
        win32con.OPEN_EXISTING,
        win32con.FILE_ATTRIBUTE_NORMAL,
        None,
    )


def _prepare(tmp_path: Path, name: str) -> Path:
    target = tmp_path / name
    target.write_bytes(ORIGINAL)
    return target


def _edit_first_byte(drv: StirlingDriver) -> None:
    safe_set_focus(drv.hwnd)
    time.sleep(0.2)
    drv.type_hex_chars("10")
    time.sleep(0.2)


class TestSaveFailurePaths:
    @pytest.mark.ported
    def test_backup_failure_declined_keeps_everything(self, ported_exe_path, tmp_path):
        """Answering "no" to the backup warning aborts the save and changes nothing."""
        target = _prepare(tmp_path, "backup_declined.dat")
        # CreateBackup copies onto "<name>.bak"; a directory there makes CopyFile fail.
        (tmp_path / "backup_declined.dat.bak").mkdir()

        # Exclusive mode 0 so this test can read the file while the application runs;
        # the lock behaviour of the same path is checked separately below.
        with stirling_settings(file_exclusive_mode=0, BackupCreate=1, BackupGenerations=1):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(target)
                _edit_first_byte(drv)
                assert _is_dirty(drv), "the edit must mark the document as modified"

                drv.post_command(CMD_FILE_SAVE)
                text = drv.answer_message_box(IDNO)
                assert text.strip(), "the backup failure must be reported"

                assert target.read_bytes() == ORIGINAL, "an aborted save must not touch the file"
                assert _is_dirty(drv), "the document keeps its unsaved edit"

                # The undo record survived too: undoing returns the original bytes, and
                # saving them (backup now possible) writes them back.
                (tmp_path / "backup_declined.dat.bak").rmdir()
                drv.undo()
                time.sleep(0.2)
                drv.post_command(CMD_FILE_SAVE)
                time.sleep(0.5)

        assert target.read_bytes() == ORIGINAL
        assert (tmp_path / "backup_declined.dat.bak").read_bytes() == ORIGINAL

    @pytest.mark.ported
    def test_backup_failure_declined_restores_the_lock(self, ported_exe_path, tmp_path):
        """Declining the save must put the exclusive lock back (OnSaveDocument #170)."""
        target = _prepare(tmp_path, "backup_lock.dat")
        (tmp_path / "backup_lock.dat.bak").mkdir()

        with stirling_settings(file_exclusive_mode=2, BackupCreate=1, BackupGenerations=1):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(target)
                assert not _can_open(target, win32con.GENERIC_READ), "mode 2 locks the file"
                _edit_first_byte(drv)

                drv.post_command(CMD_FILE_SAVE)
                drv.answer_message_box(IDNO)

                # The lock is released before saving; declining has to re-acquire it.
                # A missing re-lock would show up here as a successful open.
                assert not _can_open(target, win32con.GENERIC_READ), "the lock must be restored"
                assert _is_dirty(drv), "the document keeps its unsaved edit"

        assert target.read_bytes() == ORIGINAL, "an aborted save must not touch the file"

    @pytest.mark.ported
    def test_backup_failure_accepted_saves_without_backup(self, ported_exe_path, tmp_path):
        """Answering "yes" saves the edit even though no backup could be written."""
        target = _prepare(tmp_path, "backup_accepted.dat")
        blocked = tmp_path / "backup_accepted.dat.bak"
        blocked.mkdir()

        with stirling_settings(BackupCreate=1, BackupGenerations=1):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(target)
                _edit_first_byte(drv)

                drv.post_command(CMD_FILE_SAVE)
                drv.answer_message_box(IDYES)
                time.sleep(0.5)

                assert target.read_bytes() == EDITED, "continuing must write the edited data"
                assert not _is_dirty(drv), "a completed save clears the modified mark"

        assert blocked.is_dir(), "the blocked backup path is still the directory we created"

    @pytest.mark.ported
    def test_save_failure_keeps_document_and_recovers(self, ported_exe_path, tmp_path):
        """A denied replacement reports the reason, keeps the data, and can be retried."""
        target = _prepare(tmp_path, "save_denied.dat")

        # Exclusive mode 0 so the application does not deny our own handle.
        with stirling_settings(file_exclusive_mode=0, BackupCreate=0):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(target)
                _edit_first_byte(drv)

                hold = _hold_denying_write(target)
                try:
                    drv.post_command(CMD_FILE_SAVE)
                    text = drv.answer_message_box(win32con.IDOK)
                    assert target.name in text or text.strip(), "the failure must be reported"
                    assert target.read_bytes() == ORIGINAL, "the original data must survive"
                finally:
                    hold.Close()

                # The document is still open with its edit: no data was lost by the failure.
                assert win32gui.IsWindow(drv.hwnd)
                assert _is_dirty(drv), "the document is still modified after a failed save"

                drv.post_command(CMD_FILE_SAVE)
                time.sleep(0.5)
                assert not _is_dirty(drv), "the retry succeeds once the file is free"

        assert target.read_bytes() == EDITED

    @pytest.mark.ported
    def test_exclusive_mode_and_backup_survive_a_successful_save(self, ported_exe_path, tmp_path):
        """Issue #170 order: the lock is released for the save and re-acquired afterwards."""
        target = _prepare(tmp_path, "exclusive_backup.dat")

        with stirling_settings(file_exclusive_mode=2, BackupCreate=1, BackupGenerations=1):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(target)
                assert not _can_open(target, win32con.GENERIC_READ), "mode 2 locks the file"

                _edit_first_byte(drv)
                drv.post_command(CMD_FILE_SAVE)
                time.sleep(0.6)

                assert not _is_dirty(drv), "the save completed"
                assert not _can_open(target, win32con.GENERIC_READ), "the lock is re-acquired"

        assert target.read_bytes() == EDITED
        assert (tmp_path / "exclusive_backup.dat.bak").read_bytes() == ORIGINAL
