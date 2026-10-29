"""Issue #337: 画面の文字欄で、最終描画行の行末をまたぐ 2 バイト文字を後半バイトまで描く。

画面は「完全に見える行 + 下端で途中まで見える行」を描く。SJIS / EUC は窓の末尾を先読み
していなかったため、最後の行の最終列から窓外の次行へまたがる 2 バイト文字は後半バイトが
読めず「.」になっていた（スクロールして次行を窓に入れると正しく描かれる）。原版の画面の
文字欄（FUN_00441b66 / FUN_00441e45）はバイト列を読み進めて描くので、後半バイトまで描く。

ビューの高さを、最後の行が 2/3 ほど見えるように合わせ、ビューを撮り比べる:
  R: 全バイト 'A'（基準）
  T: 1 行目の最終列だけ 'B'（1 桁に収まる文字の対照）
  U: 1 行目の最終列の 1 つ前だけ 'B'（T との差で 1 桁の幅を測る）
  P: 全行の最終列 0x88 + 次行の先頭 0x9F（「亜」が全行で行末をまたぐ）
P と R の差分のうち最後の行の範囲は、修正後は「亜」が 17 桁目まで描かれるので T の差分より
右へはみ出す。修正前は「.」になるので、T とほぼ同じ位置で止まる。「亜」は上端の横画が右端
近くまで届くため、行の上 2/3 しか見えなくても右端が写る。
"""

import statistics

import pytest
import win32con
import win32gui

from drivers.settings_context import settings_value
from drivers.stirling_driver import StirlingDriver, ID_CHARSET_SJIS

pytestmark = pytest.mark.ported

PORT_REC0 = r"Software\StirHex\StirHex\Rec0"


@pytest.fixture(autouse=True)
def line_size_16():
    """データの配置（16 バイト/行）と表示の 1 行バイト数を揃える。保存済みの設定が 32 などだと、
    対照の 'B' と最後の行の「亜」の列がずれ、修正前でも合格しうる。"""
    with settings_value(PORT_REC0, "LineSize", 16):
        yield


# 表示行数より十分多い行数にして、最後の描画行の次の行もファイル内にあるようにする。
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
DATA_P = _with({pos: value
                for row in range(ROWS - 1)
                for pos, value in ((row * 16 + 15, 0x88), (row * 16 + 16, 0x9F))})


def _child_frame(drv) -> int:
    return win32gui.GetParent(drv.get_view_hwnd())


def _view_height(drv) -> int:
    _, _, _, bottom = win32gui.GetClientRect(drv.get_view_hwnd())
    return bottom


def _resize_child(drv, width: int, height: int):
    child = _child_frame(drv)
    left, top, _, _ = win32gui.GetWindowRect(child)
    x, y = win32gui.ScreenToClient(win32gui.GetParent(child), (left, top))
    win32gui.SetWindowPos(child, 0, x, y, width, height,
                          win32con.SWP_NOZORDER | win32con.SWP_NOACTIVATE)
    drv.wait_idle()


def _open(drv, test_file, data: bytes):
    test_file.write_bytes(data)
    drv.start(test_file)
    drv.wait_idle()
    drv.post_command(ID_CHARSET_SJIS)


def _ink_rows(capture) -> list[bool]:
    """Per scanline, whether the middle of the view (the hex column) holds anything other than
    the most common (background) pixel. The address column on the left is shaded on every
    scanline, so it is left out."""
    pixels, w, h = capture
    stride = w * 4
    x0, x1 = w // 4, w * 3 // 4
    background = statistics.mode(pixels[i:i + 4] for i in range(0, len(pixels), 4 * 97))
    blank = background * (x1 - x0)
    return [pixels[y * stride + x0 * 4:y * stride + x1 * 4] != blank for y in range(h)]


def _row_pitch(capture) -> int:
    """Row height in pixels, from the spacing of the text bands of an all-'A' view."""
    ink = _ink_rows(capture)
    starts = [y for y in range(1, len(ink)) if ink[y] and not ink[y - 1]]
    gaps = [b - a for a, b in zip(starts, starts[1:])]
    assert len(gaps) >= 5, f"could not find the text rows of the view (bands at {starts})"
    return int(statistics.median(gaps))


def _rightmost_diff_per_line(a, b) -> list[int]:
    """For each scanline, the rightmost x at which two same-size captures differ (-1 if none)."""
    pixels_a, w, h = a
    pixels_b, w_b, h_b = b
    assert (w, h) == (w_b, h_b), "view size differs between captures"
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


def test_dbcs_pair_straddling_last_drawn_row_is_drawn_whole(ported_exe_path, tmp_path):
    test_file = tmp_path / "screen_dbcs_lookahead.bin"

    # 1 回目: 既定の大きさで行の高さを測り、最後の行が 2/3 ほど見えるビューの高さを決める。
    with StirlingDriver(ported_exe_path) as drv:
        _open(drv, test_file, DATA_R)
        row_h = _row_pitch(drv.capture_view_image())
        view_h = _view_height(drv)
        left, top, right, bottom = win32gui.GetWindowRect(_child_frame(drv))
    frame_w, frame_h = right - left, bottom - top
    partial = max(1, row_h * 2 // 3)
    full_rows = (view_h - row_h) // row_h - 1         # ヘッダ 1 行を除き、1 行分は縮める側に余裕を取る
    assert full_rows >= 3, f"view is too small (height={view_h}px, row={row_h}px)"
    target_h = row_h + full_rows * row_h + partial
    target_frame_h = frame_h - (view_h - target_h)

    def capture(data: bytes):
        with StirlingDriver(ported_exe_path) as drv:
            _open(drv, test_file, data)
            _resize_child(drv, frame_w, target_frame_h)
            assert _view_height(drv) == target_h, \
                f"view height is {_view_height(drv)}px, expected {target_h}px"
            captured = drv.capture_view_image()
            assert captured is not None, "view could not be captured"
            return captured

    view_r = capture(DATA_R)
    view_t = capture(DATA_T)
    view_u = capture(DATA_U)
    view_p = capture(DATA_P)

    right_t = max(_rightmost_diff_per_line(view_r, view_t))
    right_u = max(_rightmost_diff_per_line(view_r, view_u))
    cell = right_t - right_u
    assert cell >= 4, f"could not measure the column width (x={right_t} / {right_u})"

    # 最後の描画行（下端で途中まで見える行）は、差分のいちばん下の帯にある。ヘッダ行の高さが
    #   データ行と同じとは限らないので、位置は計算せず、下端から差分の無い走査線までをその帯とする。
    lines = _rightmost_diff_per_line(view_r, view_p)
    y = len(lines) - 1
    while y >= 0 and lines[y] < 0:
        y -= 1
    band = []
    while y >= 0 and lines[y] >= 0:
        band.append(lines[y])
        y -= 1
    assert band, "the last drawn row did not change"
    assert len(band) < row_h, f"could not separate the last drawn row ({len(band)} scanlines)"
    right_last = max(band)
    # 2 バイト文字は 17 桁目まで描かれるので、16 桁目の 1 桁文字より右へ半桁以上はみ出す。
    assert right_last - right_t >= cell / 2, (
        f"the pair straddling the end of the last drawn row was not drawn whole "
        f"(rightmost change: last row x={right_last}, single byte x={right_t}, "
        f"column width={cell}, row height={row_h})")
