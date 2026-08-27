import shutil
import struct
import time
from pathlib import Path

import pytest
import pywintypes
import win32con
import win32file
import win32gui

from drivers.settings_context import stirling_settings
from drivers.stirling_driver import CMD_FILE_SAVE, StirlingDriver


ID_APP_ABOUT = 0xE140
ID_HELP_TOPICS = 32855
ID_WINDOW_NEW = 0xE130
ID_WINDOW_CASCADE = 0xE132
ID_WINDOW_TILE_HORZ = 0xE133
ID_WINDOW_TILE_VERT = 0xE134


def _window_texts(root_hwnd: int) -> list[str]:
    texts: list[str] = []

    def _collect(hwnd, _):
        text = win32gui.GetWindowText(hwnd)
        if text:
            texts.append(text)
        return True

    win32gui.EnumChildWindows(root_hwnd, _collect, None)
    return texts


def _visible_child_classes(root_hwnd: int) -> list[str]:
    classes: list[str] = []

    def _collect(hwnd, _):
        if win32gui.IsWindowVisible(hwnd):
            classes.append(win32gui.GetClassName(hwnd))
        return True

    win32gui.EnumChildWindows(root_hwnd, _collect, None)
    return classes


def _mdi_children(root_hwnd: int) -> list[int]:
    mdi_clients: list[int] = []

    def _find_mdi(hwnd, _):
        if win32gui.GetClassName(hwnd) == "MDIClient":
            mdi_clients.append(hwnd)
        return True

    win32gui.EnumChildWindows(root_hwnd, _find_mdi, None)
    if not mdi_clients:
        return []

    children: list[int] = []

    def _collect(hwnd, _):
        if win32gui.GetParent(hwnd) == mdi_clients[0] and win32gui.IsWindowVisible(hwnd):
            children.append(hwnd)
        return True

    win32gui.EnumChildWindows(mdi_clients[0], _collect, None)
    return children


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


