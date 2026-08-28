// 環境設定ダイアログ（0x8050）実装。現状は「編集１」ページのみ（増分でページ追加）。
#include "pch.h"
#include "dialog/EnvSettingsDlg.h"
#include "dialog/AccelInputDlg.h"   // 「アクセラレータの指定」ダイアログ（原 IDD 184）
#include "resource.h"
#include "app/ShellUtil.h"   // ui::BrowseForFolder（IFileDialog ベースのフォルダ選択）
#include "app/UiStrings.h"   // UI文字列はリソースから（ui::LoadW / CommandNameW）
#include "frame/ToolbarCatalog.h"   // ToolbarRawToImage（ツールバー設定のアイコン表示）

// ２ストロークタイムアウトのスライダは 0.1 秒刻み（デシ秒単位。原の表示書式 "%d.%d秒"）。
//   内部保持 twoStrokeTimeoutMs はミリ秒。スライダ位置(デシ秒)×100 で相互変換する。
static const int kTimeoutMinDs = 0;    // 0.0 秒
static const int kTimeoutMaxDs = 20;   // 2.0 秒

// ===========================================================================
// CEditPage1
// ===========================================================================
BEGIN_MESSAGE_MAP(CEditPage1, CPropertyPage)
    ON_WM_HSCROLL()
END_MESSAGE_MAP()

CEditPage1::CEditPage1() : CPropertyPage(IDD_SETTINGS_EDIT1) {}

void CEditPage1::ChargeFromSettings() {
    if (m_pS == nullptr) { return; }
    m_scrollLines       = m_pS->scrollLines;
    m_pasteOverwrite    = m_pS->pasteOverwrite ? TRUE : FALSE;
    m_searchNotFoundMsg = m_pS->searchNotFoundMsg ? TRUE : FALSE;
    m_escMenu           = m_pS->escMenu ? TRUE : FALSE;
    m_escDeselect       = m_pS->escDeselect ? TRUE : FALSE;
    m_deselectAfterCopy = m_pS->deselectAfterCopy ? TRUE : FALSE;
    m_clearUndoOnSave   = m_pS->clearUndoOnSave ? TRUE : FALSE;
    m_subCaret          = m_pS->subCaret ? TRUE : FALSE;
    m_highlightBoth     = m_pS->highlightBoth ? TRUE : FALSE;
    m_realtimeBitImage  = m_pS->realtimeBitImage ? TRUE : FALSE;
    m_twoStrokeTimeoutMs = m_pS->twoStrokeTimeoutMs;
}

void CEditPage1::HarvestToSettings() {
    if (m_pS == nullptr) { return; }
    m_pS->scrollLines       = m_scrollLines;
    m_pS->pasteOverwrite    = (m_pasteOverwrite != FALSE);
    m_pS->searchNotFoundMsg = (m_searchNotFoundMsg != FALSE);
    m_pS->escMenu           = (m_escMenu != FALSE);
    m_pS->escDeselect       = (m_escDeselect != FALSE);
    m_pS->deselectAfterCopy = (m_deselectAfterCopy != FALSE);
    m_pS->clearUndoOnSave   = (m_clearUndoOnSave != FALSE);
    m_pS->subCaret          = (m_subCaret != FALSE);
    m_pS->highlightBoth     = (m_highlightBoth != FALSE);
    m_pS->realtimeBitImage  = (m_realtimeBitImage != FALSE);
    // スライダの現在値(デシ秒)を回収し、ミリ秒へ変換。
    if (CSliderCtrl* sl = (CSliderCtrl*)GetDlgItem(IDC_ED1_2STROKE_SLIDER)) {
        m_twoStrokeTimeoutMs = sl->GetPos() * 100;
    }
    m_pS->twoStrokeTimeoutMs = m_twoStrokeTimeoutMs;
}

void CEditPage1::DoDataExchange(CDataExchange* pDX) {
    CPropertyPage::DoDataExchange(pDX);
    DDX_Text(pDX, IDC_ED1_SCROLLLINES, m_scrollLines);
    DDV_MinMaxInt(pDX, m_scrollLines, 1, 999);
    DDX_Check(pDX, IDC_ED1_PASTE_OVERWRITE, m_pasteOverwrite);
    DDX_Check(pDX, IDC_ED1_SEARCH_NOTFOUND_MSG, m_searchNotFoundMsg);
    DDX_Check(pDX, IDC_ED1_ESC_MENU, m_escMenu);
    DDX_Check(pDX, IDC_ED1_ESC_DESELECT, m_escDeselect);
    DDX_Check(pDX, IDC_ED1_DESELECT_AFTER_COPY, m_deselectAfterCopy);
    DDX_Check(pDX, IDC_ED1_CLEAR_UNDO_ON_SAVE, m_clearUndoOnSave);
    DDX_Check(pDX, IDC_ED1_SUBCARET, m_subCaret);
    DDX_Check(pDX, IDC_ED1_HILIGHT_BOTH, m_highlightBoth);
    DDX_Check(pDX, IDC_ED1_REALTIME_BITIMAGE, m_realtimeBitImage);
}

BOOL CEditPage1::OnInitDialog() {
    ChargeFromSettings();
    CPropertyPage::OnInitDialog();   // DDX_/UpdateData(FALSE) が中間値を反映

    // スクロール行数スピン（1007 のバディ）: 範囲 1..999。
    if (CSpinButtonCtrl* spin = (CSpinButtonCtrl*)GetDlgItem(IDC_ED1_SCROLLLINES_SPIN)) {
        spin->SetRange32(1, 999);
        spin->SetBuddy(GetDlgItem(IDC_ED1_SCROLLLINES));
        spin->SetPos(m_scrollLines);
    }
    // ２ストロークタイムアウト スライダ（0.1秒刻み。ミリ秒→デシ秒に四捨五入）。
    if (CSliderCtrl* sl = (CSliderCtrl*)GetDlgItem(IDC_ED1_2STROKE_SLIDER)) {
        sl->SetRange(kTimeoutMinDs, kTimeoutMaxDs, TRUE);
        sl->SetPageSize(2);    // 0.2秒
        sl->SetTicFreq(2);     // 0.2秒ごと
        int ds = (m_twoStrokeTimeoutMs + 50) / 100;
        if (ds < kTimeoutMinDs) { ds = kTimeoutMinDs; }
        if (ds > kTimeoutMaxDs) { ds = kTimeoutMaxDs; }
        sl->SetPos(ds);
    }
    UpdateTimeoutLabel();
    return TRUE;
}

BOOL CEditPage1::OnKillActive() {
    if (!CPropertyPage::OnKillActive()) {   // UpdateData(TRUE)＋DDV
        return FALSE;
    }
    HarvestToSettings();
    return TRUE;
}

void CEditPage1::UpdateTimeoutLabel() {
    CSliderCtrl* sl = (CSliderCtrl*)GetDlgItem(IDC_ED1_2STROKE_SLIDER);
    CWnd* lbl = GetDlgItem(IDC_ED1_2STROKE_LABEL);
    if (sl == nullptr || lbl == nullptr) { return; }
    const int ds = sl->GetPos();   // デシ秒（0.1秒単位）
    CStringW s;
    s.Format(L"%d.%d%s", ds / 10, ds % 10, ui::LoadW(IDS_SECONDS_SUFFIX).GetString());   // 原の書式 "%d.%d秒"
    lbl->SetWindowText(s);
}

void CEditPage1::OnHScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar) {
    if (pScrollBar != nullptr &&
        pScrollBar->GetSafeHwnd() == ::GetDlgItem(m_hWnd, IDC_ED1_2STROKE_SLIDER)) {
        UpdateTimeoutLabel();
    }
    CPropertyPage::OnHScroll(nSBCode, nPos, pScrollBar);
}

// ===========================================================================
// CEditPage2
// ===========================================================================
BEGIN_MESSAGE_MAP(CEditPage2, CPropertyPage)
END_MESSAGE_MAP()

CEditPage2::CEditPage2() : CPropertyPage(IDD_SETTINGS_EDIT2) {}

void CEditPage2::ChargeFromSettings() {
    if (m_pS == nullptr) { return; }
    m_fileHistoryCount   = m_pS->fileHistoryCount;
    m_caretAutoRestore   = m_pS->caretAutoRestore ? TRUE : FALSE;
    m_curPosToStructAddr = m_pS->curPosToStructAddr ? TRUE : FALSE;
    m_newDocEditable     = m_pS->newDocEditable ? TRUE : FALSE;
    m_endAutoInsert      = m_pS->endAutoInsert ? TRUE : FALSE;
    m_dynamicMark        = m_pS->dynamicMark ? TRUE : FALSE;
}

void CEditPage2::HarvestToSettings() {
    if (m_pS == nullptr) { return; }
    m_pS->fileHistoryCount   = m_fileHistoryCount;
    m_pS->caretAutoRestore   = (m_caretAutoRestore != FALSE);
    m_pS->curPosToStructAddr = (m_curPosToStructAddr != FALSE);
    m_pS->newDocEditable     = (m_newDocEditable != FALSE);
    m_pS->endAutoInsert      = (m_endAutoInsert != FALSE);
    m_pS->dynamicMark        = (m_dynamicMark != FALSE);
}

