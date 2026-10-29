"""Issue #324: BGREP で対象ファイルの読み取りに失敗したら、そのファイルの検索を打ち切ったことを知らせる。

BGREP は対象ファイルを必要な部分だけ読みながら検索する。検索中に読み取れなくなると、
以前は「見つからない」と区別できず、そのファイルの残りを黙って打ち切っていた。
読み取りの失敗は、テスト側のプロセスがファイルの途中に排他の範囲ロック（LockFileEx）を
かけて起こす。ロックした範囲の ReadFile は ERROR_LOCK_VIOLATION で失敗する。

移植版の仕様（原版はファイル全体を読み込んでから検索するため、該当する動作が無い）。
"""
import contextlib
from pathlib import Path

import pytest
import pywintypes
import win32con
import win32file

from drivers import bgrep
from drivers.stirling_driver import StirlingDriver

MARKER = b"\xC3\x24\x9E\x5A\x71\x0D"
FILE_SIZE = 0x40000             # 256KB（64KB ページ 4 枚）
HIT_BEFORE = 0x1000             # ロック範囲より手前（先頭ページ）
HIT_AFTER = 0x30000             # ロック範囲より後ろ（4 枚目のページ）
LOCK_POS = 0x20000              # 3 枚目のページの先頭をロックする
LOCK_LEN = 0x1000
GOOD_HIT = 0x50                 # 正常なファイルのヒット位置
READ_FAILED_SUFFIX = " : 読み取りに失敗したため、このファイルの検索を打ち切りました"


def _write(path: Path, size: int, hits: tuple[int, ...]) -> None:
    data = bytearray(size)
    for pos in hits:
        data[pos:pos + len(MARKER)] = MARKER
    path.write_bytes(bytes(data))


@contextlib.contextmanager
def _range_locked(path: Path, pos: int, length: int):
    """path の [pos, pos+length) に排他ロックをかける（他プロセスの読み取りを禁止する）。"""
    share = win32con.FILE_SHARE_READ | win32con.FILE_SHARE_WRITE | win32con.FILE_SHARE_DELETE
    handle = win32file.CreateFile(str(path), win32con.GENERIC_READ, share, None,
                                  win32con.OPEN_EXISTING, 0, None)
    overlapped = pywintypes.OVERLAPPED()
    overlapped.Offset = pos
    flags = win32con.LOCKFILE_EXCLUSIVE_LOCK | win32con.LOCKFILE_FAIL_IMMEDIATELY
    try:
        win32file.LockFileEx(handle, flags, length, 0, overlapped)
        try:
            yield
        finally:
            win32file.UnlockFileEx(handle, length, 0, overlapped)
    finally:
        handle.Close()


def _hits(lines: list[str], path: Path) -> set[int]:
    """path の結果行からヒット位置を取り出す（他ファイルの行・情報行は除く）。"""
    prefix = str(path).lower() + " : "
    out = set()
    for line in lines:
        text = line.strip()
        if not text.lower().startswith(prefix):
            continue
        tail = text[len(prefix):]
        if all(c in "0123456789abcdefABCDEF" for c in tail):
            out.add(int(tail, 16))
    return out


@pytest.mark.ported
class TestIssue324BgrepReadFailure:
    def test_read_failure_is_reported_and_other_files_continue(self, ported_exe_path, tmp_path):
        folder = tmp_path / "bgrep"
        folder.mkdir()
        locked = folder / "locked.dat"
        good = folder / "good.dat"
        _write(locked, FILE_SIZE, (HIT_BEFORE, HIT_AFTER))
        _write(good, 0x1000, (GOOD_HIT,))
        data = " ".join(f"{b:02X}" for b in MARKER)
        # マスクは指定順に処理されるため、失敗するファイルを先に検索させる（列挙順は名前順とは
        #   限らない）。失敗の後も他のファイルの検索が続くことを確かめるため。
        mask = "locked.dat;good.dat"

        with StirlingDriver(ported_exe_path) as drv:
            drv.start()

            # 対照: ロックしなければ両方のヒットが見つかり、失敗の行は出ない。
            lines, applied = bgrep.run(drv, folder, data, file_mask=mask)
            assert _hits(lines, locked) == {HIT_BEFORE, HIT_AFTER}, \
                f"ロック無しで両方のヒットが見つからない: {lines}（実効値: {applied}）"
            assert _hits(lines, good) == {GOOD_HIT}, f"正常なファイルのヒットが無い: {lines}"
            assert not [l for l in lines if l.endswith(READ_FAILED_SUFFIX)], \
                f"読めるのに読み取り失敗の行が出た: {lines}"

            # ロック範囲で読み取りに失敗させる。
            with _range_locked(locked, LOCK_POS, LOCK_LEN):
                lines, applied = bgrep.run(drv, folder, data, file_mask=mask)

        failed = [l for l in lines if l.endswith(READ_FAILED_SUFFIX)]
        assert failed == [str(locked) + READ_FAILED_SUFFIX], \
            f"読み取り失敗の行が対象ファイルについて 1 行だけ出ていない: {lines}（実効値: {applied}）"
        assert _hits(lines, locked) == {HIT_BEFORE}, \
            f"失敗より前のヒットだけが残っていない（後ろは検索しない）: {lines}"
        assert _hits(lines, good) == {GOOD_HIT}, \
            f"失敗したファイル以外の検索が続いていない: {lines}"
        good_line = f"{good} : {GOOD_HIT:08x}".lower()
        good_index = [l.strip().lower() for l in lines].index(good_line)
        assert lines.index(failed[0]) < good_index, \
            f"失敗したファイルの後に、他のファイルの検索が続いていない: {lines}"
