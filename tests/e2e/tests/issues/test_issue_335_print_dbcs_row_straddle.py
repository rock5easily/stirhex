"""Issue #335: 印刷の文字欄で、行末をまたぐ 2 バイト文字を後半バイトまで描く。

原版の SJIS 文字欄の印刷（FUN_00443a24）は、行の最終列から 2 バイト文字が始まると 17 バイトの
まま ExtTextOut し、次の行の先頭 1 桁を空白にする。移植版は 16 バイトに切り詰めていたため、
先頭バイトだけが描かれ（「・」になる）、その文字が印刷されなかった。ページの最終行では、
次ページ先頭のバイトを読んでいなかったため、切り詰めを直しても「.」になっていた。

印刷内容を直接読む手段は無いので、同じファイル名に内容を書き替えて印刷プレビュー 1 ページ目を
撮り比べる（データはすべて同じ大きさで、複数ページになる行数にする）:
  R: 全バイト 'A'（基準）
  T: 1 行目の最終列だけ 'B'（1 桁に収まる文字の対照）
  U: 1 行目の最終列の 1 つ前だけ 'B'（T との差で 1 桁の幅を測る）
  S: 1 行目の最終列 0x82 + 2 行目の先頭 0xA0（「あ」が行末をまたぐ）
  P: 全行の最終列 0x82 + 次行の先頭 0xA0（ページの最終行でも「あ」が次ページへまたぐ）
修正後は「あ」が 17 桁目まで描かれるため、R との差分の右端が T の場合より右へはみ出す。
修正前は行末が 1 桁の「・」や「.」になるので、T とほぼ同じ位置で止まる。
"""

import datetime
import time

import pytest
import win32con
import win32gui

from drivers.stirling_driver import StirlingDriver, ID_CHARSET_SJIS, ID_FILE_PRINT_PREVIEW

pytestmark = pytest.mark.ported

# 1 ページの行数はプリンタで変わる（実測 60〜80 行台）ので、確実に複数ページになる行数にする。
ROWS = 400
BASE = bytes(b"A" * (16 * ROWS))


def _with(changes: dict[int, int]) -> bytes:
    data = bytearray(BASE)
    for pos, value in changes.items():
        data[pos] = value
    return bytes(data)


DATA_R = BASE
DATA_T = _with({15: ord("B")})
DATA_U = _with({14: ord("B")})         # T を 1 桁左へずらしたもの（1 桁の幅を測る）
DATA_S = _with({15: 0x82, 16: 0xA0})   # 「あ」(82 A0) が 1 行目末から 2 行目へまたぐ
DATA_P = _with({pos: value
                for row in range(ROWS - 1)
                for pos, value in ((row * 16 + 15, 0x82), (row * 16 + 16, 0xA0))})


def _wait_for(predicate, timeout: float) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.1)
    return predicate()


def _capture_preview(exe_path, test_file, data: bytes):
    """Print preview page 1 of `data` as (pixels, width, height)."""
    # ファイル名はプレビューに印字されるので、毎回同じパスへ書き替えて撮る。
    test_file.write_bytes(data)
    with StirlingDriver(exe_path) as drv:
        drv.start(test_file)
        win32gui.ShowWindow(drv.hwnd, win32con.SW_MAXIMIZE)
        drv.wait_idle()
        drv.post_command(ID_CHARSET_SJIS)
        drv.post_command(ID_FILE_PRINT_PREVIEW)
        assert _wait_for(drv.is_print_preview_active, timeout=15.0), "print preview did not open"
        drv.wait_idle()
        captured = drv.capture_window_image(drv.hwnd)
        assert captured is not None, "print preview could not be captured"
        drv.close_print_preview()
        assert _wait_for(lambda: not drv.is_print_preview_active(), timeout=10.0), \
            "print preview did not close"
    return captured