void CEditPage2::DoDataExchange(CDataExchange* pDX) {
    CPropertyPage::DoDataExchange(pDX);
    DDX_Text(pDX, IDC_ED2_HISTORY, m_fileHistoryCount);
    DDV_MinMaxInt(pDX, m_fileHistoryCount, 0, 16);
    DDX_Check(pDX, IDC_ED2_CARET_RESTORE, m_caretAutoRestore);
    DDX_Check(pDX, IDC_ED2_CURPOS_STRUCT, m_curPosToStructAddr);
    DDX_Check(pDX, IDC_ED2_NEWDOC_EDITABLE, m_newDocEditable);
    DDX_Check(pDX, IDC_ED2_DYNAMIC_MARK, m_dynamicMark);
    DDX_Check(pDX, IDC_ED2_END_AUTOINSERT, m_endAutoInsert);
}

BOOL CEditPage2::OnInitDialog() {
    ChargeFromSettings();
    CPropertyPage::OnInitDialog();

    // ファイル履歴数スピン（1007 のバディ）: 範囲 0..16。
    if (CSpinButtonCtrl* spin = (CSpinButtonCtrl*)GetDlgItem(IDC_ED2_HISTORY_SPIN)) {
        spin->SetRange32(0, 16);
        spin->SetBuddy(GetDlgItem(IDC_ED2_HISTORY));
        spin->SetPos(m_fileHistoryCount);
    }
    return TRUE;
}

BOOL CEditPage2::OnKillActive() {
    if (!CPropertyPage::OnKillActive()) {
        return FALSE;
    }
    HarvestToSettings();
    return TRUE;
}

// ===========================================================================
// CFilePage
// ===========================================================================
BEGIN_MESSAGE_MAP(CFilePage, CPropertyPage)
    ON_BN_CLICKED(IDC_FILE_BACKUP_CREATE, &CFilePage::OnBackupCreate)
    ON_BN_CLICKED(IDC_FILE_BACKUP_FOLDER_CHK, &CFilePage::OnBackupFolderChk)
    ON_BN_CLICKED(IDC_FILE_DEFFOLDER_CHK, &CFilePage::OnDefFolderChk)
    ON_BN_CLICKED(IDC_FILE_BACKUP_FOLDER_BTN, &CFilePage::OnBackupFolderBtn)
    ON_BN_CLICKED(IDC_FILE_DEFFOLDER_BTN, &CFilePage::OnDefFolderBtn)
END_MESSAGE_MAP()

CFilePage::CFilePage() : CPropertyPage(IDD_SETTINGS_FILE) {}

void CFilePage::ChargeFromSettings() {
    if (m_pS == nullptr) { return; }
    m_backupCreate        = m_pS->backupCreate ? TRUE : FALSE;
    m_backupGenerations   = m_pS->backupGenerations;
    m_backupFolderSpecify = m_pS->backupFolderSpecify ? TRUE : FALSE;
    m_backupFolder        = m_pS->backupFolder;
    m_exclusive           = (m_pS->exclusiveControl >= 0 && m_pS->exclusiveControl <= 2) ? m_pS->exclusiveControl : 0;
    m_linkDirect          = m_pS->linkDirect ? TRUE : FALSE;
    m_defaultFolderSpecify = m_pS->defaultFolderSpecify ? TRUE : FALSE;
    m_defaultFolder       = m_pS->defaultFolder;
}

void CFilePage::HarvestToSettings() {
    if (m_pS == nullptr) { return; }
    m_pS->backupCreate         = (m_backupCreate != FALSE);
    m_pS->backupGenerations    = m_backupGenerations;
    m_pS->backupFolderSpecify  = (m_backupFolderSpecify != FALSE);
    m_pS->backupFolder         = m_backupFolder;
    m_pS->exclusiveControl     = m_exclusive;
    m_pS->linkDirect           = (m_linkDirect != FALSE);
    m_pS->defaultFolderSpecify = (m_defaultFolderSpecify != FALSE);
    m_pS->defaultFolder        = m_defaultFolder;
}

void CFilePage::DoDataExchange(CDataExchange* pDX) {
    CPropertyPage::DoDataExchange(pDX);
    DDX_Check(pDX, IDC_FILE_BACKUP_CREATE, m_backupCreate);
    DDX_Text(pDX, IDC_FILE_BACKUP_GEN, m_backupGenerations);
    DDV_MinMaxInt(pDX, m_backupGenerations, 1, 999);
    DDX_Check(pDX, IDC_FILE_BACKUP_FOLDER_CHK, m_backupFolderSpecify);
    DDX_Text(pDX, IDC_FILE_BACKUP_FOLDER, m_backupFolder);
    DDX_Radio(pDX, IDC_FILE_EXCL_NONE, m_exclusive);   // 1016=0 / 1017=1 / 1018=2
    DDX_Check(pDX, IDC_FILE_LINK_DIRECT, m_linkDirect);
    DDX_Check(pDX, IDC_FILE_DEFFOLDER_CHK, m_defaultFolderSpecify);
    DDX_Text(pDX, IDC_FILE_DEFFOLDER, m_defaultFolder);
}

BOOL CFilePage::OnInitDialog() {
    ChargeFromSettings();
    CPropertyPage::OnInitDialog();

    // 世代数スピン（1032 のバディ）: 範囲 1..999。
    if (CSpinButtonCtrl* spin = (CSpinButtonCtrl*)GetDlgItem(IDC_FILE_BACKUP_GEN_SPIN)) {
        spin->SetRange32(1, 999);
        spin->SetBuddy(GetDlgItem(IDC_FILE_BACKUP_GEN));
        spin->SetPos(m_backupGenerations);
    }
    UpdateEnableState();
    return TRUE;
}

BOOL CFilePage::OnKillActive() {
    if (!CPropertyPage::OnKillActive()) {
        return FALSE;
    }
    HarvestToSettings();
    return TRUE;
}

// チェック状態に応じて従属コントロールを有効/無効化する（原の UX に準拠）。
void CFilePage::UpdateEnableState() {
    const BOOL backup = (IsDlgButtonChecked(IDC_FILE_BACKUP_CREATE) != 0);
    const BOOL backupFolder = backup && (IsDlgButtonChecked(IDC_FILE_BACKUP_FOLDER_CHK) != 0);
    const BOOL defFolder = (IsDlgButtonChecked(IDC_FILE_DEFFOLDER_CHK) != 0);

    auto enable = [this](UINT id, BOOL on) {
        if (CWnd* w = GetDlgItem(id)) { w->EnableWindow(on); }
    };
    enable(IDC_FILE_BACKUP_GEN_LABEL, backup);
    enable(IDC_FILE_BACKUP_GEN, backup);
    enable(IDC_FILE_BACKUP_GEN_SPIN, backup);
    enable(IDC_FILE_BACKUP_FOLDER_CHK, backup);
    enable(IDC_FILE_BACKUP_FOLDER, backupFolder);
    enable(IDC_FILE_BACKUP_FOLDER_BTN, backupFolder);
    enable(IDC_FILE_DEFFOLDER, defFolder);
    enable(IDC_FILE_DEFFOLDER_BTN, defFolder);
}

void CFilePage::OnBackupCreate()    { UpdateEnableState(); }
void CFilePage::OnBackupFolderChk() { UpdateEnableState(); }
void CFilePage::OnDefFolderChk()    { UpdateEnableState(); }

// 「...」でフォルダ選択ダイアログを開き、選択結果を edit に反映する。
void CFilePage::BrowseFolder(UINT editId) {
    CWnd* edit = GetDlgItem(editId);
    if (edit == nullptr) { return; }

    CStringW cur;
    edit->GetWindowText(cur);

    CStringW selected;
    const HRESULT hr =
        ui::BrowseForFolder(GetSafeHwnd(), ui::LoadW(IDS_FOLDER_SELECT_TIP), cur, selected);
    if (SUCCEEDED(hr)) {
        edit->SetWindowText(selected);
    } else if (!ui::IsUserCancelled(hr)) {
        // キャンセル以外の失敗は握りつぶさず理由を添えて知らせる。
        ui::MsgBox(GetSafeHwnd(),
                   ui::AppendErrorReason(ui::LoadW(IDS_FOLDER_SELECT_FAILED),
                                         static_cast<DWORD>(hr)));
    }
}

void CFilePage::OnBackupFolderBtn() { BrowseFolder(IDC_FILE_BACKUP_FOLDER); }
void CFilePage::OnDefFolderBtn()    { BrowseFolder(IDC_FILE_DEFFOLDER); }

// ===========================================================================
// CWindowPage
// ===========================================================================
BEGIN_MESSAGE_MAP(CWindowPage, CPropertyPage)
    ON_BN_CLICKED(IDC_WIN_PLACE_NONE, &CWindowPage::OnPlacementChange)
    ON_BN_CLICKED(IDC_WIN_PLACE_LAST, &CWindowPage::OnPlacementChange)
    ON_BN_CLICKED(IDC_WIN_PLACE_MAX,  &CWindowPage::OnPlacementChange)
    ON_BN_CLICKED(IDC_WIN_PLACE_SPEC, &CWindowPage::OnPlacementChange)
    ON_BN_CLICKED(IDC_WIN_SBAR_POS_BOTTOM, &CWindowPage::OnPlacementChange)
    ON_BN_CLICKED(IDC_WIN_SBAR_POS_TOP,    &CWindowPage::OnPlacementChange)
    ON_BN_CLICKED(IDC_WIN_SBAR_POS_FLOAT,  &CWindowPage::OnPlacementChange)
END_MESSAGE_MAP()

CWindowPage::CWindowPage() : CPropertyPage(IDD_SETTINGS_WINDOW) {}

