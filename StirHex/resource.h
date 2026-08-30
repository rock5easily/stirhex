//{{NO_DEPENDENCIES}}
// Stirling porting resource IDs.
// IMPORTANT: keep this file ASCII-only. rc.exe includes it before the
// #pragma code_page(65001) directive, so non-ASCII bytes here would be
// misinterpreted (CP932) and can swallow #define lines.
//
// The resource script (StirHex.rc / res\StirHex.rc) keeps the original
// numeric IDs for faithfulness; these symbols map C++ code (message maps,
// DoModal) onto those same numeric values. Standard MFC command IDs
// (0xE1xx / 57600+) come from afxres.h and are NOT redefined here.
#ifndef STIRLING_RESOURCE_H
#define STIRLING_RESOURCE_H

// ---------------------------------------------------------------------------
// Menus / icons / accelerators (LoadFrame uses IDR_MAINFRAME for menu+icon+title)
// ---------------------------------------------------------------------------
#define IDR_MAINFRAME       128     // no-document menu + app icon + title string
#define IDR_STIRLINGTYPE    129     // document-open menu + doc-template string + doc icon
#define IDR_OUTPUT_POPUP    177     // Output pane context popup
#define IDR_CHARSET_POPUP   179     // Character-set context popup
#define IDR_BITIMAGE_POPUP  188     // Bit-image context popup
#define IDR_STRUCT_POPUP    189     // Struct-edit context popup
#define IDR_BYTEORDER_POPUP 191     // Byte-order context popup
#define IDR_ESCMENU_ACCEL   30997   // Esc-menu navigation accelerators

// ---------------------------------------------------------------------------
// Dialogs (numeric IDs as embedded in the original executable)
// ---------------------------------------------------------------------------
#define IDD_ABOUTBOX            100
#define IDD_COLOR_FONT          107
#define IDD_JUMP                137
#define IDD_KEYASSIGN           139
#define IDD_MARK_LIST           140
#define IDD_WINDOW_LIST         153
#define IDD_COMBO_LIST          154
#define IDD_SETTINGS_FILE       157
#define IDD_SETTINGS_DISPLAY    158
#define IDD_SETTINGS_EDIT1      159
#define IDD_FIND_MISMATCH       160
#define IDD_FIND                161
#define IDD_REPLACE             162
#define IDD_REPLACE_CONFIRM     163
#define IDD_REPLACE_STATUS      164
#define IDD_FILL_RANGE          165
#define IDD_COMPARE             166
#define IDD_DIFF_LIST           167
#define IDD_RUN                 168
#define IDD_BGREP               172
#define IDD_BGREP_STATUS        175
#define IDD_FOLDER_SELECT       176
#define IDD_SETTINGS_TOOLBAR    178
#define IDD_SETTINGS_STATUSBAR  181
#define IDD_SETTINGS_WINDOW     182
#define IDD_SETTINGS_USERMENU   183
#define IDD_ACCEL_INPUT         184
#define IDD_EXT_LIST            185
#define IDD_EXT_INPUT           186
#define IDD_HIGHLIGHT_CODE      187
#define IDD_MARK_ADDRESS        190
#define IDD_STRUCT_BAR          192
#define IDD_SYNC_SCROLL         193
#define IDD_STATUS_STRUCT       195
#define IDD_TOP_ADDRESS         196
#define IDD_SETTINGS_EDIT2      197
#define IDD_SAVE_DUMP           198
// 構造体編集バー。原の細いストリップ(192)へ編集可能なツリーグリッドを組み合わせた
// 複合テンプレート（構造体選択コンボ＋再読込＋フィールド一覧）。新規 ID。
#define IDD_STRUCT_EDIT_BAR     250
#define IDC_STRUCT_RELOAD       1200    // 再読込ボタン（Struct.def 再パース）
#define IDC_STRUCT_COMBO        1201    // 構造体選択コンボ
#define IDC_STRUCT_LIST         1202    // フィールド一覧（SysListView32, レポート/仮想）
#define IDC_STRUCT_ADDR         1203    // 基準アドレス表示（静的, "00000000"）
#define IDC_STRUCT_EDITBOX      1204    // 値のインプレース編集ボックス（動的生成）
#define IDC_STRUCT_STATUS_EDIT  1205    // 構造体バー独自ステータス: 編集禁止
#define IDC_STRUCT_STATUS_CS    1206    // 構造体バー独自ステータス: 文字セット
#define IDC_STRUCT_STATUS_ORDER 1207    // 構造体バー独自ステータス: バイトオーダー
#define IDC_STRUCT_RESIZE_GRIP  1208    // 上下ドック時の高さ変更境界
// ナビゲーション（原 << < 移動 > >>。base=構造体先頭オフセット this+0x4a0 相当）
#define IDC_STRUCT_PREVREC      1210    // "<<": base -= 構造体サイズ
#define IDC_STRUCT_PREVBYTE     1211    // "<" : base -= 1
#define IDC_STRUCT_GOTO         1212    // "移動": 先頭アドレス指定ダイアログ
#define IDC_STRUCT_NEXTBYTE     1213    // ">" : base += 1
#define IDC_STRUCT_NEXTREC      1214    // ">>": base += 構造体サイズ

// 構造体編集「先頭アドレスの指定」（IDD_TOP_ADDRESS=196、原 FUN_00460680）。
#define IDC_TOPADDR_MODE_ADDRESS 1016   // アドレス指定
#define IDC_TOPADDR_MODE_MARK    1017   // マーク登録位置
#define IDC_TOPADDR_EDIT         1007   // アドレス入力
#define IDC_TOPADDR_MARK_LIST    1021   // 登録マーク一覧（オーナードロー）
#define IDC_TOPADDR_HINT_RANGE   1029   // 有効アドレス
#define IDC_TOPADDR_HINT_CURRENT 1019   // 現在アドレス
#define IDC_TOPADDR_BASE_DEC     1020   // 10進
#define IDC_TOPADDR_BASE_HEX     1022   // 16進
#define IDC_TOPADDR_ADDR_LABEL   1124   // 「アドレス」静的
#define IDC_TOPADDR_BASE_GROUP   1125   // 「アドレスベース」グループ
// アウトプットペイン（原 CMainFrame+0x1d8 のドッキング出力バー内 CListBox）。
//   原はコントロールバー内に直接 CListBox を持つ。移植では CDialogBar 用の
//   テンプレート（リストボックス1個）を新規に用意し、下端へドッキングする。
#define IDD_OUTPUT_BAR          251     // 出力バーのダイアログテンプレート（新規）
#define IDC_OUTPUT_LIST         1300    // 結果一覧リストボックス（LBS_NOTIFY, item data=ヒット索引）
// ビットイメージ・ペイン（原 CMainFrame+0x2b8 の左端ドッキング窓）。128px幅の8bppパレット
//   画像で文書バイトを可視化する。CDialogBar 用に空テンプレートを新規に用意し、描画用の
//   子ウィンドウ（CBitImageWnd）を実行時に生成してクライアント全域へ広げる。
#define IDD_BITIMAGE_BAR        252     // ビットイメージ・バーのダイアログテンプレート（新規・空）
#define IDD_FILE_CHANGED        199
#define IDD_PRINT_RANGE_BAR     200
#define IDD_PRINT_RANGE         201
#define IDD_SELECT_RANGE        202

