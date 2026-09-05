"""Issue #121: ビットイメージ窓の表示状態と配置を保存／復元する。

原版は設定の値名自体を持たず（Ghidra で確認済み。Issue のコメント参照）、起動のたびに
「非表示・本体ウィンドウの左隣にフローティング」へ初期化される。移植版では phase5 の
独自機能として、表示状態・フローティング位置・ドッキング先を保存して復元する。

サイズは原版・移植版ともユーザーが変更できない（MFC が CalcFixedLayout の固定寸法へ
戻す）ため、保存対象にしていない。ここでもサイズは検証しない。
"""

import time
from contextlib import ExitStack

import pytest

from drivers.settings_context import (
    deleted_settings_values,
    read_reg_values,
    settings_value,
)
from drivers.stirling_driver import StirlingDriver

PORT_ENV = r"Software\StirHex\StirHex\Env"

REG_DWORD = 4

# 配置（CAppSettings::bitImagePlacement）
PLACEMENT_FLOATING = 0
PLACEMENT_DOCK_LEFT = 1
PLACEMENT_DOCK_RIGHT = 2


def _settings(stack: ExitStack, **values):
    """複数の設定値をまとめて差し替える（各値はテスト後に元へ戻る）。"""
    for name, value in values.items():
        stack.enter_context(settings_value(PORT_ENV, name, value))


def _wait_for_setting(name: str, expected: int, timeout: float = 10.0):
    """終了処理が設定を書き終えるまで待って値を突き合わせる。

    保存は ExitInstance で行われるため、終了要求から書き終わるまでに間がある
    （Issue #148 のテストと同じ理由）。
    """
    deadline = time.time() + timeout
    while True:
        value = read_reg_values(PORT_ENV).get(name)
        if value == (expected, REG_DWORD):
            return
        if time.time() >= deadline:
            pytest.fail(f"{name} が {expected} にならなかった（現在値: {value}）")
        time.sleep(0.1)


@pytest.mark.ported
def test_ported_restores_visibility(ported_exe_path, tmp_path):
    """表示状態が終了時に保存され、次回起動で復元される。"""
    test_file = tmp_path / "bitimage_show.dat"
    test_file.write_bytes(bytes(range(256)))

    with ExitStack() as stack:
        _settings(stack, BitImageShow=0, BitImagePlacement=PLACEMENT_FLOATING)

        # 1回目: 非表示で開始 → トグルで表示 → 終了時に 1 が保存される。
        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            time.sleep(0.3)
            assert not drv.is_bit_image_visible(), "既定は非表示のはず"
            drv.toggle_bit_image()
            assert drv.is_bit_image_visible(), "トグルで表示になるはず"
        _wait_for_setting("BitImageShow", 1)

        # 2回目: 表示状態で復元される → トグルで非表示 → 終了時に 0 が保存される。
        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            time.sleep(0.3)
            assert drv.is_bit_image_visible(), "前回終了時の表示状態が復元されるはず"
            drv.toggle_bit_image()
            assert not drv.is_bit_image_visible(), "トグルで非表示になるはず"
        _wait_for_setting("BitImageShow", 0)

        # 3回目: 非表示で復元される。
        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            time.sleep(0.3)
            assert not drv.is_bit_image_visible(), "非表示状態が復元されるはず"


@pytest.mark.ported
def test_ported_restores_floating_position(ported_exe_path, tmp_path):
    """フローティング位置が終了時に保存され、次回起動で復元される。"""
    test_file = tmp_path / "bitimage_pos.dat"
    test_file.write_bytes(bytes(range(256)))

    moved_to = (520, 260)

    with ExitStack() as stack:
        _settings(stack, BitImageShow=1, BitImagePlacement=PLACEMENT_FLOATING,
                  BitImageLeft=300, BitImageTop=200)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            time.sleep(0.3)
            assert drv.is_bit_image_floating(), "フローティングで復元されるはず"
            assert drv.bit_image_frame_rect()[:2] == (300, 200), (
                "設定に書いた位置で開くはず"
            )
            drv.move_bit_image_window(*moved_to)
        _wait_for_setting("BitImageLeft", moved_to[0])
        _wait_for_setting("BitImageTop", moved_to[1])

        # 移動後の位置で復元される。
        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            time.sleep(0.3)
            assert drv.bit_image_frame_rect()[:2] == moved_to, (
                "前回終了時のフローティング位置が復元されるはず"
            )