void CWindowPage::ChargeFromSettings() {
    if (m_pS == nullptr) { return; }
    m_winPlacement    = (m_pS->winPlacement >= 0 && m_pS->winPlacement <= 3) ? m_pS->winPlacement : 1;
    m_winLeft   = m_pS->winLeft;
    m_winTop    = m_pS->winTop;
    m_winWidth  = m_pS->winWidth;
    m_winHeight = m_pS->winHeight;
    m_docMaximize   = m_pS->docMaximize ? TRUE : FALSE;
    m_docFullPath   = m_pS->docFullPath ? TRUE : FALSE;
    m_showToolbar   = m_pS->showToolbar ? TRUE : FALSE;
    m_showStatusbar = m_pS->showStatusbar ? TRUE : FALSE;
    m_bitImageDockable = m_pS->bitImageDockable ? TRUE : FALSE;
    m_structBarPos       = (m_pS->structBarPos >= 0 && m_pS->structBarPos <= 2) ? m_pS->structBarPos : 0;
    m_structBarNoDock    = m_pS->structBarNoDock ? TRUE : FALSE;
    m_structBarStatusPos = (m_pS->structBarStatusPos >= 0 && m_pS->structBarStatusPos <= 2) ? m_pS->structBarStatusPos : 2;
    m_structItemRatioKeep = m_pS->structItemRatioKeep ? TRUE : FALSE;
}

void CWindowPage::HarvestToSettings() {
    if (m_pS == nullptr) { return; }
    m_pS->winPlacement = m_winPlacement;
    m_pS->winLeft   = m_winLeft;
    m_pS->winTop    = m_winTop;
    m_pS->winWidth  = m_winWidth;
    m_pS->winHeight = m_winHeight;
    m_pS->docMaximize   = (m_docMaximize != FALSE);
    m_pS->docFullPath   = (m_docFullPath != FALSE);
    m_pS->showToolbar   = (m_showToolbar != FALSE);
    m_pS->showStatusbar = (m_showStatusbar != FALSE);
    m_pS->bitImageDockable = (m_bitImageDockable != FALSE);
    m_pS->structBarPos       = m_structBarPos;
    m_pS->structBarNoDock    = (m_structBarNoDock != FALSE);
    m_pS->structBarStatusPos = m_structBarStatusPos;
    m_pS->structItemRatioKeep = (m_structItemRatioKeep != FALSE);
}

void CWindowPage::DoDataExchange(CDataExchange* pDX) {
    CPropertyPage::DoDataExchange(pDX);
    DDX_Radio(pDX, IDC_WIN_PLACE_NONE, m_winPlacement);      // 1016..1019 → 0..3
    DDX_Text(pDX, IDC_WIN_LEFT, m_winLeft);
    DDX_Text(pDX, IDC_WIN_TOP, m_winTop);
    DDX_Text(pDX, IDC_WIN_WIDTH, m_winWidth);
    DDX_Text(pDX, IDC_WIN_HEIGHT, m_winHeight);
    DDX_Check(pDX, IDC_WIN_DOC_MAXIMIZE, m_docMaximize);
    DDX_Check(pDX, IDC_WIN_DOC_FULLPATH, m_docFullPath);
    DDX_Check(pDX, IDC_WIN_SHOW_TOOLBAR, m_showToolbar);
    DDX_Check(pDX, IDC_WIN_SHOW_STATUSBAR, m_showStatusbar);
    DDX_Check(pDX, IDC_WIN_BITIMAGE_DOCK, m_bitImageDockable);
    DDX_Radio(pDX, IDC_WIN_SBAR_POS_BOTTOM, m_structBarPos);   // 1044..1046 → 0..2
    DDX_Check(pDX, IDC_WIN_SBAR_NODOCK, m_structBarNoDock);
    DDX_Radio(pDX, IDC_WIN_SBAR_ST_BOTTOM, m_structBarStatusPos);  // 1147/1148/1049 → 0..2
    DDX_Check(pDX, IDC_WIN_SITEM_RATIO, m_structItemRatioKeep);
}

BOOL CWindowPage::OnInitDialog() {
    ChargeFromSettings();
    CPropertyPage::OnInitDialog();
    UpdateEnableState();
    return TRUE;
}

BOOL CWindowPage::OnKillActive() {
    if (!CPropertyPage::OnKillActive()) {
        return FALSE;
    }
    HarvestToSettings();
    return TRUE;
}

// 「指定」選択時のみ位置エディットを、構造体バーがフローティングのときのみ
// 「ドッキング不能」を有効化する。
void CWindowPage::UpdateEnableState() {
    const BOOL spec = (IsDlgButtonChecked(IDC_WIN_PLACE_SPEC) != 0);
    const UINT ids[] = { IDC_WIN_LEFT, IDC_WIN_TOP, IDC_WIN_WIDTH, IDC_WIN_HEIGHT };
    for (UINT id : ids) {
        if (CWnd* w = GetDlgItem(id)) { w->EnableWindow(spec); }
    }
    const BOOL floating = (IsDlgButtonChecked(IDC_WIN_SBAR_POS_FLOAT) != 0);
    if (CWnd* noDock = GetDlgItem(IDC_WIN_SBAR_NODOCK)) {
        noDock->EnableWindow(floating);
    }
}

void CWindowPage::OnPlacementChange() { UpdateEnableState(); }

// ===========================================================================
// CToolBarPage
// ===========================================================================
// ツールバー機能カタログ（原 DAT_004b6c90 の8カテゴリ×表示順。名称=文字列4000-58xx）。
//   rawID = (カテゴリ<<8)|項目番号。名称は原スクリーンショット/文字列と一致。
namespace {
struct STbItem { UINT raw; };   // 名称は文字列リソース（ui::CommandNameW）
struct STbCat  { const STbItem* items; int count; };   // カテゴリ名は 4000+索引

const STbItem kTbCat0[] = {   // ファイル系
    {0x0001},{0x0002},{0x0003},{0x0004},
    {0x0005},{0x0010},{0x0006},
    {0x0008},{0x000A},{0x000B},{0x0011},
    {0x000F},
};
const STbItem kTbCat1[] = {   // カーソル移動系
    {0x0104},{0x0105},{0x0110},
    {0x0111},
};
const STbItem kTbCat2[] = {   // 選択系
    {0x0205},{0x0206},{0x0209},
};
const STbItem kTbCat3[] = {   // 編集系
    {0x0300},{0x0301},{0x0302},{0x0303},
    {0x0304},{0x0305},{0x0306},{0x0307},
    {0x030A},{0x030B},{0x030D},{0x030E},
};
const STbItem kTbCat4[] = {   // 検索・置換系
    {0x0400},{0x0404},{0x0408},{0x0409},
    {0x040A},{0x040B},{0x040C},
};
const STbItem kTbCat6[] = {   // ウィンドウ系
    {0x0600},{0x0601},{0x0602},
    {0x0603},{0x0604},{0x0605},
    {0x0606},{0x0607},
};
const STbItem kTbCat7[] = {   // その他
    {0x0700},{0x070B},{0x070C},
    {0x0701},{0x0702},{0x0704},
    {0x0705},{0x0709},
    {0x070D},   // 16進テキスト貼り付け（移植で追加。Issue #97）
};
const STbCat kTbCatalog[] = {
    { kTbCat0, _countof(kTbCat0) },
    { kTbCat1, _countof(kTbCat1) },
    { kTbCat2, _countof(kTbCat2) },
    { kTbCat3, _countof(kTbCat3) },
    { kTbCat4, _countof(kTbCat4) },
    { nullptr, 0 },                                       // メニュー系: 原カタログでは項目なし
    { kTbCat6, _countof(kTbCat6) },
    { kTbCat7, _countof(kTbCat7) },
};
CStringW TbItemName(UINT raw) { return ui::CommandNameW(raw); }

// --- オーナードロー描画の実測値（原 Stirling.exe の画素採取による） -------------
//   項目高 19 = 枠2 + アイコン15 + 枠2。左端に幅22の凸枠セル（EDGE_RAISED/BF_RECT）を置き、
//   セル内部を COLOR_3DFACE で塗り、アイコンをセル左上から (3,3) の位置へ透過描画する
//   （原はアイコン下端1行が枠の内側影と重なる。実測どおり再現）。
//   テキストは項目左端 +24、上端 +3 に描画。セパレータ項目は枠・アイコンを描かず全面が背景色。
constexpr int kTbIconW    = 16;   // 原リソース128の1コマの大きさ
constexpr int kTbIconH    = 15;
constexpr int kTbCellW    = 2 + (kTbIconW + 2) + 2;   // アイコンセルの幅 = 22
constexpr int kTbItemH    = 2 + kTbIconH + 2;         // 項目高 = 19
constexpr int kTbIconOfs  = 3;    // セル左上からのアイコン描画位置
constexpr int kTbTextLeft = 24;   // 項目左端からのテキスト描画位置
constexpr int kTbTextTop  = 3;

// リスト項目を「文字列なしオーナードロー」として追加する（原と同じく rawID を項目データに持つ）。
//   LBS_HASSTRINGS を持たないため LB_ADDSTRING/LB_INSERTSTRING の lParam がそのまま項目データ。
int TbAdd(CListBox* lb, UINT raw) {
    return (int)lb->SendMessage(LB_ADDSTRING, 0, (LPARAM)raw);
}
int TbInsert(CListBox* lb, int at, UINT raw) {
    return (int)lb->SendMessage(LB_INSERTSTRING, (WPARAM)at, (LPARAM)raw);
}
}

