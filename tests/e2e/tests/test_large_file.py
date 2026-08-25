"""2GB 超ファイルの動作検証（Issue #23）。

原版は 32bit のため 2GB 超のファイルを扱えない。ここでの検証対象は移植版のみで、
golden 比較は行わない（比較相手が存在しない）。

ファイル生成とロードに数分・数GBのメモリを要するため、既定ではスキップする。
実行するには環境変数 STIRLING_E2E_LARGE=1 を設定する（コア単体テストの
STIRLING_CORE_TEST_LARGE と同じ規約）。

    cd porting/tests/e2e
    $env:STIRLING_E2E_LARGE = "1"; $env:STIRLING_PLATFORM = "x64"
    uv run pytest tests/test_large_file.py
"""
import os
import re
import time

import pytest
import win32api
import win32con
import win32event
import win32gui
from pywinauto import timings

from drivers.stirling_driver import (
    ID_JUMP,
    ID_REVERT_FILE,
    StirlingDriver,
    safe_set_focus,
)

# 2 GiB + 64 KiB。32bit 符号付きの上限（0x7FFFFFFF）を確実に跨ぐ最小規模にして所要時間を抑える。
LARGE_SIZE = (2 << 30) + (64 << 10)
# 2GB 境界の先に置く検索マーカー（ファイル内で一意になるよう通常パターンに現れない並びにする）。
MARKER = b"\xDE\xAD\xBE\xEF\xCA\xFE\xBA\xBE"
MARKER_POS = (2 << 30) + 4096
# 上書き編集する位置（同じく 2GB 超）。
EDIT_POS = (2 << 30) + 8192
EDIT_BYTE = 0x5A

CHUNK = 8 << 20

IDC_JUMP_HINT_CURRENT = 1019  # 「現在アドレス : X」（静的）
IDC_JUMP_HINT_RANGE = 1029    # 「有効アドレス : 0 ～ N」（静的）
IDC_JUMP_EDIT = 1007          # アドレス入力
IDC_FIND_TYPE_HEX = 1016      # 検索データ種別: 16進
IDC_FIND_RANGE_CURSOR = 1018  # 検索範囲: カーソル位置から
IDC_FIND_COMBO = 1026         # 検索データ入力コンボ
IDC_FIND_NEXT = 1042          # 次検索ボタン
# 検索ダイアログを開くコマンド（MFC 標準 ID_EDIT_FIND = 0xE124）。
#   drivers 側の CMD_EDIT_FIND = 57640 は ID_EDIT_REPEAT で誤り。
ID_EDIT_FIND = 57636
IDYES = 6