class TestHelpCoreBehavior:
    @pytest.mark.ported
    def test_hv001_distribution_and_x64_gui_start(self, ported_exe_path):
        exe = Path(ported_exe_path)
        help_dir = exe.parent / "help"
        expected_help = [
            "index.html",
            "01_intro.html",
            "02_basics.html",
            "03_menu.html",
            "04_features.html",
            "05_struct.html",
            "06_env.html",
            "07_ext.html",
            "08_differences.html",
            "09_credits.html",
        ]

        assert exe.is_file()
        assert all((help_dir / name).is_file() for name in expected_help)

        image = exe.read_bytes()
        pe_offset = struct.unpack_from("<I", image, 0x3C)[0]
        assert image[pe_offset : pe_offset + 4] == b"PE\0\0"
        machine = struct.unpack_from("<H", image, pe_offset + 4)[0]
        assert machine == 0x8664, f"expected AMD64 PE, got {machine:#x}"

        with StirlingDriver(exe) as drv:
            drv.start()
            assert win32gui.IsWindowVisible(drv.hwnd)
            assert "StirHex" in win32gui.GetWindowText(drv.hwnd)

    @pytest.mark.ported
    def test_hv001_missing_help_shows_error(
        self, ported_exe_path, tmp_path
    ):
        isolated_dir = tmp_path / "without_help"
        isolated_dir.mkdir()
        isolated_exe = isolated_dir / "StirHex.exe"
        shutil.copy2(ported_exe_path, isolated_exe)

        with StirlingDriver(isolated_exe) as drv:
            drv.start()
            drv.post_command(ID_HELP_TOPICS)
            dialog, _title, items = drv.find_message_box(timeout=5.0)
            text = "\n".join(items)
            assert "help" in text.lower()
            assert "index.html" in text.lower()
            win32gui.PostMessage(dialog, win32con.WM_COMMAND, win32con.IDOK, 0)

    @pytest.mark.ported
    def test_hv002_about_dialog_text(self, ported_exe_path):
        with StirlingDriver(ported_exe_path) as drv:
            drv.start()
            drv.post_command(ID_APP_ABOUT)
            dialog, _title, _items = drv.find_message_box(timeout=5.0)
            text = "\n".join(_window_texts(dialog))

            for expected in (
                "BinaryEditor  StirHex",
                "Version 1.0.0",
                "Copyright (C) 2026 StirHex Project",
                "BinaryEditor  Stirling  Version 1.31",
                "Copyright (C) 1998-1999",
                "https://github.com/rock5easily/stirhex",
                "非公式",
                "原作者とは無関係",
            ):
                assert expected in text

            win32gui.PostMessage(dialog, win32con.WM_COMMAND, win32con.IDOK, 0)

    @pytest.mark.ported
    def test_hv003_initial_chrome_and_dirty_title(
        self, ported_exe_path, tmp_path
    ):
        test_file = tmp_path / "title_state.dat"
        test_file.write_bytes(bytes(range(64)))

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)

            menu = win32gui.GetMenu(drv.hwnd)
            assert menu
            assert win32gui.GetMenuItemCount(menu) == 6

            visible_classes = _visible_child_classes(drv.hwnd)
            assert "ToolbarWindow32" in visible_classes
            status = drv.get_statusbar_info()
            assert status is not None and status[1]
            assert any(test_file.name in title for title in drv.get_mdi_child_titles())
            assert all(
                not title.endswith("*") for title in drv.get_mdi_child_titles()
            )

            drv.type_hex_chars("A")
            time.sleep(0.3)
            assert any(title.endswith("*") for title in drv.get_mdi_child_titles())

            drv.post_command(CMD_FILE_SAVE)
            time.sleep(0.5)
            assert all(
                not title.endswith("*") for title in drv.get_mdi_child_titles()
            )

    @pytest.mark.ported
    def test_hv012_mdi_new_view_and_layout(self, ported_exe_path, tmp_path):
        first = tmp_path / "mdi_first.dat"
        second = tmp_path / "mdi_second.dat"
        first.write_bytes(bytes(range(64)))
        second.write_bytes(bytes(reversed(range(64))))

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(first)
            drv.open_file_via_dialog(second)
            assert len(_mdi_children(drv.hwnd)) == 2

            drv.post_command(ID_WINDOW_NEW)
            time.sleep(0.3)
            assert len(_mdi_children(drv.hwnd)) == 3

            for command in (
                ID_WINDOW_CASCADE,
                ID_WINDOW_TILE_HORZ,
                ID_WINDOW_TILE_VERT,
            ):
                drv.post_command(command)
                time.sleep(0.3)
                rects = [win32gui.GetWindowRect(hwnd) for hwnd in _mdi_children(drv.hwnd)]
                assert len(rects) == 3
                assert len(set(rects)) > 1

    @pytest.mark.ported
    def test_hv014_three_generation_backup(self, ported_exe_path, tmp_path):
        test_file = tmp_path / "backup.dat"
        test_file.write_bytes(b"AAAA")

        with stirling_settings(
            BackupCreate=1,
            BackupGenerations=3,
            BackupFolderSpecify=0,
        ):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                for value in ("10", "20", "30"):
                    drv.type_hex_chars(value)
                    drv.post_command(CMD_FILE_SAVE)
                    time.sleep(0.25)

        assert (tmp_path / "backup.dat.bak").read_bytes() == b"\x10\x20AA"
        assert (tmp_path / "backup.dat.bk1").read_bytes() == b"\x10AAA"
        assert (tmp_path / "backup.dat.bk2").read_bytes() == b"AAAA"

    @pytest.mark.ported
    @pytest.mark.parametrize(
        ("mode", "read_allowed", "write_allowed"),
        [(0, True, True), (1, True, False), (2, False, False)],
    )
    def test_hv016_exclusive_mode_table(
        self,
        ported_exe_path,
        tmp_path,
        mode,
        read_allowed,
        write_allowed,
    ):
        test_file = tmp_path / f"exclusive_{mode}.dat"
        test_file.write_bytes(bytes(range(64)))

        with stirling_settings(file_exclusive_mode=mode):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                assert _can_open(test_file, win32con.GENERIC_READ) is read_allowed
                assert _can_open(test_file, win32con.GENERIC_WRITE) is write_allowed
