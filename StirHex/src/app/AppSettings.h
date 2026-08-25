// CAppSettings — アプリ全体（全ドキュメント/ビュー共通）の動作環境設定モデル。
//   原は CMainFrame が保持し 環境設定ダイアログ(0x8050) が編集する（設定読込 FUN_0041f2a5）。
//   拡張子別の表示設定（CStirlingSettings）とは別レイヤで、theApp が単一インスタンスを保持する。
//   永続化は StirHex 専用レイアウト（HKCU\Software\StirHex\StirHex\Env）。原の値名/バイナリ構造は
//   再現しない。既定値は原の設定既定に沿う（未確定分は原挙動に沿う最善値。領域C確定時に精緻化）。
#pragma once

#include <windows.h>

#include <vector>

class CAppSettings {
public:
    // === 環境設定「編集１」ページ（IDD_SETTINGS_EDIT1 159） ===
    int  scrollLines        = 1;      // 垂直移動スクロール行数（1007）
    bool pasteOverwrite     = false;  // 上書モード時の貼り付けは上書き処理する（1011）
    bool searchNotFoundMsg  = false;  // 検索で見つからなかった場合にメッセージを表示する（1015）
    bool escMenu            = false;  // Escメニューを有効にする（1020）
    bool escDeselect        = false;  // Escキーで選択解除する（1025）
    bool deselectAfterCopy  = false;  // 選択データのコピー後に選択解除する（1066）
    bool clearUndoOnSave    = false;  // 保存時にアンドゥバッファをクリアする（1069）
    // UI未公開（移植独自。Issue #30）。Undo/Redo スタックが退避データとして保持する
    // 合計バイト数の上限（MB）。超過分は古いレコードから破棄する。0=無制限。
    // レジストリ Env\UndoMemoryLimitMB で変更する。
    int  undoMemoryLimitMB  = 256;
    bool subCaret           = true;   // サブキャレットを表示する（1070）
    bool highlightBoth      = true;   // データ選択時にコード・文字共に反転表示する（1088）
    bool realtimeBitImage   = true;   // 編集内容をリアルタイムでビットイメージに反映する（1102）
    int  twoStrokeTimeoutMs = 1500;   // ２ストロークキーのタイムアウト時間（1067 slider, ミリ秒。原=1.5秒）

    // === 環境設定「編集２」ページ（IDD_SETTINGS_EDIT2 197。原 DDX FUN_004659cf） ===
    int  fileHistoryCount   = 5;      // ファイル履歴数（1007）
    bool caretAutoRestore   = false;  // キャレット位置の自動復元（1011）
    bool curPosToStructAddr = true;   // 現在位置を構造体編集アドレスに自動設定（1015）
    bool newDocEditable     = true;   // 新規ドキュメントは常に編集可能として開く（1020）
    bool endAutoInsert      = true;   // 上書きモード時の末尾自動挿入（1066）
    bool dynamicMark        = false;  // ダイナミックマーク（1025）

    // === 環境設定「ファイル」ページ（IDD_SETTINGS_FILE 157。原 DDX 0x4103f4） ===
    bool     backupCreate         = true;   // バックアップファイルの作成（1030）
    int      backupGenerations    = 1;      // バックアップ世代数（1032）
    bool     backupFolderSpecify  = false;  // バックアップフォルダの指定（1035）
    CStringW backupFolder;                  // バックアップフォルダ（1036）
    int      exclusiveControl     = 0;      // ファイルの排他制御（1016/1017/1018。0=しない/1=書込禁止/2=読書禁止）
    bool     linkDirect           = false;  // リンクファイルは直接開く（1113）
    bool     defaultFolderSpecify = false;  // デフォルトフォルダの指定（1091）
    CStringW defaultFolder;                 // デフォルトフォルダ（1037）

    // === 環境設定「ウィンドウ」ページ（IDD_SETTINGS_WINDOW 182。原 DDX FUN_0046668d） ===
    int  winPlacement    = 0;      // メインウィンドウのサイズ・位置（1016..1019。0=指定しない/1=前回/2=最大化/3=指定）
    int  winLeft   = 0;            // 指定時の左（1078）
    int  winTop    = 0;            // 指定時の上（1079）
    int  winWidth  = 639;          // 指定時の幅（1080）
    int  winHeight = 479;          // 指定時の高さ（1083）
    bool docMaximize   = false;    // ドキュメントウィンドウを最大化で開く（1011）
    bool docFullPath   = false;    // ドキュメントのフルパス表示（1015）
    bool showToolbar   = true;     // ツールバーの表示（1020）
    bool showStatusbar = true;     // ステータスバーの表示（1025）
    // UI未公開。0=名前付きMutexで多重起動を禁止（既定）/ 1=複数プロセスを許可。
    // レジストリ Env\AllowMultipleInstances で変更する。
    bool allowMultipleInstances = false;
    bool bitImageDockable = false; // ビットイメージをドッキング可能にする（1066）
    int  structBarPos       = 2;   // 構造体編集バーの位置（1044..1046。0=下/1=上/2=フローティング）
    bool structBarNoDock    = true;  // ドッキング不能とする（1069）
    int  structBarStatusPos = 2;   // 構造体編集バーのステータス表示（1147/1148/1049。0=下/1=上/2=非表示）
    bool structItemRatioKeep = false; // 構造体アイテム幅の比率を保持（1070）