def _rightmost_diff_per_line(a, b) -> list[int]:
    """For each scanline, the rightmost x at which two same-size captures differ (-1 if none)."""
    pixels_a, w, h = a
    pixels_b, w_b, h_b = b
    assert (w, h) == (w_b, h_b), "frame size differs between captures"
    stride = w * 4
    result = []
    for y in range(h):
        row_a = pixels_a[y * stride:(y + 1) * stride]
        row_b = pixels_b[y * stride:(y + 1) * stride]
        rightmost = -1
        if row_a != row_b:
            for x in range(w - 1, -1, -1):
                if row_a[x * 4:x * 4 + 4] != row_b[x * 4:x * 4 + 4]:
                    rightmost = x
                    break
        result.append(rightmost)
    return result


def _capture(exe_path, test_file, datas: list[bytes]):
    """Capture every data in order; retake all if the date changed meanwhile."""
    # プレビューには印刷時の日付が出るため、撮影が日付をまたいだら撮り直す。
    for _ in range(2):
        day = datetime.date.today()
        captures = [_capture_preview(exe_path, test_file, d) for d in datas]
        if datetime.date.today() == day:
            break
    return captures


def _measure_reference(preview_r, preview_t, preview_u) -> tuple[int, int]:
    """Return (rightmost change of a one-column char at the 16th column, column width)."""
    right_t = max(_rightmost_diff_per_line(preview_r, preview_t))
    right_u = max(_rightmost_diff_per_line(preview_r, preview_u))
    # プレビューの縮尺は画面の大きさで変わるので、同じ文字を 1 桁ずらした差を 1 桁の幅とする。
    cell = right_t - right_u
    # 修正前の描画でもアンチエイリアス等で 1 画素ほど右へ出ることがあるため、1 桁が小さすぎる
    #   縮尺では判定できない。誤って合格させず、判定不能として失敗させる。
    assert cell >= 4, (
        f"print preview is too small to tell the columns apart (column width={cell}px); "
        f"run on a larger screen")
    return right_t, cell


def test_dbcs_pair_straddling_row_end_is_printed_whole(ported_exe_path, tmp_path):
    test_file = tmp_path / "print_dbcs_straddle.bin"
    preview_r, preview_t, preview_u, preview_s = _capture(
        ported_exe_path, test_file, [DATA_R, DATA_T, DATA_U, DATA_S])
    right_t, cell = _measure_reference(preview_r, preview_t, preview_u)

    right_s = max(_rightmost_diff_per_line(preview_r, preview_s))
    assert right_s >= 0, "the straddling pair did not change the preview"
    # 2 バイト文字は 17 桁目まで描かれるので、16 桁目の 1 桁文字より右へ半桁以上はみ出す。
    #   修正前は 16 桁目の「・」で止まり、はみ出しはほぼ 0（実測 1 画素）になる。
    assert right_s - right_t >= cell / 2, (
        f"the pair straddling the row end was cut at the 16th column "
        f"(rightmost change: pair x={right_s}, single byte x={right_t}, column width={cell})")


def test_dbcs_pair_straddling_page_end_is_printed_whole(ported_exe_path, tmp_path):
    test_file = tmp_path / "print_dbcs_straddle.bin"
    preview_r, preview_t, preview_u, preview_p = _capture(
        ported_exe_path, test_file, [DATA_R, DATA_T, DATA_U, DATA_P])
    right_t, cell = _measure_reference(preview_r, preview_t, preview_u)

    lines = _rightmost_diff_per_line(preview_r, preview_p)
    changed = [y for y, x in enumerate(lines) if x >= 0]
    assert changed, "the straddling pairs did not change the preview"
    # 1 ページ目の最終行は差分のいちばん下にある。その下半分（1 桁の幅ぶんの走査線）を見る。
    #   「あ」は行の高さのほぼ全体を占めるので、下半分でも 17 桁目まではみ出して見える。
    bottom = changed[-1]
    right_last = max(lines[y] for y in range(bottom - cell + 1, bottom + 1))
    assert right_last - right_t >= cell / 2, (
        f"the pair straddling the page end was not printed on the last row of page 1 "
        f"(rightmost change: last row x={right_last}, single byte x={right_t}, "
        f"column width={cell})")