@pytest.mark.ported
def test_ported_restores_docked_placement(ported_exe_path, tmp_path):
    """ドッキング先が復元される。ドッキング不可設定ならフローティングへ戻す。"""
    test_file = tmp_path / "bitimage_dock.dat"
    test_file.write_bytes(bytes(range(256)))

    with ExitStack() as stack:
        # ドッキング可能（環境設定「ウィンドウ」1066）にしたうえで左ドックを指定する。
        _settings(stack, BitImageShow=1, BitImageDockable=1,
                  BitImagePlacement=PLACEMENT_DOCK_LEFT)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            time.sleep(0.3)
            assert drv.is_bit_image_visible(), "表示状態で復元されるはず"
            assert not drv.is_bit_image_floating(), "左ドッキングで復元されるはず"
            # ドッキング中はメインフレーム配下にある。
            assert drv.bit_image_frame_hwnd() == drv.hwnd

    with ExitStack() as stack:
        # ドッキング先を保存したあとに「ドッキング可能」を OFF にした状態。
        #   ドッキングできない以上、フローティングへ戻さないと配置が破綻する。
        _settings(stack, BitImageShow=1, BitImageDockable=0,
                  BitImagePlacement=PLACEMENT_DOCK_LEFT)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            time.sleep(0.3)
            assert drv.is_bit_image_floating(), (
                "ドッキング不可の設定ではフローティングへ戻すはず"
            )


@pytest.mark.ported
def test_ported_offscreen_position_falls_back(ported_exe_path, tmp_path):
    """画面構成が変わって復元位置が画面外になる場合は既定位置へフォールバックする。"""
    test_file = tmp_path / "bitimage_offscreen.dat"
    test_file.write_bytes(bytes(range(256)))

    with ExitStack() as stack:
        # どのモニタにもかからない座標（メインウィンドウの配置復元と同じ考え方）。
        _settings(stack, BitImageShow=1, BitImagePlacement=PLACEMENT_FLOATING,
                  BitImageLeft=-30000, BitImageTop=-30000)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            time.sleep(0.3)
            left, top = drv.bit_image_frame_rect()[:2]
            assert (left, top) != (-30000, -30000), (
                "画面外の保存位置をそのまま使ってはいけない"
            )
            # 既定位置は本体ウィンドウの左隣。少なくとも画面内に出ていること。
            import win32api
            monitor = win32api.MonitorFromPoint((left + 1, top + 1), 0)  # DEFAULTTONULL
            assert monitor, f"既定位置へフォールバックするはず（実際: {left}, {top}）"


@pytest.mark.ported
def test_ported_env_settings_keeps_bit_image_state(ported_exe_path, tmp_path):
    """環境設定のOKで設定一式を保存しても、ビットイメージの状態は巻き戻らない。

    環境設定はダイアログを開いた時点の設定の複製を確定させる。表示状態と配置は
    ダイアログに項目が無い実行時の値なので、保存の前に現在値へ戻す必要がある
    （Issue #148 と同じ理由）。
    """
    test_file = tmp_path / "bitimage_env.dat"
    test_file.write_bytes(bytes(range(256)))

    with ExitStack() as stack:
        _settings(stack, BitImageShow=0, BitImagePlacement=PLACEMENT_FLOATING)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            time.sleep(0.3)
            drv.toggle_bit_image()
            assert drv.is_bit_image_visible(), "トグルで表示になるはず"
            sheet = drv.open_env_settings_dialog()
            drv.close_settings_sheet(sheet, accept=True)
            time.sleep(0.5)
            assert drv.is_bit_image_visible(), "環境設定のOKで表示状態が変わらないはず"
            # OK の時点で設定一式が保存される。表示中の値が書かれていること。
            _wait_for_setting("BitImageShow", 1)


@pytest.mark.ported
def test_ported_never_writes_position_sentinel(ported_exe_path, tmp_path):
    """一度もフローティングしていない状態でも、番兵値を設定ファイルへ書き出さない。

    フローティング位置の「未保存」は内部的に INT_MIN で表す。ドッキング指定のまま
    起動して終了すると、位置を一度も採取しないまま保存処理へ入る。この経路で
    BitImageLeft=-2147483648 のような内部値が設定ファイルへ漏れないことを確かめる。
    """
    test_file = tmp_path / "bitimage_sentinel.dat"
    test_file.write_bytes(bytes(range(256)))

    int_min = -(2 ** 31)

    with ExitStack() as stack:
        # 位置のキーを消し、ドッキング指定で起動する（＝ミニフレームが生成されない）。
        stack.enter_context(
            deleted_settings_values(PORT_ENV, ["BitImageLeft", "BitImageTop"])
        )
        _settings(stack, BitImageShow=1, BitImageDockable=1,
                  BitImagePlacement=PLACEMENT_DOCK_LEFT)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            time.sleep(0.3)
            assert not drv.is_bit_image_floating(), "左ドッキングで復元されるはず"
        _wait_for_setting("BitImagePlacement", PLACEMENT_DOCK_LEFT)

        values = read_reg_values(PORT_ENV)
        for name in ("BitImageLeft", "BitImageTop"):
            written = values.get(name)
            assert written is None or written[0] != int_min, (
                f"{name} に番兵値が書き出された（{written}）"
            )