    // === 環境設定「ステータスバー」ページ（IDD_SETTINGS_STATUSBAR 181） ===
    //   構成ペインのコマンドID列（メッセージペインを除く。原の構成配列 CMainFrame 保持相当）。
    //   既定は移植済みステータスバー(indicators[]) と同一の並び。
    std::vector<UINT> statusItems = {
        0xE709,   // アドレス表示（16進）
        0xE708,   // 変更有表示
        0xE707,   // 編集禁止表示
        0xE704,   // 挿入／上書表示
        0xE70A,   // ドキュメントサイズ（10進）表示
        0xE70D,   // キャラクターセット表示
    };

    // === 環境設定「ツールバー」ページ（IDD_SETTINGS_TOOLBAR 178） ===
    //   構成ボタンの機能ID列。ID=原カタログの rawID（(カテゴリ<<8)|項目。原 DAT_004b6c90）。
    //   セパレータは 0xFFFF。既定は代表的なボタン構成（best-effort。領域C確定時に精緻化）。
    static constexpr UINT kToolbarSep = 0xFFFF;
    std::vector<UINT> toolbarItems = {
        0x0001,        // 新規作成
        0x0002,        // 開く...
        0x0004,        // 上書保存
        0xFFFF,        // セパレータ
        0x0300,        // 元に戻す
        0x0301,        // やり直し
        0xFFFF,        // セパレータ
        0x0302,        // 切り取り
        0x0303,        // コピー
        0x0304,        // 貼り付け
        0x0305,        // 編集禁止
        0x030E,        // 構造体編集
        0xFFFF,        // セパレータ
        0x0400,        // 検索...
        0x0404,        // 不一致検索...
        0x0408,        // 次検索
        0x0409,        // 前検索
        0x040A,        // 置換...
        0x040B,        // 比較...
        0x040C,        // BGREP...
        0xFFFF,        // セパレータ
        0x0104,        // データ先頭に移動
        0x0105,        // データ末尾に移動
        0x0110,        // 指定アドレスへ移動...
        0xFFFF,        // セパレータ
        0x0700,        // マーク登録／解除
        0x0701,        // 次のマーク位置
        0x0702,        // 前のマーク位置
        0xFFFF,        // セパレータ
        0x000A,        // 印刷...
        0x0709,        // ヘルプ
    };

    // === 環境設定「ユーザーメニュー」ページ（IDD_SETTINGS_USERMENU 183） ===
    //   15メニュー（メニュー1-10／２ストローク機能1-3／Escメニュー／コンテキストメニュー。原 FUN_0042aed1）。
    //   各メニュー=機能ID列（rawID=(カテゴリ<<8)|項目。セパレータは 0xFFFF）。
    //   既定はメニュー1-10/2ストローク/Escは空、コンテキストメニュー(idx14)のみ原の既定構成。
    static const int kUserMenuCount = 15;
    static const int kContextMenuIndex = 14;   // コンテキストメニュー（右クリック）
    static const int kEscMenuIndex     = 13;   // Escメニュー
    static const int kTwoStrokeBaseIndex = 10; // ２ストローク機能1-3 → idx10..12
    // 各アイテムは 32bit 値 = (アクセラレータ文字<<16) | rawID（原の userMenu 項目形式）。
    //   アクセラレータは2ストローク第2打鍵の照合・ポップアップのニーモニックに使う（0=無し）。
    //   区切りは kUserMenuSep（下位16bit=0xFFFF）。
    static constexpr UINT kUserMenuSep = 0xFFFF;
    static UINT  UmRaw(UINT item)              { return item & 0xFFFF; }
    static UINT  UmAccel(UINT item)            { return (item >> 16) & 0xFF; }
    static UINT  UmMake(UINT accel, UINT raw)  { return ((accel & 0xFF) << 16) | (raw & 0xFFFF); }
    std::vector<std::vector<UINT>> userMenus = BuildDefaultUserMenus();
    static std::vector<std::vector<UINT>> BuildDefaultUserMenus();   // 原の既定（idx14=切取/コピー/貼付/区切/保存/削除/初期化）

    // === 環境設定「キーアサイン」ページ（IDD_KEYASSIGN 139。原 FUN_0041972b の既定＋keymap） ===
    //   keymap[modstate*0x40 + keycode] = 機能rawID（0=なし/未割当）。modstate: 0=無/1=Shift/2=Ctrl/3=Ctrl+Shift。
    //   keycode 0..0x38（キー名=文字列5000+keycode）。既定は原 FUN_0041972b を忠実再現。
    static const int kKeymapSize = 256;   // 4 modstate × 64 keycode
    std::vector<UINT> keymap = BuildDefaultKeymap();
    static std::vector<UINT> BuildDefaultKeymap();   // 原 FUN_0041972b の既定割当
    void ResetKeymapToDefault() { keymap = BuildDefaultKeymap(); }

    // --- レジストリ永続化（セクション "Env"） ---
    void Load();
    void Save() const;
};