// ---------------------------------------------------------------------------
// Stirling-specific command IDs (0x8000 range). Standard MFC commands
// (File/Edit/Window/App = 0xE1xx) are provided by afxres.h.
// ---------------------------------------------------------------------------
// Navigation（カーソル移動。原 cat1。keymap 経由でコマンドとして起動される）
#define ID_CURSOR_LEFT          32778   // 0x800a カーソル左
#define ID_CURSOR_RIGHT         32779   // 0x800b カーソル右
#define ID_CURSOR_UP            32780   // 0x800c カーソル上
#define ID_CURSOR_DOWN          32781   // 0x800d カーソル下
#define ID_GOTO_DATA_TOP        32782   // 0x800e データ先頭に移動
#define ID_GOTO_DATA_END        32783   // 0x800f データ末尾に移動
#define ID_CURSOR_LINE_HOME     32784   // 0x8010 行左端に移動
#define ID_CURSOR_LINE_END      32785   // 0x8011 行右端に移動
#define ID_CURSOR_FAST_UP       32786   // 0x8012 高速上移動（原 ±2行）
#define ID_CURSOR_FAST_DOWN     32787   // 0x8013 高速下移動（原 ±2行）
#define ID_PAGE_UP              32788   // 0x8014 ページアップ
#define ID_PAGE_DOWN            32789   // 0x8015 ページダウン
#define ID_HALF_PAGE_UP         32790   // 0x8016 半ページアップ（原 ±可視行/2）
#define ID_HALF_PAGE_DOWN       32791   // 0x8017 半ページダウン（原 ±可視行/2）
#define ID_LINE_UP              32792   // 0x8018 ラインアップ（1行スクロール）
#define ID_LINE_DOWN            32793   // 0x8019 ラインダウン（1行スクロール）
#define ID_JUMP                 32794   // 0x801a 指定アドレスへ移動
#define ID_GOTO_LAST_MODIFIED   32795   // 0x801b 最終変更箇所へ移動
// Selection（選択拡張移動。原 cat2。カーソル移動の extend 版）
#define ID_SELECT_MODE          32796   // 0x801c 選択モード開始／終了（DOS式）
#define ID_SELECT_LEFT          32797   // 0x801d 選択左
#define ID_SELECT_RIGHT         32798   // 0x801e 選択右
#define ID_SELECT_UP            32799   // 0x801f 選択上
#define ID_SELECT_DOWN          32800   // 0x8020 選択下
#define ID_SELECT_DATA_TOP      32801   // 0x8021 データ先頭まで選択
#define ID_SELECT_DATA_END      32802   // 0x8022 データ末尾まで選択
#define ID_SELECT_LINE_HOME     32803   // 0x8023 行左端まで選択
#define ID_SELECT_LINE_END      32804   // 0x8024 行右端まで選択
// Edit
#define ID_TOGGLE_READONLY      32805   // 0x8025
#define ID_TOGGLE_INSERT        32806   // 0x8026 上書／挿入切替
#define ID_TOGGLE_PANE          32807   // 0x8027 数値入力／文字入力切替
#define ID_DELETE_BYTE          32808   // 0x8028 １バイト削除
#define ID_DELETE_BYTE_BACK     32809   // 0x8029 直前の１バイト削除
#define ID_DELETE_SELECTION     32810   // 0x802a
#define ID_FILL_SELECTION       32811   // 0x802b
#define ID_SAVE_SELECTION       32812   // 0x802c（選択範囲を生バイナリでファイルへ。原は右クリックメニュー）
#define ID_REVERT_FILE          32813   // 0x802d
#define ID_STRUCT_EDIT          32814   // 0x802e (edit menu variant)
// Search / move
#define ID_FIND_MISMATCH        32818   // 0x8032
#define ID_FIND_PREV            32822   // 0x8036
#define ID_FIND_NEXT            32823   // 0x8037
#define ID_COMPARE              32824   // 0x8038
#define ID_BGREP                32825   // 0x8039
// Marks
#define ID_MARK_TOGGLE          32842   // 0x804a
#define ID_MARK_NEXT            32843   // 0x804b
#define ID_MARK_PREV            32844   // 0x804c
#define ID_MARK_CLEAR_ALL       32845   // 0x804d
#define ID_MARK_LIST            32846   // 0x804e
#define ID_MARK2_TOGGLE         32866   // 0x8062（マーク2登録／解除。原の既定メニューには無い）
#define ID_MARK3_TOGGLE         32867   // 0x8063（マーク3登録／解除。原の既定メニューには無い）
// User menus / two-stroke popups（原 cat5。呼出すと対応 userMenus[0..12] をポップアップ表示）
#define ID_USERMENU_1           32826   // 0x803a ユーザーメニュー１ → userMenus[0]
#define ID_USERMENU_10          32835   // 0x8043 ユーザーメニュー１０ → userMenus[9]
#define ID_TWOSTROKE_1          32836   // 0x8044 ２ストローク機能１ → userMenus[10]
#define ID_TWOSTROKE_3          32838   // 0x8046 ２ストローク機能３ → userMenus[12]
// Window
#define ID_ADJUST_WINDOW_SIZE   32841   // 0x8049
// Settings
#define ID_SETTINGS_ENV         32848   // 0x8050
#define ID_SETTINGS_EXT         32849   // 0x8051
#define ID_CHARSET_ASCII        32851   // 0x8053
#define ID_CHARSET_SJIS         32852   // 0x8054
#define ID_CHARSET_EUC          32853   // 0x8055
#define ID_CHARSET_UNICODE      32854   // 0x8056
#define ID_HELP_TOPICS          32855   // 0x8057
#define ID_CHARSET_EBCDIC       32856   // 0x8058
#define ID_CHARSET_EBCIDK       32857   // 0x8059
#define ID_BYTEORDER_LITTLE     32859   // 0x805b
#define ID_BYTEORDER_BIG        32860   // 0x805c
#define ID_SELECT_PAGE_UP       32861   // 0x805d 前１ページ分選択
#define ID_SELECT_PAGE_DOWN     32862   // 0x805e 次１ページ分選択
#define ID_SYNC_SCROLL          32863   // 0x805f
#define ID_SAVE_DUMP            32864   // 0x8060
#define ID_STRUCT_CARET         32865   // 0x8061 キャレット位置を構造体編集の先頭アドレスにする
#define ID_RUN_APP              32847   // 0x804f 名前を指定して実行（原の既定メニューには無い）
#define ID_PRINT_RANGE          32868   // 0x8064
#define ID_SELECT_RANGE         32869   // 0x8065 範囲を指定して選択
// Output / bit-image / struct (0x80E8 range)
#define ID_OUTPUT_PANE          33000   // 0x80e8
#define ID_OUTPUT_CLEAR         33001   // 0x80e9
#define ID_TAG_JUMP             33002   // 0x80ea
#define ID_BITIMAGE             33003   // 0x80eb
#define ID_BITIMAGE_LATEST      33004   // 0x80ec
#define ID_STRUCT_EXEC          33005   // 0x80ed
#define ID_STRUCT_RADIX_ONE_S   33007   // 0x80ef
#define ID_STRUCT_RADIX_ONE_U   33008   // 0x80f0
#define ID_STRUCT_RADIX_ONE_H   33009   // 0x80f1
#define ID_STRUCT_RADIX_ONE_DEF 33006   // 0x80ee
#define ID_STRUCT_RADIX_ALL_S   33011   // 0x80f3
#define ID_STRUCT_RADIX_ALL_U   33012   // 0x80f4
#define ID_STRUCT_RADIX_ALL_H   33013   // 0x80f5
#define ID_STRUCT_RADIX_ALL_DEF 33010   // 0x80f2
#define ID_STRUCT_EDIT_TOGGLE   33014   // 0x80f6
#define ID_SEARCH_RESULT_COPY   33015   // 0x80f7
#define ID_EDIT_PASTE_HEX       33016   // 0x80f8 クリップボードの16進テキストを貼り付け
#define ID_CHARSET_UTF8        33017   // 0x80f9 キャラクターセット: UTF-8（原には無い）
#define ID_MARK_EXPORT         33018   // 0x80fa マークの書き出し（原には無い。Issue #99）
#define ID_MARK_IMPORT         33019   // 0x80fb マークの読み込み（原には無い。Issue #99）

