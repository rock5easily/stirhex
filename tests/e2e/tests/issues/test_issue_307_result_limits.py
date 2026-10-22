"""Issue #307: BGREP とデータ比較の結果件数の上限、比較の進捗表示と中止。

- BGREP はヒットが 10,000 件を超えると検索を打ち切り、出力ペインに打ち切りを 1 行出す
- データ比較は相違範囲が 10,000 件を超えると比較を打ち切り、先頭から 10,000 件を一覧に出す
- データ比較は約 0.3 秒で終わらなければ進捗ダイアログ（[中止] 付き）を出す。中止したら
  結果を出さない。比較中に文書のデータが変わったら比較を中止して知らせる

いずれも移植版の仕様（原版には件数の上限も進捗表示もない）。
"""
import time
from pathlib import Path

import pytest
import win32con
import win32file
import win32gui

from drivers import bgrep
from drivers.stirling_driver import (
    IDC_COMPARE_LIST,
    IDC_DIFFLIST_LIST,
    LVM_GETITEMCOUNT,
    StirlingDriver,
    _control_text,
    _send_message_w,
)

LIMIT = 10000

PROGRESS_TITLE = "比較中"
DIFF_LIST_TITLE = "相違箇所一覧"
ID_DELETE_BYTE = 32808          # 0x8028 [１バイト削除]
FSCTL_SET_SPARSE = 0x000900C4


def _wait_for(predicate, timeout=10.0, interval=0.05):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(interval)
    return False


def _find_dialog(drv: StirlingDriver, title: str) -> int:
    for hwnd, cls, caption in drv._get_process_windows():
        if cls == "#32770" and caption == title and win32gui.IsWindowVisible(hwnd):
            return hwnd
    return 0


def _make_sparse(path: Path, size: int, patches: tuple = ()) -> None:
    """size バイトのスパースファイルを作る（ディスクを消費しない）。patches は (位置, バイト列)。"""
    # Python の truncate は伸ばした分へゼロを書き込むため、SetEndOfFile で伸ばす。
    handle = win32file.CreateFile(str(path), win32file.GENERIC_WRITE, 0, None,
                                  win32file.CREATE_ALWAYS, 0, None)
    try:
        win32file.DeviceIoControl(handle, FSCTL_SET_SPARSE, None, 0)
        win32file.SetFilePointer(handle, size, win32file.FILE_BEGIN)
        win32file.SetEndOfFile(handle)
        for offset, data in patches:
            win32file.SetFilePointer(handle, offset, win32file.FILE_BEGIN)
            win32file.WriteFile(handle, data)
    finally:
        handle.Close()


def _start_compare(drv: StirlingDriver) -> None:
    """アクティブな文書から [比較] を開き、もう一方の文書を選んで実行する。

    [OK] は非同期に送り、UI のアイドルを待たない。比較中は進捗ダイアログのタイマーで
    UI スレッドが動き続けるため、アイドル待ちでは比較の完了まで戻らないことがある。
    結果（進捗ダイアログ・メッセージ・相違箇所一覧）は呼出側が直接待つ。
    """
    compare = drv.open_compare_dialog()
    listbox = win32gui.GetDlgItem(compare, IDC_COMPARE_LIST)
    win32gui.SendMessage(listbox, win32con.LB_SETCURSEL, 0, 0)
    win32gui.PostMessage(compare, win32con.WM_COMMAND, win32con.IDOK, 0)


def _answer_message(drv: StirlingDriver, timeout: float = 30.0) -> str:
    """表題 StirHex のメッセージボックスを待って [OK] で閉じ、本文を返す。

    比較の選択ダイアログ（閉じる途中）や進捗ダイアログも #32770 のため、
    drv.answer_message_box のように最初の #32770 を掴まず、表題で特定する。
    MB_OK の [OK] はコントロール ID が IDCANCEL。
    """
    assert _wait_for(lambda: _find_dialog(drv, "StirHex"), timeout=timeout), (
        "メッセージボックスが出ない")
    box = _find_dialog(drv, "StirHex")
    texts: list[str] = []
    win32gui.EnumChildWindows(box, lambda h, _: texts.append(_control_text(h)) or True, None)
    win32gui.PostMessage(box, win32con.WM_COMMAND, win32con.IDCANCEL, 0)
    assert _wait_for(lambda: not win32gui.IsWindow(box)), "メッセージボックスが閉じない"
    return "\n".join(texts)