BEGIN_MESSAGE_MAP(CToolBarPage, CPropertyPage)
    ON_WM_MEASUREITEM()
    ON_WM_DRAWITEM()
    ON_CBN_SELCHANGE(IDC_TBAR_CATEGORY, &CToolBarPage::OnCategoryChange)
    ON_BN_CLICKED(IDC_TBAR_ADD, &CToolBarPage::OnAdd)
    ON_BN_CLICKED(IDC_TBAR_SEPARATOR, &CToolBarPage::OnSeparator)
    ON_BN_CLICKED(IDC_TBAR_DELETE, &CToolBarPage::OnDelete)
    ON_BN_CLICKED(IDC_TBAR_UP, &CToolBarPage::OnUp)
    ON_BN_CLICKED(IDC_TBAR_DOWN, &CToolBarPage::OnDown)
    ON_LBN_SELCHANGE(IDC_TBAR_CURRENT, &CToolBarPage::OnSelChange)
    ON_LBN_SELCHANGE(IDC_TBAR_AVAILABLE, &CToolBarPage::OnSelChange)
END_MESSAGE_MAP()

CToolBarPage::CToolBarPage() : CPropertyPage(IDD_SETTINGS_TOOLBAR) {}

void CToolBarPage::FillCategory() {
    CComboBox* cb = (CComboBox*)GetDlgItem(IDC_TBAR_CATEGORY);
    if (cb == nullptr) { return; }
    cb->ResetContent();
    for (int i = 0; i < (int)_countof(kTbCatalog); ++i) {
        cb->AddString(ui::CommandCategoryNameW(i));   // 原 文字列 4000-4007
    }
    cb->SetCurSel(0);
}

// 現在の構成にコマンド raw が含まれるか（セパレータは対象外）。
static bool TbCurrentHas(CListBox* cur, UINT raw) {
    if (cur == nullptr) { return false; }
    const int n = cur->GetCount();
    for (int i = 0; i < n; ++i) { if ((UINT)cur->GetItemData(i) == raw) { return true; } }
    return false;
}

void CToolBarPage::RefillAvailable(UINT preferRaw, bool keepIndex) {
    CListBox* lb = (CListBox*)GetDlgItem(IDC_TBAR_AVAILABLE);
    CComboBox* cb = (CComboBox*)GetDlgItem(IDC_TBAR_CATEGORY);
    CListBox* cur = (CListBox*)GetDlgItem(IDC_TBAR_CURRENT);
    if (lb == nullptr || cb == nullptr) { return; }
    const int prevSel = keepIndex ? lb->GetCurSel() : 0;
    lb->ResetContent();
    const int cat = cb->GetCurSel();
    if (cat < 0 || cat >= (int)_countof(kTbCatalog)) { return; }
    const STbCat& c = kTbCatalog[cat];
    int preferIndex = -1;
    for (int i = 0; i < c.count; ++i) {
        if (TbCurrentHas(cur, c.items[i].raw)) { continue; }   // 追加済みは除外
        const int idx = TbAdd(lb, c.items[i].raw);
        if (idx >= 0 && c.items[i].raw == preferRaw) { preferIndex = idx; }
    }
    const int count = lb->GetCount();
    if (count <= 0) { return; }
    int sel = (preferIndex >= 0) ? preferIndex : ((prevSel > 0) ? prevSel : 0);
    if (sel >= count) { sel = count - 1; }
    lb->SetCurSel(sel);   // 原は常にいずれかの項目を選択状態にする
}

void CToolBarPage::InsertCurrent(int at, UINT raw, bool select) {
    CListBox* cur = (CListBox*)GetDlgItem(IDC_TBAR_CURRENT);
    if (cur == nullptr) { return; }
    const int idx = TbInsert(cur, at, raw);
    if (idx >= 0 && select) { cur->SetCurSel(idx); }
}

// StirHex 独自生成リソース128（ツールバー用ビットマップ 16x15×55）を
// C0C0C0 透過のイメージリストに読み込む。
CImageList& CToolBarPage::Icons() {
    if (m_icons.GetSafeHandle() == nullptr) {
        m_icons.Create(IDR_MAINFRAME, kTbIconW, 0, RGB(192, 192, 192));
    }
    return m_icons;
}

// 原は全項目一律 19（LBS_OWNERDRAWVARIABLE だが高さは可変ではない）。
void CToolBarPage::OnMeasureItem(int nIDCtl, LPMEASUREITEMSTRUCT lpMIS) {
    if (nIDCtl == IDC_TBAR_CURRENT || nIDCtl == IDC_TBAR_AVAILABLE) {
        lpMIS->itemHeight = kTbItemH;
        return;
    }
    CPropertyPage::OnMeasureItem(nIDCtl, lpMIS);
}

void CToolBarPage::OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT lpDIS) {
    if (nIDCtl != IDC_TBAR_CURRENT && nIDCtl != IDC_TBAR_AVAILABLE) {
        CPropertyPage::OnDrawItem(nIDCtl, lpDIS);
        return;
    }
    CDC* pDC = CDC::FromHandle(lpDIS->hDC);
    CRect rc(lpDIS->rcItem);
    if (lpDIS->itemID == (UINT)-1) {   // 空リスト: フォーカス枠のみ
        if ((lpDIS->itemAction & ODA_FOCUS) != 0) { pDC->DrawFocusRect(&rc); }
        return;
    }

    const UINT raw = (UINT)lpDIS->itemData;
    const bool isSep = (raw == CAppSettings::kToolbarSep);
    const bool selected = (lpDIS->itemState & ODS_SELECTED) != 0;
    const COLORREF back = ::GetSysColor(selected ? COLOR_HIGHLIGHT : COLOR_WINDOW);
    const COLORREF fore = ::GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT);

    // セパレータ以外は左端に凸枠のアイコンセルを描く。背景（選択色）はその右側だけ。
    CRect rcText(rc);
    if (!isSep) {
        CRect cell(rc.left, rc.top, rc.left + kTbCellW, rc.bottom);
        pDC->FillSolidRect(&cell, ::GetSysColor(COLOR_3DFACE));
        pDC->DrawEdge(&cell, EDGE_RAISED, BF_RECT);
        const int image = ToolbarRawToImage(raw);
        if (image >= 0) {
            Icons().Draw(pDC, image, CPoint(rc.left + kTbIconOfs, rc.top + kTbIconOfs),
                         ILD_TRANSPARENT);
        }
        rcText.left += kTbCellW;
    }
    pDC->FillSolidRect(&rcText, back);

    const CStringW label = isSep ? ui::LoadW(IDS_SEPARATOR_ITEM) : TbItemName(raw);
    const int oldMode = pDC->SetBkMode(TRANSPARENT);
    const COLORREF oldColor = pDC->SetTextColor(fore);
    const COLORREF oldBack = pDC->SetBkColor(back);   // フォーカス枠の点の色はDCの前景/背景色で決まる
    pDC->TextOut(rc.left + kTbTextLeft, rc.top + kTbTextTop, label);
    if ((lpDIS->itemState & ODS_FOCUS) != 0) {
        // フォーカス枠は市松模様の XOR 描画で、点の色はDCの前景/背景色で決まる。
        //   原と同じ「黒/白」の既定色に戻してから描く（＝選択色上では反転色の点になる）。
        pDC->SetTextColor(RGB(0, 0, 0));
        pDC->SetBkColor(RGB(255, 255, 255));
        pDC->DrawFocusRect(&rcText);
    }
    pDC->SetBkColor(oldBack);
    pDC->SetTextColor(oldColor);
    pDC->SetBkMode(oldMode);
}

void CToolBarPage::RefillCurrent() {
    CListBox* cur = (CListBox*)GetDlgItem(IDC_TBAR_CURRENT);
    if (cur == nullptr || m_pS == nullptr) { return; }
    cur->ResetContent();
    for (UINT raw : m_pS->toolbarItems) { InsertCurrent(cur->GetCount(), raw, false); }
    if (cur->GetCount() > 0) { cur->SetCurSel(0); }   // 原は初期表示で先頭を選択する
}

void CToolBarPage::HarvestToSettings() {
    CListBox* cur = (CListBox*)GetDlgItem(IDC_TBAR_CURRENT);
    if (cur == nullptr || m_pS == nullptr) { return; }
    m_pS->toolbarItems.clear();
    const int n = cur->GetCount();
    for (int i = 0; i < n; ++i) { m_pS->toolbarItems.push_back((UINT)cur->GetItemData(i)); }
}

BOOL CToolBarPage::OnInitDialog() {
    CPropertyPage::OnInitDialog();
    FillCategory();
    RefillCurrent();
    RefillAvailable();
    UpdateButtons();
    return TRUE;
}

BOOL CToolBarPage::OnKillActive() {
    if (!CPropertyPage::OnKillActive()) { return FALSE; }
    HarvestToSettings();
    return TRUE;
}

void CToolBarPage::UpdateButtons() {
    CListBox* cur = (CListBox*)GetDlgItem(IDC_TBAR_CURRENT);
    CListBox* avail = (CListBox*)GetDlgItem(IDC_TBAR_AVAILABLE);
    const int curSel = (cur != nullptr) ? cur->GetCurSel() : LB_ERR;
    const int availSel = (avail != nullptr) ? avail->GetCurSel() : LB_ERR;
    const int curCount = (cur != nullptr) ? cur->GetCount() : 0;
    auto en = [this](UINT id, BOOL on) { if (CWnd* w = GetDlgItem(id)) { w->EnableWindow(on); } };
    en(IDC_TBAR_ADD, availSel != LB_ERR);
    en(IDC_TBAR_DELETE, curSel != LB_ERR);
    en(IDC_TBAR_UP, curSel != LB_ERR && curSel > 0);
    en(IDC_TBAR_DOWN, curSel != LB_ERR && curSel < curCount - 1);
}

void CToolBarPage::OnSelChange() { UpdateButtons(); }
void CToolBarPage::OnCategoryChange() { RefillAvailable(); UpdateButtons(); }

