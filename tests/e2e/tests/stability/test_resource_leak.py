"""Resource leak tests: repeat one operation and watch the counters (Issue #224).

Each test opens StirHex once, repeats a single operation, and requires the GDI, USER,
handle and thread counts to come back to where they were. Memory is judged by its trend
instead, because caches and the undo history move it for legitimate reasons.

Every operation is proven to do something before the loop starts: a loop over a command
the editor ignored would report "no leak" while measuring nothing (Issue #204).

Run them with:
    uv run pytest tests/stability --stability
"""

import time

import pytest
import win32clipboard
import win32con
import win32gui

from drivers.resource_probe import ResourceProbe
from drivers.stirling_driver import (
    CMD_FILE_CLOSE,
    CMD_FILE_NEW,
    ID_BITIMAGE,
    ID_CHARSET_ASCII,
    ID_CHARSET_SJIS,
    ID_FILE_PRINT_PREVIEW,
    ID_GOTO_DATA_END,
    ID_GOTO_DATA_TOP,
    ID_OUTPUT_PANE,
    ID_STRUCT_EDIT,
    StirlingDriver,
)

IDCANCEL = 2


def _wait_until(predicate, timeout: float = 5.0, interval: float = 0.1) -> bool:
    """Poll `predicate` until it is true or `timeout` elapses."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(interval)
    return predicate()


def _caret_address(editor: StirlingDriver) -> int:
    """Caret address as the status bar reports it (pane 1, e.g. '0x00000010')."""
    text = editor.get_statusbar_pane_text(1).strip()
    assert text.startswith("0x"), f"unexpected address pane text: {text!r}"
    return int(text, 16)


def _charset_name(editor: StirlingDriver) -> str:
    """Character set as the status bar reports it (last pane, e.g. 'SHIFT-JIS')."""
    panes = editor.get_all_statusbar_text()
    assert panes, "status bar reported no panes"
    return panes[-1].strip()


def _document_text(editor: StirlingDriver) -> str:
    """The whole document, read back through select-all + copy.

    Used to prove that an edit and its undo really changed and restored the data. It goes
    through the clipboard, so it is only used outside the measured loop.
    """
    editor.select_all()
    editor.copy()
    for _ in range(20):
        try:
            win32clipboard.OpenClipboard()
        except Exception:
            time.sleep(0.1)
            continue
        try:
            if not win32clipboard.IsClipboardFormatAvailable(win32con.CF_UNICODETEXT):
                return ""
            return win32clipboard.GetClipboardData(win32con.CF_UNICODETEXT)
        finally:
            win32clipboard.CloseClipboard()
    raise AssertionError("Could not open the clipboard to read the document back")


@pytest.fixture
def editor(ported_exe_path, sample_binary_file):
    """A running StirHex with one document open, closed again after the test."""
    with StirlingDriver(ported_exe_path) as drv:
        drv.start(sample_binary_file)
        assert drv.hwnd != 0, "StirHex did not open its main window"
        yield drv


@pytest.mark.ported
class TestWindowAndBarLeaks:
    """Windows, bars and panes: creating and destroying them must balance out."""

    def test_document_window_open_close(self, editor, leak_check):
        """A new document window and its close must return every GDI/USER object."""
        base_count = len(editor.get_mdi_child_titles())

        def operation(iteration):
            editor.post_command(CMD_FILE_NEW)
            assert _wait_until(
                lambda: len(editor.get_mdi_child_titles()) == base_count + 1
            ), f"new document window did not appear on iteration {iteration}"
            editor.post_command(CMD_FILE_CLOSE)
            assert _wait_until(
                lambda: len(editor.get_mdi_child_titles()) == base_count
            ), f"document window did not close on iteration {iteration}"

        leak_check(editor.pid, operation, label="document_window_open_close")

    def test_struct_bar_toggle(self, editor, leak_check):
        """Showing and hiding the struct edit bar must not accumulate objects."""
        visible_at_start = editor.is_struct_bar_visible()

        def operation(iteration):
            editor.post_command(ID_STRUCT_EDIT)
            assert _wait_until(
                lambda: editor.is_struct_bar_visible() != visible_at_start
            ), f"struct bar did not change visibility on iteration {iteration}"
            editor.post_command(ID_STRUCT_EDIT)
            assert _wait_until(
                lambda: editor.is_struct_bar_visible() == visible_at_start
            ), f"struct bar did not return to its original state on iteration {iteration}"

        try:
            leak_check(editor.pid, operation, label="struct_bar_toggle")
        finally:
            # The bar's visibility is persisted, so leave it as this test found it.
            if editor.is_struct_bar_visible() != visible_at_start:
                editor.post_command(ID_STRUCT_EDIT)

    def test_bit_image_toggle(self, editor, leak_check):
        """The bit image window creates a DIB section each time it is shown."""
        visible_at_start = editor.is_bit_image_visible()

        def operation(iteration):
            editor.post_command(ID_BITIMAGE)
            assert _wait_until(
                lambda: editor.is_bit_image_visible() != visible_at_start
            ), f"bit image did not change visibility on iteration {iteration}"
            editor.post_command(ID_BITIMAGE)
            assert _wait_until(
                lambda: editor.is_bit_image_visible() == visible_at_start
            ), f"bit image did not return to its original state on iteration {iteration}"

        try:
            leak_check(editor.pid, operation, label="bit_image_toggle")
        finally:
            if editor.is_bit_image_visible() != visible_at_start:
                editor.post_command(ID_BITIMAGE)

    def test_output_pane_toggle(self, editor, leak_check):
        """Showing and hiding the output pane must balance out."""
        visible_at_start = editor.is_output_pane_visible()

        def operation(iteration):
            editor.post_command(ID_OUTPUT_PANE)
            assert _wait_until(
                lambda: editor.is_output_pane_visible() != visible_at_start
            ), f"output pane did not change visibility on iteration {iteration}"
            editor.post_command(ID_OUTPUT_PANE)
            assert _wait_until(
                lambda: editor.is_output_pane_visible() == visible_at_start
            ), f"output pane did not return to its original state on iteration {iteration}"

        try:
            leak_check(editor.pid, operation, label="output_pane_toggle")
        finally:
            if editor.is_output_pane_visible() != visible_at_start:
                editor.post_command(ID_OUTPUT_PANE)

    def test_print_preview_open_close(self, editor, leak_check):
        """Print preview builds fonts and DCs per page; closing must release them."""

        def operation(iteration):
            editor.post_command(ID_FILE_PRINT_PREVIEW)
            assert _wait_until(editor.is_print_preview_active), (
                f"print preview did not open on iteration {iteration}"
            )
            editor.close_print_preview()
            assert _wait_until(lambda: not editor.is_print_preview_active()), (
                f"print preview did not close on iteration {iteration}"
            )

        # Preview is slow (it lays out pages), so it runs fewer iterations. A leak here is
        # large enough per iteration that a short loop still shows it.
        leak_check(
            editor.pid,
            operation,
            label="print_preview_open_close",
            iterations=8,
            warmup=2,
            settle=0.6,
        )


@pytest.mark.ported
class TestDialogLeaks:
    """Dialogs: every open must be matched by the close that follows it."""

    def test_env_settings_dialog(self, editor, leak_check):
        """The environment settings sheet has eight pages of controls to release."""

        def operation(iteration):
            sheet = editor.open_env_settings_dialog(timeout=5.0)
            StirlingDriver.click_dialog_button(sheet, IDCANCEL)
            assert _wait_until(lambda: not win32gui.IsWindow(sheet)), (
                f"settings sheet stayed open on iteration {iteration}"
            )

        leak_check(
            editor.pid,
            operation,
            label="env_settings_dialog",
            iterations=10,
            warmup=2,
        )

    def test_jump_dialog(self, editor, leak_check):
        """Opening the jump dialog and moving the caret must not accumulate objects."""

        def operation(iteration):
            editor.jump_to_address("10")
            assert _caret_address(editor) == 0x10, (
                f"jump dialog did not move the caret on iteration {iteration}"
            )
            editor.post_command(ID_GOTO_DATA_TOP)
            assert _wait_until(lambda: _caret_address(editor) == 0), (
                f"caret did not return to the top on iteration {iteration}"
            )

        leak_check(editor.pid, operation, label="jump_dialog", iterations=10, warmup=2)


@pytest.mark.ported
class TestViewOperationLeaks:
    """Operations inside the view: painting, editing and undo."""

    def test_navigation_and_repaint(self, editor, leak_check, sample_binary_file):
        """Scrolling end to end repaints the whole view on every iteration."""
        end_address = sample_binary_file.stat().st_size

        def operation(iteration):
            editor.post_command(ID_GOTO_DATA_END)
            assert _wait_until(lambda: _caret_address(editor) == end_address), (
                f"caret did not reach the end of data on iteration {iteration}"
            )
            editor.post_command(ID_GOTO_DATA_TOP)
            assert _wait_until(lambda: _caret_address(editor) == 0), (
                f"caret did not return to the top on iteration {iteration}"
            )

        leak_check(editor.pid, operation, label="navigation_and_repaint")

    def test_charset_switch(self, editor, leak_check):
        """Switching the character set re-renders the text pane on every iteration."""
        charset_at_start = _charset_name(editor)
        editor.post_command(ID_CHARSET_ASCII)
        assert _wait_until(lambda: _charset_name(editor) == "ASCII"), (
            f"status bar still reports {_charset_name(editor)!r} after switching to ASCII"
        )

        def operation(iteration):
            editor.post_command(ID_CHARSET_SJIS)
            assert _wait_until(lambda: _charset_name(editor) == "SHIFT-JIS"), (
                f"charset did not switch to SHIFT-JIS on iteration {iteration}"
            )
            editor.post_command(ID_CHARSET_ASCII)
            assert _wait_until(lambda: _charset_name(editor) == "ASCII"), (
                f"charset did not switch back to ASCII on iteration {iteration}"
            )

        try:
            leak_check(editor.pid, operation, label="charset_switch")
        finally:
            # The charset is a per-document display setting; put it back for the next test.
            if charset_at_start == "SHIFT-JIS":
                editor.post_command(ID_CHARSET_SJIS)

    def test_edit_and_undo(self, editor, leak_check):
        """One overwrite plus undo must return to the same document and the same memory.

        The undo history keeps the recorded edit alive until it is undone, so the counters
        only balance because every iteration undoes what it typed.
        """
        editor.post_command(ID_GOTO_DATA_TOP)
        original = _document_text(editor)
        assert original, "could not read the document back through the clipboard"

        # Prove once that the loop's operation really edits and really undoes. Reading the
        # document back goes through the clipboard, which allocates on both sides, so it
        # stays outside the measured loop.
        editor.post_command(ID_GOTO_DATA_TOP)
        editor.type_hex_chars("A5")
        assert _document_text(editor) != original, "typing did not change the document"
        editor.undo()
        assert _document_text(editor) == original, "undo did not restore the document"

        def operation(_iteration):
            editor.post_command(ID_GOTO_DATA_TOP)
            editor.type_hex_chars("A5")
            editor.undo()

        leak_check(editor.pid, operation, label="edit_and_undo")

        # The loop must have left the document exactly as it found it; otherwise the
        # iterations were not equivalent and the counters mean nothing.
        editor.post_command(ID_GOTO_DATA_TOP)
        assert _document_text(editor) == original, (
            "the edit/undo loop did not leave the document unchanged"
        )


@pytest.mark.ported
def test_idle_process_is_quiet(editor):
    """An idle editor must not accumulate resources while it just sits there.

    StirHex runs a 150 ms refresh timer for the struct bar, so "idle" still executes code.
    This checks that doing nothing for a while leaves the counters where they were.
    """
    with ResourceProbe(editor.pid) as probe:
        time.sleep(2.0)
        before = probe.sample()
        time.sleep(20.0)
        after = probe.sample()

    delta = after.delta(before)
    growth = {
        name: value
        for name, value in delta.items()
        if name in ("gdi_objects", "user_objects", "handles", "threads") and value > 0
    }
    assert not growth, f"idle editor accumulated resources over 20 s: {growth}"
    # 1 MiB over 20 idle seconds would be 3 MiB a minute - far more than an idle timer
    # should ever need.
    assert delta["private_bytes"] < 1024 * 1024, (
        f"idle editor grew private bytes by {delta['private_bytes'] / 1024:.0f} KiB in 20 s"
    )


@pytest.mark.ported
def test_process_survives_the_loop(editor):
    """A long burst of mixed operations must not crash or hang the editor."""
    for _ in range(20):
        editor.post_command(ID_GOTO_DATA_END)
        editor.post_command(ID_GOTO_DATA_TOP)
        editor.post_command(ID_STRUCT_EDIT)
        editor.post_command(ID_STRUCT_EDIT)
        editor.post_command(CMD_FILE_NEW)
        editor.post_command(CMD_FILE_CLOSE)

    assert win32gui.IsWindow(editor.hwnd), "main window disappeared during the burst"
    # A hung message loop is as bad as a crash: the window must still answer.
    result = win32gui.SendMessageTimeout(
        editor.hwnd, win32con.WM_NULL, 0, 0, win32con.SMTO_ABORTIFHUNG, 5000
    )
    assert result is not None, "main window stopped answering messages"