// ---------------------------------------------------------------------------
// Status-bar indicator panes (original custom IDs 0xE7xx). The sizing
// template strings live in the RC STRINGTABLE (59140/59143/59144/59145/
// 59146/59149). Each pane is refreshed by an ON_UPDATE_COMMAND_UI handler
// in CStirlingView (idle-time), matching the original layout:
//   { SEPARATOR, ADDRESS, MODIFIED, EDITLOCK, MODE, SIZE, CHARSET }.
// ---------------------------------------------------------------------------
#define ID_INDICATOR_MODE       0xE704   // 「上書」/「挿入」（原 FUN_00424d49）
#define ID_INDICATOR_EDITLOCK   0xE707   // 「編禁」（編集禁止時）
#define ID_INDICATOR_MODIFIED   0xE708   // 「更新」（変更あり時）
#define ID_INDICATOR_ADDRESS    0xE709   // 「0x%08X」キャレット位置16進（原 FUN_004249fa）
#define ID_INDICATOR_SIZE       0xE70A   // 「%d Bytes」総サイズ10進（原 FUN_004248be）
#define ID_INDICATOR_WORD_HEX   0xE70B   // 「W : 0x%04X」WORD値16進（原 FUN_00424xxx）
#define ID_INDICATOR_DWORD_HEX  0xE70C   // 「DW : 0x%08X」DWORD値16進
#define ID_INDICATOR_CHARSET    0xE70D   // 文字セット名（原 FUN_00424bd0）
#define ID_INDICATOR_ADDR_DEC   0xE70E   // キャレット位置10進（原 FUN_00424a94）
#define ID_INDICATOR_BYTE_DEC   0xE70F   // 「B : %d」BYTE値10進（原 FUN_00424399）
#define ID_INDICATOR_WORD_DEC   0xE710   // 「W : %d」WORD値10進
#define ID_INDICATOR_DWORD_DEC  0xE711   // 「DW : %u」DWORD値10進
#define ID_INDICATOR_SIZE_HEX   0xE712   // 「0x%08X Bytes」総サイズ16進（原 FUN_00424960）
#define ID_INDICATOR_BYTE_HEX   0xE713   // 「B : 0x%02X」BYTE値16進
#define ID_INDICATOR_FLOAT      0xE714   // 「f : %g」float値
#define ID_INDICATOR_DOUBLE     0xE715   // 「d : %g」double値
#define ID_INDICATOR_BYTEORDER  0xE716   // バイトオーダー名（原 FUN_00424c8a。6050/6051）

// --- 検索/置換ダイアログ コントロール（IDD_FIND 161 / IDD_REPLACE 162） ---
#define IDC_FIND_TYPE_HEX       1016    // 検索データ種別: 16進
#define IDC_FIND_TYPE_TEXT      1017    // 検索データ種別: 文字列
#define IDC_FIND_RANGE_CURSOR   1018    // 検索範囲: カーソル位置から
#define IDC_FIND_RANGE_ALL      1019    // 検索範囲: データ全体
#define IDC_FIND_RANGE_SEL      1044    // 検索範囲: 選択範囲内
#define IDC_FIND_COMBO          1026    // 検索データ入力コンボ
#define IDC_FIND_PREV           1041    // 前検索ボタン
#define IDC_FIND_NEXT           1042    // 次検索ボタン

// --- 不一致検索ダイアログ（IDD_FIND_MISMATCH 160, モーダル） ---
#define IDC_MISMATCH_BYTE       1007    // 不一致パターン（単一16進バイト）
#define IDC_MISMATCH_RANGE_CURSOR 1016  // 検索範囲: カーソル位置から
#define IDC_MISMATCH_RANGE_ALL  1017    // 検索範囲: データ全体
#define IDC_MISMATCH_RANGE_SEL  1018    // 検索範囲: 選択範囲内
#define IDC_MISMATCH_PREV       1041    // 前検索ボタン
#define IDC_MISMATCH_NEXT       1042    // 次検索ボタン

// --- 置換ダイアログ（IDD_REPLACE 162） ---
#define IDC_REPL_SEARCH_HEX     1016    // 検索データ種別: 16進
#define IDC_REPL_SEARCH_TEXT    1017    // 検索データ種別: 文字列
#define IDC_REPL_REPLACE_HEX    1018    // 置換データ種別: 16進
#define IDC_REPL_REPLACE_TEXT   1019    // 置換データ種別: 文字列
#define IDC_REPL_RANGE_CURSOR   1044    // 置換範囲: カーソル位置から
#define IDC_REPL_RANGE_ALL      1045    // 置換範囲: データ全体
#define IDC_REPL_RANGE_SEL      1046    // 置換範囲: 選択範囲内
#define IDC_REPL_SEARCH_COMBO   1026    // 検索データ入力コンボ
#define IDC_REPL_REPLACE_COMBO  1027    // 置換データ入力コンボ
#define IDC_REPL_PREV           1041    // 前検索ボタン
#define IDC_REPL_NEXT           1042    // 次検索ボタン
#define IDC_REPL_ALL            1038    // 一括置換ボタン

// --- 置換確認ダイアログ（IDD_REPLACE_CONFIRM 163） ---
#define IDC_RCONF_EXEC          1040    // 実行
#define IDC_RCONF_SKIP          1039    // スキップ
#define IDC_RCONF_ALL           1038    // 一括置換

// --- マーク一覧ダイアログ（IDD_MARK_LIST 140） ---
#define IDC_MARKLIST_LIST       1021    // マーク一覧リストボックス（オーナードロー）
#define IDC_MARKLIST_REMOVE     1000    // 解除
#define IDC_MARKLIST_CLEARALL   1001    // 全解除
#define IDC_MARKLIST_EDIT       1002    // 編集...
// 実行=IDOK(1) / 閉じる=IDCANCEL(2)

// --- マークアドレス指定ダイアログ（IDD_MARK_ADDRESS 190） ---
#define IDC_MARKADDR_HINT       1029    // 有効アドレス範囲ヒント（静的）
#define IDC_MARKADDR_EDIT       1007    // アドレス入力
#define IDC_MARKADDR_COLOR      1026    // マーク色コンボ（オーナードロー3色）
#define IDC_MARKADDR_BASE_DEC   1016    // アドレスベース: 10進
#define IDC_MARKADDR_BASE_HEX   1017    // アドレスベース: 16進