void CToolBarPage::OnAdd() {
    CListBox* avail = (CListBox*)GetDlgItem(IDC_TBAR_AVAILABLE);
    CListBox* cur = (CListBox*)GetDlgItem(IDC_TBAR_CURRENT);
    if (avail == nullptr || cur == nullptr) { return; }
    const int sel = avail->GetCurSel();
    if (sel == LB_ERR) { return; }
    const UINT raw = (UINT)avail->GetItemData(sel);
    const int at = cur->GetCurSel();
    InsertCurrent((at == LB_ERR) ? cur->GetCount() : at + 1, raw, true);
    RefillAvailable(0, true);   // 追加済みは追加候補から除外（選択位置は維持）
    UpdateButtons();
}

void CToolBarPage::OnSeparator() {
    CListBox* cur = (CListBox*)GetDlgItem(IDC_TBAR_CURRENT);
    if (cur == nullptr) { return; }
    const int at = cur->GetCurSel();
    InsertCurrent((at == LB_ERR) ? cur->GetCount() : at + 1, CAppSettings::kToolbarSep, true);
    UpdateButtons();
}

void CToolBarPage::OnDelete() {
    CListBox* cur = (CListBox*)GetDlgItem(IDC_TBAR_CURRENT);
    if (cur == nullptr) { return; }
    const int sel = cur->GetCurSel();
    if (sel == LB_ERR) { return; }
    const UINT removed = (UINT)cur->GetItemData(sel);
    cur->DeleteString(sel);
    const int cnt = cur->GetCount();
    if (cnt > 0) { cur->SetCurSel((sel < cnt) ? sel : cnt - 1); }
    // 削除したコマンドは追加候補へ戻り、原はその復活項目を選択状態にする。
    RefillAvailable(removed, true);
    UpdateButtons();
}

void CToolBarPage::OnUp() {
    CListBox* cur = (CListBox*)GetDlgItem(IDC_TBAR_CURRENT);
    if (cur == nullptr) { return; }
    const int sel = cur->GetCurSel();
    if (sel == LB_ERR || sel == 0) { return; }
    const UINT raw = (UINT)cur->GetItemData(sel);
    cur->DeleteString(sel);
    InsertCurrent(sel - 1, raw, true);
    UpdateButtons();
}

void CToolBarPage::OnDown() {
    CListBox* cur = (CListBox*)GetDlgItem(IDC_TBAR_CURRENT);
    if (cur == nullptr) { return; }
    const int sel = cur->GetCurSel();
    if (sel == LB_ERR || sel >= cur->GetCount() - 1) { return; }
    const UINT raw = (UINT)cur->GetItemData(sel);
    cur->DeleteString(sel);
    InsertCurrent(sel + 1, raw, true);
    UpdateButtons();
}

// ===========================================================================
// CStatusBarPage
// ===========================================================================
// ステータスバー項目カタログ（原 DAT_004b6430 の 20項目。{コマンドID, 名称}）。
namespace {
struct SStatusItem { UINT id; };   // 名称は文字列 IDS_SBAR_ITEM_BASE からカタログ順に連番
const SStatusItem kStatusCatalog[] = {
    {0xE701},
    {0xE702},
    {0xE703},
    {0xE704},
    {0xE707},
    {0xE708},
    {0xE70A},
    {0xE712},
    {0xE70E},
    {0xE709},
    {0xE70F},
    {0xE713},
    {0xE710},
    {0xE70B},
    {0xE711},
    {0xE70C},
    {0xE714},
    {0xE715},
    {0xE70D},
    {0xE716},
};
CStringW StatusItemName(UINT id) {
    for (int i = 0; i < (int)_countof(kStatusCatalog); ++i) {
        if (kStatusCatalog[i].id == id) { return ui::LoadW(IDS_SBAR_ITEM_BASE + i); }
    }
    return CStringW();
}
}

BEGIN_MESSAGE_MAP(CStatusBarPage, CPropertyPage)
    ON_BN_CLICKED(IDC_SBAR_ADD, &CStatusBarPage::OnAdd)
    ON_BN_CLICKED(IDC_SBAR_DELETE, &CStatusBarPage::OnDelete)
    ON_BN_CLICKED(IDC_SBAR_UP, &CStatusBarPage::OnUp)
    ON_BN_CLICKED(IDC_SBAR_DOWN, &CStatusBarPage::OnDown)
    ON_LBN_SELCHANGE(IDC_SBAR_CURRENT, &CStatusBarPage::OnSelChange)
    ON_LBN_SELCHANGE(IDC_SBAR_AVAILABLE, &CStatusBarPage::OnSelChange)
END_MESSAGE_MAP()

CStatusBarPage::CStatusBarPage() : CPropertyPage(IDD_SETTINGS_STATUSBAR) {}

// 現在の構成リストに指定IDが含まれるか。
static bool CurrentHasId(CListBox* cur, UINT id) {
    if (cur == nullptr) { return false; }
    const int n = cur->GetCount();
    for (int i = 0; i < n; ++i) {
        if ((UINT)cur->GetItemData(i) == id) { return true; }
    }
    return false;
}

// 追加できる項目 = カタログ − 現在の構成。現在リスト確定後に呼ぶこと。
void CStatusBarPage::FillAvailable() {
    CListBox* lb = (CListBox*)GetDlgItem(IDC_SBAR_AVAILABLE);
    CListBox* cur = (CListBox*)GetDlgItem(IDC_SBAR_CURRENT);
    if (lb == nullptr) { return; }
    lb->ResetContent();
    for (const auto& it : kStatusCatalog) {
        if (CurrentHasId(cur, it.id)) { continue; }   // 既に構成済みは除外
        const int idx = lb->AddString(StatusItemName(it.id));
        if (idx >= 0) { lb->SetItemData(idx, it.id); }
    }
}

void CStatusBarPage::RefillCurrent() {
    CListBox* lb = (CListBox*)GetDlgItem(IDC_SBAR_CURRENT);
    if (lb == nullptr || m_pS == nullptr) { return; }
    lb->ResetContent();
    for (UINT id : m_pS->statusItems) {
        const int idx = lb->AddString(StatusItemName(id));
        if (idx >= 0) { lb->SetItemData(idx, id); }
    }
}

void CStatusBarPage::HarvestToSettings() {
    CListBox* lb = (CListBox*)GetDlgItem(IDC_SBAR_CURRENT);
    if (lb == nullptr || m_pS == nullptr) { return; }
    m_pS->statusItems.clear();
    const int n = lb->GetCount();
    for (int i = 0; i < n; ++i) {
        m_pS->statusItems.push_back((UINT)lb->GetItemData(i));
    }
}

BOOL CStatusBarPage::OnInitDialog() {
    CPropertyPage::OnInitDialog();
    RefillCurrent();     // 先に現在の構成を確定
    FillAvailable();     // その後に「カタログ − 現在」で追加候補を構築
    UpdateButtons();
    return TRUE;
}

BOOL CStatusBarPage::OnKillActive() {
    if (!CPropertyPage::OnKillActive()) {
        return FALSE;
    }
    HarvestToSettings();
    return TRUE;
}

void CStatusBarPage::UpdateButtons() {
    CListBox* cur = (CListBox*)GetDlgItem(IDC_SBAR_CURRENT);
    CListBox* avail = (CListBox*)GetDlgItem(IDC_SBAR_AVAILABLE);
    const int curSel = (cur != nullptr) ? cur->GetCurSel() : LB_ERR;
    const int availSel = (avail != nullptr) ? avail->GetCurSel() : LB_ERR;
    const int curCount = (cur != nullptr) ? cur->GetCount() : 0;
    auto en = [this](UINT id, BOOL on) { if (CWnd* w = GetDlgItem(id)) { w->EnableWindow(on); } };
    en(IDC_SBAR_ADD, availSel != LB_ERR);
    en(IDC_SBAR_DELETE, curSel != LB_ERR);
    en(IDC_SBAR_UP, curSel != LB_ERR && curSel > 0);
    en(IDC_SBAR_DOWN, curSel != LB_ERR && curSel < curCount - 1);
}

void CStatusBarPage::OnSelChange() { UpdateButtons(); }

void CStatusBarPage::OnAdd() {
    CListBox* avail = (CListBox*)GetDlgItem(IDC_SBAR_AVAILABLE);
    CListBox* cur = (CListBox*)GetDlgItem(IDC_SBAR_CURRENT);
    if (avail == nullptr || cur == nullptr) { return; }
    const int sel = avail->GetCurSel();
    if (sel == LB_ERR) { return; }
    const UINT id = (UINT)avail->GetItemData(sel);
    const int at = cur->GetCurSel();
    const int insertAt = (at == LB_ERR) ? cur->GetCount() : at + 1;
    const int idx = cur->InsertString(insertAt, StatusItemName(id));
    if (idx >= 0) { cur->SetItemData(idx, id); cur->SetCurSel(idx); }
    // 追加済みは追加候補から除外（同じ項目の重複追加を防ぐ）。
    FillAvailable();
    const int ac = avail->GetCount();
    if (ac > 0) { avail->SetCurSel((sel < ac) ? sel : ac - 1); }
    UpdateButtons();
}

void CStatusBarPage::OnDelete() {
    CListBox* cur = (CListBox*)GetDlgItem(IDC_SBAR_CURRENT);
    if (cur == nullptr) { return; }
    const int sel = cur->GetCurSel();
    if (sel == LB_ERR) { return; }
    cur->DeleteString(sel);
    const int cnt = cur->GetCount();
    if (cnt > 0) { cur->SetCurSel((sel < cnt) ? sel : cnt - 1); }
    // 削除した項目は追加候補へ戻す（カタログ順で再構築）。
    FillAvailable();
    UpdateButtons();
}

