"""GDI オブジェクト・カーネルハンドルのリーク検証（Issue #49 / 親 #16）。

#48 で GDI オブジェクトとカーネルハンドルを RAII 化した。描画・ドキュメント開閉・
ペイン表示切替はいずれも繰り返し実行される経路で、1 回あたり 1 個の取りこぼしでも
長時間運用で GDI オブジェクト枯渇に至る。

ウォームアップ後の資源数を基準に、同じ操作を繰り返しても資源数が増え続けないことを
確認する。キャッシュの充填で初回だけ増える分は基準取得前に済ませる。
"""
import ctypes
import time
from pathlib import Path

import pytest
import win32api
import win32con
import win32process

from drivers.stirling_driver import (
    CMD_FILE_CLOSE,
    ID_GOTO_DATA_END,
    ID_GOTO_DATA_TOP,
    StirlingDriver,
)

GR_GDIOBJECTS = 0
GR_USEROBJECTS = 1

# ウォームアップ（基準取得前）と計測サイクル数。
WARMUP_CYCLES = 3
MEASURE_CYCLES = 12

# 許容増分。RAII 化後は原理的に 0 増だが、MFC 内部のキャッシュや遅延解放で
# 数個は揺れる。1 サイクルあたり 1 個以上増えるならリークとみなす閾値にする。
TOLERANCE = {"gdi": 8, "user": 8, "handles": 16}

SAMPLE = bytes(range(256)) * 8


def _handle_count(pid: int) -> int:
    """プロセスのオープンハンドル数。"""
    h = win32api.OpenProcess(win32con.PROCESS_QUERY_INFORMATION, False, pid)
    try:
        count = ctypes.c_ulong(0)
        ok = ctypes.windll.kernel32.GetProcessHandleCount(
            int(h), ctypes.byref(count)
        )
        if not ok:
            raise ctypes.WinError(ctypes.get_last_error())
        return int(count.value)
    finally:
        h.Close()


def _resources(pid: int) -> dict[str, int]:
    h = win32api.OpenProcess(win32con.PROCESS_QUERY_INFORMATION, False, pid)
    try:
        return {
            "gdi": win32process.GetGuiResources(int(h), GR_GDIOBJECTS),
            "user": win32process.GetGuiResources(int(h), GR_USEROBJECTS),
            "handles": _handle_count(pid),
        }
    finally:
        h.Close()


def _exercise(drv: StirlingDriver, path: Path) -> None:
    """1 サイクル分の操作。GDI（フォント／DIB／DC）とハンドル（ファイル／監視）を触る。"""
    # ドキュメントの開閉（ファイルハンドル・更新監視ハンドル・ビューのフォント）
    drv.open_file_via_dialog(path)
    time.sleep(0.2)

    # 再描画を強制（描画経路のフォント／ペン生成）
    drv.post_command(ID_GOTO_DATA_END)
    time.sleep(0.1)
    drv.post_command(ID_GOTO_DATA_TOP)
    time.sleep(0.1)

    # 構造体編集バー（ダイアログバー＋リスト）
    drv.toggle_struct_bar(True)
    time.sleep(0.2)
    drv.toggle_struct_bar(False)
    time.sleep(0.2)

    # ビットイメージ（CreateDIBSection ＋ メモリ DC）
    drv.toggle_bit_image()
    time.sleep(0.3)
    drv.toggle_bit_image()
    time.sleep(0.2)

    drv.post_command(CMD_FILE_CLOSE)
    time.sleep(0.4)


class TestIssue49ResourceLeak:
    """繰り返し操作で GDI / USER / ハンドルが増え続けないことを確認する。"""

    @pytest.mark.ported
    def test_no_resource_growth_over_repeated_cycles(self, ported_exe_path, tmp_path):
        src = tmp_path / "leak_probe.dat"
        src.write_bytes(SAMPLE)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start()
            time.sleep(0.5)

            for _ in range(WARMUP_CYCLES):
                _exercise(drv, src)

            baseline = _resources(drv.pid)

            for _ in range(MEASURE_CYCLES):
                _exercise(drv, src)

            after = _resources(drv.pid)

        growth = {k: after[k] - baseline[k] for k in baseline}
        report = (
            f"baseline={baseline} after={after} growth={growth} "
            f"cycles={MEASURE_CYCLES}"
        )
        print(f"[LEAK] {report}")

        for key, limit in TOLERANCE.items():
            assert growth[key] <= limit, (
                f"{key} が {MEASURE_CYCLES} サイクルで {growth[key]} 増加した"
                f"（許容 {limit}）: {report}"
            )