// --- 指定アドレスへ移動ダイアログ（IDD_JUMP 137） ---
#define IDC_JUMP_HINT_RANGE     1029    // "有効アドレス : 0 ～ N"（静的）
#define IDC_JUMP_HINT_CURRENT   1019    // "現在アドレス : X"（静的）
#define IDC_JUMP_EDIT           1007    // アドレス入力（先頭 +/- でカーソル相対移動）
#define IDC_JUMP_BASE_DEC       1016    // アドレスベース: 10進
#define IDC_JUMP_BASE_HEX       1017    // アドレスベース: 16進

// --- 指定範囲の初期化ダイアログ（IDD_FILL_RANGE 165） ---
#define IDC_FILL_RANGE          1046    // "指定範囲 : %08X ～ %08X"（静的）
#define IDC_FILL_EDIT           1007    // 初期化データ入力（2桁16進, 0～FF）

// --- ダンプイメージの保存ダイアログ（IDD_SAVE_DUMP 198） ---
#define IDC_SAVEDUMP_FILE       1126    // 出力ファイル名 edit
#define IDC_SAVEDUMP_BROWSE     1000    // "..." 参照ボタン
#define IDC_SAVEDUMP_WHOLE      1016    // 出力範囲: データ全体（ラジオ基点）
#define IDC_SAVEDUMP_RANGE      1017    // 出力範囲: 範囲指定

// --- 外部変更通知ダイアログ（IDD_FILE_CHANGED 199, モーダル。ビュー活性化時に表示） ---
#define IDC_FILECHG_IGNORE      1016    // 変更を無視して作業を続ける（ラジオ基点）
#define IDC_FILECHG_RELOAD      1017    // 現在編集中の内容を破棄して再読み込みする（既定）
#define IDC_FILECHG_SAVEAS      1018    // 現在編集中の内容を別ファイルに保存する
#define IDC_FILECHG_NAME_LABEL  1027    // "ファイル名(&F)" 静的（別名保存時のみ有効）
#define IDC_FILECHG_NAME        1007    // 保存先ファイル名 edit（別名保存時のみ有効）
#define IDC_FILECHG_BROWSE      1000    // "..." 参照ボタン（別名保存時のみ有効）
#define IDC_FILECHG_COMPARE     1011    // 変更されたファイルをオープンして比較実行（別名保存時のみ有効）
#define IDC_FILECHG_ICON        1129    // 警告アイコン
// OK=IDOK(1)

// --- 名前を指定して実行ダイアログ（IDD_RUN 168, モーダル。コマンド ID_RUN_APP） ---
#define IDC_RUN_COMBO           1026    // コマンドライン（履歴コンボ。編集可）
#define IDC_RUN_BROWSE          1000    // "参照(&B)..." 実行ファイル選択
// OK=IDOK(1) / キャンセル=IDCANCEL(2)

// --- BGREP ダイアログ（IDD_BGREP 172, モーダル） ---
#define IDC_BGREP_DATA_COMBO    1026    // 検索データ（16進/文字列。履歴コンボ）
#define IDC_BGREP_TYPE_HEX      1016    // データ種別: 16進
#define IDC_BGREP_TYPE_TEXT     1017    // データ種別: 文字列
#define IDC_BGREP_CHARSET       1097    // キャラクターセット（文字列種別時）
#define IDC_BGREP_FILE_COMBO    1027    // 検索するファイルの種類（"*.txt;*.exe"）
#define IDC_BGREP_FOLDER        1007    // 検索対象フォルダ edit
#define IDC_BGREP_BROWSE        1000    // "..." フォルダ選択ボタン
#define IDC_BGREP_RECURSE       1011    // サブフォルダも検索する
#define IDC_BGREP_SKIPSYS       1015    // システム属性ファイルは検索しない
// 実行=IDOK(1) / キャンセル=IDCANCEL(2) / ヘルプ=9

// --- BGREP 検索状況ダイアログ（IDD_BGREP_STATUS 175, モードレス） ---
#define IDC_BGREP_STAT_FILE     1060    // 検索中ファイル名（静的）
#define IDC_BGREP_STAT_COUNT    1096    // 検出データ件数（静的）
// キャンセル=IDCANCEL(2)
// メッセージは既存の IDS_SEARCH_EMPTY / IDS_INVALID_DATA を再利用する。

// --- データ比較ダイアログ（IDD_COMPARE 166） ---
#define IDC_COMPARE_LIST        1021    // 比較対象文書リスト（フレームタイトル, ItemData=ビュー）

// --- 相違箇所一覧ダイアログ（IDD_DIFF_LIST 167, モードレス） ---
#define IDC_DIFFLIST_HILITE     1011    // "比較結果の強調表示" チェック（両ビューの強調ON/OFF）
#define IDC_DIFFLIST_SYNC       1015    // "シンクロスクロール" チェック
#define IDC_DIFFLIST_LIST       1058    // 相違一覧（CListCtrl レポート3カラム）
#define IDC_DIFFLIST_SWITCH     1000    // "切替" 活性ビュー反転
// ジャンプ=IDOK(1) / 閉じる=IDCANCEL(2)

// --- シンクロスクロール手動登録ダイアログ（IDD_SYNC_SCROLL 193, 0x805f） ---
#define IDC_SYNC_CANDIDATE      1021    // "シンクロしないウィンドウ" 候補リスト（ItemData=エントリ索引）
#define IDC_SYNC_REGISTERED     1024    // "シンクロするウィンドウ" 登録リスト（ItemData=エントリ索引）
#define IDC_SYNC_ADD            1071    // "↓追加" 候補→登録
#define IDC_SYNC_REMOVE         1115    // "↑解除" 登録→候補
#define IDC_SYNC_RESET          1116    // "全解除" 登録を全て候補へ戻す
// OK=IDOK(1) / キャンセル=IDCANCEL(2) / ヘルプ=IDHELP(9)

// --- 範囲バー（子ダイアログ IDD_PRINT_RANGE_BAR 200。ダンプ/印刷の範囲入力で共用） ---
#define IDC_RANGEBAR_USESEL     1127    // "選択範囲" チェック（ON で選択範囲を使用し欄を無効化）
#define IDC_RANGEBAR_HINT       1029    // "有効アドレス : 0 ～ N"（静的）
#define IDC_RANGEBAR_START      1013    // 開始アドレス edit
#define IDC_RANGEBAR_SEP        1020    // "～"（静的）
#define IDC_RANGEBAR_END        1014    // 終了アドレス edit
#define IDC_RANGEBAR_BASE_GROUP 1128    // "アドレスベース" グループ
#define IDC_RANGEBAR_BASE_DEC   1016    // アドレスベース: 10進
#define IDC_RANGEBAR_BASE_HEX   1017    // アドレスベース: 16進

// --- 範囲を指定して印刷ダイアログ（IDD_PRINT_RANGE 201。範囲バー200を埋め込む） ---
#define IDC_PRINTRANGE_PREVIEW  1011    // "プレビュー経由で印刷" チェック（原 dlg+0x5c）

