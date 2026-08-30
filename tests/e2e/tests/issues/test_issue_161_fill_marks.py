"""Issue #161: 範囲初期化で範囲内のダイナミックマークが消えるかどうかを原版と揃える。

長さの変わらない上書きである「範囲の初期化」で、初期化範囲内に置いたマークが残るかを
原版・移植版の双方で確認する。マーク位置は直接読めないため、次の手順で間接的に観測する。

  1. オフセット 8 にマークを付ける
  2. オフセット 4..B（8 を含む）を 0xFF で初期化する
  3. 先頭へ移動してから「次のマークへ」を実行する
     - マークが残っていればキャレットは 8 へ移動する
     - マークが消えていればマークが 1 件も無いのでキャレットは 0 のまま
  4. キャレット位置の 1 バイトを削除して保存する
     - マークが残っていた場合: 先頭は 0x00 のまま（8 番目の 0xFF が消える）
     - マークが消えていた場合: 先頭の 0x00 が消えて 0x01 始まりになる
"""

import time

import pytest

from drivers.settings_context import stirling_settings
from drivers.stirling_driver import StirlingDriver

SAMPLE = bytes(range(32))       # 0x00..0x1F
MARK_POS = 8                    # マークを置くオフセット
FILL_START, FILL_END = 4, 0xB   # 初期化する範囲（両端含む。MARK_POS を内側に含む）
FILL_VALUE = "FF"


def _mark_survives_fill(exe_path, tmp_path, tag: str) -> bool:
    """範囲初期化のあとマークが残っているかを、上記手順の保存結果から判定する。"""
    test_file = tmp_path / f"fill_marks_{tag}.dat"
    out_file = tmp_path / f"fill_marks_{tag}_out.dat"
    test_file.write_bytes(SAMPLE)

    with stirling_settings(dynamic_mark=True):
        with StirlingDriver(exe_path) as drv:
            drv.start(test_file)
            drv.focus_view()

            drv.jump_to_address(f"{MARK_POS:X}", is_hex=True)
            time.sleep(0.2)
            drv.mark_toggle()
            time.sleep(0.2)

            drv.select_range_dialog(f"{FILL_START:X}", f"{FILL_END:X}", is_hex=True)
            time.sleep(0.2)
            drv.fill_range_dialog(FILL_VALUE)
            time.sleep(0.2)

            # 選択を解除してから先頭へ戻す（この後の Delete が選択削除にならないように）。
            drv.jump_to_address("0", is_hex=True)
            time.sleep(0.2)
            drv.mark_next()
            time.sleep(0.2)
            drv.press_delete()
            time.sleep(0.2)

            drv.save_as_via_dialog(out_file)

    assert out_file.exists(), f"{tag}: 保存されなかった"
    saved = out_file.read_bytes()
    assert len(saved) == len(SAMPLE) - 1, f"{tag}: 1 バイト削除されていない: {saved!r}"

    # 初期化されているのは 4..B。マークが残っていればその中の 1 バイトが消える。
    survived = saved[0] == 0x00
    if not survived:
        assert saved[0] == 0x01, f"{tag}: 想定外の保存結果: {saved!r}"
    print(f"[issue161] {tag}: mark survives fill = {survived} / saved={saved.hex()}")
    return survived


@pytest.mark.original
def test_original_fill_range_keeps_mark(original_exe_path, tmp_path):
    """原版: 範囲初期化で範囲内のマークが残ることを確認する（期待挙動の基準）。"""
    assert _mark_survives_fill(original_exe_path, tmp_path, "orig") is True


@pytest.mark.ported
def test_ported_fill_range_keeps_mark(ported_exe_path, tmp_path):
    """移植版: 原版と同じく、範囲初期化で範囲内のマークが残る。"""
    assert _mark_survives_fill(ported_exe_path, tmp_path, "port") is True
