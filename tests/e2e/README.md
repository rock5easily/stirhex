# Stirling E2E / ゴールデン比較テスト

バイナリエディタ **Stirling 原版（Ver 1.31）** と **移植版（Visual C++ 2022/2026）** の双方に対して同一の GUI / ウィンドウメッセージ操作を行い、生成されるバイナリデータや挙動が完全一致（ゴールデン突合）するかを自動検証する E2E テストフレームワーク。

---

## 概要と目的

- **ゴールデン比較 (Differential Testing)**: 原バイナリ（`analysis_target/Stirling.exe`）と移植版バイナリ（`porting/StirHex/Release/bin/StirHex.exe`）に同一の編集・ファイル操作を流し込み、出力ファイルの SHA-256 / バイト単位一致を検証する。
- **リグレッション防止**: x64 化や Win32 API 近代化に伴うデータ破損や挙動差異（バグ）の発生を即座に検知する。

---

## 前提条件

- Windows OS
- Python 3.10 以上
- [uv](https://docs.astral.sh/uv/) パッケージマネージャ
- 移植版 `StirHex.exe` のビルド（`porting/StirHex/Release/bin/StirHex.exe` または `Debug` 版）

### Unicode ウィンドウ操作

移植版は Unicode ウィンドウとして動作するため、ドライバが文字列を送受信するときは
`SendMessageW` と各コントロールの W 版メッセージを使用する。`WM_CHAR` の `wParam` には
エンコード済みバイトではなく Unicode コードポイントを渡す。

リストビューやステータスバーなど、`WM_USER` 以上のメッセージで文字列ポインタを渡す
コモンコントロールでは、対象プロセス内に確保したバッファを使用する。

---

## ディレクトリ構成

```text
porting/tests/e2e/
├── pyproject.toml              # uv / pytest / pywinauto 依存およびマーク定義
├── uv.lock                     # 依存関係ロックファイル
├── README.md                   # 本ドキュメント
├── conftest.py                 # 原版・移植版バイナリのパス解決、ゴールデン実行フィクスチャ
├── drivers/
│   ├── __init__.py
│   ├── process_guard.py        # 残留 Stirling / StirHex プロセスの検出・終了
│   ├── settings_context.py     # テスト前後の設定自動セットアップ・復元機構
│   ├── resource_probe.py       # プロセスのメモリ / GDI / USER / ハンドル / スレッド採取
│   ├── resource_loop.py        # 操作の反復計測とリーク判定
│   └── stirling_driver.py      # Win32 / pywinauto Stirling 自動操作ドライバ
└── tests/
    ├── __init__.py
    ├── issues/                 # GitHub Issue 検証テスト群
    │   ├── test_issue_01_shortcuts.py
    │   ├── test_issue_02_subcaret.py
    │   ├── test_issue_03_statusbar.py
    │   ├── test_issue_04_file_watch.py
    │   ├── test_issue_05_single_instance.py
    │   ├── test_issue_07_select_range.py
    │   ├── test_issue_09_user_menu.py
    │   ├── test_issue_10_dynamic_mark.py
    │   └── test_issue_11_lnk_file.py
    ├── test_golden_charset_endian.py   # キャラクタセット / エンディアン
    ├── test_golden_clipboard.py        # コピー / 切り取り / 貼り付け / Undo
    ├── test_golden_cmdline.py          # コマンドライン引数（スペースパス、複数ファイル等）
    ├── test_golden_edit_hex.py         # 16進編集
    ├── test_golden_edit_insert.py      # 挿入モード / 削除 / 文字ペイン
    ├── test_golden_jump_navigation.py  # ジャンプ / 先頭・末尾移動
    ├── test_golden_passthrough.py      # 起動スモーク & 無編集保存
    ├── test_golden_replace.py          # 検索・置換
    ├── test_golden_revert.py           # 再読込（Revert）
    ├── test_golden_save_dump.py        # ダンプ保存
    ├── test_golden_selection_fill.py   # 範囲選択 / 範囲初期化(Fill) / 範囲保存
    ├── test_leak_detector.py           # リーク判定ロジック自体の検証（アプリを起動しない）
    └── stability/                      # 安定性（リソースリーク）テスト
        ├── conftest.py                 # --stability 指定時のみ実行、計測フィクスチャ
        ├── test_resource_leak.py       # 操作を反復してリソース増加を判定
        └── test_crt_leak_report.py     # Debug ビルドの CRT リーク一覧が空であること
```

---

## カスタムマーク (Pytest Markers)

マークは「そのテストがどの実行ファイルを起動するか」を表し、**互いに重ならない**。
ゴールデン比較テストは `golden` だけを持つため、`-m ported` で原版が起動することはない。

| マーク名 | 起動するもの | 説明 |
| :--- | :--- | :--- |
| **`ported`** | 移植版のみ | 移植版 StirHex を対象とするテスト |
| **`original`** | 原版のみ | 原版 Stirling (1.31) を対象とするテスト（互換性の基準を記録する枠） |
| **`golden`** | 両方 | 原版と移植版へ同じ操作を流し、出力の完全一致を検証する |
| **`unit`** | 起動しない | ドライバ自体の検証（ビット数の整合チェック等） |

安定性テスト（`tests/stability/`）も移植版だけを起動するので `ported` を持つ。
実行に数分かかる点だけが他と違うため、マークではなく `--stability` オプションで振り分ける。
指定しない限り、`-m ported` に含まれてもスキップされる。詳細は
[安定性テスト（リソースリーク）](#安定性テストリソースリーク)を参照。

アプリを起動しない静的検証（ヘルプ記述の整合など）は e2e ではなく
`porting/tests/static/` にある。GUI 用の conftest（Win32 依存・残留プロセスの確認・
設定の自動退避）を通らないため、`uv run pytest` だけで数秒で終わる。

```powershell
cd porting/tests/static
uv run pytest
```

---

## テストの実行方法

### 1. 全テストの実行

本ディレクトリに移動し、`uv run pytest` を実行する。初回実行時に仮想環境と依存パッケージが自動解決・インストールされる。

```powershell
cd porting/tests/e2e
uv run pytest
```

※実行結果のレポートは `reports/report.html` および `reports/report.xml` に自動出力されます。
公開済みのレポートは <https://rock5easily.github.io/stirhex/> から参照できます。

### 2. マークを指定してのテスト実行

- **日常の回帰確認（移植版のみ。原版は起動しない）**:
  ```powershell
  uv run pytest -m "ported"
  ```

- **原版との互換確認（ゴールデン比較）**:
  ```powershell
  uv run pytest -m "golden"
  ```

- **原版の仕様確認のみ**:
  ```powershell
  uv run pytest -m "original"
  ```

- **移植版とドライバ検証をまとめて**:
  ```powershell
  uv run pytest -m "ported or unit"
  ```

### 3. 特定のテストファイル・ケースの実行

```powershell
# 特定ファイル
uv run pytest tests/test_golden_edit_hex.py -v

# 特定のテストケース名
uv run pytest -k "test_golden_overwrite_hex" -v
```

---

## 安定性テスト（リソースリーク）

`tests/stability/` は、同じ操作を何十回も繰り返してリソースが増え続けないことを確認する。
1件あたり数分かかるため既定の回帰実行では収集されず、**`--stability` を付けたときだけ**実行される。
起動する実行ファイルは移植版だけなので、マークは他のテストと同じ `ported` を使う。

```powershell
cd porting/tests/e2e

# 安定性テスト一式（既定: 30反復、ウォームアップ5回。全体で6〜7分）
uv run pytest tests/stability --stability

# 反復数を増やして、より小さなリークを分離する
uv run pytest tests/stability --stability --stability-iterations 100 --stability-warmup 10

# 判定ロジック自体の検証だけ（アプリを起動せず数秒で終わる）
uv run pytest tests/test_leak_detector.py
```

計測値は `reports/stability/<テスト名>.csv` に反復ごとの時系列で残るため、
合否だけでなく増加の傾きを後から確認できる。

### 判定のしかた

| 対象 | 判定 |
| :--- | :--- |
| GDI / USER オブジェクト、ハンドル、スレッド | ループの**前半と後半の両方**で増えたときにリークとする |
| メモリ（Private Bytes） | 1反復あたりの増加（最小二乗の傾き）がしきい値（既定 64 KiB）を超えたとき |

前半・後半の両方を見るのは、キャッシュの充填やフォントの実体化、Windows のスレッドプール拡張のように
**一度だけ段差を作って以降は平らになる**動きをリークと区別するため。操作ごとに漏れるリークは
どちらの半分でも増え続けるので検出できる。

各ループは、計測の前に**操作が実際に効いたこと**（バーの表示状態が反転した、ステータスバーの
アドレスやキャラクターセットが変わった、文書の内容が変わって元に戻った）を確認する。
効いていない操作を繰り返しても「リーク無し」と出るだけで、何も検証できないため。

### Debug ビルドの CRT リークレポート

Debug ビルドは、環境変数 `STIRHEX_LEAK_REPORT` にパスが設定されていると、
終了時の CRT リーク一覧をそのファイルへ書き出す（Release ビルドには何も含まれない）。
`tests/stability/test_crt_leak_report.py` がこれを使って、文書・バー・ダイアログ・編集を
一通り触ったセッションでリーク一覧が空であることを確認する。
Debug ビルドが無い場合はスキップされるため、実行前に用意しておく。

```powershell
cd porting
.\build.bat Debug x64
```

手動で確認する場合は次のようにする。

```powershell
$env:STIRHEX_LEAK_REPORT = "$env:TEMP\stirhex_leaks.txt"
.\StirHex\x64\Debug\bin\StirHex.exe
# アプリを終了してから内容を確認する。空ならリーク無し
Get-Content $env:STIRHEX_LEAK_REPORT
```

---

## 残留プロセスの検出とクリーンアップ

原版・移植版とも多重起動を禁止しているため、**プロセスが1つでも残留していると以降の起動が
すべて既存インスタンスへ委譲され、テストはメインウィンドウのタイムアウトで全滅する**
(Issue #113)。これを避けるため、以下の3段構えでガードしている。

| タイミング | 動作 |
| :--- | :--- |
| セッション開始 | 残留プロセスを検出したら、テストを1件も実行せずエラー終了する |
| テスト毎 | そのテスト中に増えたプロセスを終了させ、どのテストが残したかを警告として出力する |
| セッション終了 | セッション中に増えたプロセスを最終掃除する |

いずれもセッション開始時点で既に動いていたプロセスには触れない。手元で編集中のインスタンスを
未保存のまま終了させないための措置であり、セッション開始時の既定動作をエラー中断にしているのも
同じ理由による。

残留プロセスを自動で終了させて実行を継続したい場合は `--stale-processes=kill` を指定する。

```powershell
uv run pytest -m ported --stale-processes=kill
```

---

## ファイルフォーマット規約（文字コード・改行コード）

テストコードおよび関連設定ファイルの保守性を保ち、差分の混入を防ぐため、以下の規約を厳守してください。

- **文字コード**: `UTF-8`（BOM なし）
- **改行コード**: `LF`（`\n`）

※Windows 環境下で作業する場合も、改行コードが CRLF に変換されないようエディタや Git の設定（`.gitattributes` 等）にご留意ください。

---

## テストの作成方針

新しいゴールデン比較テストを作成する際は、`run_both_stirling` フィクスチャを利用して原版と移植版の双方に共通の操作関数 `action(drv, out_path)` を渡します。

```python
import pytest
from pathlib import Path
from drivers.stirling_driver import StirlingDriver


class TestGoldenCustom:
    """ゴールデン比較テストの例"""

    @pytest.mark.golden
    @pytest.mark.ported
    @pytest.mark.original
    def test_golden_custom_scenario(self, run_both_stirling):
        initial_data = b"HELLO_STIRLING_DATA"

        def action(drv: StirlingDriver, out_path: Path):
            # 1. ビューへのキー入力やコマンド送信
            drv.type_hex_chars("1234")
            # 2. 名前を付けて保存
            drv.save_as_via_dialog(out_path)

        orig_out, port_out = run_both_stirling(action, initial_data)

        # 3. 原版と移植版の出力がバイト単位で完全一致することを検証
        assert orig_out == port_out, "Ported output does not match Original Stirling output!"
```