// --- 範囲を指定して選択ダイアログ（IDD_SELECT_RANGE 202。範囲バー200を埋め込む） ---
#define IDC_SELRANGE_ANCHOR     1130    // 範囲バーの配置基準（原テンプレートのSTATICプレースホルダ）

// --- 環境設定「表示状態」ページ（IDD_SETTINGS_DISPLAY 158） ---
#define IDC_DISP_LINESIZE       1007    // 1行当たりの表示バイト数（edit）
#define IDC_DISP_LINESIZE_SPIN  1009    // スピン（1007 のバディ）
#define IDC_DISP_ADDR_HSCROLL   1011    // アドレスも横スクロールの対象とする
#define IDC_DISP_OPEN_READONLY  1015    // オープン時に編集禁止とする
#define IDC_DISP_OPEN_INSERT    1020    // オープン時に挿入モードとする
#define IDC_DISP_OPEN_CHARMODE  1025    // オープン時に文字入力モードとする
#define IDC_DISP_CS_ASCII       1900    // キャラクターセット: ASCII
#define IDC_DISP_CS_SJIS        1901    // キャラクターセット: シフトJIS
#define IDC_DISP_CS_EUC         1902    // キャラクターセット: EUC
#define IDC_DISP_CS_UNICODE     1903    // キャラクターセット: Unicode
#define IDC_DISP_CS_EBCDIC      1904    // キャラクターセット: EBCDIC
#define IDC_DISP_CS_EBCIDK      1906    // キャラクターセット: EBCIDK（1905 は欠番）
#define IDC_DISP_CS_UTF8        1907    // キャラクターセット: UTF-8（移植で追加。Issue #98）
#define IDC_DISP_RADIX_DEC      1910    // アドレス表示: 10進
#define IDC_DISP_RADIX_HEX      1911    // アドレス表示: 16進
#define IDC_DISP_BO_LITTLE      1920    // バイトオーダー: リトルエンディアン
#define IDC_DISP_BO_BIG         1921    // バイトオーダー: ビッグエンディアン

// --- 拡張子別設定一覧（IDD_EXT_LIST 185） ---
#define IDC_EXTLIST_LIST        1021    // 拡張子レコード一覧リストボックス
#define IDC_EXTLIST_SETTINGS    1000    // 設定...（選択レコードを編集）
#define IDC_EXTLIST_ADD         1001    // 追加...
#define IDC_EXTLIST_DELETE      1002    // 削除
// 閉じる=IDOK(1) / ヘルプ=IDHELP(9)

// --- 拡張子別設定 レコード編集ダイアログ（新規 IDD_EXT_RECORD 253。拡張子/コメント＋2ページ） ---
#define IDD_EXT_RECORD          253
#define IDC_EXTREC_EXT          1089    // 拡張子 edit
#define IDC_EXTREC_COMMENT      1090    // コメント edit
#define IDC_EXTREC_SHEET        1500    // 埋め込みプロパティシート配置用プレースホルダ

// --- 拡張子別設定「色・フォント」ページ（IDD_COLOR_FONT 107） ---
#define IDC_CF_CATEGORY         1026    // データ種別コンボ（8カテゴリ）
#define IDC_CF_TEXTCOLOR        1000    // 文字色指定ボタン（オーナードロー スウォッチ）
#define IDC_CF_BACKCOLOR        1001    // 背景色指定ボタン（オーナードロー スウォッチ）
#define IDC_CF_HILIST           1021    // 強調データ文字色リスト（強調表示コード。領域C後続）
#define IDC_CF_HL_ADD           1098    // 追加（強調表示コード）
#define IDC_CF_HL_DEL           1099    // 削除
#define IDC_CF_HL_CLEAR         1101    // 全削除
#define IDC_CF_HL_EDIT          1100    // 編集
#define IDC_CF_PREVIEW          1019    // プレビュー（オーナードロー静的）
#define IDC_CF_BITIMAGE         1011    // ビットイメージに指定色を反映させる（領域C後続）
#define IDC_CF_FONT             1010    // フォント...
#define IDC_CF_RESET            1006    // 初期設定（既定色/フォントへ戻す）
#define IDC_HL_CODE             1007    // 強調表示コード入力（IDD_HIGHLIGHT_CODE 187 のEDIT）

// --- 環境設定「編集１」ページ（IDD_SETTINGS_EDIT1 159。原 DoDataExchange 0x40e534 準拠） ---
#define IDC_ED1_SCROLLLINES         1007    // 垂直移動スクロール行数（edit）
#define IDC_ED1_SCROLLLINES_SPIN    1009    // スピン（1007 のバディ）
#define IDC_ED1_PASTE_OVERWRITE     1011    // 上書モード時の貼り付けは上書き処理する
#define IDC_ED1_SEARCH_NOTFOUND_MSG 1015    // 検索で見つからなかった場合にメッセージを表示する
#define IDC_ED1_ESC_MENU            1020    // Escメニューを有効にする
#define IDC_ED1_ESC_DESELECT        1025    // Escキーで選択解除する
#define IDC_ED1_DESELECT_AFTER_COPY 1066    // 選択データのコピー後に選択解除する
#define IDC_ED1_CLEAR_UNDO_ON_SAVE  1069    // 保存時にアンドゥバッファをクリアする
#define IDC_ED1_SUBCARET            1070    // サブキャレットを表示する
#define IDC_ED1_HILIGHT_BOTH        1088    // データ選択時にコード・文字共に反転表示する
#define IDC_ED1_REALTIME_BITIMAGE   1102    // 編集内容をリアルタイムでビットイメージに反映する
// アンドゥバッファのメモリ上限（移植で追加。Issue #102）
#define IDC_ED1_UNDO_GROUP          1160    // 「アンドゥバッファ」グループ
#define IDC_ED1_UNDO_LIMIT          1161    // メモリ上限を設ける（check）
#define IDC_ED1_UNDO_MB             1162    // 上限（edit。MB 単位）
#define IDC_ED1_UNDO_MB_SPIN        1163    // スピン（1162 のバディ）
#define IDC_ED1_UNDO_MB_UNIT        1164    // 「MB」（static）
#define IDC_ED1_2STROKE_SLIDER      1067    // ２ストロークキーのタイムアウト時間（slider）
#define IDC_ED1_2STROKE_LABEL       1068    // タイムアウト値ラベル（static）

// --- 環境設定「編集２」ページ（IDD_SETTINGS_EDIT2 197。原 DDX FUN_004659cf） ---
#define IDC_ED2_HISTORY             1007    // ファイル履歴数（edit）
#define IDC_ED2_HISTORY_SPIN        1009    // スピン（1007 のバディ）
#define IDC_ED2_CARET_RESTORE       1011    // キャレット位置の自動復元
#define IDC_ED2_CURPOS_STRUCT       1015    // 現在位置を構造体編集アドレスに自動設定
#define IDC_ED2_NEWDOC_EDITABLE     1020    // 新規ドキュメントは常に編集可能として開く
#define IDC_ED2_DYNAMIC_MARK        1025    // ダイナミックマーク
#define IDC_ED2_MARK_AUTO_RESTORE   1165    // マークの自動復元（移植で追加。Issue #100）
#define IDC_ED2_END_AUTOINSERT      1066    // 上書きモード時の末尾自動挿入

