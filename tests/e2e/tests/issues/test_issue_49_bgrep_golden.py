"""BGREP（フォルダ横断検索）の原版突き合わせ（Issue #49 / 親 #16）。

BGREP はダイアログ入力（検索データ・ファイル種別・フォルダ）と結果のアウトプット
ペインという、Unicode 化（#41）とシェル系モダナイズ（#45）の両方が通る経路。
原版と移植版へ同じ条件を与え、アウトプットペインの結果行が一致することを確認する。

原・移植ともアウトプットペインは素の CListBox で、1 行は "フルパス : %08X"。
原は ANSI ウィンドウなので、行の取得は _listbox_texts のネイティブ形式読みに任せる。
"""
import time
from pathlib import Path

import pytest
import win32con
import win32gui

from drivers.stirling_driver import (
    ID_BGREP,
    StirlingDriver,
    _control_text,
    _listbox_texts,
    _set_control_text,
    safe_set_focus,
)

# BGREP ダイアログ（IDD_BGREP 172）のコントロール ID。原版と共通。
IDC_BGREP_DATA_COMBO = 1026
IDC_BGREP_TYPE_HEX = 1016
IDC_BGREP_TYPE_TEXT = 1017
IDC_BGREP_FILE_COMBO = 1027
IDC_BGREP_FOLDER = 1007
IDC_BGREP_RECURSE = 1011

PATTERN = b"\xDE\xAD\xC0\xDE"
# 原版の 16 進入力はバイトを空白で区切る（既定値 "AA BB CC" と同じ書式）。
PATTERN_HEX = "DE AD C0 DE"


def _make_corpus(root: Path) -> None:
    """検索対象フォルダを作る。ヒット 3 件（2 ファイル）＋非ヒット 1 件。"""
    filler = bytes(range(256))
    # hit_a.dat: 0x00000010 と 0x00000100 の 2 箇所
    a = bytearray(filler * 2)
    a[0x10:0x14] = PATTERN
    a[0x100:0x104] = PATTERN
    (root / "hit_a.dat").write_bytes(bytes(a))
    # hit_b.dat: 0x00000004 の 1 箇所
    b = bytearray(filler)
    b[0x04:0x08] = PATTERN
    (root / "hit_b.dat").write_bytes(bytes(b))
    # miss.dat: ヒットなし
    (root / "miss.dat").write_bytes(filler)
    # 拡張子が対象外のファイル（*.dat 指定で拾われないこと）
    other = bytearray(filler)
    other[0x20:0x24] = PATTERN
    (root / "other.bin").write_bytes(bytes(other))


