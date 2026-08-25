"""日本語ファイル名・日本語を含む長いパスの open / save 検証（Issue #49 / 親 #16）。

Unicode 化（#41）でファイルパスは wide 層へ移行した。ANSI 版では ACP に依存していた
パス取り扱いが、ACP に関係なく成立することを確認する。

長いパスは **MAX_PATH 未満** の範囲で検証する。260 文字超のパスはアプリケーション
マニフェストの `longPathAware` オプトインが必要で、現時点では有効化していない
（analysis_artifacts/docs/21_modernize_summary.md の残置項目を参照）。
"""
import time
from pathlib import Path

import pytest

from drivers.stirling_driver import CMD_FILE_SAVE, StirlingDriver

# 非 ASCII のみで構成した名前（CP932 にも UTF-16 にも存在するが、ACP 依存の
# 取り違えがあれば必ず壊れる並び）。
JP_NAME = "テスト＿日本語ファイル名　漢字仮名交じり.dat"
JP_SAVED = "保存結果＿日本語.dat"
# 深い階層を作るためのディレクトリ名（1 段あたり 12 文字）。
JP_DIR = "階層ディレクトリ名前"

SAMPLE = bytes(range(256)) + "日本語テキスト".encode("cp932")


def _make_long_dir(root: Path, target_len: int = 200) -> Path:
    """root 配下に日本語ディレクトリを重ね、パス長が target_len を超えたところで止める。"""
    d = root
    i = 0
    while len(str(d)) < target_len:
        i += 1
        d = d / f"{JP_DIR}{i:02d}"
    d.mkdir(parents=True, exist_ok=True)
    return d


class TestIssue49JapanesePath:
    """日本語パスの open / save が ACP に依存せず成立することを確認する。"""

    @pytest.mark.ported
    def test_japanese_filename_commandline_open_and_save_as(self, ported_exe_path, tmp_path):
        """コマンドライン引数で日本語ファイル名を開き、日本語ファイル名で保存できる。"""
        src = tmp_path / JP_NAME
        src.write_bytes(SAMPLE)
        dest = tmp_path / JP_SAVED

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(src)
            time.sleep(0.5)

            titles = drv.get_mdi_child_titles()
            assert any(JP_NAME in t for t in titles), (
                f"日本語ファイル名が MDI タイトルに現れない: {titles}"
            )

            # 先頭バイトを 0xAB へ上書きしてから別名保存する。
            drv.focus_view()
            drv.type_hex_chars("AB")
            drv.save_as_via_dialog(dest)

        assert dest.exists()
        saved = dest.read_bytes()
        assert saved == b"\xAB" + SAMPLE[1:]

    @pytest.mark.ported
    def test_japanese_filename_open_via_dialog_and_overwrite_save(self, ported_exe_path, tmp_path):
        """ファイルを開くダイアログ経由でも日本語ファイル名を開き、上書き保存できる。"""
        src = tmp_path / JP_NAME
        src.write_bytes(SAMPLE)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start()
            time.sleep(0.5)
            drv.open_file_via_dialog(src)

            titles = drv.get_mdi_child_titles()
            assert any(JP_NAME in t for t in titles), (
                f"ダイアログ経由で開いた日本語ファイル名がタイトルに現れない: {titles}"
            )

            drv.focus_view()
            drv.type_hex_chars("CD")
            drv.post_command(CMD_FILE_SAVE)
            time.sleep(1.0)

        assert src.read_bytes() == b"\xCD" + SAMPLE[1:]

    @pytest.mark.ported
    def test_long_japanese_path_open_and_save_as(self, ported_exe_path, tmp_path):
        """日本語ディレクトリを重ねた長いパス（MAX_PATH 未満）で open / save が通る。"""
        deep = _make_long_dir(tmp_path)
        src = deep / JP_NAME
        dest = deep / JP_SAVED
        assert 200 < len(str(src)) < 260, f"想定した長さのパスになっていない: {len(str(src))}"
        src.write_bytes(SAMPLE)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(src)
            time.sleep(0.5)

            titles = drv.get_mdi_child_titles()
            assert any(JP_NAME in t for t in titles), (
                f"長いパスのファイル名がタイトルに現れない: {titles}"
            )

            drv.focus_view()
            drv.type_hex_chars("EF")
            drv.save_as_via_dialog(dest)

        assert dest.exists()
        assert dest.read_bytes() == b"\xEF" + SAMPLE[1:]