def _diff_list_count(drv: StirlingDriver) -> int:
    diff = drv.find_diff_list_dialog(timeout=30.0)
    return _send_message_w(win32gui.GetDlgItem(diff, IDC_DIFFLIST_LIST), LVM_GETITEMCOUNT)


class TestIssue307BgrepLimit:
    """BGREP のヒット件数の上限。"""

    @pytest.mark.ported
    def test_hits_up_to_limit_are_all_listed(self, ported_exe_path, tmp_path):
        """ちょうど上限件数なら打ち切らず、件数の集計行を出す。"""
        root = tmp_path / "exact"
        root.mkdir()
        (root / "zeros.dat").write_bytes(bytes(LIMIT))   # 00 が 10,000 件

        with StirlingDriver(ported_exe_path) as drv:
            drv.start()
            lines, applied = bgrep.run(drv, root, "00")

        assert len(lines) == LIMIT + 1, f"結果行の数が違う: {len(lines)} / {applied}"
        assert lines[-1] == f"{LIMIT}件見つかりました", lines[-1]
        assert lines[LIMIT - 1].endswith(f" : {LIMIT - 1:08X}"), lines[LIMIT - 1]

    @pytest.mark.ported
    def test_hits_over_limit_are_truncated(self, ported_exe_path, tmp_path):
        """上限を超えるヒットがあると検索を打ち切り、先頭から上限件数までを出す。"""
        root = tmp_path / "over"
        root.mkdir()
        (root / "a_zeros.dat").write_bytes(bytes(LIMIT + 1))
        (root / "b_zeros.dat").write_bytes(bytes(16))   # 打ち切り後は走査しない

        with StirlingDriver(ported_exe_path) as drv:
            drv.start()
            lines, applied = bgrep.run(drv, root, "00")

        assert len(lines) == LIMIT + 1, f"結果行の数が違う: {len(lines)} / {applied}"
        assert lines[-1] == f"{LIMIT}件を超えたため、検索を打ち切りました", lines[-1]
        hits = lines[:-1]
        assert all("a_zeros.dat : " in line for line in hits), "打ち切り後のファイルまで出ている"
        assert hits[-1].endswith(f" : {LIMIT - 1:08X}"), hits[-1]


class TestIssue307CompareLimit:
    """データ比較の相違範囲の件数の上限。"""

    @pytest.mark.ported
    def test_diffs_up_to_limit_are_all_listed(self, ported_exe_path, tmp_path):
        """ちょうど上限件数なら打ち切りのメッセージを出さずに一覧を出す。"""
        first = tmp_path / "exact_a.dat"
        second = tmp_path / "exact_b.dat"
        first.write_bytes(bytes(LIMIT * 2))
        second.write_bytes(b"\x00\xFF" * LIMIT)   # 奇数位置の 1 バイトずつが相違

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(first)
            drv.open_file_via_dialog(second)
            _start_compare(drv)
            assert _diff_list_count(drv) == LIMIT
            assert not _find_dialog(drv, "StirHex"), "上限ちょうどで打ち切りのメッセージが出た"

    @pytest.mark.ported
    def test_diffs_over_limit_are_truncated(self, ported_exe_path, tmp_path):
        """上限を超える相違範囲があると、知らせたうえで先頭から上限件数までを一覧に出す。"""
        first = tmp_path / "over_a.dat"
        second = tmp_path / "over_b.dat"
        first.write_bytes(bytes(LIMIT * 2 + 2))
        second.write_bytes(b"\x00\xFF" * (LIMIT + 1))

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(first)
            drv.open_file_via_dialog(second)
            _start_compare(drv)
            text = _answer_message(drv)
            assert f"相違箇所が{LIMIT}件を超えたため、比較を打ち切りました" in text, text
            assert f"先頭から{LIMIT}件を表示します" in text, text
            assert _diff_list_count(drv) == LIMIT