def _find_dialog(drv: StirlingDriver, ctrl_id: int, timeout: float = 10.0) -> int:
    """指定コントロールを持つ #32770 を待つ。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        for hwnd, cls, _title in drv._get_process_windows():
            if cls == "#32770" and win32gui.GetDlgItem(hwnd, ctrl_id):
                return hwnd
        time.sleep(0.2)
    raise AssertionError(f"ダイアログ（ctrl {ctrl_id}）が出なかった")


def _wait_dialogs_closed(drv: StirlingDriver, timeout: float = 60.0) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not [h for h, cls, _t in drv._get_process_windows() if cls == "#32770"]:
            return
        time.sleep(0.2)
    raise AssertionError("BGREP のダイアログが閉じない")


def _output_lines(drv: StirlingDriver, timeout: float = 30.0) -> list[str]:
    """アウトプットペインのリストボックス行を取得する。"""
    deadline = time.time() + timeout
    last: list[str] = []
    while time.time() < deadline:
        boxes: list[int] = []

        def _enum(hwnd, _):
            if win32gui.GetClassName(hwnd) == "ListBox":
                boxes.append(hwnd)
            return True

        win32gui.EnumChildWindows(drv.hwnd, _enum, None)
        for box in boxes:
            texts = _listbox_texts(box)
            if texts:
                last = texts
                return last
        time.sleep(0.3)
    return last


def _click_to_state(dlg: int, ctrl_id: int, checked: bool) -> None:
    """ラジオ／チェックを目的の状態へ。既に目的の状態ならクリックしない。"""
    hwnd = win32gui.GetDlgItem(dlg, ctrl_id)
    assert hwnd, f"コントロール {ctrl_id} が見つからない"
    want = 1 if checked else 0
    if win32gui.SendMessage(hwnd, win32con.BM_GETCHECK, 0, 0) != want:
        win32gui.SendMessage(hwnd, win32con.BM_CLICK, 0, 0)
        time.sleep(0.2)
    state = win32gui.SendMessage(hwnd, win32con.BM_GETCHECK, 0, 0)
    assert state == want, f"コントロール {ctrl_id} を {want} にできない（実際: {state}）"


def _set_and_verify(dlg: int, ctrl_id: int, text: str, attempts: int = 10) -> None:
    """コントロールへ値を入れ、実際に入ったことを読み返して確かめる。

    ダイアログは前回値（検索データの既定 "AA BB CC" や前回のフォルダ）を
    OnInitDialog の DDX で流し込む。コントロールが生成された直後に書くと
    その初期化に上書きされ、既定値のまま検索してしまう。読み返して一致する
    まで書き直すことで、値が確実に反映されてから OK を押す。
    """
    hwnd = win32gui.GetDlgItem(dlg, ctrl_id)
    assert hwnd, f"コントロール {ctrl_id} が見つからない"
    for _ in range(attempts):
        _set_control_text(hwnd, text)
        time.sleep(0.2)
        if _control_text(hwnd) == text:
            return
    raise AssertionError(
        f"コントロール {ctrl_id} に {text!r} を設定できない（実際: {_control_text(hwnd)!r}）"
    )


def _run_bgrep(drv: StirlingDriver, folder: Path) -> list[str]:
    safe_set_focus(drv.hwnd)
    time.sleep(0.3)
    drv.post_command(ID_BGREP)

    dlg = _find_dialog(drv, IDC_BGREP_FOLDER)
    time.sleep(0.5)   # OnInitDialog の DDX が終わるのを待つ

    # データ種別＝16進、サブフォルダ検索＝オフ。
    #   BM_SETCHECK は見た目を変えるだけで BN_CLICKED を出さないため、アプリ側が
    #   通知で状態を持つ実装だと前回値（文字列検索など）のまま検索してしまう。
    #   利用者と同じ経路になるよう BM_CLICK で切り替える。
    _click_to_state(dlg, IDC_BGREP_TYPE_HEX, True)
    _click_to_state(dlg, IDC_BGREP_RECURSE, False)
    _set_and_verify(dlg, IDC_BGREP_DATA_COMBO, PATTERN_HEX)
    _set_and_verify(dlg, IDC_BGREP_FILE_COMBO, "*.dat")
    _set_and_verify(dlg, IDC_BGREP_FOLDER, str(folder))

    # OK を押す直前の実効値。食い違ったときに原因を特定できるよう記録する。
    applied = {
        "data": _control_text(win32gui.GetDlgItem(dlg, IDC_BGREP_DATA_COMBO)),
        "filetype": _control_text(win32gui.GetDlgItem(dlg, IDC_BGREP_FILE_COMBO)),
        "folder": _control_text(win32gui.GetDlgItem(dlg, IDC_BGREP_FOLDER)),
        "hex_checked": win32gui.SendMessage(
            win32gui.GetDlgItem(dlg, IDC_BGREP_TYPE_HEX), win32con.BM_GETCHECK, 0, 0),
        "text_checked": win32gui.SendMessage(
            win32gui.GetDlgItem(dlg, IDC_BGREP_TYPE_TEXT), win32con.BM_GETCHECK, 0, 0),
        "recurse_checked": win32gui.SendMessage(
            win32gui.GetDlgItem(dlg, IDC_BGREP_RECURSE), win32con.BM_GETCHECK, 0, 0),
        "corpus": sorted(p.name for p in folder.iterdir()),
    }

    win32gui.PostMessage(dlg, win32con.WM_COMMAND, 1, 0)   # IDOK
    _wait_dialogs_closed(drv)
    return _output_lines(drv), applied


def _normalize(lines: list[str], folder: Path) -> list[str]:
    """比較用にフォルダ部分を除き、ファイル名とオフセットだけにして並べ替える。"""
    out = []
    for line in lines:
        text = line.strip()
        if not text:
            continue
        root = str(folder).lower()
        lowered = text.lower()
        if root in lowered:
            text = text[lowered.index(root) + len(root):].lstrip(r"\\/")
        out.append(text.lower())
    return sorted(out)


class TestIssue49BgrepGolden:
    """BGREP の検出結果が原版と一致することを確認する。"""

    @pytest.mark.golden
    @pytest.mark.ported
    @pytest.mark.original
    def test_bgrep_hits_match_original(self, original_exe_path, ported_exe_path, tmp_path):
        orig_dir = tmp_path / "orig"
        port_dir = tmp_path / "port"
        orig_dir.mkdir()
        port_dir.mkdir()
        _make_corpus(orig_dir)
        _make_corpus(port_dir)

        with StirlingDriver(original_exe_path) as drv:
            drv.start()
            time.sleep(0.5)
            orig_lines, orig_applied = _run_bgrep(drv, orig_dir)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start()
            time.sleep(0.5)
            port_lines, port_applied = _run_bgrep(drv, port_dir)

        orig_norm = _normalize(orig_lines, orig_dir)
        port_norm = _normalize(port_lines, port_dir)

        assert orig_norm, f"原版が結果を返していない: {orig_lines}"
        assert port_norm == orig_norm, (
            "BGREP の結果が原版と一致しない\n"
            f"原版 : {orig_norm}\n  実効値: {orig_applied}\n"
            f"移植版: {port_norm}\n  実効値: {port_applied}"
        )
