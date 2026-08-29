# StirHex

**StirHex** は、バイナリエディタ Stirling（Version 1.31 / Copyright (C) 1998-1999 ＤＤＳ２）を
現行の開発環境向けに再実装した、非公式の移植版バイナリエディタです。
原作者が公開・サポートする製品ではありません。権利関係は [NOTICE.md](NOTICE.md) を参照してください。

## 概要

Stirling は使い勝手の良さから、開発終了から 20 年以上経過した現在でも利用者の多いバイナリエディタです。
しかし 32bit アプリケーションであり、現在では利用を推奨されない Win32 API を使用しているため、
そのまま使い続けることがセキュリティリスクとなり得ます。

StirHex は、元アプリケーションの操作感をできる限り忠実に保ちながら、次の点を改善しています。

- **x64（64bit）対応** — Win32（x86）ビルドも引き続き可能
- **非推奨 Win32 API のモダナイズ**
- **Unicode 対応** — 日本語パス・UTF-8 テキストの取り扱いを改善
- **設定ファイル方式** — 設定をレジストリではなく `StirHex.ini` へ保存。ポータブル運用が可能
- Visual C++ 2022 以降（MFC 静的リンク）でビルド可能な C++ ソース

## 主な機能

- 16 進 / 文字（CP932・UTF-8）の並列表示と編集
- 挿入・削除を含む編集と、メモリ上限を管理する Undo / Redo
- 検索・置換（16 進 / 文字）、不一致検索、複数ファイルを対象とした BGREP 検索
- ファイル比較・相違箇所一覧・シンクロスクロール
- アドレスマーク（静的・動的）、マーク一覧、インポート／エクスポート、自動保存・自動復元
- 構造体定義（`struct.def`）に基づく構造体表示・編集バー
- ビットイメージ表示、キャラクターセット／バイトオーダー切り替え、強調表示
- 印刷・印刷プレビュー（範囲指定印刷対応）
- 環境設定（表示・色・キー割り当て・外部ツール連携 等）
- HTML ヘルプ同梱

## 動作環境

- Windows 10 / 11（x64 / x86）

## 設定ファイル

設定は INI 形式のファイル（UTF-8 / BOM なし）へ保存します。使用するファイルは次の順で決まります。

1. コマンドライン `/ini:<パス>`（`-ini:` も可）
2. `StirHex.exe` と同じフォルダーの `StirHex.ini`（**存在する場合のみ**＝ポータブルモード）
3. `%APPDATA%\StirHex\StirHex.ini`（既定）

USB メモリなどで持ち歩く場合は、`StirHex.exe` と同じフォルダーへ空の `StirHex.ini` を置いてください。

## ビルド

### 必要環境

- Visual Studio 2017 以降
  - **「C++ による MFC（静的リンク）」コンポーネントが必要**です
  - `PlatformToolset` は `$(DefaultPlatformToolset)` のため、インストール済みの最新ツールセット
    （VS2022 なら v143）で自動的にビルドされます

### バッチスクリプトでビルド

リポジトリのルートで実行します。MSBuild は `vswhere` により自動検出されます。

```bat
rem Debug ビルド（既定 x64）
build_debug.bat

rem Release ビルド（既定 x64）
build_release.bat

rem 構成・プラットフォームを直接指定
build.bat Debug x64
build.bat Release x64
build.bat Release Win32
```

ビルド成果物（実行ファイル）の出力先は次のとおりです。

```
StirHex/x64/Debug/bin/StirHex.exe     （x64 Debug）
StirHex/x64/Release/bin/StirHex.exe   （x64 Release）
StirHex/Debug/bin/StirHex.exe         （Win32 Debug）
StirHex/Release/bin/StirHex.exe       （Win32 Release）
```

ヘルプ（`StirHex/help/`）は実行ファイルと同じ場所の `help/` へコピーされます。

### Visual Studio で開く

`StirHex.sln` を開き、構成 `Debug` / `Release`、プラットフォーム `x64`（既定）/ `Win32` を
選択してビルドしてください。

## ディレクトリ構成

```
.
├── StirHex.sln             … ソリューション
├── build.bat               … 共通ビルドスクリプト（vswhere で MSBuild 自動検出）
├── build_debug.bat         … Debug ビルド
├── build_release.bat       … Release ビルド
├── tools/                  … リソース・ヘルプ生成等の補助スクリプト
├── tests/                  … コア機能テスト・e2e 自動テスト
└── StirHex/                … プロジェクト本体
    ├── StirHex.vcxproj
    ├── StirHex.rc / resource.h / res/
    ├── help/               … HTML ヘルプ（src/ は Markdown 原稿）
    └── src/
        ├── app/            … アプリケーション・設定（CWinApp, AppSettings）
        ├── core/           … コアデータモデル（BlockList, BlockCursor, StructDef 等）
        ├── doc/            … ドキュメント（Undo/Redo, マーク 等）
        ├── view/           … ビュー（16 進表示・入力）
        ├── frame/          … フレーム／ツールバー／各種バー
        └── dialog/         … 各種ダイアログ（検索・置換・比較・環境設定 等）
```

## テスト

- `tests/build_core_test.ps1` — コア機能テストのビルド・実行
- `tests/e2e/` — pytest（uv 管理）による GUI e2e 自動テスト。詳細は `tests/e2e/README.md` を参照

## 補足

- ソースファイルは **UTF-8（BOM なし）** です。プロジェクトは `/utf-8` を指定してコンパイルします。
- `.bat` / `.ps1` は ASCII のみで記述しています（`cmd.exe` の非 ASCII バイト誤解釈を避けるため）。

## ライセンス・免責

[NOTICE.md](NOTICE.md) を参照してください。本ソフトウェアは非営利かつ無保証で公開しています。