// 現在リスト内で選択項目を上下に入れ替える。
void CStatusBarPage::OnUp() {
    CListBox* cur = (CListBox*)GetDlgItem(IDC_SBAR_CURRENT);
    if (cur == nullptr) { return; }
    const int sel = cur->GetCurSel();
    if (sel == LB_ERR || sel == 0) { return; }
    const UINT id = (UINT)cur->GetItemData(sel);
    cur->DeleteString(sel);
    const int idx = cur->InsertString(sel - 1, StatusItemName(id));
    if (idx >= 0) { cur->SetItemData(idx, id); cur->SetCurSel(idx); }
    UpdateButtons();
}

void CStatusBarPage::OnDown() {
    CListBox* cur = (CListBox*)GetDlgItem(IDC_SBAR_CURRENT);
    if (cur == nullptr) { return; }
    const int sel = cur->GetCurSel();
    if (sel == LB_ERR || sel >= cur->GetCount() - 1) { return; }
    const UINT id = (UINT)cur->GetItemData(sel);
    cur->DeleteString(sel);
    const int idx = cur->InsertString(sel + 1, StatusItemName(id));
    if (idx >= 0) { cur->SetItemData(idx, id); cur->SetCurSel(idx); }
    UpdateButtons();
}

// ===========================================================================
// CUserMenuPage
// ===========================================================================
// ユーザーメニュー機能カタログ（原 FUN_0042b225 の全カテゴリ・フルコマンド集合。名称=文字列4000-58xx）。
//   rawID = (カテゴリ<<8)|項目番号。toolbar は部分集合だがメニューは全項目を持つ。
namespace {
const STbItem kUmCat0[] = {   // ファイル系（表示順 DAT_004b5424 cat0: 1,2,3,4,5,16,6,7,8,9,10,11,17,12,13,14,15）
    {0x0001},{0x0002},{0x0003},{0x0004},
    {0x0005},{0x0010},{0x0006},
    {0x0007},{0x0008},{0x0009},{0x000A},
    {0x000B},{0x0011},{0x000C},
    {0x000D},{0x000E},{0x000F},
};
const STbItem kUmCat1[] = {   // カーソル移動系（5200+item, item 0..17）
    {0x0100},{0x0101},{0x0102},{0x0103},
    {0x0104},{0x0105},{0x0106},{0x0107},
    {0x0108},{0x0109},{0x010A},{0x010B},
    {0x010C},{0x010D},{0x010E},{0x010F},
    {0x0110},{0x0111},
};
const STbItem kUmCat2[] = {   // 選択系（表示順 DAT_004b5424 cat2: 0..8,10,11,12,9）
    {0x0200},{0x0201},{0x0202},{0x0203},
    {0x0204},{0x0205},{0x0206},{0x0207},
    {0x0208},{0x020A},{0x020B},
    {0x020C},{0x0209},
};
const STbItem kUmCat3[] = {   // 編集系（5400+item, item 0..15）
    {0x0300},{0x0301},{0x0302},{0x0303},
    {0x0304},{0x0305},{0x0306},{0x0307},
    {0x0308},{0x0309},{0x030A},{0x030B},
    {0x030C},{0x030D},{0x030E},{0x030F},
};
const STbItem kUmCat4[] = {   // 検索・置換系（5500+item, item 0..13）
    {0x0400},{0x0401},{0x0402},{0x0403},
    {0x0404},{0x0405},{0x0406},
    {0x0407},{0x0408},{0x0409},{0x040A},
    {0x040B},{0x040C},{0x040D},
};
const STbItem kUmCat5[] = {   // メニュー系（5600+item, item 0..12）
    {0x0500},{0x0501},{0x0502},
    {0x0503},{0x0504},{0x0505},
    {0x0506},{0x0507},{0x0508},
    {0x0509},{0x050A},{0x050B},{0x050C},
};
const STbItem kUmCat6[] = {   // ウィンドウ系（5700+item, item 0..7）
    {0x0600},{0x0601},{0x0602},{0x0603},
    {0x0604},{0x0605},{0x0606},{0x0607},
};
const STbItem kUmCat7[] = {   // その他（表示順 DAT_004b5424 cat7: 0,11,12,1,2,3,4,5,6,7,8,10,9）
    {0x0700},{0x070B},{0x070C},
    {0x0701},{0x0702},{0x0703},
    {0x0704},{0x0705},{0x0706},{0x0707},
    {0x0708},{0x070A},{0x0709},
    {0x070D},   // 16進テキスト貼り付け（移植で追加。Issue #97）
};
const STbCat kUmCatalog[] = {
    { kUmCat0, _countof(kUmCat0) },
    { kUmCat1, _countof(kUmCat1) },
    { kUmCat2, _countof(kUmCat2) },
    { kUmCat3, _countof(kUmCat3) },
    { kUmCat4, _countof(kUmCat4) },
    { kUmCat5, _countof(kUmCat5) },
    { kUmCat6, _countof(kUmCat6) },
    { kUmCat7, _countof(kUmCat7) },
};
CStringW UmItemName(UINT raw) { return ui::CommandNameW(raw); }
}

BEGIN_MESSAGE_MAP(CUserMenuPage, CPropertyPage)
    ON_CBN_SELCHANGE(IDC_UM_MENUSET, &CUserMenuPage::OnMenuSetChange)
    ON_CBN_SELCHANGE(IDC_UM_CATEGORY, &CUserMenuPage::OnCategoryChange)
    ON_BN_CLICKED(IDC_UM_ADD, &CUserMenuPage::OnAdd)
    ON_BN_CLICKED(IDC_UM_SEPARATOR, &CUserMenuPage::OnSeparator)
    ON_BN_CLICKED(IDC_UM_DELETE, &CUserMenuPage::OnDelete)
    ON_BN_CLICKED(IDC_UM_UP, &CUserMenuPage::OnUp)
    ON_BN_CLICKED(IDC_UM_DOWN, &CUserMenuPage::OnDown)
    ON_LBN_SELCHANGE(IDC_UM_CURRENT, &CUserMenuPage::OnSelChange)
    ON_LBN_SELCHANGE(IDC_UM_AVAILABLE, &CUserMenuPage::OnSelChange)
    ON_LBN_DBLCLK(IDC_UM_CURRENT, &CUserMenuPage::OnCurrentDblClk)
END_MESSAGE_MAP()

CUserMenuPage::CUserMenuPage() : CPropertyPage(IDD_SETTINGS_USERMENU) {}

void CUserMenuPage::FillMenuSet() {
    CComboBox* cb = (CComboBox*)GetDlgItem(IDC_UM_MENUSET);
    if (cb == nullptr) { return; }
    cb->ResetContent();
    for (int i = 1; i <= 10; ++i) { CStringW s; s.Format(ui::LoadW(IDS_UM_MENU_FMT), i); cb->AddString(s); }
    for (int i = 1; i <= 3; ++i)  { CStringW s; s.Format(ui::LoadW(IDS_UM_TWOSTROKE_FMT), i); cb->AddString(s); }
    cb->AddString(ui::LoadW(IDS_UM_ESC_MENU));
    cb->AddString(ui::LoadW(IDS_UM_CONTEXT_MENU));
    cb->SetCurSel(m_curMenu);
}

void CUserMenuPage::FillCategory() {
    CComboBox* cb = (CComboBox*)GetDlgItem(IDC_UM_CATEGORY);
    if (cb == nullptr) { return; }
    cb->ResetContent();
    for (int i = 0; i < (int)_countof(kUmCatalog); ++i) {
        cb->AddString(ui::CommandCategoryNameW(i));   // 原 文字列 4000-4007
    }
    cb->SetCurSel(0);
}

static bool UmCurrentHas(CListBox* cur, UINT raw) {
    if (cur == nullptr) { return false; }
    const int n = cur->GetCount();
    // アイテムデータは (アクセラレータ<<16)|rawID。rawID 部分で比較する。
    for (int i = 0; i < n; ++i) {
        if (CAppSettings::UmRaw((UINT)cur->GetItemData(i)) == raw) { return true; }
    }
    return false;
}

void CUserMenuPage::RefillAvailable() {
    CListBox* lb = (CListBox*)GetDlgItem(IDC_UM_AVAILABLE);
    CComboBox* cb = (CComboBox*)GetDlgItem(IDC_UM_CATEGORY);
    CListBox* cur = (CListBox*)GetDlgItem(IDC_UM_CURRENT);
    if (lb == nullptr || cb == nullptr) { return; }
    lb->ResetContent();
    const int cat = cb->GetCurSel();
    if (cat < 0 || cat >= (int)_countof(kUmCatalog)) { return; }
    const STbCat& c = kUmCatalog[cat];
    for (int i = 0; i < c.count; ++i) {
        if (UmCurrentHas(cur, c.items[i].raw)) { continue; }
        const int idx = lb->AddString(ui::CommandNameW(c.items[i].raw));
        if (idx >= 0) { lb->SetItemData(idx, c.items[i].raw); }
    }
}

