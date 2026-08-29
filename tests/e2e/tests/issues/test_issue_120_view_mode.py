"""Issue #120: 排他制御されたファイルを閲覧モードで開く。

他のアプリケーションが排他制御しているファイルを開こうとしたとき、原版は
「閲覧モードで開きますか？」と確認して読み取り専用で開く。移植版はこのプロンプトを
実装しておらず、単に「開けません」で終わっていた。

閲覧モードの文書は編集できず、[編集禁止]の切り替えもできない（原 doc+0x64 = 0）。
"""

import contextlib
import os
import time

import pytest
import win32con
import win32file
import win32gui

from drivers.settings_context import settings_value
from pywinauto import timings

from drivers.stirling_driver import (
    CMD_FILE_OPEN,
    ID_TOGGLE_READONLY,
    StirlingDriver,
    _file_dialog_edit,
    _set_control_text,
    safe_set_focus,
)

PORT_ENV = r"Software\StirHex\StirHex\Env"

SAMPLE = bytes(range(256))

# 環境設定「ファイルの排他制御」: 0=しない / 1=書込禁止 / 2=読書禁止
EXCLUSIVE_NONE = 0
EXCLUSIVE_READWRITE = 2

VIEW_MODE_PROMPT = "閲覧モード"


@contextlib.contextmanager
def _hold(path, share):
    """他プロセス相当として、指定の共有モードでファイルを開いたままにする。"""
    handle = win32file.CreateFileW(
        str(path), win32file.GENERIC_READ, share, None,
        win32file.OPEN_EXISTING, win32file.FILE_ATTRIBUTE_NORMAL, None,
    )
    try:
        yield
    finally:
        handle.Close()


def _hold_allowing_readers(path):
    """読み取りだけ許して保持する。StirHex が排他を要求すると共有違反になる。"""
    return _hold(path, win32file.FILE_SHARE_READ)


def _hold_denying_everything(path):
    """共有を一切許さずに保持する。どの共有モードでも開けない。"""
    return _hold(path, 0)


def _find_prompt(drv, timeout=8.0):
    """閲覧モードの確認メッセージを探す（本文に「閲覧モード」を含む #32770）。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        for hwnd, cls, _title in drv._get_process_windows():
            if cls != "#32770" or not win32gui.IsWindowVisible(hwnd):
                continue
            texts = []

            def _enum(child, _):
                texts.append(win32gui.GetWindowText(child))
                return True

            win32gui.EnumChildWindows(hwnd, _enum, None)
            if any(VIEW_MODE_PROMPT in t for t in texts):
                return hwnd, texts
        time.sleep(0.1)
    raise AssertionError("閲覧モードの確認メッセージが出ていない")


def _click(dialog_hwnd, control_id):
    """ダイアログのボタンを押す。指定 ID が無ければ最初のボタンで代用する。"""
    try:
        btn = win32gui.GetDlgItem(dialog_hwnd, control_id)
    except Exception:
        btn = 0
    if not btn:
        buttons = []

        def _enum(child, _):
            if win32gui.GetClassName(child) == "Button":
                buttons.append(child)
            return True

        win32gui.EnumChildWindows(dialog_hwnd, _enum, None)
        btn = buttons[0] if buttons else 0
    if btn:
        win32gui.PostMessage(btn, win32con.BM_CLICK, 0, 0)
    else:
        win32gui.PostMessage(dialog_hwnd, win32con.WM_CLOSE, 0, 0)
    time.sleep(0.4)


def _find_dialog_containing(drv, needle, timeout=6.0):
    """本文に needle を含む #32770 を探す。見つからなければ (None, 見えている本文)。"""
    deadline = time.time() + timeout
    seen = []
    while time.time() < deadline:
        seen = []
        for hwnd, cls, _title in drv._get_process_windows():
            if cls != "#32770" or not win32gui.IsWindowVisible(hwnd):
                continue
            texts = []

            def _enum(child, _):
                texts.append(win32gui.GetWindowText(child))
                return True

            win32gui.EnumChildWindows(hwnd, _enum, None)
            seen.extend(texts)
            if any(needle in t for t in texts):
                return hwnd, texts
        time.sleep(0.1)
    return None, seen