def _build_large_file(path) -> None:
    """0x00..0xFF を繰り返すパターンで埋め、MARKER_POS に一意な印を置く。

    位置ごとに値を計算すると 2GB 分の Python ループで数十分かかるため、
    周期 256 のブロックを一度だけ作って書き出しを繰り返す。位置の検証は
    MARKER と、元ファイルとの直接比較で行うため周期性は問題にならない。
    """
    block = bytes(range(256)) * (CHUNK // 256)
    with open(path, "wb") as fp:
        written = 0
        while written < LARGE_SIZE:
            n = min(CHUNK, LARGE_SIZE - written)
            fp.write(block[:n])
            written += n
        fp.seek(MARKER_POS)
        fp.write(MARKER)


@pytest.fixture(scope="module")
def large_file(tmp_path_factory):
    if os.environ.get("STIRLING_E2E_LARGE") != "1":
        pytest.skip("set STIRLING_E2E_LARGE=1 to run the 2GB+ scenarios")
    path = tmp_path_factory.mktemp("large") / "large.bin"
    _build_large_file(path)
    assert path.stat().st_size == LARGE_SIZE
    return path


def _read_at(path, offset: int, count: int) -> bytes:
    with open(path, "rb") as fp:
        fp.seek(offset)
        return fp.read(count)


def _process_dialogs(drv: StirlingDriver) -> list[int]:
    """このプロセスが持つ #32770（ダイアログ／メッセージボックス）の一覧。"""
    return [h for h, cls, _t in drv._get_process_windows() if cls == "#32770"]


def _wait_no_dialog(drv: StirlingDriver, timeout: float = 30.0) -> None:
    """このプロセスの #32770 がすべて閉じるまで待つ。

    残ったダイアログを次の操作が掴んでしまうと、別ダイアログのコントロールを
    読んで誤った判定をする（ID 1019 は検索ダイアログでは「データ全体」でもある）。
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not _process_dialogs(drv):
            return
        time.sleep(0.2)
    raise AssertionError("dialog still open")


def _answer_yes(drv: StirlingDriver, timeout: float = 60.0, required: bool = True) -> bool:
    """出現したダイアログに「はい」で答え、そのダイアログが閉じるまで待つ。

    required=True のときはダイアログが出ないこと自体を失敗とする。「操作した結果
    確認を求められたか」を検証の一部にできる（例: 未編集だと再読込は何もしない）。

    待つ対象は「答えたダイアログ自身」に限る。確認が連続する経路（再読込は破棄確認
    のあと 512MB 確認が続く）では次のダイアログが即座に開くため、「ダイアログが 1 つも
    ない瞬間」を待つと取りこぼす。呼び出し側で完全な静止が要るときは _wait_no_dialog を
    別途呼ぶこと。
    """
    deadline = time.time() + timeout
    dlg = 0
    while time.time() < deadline:
        found = _process_dialogs(drv)
        if found:
            dlg = found[0]
            break
        time.sleep(0.2)
    if not dlg:
        if required:
            raise AssertionError("確認ダイアログが出なかった")
        return False
    win32gui.PostMessage(dlg, win32con.WM_COMMAND, IDYES, 0)
    while time.time() < deadline:
        if dlg not in _process_dialogs(drv):
            return True
        time.sleep(0.2)
    raise AssertionError("答えたダイアログが閉じない")


def _attach_main_frame(drv: StirlingDriver, timeout: float = 120.0) -> None:
    """メインフレームを掴み直す（ダイアログを掴んでいる場合があるため）。"""

    def _find_frame():
        for h, cls, _title in drv._get_process_windows():
            if cls != "#32770" and not cls.startswith("UAC"):
                return h
        raise RuntimeError("main frame not shown yet")

    drv.hwnd = timings.wait_until_passes(timeout, 0.5, _find_frame)
    drv.main_window = drv.app.window(handle=drv.hwnd)
    safe_set_focus(drv.hwnd)
    time.sleep(0.5)


def _confirm_large_file_dialog(drv: StirlingDriver, timeout: float = 60.0) -> None:
    """512MB 超の確認ダイアログに「はい」で答え、本物のメインフレームを掴み直す。

    このダイアログはメインフレーム表示より先に出るため、driver の start() は
    ダイアログのハンドルを main window として掴んでしまう（Issue #23 の検証で判明）。
    掴んだハンドルの種類ではなくプロセス内のダイアログを探して答えるので、
    フレームが先に生成された場合でも取りこぼさない。
    """
    _answer_yes(drv, timeout=timeout)
    _attach_main_frame(drv)


def _wait_document_open(drv: StirlingDriver, name: str, timeout: float = 120.0) -> None:
    """MDI 子ウィンドウにファイル名が出る（＝読み込み完了）まで待つ。"""

    def _loaded():
        if any(name in t for t in drv.get_mdi_child_titles()):
            return True
        raise RuntimeError("document not open yet")

    timings.wait_until_passes(timeout, 0.5, _loaded)


def _open_large_file(drv: StirlingDriver, path, load_timeout: float = 120.0):
    drv.start(path)
    _confirm_large_file_dialog(drv)
    _wait_document_open(drv, path.name, load_timeout)


def _find_jump_dialog(drv: StirlingDriver, timeout: float = 10.0) -> int:
    """ジャンプダイアログを、固有のコントロール構成で識別して掴む。

    「最初に見つかった #32770」で済ませると、閉じ遅れた別ダイアログを掴んだまま
    静的テキストを読んで通ってしまう（開発中に検索ダイアログを掴む不具合が実際に出た）。
    """

    def _find():
        for h in _process_dialogs(drv):
            if (win32gui.GetDlgItem(h, IDC_JUMP_EDIT)
                    and win32gui.GetDlgItem(h, IDC_JUMP_HINT_RANGE)
                    and win32gui.GetDlgItem(h, IDC_JUMP_HINT_CURRENT)):
                return h
        raise RuntimeError("jump dialog not found yet")

    return timings.wait_until_passes(timeout, 0.2, _find)


def _current_address(drv: StirlingDriver) -> int:
    """ジャンプダイアログの「現在アドレス」静的テキストからキャレット位置を読む。

    ステータスバーはオーナードローで SB_GETTEXT が空を返すため、こちらを使う。
    """
    _wait_no_dialog(drv)
    drv.post_command(ID_JUMP)
    dlg = _find_jump_dialog(drv)
    try:
        text = win32gui.GetDlgItemText(dlg, IDC_JUMP_HINT_CURRENT).strip()
        # 「<ラベル> : <16進>」形式。区切りの後ろが16進だけであることまで確かめる。
        m = re.fullmatch(r"[^:：]*[:：]\s*([0-9A-Fa-f]+)", text)
        assert m, f"現在アドレスを読み取れない: {text!r}"
        return int(m.group(1), 16)
    finally:
        win32gui.PostMessage(dlg, win32con.WM_COMMAND, win32con.IDCANCEL, 0)
        _wait_no_dialog(drv)


def _find_hex(drv: StirlingDriver, hex_pattern: str, timeout: float = 180.0) -> None:
    """検索ダイアログで 16進パターンをカーソル位置から前方検索する。

    ダイアログはモーダルなので、検索後に閉じてキャレット位置を読める状態に戻す。
    """
    _wait_no_dialog(drv)
    win32gui.PostMessage(drv.hwnd, win32con.WM_COMMAND, ID_EDIT_FIND, 0)

    def _find_dlg():
        found = _process_dialogs(drv)
        if found:
            return found[0]
        raise RuntimeError("find dialog not found yet")

    dlg = timings.wait_until_passes(10, 0.2, _find_dlg)
    win32gui.SendMessage(win32gui.GetDlgItem(dlg, IDC_FIND_TYPE_HEX), win32con.BM_CLICK, 0, 0)
    win32gui.SendMessage(win32gui.GetDlgItem(dlg, IDC_FIND_RANGE_CURSOR), win32con.BM_CLICK, 0, 0)
    time.sleep(0.2)
    # WM_SETTEXT はシステムがプロセス間をマーシャリングするためコンボへ直接渡せる。
    #   設定結果を GetWindowText で読み戻してはいけない（別プロセスのコントロールに対しては
    #   常に空を返す）。検索が意図通り動いたかは呼び出し側の位置アサートで確かめる。
    win32gui.SendMessage(win32gui.GetDlgItem(dlg, IDC_FIND_COMBO), win32con.WM_SETTEXT, 0, hex_pattern)
    time.sleep(0.2)
    win32gui.PostMessage(dlg, win32con.WM_COMMAND, IDC_FIND_NEXT, 0)

    # 2GB 超の走査は数十秒かかる。検索中はダイアログがメッセージを処理しないので、
    #   応答が戻る（= 検索完了）まで SendMessageTimeout で待つ。投げた直後は UI スレッドが
    #   まだ検索に入っていない可能性があるため、先に間を置いてから確認を始める。
    time.sleep(1.0)
    deadline = time.time() + timeout
    while True:
        if time.time() >= deadline:
            raise AssertionError(f"検索が {timeout} 秒で終わらなかった")
        try:
            win32gui.SendMessageTimeout(dlg, win32con.WM_NULL, 0, 0,
                                        win32con.SMTO_ABORTIFHUNG, 1000)
            break
        except Exception:
            time.sleep(0.5)
    time.sleep(0.5)
    if win32gui.IsWindow(dlg):
        # このダイアログは検索後も開きっぱなし（原と同じ）。WM_CLOSE で閉じる。
        win32gui.PostMessage(dlg, win32con.WM_CLOSE, 0, 0)
    _wait_no_dialog(drv, timeout=timeout)
    time.sleep(0.3)


def _revert_confirmed(drv: StirlingDriver, name: str) -> None:
    """ディスクから再読込する。確認ダイアログが出ることまで検証する。

    未編集の文書では OnRevertFile が beep して即 return するため確認ダイアログが
    出ない。ダイアログの出現を必須にすることで「編集済みと認識され、実際に
    再読込が走った」ことを担保する（出ないまま通ると再読込を検証できていない）。
    """
    drv.post_command(ID_REVERT_FILE)
    _answer_yes(drv, required=True)   # 「編集内容を破棄して再読み込み」の確認
    _answer_yes(drv, required=True)   # 再読込は OnOpenDocument 経由なので 512MB 確認が再度出る
    _attach_main_frame(drv)
    _wait_document_open(drv, name)


def _close_and_wait(drv: StirlingDriver, timeout: float = 120.0) -> bool:
    handle = win32api.OpenProcess(win32con.SYNCHRONIZE, False, drv.pid)
    try:
        win32gui.PostMessage(drv.hwnd, win32con.WM_CLOSE, 0, 0)
        return win32event.WaitForSingleObject(handle, int(timeout * 1000)) == win32event.WAIT_OBJECT_0
    finally:
        win32api.CloseHandle(handle)


@pytest.mark.ported
class TestLargeFile:
    """2GB 超ファイルでの読み込み／末尾ジャンプ／編集／検索／保存／再読込。"""

    def test_open_and_jump_beyond_2gb(self, ported_exe_path, large_file):
        """2GB 超のファイルを開き、末尾および 2GB 境界を跨ぐ位置へジャンプできる。"""
        with StirlingDriver(ported_exe_path) as drv:
            _open_large_file(drv, large_file)

            drv.jump_to_address(f"{LARGE_SIZE:X}", is_hex=True)   # データ末尾
            assert _current_address(drv) == LARGE_SIZE, "末尾へジャンプできていない"

            drv.jump_to_address(f"{MARKER_POS:X}", is_hex=True)
            assert _current_address(drv) == MARKER_POS, "2GB 超の位置へジャンプできていない"

            drv.jump_to_address("0", is_hex=True)
            assert _current_address(drv) == 0, "先頭へ戻れていない"

            assert _close_and_wait(drv), "終了しなかった"

    def test_edit_beyond_2gb_and_save(self, ported_exe_path, large_file, tmp_path):
        """2GB 超の位置を上書き編集して保存し、保存結果をバイト単位で確認する。

        併せて、編集していない先頭・末尾のバイトが保持されていることも確認する
        （オフセットの桁落ちがあると、この 3 点のいずれかが必ず壊れる）。
        """
        out = tmp_path / "large_saved.bin"
        with StirlingDriver(ported_exe_path) as drv:
            _open_large_file(drv, large_file)

            drv.jump_to_address(f"{EDIT_POS:X}", is_hex=True)
            assert _current_address(drv) == EDIT_POS
            drv.focus_view()
            # focus_view() のクリックでキャレットが動くため、クリック後に入れ直す。
            drv.jump_to_address(f"{EDIT_POS:X}", is_hex=True)
            drv.type_hex_chars(f"{EDIT_BYTE:02X}")   # 上書きモードで 1 バイト書き換え
            time.sleep(0.5)

            drv.save_as_via_dialog(out)
            assert _close_and_wait(drv), "終了しなかった"

        assert out.exists(), "保存されていない"
        assert out.stat().st_size == LARGE_SIZE, f"サイズが変わった: {out.stat().st_size}"
        assert _read_at(out, EDIT_POS, 1)[0] == EDIT_BYTE, "2GB 超の編集が反映されていない"
        assert _read_at(out, 0, 16) == _read_at(large_file, 0, 16), "先頭が壊れている"
        assert _read_at(out, LARGE_SIZE - 16, 16) == _read_at(large_file, LARGE_SIZE - 16, 16), \
            "末尾が壊れている"
        assert _read_at(out, MARKER_POS, len(MARKER)) == MARKER, "2GB 超のマーカーが壊れている"

    def test_search_and_reload_beyond_2gb(self, ported_exe_path, large_file):
        """2GB 超のパターンを検索でき、編集を破棄する再読込でディスクの内容に戻る。

        マーカーを壊す→検索で見つからなくなる→再読込→再び見つかる、という順で確認する。
        再読込が実際に走ったことは、確認ダイアログが出ること（未編集なら出ない）と、
        壊したマーカーが復活することの両方で担保する。
        """
        pattern = MARKER.hex().upper()
        with StirlingDriver(ported_exe_path) as drv:
            _open_large_file(drv, large_file)

            # 1. 2GB 超にあるマーカーを検索で見つけられる
            drv.jump_to_address("0", is_hex=True)
            _find_hex(drv, pattern)
            found = _current_address(drv)
            assert found == MARKER_POS, f"2GB 超の検索位置が誤り: {found:#X} != {MARKER_POS:#X}"

            # 2. マーカー先頭を上書きして壊す（2GB 超の編集）
            drv.focus_view()   # クリックでキャレットが動くので、直後に入れ直す
            drv.jump_to_address(f"{MARKER_POS:X}", is_hex=True)
            drv.type_hex_chars("00")
            time.sleep(0.5)
            after_edit = _current_address(drv)
            assert after_edit == MARKER_POS + 1, (
                f"上書き編集がキャレットを進めていない: {after_edit:#X}"
            )

            # 3. 壊したので検索は見つからず、キャレットは動かない（編集が効いた証拠）
            drv.jump_to_address("0", is_hex=True)
            _find_hex(drv, pattern)
            assert _current_address(drv) == 0, "壊したはずのマーカーが見つかっている"

            # 4. 再読込でディスクの内容に戻る（確認ダイアログの出現も検証する）
            _revert_confirmed(drv, large_file.name)

            # 5. マーカーが復活し、再び 2GB 超の位置で見つかる
            drv.jump_to_address("0", is_hex=True)
            _find_hex(drv, pattern)
            reloaded = _current_address(drv)
            assert reloaded == MARKER_POS, (
                f"再読込後にマーカーが復活していない: {reloaded:#X} != {MARKER_POS:#X}"
            )

            assert _close_and_wait(drv), "終了しなかった"