void CUserMenuPage::InsertCurrent(int at, UINT item, bool select) {
    CListBox* cur = (CListBox*)GetDlgItem(IDC_UM_CURRENT);
    if (cur == nullptr) { return; }
    const UINT raw = CAppSettings::UmRaw(item);
    CStringW label;
    if (raw == CAppSettings::kUserMenuSep) {
        label = ui::LoadW(IDS_SEPARATOR_ITEM);
    } else {
        // 「X  <名称>」形式でアクセラレータを併記（原編集リスト "%c\t" 相当）。
        const UINT accel = CAppSettings::UmAccel(item);
        if (accel != 0) { label.Format(L"%c  %s", (wchar_t)accel, UmItemName(raw).GetString()); }
        else            { label = UmItemName(raw); }
    }
    const int idx = cur->InsertString(at, label);
    if (idx >= 0) { cur->SetItemData(idx, item); if (select) { cur->SetCurSel(idx); } }
}

void CUserMenuPage::RefillCurrent() {
    CListBox* cur = (CListBox*)GetDlgItem(IDC_UM_CURRENT);
    if (cur == nullptr || m_pS == nullptr) { return; }
    cur->ResetContent();
    if (m_curMenu < 0 || m_curMenu >= (int)m_pS->userMenus.size()) { return; }
    for (UINT raw : m_pS->userMenus[m_curMenu]) { InsertCurrent(cur->GetCount(), raw, false); }
}

void CUserMenuPage::HarvestCurrent() {
    CListBox* cur = (CListBox*)GetDlgItem(IDC_UM_CURRENT);
    if (cur == nullptr || m_pS == nullptr) { return; }
    if (m_curMenu < 0 || m_curMenu >= (int)m_pS->userMenus.size()) { return; }
    std::vector<UINT>& v = m_pS->userMenus[m_curMenu];
    v.clear();
    const int n = cur->GetCount();
    for (int i = 0; i < n; ++i) { v.push_back((UINT)cur->GetItemData(i)); }
}

BOOL CUserMenuPage::OnInitDialog() {
    CPropertyPage::OnInitDialog();
    m_curMenu = 0;
    FillMenuSet();
    FillCategory();
    RefillCurrent();
    RefillAvailable();
    UpdateButtons();
    return TRUE;
}

BOOL CUserMenuPage::OnKillActive() {
    if (!CPropertyPage::OnKillActive()) { return FALSE; }
    HarvestCurrent();
    return TRUE;
}

void CUserMenuPage::UpdateButtons() {
    CListBox* cur = (CListBox*)GetDlgItem(IDC_UM_CURRENT);
    CListBox* avail = (CListBox*)GetDlgItem(IDC_UM_AVAILABLE);
    const int curSel = (cur != nullptr) ? cur->GetCurSel() : LB_ERR;
    const int availSel = (avail != nullptr) ? avail->GetCurSel() : LB_ERR;
    const int curCount = (cur != nullptr) ? cur->GetCount() : 0;
    auto en = [this](UINT id, BOOL on) { if (CWnd* w = GetDlgItem(id)) { w->EnableWindow(on); } };
    en(IDC_UM_ADD, availSel != LB_ERR);
    en(IDC_UM_DELETE, curSel != LB_ERR);
    en(IDC_UM_UP, curSel != LB_ERR && curSel > 0);
    en(IDC_UM_DOWN, curSel != LB_ERR && curSel < curCount - 1);
}

void CUserMenuPage::OnSelChange() { UpdateButtons(); }
void CUserMenuPage::OnCategoryChange() { RefillAvailable(); UpdateButtons(); }

// メニュー設定切替: 現在の編集内容を保存してから新メニューを読み込む。
void CUserMenuPage::OnMenuSetChange() {
    CComboBox* cb = (CComboBox*)GetDlgItem(IDC_UM_MENUSET);
    if (cb == nullptr) { return; }
    const int sel = cb->GetCurSel();
    if (sel == LB_ERR || sel == m_curMenu) { return; }
    HarvestCurrent();       // 旧メニューを保存
    m_curMenu = sel;
    RefillCurrent();        // 新メニューを読み込み
    RefillAvailable();
    UpdateButtons();
}

void CUserMenuPage::OnAdd() {
    CListBox* avail = (CListBox*)GetDlgItem(IDC_UM_AVAILABLE);
    CListBox* cur = (CListBox*)GetDlgItem(IDC_UM_CURRENT);
    if (avail == nullptr || cur == nullptr) { return; }
    const int sel = avail->GetCurSel();
    if (sel == LB_ERR) { return; }
    const UINT raw = (UINT)avail->GetItemData(sel);   // モーダル前に退避（OnCurrentDblClk と同様）
    // 原と同じく、まずアクセラレータ指定ダイアログを出す（キャンセルで追加中止）。
    //   アクセラレータは 2 ストローク第 2 打鍵の照合とポップアップのニーモニックに使う。
    CAccelInputDlg dlg(this, 0, true);
    if (dlg.DoModal() != IDOK) { return; }
    const int at = cur->GetCurSel();
    InsertCurrent((at == LB_ERR) ? cur->GetCount() : at + 1,
                  CAppSettings::UmMake(dlg.ResultAccel(), raw), true);
    RefillAvailable();
    UpdateButtons();
}

// 項目のダブルクリックでアクセラレータを変更する（原 FUN_0042c462）。
//   セパレータはアクセラレータを持てないので対象外とする
//   （原は無条件に付与してセパレータ表示を壊していた）。
void CUserMenuPage::OnCurrentDblClk() {
    CListBox* cur = (CListBox*)GetDlgItem(IDC_UM_CURRENT);
    if (cur == nullptr) { return; }
    const int sel = cur->GetCurSel();
    if (sel == LB_ERR) { return; }
    const UINT item = (UINT)cur->GetItemData(sel);
    const UINT raw = CAppSettings::UmRaw(item);
    if (raw == CAppSettings::kUserMenuSep) { return; }
    CAccelInputDlg dlg(this, CAppSettings::UmAccel(item), false);
    if (dlg.DoModal() != IDOK) { return; }
    cur->DeleteString(sel);
    InsertCurrent(sel, CAppSettings::UmMake(dlg.ResultAccel(), raw), true);
    UpdateButtons();
}

void CUserMenuPage::OnSeparator() {
    CListBox* cur = (CListBox*)GetDlgItem(IDC_UM_CURRENT);
    if (cur == nullptr) { return; }
    const int at = cur->GetCurSel();
    InsertCurrent((at == LB_ERR) ? cur->GetCount() : at + 1, CAppSettings::kToolbarSep, true);
    UpdateButtons();
}

void CUserMenuPage::OnDelete() {
    CListBox* cur = (CListBox*)GetDlgItem(IDC_UM_CURRENT);
    if (cur == nullptr) { return; }
    const int sel = cur->GetCurSel();
    if (sel == LB_ERR) { return; }
    cur->DeleteString(sel);
    const int cnt = cur->GetCount();
    if (cnt > 0) { cur->SetCurSel((sel < cnt) ? sel : cnt - 1); }
    RefillAvailable();
    UpdateButtons();
}

void CUserMenuPage::OnUp() {
    CListBox* cur = (CListBox*)GetDlgItem(IDC_UM_CURRENT);
    if (cur == nullptr) { return; }
    const int sel = cur->GetCurSel();
    if (sel == LB_ERR || sel == 0) { return; }
    const UINT raw = (UINT)cur->GetItemData(sel);
    cur->DeleteString(sel);
    InsertCurrent(sel - 1, raw, true);
    UpdateButtons();
}

void CUserMenuPage::OnDown() {
    CListBox* cur = (CListBox*)GetDlgItem(IDC_UM_CURRENT);
    if (cur == nullptr) { return; }
    const int sel = cur->GetCurSel();
    if (sel == LB_ERR || sel >= cur->GetCount() - 1) { return; }
    const UINT raw = (UINT)cur->GetItemData(sel);
    cur->DeleteString(sel);
    InsertCurrent(sel + 1, raw, true);
    UpdateButtons();
}

// ===========================================================================
// CKeyAssignPage
// ===========================================================================
namespace {
// キー名（原の文字列表 5000+keycode。keycode 0..0x38）。
CStringW KaKeyName(int kc) {
    return (kc >= 0 && kc <= 0x38) ? ui::LoadW(5000 + kc) : CStringW();
}
// 修飾状態別のキーコード表（原 DAT_004b5ed0/5ef0/5f20）。
const int kKeysNone[]  = { 0,1,2,3,4,5,6,7,8,9,10,11, 0x2E,0x2F,0x36 };
const int kKeysShift[] = { 0,1,2,3,4,5,6,7,8,9,10,11, 0x2E,0x2F,0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38 };
// Ctrl / Ctrl+Shift は全57キー（0..0x38）。
// 機能名（usermenu フルカタログを共用。0=なし）。
CStringW KaFuncName(UINT raw) { return ui::CommandNameW(raw); }   // raw=0 は原 5100「なし」
}

BEGIN_MESSAGE_MAP(CKeyAssignPage, CPropertyPage)
    ON_BN_CLICKED(IDC_KA_CTRL, &CKeyAssignPage::OnModifierChange)
    ON_BN_CLICKED(IDC_KA_SHIFT, &CKeyAssignPage::OnModifierChange)
    ON_LBN_SELCHANGE(IDC_KA_KEYLIST, &CKeyAssignPage::OnKeySelChange)
    ON_CBN_SELCHANGE(IDC_KA_FUNC_CATEGORY, &CKeyAssignPage::OnFuncCategoryChange)
    ON_LBN_SELCHANGE(IDC_KA_FUNC_LIST, &CKeyAssignPage::OnFuncSelChange)
    ON_BN_CLICKED(IDC_KA_RESET, &CKeyAssignPage::OnResetKeymap)
    ON_BN_CLICKED(IDC_KA_LOAD, &CKeyAssignPage::OnLoad)
    ON_BN_CLICKED(IDC_KA_SAVE, &CKeyAssignPage::OnSave)
END_MESSAGE_MAP()