// --- 環境設定「ファイル」ページ（IDD_SETTINGS_FILE 157。原 DDX 0x4103f4） ---
#define IDC_FILE_BACKUP_CREATE      1030    // バックアップファイルの作成
#define IDC_FILE_BACKUP_GEN_LABEL   1031    // バックアップ世代数（label）
#define IDC_FILE_BACKUP_GEN         1032    // バックアップ世代数（edit）
#define IDC_FILE_BACKUP_GEN_SPIN    1034    // スピン（1032 のバディ）
#define IDC_FILE_BACKUP_FOLDER_CHK  1035    // バックアップフォルダの指定
#define IDC_FILE_BACKUP_FOLDER      1036    // バックアップフォルダ（edit）
#define IDC_FILE_BACKUP_FOLDER_BTN  1092    // バックアップフォルダ参照（...）
#define IDC_FILE_EXCL_NONE          1016    // 排他制御: しない
#define IDC_FILE_EXCL_WRITE         1017    // 排他制御: 書き込み禁止
#define IDC_FILE_EXCL_RW            1018    // 排他制御: 読み書き禁止
#define IDC_FILE_LINK_DIRECT        1113    // リンクファイルは直接開く
#define IDC_FILE_DEFFOLDER_CHK      1091    // デフォルトフォルダの指定
#define IDC_FILE_DEFFOLDER          1037    // デフォルトフォルダ（edit）
#define IDC_FILE_DEFFOLDER_BTN      1093    // デフォルトフォルダ参照（...）
// 設定ファイルの所在表示（移植で追加。Issue #111）
#define IDC_FILE_INI_GROUP          1149    // 「設定ファイル」グループ
#define IDC_FILE_INI_PATH           1150    // 設定ファイルのパス（読み取り専用 edit）
#define IDC_FILE_INI_SOURCE         1151    // 保存先がどの規則で決まったか（static）
#define IDC_FILE_INI_OPEN           1153    // 「フォルダを開く」
#define IDC_FILE_INI_READONLY       1154    // 読み込み失敗で保存しない旨の注記（static）
// 大きいファイルを開く前の確認（移植で追加。Issue #101）
#define IDC_FILE_LARGE_GROUP        1155    // 「大きいファイル」グループ
#define IDC_FILE_LARGE_WARN         1156    // 開く前に確認する（check）
#define IDC_FILE_LARGE_MB           1157    // しきい値（edit。MB 単位）
#define IDC_FILE_LARGE_MB_SPIN      1158    // スピン（1157 のバディ）
#define IDC_FILE_LARGE_MB_UNIT      1159    // 「MB 以上」（static）

// --- 環境設定「ウィンドウ」ページ（IDD_SETTINGS_WINDOW 182。原 DDX FUN_0046668d） ---
#define IDC_WIN_PLACE_NONE      1016    // メインウィンドウ: 指定しない（radio group 先頭）
#define IDC_WIN_PLACE_LAST      1017    // メインウィンドウ: 前回終了時の位置・サイズ
#define IDC_WIN_PLACE_MAX       1018    // メインウィンドウ: 最大化
#define IDC_WIN_PLACE_SPEC      1019    // メインウィンドウ: 指定
#define IDC_WIN_LEFT            1078    // 指定時の左（edit）
#define IDC_WIN_TOP             1079    // 指定時の上（edit）
#define IDC_WIN_WIDTH           1080    // 指定時の幅（edit）
#define IDC_WIN_HEIGHT          1083    // 指定時の高さ（edit）
#define IDC_WIN_DOC_MAXIMIZE    1011    // ドキュメントウィンドウを最大化で開く
#define IDC_WIN_DOC_FULLPATH    1015    // ドキュメントのフルパス表示
#define IDC_WIN_SHOW_TOOLBAR    1020    // ツールバーの表示
#define IDC_WIN_SHOW_STATUSBAR  1025    // ステータスバーの表示
#define IDC_WIN_BITIMAGE_DOCK   1066    // ビットイメージをドッキング可能にする
#define IDC_WIN_SBAR_POS_BOTTOM 1044    // 構造体編集バーの位置: 下（radio group 先頭）
#define IDC_WIN_SBAR_POS_TOP    1045    // 構造体編集バーの位置: 上
#define IDC_WIN_SBAR_POS_FLOAT  1046    // 構造体編集バーの位置: フローティング
#define IDC_WIN_SBAR_NODOCK     1069    // ドッキング不能とする
#define IDC_WIN_SBAR_ST_BOTTOM  1147    // 構造体編集バーのステータス表示: 下（radio group 先頭）
#define IDC_WIN_SBAR_ST_TOP     1148    // 構造体編集バーのステータス表示: 上
#define IDC_WIN_SBAR_ST_HIDE    1049    // 構造体編集バーのステータス表示: 非表示
#define IDC_WIN_SITEM_RATIO     1070    // 構造体アイテム幅の比率を保持

// --- 環境設定「ステータスバー」ページ（IDD_SETTINGS_STATUSBAR 181） ---
#define IDC_SBAR_CURRENT        1021    // 現在のステータスバー設定（listbox）
#define IDC_SBAR_AVAILABLE      1024    // 追加できる項目（listbox）
#define IDC_SBAR_ADD            1071    // <<追加
#define IDC_SBAR_DELETE         1072    // 削除>>
#define IDC_SBAR_UP             1073    // 上へ移動
#define IDC_SBAR_DOWN           1074    // 下へ移動

// --- 環境設定「ツールバー」ページ（IDD_SETTINGS_TOOLBAR 178。原カタログ DAT_004b6c90/名称4000-58xx） ---
#define IDC_TBAR_CURRENT        1021    // 現在のツールバー設定（listbox）
#define IDC_TBAR_CATEGORY       1026    // 追加できる機能: カテゴリ（combo, 名称4000-4007）
#define IDC_TBAR_AVAILABLE      1024    // 追加できる機能: 項目（listbox）
#define IDC_TBAR_UP             1073    // 上へ移動
#define IDC_TBAR_DOWN           1074    // 下へ移動
#define IDC_TBAR_SEPARATOR      1075    // <<セパレータ
#define IDC_TBAR_ADD            1071    // <<追加
#define IDC_TBAR_DELETE         1072    // 削除>>

// --- 環境設定「ユーザーメニュー」ページ（IDD_SETTINGS_USERMENU 183。原 DDX 0x42ad85） ---
#define IDC_UM_MENUSET          1084    // 現在のメニュー設定（combo, 15メニュー）
#define IDC_UM_CURRENT          1085    // メニュー項目（listbox）
#define IDC_UM_CATEGORY         1083    // 追加できる機能: カテゴリ（combo, 4000-4007）
#define IDC_UM_AVAILABLE        1086    // 追加できる機能: 項目（listbox）
#define IDC_UM_UP               1073    // 上へ移動
#define IDC_UM_DOWN             1074    // 下へ移動
#define IDC_UM_SEPARATOR        1075    // <<セパレータ
#define IDC_UM_ADD              1071    // <<追加
#define IDC_UM_DELETE           1072    // 削除>>

