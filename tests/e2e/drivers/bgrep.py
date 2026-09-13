"""BGREP（フォルダ横断検索）ダイアログの操作ヘルパ。

ダイアログの入力と結果一覧の読み取りは、原版突き合わせ（Issue #49）だけでなく
オプションの検証や 4GiB 境界のケースからも使う。テストモジュールに置いたままだと
`tests/issues/` にパッケージ初期化が無く import できないため、ここへ集約する
（Issue #205）。
"""
import time
from pathlib import Path

import win32con
import win32gui

from .stirling_driver import (
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

def find_dialog(drv: StirlingDriver, ctrl_id: int, timeout: float = 10.0) -> int:
    """指定コントロールを持つ #32770 を待つ。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        for hwnd, cls, _title in drv._get_process_windows():
            if cls == "#32770" and win32gui.GetDlgItem(hwnd, ctrl_id):
                return hwnd
        time.sleep(0.2)
    raise AssertionError(f"ダイアログ（ctrl {ctrl_id}）が出なかった")


def wait_dialogs_closed(drv: StirlingDriver, timeout: float = 60.0) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not [h for h, cls, _t in drv._get_process_windows() if cls == "#32770"]:
            return
        time.sleep(0.2)
    raise AssertionError("BGREP のダイアログが閉じない")


def output_lines(drv: StirlingDriver, timeout: float = 30.0) -> list[str]:
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


def click_to_state(dlg: int, ctrl_id: int, checked: bool) -> None:
    """ラジオ／チェックを目的の状態へ。既に目的の状態ならクリックしない。"""
    hwnd = win32gui.GetDlgItem(dlg, ctrl_id)
    assert hwnd, f"コントロール {ctrl_id} が見つからない"
    want = 1 if checked else 0
    if win32gui.SendMessage(hwnd, win32con.BM_GETCHECK, 0, 0) != want:
        win32gui.SendMessage(hwnd, win32con.BM_CLICK, 0, 0)
        time.sleep(0.2)
    state = win32gui.SendMessage(hwnd, win32con.BM_GETCHECK, 0, 0)
    assert state == want, f"コントロール {ctrl_id} を {want} にできない（実際: {state}）"


def set_and_verify(dlg: int, ctrl_id: int, text: str, attempts: int = 10) -> None:
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


def run(
    drv: StirlingDriver,
    folder: Path,
    data: str,
    hex_mode: bool = True,
    recurse: bool = False,
    file_mask: str = "*.dat",
    scan_timeout: float = 60.0,
) -> list[str]:
    """BGREP を 1 回実行し、(結果行, OK 直前の実効値) を返す。

    data は検索データ（16進なら "DE AD C0 DE" のような空白区切り）。既定の条件は原版
    突き合わせと同じ 16進・再帰なし・*.dat で、再帰や拡張子、文字列検索を変える引数は
    移植版側のケースが使う（Issue #205）。
    """
    safe_set_focus(drv.hwnd)
    time.sleep(0.3)
    drv.post_command(ID_BGREP)

    dlg = find_dialog(drv, IDC_BGREP_FOLDER)
    time.sleep(0.5)   # OnInitDialog の DDX が終わるのを待つ

    # データ種別とサブフォルダ検索。
    #   BM_SETCHECK は見た目を変えるだけで BN_CLICKED を出さないため、アプリ側が
    #   通知で状態を持つ実装だと前回値（文字列検索など）のまま検索してしまう。
    #   利用者と同じ経路になるよう BM_CLICK で切り替える。
    #   ラジオは「選ぶ側」だけをクリックする。外す側をクリックしても解除されない。
    wanted = IDC_BGREP_TYPE_HEX if hex_mode else IDC_BGREP_TYPE_TEXT
    other = IDC_BGREP_TYPE_TEXT if hex_mode else IDC_BGREP_TYPE_HEX
    click_to_state(dlg, wanted, True)
    other_state = win32gui.SendMessage(win32gui.GetDlgItem(dlg, other), win32con.BM_GETCHECK, 0, 0)
    assert other_state == 0, f"データ種別のラジオが排他になっていない（{other}={other_state}）"
    click_to_state(dlg, IDC_BGREP_RECURSE, recurse)
    set_and_verify(dlg, IDC_BGREP_DATA_COMBO, data)
    set_and_verify(dlg, IDC_BGREP_FILE_COMBO, file_mask)
    set_and_verify(dlg, IDC_BGREP_FOLDER, str(folder))

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
    # 走査中は進捗ダイアログが出たままになる。GB 級のフォルダでは既定より長く待つ。
    wait_dialogs_closed(drv, timeout=scan_timeout)
    return output_lines(drv, timeout=scan_timeout), applied


def normalize(lines: list[str], folder: Path) -> list[str]:
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



def activate_output_line(drv: StirlingDriver, index: int) -> None:
    """アウトプットペインの index 行を選んで実行する（利用者のダブルクリック相当）。"""
    boxes: list[int] = []

    def _enum(hwnd, _):
        if win32gui.GetClassName(hwnd) == "ListBox":
            boxes.append(hwnd)
        return True

    win32gui.EnumChildWindows(drv.hwnd, _enum, None)
    assert boxes, "アウトプットペインのリストボックスが無い"
    box = boxes[0]
    win32gui.SendMessage(box, 0x0186, index, 0)   # LB_SETCURSEL
    time.sleep(0.2)
    parent = win32gui.GetParent(box)
    ctrl_id = win32gui.GetDlgCtrlID(box)
    # WM_COMMAND(LBN_DBLCLK=2) を親へ送る。実際のダブルクリックと同じ通知。
    win32gui.SendMessage(parent, win32con.WM_COMMAND, (2 << 16) | ctrl_id, box)
    time.sleep(0.5)