CKeyAssignPage::CKeyAssignPage() : CPropertyPage(IDD_KEYASSIGN) {}

void CKeyAssignPage::DoDataExchange(CDataExchange* pDX) {
    CPropertyPage::DoDataExchange(pDX);
    DDX_Check(pDX, IDC_KA_CTRL, m_ctrl);
    DDX_Check(pDX, IDC_KA_SHIFT, m_shift);
}

int CKeyAssignPage::ModState() const {
    int s = 0;
    if (IsDlgButtonChecked(IDC_KA_CTRL))  { s |= 2; }
    if (IsDlgButtonChecked(IDC_KA_SHIFT)) { s |= 1; }
    return s;
}

void CKeyAssignPage::RefillKeyList() {
    CListBox* lb = (CListBox*)GetDlgItem(IDC_KA_KEYLIST);
    if (lb == nullptr) { return; }
    lb->ResetContent();
    const int mod = ModState();
    // 修飾プレフィックス（原 s_Shift__/s_Ctrl__/s_Ctrl__Shift__）。
    CStringW prefix;
    if (mod == 1) { prefix = L"Shift + "; }
    else if (mod == 2) { prefix = L"Ctrl + "; }
    else if (mod == 3) { prefix = L"Ctrl + Shift + "; }
    // キーコード集合を選択。
    const int* keys; int n;
    if (mod == 0) { keys = kKeysNone; n = _countof(kKeysNone); }
    else if (mod == 1) { keys = kKeysShift; n = _countof(kKeysShift); }
    else { static int all[57]; for (int i = 0; i < 57; ++i) { all[i] = i; } keys = all; n = 57; }
    for (int i = 0; i < n; ++i) {
        const int kc = keys[i];
        const int idx = lb->AddString(prefix + KaKeyName(kc));
        if (idx >= 0) { lb->SetItemData(idx, (DWORD_PTR)kc); }
    }
}

void CKeyAssignPage::FillFuncCategory() {
    CComboBox* cb = (CComboBox*)GetDlgItem(IDC_KA_FUNC_CATEGORY);
    if (cb == nullptr) { return; }
    cb->ResetContent();
    for (int i = 0; i < (int)_countof(kUmCatalog); ++i) {
        cb->AddString(ui::CommandCategoryNameW(i));   // 原 文字列 4000-4007
    }
    cb->SetCurSel(0);
}

void CKeyAssignPage::RefillFuncList() {
    CListBox* lb = (CListBox*)GetDlgItem(IDC_KA_FUNC_LIST);
    CComboBox* cb = (CComboBox*)GetDlgItem(IDC_KA_FUNC_CATEGORY);
    if (lb == nullptr || cb == nullptr) { return; }
    lb->ResetContent();
    const int cat = cb->GetCurSel();
    if (cat < 0 || cat >= (int)_countof(kUmCatalog)) { return; }
    if (cat == 0) {   // ファイル系の先頭に「なし」（未割当）。
        const int idx = lb->AddString(ui::CommandNameW(0));   // 原 5100「なし」
        if (idx >= 0) { lb->SetItemData(idx, 0); }
    }
    const STbCat& c = kUmCatalog[cat];
    for (int i = 0; i < c.count; ++i) {
        const int idx = lb->AddString(ui::CommandNameW(c.items[i].raw));
        if (idx >= 0) { lb->SetItemData(idx, c.items[i].raw); }
    }
}

int CKeyAssignPage::CurKeyCode() const {
    CListBox* lb = (CListBox*)GetDlgItem(IDC_KA_KEYLIST);
    if (lb == nullptr) { return -1; }
    const int sel = lb->GetCurSel();
    if (sel == LB_ERR) { return -1; }
    return (int)lb->GetItemData(sel);
}

// 選択キーの割当機能を機能セレクタ（カテゴリ＋一覧）へ反映する。
void CKeyAssignPage::ShowAssignedFunc() {
    CComboBox* cb = (CComboBox*)GetDlgItem(IDC_KA_FUNC_CATEGORY);
    CListBox* fl = (CListBox*)GetDlgItem(IDC_KA_FUNC_LIST);
    if (cb == nullptr || fl == nullptr || m_pS == nullptr) { return; }
    const int kc = CurKeyCode();
    if (kc < 0) { return; }
    const int mod = ModState();
    const int idx = mod * 0x40 + kc;
    const UINT raw = (idx >= 0 && idx < (int)m_pS->keymap.size()) ? m_pS->keymap[idx] : 0;
    const int cat = (raw == 0) ? 0 : (int)((raw >> 8) & 0xFF);
    cb->SetCurSel((cat >= 0 && cat < (int)_countof(kUmCatalog)) ? cat : 0);
    RefillFuncList();
    // 一覧から該当項目を選択。
    for (int i = 0; i < fl->GetCount(); ++i) {
        if ((UINT)fl->GetItemData(i) == raw) { fl->SetCurSel(i); break; }
    }
}

BOOL CKeyAssignPage::OnInitDialog() {
    CPropertyPage::OnInitDialog();
    RefillKeyList();
    FillFuncCategory();
    RefillFuncList();
    return TRUE;
}

void CKeyAssignPage::OnModifierChange() { RefillKeyList(); }

void CKeyAssignPage::OnKeySelChange() { ShowAssignedFunc(); }

void CKeyAssignPage::OnFuncCategoryChange() { RefillFuncList(); }

// 機能選択→現在の（キー＋修飾）へ割当。
void CKeyAssignPage::OnFuncSelChange() {
    CListBox* fl = (CListBox*)GetDlgItem(IDC_KA_FUNC_LIST);
    if (fl == nullptr || m_pS == nullptr) { return; }
    const int kc = CurKeyCode();
    if (kc < 0) { return; }
    const int sel = fl->GetCurSel();
    if (sel == LB_ERR) { return; }
    const UINT raw = (UINT)fl->GetItemData(sel);
    const int idx = ModState() * 0x40 + kc;
    if (idx >= 0 && idx < (int)m_pS->keymap.size()) { m_pS->keymap[idx] = raw; }
}

void CKeyAssignPage::OnResetKeymap() {
    if (m_pS == nullptr) { return; }
    if (ui::MsgBoxRes(GetSafeHwnd(), IDS_KA_RESET_CONFIRM, MB_OKCANCEL | MB_ICONQUESTION) != IDOK) {
        return;
    }
    m_pS->ResetKeymapToDefault();
    ShowAssignedFunc();
}

void CKeyAssignPage::OnSave() {
    if (m_pS == nullptr) { return; }
    CFileDialog dlg(FALSE, _T("key"), _T("keymap.key"),
                    OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY, nullptr, this);
    if (dlg.DoModal() != IDOK) { return; }
    CFile f;
    if (!f.Open(dlg.GetPathName(), CFile::modeCreate | CFile::modeWrite)) {
        ui::MsgBoxRes(GetSafeHwnd(), IDS_KA_EXPORT_FAILED, MB_OK | MB_ICONERROR);
        return;
    }
    // 原形式に合わせ 256 × ushort（512バイト）で保存。
    for (int i = 0; i < CAppSettings::kKeymapSize; ++i) {
        const unsigned short v = (unsigned short)(m_pS->keymap[i] & 0xFFFF);
        f.Write(&v, sizeof(v));
    }
    f.Close();
}

void CKeyAssignPage::OnLoad() {
    if (m_pS == nullptr) { return; }
    CFileDialog dlg(TRUE, _T("key"), nullptr,
                    OFN_FILEMUSTEXIST | OFN_HIDEREADONLY, nullptr, this);
    if (dlg.DoModal() != IDOK) { return; }
    CFile f;
    if (!f.Open(dlg.GetPathName(), CFile::modeRead)) {
        ui::MsgBoxRes(GetSafeHwnd(), IDS_KA_IMPORT_FAILED, MB_OK | MB_ICONERROR);
        return;
    }
    if (f.GetLength() != CAppSettings::kKeymapSize * sizeof(unsigned short)) {
        ui::MsgBoxRes(GetSafeHwnd(), IDS_KA_FILE_INVALID, MB_OK | MB_ICONERROR);
        return;
    }
    for (int i = 0; i < CAppSettings::kKeymapSize; ++i) {
        unsigned short v = 0;
        f.Read(&v, sizeof(v));
        m_pS->keymap[i] = v;
    }
    f.Close();
    ShowAssignedFunc();
}

// ===========================================================================
// CEnvSheet
// ===========================================================================
BEGIN_MESSAGE_MAP(CEnvSheet, CPropertySheet)
END_MESSAGE_MAP()

CEnvSheet::CEnvSheet(const CAppSettings& src, CWnd* pParent)
    : CPropertySheet(ui::LoadW(IDS_ENV_SHEET_TITLE), pParent), m_s(src) {
    m_page1.m_pS = &m_s;
    m_page2.m_pS = &m_s;
    m_pageFile.m_pS = &m_s;
    m_pageWindow.m_pS = &m_s;
    m_pageKeyAssign.m_pS = &m_s;
    m_pageUserMenu.m_pS = &m_s;
    m_pageToolBar.m_pS = &m_s;
    m_pageStatusBar.m_pS = &m_s;
    AddPage(&m_page1);
    AddPage(&m_page2);
    AddPage(&m_pageFile);
    AddPage(&m_pageWindow);
    // 原のページ順（ウィンドウ→キーアサイン→ユーザーメニュー→ツールバー→ステータスバー）。
    AddPage(&m_pageKeyAssign);
    AddPage(&m_pageUserMenu);
    AddPage(&m_pageToolBar);
    AddPage(&m_pageStatusBar);
}
