// 環境設定ダイアログ（原 0x8050）。原は CPropertySheet に 8 ページ
//   （編集1/編集2/ファイル/ウィンドウ/キーアサイン/ユーザーメニュー/ツールバー/ステータスバー）。
//   本移植では単独プロパティシートとして忠実再現し、ページは増分で追加する（現状: 編集1）。
#pragma once

#include <afxdlgs.h>   // CPropertySheet / CPropertyPage

#include "app/AppSettings.h"

// 「編集１」ページ（子ダイアログ IDD_SETTINGS_EDIT1 159）。
//   垂直移動スクロール行数・各種編集動作チェック・２ストロークタイムアウトを編集する。
class CEditPage1 : public CPropertyPage {
public:
    CEditPage1();

    CAppSettings* m_pS = nullptr;   // シートの作業コピーを指す（OK でシート側が確定）

protected:
    // DDX 中間値
    int  m_scrollLines = 1;
    BOOL m_pasteOverwrite = FALSE;
    BOOL m_searchNotFoundMsg = FALSE;
    BOOL m_escMenu = FALSE;
    BOOL m_escDeselect = FALSE;
    BOOL m_deselectAfterCopy = FALSE;
    BOOL m_clearUndoOnSave = FALSE;
    BOOL m_subCaret = FALSE;
    BOOL m_highlightBoth = FALSE;
    BOOL m_realtimeBitImage = FALSE;
    int  m_twoStrokeTimeoutMs = 500;
    BOOL m_undoMemoryLimit = TRUE;    // アンドゥバッファのメモリ上限を設ける
    int  m_undoMemoryLimitMB = 256;   // その上限（MB）

    virtual void DoDataExchange(CDataExchange* pDX);
    virtual BOOL OnInitDialog();
    virtual BOOL OnKillActive();

    void ChargeFromSettings();   // m_pS → 中間値
    void HarvestToSettings();    // 中間値 → m_pS
    void UpdateTimeoutLabel();   // スライダ値をラベルへ反映
    void UpdateEnableState();    // チェック状態に応じて従属コントロールを有効/無効化

    afx_msg void OnHScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar);
    afx_msg void OnUndoLimitChk();
    DECLARE_MESSAGE_MAP()
};

// 「編集２」ページ（子ダイアログ IDD_SETTINGS_EDIT2 197）。
//   ファイル履歴数・キャレット自動復元・構造体アドレス自動設定・新規編集可・末尾自動挿入・
//   ダイナミックマークを編集する。
class CEditPage2 : public CPropertyPage {
public:
    CEditPage2();

    CAppSettings* m_pS = nullptr;

protected:
    int  m_fileHistoryCount = 4;
    BOOL m_caretAutoRestore = FALSE;
    BOOL m_curPosToStructAddr = FALSE;
    BOOL m_newDocEditable = FALSE;
    BOOL m_endAutoInsert = FALSE;
    BOOL m_dynamicMark = FALSE;
    BOOL m_markAutoRestore = FALSE;

    virtual void DoDataExchange(CDataExchange* pDX);
    virtual BOOL OnInitDialog();
    virtual BOOL OnKillActive();

    void ChargeFromSettings();
    void HarvestToSettings();
    DECLARE_MESSAGE_MAP()
};

// 「ファイル」ページ（子ダイアログ IDD_SETTINGS_FILE 157）。
//   バックアップ作成/世代数/フォルダ、ファイル排他制御、リンク直接オープン、デフォルトフォルダを編集。
//   併せて、使用中の設定ファイルの所在を読み取り専用で表示する（移植で追加。Issue #111）。
class CFilePage : public CPropertyPage {
public:
    CFilePage();

    CAppSettings* m_pS = nullptr;

protected:
    BOOL     m_backupCreate = FALSE;
    int      m_backupGenerations = 1;
    BOOL     m_backupFolderSpecify = FALSE;
    CString  m_backupFolder;     // 表示・入力用（wide 層。確定値は m_pS が保持）
    int      m_exclusive = 0;    // 0=しない/1=書込禁止/2=読書禁止
    BOOL     m_linkDirect = FALSE;
    BOOL     m_defaultFolderSpecify = FALSE;
    CString  m_defaultFolder;
    BOOL     m_largeFileWarn = TRUE;    // 大きいファイルを開く前に確認する
    int      m_largeFileWarnMB = 512;   // そのしきい値（MB）

    virtual void DoDataExchange(CDataExchange* pDX);
    virtual BOOL OnInitDialog();
    virtual BOOL OnKillActive();

    void ChargeFromSettings();
    void HarvestToSettings();
    void UpdateEnableState();    // チェック状態に応じて従属コントロールを有効/無効化
    void ShowSettingsFileLocation();  // 設定ファイルの所在を表示する（Issue #111）
    void BrowseFolder(UINT editId);   // 「...」でフォルダ選択→edit へ反映

