"""Issue #313: 印刷は表示の 1 行バイト数に関わらず 16 バイト/行で割り付ける。

原版の印刷（OnBeginPrinting 0x442840 / ダンプ本体 0x442f27）は、ヘッダが 77 桁の定数文字列、
行アドレスが +0x10 刻み、総行数も 16 バイト単位で、表示設定の 1 行バイト数（LineSize）を見ない。
移植版はページ数を 16 バイト単位で数えながら割付だけ LineSize を使っていたため、16 以外では
ページが不足するか空白ページが出た（32 なら 4 ページ中 2 ページが空白で、文字欄も枠外へはみ出す）。

印刷内容を直接読む手段は無いので、同じファイルの印刷プレビュー 1 ページ目を LineSize=16 と 32 で
撮り比べ、画素単位で一致することを確かめる。LineSize が実際に効いていることは、通常の
ビューの描画が両者で異なることで併せて確かめる（素通りしただけの一致を排除する対照）。
"""

import datetime
import time

import pytest
import win32con
import win32gui

from drivers.settings_context import settings_value
from drivers.stirling_driver import StirlingDriver, ID_FILE_PRINT_PREVIEW

pytestmark = pytest.mark.ported

PORT_REC0 = r"Software\StirHex\StirHex\Rec0"


def _wait_for(predicate, timeout: float) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.1)
    return predicate()


def _capture_view_and_preview(exe_path, test_file, line_size: int):
    """Return (view pixels, print preview pixels of page 1) with the given LineSize."""
    with settings_value(PORT_REC0, "LineSize", line_size):
        with StirlingDriver(exe_path) as drv:
            drv.start(test_file)
            # 両方の撮影でフレームの大きさを揃える（プレビューはフレーム全体に出る）。
            win32gui.ShowWindow(drv.hwnd, win32con.SW_MAXIMIZE)
            drv.wait_idle()
            view = drv.capture_view_image()
            assert view is not None, "data view could not be captured"

            drv.post_command(ID_FILE_PRINT_PREVIEW)
            assert _wait_for(drv.is_print_preview_active, timeout=15.0), \
                "print preview did not open"
            drv.wait_idle()
            preview = drv.capture_window_image(drv.hwnd)
            assert preview is not None, "print preview could not be captured"

            drv.close_print_preview()
            assert _wait_for(lambda: not drv.is_print_preview_active(), timeout=10.0), \
                "print preview did not close"
    return view, preview


def test_print_layout_ignores_display_line_size(ported_exe_path, tmp_path):
    # 4096 バイト = 16 バイト/行で 256 行。1 ページに収まらず複数ページになる大きさにする。
    test_file = tmp_path / "print_line_size.bin"
    test_file.write_bytes(bytes(i & 0xFF for i in range(4096)))

    # プレビューには印刷時の日付が出るため、2 回の撮影が日付をまたいだら撮り直す。
    for _ in range(2):
        day = datetime.date.today()
        view16, preview16 = _capture_view_and_preview(ported_exe_path, test_file, 16)
        view32, preview32 = _capture_view_and_preview(ported_exe_path, test_file, 32)
        if datetime.date.today() == day:
            break

    # 対照: LineSize は通常の表示には効いている。
    assert view16 != view32, "LineSize=32 did not change the data view; the setting was not applied"
    # 本題: 印刷の割付は LineSize に依存しない（原と同じ 16 バイト/行）。
    assert preview16[1:] == preview32[1:], "frame size differs between the two captures"
    assert preview16[0] == preview32[0], \
        "print preview page 1 differs between LineSize=16 and LineSize=32"
