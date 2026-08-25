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
│   ├── settings_context.py     # テスト前後のレジストリ自動セットアップ・復元機構
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
    └── test_golden_selection_fill.py   # 範囲選択 / 範囲初期化(Fill) / 範囲保存
```

---

## カスタムマーク (Pytest Markers)

テスト対象や検証種別に応じて以下のカスタムマークが定義されています。

| マーク名 | 対象 | 説明 |
| :--- | :--- | :--- |
| **`ported`** | 移植版 | 移植版 Stirling を検証対象とするテスト（移植版単体テスト + ゴールデン比較テスト） |
| **`original`** | 原版 | 原版 Stirling (1.31) を検証対象とするテスト（原版単体テスト + ゴールデン比較テスト） |
| **`golden`** | 比較 | 原版と移植版の両方を起動し、バイナリ出力の完全一致を検証するゴールデン比較テスト |

---

## テストの実行方法

### 1. 全テストの実行

本ディレクトリに移動し、`uv run pytest` を実行する。初回実行時に仮想環境と依存パッケージが自動解決・インストールされる。

```powershell
cd porting/tests/e2e
uv run pytest
```

※実行結果のレポートは `reports/report.html` および `reports/report.xml` に自動出力されます。

### 2. マークを指定してのテスト実行

- **移植版のテストのみを実行する場合（推奨）**:
  ```powershell
  uv run pytest -m "ported"
  ```

- **原版のテストのみを実行する場合**:
  ```powershell
  uv run pytest -m "original"
  ```

- **ゴールデン比較テストのみを実行する場合**:
  ```powershell
  uv run pytest -m "golden"
  ```

- **移植版の単体テスト（Issue検証等）のみを実行する場合（ゴールデン比較を除く）**:
  ```powershell
  uv run pytest -m "ported and not golden"
  ```

### 3. 特定のテストファイル・ケースの実行

```powershell
# 特定ファイル
uv run pytest tests/test_golden_edit_hex.py -v

# 特定のテストケース名
uv run pytest -k "test_golden_overwrite_hex" -v
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