    afx_msg void OnBackupCreate();
    afx_msg void OnBackupFolderChk();
    afx_msg void OnDefFolderChk();
    afx_msg void OnLargeFileWarnChk();
    afx_msg void OnBackupFolderBtn();
    afx_msg void OnDefFolderBtn();
    afx_msg void OnOpenSettingsFolder();
    DECLARE_MESSAGE_MAP()
};

// 「ウィンドウ」ページ（子ダイアログ IDD_SETTINGS_WINDOW 182）。
//   メインウィンドウ配置、ドキュメント/バー表示、構造体編集バーの位置/ステータス表示を編集。
class CWindowPage : public CPropertyPage {
public:
    CWindowPage();

    CAppSettings* m_pS = nullptr;

protected:
    int  m_winPlacement = 1;
    int  m_winLeft = 0, m_winTop = 0, m_winWidth = 639, m_winHeight = 479;
    BOOL m_docMaximize = FALSE;
    BOOL m_docFullPath = FALSE;
    BOOL m_showToolbar = FALSE;
    BOOL m_showStatusbar = FALSE;
    BOOL m_bitImageDockable = FALSE;
    int  m_structBarPos = 0;
    BOOL m_structBarNoDock = FALSE;
    int  m_structBarStatusPos = 2;
    BOOL m_structItemRatioKeep = FALSE;

    virtual void DoDataExchange(CDataExchange* pDX);
    virtual BOOL OnInitDialog();
    virtual BOOL OnKillActive();

    void ChargeFromSettings();
    void HarvestToSettings();
    void UpdateEnableState();   // 「指定」選択時のみ位置エディットを有効化

    afx_msg void OnPlacementChange();
    DECLARE_MESSAGE_MAP()
};

// 「ステータスバー」ページ（子ダイアログ IDD_SETTINGS_STATUSBAR 181）。
//   現在の構成(1021)と追加できる項目(1024)の2リストで、追加/削除/上下移動を行う。
//   確定後はメインステータスバーのペイン列を動的再構築し、構成を永続化する。
class CStatusBarPage : public CPropertyPage {
public:
    CStatusBarPage();

    CAppSettings* m_pS = nullptr;

protected:
    virtual BOOL OnInitDialog();
    virtual BOOL OnKillActive();

    void RefillCurrent();     // m_pS->statusItems → 現在リスト(1021)
    void FillAvailable();     // カタログ全項目 → 追加リスト(1024)
    void HarvestToSettings(); // 現在リスト → m_pS->statusItems
    void UpdateButtons();     // 選択状態に応じてボタン活性

    afx_msg void OnAdd();
    afx_msg void OnDelete();
    afx_msg void OnUp();
    afx_msg void OnDown();
    afx_msg void OnSelChange();
    DECLARE_MESSAGE_MAP()
};

// 「ツールバー」ページ（子ダイアログ IDD_SETTINGS_TOOLBAR 178）。
//   カテゴリ(1026)で絞った機能(1024)を現在の構成(1021)へ追加/削除/並べ替え、セパレータ挿入。
//   原カタログ（8カテゴリ×コマンド、DAT_004b6c90／名称4000-58xx）を忠実再現。
//   確定後は原リソース128のアイコンを使ってメインツールバーを動的再構築し、構成を永続化する。
class CToolBarPage : public CPropertyPage {
public:
    CToolBarPage();

    CAppSettings* m_pS = nullptr;

protected:
    virtual BOOL OnInitDialog();
    virtual BOOL OnKillActive();

    void FillCategory();      // カテゴリコンボ(1026)に8カテゴリ名
    // 選択カテゴリの項目 − 現在構成 → 追加リスト(1024)。
    //   原の選択挙動: 復活した項目(preferRaw)があればそれを選び、無ければ
    //   直前の選択位置を維持する（keepIndex）。カテゴリ切替時のみ先頭へ戻す。
    void RefillAvailable(UINT preferRaw = 0, bool keepIndex = false);
    void RefillCurrent();     // m_pS->toolbarItems → 現在リスト(1021)
    void HarvestToSettings(); // 現在リスト → m_pS->toolbarItems
    void UpdateButtons();
    void InsertCurrent(int at, UINT raw, bool select);   // 現在リストへ1項目挿入

    // 原と同じオーナードロー描画（LBS_OWNERDRAWVARIABLE）。左端に 22x19 の凸枠セルを置き、
    //   その中へリソース128のアイコン(16x15)を透過描画し、右側にコマンド名を描く。
    CImageList m_icons;               // 原リソース128（16x15×54、C0C0C0 透過）
    CImageList& Icons();              // 遅延生成

    afx_msg void OnMeasureItem(int nIDCtl, LPMEASUREITEMSTRUCT lpMIS);
    afx_msg void OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT lpDIS);
    afx_msg void OnCategoryChange();
    afx_msg void OnAdd();
    afx_msg void OnSeparator();
    afx_msg void OnDelete();
    afx_msg void OnUp();
    afx_msg void OnDown();
    afx_msg void OnSelChange();
    DECLARE_MESSAGE_MAP()
};