// --- アクセラレータの指定ダイアログ（IDD_ACCEL_INPUT 184、モーダル。原 FUN_004013f0 系） ---
//   ユーザーメニュー項目のアクセラレータ（1文字、大文字強制）を入力する。
#define IDC_ACCEL_EDIT          1007    // アクセラレータ入力（edit、ES_UPPERCASE / LimitText(1)）

// --- 環境設定「キーアサイン」ページ（IDD_KEYASSIGN 139。原 DDX 0x419a1d／充填 FUN_00419b97） ---
//   機能セレクタ(1026/1024)は原では実行時生成のカスタム コンボリスト。方針により標準コントロールで再現。
#define IDC_KA_KEYLIST          1021    // キー一覧（listbox。Ctrl/Shift状態で内容が変化）
#define IDC_KA_CTRL             1022    // Ctrl（checkbox）
#define IDC_KA_SHIFT            1023    // Shift（checkbox）
#define IDC_KA_RESET            1000    // 初期設定...（既定キーマップへ戻す）
#define IDC_KA_LOAD             1001    // 読み込み...（キーマップ読込）
#define IDC_KA_SAVE             1002    // 書き出し...（キーマップ保存）
#define IDC_KA_FUNC_CATEGORY    1026    // 機能: カテゴリ（combo, 4000-4007）
#define IDC_KA_FUNC_LIST        1024    // 機能: 項目（listbox）

// --- マーク関連 文字列（文字列テーブル） ---
#define IDS_MARK_NEW_ENTRY      1005    // "＜マークの新規登録＞"（一覧末尾の追加行）
#define IDS_MARK_ADDR_DEC       1000    // "数字を入力してください"（10進解析失敗）
#define IDS_MARK_ADDR_HEX       1004    // "１６進数で入力してください"（16進解析失敗）
#define IDS_MARK_ADDR_RANGE     1003    // "指定アドレスは無効です"（範囲超過）
#define IDS_NO_VALID_SELECTION  1057    // "有効な項目が選択されていません"（一覧未選択）

// --- 検索/置換メッセージ（文字列テーブル） ---
#define IDS_SEARCH_NOTFOUND     1006    // "見つかりませんでした"（検索不一致メッセージ。原 view+0x264 有効時）
#define IDS_SEARCH_EMPTY        1014    // "検索データが入力されていません"
#define IDS_INVALID_DATA        1015    // "データの指定が不正です"
#define IDS_REPLACE_EMPTY       1016    // "置換データが…\n\n検索データ削除モードで実行しますか？"
#define IDS_REPLACE_COUNT       1017    // "%d個置換しました"

// --- 指定範囲の初期化メッセージ（文字列テーブル） ---
#define IDS_FILL_EMPTY          1021    // "初期化データが指定されていません"

// --- 選択範囲の保存メッセージ（文字列テーブル） ---
#define IDS_SAVE_BACKUP_FAILED  1010    // "ファイルのバックアップに失敗しました"（CMirrorFile 開失敗）

// --- 編集前に戻す 確認（文字列テーブル） ---
#define IDS_REVERT_CONFIRM      1022    // "編集内容を破棄してファイルを再読み込みします"（YESNO）

// --- データ比較メッセージ（文字列テーブル） ---
#define IDS_COMPARE_EMPTY       1023    // "空のドキュメントが指定されています"
#define IDS_COMPARE_SIZEDIFF    1024    // "ドキュメントのサイズが異なります\n小さい方のサイズ分を比較します"
#define IDS_COMPARE_NODIFF      1025    // "違いはありません"
#define IDS_SAVEDUMP_NOFILE     1027    // "ファイル名が入力されていません"（ダンプ保存 空入力）

// 名前を指定して実行（原はコマンドライン空で 1027、起動失敗で 1026 を表示）。
#define IDS_RUN_NOFILE          1027    // "ファイル名が入力されていません"（コマンドライン空）
#define IDS_RUN_FAILED          1026    // "%sの実行に失敗しました"（ShellExecute 失敗）
#define IDS_RUN_FILTER          1063    // 参照ダイアログのフィルタ（原はコード内にハードコード）
#define IDS_BITIMAGE_FAILED     1050    // "ビットイメージの作成に失敗しました"（DIB生成失敗）
#define IDS_FILE_NOT_FOUND      1054    // "%sが見つかりません"（コマンドライン指定ファイルが無い）

// 構造体編集ポップアップ(189)の第1項目ラベル。原は行種別に応じて実行時に差し替える
//   （スカラ葉="編集(&E)"＝資源既定 / 折り畳みコンテナ="開く(&E)" / 展開済み="閉じる(&C)"）。
//   原の文字列表に該当エントリが無いため、移植では空き番号へ追加する。
// キャラクターセット名（原の文字列表 6040-6045。6046=UTF-8 は移植で追加。Issue #98/#125）。
//   索引 = 文字セットID（0=ASCII/1=SHIFT-JIS/2=EUC/3=Unicode/4=EBCDIC/5=EBCIDK/6=UTF-8）。
#define IDS_CHARSET_NAME_BASE   6040
#define IDS_CHARSET_NAME_LAST   6046

#define IDS_STRUCT_MENU_OPEN    1061    // "開く(&E)"
#define IDS_STRUCT_MENU_CLOSE   1062    // "閉じる(&C)"