class TestIssue307CompareProgress:
    """比較に時間がかかるときの進捗ダイアログ。

    スパースファイルは遅延読み込み（既定 512MB 以上）で開く。8GB の比較は読み取りと 1 バイト
    ずつの照合で十数秒かかるため、速い環境でも 0.3 秒では終わらずに進捗ダイアログが出る。
    """

    BIG = 8 * 1024 ** 3

    @pytest.mark.ported
    def test_progress_completes_and_lists_the_diff(self, ported_exe_path, tmp_path):
        size = self.BIG
        first = tmp_path / "done_a.bin"
        second = tmp_path / "done_b.bin"
        _make_sparse(first, size)
        _make_sparse(second, size, patches=((size - 16, b"\x01\x02"),))

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(first)
            drv.open_file_via_dialog(second)
            _start_compare(drv)
            assert _wait_for(lambda: _find_dialog(drv, PROGRESS_TITLE), timeout=10.0), (
                "進捗ダイアログが出ない")
            diff = drv.find_diff_list_dialog(timeout=120.0)
            assert not _find_dialog(drv, PROGRESS_TITLE), "完了後も進捗ダイアログが残っている"
            assert drv.get_diff_list_rows(diff) == [
                (f"{size - 16:08X}", f"{size - 15:08X}", "00000002")
            ]

    @pytest.mark.ported
    def test_cancel_shows_no_result(self, ported_exe_path, tmp_path):
        first = tmp_path / "cancel_a.bin"
        second = tmp_path / "cancel_b.bin"
        _make_sparse(first, self.BIG, patches=((0, b"\x01"),))
        _make_sparse(second, self.BIG)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(first)
            drv.open_file_via_dialog(second)
            _start_compare(drv)
            assert _wait_for(lambda: _find_dialog(drv, PROGRESS_TITLE), timeout=10.0), (
                "進捗ダイアログが出ない")
            progress = _find_dialog(drv, PROGRESS_TITLE)
            win32gui.PostMessage(progress, win32con.WM_COMMAND, win32con.IDCANCEL, 0)
            assert _wait_for(lambda: not win32gui.IsWindow(progress), timeout=10.0), (
                "中止しても進捗ダイアログが閉じない")
            drv.wait_idle()
            assert not _find_dialog(drv, DIFF_LIST_TITLE), "中止したのに相違箇所一覧が出た"
            assert not _find_dialog(drv, "StirHex"), "中止したのにメッセージが出た"

    @pytest.mark.ported
    def test_data_change_during_compare_aborts(self, ported_exe_path, tmp_path):
        """比較中に文書のデータが変わったら、比較を中止して知らせる。

        利用者はモードレスのダイアログ（置換など）から比較中の文書を編集できる。ここでは
        ビューへ [１バイト削除] を直接送って同じ状況を作る。
        """
        first = tmp_path / "change_a.bin"
        second = tmp_path / "change_b.bin"
        _make_sparse(first, self.BIG)
        _make_sparse(second, self.BIG)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(first)
            drv.open_file_via_dialog(second)
            view = drv.get_view_hwnd()   # アクティブ＝比較を始める文書のビュー
            _start_compare(drv)
            assert _wait_for(lambda: _find_dialog(drv, PROGRESS_TITLE), timeout=10.0), (
                "進捗ダイアログが出ない")
            win32gui.PostMessage(view, win32con.WM_COMMAND, ID_DELETE_BYTE, 0)
            text = _answer_message(drv)
            assert "比較中にデータが変更されたため、比較を中止しました" in text, text
            assert not _find_dialog(drv, PROGRESS_TITLE)
            assert not _find_dialog(drv, DIFF_LIST_TITLE)