def _dismiss_all(drv, timeout=5.0):
    """開いているメッセージを順に閉じる。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        boxes = [h for h, cls, _t in drv._get_process_windows()
                 if cls == "#32770" and win32gui.IsWindowVisible(h)]
        if not boxes:
            return
        for h in boxes:
            _click(h, win32con.IDOK)
        time.sleep(0.2)


def _refresh_main_window(drv):
    """メインフレームのハンドルを取り直す。

    コマンドライン指定で開くと確認メッセージが InitInstance 中に出るため、start() が
    掴むのはメインフレームではなくそのメッセージボックスになる。確認へ答えた後に
    実際のメインフレームを掴み直す。
    """
    def _find():
        for hwnd, cls, _title in drv._get_process_windows():
            if cls != "#32770" and win32gui.IsWindowVisible(hwnd):
                return hwnd
        raise RuntimeError("main frame not found yet")

    hwnd = timings.wait_until_passes(8, 0.2, _find)
    drv.hwnd = hwnd
    drv.main_window = drv.app.window(handle=hwnd)
    safe_set_focus(hwnd)
    time.sleep(0.3)
    return hwnd


def _no_dialog(drv):
    return not [h for h, cls, _t in drv._get_process_windows()
                if cls == "#32770" and win32gui.IsWindowVisible(h)]


@pytest.mark.ported
class TestIssue120ViewMode:

    def test_prompt_offers_view_mode_and_opens_read_only(self, ported_exe_path, tmp_path):
        """自分の排他要求が通らないファイルは、確認のうえ閲覧モードで開ける。

        他プロセスが読み取りを許して保持している状態で、環境設定「読書禁止」で開こうと
        すると共有違反になる。修正前はここで「開けません」で終わっていた。
        """
        target = tmp_path / "locked_open.dat"
        target.write_bytes(SAMPLE)

        with settings_value(PORT_ENV, "ExclusiveControl", EXCLUSIVE_READWRITE):
            with _hold_allowing_readers(target):
                with StirlingDriver(ported_exe_path) as drv:
                    drv.start(target)
                    dialog, texts = _find_prompt(drv)
                    body = "\n".join(texts)
                    assert "排他制御" in body, body

                    _click(dialog, win32con.IDOK)   # [OK] = 閲覧モードで開く
                    time.sleep(0.8)
                    assert _no_dialog(drv), "閲覧モードで開けずエラーになっている"
                    _refresh_main_window(drv)
                    titles = drv.get_mdi_child_titles()
                    assert any(target.name in t for t in titles), (
                        "閲覧モードで開けていない: %s" % (titles,)
                    )

                    # 閲覧モードは編集できず、[編集禁止]の切り替えもできない。
                    assert "編禁" in drv.get_all_statusbar_text(), (
                        "閲覧モードなのに編集可の表示になっている: %s"
                        % (drv.get_all_statusbar_text(),)
                    )
                    drv.post_command(ID_TOGGLE_READONLY)
                    time.sleep(0.4)
                    assert "編禁" in drv.get_all_statusbar_text(), (
                        "閲覧モードの文書で編集禁止を解除できてしまう"
                    )
                    drv.type_hex_chars("41")
                    time.sleep(0.3)

        # 閲覧モードの文書は編集できないため、ファイルの内容は変わらない。
        assert target.read_bytes() == SAMPLE

    def test_cancel_does_not_open_the_document(self, ported_exe_path, tmp_path):
        """[キャンセル]では文書を開かない。

        確認そのものは追加のメッセージを出さないが、コマンドライン指定のファイルを
        開けなかった場合は、原版と同じく起動時に「%sが見つかりません」を表示して
        終了する経路へ入る（開けなかったことの通知はそこが担う）。
        """
        target = tmp_path / "locked_cancel.dat"
        target.write_bytes(SAMPLE)

        with settings_value(PORT_ENV, "ExclusiveControl", EXCLUSIVE_READWRITE):
            with _hold_allowing_readers(target):
                with StirlingDriver(ported_exe_path) as drv:
                    drv.start(target)
                    dialog, _texts = _find_prompt(drv)
                    _click(dialog, win32con.IDCANCEL)
                    time.sleep(0.8)

                    notice, texts = _find_dialog_containing(drv, "見つかりません")
                    assert notice, (
                        "コマンドライン指定を開けなかった通知が出ていない: %s" % (texts,)
                    )
                    assert not any("排他制御" in t for t in texts), (
                        "キャンセルしたのに確認が再表示されている"
                    )
                    _dismiss_all(drv)

    def test_unopenable_file_still_reports_the_error(self, ported_exe_path, tmp_path):
        """共有を一切許さないファイルは、閲覧モードでも開けずエラーになる。

        原版も同じで、確認に[OK]と答えても開き直しに失敗してエラーメッセージになる。
        """
        target = tmp_path / "denied.dat"
        target.write_bytes(SAMPLE)

        with settings_value(PORT_ENV, "ExclusiveControl", EXCLUSIVE_NONE):
            with _hold_denying_everything(target):
                with StirlingDriver(ported_exe_path) as drv:
                    drv.start(target)
                    dialog, _texts = _find_prompt(drv)
                    _click(dialog, win32con.IDOK)
                    time.sleep(0.8)

                    error, texts = _find_dialog_containing(drv, "読み込みに失敗")
                    assert error, "開けなかったのにエラーが出ていない: %s" % (texts,)
                    _dismiss_all(drv)

    def test_own_exclusive_lock_is_held_while_open(self, ported_exe_path, tmp_path):
        """排他制御「読書禁止」で開いている間は、他プロセスから開けない。"""
        target = tmp_path / "own_lock.dat"
        target.write_bytes(SAMPLE)

        with settings_value(PORT_ENV, "ExclusiveControl", EXCLUSIVE_READWRITE):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(target)
                time.sleep(0.5)
                assert drv.get_mdi_child_titles(), "開けていない（前提が崩れている）"

                with pytest.raises(Exception):
                    win32file.CreateFileW(
                        str(target), win32file.GENERIC_READ, 0, None,
                        win32file.OPEN_EXISTING, win32file.FILE_ATTRIBUTE_NORMAL, None,
                    )

        # 閉じた後はロックが解放される。
        handle = win32file.CreateFileW(
            str(target), win32file.GENERIC_READ, 0, None,
            win32file.OPEN_EXISTING, win32file.FILE_ATTRIBUTE_NORMAL, None,
        )
        handle.Close()

    def test_read_only_file_opens_in_view_mode(self, ported_exe_path, tmp_path):
        """読み取り専用属性のファイルは、確認なしで閲覧モードとして開く（原挙動）。"""
        target = tmp_path / "read_only.dat"
        target.write_bytes(SAMPLE)
        os.chmod(target, 0o444)
        try:
            with settings_value(PORT_ENV, "ExclusiveControl", EXCLUSIVE_NONE):
                with StirlingDriver(ported_exe_path) as drv:
                    drv.start(target)
                    time.sleep(0.6)
                    assert _no_dialog(drv), "読み取り専用属性で確認メッセージが出ている"
                    assert drv.get_mdi_child_titles(), "読み取り専用ファイルを開けていない"
                    assert "編禁" in drv.get_all_statusbar_text(), (
                        "読み取り専用ファイルが編集可で開かれている: %s"
                        % (drv.get_all_statusbar_text(),)
                    )
                    safe_set_focus(drv.hwnd)
                    drv.post_command(ID_TOGGLE_READONLY)
                    time.sleep(0.4)
                    assert "編禁" in drv.get_all_statusbar_text(), (
                        "読み取り専用ファイルで編集禁止を解除できてしまう"
                    )
        finally:
            os.chmod(target, 0o666)