// 「ユーザーメニュー」ページ（子ダイアログ IDD_SETTINGS_USERMENU 183）。
//   メニュー設定コンボ(1084)で15メニューを切替え、カテゴリ(1083)で絞った機能(1086)を
//   項目リスト(1085)へ追加/削除/セパレータ/並べ替え。原カタログ（フル）を忠実再現。
//   構成は右クリック／ユーザーメニュー／2ストロークメニューの実行時ポップアップへ反映・永続化する。
class CUserMenuPage : public CPropertyPage {
public:
    CUserMenuPage();

    CAppSettings* m_pS = nullptr;

protected:
    int m_curMenu = 0;   // 編集中のメニュー設定索引（0..14）

    virtual BOOL OnInitDialog();
    virtual BOOL OnKillActive();

    void FillMenuSet();       // メニュー設定コンボ(1084)に15メニュー名
    void FillCategory();      // カテゴリコンボ(1083)に8カテゴリ名
    void RefillAvailable();   // 選択カテゴリの全項目 − 現在メニュー → 機能リスト(1086)
    void RefillCurrent();     // m_pS->userMenus[m_curMenu] → 項目リスト(1085)
    void HarvestCurrent();    // 項目リスト → m_pS->userMenus[m_curMenu]
    void UpdateButtons();
    void InsertCurrent(int at, UINT raw, bool select);

    afx_msg void OnMenuSetChange();
    afx_msg void OnCategoryChange();
    afx_msg void OnAdd();
    afx_msg void OnSeparator();
    afx_msg void OnDelete();
    afx_msg void OnUp();
    afx_msg void OnDown();
    afx_msg void OnSelChange();
    afx_msg void OnCurrentDblClk();   // 項目のダブルクリックでアクセラレータを変更（原 FUN_0042c462）
    DECLARE_MESSAGE_MAP()
};

// 「キーアサイン」ページ（子ダイアログ IDD_KEYASSIGN 139）。
//   キー一覧(1021)は Ctrl(1022)/Shift(1023) 状態で内容が変化。選択キーの割当機能を
//   機能セレクタ（カテゴリ1026＋一覧1024）で表示・変更。初期設定/読み込み/書き出し対応。
//   ※ 機能セレクタは原では実行時生成のカスタム コンボリスト。方針により標準コントロールで挙動再現。
//   確定したkeymapはビューのキー入力ディスパッチへ即時反映し、設定ファイルへ永続化する。
class CKeyAssignPage : public CPropertyPage {
public:
    CKeyAssignPage();

    CAppSettings* m_pS = nullptr;

protected:
    virtual void DoDataExchange(CDataExchange* pDX);
    virtual BOOL OnInitDialog();

    int  ModState() const;          // 0=無/1=Shift/2=Ctrl/3=Ctrl+Shift
    void RefillKeyList();           // 修飾状態に応じたキー一覧を構築
    void FillFuncCategory();        // 機能カテゴリコンボ
    void RefillFuncList();          // 選択カテゴリの機能一覧（先頭に「なし」）
    void ShowAssignedFunc();        // 選択キーの割当機能を機能セレクタへ反映
    int  CurKeyCode() const;        // キー一覧の選択キーコード（無ければ -1）

    BOOL m_ctrl = FALSE;
    BOOL m_shift = FALSE;

    afx_msg void OnModifierChange();  // Ctrl/Shift トグル
    afx_msg void OnKeySelChange();    // キー選択→割当機能表示
    afx_msg void OnFuncCategoryChange();
    afx_msg void OnFuncSelChange();   // 機能選択→現在キーへ割当
    // 「初期設定...」ボタン。名前を OnReset にすると MFC の仮想 CPropertyPage::OnReset()
    //   （Cancel 押下時に呼ばれる）を誤ってオーバーライドし、キャンセルで確認が出るため改名。
    afx_msg void OnResetKeymap();     // 初期設定
    afx_msg void OnLoad();            // 読み込み
    afx_msg void OnSave();            // 書き出し
    DECLARE_MESSAGE_MAP()
};

// 環境設定プロパティシート本体。作業コピー m_s を保持し、DoModal()==IDOK で呼び出し側が確定する。
class CEnvSheet : public CPropertySheet {
public:
    CEnvSheet(const CAppSettings& src, CWnd* pParent = nullptr);

    CAppSettings m_s;   // in/out（OK で確定）

protected:
    CEditPage1     m_page1;
    CEditPage2     m_page2;
    CFilePage      m_pageFile;
    CWindowPage    m_pageWindow;
    CKeyAssignPage m_pageKeyAssign;
    CToolBarPage   m_pageToolBar;
    CUserMenuPage  m_pageUserMenu;
    CStatusBarPage m_pageStatusBar;
    DECLARE_MESSAGE_MAP()
};