// 移植で追加した UI 文字列（原の文字列表に該当が無いもの）。1100番台を使用する。
#define IDS_ADDR_RANGE_HEX      1100    // "有効アドレス : 0 ～ %X"
#define IDS_ADDR_RANGE_DEC      1101    // "有効アドレス : 0 ～ %d"
#define IDS_ADDR_CURRENT_HEX    1102    // "現在アドレス : %X"
#define IDS_ADDR_CURRENT_DEC    1103    // "現在アドレス : %d"
#define IDS_FILL_RANGE_LABEL    1104    // "指定範囲 : %08X ～ %08X"
#define IDS_SAVEDUMP_OVERWRITE  1105    // "%s は既に存在します。\n上書きしますか？"
#define IDS_SEPARATOR_ITEM      1106    // "＜セパレータ＞"
#define IDS_UM_MENU_FMT         1107    // "メニュー%d"
#define IDS_UM_TWOSTROKE_FMT    1108    // "２ストローク機能%d"
#define IDS_UM_ESC_MENU         1109    // "Esc メニュー"
#define IDS_UM_CONTEXT_MENU     1110    // "コンテキストメニュー"
#define IDS_KA_RESET_CONFIRM    1111    // "キーアサインを初期設定に戻します"
#define IDS_KA_EXPORT_FAILED    1112    // "書き出しに失敗しました"
#define IDS_KA_IMPORT_FAILED    1113    // "読み込みに失敗しました"
#define IDS_KA_FILE_INVALID     1114    // "キーマップファイルが不正です"
#define IDS_HEX_BYTE_INPUT      1115    // "16進数で 00〜FF を入力してください"
#define IDS_SECONDS_SUFFIX      1116    // "秒"
#define IDS_FOLDER_SELECT_TIP   1117    // "フォルダを選択してください"
#define IDS_FOLDER_NOT_FOUND    1118    // "フォルダが存在しません。"
#define IDS_BGREP_ACCESS_DENIED 1119    // " : アクセスを拒否されました"
#define IDS_BGREP_NO_FILES      1120    // "該当ファイルがありません"
#define IDS_BGREP_HIT_COUNT     1121    // "%d件見つかりました"
#define IDS_DIFF_COL_START      1122    // "相違箇所"
#define IDS_DIFF_COL_END        1123    // "相違終了箇所"
#define IDS_DIFF_COL_SIZE       1124    // "相違サイズ"
#define IDS_STRUCT_COL_TYPE     1125    // "型"
#define IDS_STRUCT_COL_NAME     1126    // "シンボル名"
#define IDS_STRUCT_COL_VALUE    1127    // "値"
#define IDS_COLOR_HEADER        1128    // "ヘッダー"
#define IDS_COLOR_ADDRESS       1129    // "アドレス表示"
#define IDS_COLOR_DATA          1130    // "データ"
#define IDS_COLOR_MARK1         1131    // "マーク1"
#define IDS_COLOR_MARK2         1132    // "マーク2"
#define IDS_COLOR_MARK3         1133    // "マーク3"
#define IDS_COLOR_COMPARE       1134    // "比較相違箇所"
#define IDS_COLOR_STRUCT        1135    // "構造体編集該当部"
#define IDS_EXT_TAB_DISPLAY     1136    // "表示状態"
#define IDS_EXT_TAB_COLORFONT   1137    // "色・フォント"
#define IDS_ENV_SHEET_TITLE     1138    // "環境設定"
#define IDS_INDICATOR_INSERT    1139    // "挿入"
#define IDS_BAR_BITIMAGE        1140    // "ビットイメージ"
#define IDS_BAR_OUTPUT          1141    // "アウトプット"
#define IDS_FILTER_ALL_FILES    1142    // "すべてのファイル (*.*)|*.*||"
#define IDS_ERR_OLE_INIT        1163    // "OLE/COMライブラリを初期化できませんでした。"
#define IDS_ERR_MUTEX           1164    // "多重起動防止用Mutexを作成できませんでした。"
#define IDS_ERR_TRANSFER        1165    // "既存のStirHexへファイルを転送できませんでした。"
#define IDS_ERR_MAINWND_ID      1166    // "単一起動用のメインウィンドウ識別情報を設定できませんでした。"
#define IDS_FOLDER_SELECT_TITLE 1167    // "フォルダの選択"
#define IDS_ERR_FILE_OUT_OF_MEMORY 1168  // "メモリが不足しているため読み込めませんでした..."
#define IDS_CONFIRM_LARGE_FILE  1169    // "このファイルはサイズが大きいため..."
#define IDS_ERROR_REASON        1170    // "\n\n理由 : %s"（エラーへ添える Win32/HRESULT の説明）
#define IDS_FOLDER_SELECT_FAILED 1171   // "フォルダ選択ダイアログを表示できませんでした。"
#define IDS_STRUCT_DEF_PARSE_ERROR 1172 // "構造体定義ファイル(Struct.def)を読み込めませんでした。..."
#define IDS_ERR_CLIPBOARD_COPY  1173    // "クリップボードへコピーできませんでした。"
#define IDS_CONFIRM_UNDOLESS_EDIT 1174 // "この操作は取り消し（アンドゥ）用に..."
#define IDS_ERR_HELP_NOT_FOUND  1175    // "ヘルプファイルが見つかりません。..."
#define IDS_ERR_HELP_OPEN       1176    // "ヘルプファイルを開けませんでした。"
#define IDS_ERR_APP_ID          1177    // "アプリケーション識別子を設定できませんでした。"
#define IDS_ERR_PASTE_HEX_NO_TEXT   1178    // "クリップボードにテキストがありません。"
#define IDS_ERR_PASTE_HEX_READ      1179    // "クリップボードのテキストを取得できませんでした。"
#define IDS_ERR_PASTE_HEX_EMPTY     1180    // "16進数値が含まれていません。"
#define IDS_ERR_PASTE_HEX_INVALID   1181    // "16進数として扱えない文字があります（%d文字目）"
#define IDS_ERR_PASTE_HEX_ODD       1182    // "2桁単位になっていません（%d文字目）"
#define IDS_ERR_SETTINGS_LOAD       1183    // "設定ファイルを読み込めませんでした。..."
#define IDS_ERR_SETTINGS_SAVE       1184    // "設定ファイルを保存できませんでした。..."
#define IDS_SETTINGS_SRC_CMDLINE    1185    // "保存先: コマンドライン指定（/ini:）"
#define IDS_SETTINGS_SRC_PORTABLE   1186    // "保存先: 実行ファイルと同じフォルダ..."
#define IDS_SETTINGS_SRC_APPDATA    1187    // "保存先: %APPDATA%（既定）"
#define IDS_SETTINGS_READONLY_NOTE  1188    // "読み込みに失敗したため..."
#define IDS_ERR_SETTINGS_REVEAL     1189    // "設定ファイルの場所を開けませんでした"
#define IDS_MARK_FILE_FILTER        1190    // マークファイルのフィルタ（| 区切り）
#define IDS_ERR_MARK_LOAD           1191    // "マークファイルを読み込めませんでした..."
#define IDS_ERR_MARK_SAVE           1192    // "マークファイルを保存できませんでした..."
#define IDS_CONFIRM_MARK_MERGE      1193    // 既存マークに追加するか置き換えるか
#define IDS_CONFIRM_MARK_SIZE       1194    // データサイズが異なる旨の確認
#define IDS_MARK_IMPORT_DONE        1195    // "%d 件のマークを読み込みました"
#define IDS_MARK_IMPORT_DONE_SKIP   1196    // 範囲外を読み飛ばした場合
#define IDS_ERR_RANGE_READ_FAILED   1198    // "データを読み取れませんでした..."
#define IDS_ERR_EDIT_OUT_OF_MEMORY  1197    // "メモリが不足しているため、この操作を実行できませんでした"
#define IDS_SBAR_ITEM_BASE      1143    // ステータスバー項目名20件（カタログ順に連番）

// 既存の文字列リソースを参照するための別名（文言は原の文字列表にあるもの）。
#define IDS_EXT_RESET_CONFIRM       1002    // "初期設定値に戻します"（拡張子別設定）
#define IDS_EXT_DELETE_CONFIRM      1051    // "選択された項目を削除します"
#define IDS_EXT_BASE_UNDELETABLE    1052    // "基本設定は削除できません"
#define IDS_EXT_DELETE_ALL_CONFIRM  1055    // "登録されている項目を全て削除します"
#define IDS_ERR_SAVE_FAILED         1018    // 保存失敗（"%s"にパスを埋める）
// 排他制御されたファイルを閲覧モードで開くかの確認（原 AfxMessageBox(1041, MB_OKCANCEL)。Issue #120）
#define IDS_CONFIRM_VIEW_MODE       1041
#define IDS_ERR_LOAD_FAILED         1020    // 読込失敗（"%s"にパスを埋める）
#define IDS_ERR_WRITE_FAILED        1059    // "ファイルの書き込みに失敗しました"
#define IDS_INDICATOR_OVERWRITE_TEXT 59140  // "上書"（ステータスバー表示）
#define IDS_INDICATOR_EDITLOCK_TEXT  59143  // "編禁"（ステータスバー表示）
#define IDS_INDICATOR_MODIFIED_TEXT  59144  // "更新"（ステータスバー表示）

#endif // STIRLING_RESOURCE_H
