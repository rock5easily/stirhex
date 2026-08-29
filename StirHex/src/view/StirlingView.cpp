// CStirlingView 実装。3カラム（アドレス／16進／文字）描画＋縦スクロール。
//   原 CStirlingView::OnDraw(0x0043ea2e) の描画モデルに準拠（doc 06/12）。
//   可視行のみ BlockCursor 経由で BlockList から直接読み取り、大容量でも軽量に描く。
//   キャレット/選択反転(PatBlt)/文字セット別レンダラ(this+0x344)/
//   バイト単位色(GetByteColor 0x45cf92)/マーク色を実装済み。
#include "pch.h"
#include "app/UiStrings.h"   // ui::MsgBox（表題はアプリ名で統一）
#include <afxpriv.h>   // CPreviewView / AFX_IDD_PREVIEW_TOOLBAR（全画面印刷プレビュー）
#include "resource.h"
#include "view/StirlingView.h"
#include "doc/StirlingDoc.h"
#include "app/StirlingApp.h"
#include "app/ShellUtil.h"   // ui::DragQueryPath（MAX_PATH 非依存のドロップパス取得）
#include "app/ClipboardUtil.h"   // クリップボード転送の RAII（#47）
#include "app/MarkFile.h"        // マークファイルの直列化（Issue #99）
#include "app/SettingsFile.h"    // UTF-8 テキストファイルの読み書き
#include "core/HexText.h"   // 16進テキスト → バイト列（Issue #97）
#include "core/Utf8Text.h"   // UTF-8 の復号・符号化（Issue #98）
#include "util/ScopedGdi.h"   // GDI オブジェクトの RAII（Issue #48）
#include "frame/MainFrame.h"
#include "frame/UserMenuCatalog.h"   // ユーザーメニュー構築（rawID→cmdID/名称・BuildUserPopup）
#include "dialog/FindDlg.h"
#include "dialog/FindMismatchDlg.h"
#include "dialog/ReplaceDlg.h"
#include "dialog/ReplaceConfirmDlg.h"
#include "dialog/MarkListDlg.h"
#include "dialog/JumpDlg.h"
#include "dialog/FillRangeDlg.h"
#include "dialog/SaveDumpDlg.h"
#include "dialog/PrintRangeDlg.h"
#include "dialog/SelectRangeDlg.h"
#include "dialog/RunDlg.h"
#include "dialog/FileChangedDlg.h"
#include "dialog/CompareDlg.h"
#include "dialog/DiffListDlg.h"
#include "dialog/SyncScrollDlg.h"
#include "core/BlockCursor.h"
#include "core/Cp932Text.h"   // IsCp932LeadByte（byte 層の CP932 先行バイト判定）

#include <algorithm>
#include <cctype>

#include <string>
#include <commctrl.h>   // SetWindowSubclass（プレビューツールバー固定用）

IMPLEMENT_DYNCREATE(CStirlingView, CView)

// ===========================================================================
// 印刷プレビュー用ビュー（原 CStirlingPreviewView 相当）
//   全画面プレビュー（メインフレームでホスト）では MFC のプレビューツールバーが
//   マウスでドラッグ移動できてしまうため、ツールバー窓をサブクラス化して
//   「移動を起こすメッセージ」を握り潰し、上端に固定する。
// ===========================================================================
static LRESULT CALLBACK PreviewToolBarSubclassProc(HWND hWnd, UINT msg, WPARAM wParam,
                                                   LPARAM lParam, UINT_PTR /*id*/, DWORD_PTR /*ref*/) {
    switch (msg) {
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
        if (wParam == HTCAPTION) { return 0; }   // キャプション相当のドラッグ/移動を無効化
        break;
    case WM_LBUTTONDOWN:                          // バー空き領域クリックでのドラッグ開始を無効化
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_MOVE) { return 0; }   // システムメニュー/矢印での移動を無効化
        break;
    default:
        break;
    }
    return ::DefSubclassProc(hWnd, msg, wParam, lParam);
}

class CStirlingPreviewView : public CPreviewView {
    DECLARE_DYNCREATE(CStirlingPreviewView)
protected:
    afx_msg int OnCreate(LPCREATESTRUCT lpCreateStruct);
    DECLARE_MESSAGE_MAP()
};

IMPLEMENT_DYNCREATE(CStirlingPreviewView, CPreviewView)

BEGIN_MESSAGE_MAP(CStirlingPreviewView, CPreviewView)
    ON_WM_CREATE()
END_MESSAGE_MAP()

int CStirlingPreviewView::OnCreate(LPCREATESTRUCT lpCreateStruct) {
    if (CPreviewView::OnCreate(lpCreateStruct) == -1) { return -1; }
    // プレビュービュー生成時点で m_pToolBar は既に生成済み（DoPrintPreview の順序）。
    if (m_pToolBar != nullptr && m_pToolBar->GetSafeHwnd() != nullptr) {
        ::SetWindowSubclass(m_pToolBar->GetSafeHwnd(), PreviewToolBarSubclassProc, 1, 0);
    }
    return 0;
}

BEGIN_MESSAGE_MAP(CStirlingView, CView)
    ON_WM_CREATE()
    ON_WM_DESTROY()
    ON_WM_SIZE()
    ON_WM_DROPFILES()
    ON_WM_ERASEBKGND()
    ON_WM_VSCROLL()
    ON_WM_HSCROLL()
    ON_WM_MOUSEWHEEL()
    ON_WM_CHAR()
    ON_WM_KEYDOWN()
    ON_WM_TIMER()
    ON_WM_LBUTTONDOWN()
    ON_WM_MOUSEMOVE()
    ON_WM_LBUTTONUP()
    ON_WM_SETFOCUS()
    ON_WM_KILLFOCUS()
    ON_WM_CONTEXTMENU()   // 右クリック→コンテキストメニュー(userMenus[14])
    // ユーザーメニュー/2ストローク呼出コマンド(0x803A-0x8046)→対応 userMenus をポップアップ
    ON_COMMAND_RANGE(ID_USERMENU_1, ID_TWOSTROKE_3, &CStirlingView::OnUserMenuInvoke)
    // 名前を指定して実行（0x804f）。ユーザーメニュー/キーアサインからはビューへ届くため、
    //   原と同じくビューとフレームの双方で受ける。
    ON_COMMAND(ID_RUN_APP, &CStirlingView::OnRunApp)
    ON_COMMAND(ID_EDIT_UNDO, &CStirlingView::OnEditUndo)
    ON_COMMAND(ID_EDIT_REDO, &CStirlingView::OnEditRedo)
    ON_UPDATE_COMMAND_UI(ID_EDIT_UNDO, &CStirlingView::OnUpdateEditUndo)
    ON_UPDATE_COMMAND_UI(ID_EDIT_REDO, &CStirlingView::OnUpdateEditRedo)
    ON_COMMAND(ID_EDIT_COPY, &CStirlingView::OnEditCopy)
    ON_COMMAND(ID_EDIT_CUT, &CStirlingView::OnEditCut)
    ON_COMMAND(ID_EDIT_PASTE, &CStirlingView::OnEditPaste)
    ON_COMMAND(ID_EDIT_PASTE_HEX, &CStirlingView::OnEditPasteHex)
    ON_UPDATE_COMMAND_UI(ID_EDIT_COPY, &CStirlingView::OnUpdateEditCopy)
    ON_UPDATE_COMMAND_UI(ID_EDIT_CUT, &CStirlingView::OnUpdateEditCut)
    ON_UPDATE_COMMAND_UI(ID_EDIT_PASTE, &CStirlingView::OnUpdateEditPaste)
    ON_UPDATE_COMMAND_UI(ID_EDIT_PASTE_HEX, &CStirlingView::OnUpdateEditPasteHex)
    ON_COMMAND_RANGE(ID_CHARSET_ASCII, ID_CHARSET_UNICODE, &CStirlingView::OnCharset)
    ON_COMMAND_RANGE(ID_CHARSET_EBCDIC, ID_CHARSET_EBCIDK, &CStirlingView::OnCharset)
    ON_COMMAND(ID_CHARSET_UTF8, &CStirlingView::OnCharsetUtf8)   // 移植で追加（Issue #98）
    ON_UPDATE_COMMAND_UI_RANGE(ID_CHARSET_ASCII, ID_CHARSET_UNICODE, &CStirlingView::OnUpdateCharset)
    ON_UPDATE_COMMAND_UI_RANGE(ID_CHARSET_EBCDIC, ID_CHARSET_EBCIDK, &CStirlingView::OnUpdateCharset)
    ON_UPDATE_COMMAND_UI(ID_CHARSET_UTF8, &CStirlingView::OnUpdateCharset)
    ON_COMMAND_RANGE(ID_BYTEORDER_LITTLE, ID_BYTEORDER_BIG, &CStirlingView::OnByteOrder)
    ON_UPDATE_COMMAND_UI_RANGE(ID_BYTEORDER_LITTLE, ID_BYTEORDER_BIG, &CStirlingView::OnUpdateByteOrder)
    ON_MESSAGE(WM_IME_CHAR, &CStirlingView::OnImeChar)
    // 外部プロセスによるファイル変更の確認（原 WM_USER+0x1B。OnActivateView から自身へポスト）。
    ON_MESSAGE(WM_STIRLING_CHECK_FILE, &CStirlingView::OnCheckFileChanged)
    ON_COMMAND(ID_MARK_TOGGLE, &CStirlingView::OnMarkToggle)
    ON_UPDATE_COMMAND_UI(ID_MARK_TOGGLE, &CStirlingView::OnUpdateMarkToggle)
    ON_COMMAND(ID_MARK2_TOGGLE, &CStirlingView::OnMark2Toggle)
    ON_COMMAND(ID_MARK3_TOGGLE, &CStirlingView::OnMark3Toggle)
    ON_UPDATE_COMMAND_UI(ID_MARK2_TOGGLE, &CStirlingView::OnUpdateMarkToggle)
    ON_UPDATE_COMMAND_UI(ID_MARK3_TOGGLE, &CStirlingView::OnUpdateMarkToggle)
    ON_COMMAND(ID_MARK_NEXT, &CStirlingView::OnMarkNext)
    ON_COMMAND(ID_MARK_PREV, &CStirlingView::OnMarkPrev)
    ON_COMMAND(ID_MARK_CLEAR_ALL, &CStirlingView::OnMarkClearAll)
    ON_UPDATE_COMMAND_UI(ID_MARK_NEXT, &CStirlingView::OnUpdateMarkExists)
    ON_UPDATE_COMMAND_UI(ID_MARK_PREV, &CStirlingView::OnUpdateMarkExists)
    ON_UPDATE_COMMAND_UI(ID_MARK_CLEAR_ALL, &CStirlingView::OnUpdateMarkExists)
    ON_COMMAND(ID_MARK_LIST, &CStirlingView::OnMarkList)
    ON_COMMAND(ID_MARK_EXPORT, &CStirlingView::OnMarkExport)
    ON_UPDATE_COMMAND_UI(ID_MARK_EXPORT, &CStirlingView::OnUpdateMarkExists)
    ON_COMMAND(ID_MARK_IMPORT, &CStirlingView::OnMarkImport)
    // カーソル移動（原 cat1。keymap 経由で起動）
    ON_COMMAND(ID_CURSOR_LEFT, &CStirlingView::OnCursorLeft)
    ON_COMMAND(ID_CURSOR_RIGHT, &CStirlingView::OnCursorRight)
    ON_COMMAND(ID_CURSOR_UP, &CStirlingView::OnCursorUp)
    ON_COMMAND(ID_CURSOR_DOWN, &CStirlingView::OnCursorDown)
    ON_COMMAND(ID_CURSOR_LINE_HOME, &CStirlingView::OnCursorLineHome)
    ON_COMMAND(ID_CURSOR_LINE_END, &CStirlingView::OnCursorLineEnd)
    ON_COMMAND(ID_CURSOR_FAST_UP, &CStirlingView::OnCursorFastUp)
    ON_COMMAND(ID_CURSOR_FAST_DOWN, &CStirlingView::OnCursorFastDown)
    ON_COMMAND(ID_PAGE_UP, &CStirlingView::OnPageUp)
    ON_COMMAND(ID_PAGE_DOWN, &CStirlingView::OnPageDown)
    ON_COMMAND(ID_HALF_PAGE_UP, &CStirlingView::OnHalfPageUp)
    ON_COMMAND(ID_HALF_PAGE_DOWN, &CStirlingView::OnHalfPageDown)
    ON_COMMAND(ID_LINE_UP, &CStirlingView::OnLineUp)
    ON_COMMAND(ID_LINE_DOWN, &CStirlingView::OnLineDown)
    // 選択拡張（原 cat2）
    ON_COMMAND(ID_SELECT_MODE, &CStirlingView::OnSelectMode)
    ON_COMMAND(ID_SELECT_LEFT, &CStirlingView::OnSelectLeft)
    ON_COMMAND(ID_SELECT_RIGHT, &CStirlingView::OnSelectRight)
    ON_COMMAND(ID_SELECT_UP, &CStirlingView::OnSelectUp)
    ON_COMMAND(ID_SELECT_DOWN, &CStirlingView::OnSelectDown)
    ON_COMMAND(ID_SELECT_DATA_TOP, &CStirlingView::OnSelectDataTop)
    ON_COMMAND(ID_SELECT_DATA_END, &CStirlingView::OnSelectDataEnd)
    ON_COMMAND(ID_SELECT_LINE_HOME, &CStirlingView::OnSelectLineHome)
    ON_COMMAND(ID_SELECT_LINE_END, &CStirlingView::OnSelectLineEnd)
    ON_COMMAND(ID_SELECT_PAGE_UP, &CStirlingView::OnSelectPageUp)
    ON_COMMAND(ID_SELECT_PAGE_DOWN, &CStirlingView::OnSelectPageDown)
    // 編集トグル/削除（原 cat3）
    ON_COMMAND(ID_TOGGLE_INSERT, &CStirlingView::OnToggleInsert)
    ON_COMMAND(ID_TOGGLE_PANE, &CStirlingView::OnTogglePane)
    ON_COMMAND(ID_DELETE_BYTE, &CStirlingView::OnDeleteByte)
    ON_COMMAND(ID_DELETE_BYTE_BACK, &CStirlingView::OnDeleteByteBack)
    ON_COMMAND(ID_GOTO_DATA_TOP, &CStirlingView::OnGotoDataTop)
    ON_COMMAND(ID_GOTO_DATA_END, &CStirlingView::OnGotoDataEnd)
    ON_COMMAND(ID_JUMP, &CStirlingView::OnJump)
    ON_COMMAND(ID_GOTO_LAST_MODIFIED, &CStirlingView::OnGotoLastModified)
    ON_UPDATE_COMMAND_UI(ID_GOTO_LAST_MODIFIED, &CStirlingView::OnUpdateGotoLastModified)
    ON_COMMAND(ID_EDIT_FIND, &CStirlingView::OnEditFind)
    ON_COMMAND(ID_FIND_MISMATCH, &CStirlingView::OnFindMismatch)
    ON_COMMAND(ID_SYNC_SCROLL, &CStirlingView::OnSyncScroll)
    ON_COMMAND(ID_ADJUST_WINDOW_SIZE, &CStirlingView::OnAdjustWindowSize)
    ON_COMMAND(ID_EDIT_REPLACE, &CStirlingView::OnEditReplace)
    ON_UPDATE_COMMAND_UI(ID_EDIT_REPLACE, &CStirlingView::OnUpdateEditReplace)
    ON_COMMAND(ID_FIND_NEXT, &CStirlingView::OnFindNextCmd)
    ON_COMMAND(ID_FIND_PREV, &CStirlingView::OnFindPrevCmd)
    ON_COMMAND(ID_DELETE_SELECTION, &CStirlingView::OnDeleteSelection)
    ON_UPDATE_COMMAND_UI(ID_DELETE_SELECTION, &CStirlingView::OnUpdateEditSelectionCmd)
    ON_COMMAND(ID_FILL_SELECTION, &CStirlingView::OnFillSelection)
    ON_UPDATE_COMMAND_UI(ID_FILL_SELECTION, &CStirlingView::OnUpdateEditSelectionCmd)
    ON_COMMAND(ID_SAVE_SELECTION, &CStirlingView::OnSaveSelection)
    ON_UPDATE_COMMAND_UI(ID_SAVE_SELECTION, &CStirlingView::OnUpdateSelectionCmd)
    ON_COMMAND(ID_SAVE_DUMP, &CStirlingView::OnSaveDump)
    // 印刷（標準MFCコマンド。CView が実処理、印刷仮想関数はビュー側でオーバーライド）
    ON_COMMAND(ID_FILE_PRINT, &CView::OnFilePrint)
    ON_COMMAND(ID_FILE_PRINT_DIRECT, &CView::OnFilePrint)
    ON_COMMAND(ID_FILE_PRINT_PREVIEW, &CStirlingView::OnFilePrintPreview)
    ON_COMMAND(ID_PRINT_RANGE, &CStirlingView::OnPrintRange)
    ON_COMMAND(ID_SELECT_RANGE, &CStirlingView::OnSelectRange)
    ON_COMMAND(ID_TOGGLE_READONLY, &CStirlingView::OnToggleReadOnly)
    ON_UPDATE_COMMAND_UI(ID_TOGGLE_READONLY, &CStirlingView::OnUpdateToggleReadOnly)
    ON_COMMAND(ID_COMPARE, &CStirlingView::OnCompare)
    ON_UPDATE_COMMAND_UI(ID_COMPARE, &CStirlingView::OnUpdateCompare)
    ON_COMMAND(ID_EDIT_SELECT_ALL, &CStirlingView::OnEditSelectAll)
    ON_UPDATE_COMMAND_UI(ID_EDIT_SELECT_ALL, &CStirlingView::OnUpdateEditSelectAll)
    ON_COMMAND(ID_REVERT_FILE, &CStirlingView::OnRevertFile)
    ON_UPDATE_COMMAND_UI(ID_REVERT_FILE, &CStirlingView::OnUpdateRevertFile)
    // ステータスバー各ペインの更新（アイドル時。原はビュー側で更新）。カタログ全20項目に対応。
    ON_UPDATE_COMMAND_UI(ID_INDICATOR_ADDRESS,   &CStirlingView::OnUpdateIndicatorAddress)
    ON_UPDATE_COMMAND_UI(ID_INDICATOR_ADDR_DEC,  &CStirlingView::OnUpdateIndicatorAddrDec)
    ON_UPDATE_COMMAND_UI(ID_INDICATOR_MODIFIED,  &CStirlingView::OnUpdateIndicatorModified)
    ON_UPDATE_COMMAND_UI(ID_INDICATOR_EDITLOCK,  &CStirlingView::OnUpdateIndicatorEditLock)
    ON_UPDATE_COMMAND_UI(ID_INDICATOR_MODE,      &CStirlingView::OnUpdateIndicatorMode)
    ON_UPDATE_COMMAND_UI(ID_INDICATOR_SIZE,      &CStirlingView::OnUpdateIndicatorSize)
    ON_UPDATE_COMMAND_UI(ID_INDICATOR_SIZE_HEX,  &CStirlingView::OnUpdateIndicatorSizeHex)
    ON_UPDATE_COMMAND_UI(ID_INDICATOR_CHARSET,   &CStirlingView::OnUpdateIndicatorCharset)
    ON_UPDATE_COMMAND_UI(ID_INDICATOR_BYTEORDER, &CStirlingView::OnUpdateIndicatorByteOrder)
    ON_UPDATE_COMMAND_UI(ID_INDICATOR_BYTE_DEC,  &CStirlingView::OnUpdateIndicatorByteDec)
    ON_UPDATE_COMMAND_UI(ID_INDICATOR_BYTE_HEX,  &CStirlingView::OnUpdateIndicatorByteHex)
    ON_UPDATE_COMMAND_UI(ID_INDICATOR_WORD_DEC,  &CStirlingView::OnUpdateIndicatorWordDec)
    ON_UPDATE_COMMAND_UI(ID_INDICATOR_WORD_HEX,  &CStirlingView::OnUpdateIndicatorWordHex)
    ON_UPDATE_COMMAND_UI(ID_INDICATOR_DWORD_DEC, &CStirlingView::OnUpdateIndicatorDwordDec)
    ON_UPDATE_COMMAND_UI(ID_INDICATOR_DWORD_HEX, &CStirlingView::OnUpdateIndicatorDwordHex)
    ON_UPDATE_COMMAND_UI(ID_INDICATOR_FLOAT,     &CStirlingView::OnUpdateIndicatorFloat)
    ON_UPDATE_COMMAND_UI(ID_INDICATOR_DOUBLE,    &CStirlingView::OnUpdateIndicatorDouble)
END_MESSAGE_MAP()

CStirlingView::CStirlingView()
    : m_bytesPerRow(16)
    , m_addrRadix(1)
    , m_charW(0)
    , m_rowH(0)
    , m_topLine(0)
    , m_caretPos(0)
    , m_activePane(0)
    , m_nibbleLow(false)
    , m_caretShown(false)
    , m_selActive(false)
    , m_selAnchor(0)
    , m_dragging(false)
    , m_clrHeaderText(0), m_clrHeaderBack(0)
    , m_clrAddrText(0), m_clrAddrBack(0)
    , m_clrDataText(0), m_clrDataBack(0)
    , m_lastFindRange(0)
    , m_wholeSearchStarted(false)
    , m_findSelCaptured(false)
    , m_findSelLo(0), m_findSelHi(0)
    , m_compareActive(false) {
    // 表示設定は theApp 共有の設定オブジェクトから取得（原 CMainFrame 共有設定 view+0x248 相当）。
    const CStirlingSettings& s = theApp.Settings();
    m_bytesPerRow  = s.lineSize;
    m_addrRadix    = s.addressBase;
    m_clrHeaderText = s.headerText;  m_clrHeaderBack = s.headerBack;
    m_clrAddrText   = s.addrText;    m_clrAddrBack   = s.addrBack;
    m_clrDataText   = s.dataText;    m_clrDataBack   = s.dataBack;
    s.BuildByteColorTable(m_byteColorTable);
}

CStirlingView::~CStirlingView() {}

CStirlingDoc* CStirlingView::GetDocument() const {
    return STATIC_DOWNCAST(CStirlingDoc, m_pDocument);
}

BOOL CStirlingView::PreCreateWindow(CREATESTRUCT& cs) {
    // 縦横スクロールバーを持つ独自スクロールビュー（原 PreCreateWindow は style |= 0x300000）。
    cs.style |= WS_VSCROLL | WS_HSCROLL;
    return CView::PreCreateWindow(cs);
}

// 固定ピッチフォントを一度だけ生成し、文字幅/行高を確定する。
// UTF-8ソース(/utf-8)＋ワイド文字列＋CreateFontIndirectW で face 名を安全に渡す。
// この文書に適用する表示設定（拡張子で解決した doc の設定。doc 未接続時は theApp 既定）。
const CStirlingSettings& CStirlingView::CurSettings() const {
    CStirlingDoc* pDoc = GetDocument();
    return pDoc ? pDoc->Settings() : theApp.Settings();
}

// 拡張子別設定変更後に表示設定を再取得して反映する（全ビュー適用の各ビュー処理）。
void CStirlingView::ReloadSettings() {
    const CStirlingSettings& s = CurSettings();
    m_bytesPerRow = (s.lineSize >= 2) ? s.lineSize : 16;
    m_addrRadix   = s.addressBase ? 1 : 0;
    m_clrHeaderText = s.headerText;  m_clrHeaderBack = s.headerBack;
    m_clrAddrText   = s.addrText;    m_clrAddrBack   = s.addrBack;
    m_clrDataText   = s.dataText;    m_clrDataBack   = s.dataBack;
    s.BuildByteColorTable(m_byteColorTable);   // データ文字色＋強調表示コード → バイト値別色表

    // フォントを作り直してメトリクスを再確定（フォント高が変わる可能性に備える）。
    m_font.DeleteObject();
    m_fontUtf8.DeleteObject();          // UTF-8 文字欄用（Issue #98）
    m_utf8CellWidth.clear();            // セル幅はフォント依存なので測り直す
    m_charW = 0;
    m_rowH = 0;
    if (GetSafeHwnd() != nullptr) {
        CClientDC dc(this);
        EnsureFont(&dc);
    }

    // 拡張子別設定の変更で1行バイト数やフォントが変わった場合も、初回表示時と同じく
    // 子フレーム幅を再計算する（一覧確定後の開いている文書へ反映）。
    FitFrameWidth();

    // 1行バイト数が変わるとキャレットの行/列が変わる。絶対位置は不変だが範囲だけ丸める。
    const stirling::FileOffset total = Total();
    if (m_caretPos > total) { m_caretPos = total; }
    if (m_selAnchor > total) { m_selAnchor = total; }

    UpdateScrollInfo();
    UpdateCaret();
    Invalidate(FALSE);
}

void CStirlingView::EnsureFont(CDC* pDC) {
    if (m_charW != 0) {
        return;
    }
    if (m_font.GetSafeHandle() == nullptr) {
        const CStirlingSettings& s = CurSettings();
        LOGFONTW lf = {0};
        lf.lfHeight = s.fontHeight;   // 負値=文字高さ(px)。既定 -16
        lf.lfWeight = s.fontWeight;
        lf.lfItalic = (BYTE)(s.fontItalic ? 1 : 0);
        lf.lfCharSet = SHIFTJIS_CHARSET;
        lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
        wcscpy_s(lf.lfFaceName, LF_FACESIZE,
                 s.fontFace.IsEmpty() ? L"ＭＳ ゴシック" : (LPCWSTR)s.fontFace);
        HFONT hFont = ::CreateFontIndirectW(&lf);
        if (hFont != nullptr) {
            m_font.Attach(hFont);   // 以後の破棄は CFont（m_font）が担う
        }
        // UTF-8 文字欄用（Issue #98）。face / 寸法は同じで charset だけ DEFAULT にし、
        //   GDI のフォントリンクに CP932 外のグリフを補わせる。桁幅・行高は m_font から
        //   採るため、このフォントの有無でレイアウトは変わらない。
        lf.lfCharSet = DEFAULT_CHARSET;
        HFONT hFontUtf8 = ::CreateFontIndirectW(&lf);
        if (hFontUtf8 != nullptr) {
            m_fontUtf8.Attach(hFontUtf8);
        }
    }
    TEXTMETRIC tm = {0};
    {
        const stirling::ScopedSelectFont selFont(pDC, &m_font);
        pDC->GetTextMetrics(&tm);
    }
    m_charW = tm.tmAveCharWidth;
    m_rowH = tm.tmHeight + tm.tmExternalLeading;
    if (m_charW <= 0) { m_charW = 8; }
    if (m_rowH <= 0) { m_rowH = 16; }
}

// アドレス表示桁（Issue #21）。
//   原は 16進8桁 / 10進10桁の固定。総サイズが 32bit に収まる間はその桁を保ち、
//   4GB 以上のファイルでのみ必要な桁へ広げる（原との golden 比較を壊さないため）。
int CStirlingView::AddrDigits() const {
    const stirling::FileOffset total = Total();
    if (m_addrRadix) {
        int digits = 8;                                  // 16進: 原の既定
        if (total > 0xFFFFFFFFLL) {
            digits = 0;
            for (stirling::FileOffset v = total; v > 0; v >>= 4) { ++digits; }
        }
        return digits;
    }
    int digits = 10;                                     // 10進: 原の既定
    if (total > 9999999999LL) {
        digits = 0;
        for (stirling::FileOffset v = total; v > 0; v /= 10) { ++digits; }
    }
    return digits;
}

int CStirlingView::ColAddrX() const {
    return m_charW;                 // 左余白 1セル
}

int CStirlingView::ColHexX() const {
    return ColAddrX() + (AddrDigits() + 2) * m_charW;   // アドレス欄＋区切り2セル
}

int CStirlingView::ColCharX() const {
    // 16進欄は各バイト2桁を3セル間隔で配置し末尾桁は bytesPerRow*3-1 セルで終わる。
    // 原は16進欄と文字欄の間をスペース3個分空けるため +2 セル（末尾ギャップ1＋2）。
    return ColHexX() + (m_bytesPerRow * 3 + 2) * m_charW;
}

// レイアウト全体の横幅(px, 未スクロール基準)。文字欄の右端＋右余白2セル（原と一致）。
int CStirlingView::ContentWidthPx() const {
    return ColCharX() + m_bytesPerRow * m_charW + 2 * m_charW;
}

// 横スクロール最大位置(px)。内容幅がクライアント幅を超えた分。
int CStirlingView::MaxHScroll() const {
    CRect rc;
    GetClientRect(&rc);
    int m = ContentWidthPx() - rc.Width();
    return (m > 0) ? m : 0;
}

// 「アドレスも横スクロールの対象とする」OFF＝アドレス欄は固定（データ欄のみ横スクロール）。
bool CStirlingView::FreezeAddr() const {
    return !CurSettings().addrHScroll;
}

stirling::FileOffset CStirlingView::TotalRows() const {
    CStirlingDoc* pDoc = GetDocument();
    const stirling::FileOffset total = pDoc ? pDoc->GetTotalLength() : 0;
    if (total <= 0) {
        return 0;
    }
    // 末尾位置(total=データ末尾の後ろ)が乗る行まで含める。データ長が bytesPerRow で割り切れる
    // 場合は、末尾位置だけの空行が1行増える（原はこの行までスクロール可能）。
    return total / m_bytesPerRow + 1;
}

int CStirlingView::VisibleRows() const {
    if (m_rowH <= 0) {
        return 1;
    }
    CRect rc;
    GetClientRect(&rc);
    const int h = rc.Height() - m_rowH;    // ヘッダ1行分を除く
    if (h <= 0) {
        return 0;
    }
    return h / m_rowH + 1;                  // 端数行も含める
}

// 完全に表示できるデータ行数（端数行は含めない）。キャレットの可視/スクロール判定に使う。
int CStirlingView::FullyVisibleRows() const {
    if (m_rowH <= 0) { return 1; }
    CRect rc;
    GetClientRect(&rc);
    const int h = rc.Height() - m_rowH;    // ヘッダ1行分を除く
    if (h <= 0) { return 0; }
    return h / m_rowH;                      // 端数行は数えない
}


// ---- スクロールバーのスケーリング（Issue #21）----
// Win32 の SCROLLINFO は 32bit のため、総行数が INT_MAX を超える場合は比率で写す。
// 1行2バイト(設定の下限)では 4GB 超のファイルで総行数が INT_MAX を超え得る。
// 総行数が INT_MAX 以下のときは 1:1 で、従来（32bit 時代）と完全に同じ挙動になる。
namespace {
// 比率モードで用いるスクロールバーの目盛り数（十分細かく、int に余裕を持って収まる値）。
const int kScaledScrollRange = 1 << 20;   // 1,048,576 段階
}  // namespace

stirling::FileOffset CStirlingView::TotalRowsForScroll() const {
    const stirling::FileOffset rows = TotalRows();
    return (rows > 0) ? rows : 0;
}

int CStirlingView::RowToScrollPos(stirling::FileOffset row) const {
    const stirling::FileOffset rows = TotalRowsForScroll();
    if (rows <= 0) { return 0; }
    if (row <= 0) { return 0; }
    if (rows <= INT_MAX) {                       // 1:1（従来と同一）
        return (row > rows - 1) ? static_cast<int>(rows - 1) : static_cast<int>(row);
    }
    if (row >= rows - 1) { return kScaledScrollRange - 1; }
    // 64bit で乗算してから割る。row * 2^20 が int64 を溢れるのは行数が約 8.8e12
    //   （1行2バイトで約 17TB）を超えた場合で、メモリ常駐モデルでは到達しない。
    return static_cast<int>(row * (kScaledScrollRange - 1) / (rows - 1));
}

stirling::FileOffset CStirlingView::ScrollPosToRow(int pos) const {
    const stirling::FileOffset rows = TotalRowsForScroll();
    if (rows <= 0) { return 0; }
    if (pos <= 0) { return 0; }
    if (rows <= INT_MAX) {                       // 1:1（従来と同一）
        const stirling::FileOffset r = pos;
        return (r > rows - 1) ? (rows - 1) : r;
    }
    if (pos >= kScaledScrollRange - 1) { return rows - 1; }
    return static_cast<stirling::FileOffset>(pos) * (rows - 1) / (kScaledScrollRange - 1);
}

void CStirlingView::UpdateScrollInfo() {
    const stirling::FileOffset totalRows = TotalRows();
    // 最大スクロール位置・ページ量は「完全表示行数」で決める。端数込み VisibleRows で計算すると
    // 最下行を完全表示する位置まで届かず、末尾データが視認できなくなる。
    int visRows = FullyVisibleRows();
    if (visRows <= 0) { visRows = 1; }
    stirling::FileOffset maxTop = totalRows - visRows;
    if (maxTop < 0) { maxTop = 0; }
    if (m_topLine > maxTop) { m_topLine = maxTop; }
    if (m_topLine < 0) { m_topLine = 0; }

    SCROLLINFO si = {0};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
    si.nMin = 0;
    if (totalRows <= INT_MAX) {
        // 1:1（従来と同一の値をそのまま渡す）。
        si.nMax = (totalRows > 0) ? static_cast<int>(totalRows - 1) : 0;
        si.nPage = static_cast<UINT>(visRows);
    } else {
        // 比率モード: 目盛りを固定範囲へ写し、ページ量も同じ比率で縮める。
        si.nMax = kScaledScrollRange - 1;
        stirling::FileOffset page = static_cast<stirling::FileOffset>(visRows) *
                              (kScaledScrollRange - 1) / (totalRows - 1);
        if (page < 1) { page = 1; }
        si.nPage = static_cast<UINT>(page);
    }
    si.nPos = RowToScrollPos(m_topLine);
    SetScrollInfo(SB_VERT, &si, TRUE);

    // 横スクロール（px 単位）。内容幅 > クライアント幅 のとき有効。
    CRect rc;
    GetClientRect(&rc);
    const int contentW = ContentWidthPx();
    const int maxH = MaxHScroll();
    if (m_hScroll > maxH) { m_hScroll = maxH; }
    if (m_hScroll < 0) { m_hScroll = 0; }
    SCROLLINFO sh = {0};
    sh.cbSize = sizeof(sh);
    sh.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
    sh.nMin = 0;
    sh.nMax = (contentW > 0) ? (contentW - 1) : 0;
    sh.nPage = (rc.Width() > 0) ? static_cast<UINT>(rc.Width()) : 1;
    sh.nPos = m_hScroll;
    SetScrollInfo(SB_HORZ, &sh, TRUE);
}

void CStirlingView::OnHScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar) {
    const int maxH = MaxHScroll();
    const int line = (m_charW > 0) ? m_charW : 8;
    CRect rc;
    GetClientRect(&rc);
    const int page = (rc.Width() > line) ? (rc.Width() - line) : line;

    int newH = m_hScroll;
    switch (nSBCode) {
    case SB_LINELEFT:   newH -= line; break;
    case SB_LINERIGHT:  newH += line; break;
    case SB_PAGELEFT:   newH -= page; break;
    case SB_PAGERIGHT:  newH += page; break;
    case SB_LEFT:       newH = 0; break;
    case SB_RIGHT:      newH = maxH; break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION: {
        SCROLLINFO si = {0};
        si.cbSize = sizeof(si);
        si.fMask = SIF_TRACKPOS;
        newH = GetScrollInfo(SB_HORZ, &si) ? si.nTrackPos : static_cast<int>(nPos);
        break;
    }
    default: break;
    }
    if (newH > maxH) { newH = maxH; }
    if (newH < 0) { newH = 0; }
    if (newH != m_hScroll) {
        m_hScroll = newH;
        SetScrollPos(SB_HORZ, m_hScroll, TRUE);
        Invalidate(FALSE);
        UpdateCaret();
    }
    CView::OnHScroll(nSBCode, nPos, pScrollBar);
}

int CStirlingView::OnCreate(LPCREATESTRUCT lpCreateStruct) {
    if (CView::OnCreate(lpCreateStruct) == -1) {
        return -1;
    }
    CClientDC dc(this);
    EnsureFont(&dc);
    UpdateScrollInfo();
    DragAcceptFiles(TRUE);   // ドキュメント欄へのファイルドロップを受け付ける（原と同様）
    return 0;
}

// ファイルのドラッグ＆ドロップでオープン（原 shell32 DragQueryFileA ループ）。
//   原ヘルプ準拠: D&D はリンクファイル(.lnk)を解決せず常にそのまま開くため、
//   CWinApp::OpenDocumentFile ではなく theApp.OpenDroppedFile を使う。
void CStirlingView::OnDropFiles(HDROP hDropInfo) {
    const UINT count = ::DragQueryFile(hDropInfo, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        const CStringW path = ui::DragQueryPath(hDropInfo, i);
        if (!path.IsEmpty()) {
            theApp.OpenDroppedFile(path);
        }
    }
    ::DragFinish(hDropInfo);
}

void CStirlingView::OnSize(UINT nType, int cx, int cy) {
    CView::OnSize(nType, cx, cy);
    UpdateScrollInfo();
    UpdateCaret();
}

// 文書の読み込み/更新後に呼ばれる（OnInitialUpdate 経由含む）。スクロール範囲を再計算して
// ファイルを開いた直後からツマミが正しく表示されるようにする。
void CStirlingView::OnUpdate(CView* /*pSender*/, LPARAM /*lHint*/, CObject* /*pHint*/) {
    UpdateScrollInfo();
    Invalidate(FALSE);
    UpdateCaret();
}

BOOL CStirlingView::OnEraseBkgnd(CDC* /*pDC*/) {
    // OnDraw 側で全面を塗るため、既定の消去は行わない（ちらつき抑止）。
    return TRUE;
}

void CStirlingView::OnDraw(CDC* pDC) {
    // 画面描画は完成した内容を一括転送し、背景だけが先に見える状態を防ぐ。
    // 印刷・プレビューではプリンタ／プレビューDCへ従来どおり直接描画する。
    if (pDC->IsPrinting()) {
        DrawContent(pDC);
        return;
    }

    CRect client;
    GetClientRect(&client);
    if (client.IsRectEmpty()) { return; }

    CDC memDC;
    if (!memDC.CreateCompatibleDC(pDC)) {
        TRACE("CStirlingView: CreateCompatibleDC failed; drawing directly.\n");
        DrawContent(pDC);
        return;
    }

    CBitmap bitmap;
    if (!bitmap.CreateCompatibleBitmap(pDC, client.Width(), client.Height())) {
        TRACE("CStirlingView: CreateCompatibleBitmap failed; drawing directly.\n");
        DrawContent(pDC);
        return;
    }

    const stirling::ScopedSelectObject selBitmap(&memDC, &bitmap);
    DrawContent(&memDC);

    CRect clip;
    if (pDC->GetClipBox(&clip) != NULLREGION) {
        clip.IntersectRect(clip, client);
        if (!clip.IsRectEmpty()) {
            pDC->BitBlt(clip.left, clip.top, clip.Width(), clip.Height(),
                         &memDC, clip.left, clip.top, SRCCOPY);
        }
    }
}

void CStirlingView::DrawContent(CDC* pDC) {
    EnsureFont(pDC);
    CStirlingDoc* pDoc = GetDocument();

    CRect client;
    GetClientRect(&client);

    const int hoff = m_hScroll;
    const bool freeze = FreezeAddr();
    const int addrShift = freeze ? 0 : hoff;      // アドレス欄の横シフト（固定時0）
    const int addrBandRight = ColHexX() - m_charW;   // アドレス欄背景の右端(基準)

    // --- 背景（データ欄＝全面／ヘッダ帯＝全幅固定） ---
    pDC->FillSolidRect(client, m_clrDataBack);
    pDC->FillSolidRect(CRect(client.left, 0, client.right, m_rowH), m_clrHeaderBack);

    const stirling::ScopedSelectFont selFont(pDC, &m_font);
    pDC->SetBkMode(OPAQUE);

    const stirling::FileOffset total = pDoc ? pDoc->GetTotalLength() : 0;
    const int rows = VisibleRows();
    const stirling::FileOffset totalRows = TotalRows();

    // =====================================================================
    // データ領域（16進欄＋文字欄＋ルーラ）: 内容を -hoff シフト。
    //   アドレス固定時はアドレス欄右端でクリップして食い込みを防ぐ。
    // =====================================================================
    {
        const int saved = pDC->SaveDC();
        if (freeze) {
            pDC->IntersectClipRect(addrBandRight, 0, client.right, client.bottom);
        }
        pDC->OffsetViewportOrg(-hoff, 0);

        // ヘッダ: 列番号ルーラ / 文字欄ルーラ。基数に追従（原: 16進=00..0F/0-F、10進=00..15/0-5）。
        pDC->SetTextColor(m_clrHeaderText);
        pDC->SetBkColor(m_clrHeaderBack);
        {
            CStringW ruler;
            for (int i = 0; i < m_bytesPerRow; ++i) {
                CStringW c;
                c.Format(m_addrRadix ? L"%02X " : L"%02d ", i);
                ruler += c;
            }
            pDC->TextOutW(ColHexX(), 0, ruler);
            CStringW charRuler;
            for (int i = 0; i < m_bytesPerRow; ++i) {
                charRuler += m_addrRadix ? L"0123456789ABCDEF"[i & 0x0F]
                                         : (wchar_t)(L'0' + (i % 10));   // 10進は列番号の下1桁
            }
            pDC->TextOutW(ColCharX(), 0, charRuler);
        }

        if (total > 0) {
            // 文字欄（文字セット別描画）を先に。選択反転 PatBlt は後段で重ねる。
            DrawCharColumn(pDC, m_topLine, rows, total);

            stirling::BlockCursor cur(&pDoc->Blocks());
            COLORREF rowFg[256] = {0};   // 1行分のバイト色（原の同色連続束ね再現用）
            COLORREF rowBg[256] = {0};
            bool rowMark[256] = {false};  // マーク由来の色か（マークは1バイト単位の束）
            for (int r = 0; r < rows; ++r) {
                const stirling::FileOffset line = m_topLine + r;
                if (line >= totalRows) break;
                const stirling::FileOffset off = line * m_bytesPerRow;
                // 残りバイト数は 64bit のままクランプしてから int へ落とす。
                //   先にキャストすると (total - off) が INT_MAX を超える行で負値になり、
                //   その行の16進欄が描画されなくなる（2GB 超ファイルで実際に発生）。
                stirling::FileOffset avail = total - off;
                if (avail < 0) { avail = 0; }
                int n = (avail > m_bytesPerRow) ? m_bytesPerRow : static_cast<int>(avail);
                unsigned char buf[256] = {0};
                if (n > static_cast<int>(sizeof(buf))) { n = static_cast<int>(sizeof(buf)); }
                if (n > 0 && cur.Seek(off, stirling::BlockCursor::kBegin, nullptr)) { cur.Read(n, buf); }
                const int y = (r + 1) * m_rowH;

                // 16進欄（バイト毎に色付き2桁描画）。原は同じ色属性の連続バイトを束ねて
                //   ExtTextOutA するため（[[06_CStirlingView]] OnDraw）、桁間スペースは束の
                //   内側だけがその背景色で塗られ、束の境界では塗られずデータ背景色のまま
                //   残る。マークは1バイト単位の登録＝常に単独の束なので、同種マークが
                //   隣接しても間のスペースは塗らない（原版で確認済）。これにより構造体
                //   編集該当部にあるマークは2桁分だけが着色される。
                pDC->SetBkMode(OPAQUE);
                for (int i = 0; i < n; ++i) {
                    rowMark[i] = GetByteColor(off + i, buf[i], rowFg[i], rowBg[i]);
                }
                for (int i = 0; i < n; ++i) {
                    CStringW cell; cell.Format(L"%02X", buf[i]);
                    pDC->SetTextColor(rowFg[i]);
                    pDC->SetBkColor(rowBg[i]);
                    pDC->TextOutW(ColHexX() + i * 3 * m_charW, y, cell);
                    // 束の内側の桁間スペースのみ帯化（データ背景色は塗り済みのため省略）。
                    if (i + 1 < n && !rowMark[i] && !rowMark[i + 1] &&
                        rowBg[i + 1] == rowBg[i] && rowBg[i] != m_clrDataBack) {
                        pDC->FillSolidRect(ColHexX() + i * 3 * m_charW + 2 * m_charW, y,
                                           m_charW, m_rowH, rowBg[i]);
                    }
                }
                // 選択反転（原 PatBlt ROP=DSTINVERT）。highlightBoth(view+0x27c) 無効時は
                //   アクティブペイン(m_activePane: 0=16進/1=文字)のみ反転（原 FUN_0045787f の
                //   0x27c/0x11c 分岐）。有効時は両ペイン反転。
                if (m_selActive) {
                    const stirling::FileOffset selLo = SelLo();
                    const stirling::FileOffset selHi = SelHi();
                    const bool both = theApp.AppSettings().highlightBoth;
                    const bool invHex  = both || (m_activePane == 0);
                    const bool invChar = both || (m_activePane == 1);
                    for (int i = 0; i < n; ++i) {
                        const stirling::FileOffset idx = off + i;
                        if (idx < selLo || idx >= selHi) continue;
                        int hexW = 2 * m_charW;
                        if (i + 1 < n && idx + 1 < selHi) { hexW = 3 * m_charW; }
                        if (invHex)  { pDC->PatBlt(ColHexX() + i * 3 * m_charW, y, hexW, m_rowH, DSTINVERT); }
                        if (invChar) { pDC->PatBlt(ColCharX() + i * m_charW, y, m_charW, m_rowH, DSTINVERT); }
                    }
                }
            }

            // サブキャレット（環境設定 subCaret, 原 view+0x278）。非アクティブペインの対応セル
            //   下端に水平下線を描く。原はペイン子ウィンドウの静的キャレットバーだが、単一
            //   ウィンドウ描画では下線で再現する。主キャレットと同様、選択中は表示しない。
            if (theApp.AppSettings().subCaret && !m_selActive) {
                const stirling::FileOffset crow = CaretRow();
                if (crow >= m_topLine && crow < m_topLine + rows) {
                    const int col  = CaretCol();
                    const int yBar = static_cast<int>(crow - m_topLine + 1) * m_rowH + m_rowH - 2;   // セル下端
                    int bx, bw;
                    if (m_activePane == 0) {                 // 16進が主 → 文字ペインに下線
                        bx = ColCharX() + col * m_charW;        bw = m_charW;
                    } else {                                  // 文字が主 → 16進ペインに下線
                        bx = ColHexX() + col * 3 * m_charW;     bw = 2 * m_charW;
                    }
                    pDC->FillSolidRect(bx, yBar, bw, 2, RGB(0, 0, 0));
                }
            }
        }
        pDC->RestoreDC(saved);
    }

    // =====================================================================
    // アドレス領域（ADDRESS ヘッダ＋アドレス欄）: 全体スクロール時は -hoff、
    //   固定時は 0＋アドレス欄右端でクリップ。
    // =====================================================================
    {
        const int saved = pDC->SaveDC();
        if (freeze) {
            pDC->IntersectClipRect(0, 0, addrBandRight, client.bottom);
        } else {
            pDC->OffsetViewportOrg(-addrShift, 0);
        }
        // アドレス欄背景（ヘッダ下～下端）。文字色がデータ背景と同色でも可読にする。
        pDC->FillSolidRect(CRect(client.left, m_rowH, addrBandRight, client.bottom), m_clrAddrBack);

        pDC->SetBkMode(OPAQUE);
        pDC->SetTextColor(m_clrHeaderText);
        pDC->SetBkColor(m_clrHeaderBack);
        pDC->TextOutW(ColAddrX(), 0, L"ADDRESS");

        if (total > 0) {
            for (int r = 0; r < rows; ++r) {
                const stirling::FileOffset line = m_topLine + r;
                if (line >= totalRows) break;
                const stirling::FileOffset off = line * m_bytesPerRow;
                const int y = (r + 1) * m_rowH;
                CStringW addr;
                const int addrDigits = AddrDigits();
                if (m_addrRadix) {
                    addr.Format(L"%0*llX", addrDigits, static_cast<long long>(off));
                } else {
                    addr.Format(L"%0*llu", addrDigits, static_cast<long long>(off));
                }
                pDC->SetTextColor(m_clrAddrText);
                pDC->SetBkColor(m_clrAddrBack);
                pDC->TextOutW(ColAddrX(), y, addr);
            }
        }
        pDC->RestoreDC(saved);
    }
}

// 文書接続後に呼ばれる。この文書の拡張子で解決された設定をビューへ反映する。
void CStirlingView::OnInitialUpdate() {
    // 新規ビューの初期ペインだけは、この文書に解決された拡張子別設定を適用する。
    m_activePane = CurSettings().openCharMode ? 1 : 0;
    // ReloadSettings() からは変更せず、既存ビューの入力ペインを維持する。
    ReloadSettings();   // doc の Settings() と幅を取り込み、メトリクス/色/スクロールを確定
    CView::OnInitialUpdate();
    // キャレット位置の自動復元（原ヘルプ caretAutoRestore）。設定ONかつパスありの文書で、
    //   前回終了時に記録したキャレット位置があれば復元する（データ範囲へクランプ）。
    if (theApp.AppSettings().caretAutoRestore) {
        CStirlingDoc* pDoc = GetDocument();
        if (pDoc != nullptr) {
            const CString path = pDoc->GetPathName();
            if (!path.IsEmpty()) {
                const stirling::FileOffset pos = theApp.LookupCaretPos(path);
                if (pos >= 0) { MoveCaretTo(pos, false); }
            }
        }
    }
}

// 子フレーム幅をこの文書の設定に合わせて補正（原 CChildFrame::PreCreateWindow と同一式。高さは維持）。
void CStirlingView::FitFrameWidth() {
    CFrameWnd* pChild = GetParentFrame();
    if (pChild == nullptr || pChild->IsZoomed() || pChild->IsIconic()) {
        return;
    }
    if (m_charW <= 0) {
        CClientDC dc(this);
        EnsureFont(&dc);
    }
    // 必要クライアント幅（実レイアウト幅＋縦スクロールバー）→ ウィンドウ幅を AdjustWindowRectEx で
    //   概算し、その後 MaxHScroll() が 0 になるよう実測で詰める（枠の実寸差を吸収）。
    CRect rc(0, 0, ContentWidthPx() + ::GetSystemMetrics(SM_CXVSCROLL), 0);
    ::AdjustWindowRectEx(&rc, pChild->GetStyle() & ~(WS_HSCROLL | WS_VSCROLL),
                         FALSE, pChild->GetExStyle());
    int mdiW = 0;
    if (CWnd* pMdiClient = pChild->GetParent()) {
        CRect mc; pMdiClient->GetWindowRect(&mc);
        mdiW = mc.Width();
    }
    CRect cur;
    pChild->GetWindowRect(&cur);
    int width = rc.Width();
    if (mdiW > 0 && width > mdiW) { width = mdiW; }
    pChild->SetWindowPos(nullptr, 0, 0, width, cur.Height(), SWP_NOMOVE | SWP_NOZORDER);
    // 実測補正: 横スクロール余地が残っていれば、その分だけ子フレーム幅を広げる。
    for (int i = 0; i < 2; ++i) {
        const int extra = MaxHScroll();
        if (extra <= 0) { break; }
        pChild->GetWindowRect(&cur);
        width = cur.Width() + extra;
        if (mdiW > 0 && width > mdiW) { width = mdiW; break; }
        pChild->SetWindowPos(nullptr, 0, 0, width, cur.Height(), SWP_NOMOVE | SWP_NOZORDER);
    }
}

void CStirlingView::OnVScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar) {
    const stirling::FileOffset totalRows = TotalRows();
    // maxTop は UpdateScrollInfo と同じ「完全表示行数」基準（最下行を完全表示する位置まで許可）。
    int visRows = FullyVisibleRows();
    if (visRows <= 0) { visRows = 1; }
    stirling::FileOffset maxTop = totalRows - visRows;
    if (maxTop < 0) { maxTop = 0; }

    // 行スクロール量（垂直移動スクロール行数。原 CStirlingView::OnVScroll の view+0x25c）。
    int lineStep = theApp.AppSettings().scrollLines;
    if (lineStep < 1) { lineStep = 1; }

    stirling::FileOffset newTop = m_topLine;
    switch (nSBCode) {
    case SB_LINEUP:    newTop -= lineStep; break;
    case SB_LINEDOWN:  newTop += lineStep; break;
    case SB_PAGEUP:    newTop -= (visRows > 1 ? visRows - 1 : 1); break;
    case SB_PAGEDOWN:  newTop += (visRows > 1 ? visRows - 1 : 1); break;
    case SB_TOP:       newTop = 0; break;
    case SB_BOTTOM:    newTop = maxTop; break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION: {
        SCROLLINFO si = {0};
        si.cbSize = sizeof(si);
        si.fMask = SIF_TRACKPOS;
        // スクロールバー位置 → 行番号（比率モードでは逆変換が入る）。
        if (GetScrollInfo(SB_VERT, &si)) {
            newTop = ScrollPosToRow(si.nTrackPos);
        } else {
            newTop = ScrollPosToRow(static_cast<int>(nPos));
        }
        break;
    }
    default: break;
    }

    if (newTop > maxTop) { newTop = maxTop; }
    if (newTop < 0) { newTop = 0; }
    if (newTop != m_topLine) {
        m_topLine = newTop;
        SetScrollPos(SB_VERT, RowToScrollPos(m_topLine), TRUE);
        Invalidate(FALSE);
        UpdateCaret();
        SyncPropagate();
    }
    CView::OnVScroll(nSBCode, nPos, pScrollBar);
}

// マウスホイール縦スクロール。
//   原 Stirling はホイール非対応（ビューのメッセージマップに WM_MOUSEWHEEL 無し）。
//   本移植の機能追加として、OS の「ホイールのスクロール行数」設定を尊重する:
//     ・通常は1ノッチ = 設定行数（既定3行）
//     ・設定が WHEEL_PAGESCROLL なら1ノッチ = 1画面（可視行数）
//     ・高分解能ホイール/タッチパッド（|zDelta| < WHEEL_DELTA）は端数を累積
BOOL CStirlingView::OnMouseWheel(UINT /*nFlags*/, short zDelta, CPoint /*pt*/) {
    const stirling::FileOffset totalRows = TotalRows();
    int visRows = FullyVisibleRows();
    if (visRows <= 0) { visRows = 1; }
    stirling::FileOffset maxTop = totalRows - visRows;
    if (maxTop < 0) { maxTop = 0; }

    // 1ノッチあたりのスクロール行数を OS 設定から取得する。
    UINT linesPerNotch = 3;
    ::SystemParametersInfo(SPI_GETWHEELSCROLLLINES, 0, &linesPerNotch, 0);
    int step = (linesPerNotch == WHEEL_PAGESCROLL)
                   ? visRows                             // 1画面スクロール
                   : static_cast<int>(linesPerNotch);
    if (step < 1) { step = 1; }

    // 端数を累積し、1ノッチ（WHEEL_DELTA）に達した分だけスクロールする。
    m_wheelAccum += zDelta;
    const int notches = m_wheelAccum / WHEEL_DELTA;
    if (notches == 0) { return TRUE; }
    m_wheelAccum -= notches * WHEEL_DELTA;

    stirling::FileOffset newTop = m_topLine - notches * step;     // zDelta>0(奥)=上へスクロール
    if (newTop > maxTop) { newTop = maxTop; }
    if (newTop < 0) { newTop = 0; }
    if (newTop != m_topLine) {
        m_topLine = newTop;
        SetScrollPos(SB_VERT, RowToScrollPos(m_topLine), TRUE);
        Invalidate(FALSE);
        UpdateCaret();
        SyncPropagate();
    }
    return TRUE;
}

// ===========================================================================
// キャレット / 入力
// ===========================================================================

static bool IsHexDigit(UINT ch) {
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}
static int HexVal(UINT ch) {
    if (ch >= '0' && ch <= '9') return static_cast<int>(ch - '0');
    return static_cast<int>((ch | 0x20) - 'a' + 10);
}

// Unicode ウィンドウの WM_CHAR / WM_IME_CHAR で受け取る UTF-16 コード単位を、
// 原版の byte 層が期待する CP932 コード値（DBCS は hi=先頭/lo=後続）へ変換する。
static bool WideInputCharToCp932(UINT ch, unsigned int& cp932) {
    const wchar_t wc = static_cast<wchar_t>(ch);
    char mb[2] = {0};
    const int count = ::WideCharToMultiByte(932, 0, &wc, 1, mb, sizeof(mb),
                                            nullptr, nullptr);
    if (count == 1) {
        cp932 = static_cast<unsigned char>(mb[0]);
        return true;
    }
    if (count == 2) {
        cp932 = (static_cast<unsigned char>(mb[0]) << 8) |
                static_cast<unsigned char>(mb[1]);
        return true;
    }
    return false;
}

stirling::FileOffset CStirlingView::Total() const {
    CStirlingDoc* pDoc = GetDocument();
    return pDoc ? pDoc->GetTotalLength() : 0;
}

stirling::FileOffset CStirlingView::CaretRow() const {
    return (m_bytesPerRow > 0) ? m_caretPos / m_bytesPerRow : 0;
}

int CStirlingView::CaretCol() const {
    return (m_bytesPerRow > 0) ? m_caretPos % m_bytesPerRow : 0;
}

void CStirlingView::CaretPixel(int& x, int& y) const {
    const stirling::FileOffset row = CaretRow();
    const int col = CaretCol();
    y = static_cast<int>(row - m_topLine + 1) * m_rowH;   // +1: ヘッダ1行分
    if (m_activePane == 0) {
        // 16進ペイン: 1バイト=3セル。下位ニブル待ちなら2文字目へ。
        x = ColHexX() + col * 3 * m_charW + (m_nibbleLow ? m_charW : 0);
    } else {
        x = ColCharX() + col * m_charW;
    }
    x -= m_hScroll;   // データ欄は横スクロール量ぶん左へ（キャレットはデータ欄内）
}

void CStirlingView::CreateCaretForMode() {
    if (m_charW <= 0 || m_rowH <= 0) {
        CClientDC dc(this);
        EnsureFont(&dc);
    }
    CStirlingDoc* pDoc = GetDocument();
    const bool ow = (pDoc != nullptr) && pDoc->IsOverwriteMode();
    const int w = ow ? m_charW : 2;       // 上書き=ブロック / 挿入=細線
    CreateSolidCaret(w > 0 ? w : 2, m_rowH > 0 ? m_rowH : 16);
    m_caretShown = false;
}

// サブキャレット下線の旧位置を消し、新位置を局所的に無効化する（描画は OnDraw）。
//   フォーカスに依存せず更新する（原のサブキャレットはウィンドウ可視性で表示される）。
void CStirlingView::RefreshSubCaret() {
    if (m_subCaretDrawn) {          // 旧位置を消す
        InvalidateRect(m_subCaretRect, FALSE);
        m_subCaretDrawn = false;
    }
    if (!theApp.AppSettings().subCaret || m_selActive || m_charW <= 0) { return; }
    const stirling::FileOffset crow = CaretRow();
    if (crow < m_topLine || crow >= m_topLine + VisibleRows()) { return; }
    const int col = CaretCol();
    int bx, bw;
    if (m_activePane == 0) { bx = ColCharX() + col * m_charW;     bw = m_charW; }
    else                   { bx = ColHexX() + col * 3 * m_charW;  bw = 2 * m_charW; }
    bx -= m_hScroll;
    const int y = static_cast<int>(crow - m_topLine + 1) * m_rowH + m_rowH - 2;
    m_subCaretRect = CRect(bx, y, bx + bw, y + 2);
    m_subCaretDrawn = true;
    InvalidateRect(m_subCaretRect, FALSE);
}

void CStirlingView::UpdateCaret() {
    RefreshSubCaret();   // サブキャレット下線の再描画（フォーカス有無に関わらず）
    if (::GetFocus() != GetSafeHwnd()) {
        return;   // フォーカスが無い間はキャレット非所有
    }
    const stirling::FileOffset row = CaretRow();
    // 範囲選択中はキャレットを表示しない（原挙動）。可視行外でも非表示。
    bool visible = !m_selActive &&
                   (row >= m_topLine) && (row < m_topLine + VisibleRows());
    int x = 0, y = 0;
    if (visible) {
        CaretPixel(x, y);
        // 横スクロールでデータ欄の外（アドレス固定欄の下やクライアント右端外）へ出たら隠す。
        CRect rc;
        GetClientRect(&rc);
        const int leftLimit = FreezeAddr() ? (ColHexX() - m_charW) : 0;
        if (x < leftLimit || x >= rc.right) {
            visible = false;
        }
    }
    if (!visible) {
        if (m_caretShown) { HideCaret(); m_caretShown = false; }
        return;
    }
    SetCaretPos(CPoint(x, y));
    if (!m_caretShown) { ShowCaret(); m_caretShown = true; }
}

void CStirlingView::EnsureCaretVisible() {
    const stirling::FileOffset row = CaretRow();
    // 原挙動: キャレット行が一部でも欠ける位置なら、行全体が収まるようスクロールする。
    // そのため端数行を含めない「完全表示行数」で下端判定する。
    const int vis = FullyVisibleRows();
    stirling::FileOffset newTop = m_topLine;
    if (row < newTop) {
        newTop = row;
    } else if (vis > 0 && row >= newTop + vis) {
        newTop = row - vis + 1;
    }
    if (newTop < 0) { newTop = 0; }
    if (newTop != m_topLine) {
        m_topLine = newTop;
        UpdateScrollInfo();
        Invalidate(FALSE);
        SyncPropagate();   // キャレット移動によるスクロールも同期
    }

    // --- 横方向: キャレット列が可視域に入るよう横スクロールを調整 ---
    if (m_charW > 0) {
        const int col = CaretCol();
        const int cellW = (m_activePane == 0) ? (3 * m_charW) : m_charW;
        const int baseX = (m_activePane == 0) ? (ColHexX() + col * 3 * m_charW)
                                              : (ColCharX() + col * m_charW);
        CRect rc;
        GetClientRect(&rc);
        const int leftVis = FreezeAddr() ? (ColHexX() - m_charW) : 0;
        int newH = m_hScroll;
        if (baseX - newH < leftVis) {
            newH = baseX - leftVis;                    // 左が欠ける→左スクロール
        } else if (baseX + cellW - newH > rc.right) {
            newH = baseX + cellW - rc.right;           // 右が欠ける→右スクロール
        }
        const int maxH = MaxHScroll();
        if (newH > maxH) { newH = maxH; }
        if (newH < 0) { newH = 0; }
        if (newH != m_hScroll) {
            m_hScroll = newH;
            SetScrollPos(SB_HORZ, m_hScroll, TRUE);
            Invalidate(FALSE);
        }
    }
}

void CStirlingView::MoveCaretTo(stirling::FileOffset newPos, bool extend) {
    const stirling::FileOffset total = Total();
    if (newPos < 0) { newPos = 0; }
    if (newPos > total) { newPos = total; }

    const bool wasSel = m_selActive;
    if (extend) {
        if (!m_selActive) {
            m_selAnchor = m_caretPos;   // 拡張開始時の位置をアンカーに
        }
        m_caretPos = newPos;
        m_selActive = (m_selAnchor != m_caretPos);
    } else {
        m_caretPos = newPos;
        m_selActive = false;
    }
    m_nibbleLow = false;
    EnsureCaretVisible();
    if (extend || wasSel) {
        Invalidate(FALSE);          // 選択の増減があれば再描画
    }
    UpdateCaret();
}

void CStirlingView::ClearSelection() {
    if (m_selActive) {
        m_selActive = false;
        Invalidate(FALSE);
    }
}

bool CStirlingView::DeleteSelection() {
    if (!m_selActive) {
        return false;
    }
    const stirling::FileOffset lo = SelLo();
    const stirling::FileOffset hi = SelHi();
    const stirling::FileOffset count = hi - lo;
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr || count <= 0) {
        m_selActive = false;
        return false;
    }
    if (!pDoc->DeleteRange(lo, count)) {   // 1 Undo レコードで範囲削除
        return false;   // 上限超過の確認で中止された等。データも選択も維持する（Issue #30）
    }
    m_selActive = false;
    m_caretPos = lo;
    m_nibbleLow = false;
    return true;
}

bool CStirlingView::HitTest(CPoint pt, stirling::FileOffset& row, int& col, int& pane) const {
    if (m_charW <= 0 || m_rowH <= 0 || pt.y < m_rowH) {
        return false;                // ヘッダ帯またはメトリクス未確定
    }
    row = static_cast<stirling::FileOffset>(pt.y / m_rowH - 1) + m_topLine;
    if (row < 0) { row = 0; }

    const int hexX = ColHexX();
    const int charX = ColCharX();
    // 固定アドレス欄内のクリックは先頭可視データ列へクランプ。
    if (FreezeAddr() && pt.x < hexX - m_charW) {
        pane = 0;
        col = m_hScroll / (3 * m_charW);
        if (col < 0) { col = 0; }
        if (col >= m_bytesPerRow) { col = m_bytesPerRow - 1; }
        return true;
    }
    const int lx = pt.x + m_hScroll;   // データ欄論理X（横スクロール分を戻す）
    if (lx >= charX) {
        pane = 1;
        col = (lx - charX) / m_charW;
    } else if (lx >= hexX) {
        pane = 0;
        col = (lx - hexX) / (3 * m_charW);
    } else {
        pane = 0;
        col = 0;
    }
    if (col < 0) { col = 0; }
    if (col >= m_bytesPerRow) { col = m_bytesPerRow - 1; }
    return true;
}

void CStirlingView::AfterEdit(stirling::FileOffset newCaretPos) {
    const stirling::FileOffset total = Total();
    if (newCaretPos < 0) { newCaretPos = 0; }
    if (newCaretPos > total) { newCaretPos = total; }
    m_caretPos = newCaretPos;
    UpdateScrollInfo();
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc != nullptr) {
        pDoc->UpdateAllViews(nullptr);   // 全ビュー再描画（MDI 複製ウィンドウ対応）
    } else {
        Invalidate(FALSE);
    }
    EnsureCaretVisible();
    UpdateCaret();
    // ビットイメージのリアルタイム反映は CStirlingDoc::SetModifiedFlag から通知される
    //   （構造体編集バーからの書換えや再読込にも追従させるため、集約点をそちらへ移した）。
}

void CStirlingView::OnSetFocus(CWnd* pOldWnd) {
    CView::OnSetFocus(pOldWnd);
    CreateCaretForMode();
    UpdateCaret();
}

void CStirlingView::OnKillFocus(CWnd* pNewWnd) {
    CView::OnKillFocus(pNewWnd);
    CancelTwoStroke();                 // ２ストローク保留を解除（フォーカスが外れたら中断）
    if (m_caretShown) { HideCaret(); m_caretShown = false; }
    DestroyCaret();
}

void CStirlingView::OnChar(UINT nChar, UINT nRepCnt, UINT nFlags) {
    if (m_swallowNextChar) {                      // 直前に消費したキーの WM_CHAR を1回だけ捨てる
        m_swallowNextChar = false;
        return;
    }
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr || nChar < 0x20) {
        CView::OnChar(nChar, nRepCnt, nFlags);   // 制御文字は OnKeyDown 側で処理
        return;
    }
    if (!pDoc->CanEdit()) {
        ::MessageBeep(0);                        // 編集禁止中は入力を beep で拒否（原 ProcessCharInput）
        return;
    }

    // ---- 文字ペイン: 文字セット別に変換して入力（選択/挿入/上書きは InputTextChar 内で処理）----
    if (m_activePane == 1) {
        // Unicode ウィンドウの WM_CHAR は UTF-16 コード単位を渡す。byte 層の
        // InputTextChar は原版と同じ CP932 コード値を受け取るため、境界で変換する
        // （UTF-8 のときだけ CP932 を経由せず直接符号化する。Issue #98）。
        InputWideChar(nChar);
        return;
    }

    // ==== 以下 16進ペイン ====
    // 選択中の入力＝範囲置換（原 ReplaceRange, 単一Undoレコード）。上位ニブルで置換し
    // 以降は下位ニブルを SetByteNoUndo で畳み込む（1バイト粒度）。
    if (m_selActive) {
        const stirling::FileOffset lo = SelLo();
        const stirling::FileOffset len = SelHi() - lo;
        if (!IsHexDigit(nChar)) {
            ::MessageBeep(MB_ICONASTERISK);       // 非16進は選択を保持したまま拒否
            return;
        }
        const unsigned char hb = static_cast<unsigned char>(HexVal(nChar) << 4);
        if (!pDoc->ReplaceRange(lo, len, std::vector<unsigned char>{hb})) {
            return;   // 上限超過の確認で中止された等。選択・ニブル状態を維持（Issue #30）
        }
        m_selActive = false;
        m_nibbleLow = true;
        AfterEdit(lo);
        return;
    }

    const stirling::FileOffset total = Total();
    const stirling::FileOffset pos = m_caretPos;
    const bool ow = pDoc->IsOverwriteMode();
    const bool useOverwrite = ow && pos < total;

    if (!IsHexDigit(nChar)) {
        ::MessageBeep(MB_ICONASTERISK);
        return;
    }
    // 上書きモードでデータ末尾に入力しようとした場合、末尾自動挿入 OFF なら付加せず拒否
    //   （原ヘルプ「上書きモード時の末尾自動挿入」endAutoInsert）。挿入モードは常に付加。
    if (!m_nibbleLow && ow && pos >= total && !theApp.AppSettings().endAutoInsert) {
        ::MessageBeep(0);
        return;
    }
    const int nib = HexVal(nChar);
    if (!m_nibbleLow) {
        // 上位ニブル: 下位を 0 にしたバイトを書込み、下位ニブル待ちへ（キャレット据置）。
        const unsigned char hb = static_cast<unsigned char>(nib << 4);
        const bool okb = useOverwrite ? pDoc->OverwriteByteAt(pos, hb)
                                      : pDoc->InsertByteAt(pos, hb);
        if (okb) {
            m_nibbleLow = true;
            AfterEdit(pos);                      // 位置は据置（下位ニブル待ち）
        }
    } else {
        // 下位ニブル: 現バイトの下位へ合成し、バイト確定＋前進。
        // Undo記録なしで書換え、上位ニブルの1レコードへ畳み込む（原の1バイト粒度）。
        unsigned char cur = 0;
        pDoc->GetByteAt(pos, &cur);
        const unsigned char nb = static_cast<unsigned char>((cur & 0xF0) | nib);
        if (pDoc->SetByteNoUndo(pos, nb)) {
            m_nibbleLow = false;
            AfterEdit(pos + 1);
        }
    }
}

namespace {
// VK→内部keycode変換（原 FUN_00418b31 を忠実移植）。mod: bit0=Shift, bit1=Ctrl（原 FUN_00418e76）。
//   戻り: keycode(0..0x38) / 非マップは -1。修飾ゲートは原に忠実
//   （矢印/BS/DEL は「いずれかの修飾」必須、英字/OEM記号は Ctrl 必須。Home/End/Insert/Fキーは無条件）。
int VkToKeycode(UINT vk, int mod) {
    const bool ctrl   = (mod & 2) != 0;
    const bool anyMod = (mod != 0);
    switch (vk) {
    case VK_BACK:       return anyMod ? 0x38 : -1;   // 0x08
    case VK_END:        return 0x2f;                 // 0x23
    case VK_HOME:       return 0x2e;                 // 0x24
    case VK_LEFT:       return anyMod ? 0x30 : -1;   // 0x25
    case VK_UP:         return anyMod ? 0x32 : -1;   // 0x26
    case VK_RIGHT:      return anyMod ? 0x31 : -1;   // 0x27
    case VK_DOWN:       return anyMod ? 0x33 : -1;   // 0x28
    case VK_INSERT:     return 0x36;                 // 0x2d
    case VK_DELETE:     return anyMod ? 0x37 : -1;   // 0x2e
    case VK_OEM_COMMA:  return ctrl ? 0x2c : -1;     // 0xbc ','
    case VK_OEM_PERIOD: return ctrl ? 0x2d : -1;     // 0xbe '.'
    case 0xC0:          return ctrl ? 0x0c : -1;     // VK_OEM_3  '@'
    case 0xDB:          return ctrl ? 0x27 : -1;     // VK_OEM_4  '['
    case 0xDC:          return ctrl ? 0x28 : -1;     // VK_OEM_5  '\'（原と同じく keycode 0x28）
    case 0xDD:          return ctrl ? 0x28 : -1;     // VK_OEM_6  ']'
    case 0xDE:          return ctrl ? 0x2a : -1;     // VK_OEM_7  '^'
    case 0xE2:          return ctrl ? 0x2b : -1;     // VK_OEM_102 '\_'
    default:
        if (vk >= 'A' && vk <= 'Z')     { return ctrl ? (int)(vk - 0x34) : -1; }  // Ctrl+英字
        if (vk >= VK_F1 && vk <= VK_F12) { return (int)(vk - VK_F1); }            // F1..F12 → 0..0x0b
        return -1;
    }
}
}  // namespace

UINT CStirlingView::KeymapLookup(UINT vk) const {
    int mod = 0;
    if (::GetAsyncKeyState(VK_SHIFT)   & 0x8000) { mod |= 1; }
    if (::GetAsyncKeyState(VK_CONTROL) & 0x8000) { mod |= 2; }
    const int kc = VkToKeycode(vk, mod);
    if (kc < 0) { return 0; }
    const std::vector<UINT>& km = theApp.AppSettings().keymap;
    const int idx = mod * 0x40 + kc;                 // 原 keymap[modstate*0x40 + keycode]
    if (idx < 0 || idx >= (int)km.size()) { return 0; }
    return km[idx];
}

void CStirlingView::DispatchKeymapRaw(UINT raw) {
    const UINT r = raw & 0xffff;
    if (r == 0) { return; }
    if ((r & 0xff00) == 0x0500) {                    // 原 cat5: ユーザーメニュー/2ストローク
        if (r <= 0x0509) {
            PopupUserMenuAtCaret((int)(r & 0xff));           // ユーザーメニュー1-10 → userMenus[0..9]
        } else if (r <= 0x050c) {
            StartTwoStroke(CAppSettings::kTwoStrokeBaseIndex + (int)(r - 0x050a));  // 2ストローク機能1-3
        }
        return;
    }
    const UINT cmd = UserMenuRawToCmd(r);            // rawID→cmdID（原 FUN_00409439/DAT_004b51e6）
    if (cmd != 0) {
        SendMessage(WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
    }
}

// ２ストローク第1打鍵（原 FUN_00427591 の 2ストローク有効時分岐）。保留状態に入りタイマ開始。
void CStirlingView::StartTwoStroke(int menuIdx) {
    const CAppSettings& s = theApp.AppSettings();
    if (menuIdx < 0 || menuIdx >= (int)s.userMenus.size()) { return; }
    m_twoStrokeMenuIdx = menuIdx;
    int ms = s.twoStrokeTimeoutMs;
    if (ms <= 0) { ms = 500; }
    SetTimer(kTwoStrokeTimerId, (UINT)ms, nullptr);
}

void CStirlingView::CancelTwoStroke() {
    if (m_twoStrokeMenuIdx >= 0) {
        KillTimer(kTwoStrokeTimerId);
        m_twoStrokeMenuIdx = -1;
    }
}

// ２ストローク第2打鍵（原 FUN_004275e5）。押下VKを保留メニュー各項目のアクセラレータと照合し、
//   一致すればその機能を起動。Escでキャンセル。不一致キーは無視して保留継続。
bool CStirlingView::HandleTwoStrokeSecond(UINT vk) {
    const int idx = m_twoStrokeMenuIdx;
    m_swallowNextChar = true;   // 第2打鍵は消費するので、続く WM_CHAR（文字入力）を抑止する
    if (vk == VK_ESCAPE) { CancelTwoStroke(); return true; }   // キャンセル
    const CAppSettings& s = theApp.AppSettings();
    if (idx < 0 || idx >= (int)s.userMenus.size()) { CancelTwoStroke(); return true; }
    for (UINT item : s.userMenus[idx]) {
        const UINT raw = CAppSettings::UmRaw(item);
        if (raw == 0 || raw == CAppSettings::kUserMenuSep) { continue; }
        const UINT accel = CAppSettings::UmAccel(item);
        if (accel == 0) { continue; }
        if (::toupper((int)vk) == ::toupper((int)accel)) {     // アクセラレータ一致
            KillTimer(kTwoStrokeTimerId);
            m_twoStrokeMenuIdx = -1;
            DispatchKeymapRaw(raw);                            // 機能起動（rawIDで再ディスパッチ）
            return true;
        }
    }
    return true;   // 不一致でも消費（保留継続。normal keymap には流さない）
}

void CStirlingView::OnTimer(UINT_PTR nIDEvent) {
    if (nIDEvent == kTwoStrokeTimerId) {
        KillTimer(kTwoStrokeTimerId);
        const int idx = m_twoStrokeMenuIdx;
        m_twoStrokeMenuIdx = -1;
        if (idx >= 0) { PopupUserMenuAtCaret(idx); }           // タイムアウト→視覚ポップアップ
        return;
    }
    CView::OnTimer(nIDEvent);
}

// --- カーソル移動の内部実体（コマンド/既定ナビ共用。ext=選択拡張） ---
void CStirlingView::CaretUpOne(bool ext) {
    const int bpr = m_bytesPerRow;
    MoveCaretTo(m_caretPos >= bpr ? m_caretPos - bpr : m_caretPos, ext);
}

void CStirlingView::CaretDownOne(bool ext) {
    const int bpr = m_bytesPerRow;
    const stirling::FileOffset total = Total();
    const stirling::FileOffset np = m_caretPos + bpr;
    if (np <= total) {
        MoveCaretTo(np, ext);                        // 真下の位置が有効ならそこへ
    } else {
        // 真下にデータが無い場合: 最下段でなければデータ末尾の後ろ(total)へ、既に最下段なら不動（原挙動）。
        const stirling::FileOffset caretRow = m_caretPos / bpr;
        const stirling::FileOffset lastRow  = total / bpr;
        if (caretRow < lastRow) { MoveCaretTo(total, ext); }
    }
}

void CStirlingView::CaretLineEndTo(bool ext) {
    const int bpr = m_bytesPerRow;
    const stirling::FileOffset total = Total();
    const stirling::FileOffset rowStart = (m_caretPos / bpr) * bpr;
    const int rowBytes = (total - rowStart < bpr) ? static_cast<int>(total - rowStart) : bpr;
    MoveCaretTo(rowBytes > 0 ? rowStart + rowBytes - 1 : rowStart, ext);
}

void CStirlingView::ScrollByLines(int lines) {
    int visRows = FullyVisibleRows();
    if (visRows <= 0) { visRows = 1; }
    stirling::FileOffset maxTop = TotalRows() - visRows;
    if (maxTop < 0) { maxTop = 0; }
    stirling::FileOffset newTop = m_topLine + lines;
    if (newTop < 0) { newTop = 0; }
    if (newTop > maxTop) { newTop = maxTop; }
    if (newTop != m_topLine) {
        m_topLine = newTop;
        SetScrollPos(SB_VERT, RowToScrollPos(m_topLine), TRUE);
        Invalidate(FALSE);
        UpdateCaret();
    }
}

// --- カーソル移動コマンド（原 cat1。ext = 選択モード中は選択拡張） ---
void CStirlingView::OnCursorLeft()     { MoveCaretTo(m_caretPos - 1, m_selectMode); }
void CStirlingView::OnCursorRight()    { MoveCaretTo(m_caretPos + 1, m_selectMode); }
void CStirlingView::OnCursorUp()       { CaretUpOne(m_selectMode); }
void CStirlingView::OnCursorDown()     { CaretDownOne(m_selectMode); }
void CStirlingView::OnCursorLineHome() { MoveCaretTo((m_caretPos / m_bytesPerRow) * m_bytesPerRow, m_selectMode); }
void CStirlingView::OnCursorLineEnd()  { CaretLineEndTo(m_selectMode); }
void CStirlingView::OnCursorFastUp()   { MoveCaretTo(m_caretPos - 2 * m_bytesPerRow, m_selectMode); }   // 原 ±2行
void CStirlingView::OnCursorFastDown() { MoveCaretTo(m_caretPos + 2 * m_bytesPerRow, m_selectMode); }
void CStirlingView::OnPageUp()         { MoveCaretTo(m_caretPos - VisibleRows() * m_bytesPerRow, m_selectMode); }
void CStirlingView::OnPageDown()       { MoveCaretTo(m_caretPos + VisibleRows() * m_bytesPerRow, m_selectMode); }
void CStirlingView::OnHalfPageUp()     { MoveCaretTo(m_caretPos - (VisibleRows() / 2) * m_bytesPerRow, m_selectMode); }
void CStirlingView::OnHalfPageDown()   { MoveCaretTo(m_caretPos + (VisibleRows() / 2) * m_bytesPerRow, m_selectMode); }
void CStirlingView::OnLineUp()         { ScrollByLines(-1); }
void CStirlingView::OnLineDown()       { ScrollByLines(1); }

// --- 選択拡張コマンド（原 cat2。always extend=true） ---
void CStirlingView::OnSelectMode() {
    // DOS式選択モードのトグル（原 0x801c）。有効化後はプレーンなカーソル移動が選択を拡張する。
    //   MoveCaretTo(pos,true) が初回移動でアンカーを現位置に設定するため、事前アンカー設定は不要。
    m_selectMode = !m_selectMode;
}
void CStirlingView::OnSelectLeft()     { MoveCaretTo(m_caretPos - 1, true); }
void CStirlingView::OnSelectRight()    { MoveCaretTo(m_caretPos + 1, true); }
void CStirlingView::OnSelectUp()       { CaretUpOne(true); }
void CStirlingView::OnSelectDown()     { CaretDownOne(true); }
void CStirlingView::OnSelectDataTop()  { MoveCaretTo(0, true); }
void CStirlingView::OnSelectDataEnd()  { MoveCaretTo(Total(), true); }
void CStirlingView::OnSelectLineHome() { MoveCaretTo((m_caretPos / m_bytesPerRow) * m_bytesPerRow, true); }
void CStirlingView::OnSelectLineEnd()  { CaretLineEndTo(true); }
void CStirlingView::OnSelectPageUp()   { MoveCaretTo(m_caretPos - VisibleRows() * m_bytesPerRow, true); }
void CStirlingView::OnSelectPageDown() { MoveCaretTo(m_caretPos + VisibleRows() * m_bytesPerRow, true); }

// --- 編集トグル/削除コマンド（原 cat3） ---
void CStirlingView::OnToggleInsert() {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr) { return; }
    pDoc->SetOverwriteMode(!pDoc->IsOverwriteMode());
    DestroyCaret();
    CreateCaretForMode();               // モードに応じた形状で再生成
    UpdateCaret();
}
void CStirlingView::OnTogglePane() {
    m_activePane ^= 1;                  // 16進↔文字ペイン
    m_nibbleLow = false;
    UpdateCaret();
}
void CStirlingView::OnDeleteByte() {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr) { return; }
    const stirling::FileOffset total = Total();
    if (!pDoc->CanEdit()) {                            // 編集禁止中は削除不可（原 CanEdit ゲート）
        ::MessageBeep(MB_ICONASTERISK);
    } else if (m_selActive) {
        const stirling::FileOffset lo = SelLo();
        if (DeleteSelection()) {   // 中止時は選択・キャレットを維持（Issue #30）
            AfterEdit(lo);
        }
    } else if (m_caretPos < total) {
        pDoc->DeleteByteAt(m_caretPos, nullptr);
        AfterEdit(m_caretPos);
    } else {
        ::MessageBeep(MB_ICONASTERISK);
    }
}
void CStirlingView::OnDeleteByteBack() {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr) { return; }
    if (!pDoc->CanEdit()) {                            // 編集禁止中は削除不可
        ::MessageBeep(MB_ICONASTERISK);
    } else if (m_selActive) {
        const stirling::FileOffset lo = SelLo();
        if (DeleteSelection()) {   // 中止時は選択・キャレットを維持（Issue #30）
            AfterEdit(lo);
        }
    } else if (m_caretPos > 0) {
        pDoc->DeleteByteAt(m_caretPos - 1, nullptr);
        AfterEdit(m_caretPos - 1);
    } else {
        ::MessageBeep(MB_ICONASTERISK);
    }
}

void CStirlingView::OnKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags) {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr) {
        CView::OnKeyDown(nChar, nRepCnt, nFlags);
        return;
    }
    m_swallowNextChar = false;   // 前キーの抑止フラグは持ち越さない（今回のキーで判断し直す）

    // ２ストローク保留中は第2打鍵として処理（原 FUN_004273ec の view+0xf40!=-1 分岐→FUN_004275e5）。
    if (m_twoStrokeMenuIdx >= 0) {
        HandleTwoStrokeSecond(nChar);
        return;
    }

    // Esc（原 CStirlingView_OnKeyDown case 0x1b）。選択状態で挙動が排他:
    //   選択なし → escMenu(view+0x268) 有効時に userMenus[13] を２ストロークとして起動
    //              （第1打鍵=Esc→タイマ→第2打鍵でアクセラレータ選択／タイムアウトで視覚ポップアップ）。
    //   選択あり → escDeselect(view+0x26c) 有効時に選択解除（原 FUN_004497b3）。
    //   いずれの条件も満たさない場合は何もしない（原に合わせ既定処理へは委譲しない）。
    if (nChar == VK_ESCAPE) {
        if (!m_selActive) {
            if (theApp.AppSettings().escMenu) { StartTwoStroke(CAppSettings::kEscMenuIndex); }
        } else if (theApp.AppSettings().escDeselect) {
            m_selActive = false;
            m_nibbleLow = false;
            Invalidate(FALSE);
            UpdateCaret();
        }
        return;
    }

    // キーアサイン（keymap）を最初に参照（原 FUN_004273ec → FUN_00418b31 → keymap）。
    //   マップされていれば機能コマンド/ユーザーメニューを起動して消費する。
    const UINT raw = KeymapLookup(nChar);
    if (raw != 0) {
        DispatchKeymapRaw(raw);
        return;
    }

    // 非マップキー = 既定ナビ（原も keymap 外で処理する範囲）。プレーンな矢印/PgUp/PgDn/Tab/Delete/
    //   Backspace。選択モード中はカーソル移動が選択を拡張する。その他（英数字等）は OnChar へ委譲。
    const bool ext = m_selectMode;
    switch (nChar) {
    case VK_LEFT:   MoveCaretTo(m_caretPos - 1, ext); return;
    case VK_RIGHT:  MoveCaretTo(m_caretPos + 1, ext); return;
    case VK_UP:     CaretUpOne(ext); return;
    case VK_DOWN:   CaretDownOne(ext); return;
    case VK_PRIOR:  MoveCaretTo(m_caretPos - VisibleRows() * m_bytesPerRow, ext); return;
    case VK_NEXT:   MoveCaretTo(m_caretPos + VisibleRows() * m_bytesPerRow, ext); return;
    case VK_TAB:    OnTogglePane(); return;
    case VK_DELETE: OnDeleteByte(); return;
    case VK_BACK:   OnDeleteByteBack(); return;
    default:
        CView::OnKeyDown(nChar, nRepCnt, nFlags);
        return;
    }
}

void CStirlingView::OnLButtonDown(UINT nFlags, CPoint point) {
    SetFocus();
    stirling::FileOffset row = 0;
    int col = 0, pane = 0;
    if (HitTest(point, row, col, pane)) {
        const stirling::FileOffset total = Total();
        stirling::FileOffset pos = row * m_bytesPerRow + col;
        if (pos > total) { pos = total; }
        m_activePane = pane;
        m_nibbleLow = false;

        if ((nFlags & MK_SHIFT) != 0) {
            // Shift+クリック: 現アンカーから pos まで選択拡張。
            if (!m_selActive) { m_selAnchor = m_caretPos; }
            m_caretPos = pos;
            m_selActive = (m_selAnchor != m_caretPos);
            Invalidate(FALSE);
        } else {
            const bool wasSel = m_selActive;
            m_caretPos = pos;
            m_selAnchor = pos;         // ドラッグのアンカー（クリックしたバイト境界）
            m_selActive = false;
            if (wasSel) { Invalidate(FALSE); }
        }
        m_dragging = true;
        SetCapture();
        UpdateCaret();
    }
    CView::OnLButtonDown(nFlags, point);
}

void CStirlingView::OnMouseMove(UINT nFlags, CPoint point) {
    if (m_dragging && (nFlags & MK_LBUTTON) != 0 && m_charW > 0 && m_rowH > 0) {
        const int bpr = m_bytesPerRow;
        const stirling::FileOffset total = Total();
        if (total > 0) {
            // ホバー中の行。データ領域上端(ヘッダ帯)より上へドラッグした場合は先頭可視行より
            // 上の行を指し、EnsureCaretVisible で上方向にもオートスクロールさせる（原挙動）。
            stirling::FileOffset row;
            if (point.y >= m_rowH) {
                row = static_cast<stirling::FileOffset>(point.y / m_rowH - 1) + m_topLine;
            } else {
                // ヘッダ帯以上（y<rowH、キャプチャ中は負値もあり）→ 先頭可視行より上へ。
                row = m_topLine - 1 - static_cast<stirling::FileOffset>((m_rowH - 1 - point.y) / m_rowH);
            }
            if (row < 0) { row = 0; }

            // 選択境界（そのバイトの「右端」に到達した時にそのバイトを含める＝原挙動）。
            //   16進: 2桁目の右端(=セル先頭+2文字幅)を超えたバイト数。
            //   文字: その文字セルの右端(=セル先頭+1文字幅)を超えたバイト数。
            int c;
            if (m_activePane == 1) {
                const int rx = point.x - ColCharX();
                c = (rx <= 0) ? 0 : rx / m_charW;
            } else {
                const int rx = point.x - ColHexX();
                c = (rx < 2 * m_charW) ? 0 : (rx - 2 * m_charW) / (3 * m_charW) + 1;
            }
            if (c < 0) { c = 0; }
            if (c > bpr) { c = bpr; }

            stirling::FileOffset caret = row * bpr + c;   // 選択の可動端（バイト境界）
            if (caret < 0) { caret = 0; }
            if (caret > total) { caret = total; }
            m_caretPos = caret;
            m_selActive = (m_selAnchor != m_caretPos);
            EnsureCaretVisible();                 // 端でのオートスクロール（簡易）
            Invalidate(FALSE);
            UpdateCaret();
        }
    }
    CView::OnMouseMove(nFlags, point);
}

void CStirlingView::OnLButtonUp(UINT nFlags, CPoint point) {
    if (m_dragging) {
        m_dragging = false;
        ReleaseCapture();
    }
    CView::OnLButtonUp(nFlags, point);
}

void CStirlingView::OnEditUndo() {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr || !pDoc->CanEdit()) { return; }   // 編集禁止中は Undo 不可
    const stirling::FileOffset pos = pDoc->Undo();
    if (pos < 0) {
        ::MessageBeep(MB_ICONASTERISK);
        return;
    }
    m_selActive = false;
    m_nibbleLow = false;
    AfterEdit(pos);   // キャレットを編集位置へ・全ビュー更新
}

void CStirlingView::OnEditRedo() {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr || !pDoc->CanEdit()) { return; }   // 編集禁止中は Redo 不可
    const stirling::FileOffset pos = pDoc->Redo();
    if (pos < 0) {
        ::MessageBeep(MB_ICONASTERISK);
        return;
    }
    m_selActive = false;
    m_nibbleLow = false;
    AfterEdit(pos);
}

void CStirlingView::OnUpdateEditUndo(CCmdUI* pCmdUI) {
    CStirlingDoc* pDoc = GetDocument();
    pCmdUI->Enable(pDoc != nullptr && pDoc->CanEdit() && pDoc->CanUndo());
}

void CStirlingView::OnUpdateEditRedo(CCmdUI* pCmdUI) {
    CStirlingDoc* pDoc = GetDocument();
    pCmdUI->Enable(pDoc != nullptr && pDoc->CanEdit() && pDoc->CanRedo());
}

// ===========================================================================
// クリップボード（原 OnEditCopy/Cut/Paste）
//   内部バイナリクリップボードは theApp が保持（原 CMainFrame 保有の
//   WM_USER+7 取得 / WM_USER+8 格納 に相当）。加えて Windows クリップボードへ
//   テキストを書き出し、外部アプリとの相互運用を保つ（書式は SetClipboardText 参照）。
// ===========================================================================

// 選択バイト列を Windows クリップボードへ書き出す（原 FUN_0045c53f）。
//   16進ペイン: "%02X" をスペース区切り（大文字）。ASCII 層なので CF_UNICODETEXT で渡す
//               （CF_TEXT は OS が自動合成する）。
//   文字ペイン: 生バイトを CF_TEXT で渡す。byte 層＝原版と同じバイト列が他アプリへ
//               渡ることが仕様（設計メモ §6.5）。ワイド化すると不正バイト列が
//               置換文字へ潰れ、貼り付け先の内容が変わる。
void CStirlingView::SetClipboardText(const std::vector<unsigned char>& bytes) {
    if (bytes.empty()) {
        return;
    }
    DWORD error = ERROR_SUCCESS;
    bool ok = false;
    if (m_activePane == 0) {
        CStringW text;
        // 1 バイトあたり 3 文字（先頭のみ 2 文字）。選択範囲が大きいので事前に確保する。
        text.Preallocate(static_cast<int>(
            std::min<size_t>(bytes.size() * 3, static_cast<size_t>(INT_MAX))));
        for (size_t i = 0; i < bytes.size(); ++i) {
            text.AppendFormat((i == 0) ? L"%02X" : L" %02X", bytes[i]);
        }
        ok = ui::PutClipboardTextW(GetSafeHwnd(), text.GetString(),
                                   static_cast<size_t>(text.GetLength()), error);
    } else {
        // [byte層] 編集対象のバイト列をそのまま渡す。詳細は ClipboardUtil.h / 設計メモ §6.5
        ok = ui::PutClipboardTextA(GetSafeHwnd(),
                                   reinterpret_cast<const char*>(bytes.data()),
                                   bytes.size(), error);
    }
    if (!ok) {
        // 他プロセスがクリップボードをロックしている等。無言で中断せず理由を提示する。
        ui::MsgBox(GetSafeHwnd(),
                   ui::AppendErrorReason(ui::LoadW(IDS_ERR_CLIPBOARD_COPY), error));
    }
}

void CStirlingView::OnEditCopy() {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr || !m_selActive) {
        ::MessageBeep(0);
        return;
    }
    const stirling::FileOffset lo = SelLo();
    const stirling::FileOffset len = SelHi() - lo;
    std::vector<unsigned char> bytes = pDoc->ReadRange(lo, len);
    if (bytes.empty()) {
        ::MessageBeep(0);
        return;
    }
    SetClipboardText(bytes);              // Windows クリップボード(CF_TEXT)
    theApp.m_binClipboard = bytes;        // 内部バイナリクリップボード（原 WM_USER+8）
    // コピー後の選択解除（原 FUN_0044985f: view+0x270 有効時に FUN_004497b3 で解除）。
    if (theApp.AppSettings().deselectAfterCopy) {
        m_selActive = false;
        m_nibbleLow = false;
        Invalidate(FALSE);
        UpdateCaret();
    }
}

void CStirlingView::OnEditCut() {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr || !pDoc->CanEdit() || !m_selActive) {   // Cut は削除を伴うため編集禁止中は不可
        ::MessageBeep(0);
        return;
    }
    const stirling::FileOffset lo = SelLo();
    const stirling::FileOffset len = SelHi() - lo;
    std::vector<unsigned char> bytes = pDoc->ReadRange(lo, len);
    if (bytes.empty()) {
        ::MessageBeep(0);
        return;
    }
    // 削除を先に行う。中止された場合にクリップボードを書き換えないため（Issue #30）。
    if (!pDoc->DeleteRange(lo, len)) {    // 選択範囲削除（1 Undo レコード）
        return;                           // データも選択も維持する
    }
    SetClipboardText(bytes);              // Windows クリップボード(CF_TEXT)
    theApp.m_binClipboard = bytes;        // 内部バイナリクリップボード（原 Cut も格納する）
    m_selActive = false;
    m_nibbleLow = false;
    AfterEdit(lo);
}

void CStirlingView::OnEditPaste() {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr || !pDoc->CanEdit()) {   // 貼り付けは編集のため禁止中は不可
        ::MessageBeep(0);
        return;
    }
    const std::vector<unsigned char> clip = theApp.m_binClipboard;   // 内部バイナリ（原 WM_USER+7）
    if (clip.empty()) {
        ::MessageBeep(0);
        return;
    }
    PasteBytes(clip);
}

// 貼り付け本体（原 FUN_00449a02 の分岐をそのまま保つ）。
//   呼び出し側で編集可否とデータの空判定を済ませておくこと。
void CStirlingView::PasteBytes(const std::vector<unsigned char>& clip) {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr || clip.empty()) {
        return;
    }
    const int len = static_cast<int>(clip.size());
    if (m_selActive) {
        // 選択あり: 範囲を貼付データで置換（原 ReplaceRange）
        const stirling::FileOffset lo = SelLo();
        const stirling::FileOffset sel = SelHi() - lo;
        if (!pDoc->ReplaceRange(lo, sel, clip)) {
            return;   // 上限超過の確認で中止された等。選択を維持（Issue #30）
        }
        m_selActive = false;
        m_nibbleLow = false;
        AfterEdit(lo + len);
    } else {
        // 選択なし。原 FUN_00449a02: 上書きモード ＆ pasteOverwrite(view+0x260) ＆ カーソルが
        //   末尾未満なら「上書き貼付」（カーソル位置から min(貼付長, 残バイト数) を置換）。
        //   それ以外は挿入。いずれも新カーソル＝pos+len。
        const stirling::FileOffset pos = m_caretPos;
        const stirling::FileOffset total = Total();
        if (pDoc->IsOverwriteMode() && theApp.AppSettings().pasteOverwrite && pos < total) {
            stirling::FileOffset delLen = len;
            if (pos + delLen > total) { delLen = total - pos; }   // 末尾を越えない範囲だけ上書き
            pDoc->ReplaceRange(pos, delLen, clip);   // 上書き（1 Undo レコード）
        } else {
            pDoc->ReplaceRange(pos, 0, clip);        // 挿入（1 Undo レコード）
        }
        m_nibbleLow = false;
        AfterEdit(pos + len);
    }
}

void CStirlingView::OnUpdateEditCopy(CCmdUI* pCmdUI) {
    pCmdUI->Enable(m_selActive);
}

void CStirlingView::OnUpdateEditCut(CCmdUI* pCmdUI) {
    CStirlingDoc* pDoc = GetDocument();
    pCmdUI->Enable(pDoc != nullptr && pDoc->CanEdit() && m_selActive);
}

void CStirlingView::OnUpdateEditPaste(CCmdUI* pCmdUI) {
    CStirlingDoc* pDoc = GetDocument();
    pCmdUI->Enable(pDoc != nullptr && pDoc->CanEdit() && !theApp.m_binClipboard.empty());
}

// 16進テキスト貼り付け（Issue #97。移植で追加した機能。原には対応する処理は無い）。
//   Windows クリップボードのテキストを16進表記として解析し、バイト列として貼り付ける。
//   内部バイナリクリップボード（theApp.m_binClipboard）はコピー元ではないため更新しない。
//   解析に失敗した場合は理由を提示して何も貼り付けない（部分適用はしない）。
void CStirlingView::OnEditPasteHex() {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr || !pDoc->CanEdit()) {   // 貼り付けは編集のため禁止中は不可
        ::MessageBeep(0);
        return;
    }
    std::wstring text;
    DWORD error = ERROR_SUCCESS;
    if (!ui::GetClipboardTextW(GetSafeHwnd(), text, error)) {
        // 書式が無いだけの場合と、取得に失敗した場合を区別して提示する。
        const UINT id = (error == ERROR_NOT_FOUND) ? IDS_ERR_PASTE_HEX_NO_TEXT
                                                   : IDS_ERR_PASTE_HEX_READ;
        ui::MsgBox(GetSafeHwnd(), ui::AppendErrorReason(ui::LoadW(id), error));
        return;
    }
    std::vector<unsigned char> bytes;
    const stirling::HexTextParseResult result = stirling::ParseHexText(text, bytes);
    if (!result.Ok()) {
        CStringW msg;
        // 位置は利用者に見せるので 1 起点に直す。
        const int pos = static_cast<int>(result.errorPos) + 1;
        switch (result.error) {
            case stirling::HexTextError::InvalidChar:
                msg.Format(ui::LoadW(IDS_ERR_PASTE_HEX_INVALID), pos);
                break;
            case stirling::HexTextError::OddDigits:
                msg.Format(ui::LoadW(IDS_ERR_PASTE_HEX_ODD), pos);
                break;
            default:
                msg = ui::LoadW(IDS_ERR_PASTE_HEX_EMPTY);
                break;
        }
        ui::MsgBox(GetSafeHwnd(), msg);
        return;
    }
    PasteBytes(bytes);
}

void CStirlingView::OnUpdateEditPasteHex(CCmdUI* pCmdUI) {
    CStirlingDoc* pDoc = GetDocument();
    pCmdUI->Enable(pDoc != nullptr && pDoc->CanEdit() && ui::HasClipboardText());
}

// ===========================================================================
// 文字セット別 文字欄描画（原 this+0x344 レンダラ群 FUN_00441a10 等の移植）
//   文字セット: 0=ASCII 1=SJIS 2=EUC 3=Unicode 4=EBCDIC 5=EBCIDK
//   不変条件: 各ソースバイトは必ず1表示セルを消費する（DBCSペア=2バイト=2セル）。
//   これにより16進欄・選択反転とのセル整列が保たれる。
// ===========================================================================
namespace {

// EBCDIC / EBCIDK の256バイト変換表（原 DAT_004b56a8 / DAT_004b57a8 を移植）。
// 添字=元バイト、値=表示バイト（SJIS/ASCII）。0 は無効（'.' 表示）。
const char* const kEbcdicHex =
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000020000000000000000000002e3c282b7c2600000000000000000021242a293b002d2f0000000000000000002c255f3e3f000000000000000000603a2340273d2200616263646566676869000000000000006a6b6c6d6e6f707172000000000000007e737475767778797a000000000000000000000000000000000000000000007b4142434445464748490000000000007d4a4b4c4d4e4f5051520000000000005c00535455565758595a00000000000030313233343536373839000000000000";
const char* const kEbcidkHex =
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000020a1a2a3a4a5a6a7a8a9002e3c282b7c26aaabacadaeaf00b000215c2a293b002d2f0000000000000000002c255f3e3f000000000000000000603a2340273d2200b1b2b3b4b5b6b7b8b9ba00bbbcbdbebfc0c1c2c3c4c5c6c7c8c90000cacbcc0000cdcecfd0d1d2d3d4d500d6d7d8d900000000000000000000dadbdcdddedf00414243444546474849000000000000004a4b4c4d4e4f5051520000000000002400535455565758595a00000000000030313233343536373839000000000000";

int HexNyb(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}
void DecodeHex256(const char* hex, unsigned char* out) {
    for (int i = 0; i < 256; ++i) {
        out[i] = static_cast<unsigned char>((HexNyb(hex[i * 2]) << 4) | HexNyb(hex[i * 2 + 1]));
    }
}
const unsigned char* EbcdicTable() {
    static unsigned char t[256];
    static bool init = false;
    if (!init) { DecodeHex256(kEbcdicHex, t); init = true; }
    return t;
}
const unsigned char* EbcidkTable() {
    static unsigned char t[256];
    static bool init = false;
    if (!init) { DecodeHex256(kEbcidkHex, t); init = true; }
    return t;
}

// 逆変換表（ASCII/SJIS バイト → EBCDIC / EBCIDK。原 DAT_004b58a8 / DAT_004b59a8 を移植）。
// 添字=入力バイト、値=EBCDIC(K)バイト。0 は写像なし（原は元バイトを保持）。
const char* const kRevEbcdicHex =
    "0000000000000000000000000000000000000000000000000000000000000000405a7f7b5b6c507d4d5d5c4e6b604b61f0f1f2f3f4f5f6f7f8f97a5e4c7e6e6f7cc1c2c3c4c5c6c7c8c9d1d2d3d4d5d6d7d8d9e2e3e4e5e6e7e8e900e000006d79818283848586878889919293949596979899a2a3a4a5a6a7a8a9c04fd0a1000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000";
const char* const kRevEbcidkHex =
    "0000000000000000000000000000000000000000000000000000000000000000405a7f7be06c507d4d5d5c4e6b604b61f0f1f2f3f4f5f6f7f8f97a5e4c7e6e6f7cc1c2c3c4c5c6c7c8c9d1d2d3d4d5d6d7d8d9e2e3e4e5e6e7e8e9005b00006d790000000000000000000000000000000000000000000000000000004f000000000000000000000000000000000000000000000000000000000000000000000000414243444546474849515253545556588182838485868788898a8c8d8e8f909192939495969798999a9d9e9fa2a3a4a5a6a7a8a9aaacadaeafbabbbcbdbebf0000000000000000000000000000000000000000000000000000000000000000";
const unsigned char* RevEbcdicTable() {
    static unsigned char t[256];
    static bool init = false;
    if (!init) { DecodeHex256(kRevEbcdicHex, t); init = true; }
    return t;
}
const unsigned char* RevEbcidkTable() {
    static unsigned char t[256];
    static bool init = false;
    if (!init) { DecodeHex256(kRevEbcidkHex, t); init = true; }
    return t;
}

// シフトJIS(hi=先頭,lo=後続) → JISコード（標準アルゴリズム。EUC入力用）。
unsigned int SjisToJis(unsigned char hi, unsigned char lo) {
    int h = hi, l = lo;
    h -= (h <= 0x9f) ? 0x71 : 0xb1;
    h = (h << 1) + 1;
    if (l > 0x9e) { l -= 0x7e; h += 1; }
    else { if (l > 0x7e) l -= 1; l -= 0x1f; }
    return ((h & 0xff) << 8) | (l & 0xff);
}

// JIS区点コード → シフトJIS（原 FUN_0046903d の変換本体を移植。日本語ロケール前提）。
unsigned int JisToSjis(unsigned int jis) {
    unsigned int hi = (jis >> 8) & 0xff;
    unsigned int lo = jis & 0xff;
    if (hi > 0x20 && hi < 0x7f && lo > 0x20 && lo < 0x7f) {
        if ((jis & 0x100) == 0)      lo += 0x7e;   // 上位バイトが偶数
        else if (lo < 0x60)          lo += 0x1f;
        else                         lo += 0x20;
        unsigned int t = (hi - 0x21) >> 1;
        unsigned int s = t + 0x81;
        if (s > 0x9f) s = t + 0xc1;
        return (s << 8) | lo;
    }
    return 0;
}

// BuildCharCells の carry（行をまたぐ 2 セル文字の持ち越し）の意味。
constexpr int kCarryNone     = 0;   // 持ち越しなし
constexpr int kCarryBlank    = 1;   // 直前行の 2 セル文字が行末をまたいだ。空白 1 セルを置く
                                    //   （該当バイトは直前行で消費済みなので読み飛ばさない）
constexpr int kCarrySkipByte = 2;   // 窓の先頭が文字の途中（トレイル/2 バイト目）。
                                    //   空白 1 セルを置き、そのバイトを読み飛ばす

} // namespace

// 窓（表示・印刷・ダンプの開始位置）の先頭バイトが多バイト文字の途中かを判定する。
//   途中なら kCarrySkipByte を返し、BuildCharCells がそのバイトを空白 1 セルに
//   置き換えて読み飛ばす（原と同一。行頭のセルずれを起こさない）。
//   判定は 2 段構え（原と同一）。開始バイト自身が BuildCharCells のペア条件を満たす
//   2 バイト目でなければ、直前がいくら先行バイトでも文字は成立しないため持ち越さない。
int CStirlingView::InitialCarry(int charset, stirling::FileOffset startAbs, stirling::FileOffset /*total*/) {
    if (startAbs <= 0) return kCarryNone;
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr) return kCarryNone;
    unsigned char cur = 0;
    if (charset != 3 && !pDoc->GetByteAt(startAbs, &cur)) return kCarryNone;
    switch (charset) {
    case 3: // Unicode: 2バイト境界。開始オフセットが奇数なら文字の途中
        return (startAbs & 1) ? kCarrySkipByte : kCarryNone;
    case 1: { // SJIS: 直前の DBCS 先頭バイト連続数が奇数なら startAbs はトレイル
        // 開始バイトが有効なトレイル（BuildCharCells と同条件）でなければ、直前の
        //   先行バイトは単独で '.' になり、開始バイトは独立した 1 セルになる。
        if (!(cur >= 0x40 && cur <= 0xfc && cur != 0x7f)) return kCarryNone;
        int run = 0;
    stirling::FileOffset p = startAbs - 1;
        unsigned char b = 0;
        // [byte層] 先行バイト判定はロケール非依存（CP932 固定）。詳細: 20_unicode_layering.md §4.2
        while (p >= 0 && pDoc->GetByteAt(p, &b) && stirling::IsCp932LeadByte(b)) { ++run; --p; }
        return (run & 1) ? kCarrySkipByte : kCarryNone;
    }
    case 2: { // EUC: 直前の主プレーン(0xa1-0xfe)連続数が奇数、または 0x8e の直後のカナ
        if (cur >= 0xa1 && cur <= 0xfe) {           // 主プレーン対の 2 バイト目になり得る
            int run = 0;
    stirling::FileOffset p = startAbs - 1;
            unsigned char b = 0;
            while (p >= 0 && pDoc->GetByteAt(p, &b) && b >= 0xa1 && b <= 0xfe) { ++run; --p; }
            if (run & 1) return kCarrySkipByte;
        }
        unsigned char pb = 0;
        // 半角カナ単一シフト（0x8e）は、直後が 0xa1-0xdf のときだけ 2 バイトを消費する。
        if (cur >= 0xa1 && cur <= 0xdf &&
            pDoc->GetByteAt(startAbs - 1, &pb) && pb == 0x8e) { return kCarrySkipByte; }
        return kCarryNone;
    }
    default: // ASCII / EBCDIC / EBCIDK は1バイト固定
        return kCarryNone;
    }
}

// buf[gi..] から cols セル分の文字欄テキストを構築（原 this+0x344 レンダラ群 / ダンプ
//   レンダラ FUN_0045e0a9 系と同一の byte→セル写像）。gi/carry を更新し、末尾で eof=true。
//   不変条件: 各ソースバイトは 1 セルを消費（DBCSペア=2バイト2セル）。表示とダンプ保存で共用。
std::string CStirlingView::BuildCharCells(const std::vector<unsigned char>& buf, int& gi, int cols,
                                          int cs, int& carry, bool beBig,
                                          const unsigned char* ebc, bool& eof) const {
    const int nbuf = static_cast<int>(buf.size());
    std::string out;
    int col = 0;
    eof = false;
    if (carry != kCarryNone) {
        out.push_back(' '); col = 1;
        // 窓の先頭が文字の途中なら、そのバイトを空白 1 セルに置き換えて読み飛ばす。
        //   行をまたいだ持ち越し（kCarryBlank）は直前行で消費済みなので進めない。
        if (carry == kCarrySkipByte) { ++gi; }
        carry = kCarryNone;
    }
    while (col < cols) {
        if (gi >= nbuf) { eof = true; break; }
        const unsigned char b = buf[gi];
        switch (cs) {
        case 1: {  // SJIS
            if ((b >= 0x20 && b <= 0x7e) || (b >= 0xa1 && b <= 0xdf)) {
                out.push_back(static_cast<char>(b)); ++gi; ++col;
            } else if (stirling::IsCp932LeadByte(b)) {
                if (gi + 1 < nbuf) {
                    const unsigned char t = buf[gi + 1];
                    if (t >= 0x40 && t <= 0xfc && t != 0x7f) {
                        out.push_back(static_cast<char>(b));
                        out.push_back(static_cast<char>(t)); gi += 2; col += 2;
                    } else { out.push_back('.'); ++gi; ++col; }
                } else { out.push_back('.'); ++gi; ++col; }
            } else { out.push_back('.'); ++gi; ++col; }
            break;
        }
        case 2: {  // EUC-JP
            if (b >= 0xa1 && b <= 0xfe) {                 // 主プレーン先頭
                if (gi + 1 < nbuf && buf[gi + 1] >= 0xa1 && buf[gi + 1] <= 0xfe) {
                    const unsigned int jis =
                        ((static_cast<unsigned int>(b) - 0x80) << 8) |
                        (static_cast<unsigned int>(buf[gi + 1]) - 0x80);
                    const unsigned int sj = JisToSjis(jis);
                    if (sj) { out.push_back(static_cast<char>(sj >> 8));
                              out.push_back(static_cast<char>(sj & 0xff)); }
                    else    { out.push_back('.'); out.push_back(' '); }
                    gi += 2; col += 2;
                } else { out.push_back('.'); ++gi; ++col; }
            } else if (b == 0x8e) {                        // 半角カナ単一シフト
                if (gi + 1 < nbuf && buf[gi + 1] >= 0xa1 && buf[gi + 1] <= 0xdf) {
                    out.push_back(static_cast<char>(buf[gi + 1]));
                    out.push_back(' '); gi += 2; col += 2;
                } else { out.push_back('.'); ++gi; ++col; }
            } else if (b >= 0x20 && b <= 0x7e) {
                out.push_back(static_cast<char>(b)); ++gi; ++col;
            } else { out.push_back('.'); ++gi; ++col; }
            break;
        }
        case 3: {  // Unicode（2バイト→UTF-16→SJIS）
            if (gi + 1 < nbuf) {
                const unsigned char b0 = buf[gi], b1 = buf[gi + 1];
                const wchar_t wc = beBig ? static_cast<wchar_t>((b0 << 8) | b1)
                                         : static_cast<wchar_t>(b0 | (b1 << 8));
                char mb[8] = {0};
                BOOL usedDef = FALSE;
                // WC_NO_BEST_FIT_CHARS + usedDef で「SJIS へ写像不可」を厳密に検出する
                // （既定では '?' に置換して成功を返すため無効判定できない）。
                const int mn = ::WideCharToMultiByte(932, WC_NO_BEST_FIT_CHARS, &wc, 1,
                                                     mb, sizeof(mb), nullptr, &usedDef);
                if (mn <= 0 || usedDef) {
                    out.push_back('.'); out.push_back('.');   // 変換不可: 2セルとも '.'（原と一致）
                } else if (mn == 2) {
                    out.push_back(mb[0]); out.push_back(mb[1]);
                } else {
                    unsigned char c = static_cast<unsigned char>(mb[0]);
                    if (!((c >= 0x20 && c <= 0x7e) || (c >= 0xa1 && c <= 0xdf))) c = '.';
                    out.push_back(static_cast<char>(c)); out.push_back(' ');
                }
                gi += 2; col += 2;
            } else { out.push_back('.'); ++gi; ++col; }
            break;
        }
        case 4: case 5: {  // EBCDIC / EBCIDK（1バイト変換表）
            const unsigned char t = ebc[b];
            out.push_back(t ? static_cast<char>(t) : '.'); ++gi; ++col;
            break;
        }
        default: {         // ASCII
            out.push_back((b >= 0x20 && b <= 0x7e) ? static_cast<char>(b) : '.');
            ++gi; ++col;
            break;
        }
        }
    }
    carry = (col > cols) ? kCarryBlank : kCarryNone;   // 2セル単位が行末をまたいだら次行先頭に空白
    return out;
}

// ---------------------------------------------------------------------------
// UTF-8 文字欄（charset 6。移植で追加。Issue #98）
//   他の文字セットは CP932 バイト列を ExtTextOutA で描くが、UTF-8 は CP932 に無い
//   文字（ハングル・簡体字・絵文字など）も表示するためワイド描画にする。
//   不変条件は同じ: 1 ソースバイト = 1 表示セル。多バイト文字はバイト数ぶんのセルを
//   占め、グリフが使い切らなかったセルは空白で埋める。
//     例: "あ"(E3 81 82) = 3 セル → 二倍幅グリフ(2セル) + 空白1セル
//         "e acute"(C3 A9) = 2 セル → 一倍幅グリフ(1セル) + 空白1セル
//   不正・不完全な列は 1 バイトずつ '.' にして桁を保つ。
// ---------------------------------------------------------------------------

// コードポイントのグリフが 1 セル幅か 2 セル幅か。
//   East Asian Width の表を持たず、実際に使うフォントで GDI に実測させる
//   （フォント差で見え方がずれないようにするため）。BMP は cache（0=未測定/1/2）へ
//   記録する。cache は空でもよい（その場合は毎回測る）。
static int Utf8GlyphCells(HDC hdc, unsigned int cp, int charW, std::vector<unsigned char>* cache) {
    if (charW <= 0) { return 1; }
    if (cache != nullptr && cp < cache->size() && (*cache)[cp] != 0) {
        return (*cache)[cp];
    }
    int width = 0;
    if (cp < 0x10000) {
        const wchar_t wc = static_cast<wchar_t>(cp);
        int w = 0;
        if (!::GetCharWidth32W(hdc, wc, wc, &w)) {
            SIZE sz = {0, 0};
            if (::GetTextExtentPoint32W(hdc, &wc, 1, &sz)) { w = sz.cx; }
        }
        width = w;
    } else {
        const unsigned int v = cp - 0x10000;
        const wchar_t pair[2] = { static_cast<wchar_t>(0xD800 + (v >> 10)),
                                  static_cast<wchar_t>(0xDC00 + (v & 0x3FF)) };
        SIZE sz = {0, 0};
        if (::GetTextExtentPoint32W(hdc, pair, 2, &sz)) { width = sz.cx; }
    }
    // 1.5 セルを超える送りは二倍幅とみなす（半角と全角の中間に落ちる字形を全角側へ寄せる）。
    const int cells = (width * 2 > charW * 3) ? 2 : 1;
    if (cache != nullptr && cp < cache->size()) {
        (*cache)[cp] = static_cast<unsigned char>(cells);
    }
    return cells;
}

// 表示できない制御文字か（他の文字セットと同じく '.' にする）。
//   C0（U+0000-U+001F）/ DEL / C1（U+0080-U+009F）。
static bool Utf8IsControl(unsigned int cp) {
    return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F);
}

// buf[gi..] から cols セル分の UTF-8 文字欄を構築する。
//   out: 描画するワイド文字列 / dx: out の各文字の送り幅（ExtTextOutW へ渡す）。
//   carryCells: 直前の行からはみ出したセル数（行頭に空白で詰める）。更新する。
//   グリフがセルを使い切らない分は空白で埋めるため、out.size() と消費セル数は一致しない。
//   行末をまたぐ文字はそのまま描き切り、はみ出したセル数を carryCells へ残す
//   （既存の文字セットが 2 セル文字で 1 セルはみ出すのと同じ扱い）。
void CStirlingView::BuildCharCellsUtf8(const std::vector<unsigned char>& buf, int& gi, int cols,
                                       int& carryCells, HDC hdc, int charW,
                                       std::vector<unsigned char>* cache,
                                       std::wstring& out, std::vector<INT>& dx,
                                       int& cellsOut, bool& eof) const {
    const int nbuf = static_cast<int>(buf.size());
    out.clear();
    dx.clear();
    eof = false;
    cellsOut = 0;
    int col = 0;

    // 直前の行からのはみ出しを空白で詰める。
    for (; carryCells > 0 && col < cols; --carryCells, ++col) {
        out.push_back(L' ');
        dx.push_back(charW);
    }
    if (carryCells > 0) { cellsOut = col; return; }   // 1 行がはみ出しだけで埋まった

    while (col < cols) {
        if (gi >= nbuf) { eof = true; break; }
        const stirling::Utf8Decoded d =
            stirling::DecodeUtf8(&buf[gi], static_cast<size_t>(nbuf - gi));
        if (!d.ok) {
            // 不正な列も、バッファ端で途切れた列も 1 バイト = 1 セルの '.' にする。
            //   途切れた列は次の窓で読み直されるため、ここで先読みはしない。
            out.push_back(L'.');
            dx.push_back(charW);
            ++gi; ++col;
            continue;
        }
        const int bytes = d.length;
        int glyphCells = 1;
        if (Utf8IsControl(d.codePoint)) {
            // 制御文字はバイト数ぶんの '.' にする（1 バイト = 1 セルを保つ）。
            for (int i = 0; i < bytes; ++i) { out.push_back(L'.'); dx.push_back(charW); }
            gi += bytes; col += bytes;
            continue;
        }
        glyphCells = Utf8GlyphCells(hdc, d.codePoint, charW, cache);
        if (glyphCells > bytes) { glyphCells = bytes; }   // セル数はバイト数を超えない

        if (d.codePoint < 0x10000) {
            out.push_back(static_cast<wchar_t>(d.codePoint));
            dx.push_back(glyphCells * charW);
        } else {
            const unsigned int v = d.codePoint - 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (v >> 10)));
            dx.push_back(glyphCells * charW);
            out.push_back(static_cast<wchar_t>(0xDC00 + (v & 0x3FF)));
            dx.push_back(0);   // サロゲートペアは 2 コード単位で 1 グリフ。送りは先頭に集約する
        }
        for (int i = glyphCells; i < bytes; ++i) {   // 余ったセルは空白で埋める
            out.push_back(L' ');
            dx.push_back(charW);
        }
        gi += bytes;
        col += bytes;
    }
    cellsOut = col;
    carryCells = (col > cols) ? (col - cols) : 0;
}

// 窓の先頭が UTF-8 の多バイト文字の途中なら、読み飛ばすバイト数（0..3）を返す。
//   手前 3 バイトと、そこから 4 バイトを読んで core の判定へ渡す（列全体の妥当性を
//   見るため。壊れた列なら 0 を返し、各バイトが独立した '.' セルになる）。
int CStirlingView::InitialCarryUtf8(stirling::FileOffset startAbs) {
    if (startAbs <= 0) { return 0; }
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr) { return 0; }
    const stirling::FileOffset back = (startAbs >= 3) ? 3 : startAbs;
    unsigned char win[7] = {0};
    size_t n = 0;
    for (stirling::FileOffset p = startAbs - back; p < startAbs + 4; ++p) {
        unsigned char b = 0;
        if (!pDoc->GetByteAt(p, &b)) { break; }   // データ末尾。読めた範囲で判定する
        win[n++] = b;
        if (n >= sizeof(win)) { break; }
    }
    return stirling::Utf8CarryBytesAt(win, n, static_cast<size_t>(back));
}

void CStirlingView::DrawCharColumn(CDC* pDC, stirling::FileOffset firstRow, int rows,
                                   stirling::FileOffset total) {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr || rows <= 0) return;
    const int bpr = m_bytesPerRow;
    const int cs = pDoc->GetCharset();
    const stirling::FileOffset start = firstRow * bpr;
    if (start >= total) return;
    int winLen = rows * bpr;
    if (start + winLen > total) { winLen = static_cast<int>(total - start); }
    // UTF-8 は行末で文字が途切れると次行のセル数が変わるため、窓の末尾を最大 3 バイト
    //   だけ余分に読む（余分は復号のためだけで、セルとしては描かない）。
    int readLen = winLen;
    if (cs == 6 && start + readLen + 3 <= total) { readLen += 3; }
    else if (cs == 6 && start + readLen < total) { readLen = static_cast<int>(total - start); }
    std::vector<unsigned char> buf = pDoc->ReadRange(start, readLen);
    const int nbuf = static_cast<int>(buf.size());
    if (nbuf <= 0) return;

    std::vector<INT> dx(static_cast<size_t>(bpr) + 4, m_charW);  // 1バイト=charW
    const int x = ColCharX();
    const bool beBig = pDoc->IsByteOrderBig();
    const unsigned char* ebc = (cs == 4) ? EbcdicTable() : (cs == 5) ? EbcidkTable() : nullptr;

    pDC->SetTextColor(m_clrDataText);
    pDC->SetBkColor(m_clrDataBack);

    if (cs == 6) {   // UTF-8（ワイド描画。Issue #98）
        DrawCharColumnUtf8(pDC, start, rows, bpr, buf, x);
        return;
    }

    int gi = 0;
    int carry = InitialCarry(cs, start, total);
    for (int r = 0; r < rows; ++r) {
        const stirling::FileOffset rowAbs = start + r * bpr;
        if (rowAbs >= total) break;
        const int y = (r + 1) * m_rowH;    // +1: ヘッダ1行分
        bool eof = false;
        const std::string out = BuildCharCells(buf, gi, bpr, cs, carry, beBig, ebc, eof);
        if (!out.empty()) {
            // [byte層] 編集対象バイト列を CP932 として描画する。ワイド化しないこと。
            //   理由: dx 配列がバイト単位（16進欄と桁を揃える）／不正バイト列を保持する。
            //   フォントは SHIFTJIS_CHARSET 固定のため、GDI はシステム ANSI コード
            //   ページではなくフォント charset の CP932 で解釈する。
            //   詳細: analysis_artifacts/docs/20_unicode_layering.md §4.1
            ::ExtTextOutA(pDC->GetSafeHdc(), x, y, 0, nullptr,
                          out.c_str(), static_cast<UINT>(out.size()), dx.data());
        }
        if (eof) break;
    }
}

// UTF-8 文字欄の描画（Issue #98）。桁幅・行高は既存フォントのまま、文字欄だけ
//   DEFAULT_CHARSET のフォントでワイド描画する。dx で各グリフの送りを指定するため、
//   16進欄との桁ずれは起きない。
void CStirlingView::DrawCharColumnUtf8(CDC* pDC, stirling::FileOffset start, int rows, int bpr,
                                       const std::vector<unsigned char>& buf, int x) {
    if (m_utf8CellWidth.size() != 0x10000) { m_utf8CellWidth.assign(0x10000, 0); }
    const stirling::ScopedSelectFont selFont(pDC, &m_fontUtf8);
    HDC hdc = pDC->GetSafeHdc();

    int gi = InitialCarryUtf8(start);       // 窓の先頭が文字の途中なら、そのバイトを飛ばす
    int carryCells = gi;                    // 飛ばしたバイトも窓の中＝同じ数の空白セルを置く
    std::wstring out;
    std::vector<INT> dx;
    for (int r = 0; r < rows; ++r) {
        const int y = (r + 1) * m_rowH;     // +1: ヘッダ1行分
        bool eof = false;
        int cells = 0;
        BuildCharCellsUtf8(buf, gi, bpr, carryCells, hdc, m_charW, &m_utf8CellWidth,
                           out, dx, cells, eof);
        if (!out.empty()) {
            ::ExtTextOutW(hdc, x, y, 0, nullptr, out.c_str(),
                          static_cast<UINT>(out.size()), dx.data());
        }
        if (eof) break;
    }
}

// ---- 文字セット / バイトオーダ 切替コマンド ----

void CStirlingView::OnCharset(UINT nID) {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr) return;
    int cs;
    switch (nID) {
    case ID_CHARSET_ASCII:   cs = 0; break;
    case ID_CHARSET_SJIS:    cs = 1; break;
    case ID_CHARSET_EUC:     cs = 2; break;
    case ID_CHARSET_UNICODE: cs = 3; break;
    case ID_CHARSET_EBCDIC:  cs = 4; break;
    case ID_CHARSET_EBCIDK:  cs = 5; break;
    case ID_CHARSET_UTF8:    cs = 6; break;   // 移植で追加（Issue #98）
    default: return;
    }
    if (cs != pDoc->GetCharset()) {
        pDoc->SetCharset(cs);
        m_nibbleLow = false;
        pDoc->UpdateAllViews(nullptr);
    }
}

// UTF-8 は既存の ID 範囲（0x8053-0x8057）と連続しないため単独のハンドラで受ける。
void CStirlingView::OnCharsetUtf8() { OnCharset(ID_CHARSET_UTF8); }

void CStirlingView::OnUpdateCharset(CCmdUI* pCmdUI) {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr) { pCmdUI->Enable(FALSE); return; }
    int cs = -1;
    switch (pCmdUI->m_nID) {
    case ID_CHARSET_ASCII:   cs = 0; break;
    case ID_CHARSET_SJIS:    cs = 1; break;
    case ID_CHARSET_EUC:     cs = 2; break;
    case ID_CHARSET_UNICODE: cs = 3; break;
    case ID_CHARSET_EBCDIC:  cs = 4; break;
    case ID_CHARSET_EBCIDK:  cs = 5; break;
    case ID_CHARSET_UTF8:    cs = 6; break;   // 移植で追加（Issue #98）
    }
    pCmdUI->SetRadio(cs == pDoc->GetCharset() ? TRUE : FALSE);
}

void CStirlingView::OnByteOrder(UINT nID) {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr) return;
    const bool big = (nID == ID_BYTEORDER_BIG);
    if (big != pDoc->IsByteOrderBig()) {
        pDoc->SetByteOrderBig(big);
        pDoc->UpdateAllViews(nullptr);
    }
}

void CStirlingView::OnUpdateByteOrder(CCmdUI* pCmdUI) {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr) { pCmdUI->Enable(FALSE); return; }
    const bool big = (pCmdUI->m_nID == ID_BYTEORDER_BIG);
    pCmdUI->SetRadio(big == pDoc->IsByteOrderBig() ? TRUE : FALSE);
}

// ===========================================================================
// 文字セット別入力（原 CStirlingView_TranslateInputChar / Insert/OverwriteTextBytes）
//   入力文字(WM_CHAR/WM_IME_CHAR。既定コードページ=SJIS。全角は hi=先頭/lo=後続バイト)を
//   現文字セットのバイト列（ファイル格納順）へ変換する。
// ===========================================================================
std::vector<unsigned char> CStirlingView::TranslateInputChar(int charset, unsigned int ch) {
    std::vector<unsigned char> out;
    const bool dbcs = (ch >= 0x100);
    const unsigned char lo = static_cast<unsigned char>(ch & 0xff);         // 単バイト / DBCS後続
    const unsigned char hi = static_cast<unsigned char>((ch >> 8) & 0xff);  // DBCS先頭

    switch (charset) {
    case 0:   // ASCII
    case 1:   // シフトJIS（入力がそのまま SJIS）
        if (dbcs) { out.push_back(hi); out.push_back(lo); }
        else        out.push_back(lo);
        break;

    case 2:   // EUC-JP
        if (!dbcs) {
            if (lo >= 0xa1 && lo <= 0xdf) { out.push_back(0x8e); out.push_back(lo); } // 半角カナ
            else                            out.push_back(lo);
        } else {
            // SJIS(hi,lo) → JIS → EUC(各バイト|0x80)。原 FUN_004690a5|0x8080 に一致。
            const unsigned int jis = SjisToJis(hi, lo);
            out.push_back(static_cast<unsigned char>(((jis >> 8) & 0xff) | 0x80));
            out.push_back(static_cast<unsigned char>((jis & 0xff) | 0x80));
        }
        break;

    case 3: {  // Unicode（SJIS → UTF-16、リトルエンディアン格納。原も入力は常にLE）
        wchar_t wc = 0;
        if (!dbcs) {
            const char s[1] = { static_cast<char>(lo) };
            ::MultiByteToWideChar(932, 0, s, 1, &wc, 1);
        } else {
            const char s[2] = { static_cast<char>(hi), static_cast<char>(lo) };
            ::MultiByteToWideChar(932, 0, s, 2, &wc, 1);
        }
        out.push_back(static_cast<unsigned char>(wc & 0xff));
        out.push_back(static_cast<unsigned char>((wc >> 8) & 0xff));
        break;
    }

    case 4:    // EBCDIC
    case 5: {  // EBCIDK
        const unsigned char* rev = (charset == 4) ? RevEbcdicTable() : RevEbcidkTable();
        if (dbcs) {
            const unsigned char e1 = rev[hi] ? rev[hi] : hi;   // 写像なしは元バイト保持（原挙動）
            const unsigned char e2 = rev[lo] ? rev[lo] : lo;
            out.push_back(e1); out.push_back(e2);
        } else {
            out.push_back(rev[lo] ? rev[lo] : lo);
        }
        break;
    }

    default:
        out.push_back(lo);
        break;
    }
    return out;
}

void CStirlingView::InputTextChar(unsigned int ch) {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr) return;
    InputBytes(TranslateInputChar(pDoc->GetCharset(), ch));
}

// ワイド入力（WM_CHAR / WM_IME_CHAR の UTF-16 コード単位）を 1 文字ぶん処理する。
//   UTF-8 のときは CP932 を経由せずワイドから直接符号化する（Issue #98）。
//   CP932 に無い文字も入力できるようにするため。他の文字セットは従来どおり
//   CP932 のコード値へ落としてから文字セット別の変換にかける。
void CStirlingView::InputWideChar(UINT unit) {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr) return;
    const wchar_t wc = static_cast<wchar_t>(unit);
    if (pDoc->GetCharset() == 6) {
        // サロゲートペアは 2 回に分けて届く。上位を保持して下位が来たときに合成する。
        if (wc >= 0xD800 && wc <= 0xDBFF) { m_pendingHighSurrogate = wc; return; }
        unsigned int cp = static_cast<unsigned int>(wc);
        if (wc >= 0xDC00 && wc <= 0xDFFF) {
            if (m_pendingHighSurrogate == 0) { return; }   // 対になっていない下位は捨てる
            cp = 0x10000u + ((static_cast<unsigned int>(m_pendingHighSurrogate) - 0xD800u) << 10)
                 + (static_cast<unsigned int>(wc) - 0xDC00u);
        }
        m_pendingHighSurrogate = 0;
        std::vector<unsigned char> bytes;
        if (!stirling::EncodeUtf8(cp, bytes)) { return; }
        InputBytes(bytes);
        return;
    }
    m_pendingHighSurrogate = 0;
    unsigned int cp932 = 0;
    if (WideInputCharToCp932(unit, cp932)) { InputTextChar(cp932); }
}

// 変換済みバイト列を現在のモードで挿入／上書きする（原 InputTextChar の後半）。
void CStirlingView::InputBytes(std::vector<unsigned char> bytes) {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr || bytes.empty()) return;
    const int n = static_cast<int>(bytes.size());

    if (m_selActive) {
        // 選択範囲を変換バイト列で置換（単一Undoレコード）
        const stirling::FileOffset lo = SelLo();
        const stirling::FileOffset len = SelHi() - lo;
        if (!pDoc->ReplaceRange(lo, len, bytes)) {
            return;   // 上限超過の確認で中止された等。選択を維持（Issue #30）
        }
        m_selActive = false;
        m_nibbleLow = false;
        AfterEdit(lo + n);
        return;
    }

    const stirling::FileOffset pos = m_caretPos;
    const stirling::FileOffset total = Total();
    // 上書きモード: 変換後の n バイトを上書き（in-place。EOF 付近は伸長）。挿入モードは挿入。
    if (pDoc->IsOverwriteMode()) {
        stirling::FileOffset room = total - pos;                 // 末尾までの上書き可能バイト数
        if (room < 0) { room = 0; }
        // 末尾自動挿入 OFF: 上書きモードでは末尾を越えて付加しない（原 endAutoInsert）。
        //   末尾では拒否、末尾を跨ぐ多バイト文字は収まる分のみ上書きしはみ出しは捨てる。
        if (!theApp.AppSettings().endAutoInsert && n > room) {
            if (room == 0) { ::MessageBeep(0); return; }
            bytes.resize(static_cast<size_t>(room));   // room < n <= バイト列長のため縮小のみ
        }
        stirling::FileOffset del = room;
        if (del > static_cast<stirling::FileOffset>(bytes.size())) {
            del = static_cast<stirling::FileOffset>(bytes.size());
        }
        pDoc->ReplaceRange(pos, del, bytes);
    } else {
        pDoc->ReplaceRange(pos, 0, bytes);   // 挿入（単一Undoレコード）
    }
    m_nibbleLow = false;
    AfterEdit(pos + static_cast<int>(bytes.size()));
}

// IME 確定の全角文字（既定処理を呼ばず二重入力を防ぐ）。文字ペインでのみ受理。
LRESULT CStirlingView::OnImeChar(WPARAM wParam, LPARAM /*lParam*/) {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc != nullptr) {
        if (!pDoc->CanEdit()) {
            ::MessageBeep(0);   // 編集禁止中は IME 確定入力も拒否
        } else if (m_activePane == 1) {
            InputWideChar(static_cast<UINT>(wParam));
        } else {
            ::MessageBeep(0);   // 16進ペインでは全角入力不可（原挙動）
        }
    }
    return 0;
}

// ===========================================================================
// マーク（原 GetByteColor のマーク色＋マーク登録/移動/解除コマンド）
// ===========================================================================
bool CStirlingView::GetByteColor(stirling::FileOffset absPos, unsigned char byteVal,
                                 COLORREF& fg, COLORREF& bg) const {
    // 比較モード（原 view+0x300 && view+0x2fc）: 相違範囲は比較色、他はデータ色。
    //   原と同じくマーク/検索色は比較モードでは表示しない。
    if (m_compareActive && !m_compareDiffs.empty()) {
        const CStirlingSettings& s = CurSettings();
        if (InCompareDiff(absPos)) {
            fg = s.compareText;
            bg = s.compareBack;
        } else {
            fg = m_clrDataText;
            bg = m_clrDataBack;
        }
        return false;
    }

    CStirlingDoc* pDoc = GetDocument();
    int type = 0;
    if (pDoc != nullptr && pDoc->GetMark(absPos, &type) && type >= 0 && type <= 2) {
        const CStirlingSettings& s = CurSettings();
        fg = s.markText[type];
        bg = s.markBack[type];
        return true;
    }
    // 構造体編集の表示範囲（原 view+0x30c/0x304/0x308）: 範囲内は青文字（マークの次点）。
    if (m_structHiliteActive && absPos >= m_structHiliteStart && absPos <= m_structHiliteEnd) {
        const CStirlingSettings& s = CurSettings();
        fg = s.structText;
        bg = s.structBack;
        return false;
    }
    // 既定枝（原 view+0x40 バイト値別色表）: fg=強調コード反映済のバイト色 / bg=データ背景色。
    fg = m_byteColorTable[byteVal];
    bg = m_clrDataBack;
    return false;
}

// 構造体編集の表示範囲[start,end]（両端含む）を設定し再描画。
void CStirlingView::SetStructHighlight(stirling::FileOffset start, stirling::FileOffset end) {
    if (m_structHiliteActive && m_structHiliteStart == start && m_structHiliteEnd == end) {
        return;   // 変化なし
    }
    m_structHiliteActive = true;
    m_structHiliteStart = start;
    m_structHiliteEnd = end;
    Invalidate(FALSE);
}

void CStirlingView::ClearStructHighlight() {
    if (!m_structHiliteActive) return;
    m_structHiliteActive = false;
    m_structHiliteEnd = -1;
    Invalidate(FALSE);
}

// pos が相違範囲[start,end]（昇順）内か（二分探索）。
bool CStirlingView::InCompareDiff(stirling::FileOffset pos) const {
    // start <= pos となる最後の範囲を探し、その end を確認する。
    int lo = 0, hi = static_cast<int>(m_compareDiffs.size()) - 1, found = -1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (m_compareDiffs[mid].first <= pos) { found = mid; lo = mid + 1; }
        else { hi = mid - 1; }
    }
    return found >= 0 && pos <= m_compareDiffs[found].second;
}

void CStirlingView::SetCompareResult(const std::vector<std::pair<stirling::FileOffset, stirling::FileOffset>>& diffs) {
    m_compareDiffs = diffs;
    m_compareActive = true;
    Invalidate(FALSE);
}

void CStirlingView::ClearCompareResult() {
    if (m_compareActive || !m_compareDiffs.empty()) {
        m_compareDiffs.clear();
        m_compareActive = false;
        Invalidate(FALSE);
    }
}

// 強調表示ON/OFF（差分は保持。原 view+0x300 = FUN_0045d228）。
void CStirlingView::SetCompareHighlight(bool on) {
    if (m_compareActive != on) {
        m_compareActive = on;
        Invalidate(FALSE);
    }
}

// 相違範囲[start,end]（両端含む）を選択し縦中央へ（ジャンプ。原 msg 0x410）。
void CStirlingView::GotoCompareDiff(stirling::FileOffset start, stirling::FileOffset end) {
    const stirling::FileOffset total = Total();
    stirling::FileOffset lo = start, hi = end + 1;   // end は両端含む → 半開境界 hi=end+1
    if (lo < 0) { lo = 0; }
    if (lo > total) { lo = total; }
    if (hi > total) { hi = total; }
    m_selAnchor = lo;
    m_caretPos  = hi;
    m_selActive = (hi > lo);
    m_nibbleLow = false;
    CenterCaretRow();   // 内部で SyncPropagate 済（画面外なら縦中央＋同期）
    Invalidate(FALSE);
    UpdateCaret();
    SetFocus();
}

// 2ビュー連動（相違一覧ダイアログ用）。グループ機構へ委譲する。
//   enabled かつ partner 有効 → {this, partner} の2要素グループを構成。
//   無効化 → 自分と旧相手のグループを解除。
void CStirlingView::SetSyncPartner(CStirlingView* partner, bool enabled) {
    if (enabled && partner != nullptr) {
        ApplySyncGroup({ this, partner });
    } else {
        ApplySyncGroup({ this });   // 自分のみ＝グループ解除
    }
}

// 同期グループを確定する（原 OnSyncScroll: FUN_0044d6a9 のグループ再構成ロジック）。
//   members は自分を含む全メンバー。
//   1) 旧グループの各相手の同期配列を解除する。
//   2) 新メンバー全員の同期配列を「自分以外の全メンバー」に設定する（対称・完全連結）。
void CStirlingView::ApplySyncGroup(const std::vector<CStirlingView*>& members) {
    // 1) 旧相手を解除（このビューの旧配列＝旧グループの全メンバー）。
    for (CStirlingView* old : m_syncGroup) {
        if (old != nullptr) { old->m_syncGroup.clear(); }
    }
    m_syncGroup.clear();
    // 2) 新メンバー全員へ「自分以外」を設定。
    for (CStirlingView* m : members) {
        if (m == nullptr) { continue; }
        m->m_syncGroup.clear();
        for (CStirlingView* other : members) {
            if (other != nullptr && other != m) {
                m->m_syncGroup.push_back(other);
            }
        }
    }
}

// 開いている全 CStirlingView を列挙（原 FUN_0044cbad: 全テンプレート→全ドキュメント→全ビュー）。
void CStirlingView::EnumAllViews(std::vector<CStirlingView*>& out) {
    out.clear();
    CWinApp* pApp = AfxGetApp();
    if (pApp == nullptr) { return; }
    POSITION posTmpl = pApp->GetFirstDocTemplatePosition();
    while (posTmpl != nullptr) {
        CDocTemplate* pTmpl = pApp->GetNextDocTemplate(posTmpl);
        if (pTmpl == nullptr) { continue; }
        POSITION posDoc = pTmpl->GetFirstDocPosition();
        while (posDoc != nullptr) {
            CDocument* pDoc = pTmpl->GetNextDoc(posDoc);
            if (pDoc == nullptr) { continue; }
            POSITION posView = pDoc->GetFirstViewPosition();
            while (posView != nullptr) {
                CView* pView = pDoc->GetNextView(posView);
                if (CStirlingView* pSV = DYNAMIC_DOWNCAST(CStirlingView, pView)) {
                    out.push_back(pSV);
                }
            }
        }
    }
}

// シンクロスクロール手動登録ダイアログ（原 CStirlingView_OnSyncScroll @0045bf5b → FUN_0044d6a9）。
//   全ウィンドウ数が2未満なら Beep のみ。2以上ならダイアログを表示しグループを編集する。
void CStirlingView::OnSyncScroll() {
    std::vector<CStirlingView*> all;
    EnumAllViews(all);
    if (all.size() < 2) {
        ::MessageBeep(0);
        return;
    }
    CSyncScrollDlg dlg(this);
    dlg.DoModal();   // グループ確定は dlg 内 OnOK が ApplySyncGroup を呼ぶ
}

// ウィンドウサイズ補正（0x8049, 原 FUN_00449d39）。
//   子フレームを「アドレス／16進／文字欄が収まる幅」＋「最大17行分の高さ」へ整える。
//   幅は charW*(bpr*4+15)+枠、MDIクライアント幅で上限クランプ。
//   高さは rowH*min(可視行+1,17)+枠、現在の子フレーム高で下限クランプ。
//   最大化/最小化中は補正しない（ビープ）。
void CStirlingView::OnAdjustWindowSize() {
    CFrameWnd* pChild = GetParentFrame();   // MDI 子フレーム
    if (pChild == nullptr || pChild->IsZoomed() || pChild->IsIconic()) {
        ::MessageBeep(0);
        return;
    }
    // フォントメトリクス（charW/rowH）を確定させる。
    if (m_charW <= 0 || m_rowH <= 0) {
        CClientDC dc(this);
        EnsureFont(&dc);
    }

    // 幅の上限＝MDIクライアント幅、高さの下限＝現在の子フレーム高。
    CWnd* pMdiClient = pChild->GetParent();
    CRect rcClient, rcChild;
    if (pMdiClient != nullptr) {
        pMdiClient->GetWindowRect(&rcClient);
    }
    pChild->GetWindowRect(&rcChild);

    // 必要クライアント寸法（実レイアウト幅＋縦SB / ヘッダ含む最大17行）→ ウィンドウ寸法を
    //   AdjustWindowRectEx で正確に算出（モダンWindowsのフレーム＋パディング境界を含む）。
    int rows = VisibleRows() + 1;
    if (rows > 17) { rows = 17; }
    CRect rc(0, 0, ContentWidthPx() + ::GetSystemMetrics(SM_CXVSCROLL), m_rowH * rows);
    ::AdjustWindowRectEx(&rc, pChild->GetStyle() & ~(WS_HSCROLL | WS_VSCROLL),
                         FALSE, pChild->GetExStyle());
    int width = rc.Width();
    int height = rc.Height();

    // クランプ（原: 幅=min(計算, MDIクライアント幅) / 高=max(計算, 現子フレーム高)）。
    if (pMdiClient != nullptr && width > rcClient.Width()) {
        width = rcClient.Width();
    }
    if (height < rcChild.Height()) {
        height = rcChild.Height();
    }

    pChild->SetWindowPos(nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER);
    // 実測補正: 横スクロール余地が残っていれば、その分だけ幅を広げる（高さは維持）。
    for (int i = 0; i < 2; ++i) {
        const int extra = MaxHScroll();
        if (extra <= 0) { break; }
        CRect cw; pChild->GetWindowRect(&cw);
        width = cw.Width() + extra;
        if (pMdiClient != nullptr && width > rcClient.Width()) { width = rcClient.Width(); break; }
        pChild->SetWindowPos(nullptr, 0, 0, width, cw.Height(), SWP_NOMOVE | SWP_NOZORDER);
    }
}

// ビュー破棄時: 所有する相違一覧ダイアログを閉じ、同期相手の参照も相互に切る。
void CStirlingView::OnDestroy() {
    // キャレット位置の自動復元用に、クローズ時のキャレット位置をパス別に記録する
    //   （原 caretAutoRestore の MRU 並列配列更新に相当。設定OFFでも記録し、次回ON時に有効）。
    if (CStirlingDoc* pDoc = GetDocument()) {
        const CString path = pDoc->GetPathName();
        if (!path.IsEmpty()) { theApp.RecordCaretPos(path, m_caretPos); }
    }
    if (m_pDiffDlg != nullptr && ::IsWindow(m_pDiffDlg->GetSafeHwnd())) {
        m_pDiffDlg->DestroyWindow();   // Cleanup→PostNcDestroy（自己破棄・参照クリア）
    }
    m_pDiffDlg = nullptr;

    // 対象ビューとして参照されている一覧も閉じる。OnViewDestroyed() は対象外を
    // 無視するため、全ビューを走査し、自己破棄後は各反復でポインターを再取得する。
    std::vector<CStirlingView*> views;
    EnumAllViews(views);
    for (CStirlingView* view : views) {
        if (view == nullptr || view == this) { continue; }
        CDiffListDlg* pDlg = view->DiffDlg();
        if (pDlg == nullptr || !::IsWindow(pDlg->GetSafeHwnd())) { continue; }
        pDlg->OnViewDestroyed(this);
    }

    // 同期グループから自分を除去（このビュー解放後のアクセスを防ぐ）。相手側の配列からも自分を外す。
    for (CStirlingView* p : m_syncGroup) {
        if (p != nullptr) {
            p->m_syncGroup.erase(
                std::remove(p->m_syncGroup.begin(), p->m_syncGroup.end(), this),
                p->m_syncGroup.end());
        }
    }
    m_syncGroup.clear();
    // 構造体編集バーの参照を無効化（このビュー解放後のアクセス防止）。
    if (CMainFrame* pFrame = DYNAMIC_DOWNCAST(CMainFrame, AfxGetMainWnd())) {
        pFrame->NotifyViewDestroyed(this);
    }
    CView::OnDestroy();
}

// 先頭行を同期グループ全員へ伝播。各相手の SetTopLineDirect は再伝播しない（再帰回避）。
void CStirlingView::SyncPropagate() {
    for (CStirlingView* p : m_syncGroup) {
        if (p != nullptr) { p->SetTopLineDirect(m_topLine); }
    }
}

// 同期用: 先頭行を直接設定（伝播しない＝再帰回避）。
void CStirlingView::SetTopLineDirect(stirling::FileOffset top) {
    int visRows = FullyVisibleRows();
    if (visRows <= 0) { visRows = 1; }
    stirling::FileOffset maxTop = TotalRows() - visRows;
    if (maxTop < 0) { maxTop = 0; }
    if (top < 0) { top = 0; }
    if (top > maxTop) { top = maxTop; }
    if (top != m_topLine) {
        m_topLine = top;
        SetScrollPos(SB_VERT, RowToScrollPos(m_topLine), TRUE);
        Invalidate(FALSE);
        UpdateCaret();
    }
}

void CStirlingView::ToggleMarkAt(int type) {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr) { return; }
    if (m_caretPos >= Total()) {         // 末尾(データ無し)にはマークできない（原挙動）
        ::MessageBeep(0);
        return;
    }
    pDoc->ToggleMark(m_caretPos, type);  // 未登録→登録/同種別→解除/別種別→変更（原 FUN_0042943a）
    pDoc->UpdateAllViews(nullptr);
}

void CStirlingView::OnMarkToggle()  { ToggleMarkAt(0); }   // Mark1（メニュー/0x804a）
void CStirlingView::OnMark2Toggle() { ToggleMarkAt(1); }   // Mark2（0x8062, メニュー非露出）
void CStirlingView::OnMark3Toggle() { ToggleMarkAt(2); }   // Mark3（0x8063, メニュー非露出）

void CStirlingView::OnUpdateMarkToggle(CCmdUI* pCmdUI) {
    // キャレットが実データ上（末尾の後ろでない）のときのみ有効（原挙動）。Mark1/2/3 共通。
    pCmdUI->Enable(m_caretPos < Total());
}

void CStirlingView::OnMarkNext() {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr || !pDoc->HasMarks()) { ::MessageBeep(0); return; }
    const stirling::FileOffset pos = pDoc->NextMark(m_caretPos);
    if (pos >= 0) { JumpToMark(pos); }   // 画面外なら縦中央へスクロール（一覧の実行と同挙動）
}

void CStirlingView::OnMarkPrev() {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr || !pDoc->HasMarks()) { ::MessageBeep(0); return; }
    const stirling::FileOffset pos = pDoc->PrevMark(m_caretPos);
    if (pos >= 0) { JumpToMark(pos); }   // 画面外なら縦中央へスクロール（一覧の実行と同挙動）
}

void CStirlingView::OnMarkClearAll() {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr || !pDoc->HasMarks()) { ::MessageBeep(0); return; }
    pDoc->ClearMarks();
    pDoc->UpdateAllViews(nullptr);
}

void CStirlingView::OnUpdateMarkExists(CCmdUI* pCmdUI) {
    CStirlingDoc* pDoc = GetDocument();
    pCmdUI->Enable(pDoc != nullptr && pDoc->HasMarks());
}

// マーク一覧ダイアログ（原 CMarkListDlg / FUN_0044d18a）。モーダル。
//   実行(IDOK)で選択マーク位置が返れば、そこへキャレットを移動する（原はダイアログ終了後に移動）。
void CStirlingView::OnMarkList() {
    CMarkListDlg dlg(this);
    if (dlg.DoModal() == IDOK && dlg.m_doJump) {
        JumpToMark(dlg.m_jumpPos);
    }
}

// マークファイルの選択（書き出し／読み込み共通）。既定名は文書名 + .mrk。
//   戻り値: 利用者が選んだパス。キャンセルなら空。
static CStringW AskMarkFilePath(CWnd* owner, const CStringW& docPath, bool forSave) {
    CStringW initial;
    if (!docPath.IsEmpty()) {
        initial = docPath;
        const int dot = initial.ReverseFind(L'.');
        const int sep = initial.ReverseFind(L'\\');
        if (dot > sep) { initial = initial.Left(dot); }
        initial += L".mrk";
    }

    CFileDialog dlg(forSave ? FALSE : TRUE, L"mrk", initial,
                    forSave ? (OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY)
                            : (OFN_FILEMUSTEXIST | OFN_HIDEREADONLY),
                    ui::LoadW(IDS_MARK_FILE_FILTER), owner);
    if (dlg.DoModal() != IDOK) { return CStringW(); }
    return CStringW(dlg.GetPathName());
}

// マークをファイルへ書き出す（Issue #99）。マークが無いときはメニューが無効なため来ない。
void CStirlingView::OnMarkExport() {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr || !pDoc->HasMarks()) { return; }

    const CStringW path = AskMarkFilePath(this, CStringW(pDoc->GetPathName()), true);
    if (path.IsEmpty()) { return; }

    stirling::marks::MarkFileData data;
    data.sourcePath = static_cast<LPCWSTR>(CStringW(pDoc->GetPathName()));
    data.sourceSize = pDoc->GetTotalLength();
    for (const auto& kv : pDoc->Marks()) {
        // 内部種別 0..2 → ファイル上の 1..3（UI の「マーク1/2/3」に合わせる）。
        data.marks[kv.first] = kv.second + 1;
    }

    std::wstring error;
    if (!stirling::settings::WriteTextFileUtf8(
            static_cast<LPCWSTR>(path), stirling::marks::SerializeMarks(data), error)) {
        CStringW message;
        message.Format(ui::LoadW(IDS_ERR_MARK_SAVE), error.c_str());
        ui::MsgBox(GetSafeHwnd(), message);
    }
}

// マークをファイルから読み込む（Issue #99）。
//   既存マークがあれば追加／置き換えを問い、データの大きさが違えば続行を問う。
//   ファイルが1つでも解釈できなければ、マークには一切手を触れない。
void CStirlingView::OnMarkImport() {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr) { return; }

    const CStringW path = AskMarkFilePath(this, CStringW(pDoc->GetPathName()), false);
    if (path.IsEmpty()) { return; }

    std::wstring text;
    std::wstring error;
    stirling::marks::MarkFileData data;
    if (!stirling::settings::ReadTextFileUtf8(static_cast<LPCWSTR>(path), text, error) ||
        !stirling::marks::ParseMarks(text, data, error)) {
        CStringW message;
        message.Format(ui::LoadW(IDS_ERR_MARK_LOAD), error.c_str());
        ui::MsgBox(GetSafeHwnd(), message);
        return;
    }

    const stirling::FileOffset total = pDoc->GetTotalLength();
    if (data.sourceSize >= 0 && data.sourceSize != total) {
        CStringW message;
        CStringW sizeText;
        sizeText.Format(L"%lld", static_cast<long long>(data.sourceSize));
        message.Format(ui::LoadW(IDS_CONFIRM_MARK_SIZE), static_cast<LPCWSTR>(sizeText));
        if (ui::MsgBox(GetSafeHwnd(), message, MB_YESNO | MB_ICONQUESTION) != IDYES) { return; }
    }

    bool merge = true;
    if (pDoc->HasMarks()) {
        const int answer = ui::MsgBox(GetSafeHwnd(), ui::LoadW(IDS_CONFIRM_MARK_MERGE),
                                      MB_YESNOCANCEL | MB_ICONQUESTION);
        if (answer == IDCANCEL) { return; }
        merge = (answer == IDYES);
    }
    if (!merge) { pDoc->ClearMarks(); }

    int applied = 0;
    int skipped = 0;
    for (const auto& kv : data.marks) {
        // データ末尾を超える位置は捨てる（別バージョンのデータへ流用した場合に起こる）。
        if (kv.first < 0 || kv.first >= total) { ++skipped; continue; }
        pDoc->SetMark(kv.first, kv.second - 1);   // ファイル上の 1..3 → 内部種別 0..2
        ++applied;
    }
    Invalidate(FALSE);

    CStringW message;
    if (skipped > 0) {
        message.Format(ui::LoadW(IDS_MARK_IMPORT_DONE_SKIP), applied, skipped);
    } else {
        message.Format(ui::LoadW(IDS_MARK_IMPORT_DONE), applied);
    }
    ui::MsgBox(GetSafeHwnd(), message, MB_OK | MB_ICONINFORMATION);
}

void CStirlingView::JumpToMark(stirling::FileOffset pos) {
    // 原: 位置がデータ範囲外なら beep（データ縮小でマークが宙に浮いた場合の保険）。
    if (pos < 0 || pos >= Total()) { ::MessageBeep(0); return; }
    GotoPos(pos);
}

// 指定位置へキャレットを移動し、画面外の場合のみ縦中央へスクロール（原 FUN_0044d960 mode2）。
// ジャンプ/最終変更箇所/マーク実行で共用。pos は [0, total]（末尾位置も可）。
void CStirlingView::GotoPos(stirling::FileOffset pos) {
    const stirling::FileOffset total = Total();
    if (pos < 0) { pos = 0; }
    if (pos > total) { pos = total; }
    m_selActive = false;
    m_caretPos  = pos;
    m_nibbleLow = false;
    CenterCaretRow();
    Invalidate(FALSE);
    UpdateCaret();
    SetFocus();
}

void CStirlingView::OnGotoDataTop() {
    MoveCaretTo(0, false);            // 先頭へ（原 mode0: 最小スクロールで先頭表示）
}

void CStirlingView::OnGotoDataEnd() {
    MoveCaretTo(Total(), false);     // 末尾の後ろへ（原 mode0）
}

void CStirlingView::OnJump() {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr) { return; }
    CJumpDlg dlg(this, Total(), m_caretPos);
    if (dlg.DoModal() == IDOK) {
        GotoPos(dlg.ResultAddr());   // 画面外なら縦中央（原 mode2）
    }
}

void CStirlingView::OnGotoLastModified() {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr || !pDoc->HasLastModified()) {
        ::MessageBeep(0);            // 変更履歴が無ければ beep（原 FUN_0044db0b）
        return;
    }
    GotoPos(pDoc->LastModifiedPos());
}

void CStirlingView::OnUpdateGotoLastModified(CCmdUI* pCmdUI) {
    CStirlingDoc* pDoc = GetDocument();
    pCmdUI->Enable(pDoc != nullptr && pDoc->HasLastModified());
}

// 選択範囲の削除（原 0x802a CStirlingView_DeleteSelection）。
//   選択が無ければ無処理（原は beep しない＝update で無効化済のため通常到達しない）。
//   削除後はキャレットを範囲先頭へ、選択解除（既存 DeleteSelection と一致）。
void CStirlingView::OnDeleteSelection() {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr || !pDoc->CanEdit() || !m_selActive) {
        return;
    }
    const stirling::FileOffset lo = SelLo();
    if (DeleteSelection()) {          // caret=lo, selActive=false を内部で設定
        AfterEdit(lo);                // 原 code7: 先頭へ＋最小スクロール（縦中央にはしない）
    }
}

// 選択範囲の初期化（原 0x802b FUN_004467ef→IDD 165→CStirlingDoc_FillRange）。
//   選択範囲を単一バイト値で上書き（単一Undoレコード）。原はダイアログで 2桁16進を入力。
//   確定後はキャレットを範囲末尾+1へ移動して選択を解除し、画面外なら縦中央スクロール（原 mode2）。
void CStirlingView::OnFillSelection() {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr || !pDoc->CanEdit() || !m_selActive) {
        return;                       // 原は beep しない（update で無効化済）
    }
    const stirling::FileOffset lo = SelLo();
    const stirling::FileOffset hi = SelHi();
    if (hi <= lo) {
        return;
    }
    // 原ダイアログの範囲表示は両端含む [start, end]＝[lo, hi-1]。
    CFillRangeDlg dlg(this, lo, hi - 1);
    if (dlg.DoModal() != IDOK) {
        return;
    }
    // 範囲を定数バイトで上書き（長さ不変＝ReplaceRange で単一 kReplace レコード）。
    const std::vector<unsigned char> bytes(static_cast<size_t>(hi - lo), dlg.Value());
    if (!pDoc->ReplaceRange(lo, hi - lo, bytes)) {
        return;   // 上限超過の確認で中止された等。選択を維持（Issue #30）
    }

    // 原 FUN_004467ef: キャレットを end+1(=hi) へ移動し選択を解除。画面外なら縦中央スクロール。
    m_selActive = false;
    m_caretPos  = hi;
    m_nibbleLow = false;
    UpdateScrollInfo();
    pDoc->UpdateAllViews(nullptr);   // 全ビュー再描画（MDI 複製ウィンドウ対応）
    CenterCaretRow();                // 原 mode2: 画面外のときのみ縦中央
    UpdateCaret();
    SetFocus();
}

void CStirlingView::OnUpdateSelectionCmd(CCmdUI* pCmdUI) {
    // 選択範囲の保存（読取専用でも可）。原 FUN_0045bd1e は選択有無のみで活性。
    pCmdUI->Enable(m_selActive);
}

void CStirlingView::OnUpdateEditSelectionCmd(CCmdUI* pCmdUI) {
    // 選択範囲の削除/初期化（編集を伴う）。原 FUN_0045bc66/bcc2: CanEdit && 選択あり。
    CStirlingDoc* pDoc = GetDocument();
    pCmdUI->Enable(pDoc != nullptr && pDoc->CanEdit() && m_selActive);
}

// 編集禁止／許可の切替（原 0x8025 FUN_0045bb74→doc FUN_00436ce5＋UI反映）。
void CStirlingView::OnToggleReadOnly() {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr || !pDoc->ToggleEditState()) {
        ::MessageBeep(0);            // ロック状態（切替不可）は beep（原 FUN_0044db5b）
    }
    // ToggleEditState 内で UpdateAllViews 済（読取専用表示の反映）。
}

void CStirlingView::OnUpdateEditReplace(CCmdUI* pCmdUI) {
    CStirlingDoc* pDoc = GetDocument();
    pCmdUI->Enable(pDoc != nullptr && pDoc->CanEdit());
}

void CStirlingView::OnUpdateToggleReadOnly(CCmdUI* pCmdUI) {
    // 原 FUN_0045bb8f: 有効=状態≠0（ロックでなければ切替可）、チェック=編集不可（＝編集禁止中）。
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr) { pCmdUI->Enable(FALSE); return; }
    pCmdUI->Enable(pDoc->EditState() != 0);
    pCmdUI->SetCheck(pDoc->CanEdit() ? 0 : 1);
}

// ===========================================================================
// データ比較（原 0x8038 FUN_0044c6b0→FUN_0044c75e→FUN_0044c96c。フェーズ1: 差分ハイライト）
// ===========================================================================
namespace {

// 開いている CStirlingDoc（ビューを持つもの）の数を数える（原 FUN_0044cbad(0)）。
int CountOpenDocs() {
    int n = 0;
    CWinApp* pApp = AfxGetApp();
    POSITION posT = pApp->GetFirstDocTemplatePosition();
    while (posT != nullptr) {
        CDocTemplate* pTmpl = pApp->GetNextDocTemplate(posT);
        POSITION posD = pTmpl->GetFirstDocPosition();
        while (posD != nullptr) {
            CDocument* pDoc = pTmpl->GetNextDoc(posD);
            if (pDoc->GetFirstViewPosition() != nullptr) { ++n; }
        }
    }
    return n;
}

// 文書のすべての CStirlingView へ比較結果を設定（diffs!=null）／解除（null）。
void ApplyCompareToDoc(CDocument* pDoc, const std::vector<std::pair<stirling::FileOffset, stirling::FileOffset>>* diffs) {
    POSITION posV = pDoc->GetFirstViewPosition();
    while (posV != nullptr) {
        CView* pView = pDoc->GetNextView(posV);
        if (CStirlingView* pSV = DYNAMIC_DOWNCAST(CStirlingView, pView)) {
            if (diffs != nullptr) { pSV->SetCompareResult(*diffs); }
            else                  { pSV->ClearCompareResult(); }
        }
    }
}

// 2文書の先頭 size バイトを比較し、相違の連続範囲[start,end]（昇順）を収集（原 FUN_0044c96c）。
std::vector<std::pair<stirling::FileOffset, stirling::FileOffset>> ComputeCompareDiffs(CStirlingDoc* a, CStirlingDoc* b, stirling::FileOffset size) {
    std::vector<std::pair<stirling::FileOffset, stirling::FileOffset>> diffs;
    const int kChunk = 65536;                 // 読取チャンクはメモリ上のバッファ長のため int
    stirling::FileOffset runStart = -1;
    stirling::FileOffset pos = 0;
    while (pos < size) {
        const int n = (size - pos < kChunk) ? static_cast<int>(size - pos) : kChunk;
        const std::vector<unsigned char> ba = a->ReadRange(pos, n);
        const std::vector<unsigned char> bb = b->ReadRange(pos, n);
        const int m = static_cast<int>((ba.size() < bb.size()) ? ba.size() : bb.size());
        for (int i = 0; i < m; ++i) {
            if (ba[i] != bb[i]) {
                if (runStart < 0) { runStart = pos + i; }
            } else if (runStart >= 0) {
                diffs.push_back(std::make_pair(runStart, pos + i - 1));
                runStart = -1;
            }
        }
        pos += m;
        if (m < n) { break; }   // 読み取り不足（想定外の安全弁）
    }
    if (runStart >= 0) { diffs.push_back(std::make_pair(runStart, size - 1)); }
    return diffs;
}

void ShowCompareMsg(HWND hWnd, UINT strId) {
    ui::MsgBox(hWnd, ui::LoadW(strId), MB_OK | MB_ICONEXCLAMATION);
}

} // namespace

// データ比較（原 FUN_0044c6b0/FUN_0044c75e）。他文書を選択し、min(サイズ)まで比較して
//   相違バイトを両文書のビューで比較色に着色する（フェーズ1。相違一覧/シンクロは次フェーズ）。
void CStirlingView::OnCompare() {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr) { return; }
    if (CountOpenDocs() < 2) { ::MessageBeep(0); return; }   // 2文書以上必要

    CCompareDlg dlg(this, pDoc);
    if (dlg.DoModal() != IDOK) { return; }
    RunCompareWith(dlg.TargetView());
}

// 比較の実行本体（原 FUN_00409930 系）。対象ビューは選択ダイアログ、または外部変更通知の
//   「変更されたファイルをオープンして比較実行」から与えられる。
void CStirlingView::RunCompareWith(CStirlingView* pTargetView) {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr) { return; }
    if (pTargetView == nullptr) { return; }
    CStirlingDoc* pTargetDoc = pTargetView->GetDocument();
    if (pTargetDoc == nullptr) { return; }

    const stirling::FileOffset size1 = pDoc->GetTotalLength();
    const stirling::FileOffset size2 = pTargetDoc->GetTotalLength();
    if (size1 == 0 || size2 == 0) {
        ShowCompareMsg(GetSafeHwnd(), IDS_COMPARE_EMPTY);       // 原 1023
        return;
    }
    stirling::FileOffset cmpSize = size1;
    if (size1 != size2) {
        ShowCompareMsg(GetSafeHwnd(), IDS_COMPARE_SIZEDIFF);   // 原 1024（先に通知し小さい方まで）
        cmpSize = (size2 < size1) ? size2 : size1;
    }

    const std::vector<std::pair<stirling::FileOffset, stirling::FileOffset>> diffs = ComputeCompareDiffs(pDoc, pTargetDoc, cmpSize);
    if (diffs.empty()) {
        ShowCompareMsg(GetSafeHwnd(), IDS_COMPARE_NODIFF);     // 原 1025
        ApplyCompareToDoc(pDoc, nullptr);                     // 既存の比較モードを解除
        ApplyCompareToDoc(pTargetDoc, nullptr);
        return;
    }

    // 既存の相違一覧ダイアログがあれば閉じる（比較をやり直す）。
    if (m_pDiffDlg != nullptr && ::IsWindow(m_pDiffDlg->GetSafeHwnd())) {
        m_pDiffDlg->DestroyWindow();   // Cleanup→PostNcDestroy で自己破棄・参照クリア
    }

    // 相違一覧ダイアログ（モードレス）を生成（原 FUN_00409930）。開始ビューが所有する。
    CDiffListDlg* pDlg = new CDiffListDlg();
    if (pDlg->CreateModeless(this, pTargetView, diffs, AfxGetMainWnd())) {
        // 両文書の全ビューへ差分を設定（比較色ハイライト）。
        // ダイアログ生成後に適用するため、生成失敗時に比較状態だけを残さない。
        ApplyCompareToDoc(pDoc, &diffs);
        ApplyCompareToDoc(pTargetDoc, &diffs);
        m_pDiffDlg = pDlg;
        pDlg->ShowWindow(SW_SHOW);
    } else {
        delete pDlg;
    }
}

void CStirlingView::OnUpdateCompare(CCmdUI* pCmdUI) {
    pCmdUI->Enable(CountOpenDocs() >= 2);   // 原 FUN_0045bf31: 文書2つ以上で有効
}

// 全て選択（原 57642=ID_EDIT_SELECT_ALL FUN_00448b29）。空でなければ [0,total) を選択、caret先頭。
void CStirlingView::OnEditSelectAll() {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr) { return; }
    m_nibbleLow = false;
    const stirling::FileOffset total = Total();
    if (total == 0) { return; }        // 原: データ0件は何もしない
    m_selAnchor = total;               // 原: caret=(0,0), 選択他端=末尾 → [0,total)
    m_caretPos  = 0;
    m_selActive = true;
    EnsureCaretVisible();               // caret=0 → 先頭表示（原 scroll mode1 相当）
    Invalidate(FALSE);
    UpdateCaret();
    SetFocus();
}

void CStirlingView::OnUpdateEditSelectAll(CCmdUI* pCmdUI) {
    pCmdUI->Enable(Total() > 0);
}

// 編集前に戻す（原 0x802d FUN_00446c4a→FUN_00446c7a）。変更を破棄しファイル再読込。
void CStirlingView::OnRevertFile() {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr) { return; }
    if (!pDoc->CanEdit() || pDoc->GetPathName().IsEmpty() || !pDoc->IsModified()) {
        ::MessageBeep(0);              // 原 FUN_00446c4a: 未変更/パス無しは beep
        return;
    }
    // 確認（原 1022, YESNO。Yes 以外は中止）。
    const int r = ui::MsgBox(GetSafeHwnd(), ui::LoadW(IDS_REVERT_CONFIRM), MB_YESNO | MB_ICONQUESTION);
    if (r != IDYES) { return; }

    if (pDoc->RevertToSaved()) {        // DeleteContents→再読込→SetModifiedFlag(FALSE)
        m_caretPos = 0;
        m_selActive = false;
        m_nibbleLow = false;
        m_topLine = 0;
        pDoc->UpdateAllViews(nullptr);  // 全ビュー再描画（キャレット等は各 OnUpdate で再計算）
        UpdateScrollInfo();
        UpdateCaret();
    } else {
        ::MessageBeep(0);              // 再読込失敗（原は文書クローズ。ここでは beep）
    }
}

// --- 外部プロセスによるファイル変更の検知（原 CView::OnActivateView @0x00450d2c）---
//   原はアクティブ化のたびに自身へ WM_USER+0x1B をポストし、そのハンドラで文書の
//   保持時刻とディスク上の更新時刻を突き合わせる（バックグラウンド監視はしない）。
void CStirlingView::OnActivateView(BOOL bActivate, CView* pActivateView, CView* pDeactiveView) {
    CView::OnActivateView(bActivate, pActivateView, pDeactiveView);
    if (bActivate && GetSafeHwnd() != nullptr) {
        PostMessage(WM_STIRLING_CHECK_FILE);
    }
}

// 外部変更の確認と通知ダイアログ（原 FUN_00450d76 → IDD_FILE_CHANGED 199）。
LRESULT CStirlingView::OnCheckFileChanged(WPARAM /*wParam*/, LPARAM /*lParam*/) {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr || m_checkingFileChange) { return 0; }
    if (!pDoc->HasExternalChange()) { return 0; }

    const CString changedPath = pDoc->GetPathName();   // 変更されたファイル（比較実行で使う）
    m_checkingFileChange = true;
    CFileChangedDlg dlg(this, changedPath);
    const INT_PTR ret = dlg.DoModal();
    m_checkingFileChange = false;
    if (ret != IDOK) { return 0; }   // 決定していないので次のアクティブ化で再度確認する

    switch (dlg.GetChoice()) {
    case CFileChangedDlg::kIgnore:
        pDoc->SyncDiskTime();          // 以後は同じ変更で通知しない（原実測）
        break;

    case CFileChangedDlg::kReload:
        if (pDoc->RevertToSaved()) {   // 編集内容を破棄してディスクから読み直す
            m_caretPos = 0;
            m_selActive = false;
            m_nibbleLow = false;
            m_topLine = 0;
            pDoc->UpdateAllViews(nullptr);
            UpdateScrollInfo();
            UpdateCaret();
        } else {
            ::MessageBeep(0);
        }
        pDoc->SyncDiskTime();
        break;

    case CFileChangedDlg::kSaveAs: {
        // 現在の内容を別名で保存し、文書をその新しいパスへ切り替える（原は表題が変わる）。
        const CString newPath = dlg.SaveAsPath();
        if (!pDoc->OnSaveDocument(newPath)) {
            ::MessageBeep(0);
            break;
        }
        pDoc->SetPathName(newPath);
        pDoc->SyncDiskTime();
        if (dlg.CompareAfterSave() && !changedPath.IsEmpty()) {
            // 変更されたファイルを開いて比較実行（原は 0x041E をメインフレームへポスト）。
            CDocument* pOpened = AfxGetApp()->OpenDocumentFile(changedPath);
            CStirlingDoc* pChangedDoc = DYNAMIC_DOWNCAST(CStirlingDoc, pOpened);
            CStirlingView* pChangedView = nullptr;
            if (pChangedDoc != nullptr) {
                POSITION pos = pChangedDoc->GetFirstViewPosition();
                while (pos != nullptr) {
                    CView* v = pChangedDoc->GetNextView(pos);
                    if (CStirlingView* sv = DYNAMIC_DOWNCAST(CStirlingView, v)) { pChangedView = sv; break; }
                }
            }
            if (pChangedView != nullptr) {
                // 比較は変更元（この文書）を起点に行う。開いた側がアクティブになっているため戻す。
                if (CFrameWnd* frame = GetParentFrame()) { frame->ActivateFrame(); }
                RunCompareWith(pChangedView);
            }
        }
        break;
    }
    }
    return 0;
}

void CStirlingView::OnUpdateRevertFile(CCmdUI* pCmdUI) {
    // 原 FUN_0045bd55: CanEdit && 変更あり && パスあり。
    CStirlingDoc* pDoc = GetDocument();
    pCmdUI->Enable(pDoc != nullptr && pDoc->CanEdit() && pDoc->IsModified()
                   && !pDoc->GetPathName().IsEmpty());
}

// --- ステータスバー各ペインの更新（原 FUN_00424xxx。アイドル時に呼ばれる） ---
//   原はビュー側で更新し、常に Enable(TRUE) してからペイン文字列を設定する。
//   表示文字列は wide 層（リソースから読んだワイド文字列をそのまま渡す）。

void CStirlingView::OnUpdateIndicatorAddress(CCmdUI* pCmdUI) {
    // 原 FUN_004249fa: 「0x%08X」キャレット位置。
    pCmdUI->Enable(TRUE);
    CString s;
    s.Format(_T("0x%0*llX"), AddrDigits(), static_cast<long long>(m_caretPos));
    pCmdUI->SetText(s);
}

void CStirlingView::OnUpdateIndicatorModified(CCmdUI* pCmdUI) {
    // 原 FUN_00424b29: 変更ありで「更新」、無ければ空。
    CStirlingDoc* pDoc = GetDocument();
    pCmdUI->Enable(TRUE);
    pCmdUI->SetText((pDoc != nullptr && pDoc->IsModified())
                    ? ui::LoadW(IDS_INDICATOR_MODIFIED_TEXT) : CStringW());
}

void CStirlingView::OnUpdateIndicatorEditLock(CCmdUI* pCmdUI) {
    // 原 59143「編禁」: 編集禁止（CanEdit でない）時に表示、可のときは空。
    CStirlingDoc* pDoc = GetDocument();
    pCmdUI->Enable(TRUE);
    pCmdUI->SetText((pDoc != nullptr && !pDoc->CanEdit())
                    ? ui::LoadW(IDS_INDICATOR_EDITLOCK_TEXT) : CStringW());
}

void CStirlingView::OnUpdateIndicatorMode(CCmdUI* pCmdUI) {
    // 原 FUN_00424d49: 上書モードで「上書」、挿入モードで「挿入」。
    CStirlingDoc* pDoc = GetDocument();
    pCmdUI->Enable(TRUE);
    const bool ow = (pDoc != nullptr) && pDoc->IsOverwriteMode();
    pCmdUI->SetText(ui::LoadW(ow ? IDS_INDICATOR_OVERWRITE_TEXT : IDS_INDICATOR_INSERT));
}

void CStirlingView::OnUpdateIndicatorSize(CCmdUI* pCmdUI) {
    // 原 FUN_004248be: 「%d Bytes」総サイズ（10進）。
    pCmdUI->Enable(TRUE);
    CString s;
    s.Format(_T("%lld Bytes"), static_cast<long long>(Total()));
    pCmdUI->SetText(s);
}

void CStirlingView::OnUpdateIndicatorCharset(CCmdUI* pCmdUI) {
    // 原 FUN_00424bd0: 文字セット名（0=ASCII/1=SHIFT-JIS/2=EUC/3=Unicode/4=EBCDIC/5=EBCIDK）。
    //   ラベルは RC 6040+cs。6=UTF-8 は移植で追加（Issue #98）。表は ui::CharsetNameW へ集約（Issue #125）。
    CStirlingDoc* pDoc = GetDocument();
    pCmdUI->Enable(TRUE);
    const int cs = (pDoc != nullptr) ? pDoc->GetCharset() : 1;
    pCmdUI->SetText(ui::CharsetNameW(cs));
}

void CStirlingView::OnUpdateIndicatorAddrDec(CCmdUI* pCmdUI) {
    // 原 FUN_00424a94: キャレット位置（10進）。
    pCmdUI->Enable(TRUE);
    CString s;
    s.Format(_T("%lld"), static_cast<long long>(m_caretPos));
    pCmdUI->SetText(s);
}

void CStirlingView::OnUpdateIndicatorSizeHex(CCmdUI* pCmdUI) {
    // 原 FUN_00424960: 「0x%08X Bytes」総サイズ（16進）。
    pCmdUI->Enable(TRUE);
    CString s;
    s.Format(_T("0x%0*llX Bytes"), AddrDigits(), static_cast<long long>(Total()));
    pCmdUI->SetText(s);
}

void CStirlingView::OnUpdateIndicatorByteOrder(CCmdUI* pCmdUI) {
    // 原 FUN_00424c8a: バイトオーダー名（6050 "LittleEndian" / 6051 "BigEndian"）。
    CStirlingDoc* pDoc = GetDocument();
    pCmdUI->Enable(TRUE);
    if (pDoc == nullptr) { pCmdUI->SetText(_T("")); return; }
    pCmdUI->SetText(pDoc->IsByteOrderBig() ? _T("BigEndian") : _T("LittleEndian"));
}

// キャレット位置から最大 want バイトを読取る（総サイズ超過分は除外）。
//   戻り=実読取数。0 のときキャレットはデータ外（原 this+0xb70 フラグ相当）。
int CStirlingView::ReadBytesAtCaret(int want, unsigned char out[8]) {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr) { return 0; }
    const stirling::FileOffset total = Total();
    if (m_caretPos < 0 || m_caretPos >= total) { return 0; }   // キャレットがデータ外
    int n = 0;
    for (; n < want && n < 8 && m_caretPos + n < total; ++n) {
        unsigned char b = 0;
        if (!pDoc->GetByteAt(m_caretPos + n, &b)) { break; }
        out[n] = b;
    }
    return n;
}

namespace {
// n バイト(ファイル順)からバイトオーダーに従い整数値を組み立てる。
unsigned long AssembleInt(const unsigned char* b, int n, bool big) {
    unsigned long v = 0;
    if (big) { for (int i = 0; i < n; ++i)     { v = (v << 8) | b[i]; } }
    else     { for (int i = n - 1; i >= 0; --i) { v = (v << 8) | b[i]; } }
    return v;
}
// n バイト(ファイル順)を x86 ネイティブ(リトルエンディアン)並びへ整える。不足分は 0 詰め。
void ToNativeLE(const unsigned char* b, int n, bool big, unsigned char le[8]) {
    for (int i = 0; i < 8; ++i) { le[i] = 0; }
    for (int i = 0; i < n; ++i) { le[i] = big ? b[n - 1 - i] : b[i]; }
}
}  // namespace

void CStirlingView::OnUpdateIndicatorByteDec(CCmdUI* pCmdUI) {
    // 原 FUN_00424399: 「B : %d」BYTE値（10進）。キャレットがデータ外なら空。
    pCmdUI->Enable(TRUE);
    unsigned char b[8];
    if (ReadBytesAtCaret(1, b) < 1) { pCmdUI->SetText(_T("")); return; }
    CString s; s.Format(_T("B : %d"), (int)b[0]);
    pCmdUI->SetText(s);
}

void CStirlingView::OnUpdateIndicatorByteHex(CCmdUI* pCmdUI) {
    // 原: 「B : 0x%02X」BYTE値（16進）。
    pCmdUI->Enable(TRUE);
    unsigned char b[8];
    if (ReadBytesAtCaret(1, b) < 1) { pCmdUI->SetText(_T("")); return; }
    CString s; s.Format(_T("B : 0x%02X"), (unsigned)b[0]);
    pCmdUI->SetText(s);
}

void CStirlingView::OnUpdateIndicatorWordDec(CCmdUI* pCmdUI) {
    // 原: 「W : %d」WORD値（10進, バイトオーダー準拠）。
    CStirlingDoc* pDoc = GetDocument();
    pCmdUI->Enable(TRUE);
    unsigned char b[8];
    const int n = ReadBytesAtCaret(2, b);
    if (n < 1 || pDoc == nullptr) { pCmdUI->SetText(_T("")); return; }
    const unsigned long v = AssembleInt(b, n, pDoc->IsByteOrderBig());
    CString s; s.Format(_T("W : %d"), (int)(unsigned short)v);
    pCmdUI->SetText(s);
}

void CStirlingView::OnUpdateIndicatorWordHex(CCmdUI* pCmdUI) {
    // 原: 「W : 0x%04X」WORD値（16進, バイトオーダー準拠）。
    CStirlingDoc* pDoc = GetDocument();
    pCmdUI->Enable(TRUE);
    unsigned char b[8];
    const int n = ReadBytesAtCaret(2, b);
    if (n < 1 || pDoc == nullptr) { pCmdUI->SetText(_T("")); return; }
    const unsigned long v = AssembleInt(b, n, pDoc->IsByteOrderBig());
    CString s; s.Format(_T("W : 0x%04X"), (unsigned)(unsigned short)v);
    pCmdUI->SetText(s);
}

void CStirlingView::OnUpdateIndicatorDwordDec(CCmdUI* pCmdUI) {
    // 原: 「DW : 」＋ DWORD値（10進, バイトオーダー準拠）。
    CStirlingDoc* pDoc = GetDocument();
    pCmdUI->Enable(TRUE);
    unsigned char b[8];
    const int n = ReadBytesAtCaret(4, b);
    if (n < 1 || pDoc == nullptr) { pCmdUI->SetText(_T("")); return; }
    const unsigned long v = AssembleInt(b, n, pDoc->IsByteOrderBig());
    CString s; s.Format(_T("DW : %u"), (unsigned)v);
    pCmdUI->SetText(s);
}

void CStirlingView::OnUpdateIndicatorDwordHex(CCmdUI* pCmdUI) {
    // 原: 「DW : 0x%08X」DWORD値（16進, バイトオーダー準拠）。
    CStirlingDoc* pDoc = GetDocument();
    pCmdUI->Enable(TRUE);
    unsigned char b[8];
    const int n = ReadBytesAtCaret(4, b);
    if (n < 1 || pDoc == nullptr) { pCmdUI->SetText(_T("")); return; }
    const unsigned long v = AssembleInt(b, n, pDoc->IsByteOrderBig());
    CString s; s.Format(_T("DW : 0x%08X"), (unsigned)v);
    pCmdUI->SetText(s);
}

void CStirlingView::OnUpdateIndicatorFloat(CCmdUI* pCmdUI) {
    // 原: 「f : 」＋ float値（4バイト, バイトオーダー準拠）。
    CStirlingDoc* pDoc = GetDocument();
    pCmdUI->Enable(TRUE);
    unsigned char b[8];
    const int n = ReadBytesAtCaret(4, b);
    if (n < 1 || pDoc == nullptr) { pCmdUI->SetText(_T("")); return; }
    unsigned char le[8]; ToNativeLE(b, n, pDoc->IsByteOrderBig(), le);
    float f; ::memcpy(&f, le, sizeof(f));
    CString s; s.Format(_T("f : %g"), (double)f);
    pCmdUI->SetText(s);
}

void CStirlingView::OnUpdateIndicatorDouble(CCmdUI* pCmdUI) {
    // 原: 「d : 」＋ double値（8バイト, バイトオーダー準拠）。
    CStirlingDoc* pDoc = GetDocument();
    pCmdUI->Enable(TRUE);
    unsigned char b[8];
    const int n = ReadBytesAtCaret(8, b);
    if (n < 1 || pDoc == nullptr) { pCmdUI->SetText(_T("")); return; }
    unsigned char le[8]; ToNativeLE(b, n, pDoc->IsByteOrderBig(), le);
    double d; ::memcpy(&d, le, sizeof(d));
    CString s; s.Format(_T("d : %g"), d);
    pCmdUI->SetText(s);
}

// ===========================================================================
// ユーザーメニュー実適用（原 15メニュー設定。idx0-9=メニュー1-10 / 10-12=2ストローク /
//   13=Escメニュー / 14=コンテキストメニュー）。構成→HMENU は BuildUserPopup。
//   コマンド(cmdID)は TrackPopupMenu の owner=this 経由で通常のコマンド経路にディスパッチ。
// ===========================================================================
void CStirlingView::PopupUserMenuAtCaret(int idx) {
    const std::vector<std::vector<UINT>>& menus = theApp.AppSettings().userMenus;
    if (idx < 0 || idx >= (int)menus.size() || menus[idx].empty()) { return; }
    CMenu menu;
    if (!BuildUserPopup(menus[idx], menu)) { return; }
    int x, y; CaretPixel(x, y);
    CPoint pt(x, y + m_rowH);   // キャレット直下
    ClientToScreen(&pt);
    menu.TrackPopupMenu(TPM_LEFTALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, this);
}

void CStirlingView::OnContextMenu(CWnd* /*pWnd*/, CPoint point) {
    const std::vector<std::vector<UINT>>& menus = theApp.AppSettings().userMenus;
    const int idx = 14;   // コンテキストメニュー
    if (idx >= (int)menus.size() || menus[idx].empty()) { return; }   // 未設定なら何もしない（原挙動）
    CMenu menu;
    if (!BuildUserPopup(menus[idx], menu)) { return; }
    if (point.x == -1 && point.y == -1) {   // キーボード(Shift+F10)からはキャレット直下
        int x, y; CaretPixel(x, y);
        point.SetPoint(x, y + m_rowH);
        ClientToScreen(&point);
    }
    menu.TrackPopupMenu(TPM_LEFTALIGN | TPM_RIGHTBUTTON, point.x, point.y, this);
}

// 「名前を指定して実行」（原 0x804f）。ダイアログ→起動は RunAppCommand が担う。
void CStirlingView::OnRunApp() {
    RunAppCommand(this);
}

void CStirlingView::OnUserMenuInvoke(UINT nID) {
    // nID: ID_USERMENU_1(0x803A)..ID_TWOSTROKE_3(0x8046) → userMenus[0..12]
    PopupUserMenuAtCaret((int)nID - ID_USERMENU_1);
}

// 選択範囲を生バイナリでファイルに保存（原 0x802c FUN_00446986）。
//   選択範囲 [start, end]（両端含む＝我々の [lo, hi)）の生バイトを読み取り、
//   名前を付けて保存ダイアログ→CMirrorFile(modeCreate|modeWrite) で書き出す。
//   選択が無ければ無処理（原は beep だが update で無効化済。削除/初期化と同方針）。
//   原と同様、右クリックのユーザ設定メニューからも起動できる。
void CStirlingView::OnSaveSelection() {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr || !m_selActive) {
        return;
    }
    const stirling::FileOffset lo = SelLo();
    const stirling::FileOffset hi = SelHi();
    const std::vector<unsigned char> bytes = pDoc->ReadRange(lo, hi - lo);

    // 名前を付けて保存（原 FUN_00487a35: 保存, OFN_HIDEREADONLY|OFN_OVERWRITEPROMPT）。
    // MBCS＋/utf-8 の CP932 化を避けるため、フィルタに日本語リテラルは使わない（NULL＝全ファイル）。
    CFileDialog dlg(FALSE, nullptr, nullptr,
                    OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT, nullptr, this);
    if (dlg.DoModal() != IDOK) {
        return;
    }
    const CString path = dlg.GetPathName();

    // 生バイトをファイルへ書き出す（原 CMirrorFile modeCreate|modeWrite）。
    CFile file;
    CFileException ex;
    if (!file.Open(path, CFile::modeCreate | CFile::modeWrite | CFile::shareExclusive, &ex)) {
        ui::MsgBoxRes(GetSafeHwnd(), IDS_SAVE_BACKUP_FAILED);
        return;
    }
    if (!bytes.empty()) {
        file.Write(bytes.data(), static_cast<UINT>(bytes.size()));
    }
    file.Close();
}

// ダンプイメージの保存（原 0x8060 FUN_0045c506→FUN_00446fc8→ダイアログ198→FUN_0045d3e2）。
//   表示と同じ整形テキストダンプ（アドレス＋16進＋文字欄）をファイルへ。データ全体/範囲指定。
void CStirlingView::OnSaveDump() {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr) { return; }
    const stirling::FileOffset total = Total();
    if (total == 0) { ::MessageBeep(0); return; }   // 原 FUN_00446fc8: データ0件は beep

    // 既定ファイル名: 元ファイルの拡張子を .DMP へ（原 FUN_0044737a, DAT_004b68b8=".DMP"）。
    CString defName;
    const CString path = pDoc->GetPathName();
    if (!path.IsEmpty()) {
        const int dot = path.ReverseFind(_T('.'));
        const int slash = path.ReverseFind(_T('\\'));
        defName = (dot > slash) ? (path.Left(dot) + _T(".DMP")) : (path + _T(".DMP"));
    } else {
        defName = pDoc->GetTitle() + _T(".DMP");
    }

    const bool hasSel = m_selActive;
    const stirling::FileOffset selStart = hasSel ? SelLo() : 0;
    const stirling::FileOffset selEnd   = hasSel ? (SelHi() - 1) : 0;   // 両端含む（SelHi は半開境界）

    CSaveDumpDlg dlg(this, defName, total, hasSel, selStart, selEnd);
    if (dlg.DoModal() != IDOK) { return; }

    if (!WriteDumpImage(dlg.m_fileName, dlg.Start(), dlg.End())) {
        ui::MsgBoxRes(GetSafeHwnd(), IDS_SAVE_BACKUP_FAILED);
    }
}

// ===========================================================================
// 印刷（原 CStirlingView 印刷仮想関数 0x4427e5/442840/442b29/442af3 の移植）
//   整形ダンプ（アドレス11 + 16進48 + 区切り2 + 文字16 = 77桁）をプリンタDCへ割付ける。
//   フォント=ＭＳ明朝(h100 SHIFTJIS)。色=モノクロ、比較差分のみ反転（原 FUN_0045d161）。
// ===========================================================================

// 印刷対象の先頭/末尾アドレス（両端含む）。印刷範囲指定時は範囲、既定は全体。
stirling::FileOffset CStirlingView::PrintFirstAddr() const {
    return m_printRangeActive ? m_printRangeStart : 0;
}
stirling::FileOffset CStirlingView::PrintLastAddr() const {
    return m_printRangeActive ? m_printRangeEnd : (Total() - 1);
}

// 印刷対象の総行数（原 OnBeginPrinting/FUN_00442f27 の 16 境界整列した行数）。
stirling::FileOffset CStirlingView::PrintTotalRows() const {
    const stirling::FileOffset start = PrintFirstAddr();
    const stirling::FileOffset end   = PrintLastAddr();
    if (end < start) { return 0; }
    long long count = static_cast<long long>(end) - start + 1;
    int extra = 0;
    if (start % 16 != 0) { count -= (16 - start % 16); extra = 1; }   // 先頭部分行を分離
    // 行数は 64bit のまま計算する（32GB 超で int だと桁落ち・符号反転する）。
    stirling::FileOffset rows = extra + (count >> 4);
    if (count % 16 != 0) { rows += 1; }
    return (rows < 1) ? 1 : rows;
}

// 印刷1バイトの前景/背景色（原 FUN_0045d161）: 既定=黒/白、比較差分=白/黒（反転）。
void CStirlingView::GetPrintByteColor(stirling::FileOffset absPos, COLORREF& fg, COLORREF& bg) const {
    if (m_compareActive && !m_compareDiffs.empty() && InCompareDiff(absPos)) {
        fg = RGB(255, 255, 255);
        bg = RGB(0, 0, 0);
    } else {
        fg = RGB(0, 0, 0);
        bg = RGB(255, 255, 255);
    }
}

// 印刷用フォント（ＭＳ明朝 10pt SHIFTJIS）を未生成なら生成。
//   原 FUN_00489de7（＝CFont::CreatePointFontIndirect 相当）: lfHeight=100 は「10.0ポイント」で、
//   属性DC(プリンタ)の LOGPIXELSY を使って device 単位の文字高へ変換し、負の lfHeight で生成する。
//   （100/720 インチ = 10pt。当初 lfHeight=100 を論理単位のまま使い 600dpi でフォントが巨大化
//    →1ページの行数が原より少なくなっていた）。
void CStirlingView::SelectPrintFont(CDC* pDC) {
    if (m_printFont.GetSafeHandle() != nullptr) { return; }
    // 属性DC(プリンタ)の縦DPIで 10pt を device 単位へ変換（原と同一。プレビューでも属性DC=プリンタ）。
    const int cyChar = ::MulDiv(100, pDC->GetDeviceCaps(LOGPIXELSY), 720);
    LOGFONTW lf = {0};
    lf.lfHeight = -cyChar;                    // 負値=文字高（原 FUN_00489de7 と同じ）
    lf.lfWeight = 400;                        // 原 local_34=400
    lf.lfCharSet = SHIFTJIS_CHARSET;          // 原 local_2d=0x80
    lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    wcscpy_s(lf.lfFaceName, LF_FACESIZE, L"ＭＳ 明朝");   // 原 DAT_004b67ec="ＭＳ 明朝"
    HFONT hf = ::CreateFontIndirectW(&lf);
    if (hf != nullptr) { m_printFont.Attach(hf); }   // 以後の破棄は CFont（m_printFont）が担う
    // UTF-8 文字欄用（Issue #98）。寸法は同じで charset だけ DEFAULT にする。
    lf.lfCharSet = DEFAULT_CHARSET;
    HFONT hfUtf8 = ::CreateFontIndirectW(&lf);
    if (hfUtf8 != nullptr) { m_printFontUtf8.Attach(hfUtf8); }
}

// ページ描画矩形から、印刷フォントのメトリクスとダンプグリッド領域・1ページ行数を算出。
//   原: ページ矩形を deflate(2*charW,2*rowH) + top+=rowH、rowsPerPage=rectHeight/rowH-2。
CStirlingView::PrintLayout CStirlingView::ComputePrintLayout(CDC* pDC, const CRect& page) const {
    PrintLayout L;
    TEXTMETRIC tm = {0};
    {
        const stirling::ScopedSelectFont selFont(pDC, const_cast<CFont*>(&m_printFont));
        pDC->GetTextMetrics(&tm);
    }
    L.charW = (tm.tmAveCharWidth > 0) ? tm.tmAveCharWidth : 8;
    L.rowH  = (tm.tmHeight > 0) ? tm.tmHeight : 16;
    CRect g = page;
    g.DeflateRect(L.charW * 2, L.rowH * 2);   // 原 FUN_004065f0（左右2文字・上下2行の余白）
    g.top += L.rowH;                          // 原 local_90(top) += tmHeight
    L.grid = g;
    L.rowsPerPage = g.Height() / L.rowH - 2;  // 原 rectHeight/rowH - 2（ヘッダ2行分を控除）
    if (L.rowsPerPage < 1) { L.rowsPerPage = 1; }
    return L;
}

// 原 OnPreparePrinting（0x4427e5）: 空文書はビープ+中止、そうでなければ DoPreparePrinting。
BOOL CStirlingView::OnPreparePrinting(CPrintInfo* pInfo) {
    if (Total() == 0) { ::MessageBeep(0); return FALSE; }
    return DoPreparePrinting(pInfo);
}

// 原 OnBeginPrinting（0x442840）: 印刷フォント生成＋総ページ数を SetMinPage/SetMaxPage。
void CStirlingView::OnBeginPrinting(CDC* pDC, CPrintInfo* pInfo) {
    SelectPrintFont(pDC);
    // ページ矩形（原は GetDeviceCaps(HORZRES/VERTRES)。m_rectDraw は OnPrint 時に確定）。
    CRect page(0, 0, pDC->GetDeviceCaps(HORZRES), pDC->GetDeviceCaps(VERTRES));
    const PrintLayout L = ComputePrintLayout(pDC, page);
    const stirling::FileOffset totalRows = PrintTotalRows();
    int pages = (L.rowsPerPage > 0)
                    ? static_cast<int>((totalRows + L.rowsPerPage - 1) / L.rowsPerPage) : 1;
    if (pages < 1) { pages = 1; }
    pInfo->SetMinPage(1);
    pInfo->SetMaxPage(pages);
}

// 原 OnEndPrinting（0x442af3）: フォント削除＋非プレビュー時に印刷範囲を解放。
void CStirlingView::OnEndPrinting(CDC* /*pDC*/, CPrintInfo* pInfo) {
    m_printFont.DeleteObject();
    if (pInfo == nullptr || !pInfo->m_bPreview) {
        m_printRangeActive = false;   // 原 FUN_0045e915（view+0x338 解放）
    }
}

// 印刷プレビューを**メインフレーム全体**に表示する（原の挙動）。
//   MDI の既定（CView::OnFilePrintPreview）は子フレーム内にプレビューを表示するため、
//   DoPrintPreview が親フレームに使う GetParentFrame() がメインフレームを返すよう、プレビュー中だけ
//   ビューを **MDICLIENT 直下** へ付け替える（定番手法 KB Q100259 の変種）。
//   ・GetParentFrame() は MDICLIENT の上位＝メインフレームを返す（MDICLIENT はフレームではない）。
//   ・ビューは MDICLIENT 内にあるので OnSetPreviewMode が MDICLIENT を隠すと一緒に隠れ、
//     メインフレーム直下のコントロールバー/ドッキング配置（プレビューツールバー）に干渉しない。
//     （メインフレーム直下へ付け替えるとツールバーがフローティング化してドラッグ移動できてしまう）。
//   ・ID 衝突も起きない（MDICLIENT の孫であり、GetDlgItem(mainFrame,0xE900) は MDICLIENT を返す）。
void CStirlingView::OnFilePrintPreview() {
    CMDIFrameWnd* pMainFrame = DYNAMIC_DOWNCAST(CMDIFrameWnd, AfxGetMainWnd());
    CMDIChildWnd* pChild = (pMainFrame != nullptr) ? pMainFrame->MDIGetActive() : nullptr;
    HWND hMdiClient = (pMainFrame != nullptr) ? pMainFrame->m_hWndMDIClient : nullptr;
    if (pMainFrame == nullptr || pChild == nullptr || hMdiClient == nullptr) {
        CView::OnFilePrintPreview();   // フォールバック（子フレーム内プレビュー）
        return;
    }

    m_pPreviewChildFrame = pChild;     // 復帰用に子フレームを記憶
    ::SetParent(m_hWnd, hMdiClient);   // ビューを MDICLIENT 直下へ付け替え

    CPrintPreviewState* pState = new CPrintPreviewState;
    if (!DoPrintPreview(AFX_IDD_PREVIEW_TOOLBAR, this, RUNTIME_CLASS(CStirlingPreviewView), pState)) {
        // 失敗時は付け替えを巻き戻す（OnEndPrintPreview は呼ばれない）。
        ::SetParent(m_hWnd, pChild->GetSafeHwnd());
        m_pPreviewChildFrame = nullptr;
        delete pState;
    }
}

// 原 OnEndPrintPreview（0x45eccf）: プレビュー終了で印刷範囲を解放。
//   併せて、全画面プレビュー用に MDICLIENT へ付け替えたビューを子フレームへ戻す。
void CStirlingView::OnEndPrintPreview(CDC* pDC, CPrintInfo* pInfo, POINT point, CPreviewView* pView) {
    m_printRangeActive = false;
    CView::OnEndPrintPreview(pDC, pInfo, point, pView);   // メインフレームのプレビュー解除（この時点で
                                                          // ビューは MDICLIENT 内＝GetParentFrame はメイン）
    if (m_pPreviewChildFrame != nullptr) {
        CFrameWnd* pChild = m_pPreviewChildFrame;
        m_pPreviewChildFrame = nullptr;
        // ビューを子フレームへ戻し、子フレームを再レイアウト・再アクティブ化する。
        ::SetParent(m_hWnd, pChild->GetSafeHwnd());
        pChild->RecalcLayout();
        if (CMDIFrameWnd* pMainFrame = DYNAMIC_DOWNCAST(CMDIFrameWnd, AfxGetMainWnd())) {
            if (CMDIChildWnd* pMdiChild = DYNAMIC_DOWNCAST(CMDIChildWnd, pChild)) {
                pMainFrame->MDIActivate(pMdiChild);
            }
        }
    }
}

// 印刷対象の文字セット名（右下に印字。ステータスバーと同じ表記）。
static const char* PrintCharsetName(int cs) {
    switch (cs) {
    case 0:  return "ASCII";
    case 1:  return "SHIFT-JIS";
    case 2:  return "EUC";
    case 3:  return "Unicode";
    case 4:  return "EBCDIC";
    case 5:  return "EBCIDK";
    case 6:  return "UTF-8";      // 移植で追加（Issue #98）
    default: return "ASCII";
    }
}

// 原 OnPrint（0x442b29 + ダンプ本体 FUN_00442f27/FUN_00443474）: 1ページ分を描画する。
void CStirlingView::OnPrint(CDC* pDC, CPrintInfo* pInfo) {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr) { return; }
    SelectPrintFont(pDC);

    const CRect page = pInfo->m_rectDraw;   // フレームワークが設定したページ矩形（論理単位）
    const PrintLayout L = ComputePrintLayout(pDC, page);
    const int charW = L.charW, rowH = L.rowH;
    const int bpr = m_bytesPerRow;
    const int radix = m_addrRadix;          // 0=10進 / 非0=16進
    const int cs = pDoc->GetCharset();
    const bool beBig = pDoc->IsByteOrderBig();
    const unsigned char* ebc = (cs == 4) ? EbcdicTable() : (cs == 5) ? EbcidkTable() : nullptr;

    static const wchar_t* const kHexDigits = L"0123456789ABCDEF";

    const stirling::ScopedSelectFont selPrintFont(pDC, &m_printFont);
    const int oldBk = pDC->SetBkMode(TRANSPARENT);
    const COLORREF oldTx = pDC->GetTextColor();
    const COLORREF oldBg = pDC->GetBkColor();

    // --- ページ装飾（原 0x442b29）: ファイル名/日付/ページ番号/文字セット名/外枠 ---
    const int decoLeft   = page.left + charW;
    const int decoRight  = page.right - charW;
    const int decoTop    = page.top + rowH;
    const int decoBottom = page.bottom - rowH * 2;
    pDC->SetTextColor(RGB(0, 0, 0));

    CString title = pDoc->GetPathName();          // ファイル名（左上）
    if (title.IsEmpty()) { title = pDoc->GetTitle(); }
    pDC->TextOut(decoLeft, decoTop, title);

    SYSTEMTIME stNow; ::GetLocalTime(&stNow);      // 日付（右上）原 "%d/%2d/%2d"
    CString date;
    date.Format(_T("%d/%2d/%2d"), stNow.wYear, stNow.wMonth, stNow.wDay);
    { CSize sz = pDC->GetTextExtent(date); pDC->TextOut(decoRight - sz.cx, decoTop, date); }

    CString pageStr;                               // ページ番号（下・中央）原 "- %d/%d "
    pageStr.Format(_T("- %u/%u "), pInfo->m_nCurPage, pInfo->GetMaxPage());
    { CSize sz = pDC->GetTextExtent(pageStr);
      pDC->TextOut((decoLeft + decoRight) / 2 - sz.cx / 2, decoBottom + rowH, pageStr); }

    CString csName(PrintCharsetName(cs));          // 文字セット名（右下）
    { CSize sz = pDC->GetTextExtent(csName);
      pDC->TextOut(decoRight - sz.cx, decoBottom + rowH, csName); }

    { const int boxTop = decoTop + rowH * 3 / 2;   // 外枠（黒・既定ペン）
      pDC->MoveTo(decoLeft, boxTop);
      pDC->LineTo(decoRight, boxTop);
      pDC->LineTo(decoRight, decoBottom);
      pDC->LineTo(decoLeft, decoBottom);
      pDC->LineTo(decoLeft, boxTop); }

    // --- ダンプグリッド（原 FUN_00442f27）---
    const int gx = L.grid.left;    // グリッド左端（アドレス欄原点）
    const int gy = L.grid.top;     // ヘッダ行の原点

    // アドレス欄の桁数（16進 " XXXXXXXX  " / 10進 "DDDDDDDDDD "）。
    //   4GB 未満は原と同じ 11 桁になり、印刷レイアウトは変わらない（Issue #21）。
    const int addrCols = (radix == 0) ? (AddrDigits() + 1) : (AddrDigits() + 3);
    // 1行の総桁数（原の 77 桁 = 11 + bpr*3 + 2 + bpr）。
    const int lineCols = addrCols + bpr * 3 + 2 + bpr;

    // ヘッダ行（" ADDRESS   " + 列見出し + "  " + 文字欄見出し）
    {
        CStringW hdr = L" ADDRESS   ";
        // アドレス欄が広がった分だけ見出しを右へ寄せる（データ行と桁を合わせる）。
        for (int i = 11; i < addrCols; ++i) { hdr += L' '; }
        for (int col = 0; col < bpr; ++col) {
            CStringW c; c.Format((radix == 0) ? L"%02d " : L"%02X ", col); hdr += c;
        }
        hdr += L"  ";
        for (int col = 0; col < bpr; ++col) {
            hdr += (radix == 0) ? static_cast<wchar_t>(L'0' + (col % 10)) : kHexDigits[col & 0xf];
        }
        pDC->SetTextColor(RGB(0, 0, 0));
        pDC->SetBkColor(RGB(255, 255, 255));
        pDC->TextOutW(gx, gy, hdr);
    }
    // ヘッダ下の区切り線（原: gx+charW 〜 gx+77*charW。桁が広がれば追従する）
    pDC->MoveTo(gx + charW, gy + rowH * 3 / 2);
    pDC->LineTo(gx + lineCols * charW, gy + rowH * 3 / 2);

    // このページのデータ行を算出（原 FUN_00442f27 のページ割付）
    const stirling::FileOffset start = PrintFirstAddr();
    const stirling::FileOffset end   = PrintLastAddr();
    const stirling::FileOffset totalRows = PrintTotalRows();
    const int rowsPerPage = L.rowsPerPage;
    const int pageIndex = static_cast<int>(pInfo->m_nCurPage) - 1;

    const stirling::FileOffset alignedStart = start - (start % bpr);
    const stirling::FileOffset rowAddrTop = alignedStart +
        static_cast<stirling::FileOffset>(pageIndex) * rowsPerPage * bpr;   // 先頭行アドレス（整列）
    stirling::FileOffset dataStart = (pageIndex == 0) ? start : rowAddrTop;   // データ読み出し開始
    if (dataStart < start) { dataStart = start; }
    long long byteCount = static_cast<long long>(rowsPerPage) * bpr;
    if (dataStart + byteCount > static_cast<long long>(end) + 1) {
        byteCount = static_cast<long long>(end) + 1 - dataStart;
    }
    if (byteCount < 0) { byteCount = 0; }

    // UTF-8 はページ境界で文字が途切れるとセル数が変わるため、末尾を最大 3 バイト余分に読む。
    //   先読みは指定範囲の論理終端 end までに限る。範囲外のバイトまで読むと
    //   BuildCharCellsUtf8 がそれを消費して文字欄だけ範囲外の文字を描いてしまう（Issue #124）。
    long long readCount = byteCount;
    if (cs == 6) {
        const long long avail = static_cast<long long>(Total()) - dataStart;
        const long long inRange = static_cast<long long>(end) + 1 - dataStart;
        long long limit = (inRange < avail) ? inRange : avail;
        if (limit < 0) { limit = 0; }
        readCount = (byteCount + 3 <= limit) ? (byteCount + 3) : limit;
        if (readCount < 0) { readCount = 0; }
    }
    const std::vector<unsigned char> buf = pDoc->ReadRange(dataStart, static_cast<int>(readCount));
    const int nbuf = static_cast<int>(buf.size());
    int gi = 0;
    int carry = InitialCarry(cs, dataStart, Total());
    // UTF-8 の持ち越し（セル数）と読み飛ばし。窓の先頭が文字の途中なら空白で詰める。
    int carryCellsUtf8 = 0;
    std::vector<unsigned char> utf8CellCache;
    if (cs == 6) {
        gi = InitialCarryUtf8(dataStart);
        carryCellsUtf8 = gi;
        utf8CellCache.assign(0x10000, 0);
    }

    stirling::FileOffset rowsThisPage =
        totalRows - static_cast<stirling::FileOffset>(pageIndex) * rowsPerPage;
    if (rowsThisPage > rowsPerPage) { rowsThisPage = rowsPerPage; }
    if (rowsThisPage < 0) { rowsThisPage = 0; }

    std::vector<INT> dx(static_cast<size_t>(bpr) + 4, charW);
    const int hexX  = gx + addrCols * charW;
    const int charX = gx + (addrCols + bpr * 3 + 2) * charW;

    for (int r = 0; r < rowsThisPage; ++r) {
        const stirling::FileOffset rowAddr = rowAddrTop + r * bpr;
        if (rowAddr > end) { break; }
        const int y = gy + (2 + r) * rowH;   // データ行はヘッダから2行下（原 local_18=top+2*rowH）

        // アドレス欄（既定 11 桁。16進 " XXXXXXXX  " / 10進 "DDDDDDDDDD "）
        CStringW addr;
        if (radix == 0) {
            addr.Format(L"%0*llu ", AddrDigits(), static_cast<long long>(rowAddr));
        } else {
            addr.Format(L" %0*llX  ", AddrDigits(), static_cast<long long>(rowAddr));
        }
        pDC->SetTextColor(RGB(0, 0, 0));
        pDC->SetBkColor(RGB(255, 255, 255));
        pDC->TextOutW(gx, y, addr);

        // 16進欄（バイト毎に印刷色。範囲外列は空白）
        pDC->SetBkMode(OPAQUE);
        for (int col = 0; col < bpr; ++col) {
            const stirling::FileOffset p = rowAddr + col;
            if (p < start || p > end) { continue; }
            const int bi = static_cast<int>(p - dataStart);
            if (bi < 0 || bi >= nbuf) { continue; }
            const unsigned char b = buf[bi];
            COLORREF fg = 0, bg = 0;
            GetPrintByteColor(p, fg, bg);
            CStringW cell; cell.Format(L"%02X", b);
            pDC->SetTextColor(fg);
            pDC->SetBkColor(bg);
            pDC->TextOutW(hexX + col * 3 * charW, y, cell);
            // 連続する比較差分は桁間スペースも背景色（原挙動: 比較のみ帯化）
            if (m_compareActive && InCompareDiff(p) && col + 1 < bpr) {
                const stirling::FileOffset p2 = rowAddr + col + 1;
                if (p2 <= end && InCompareDiff(p2)) {
                    pDC->FillSolidRect(hexX + col * 3 * charW + 2 * charW, y, charW, rowH, bg);
                }
            }
        }
        pDC->SetBkMode(TRANSPARENT);

        // 文字欄（先頭範囲外は空白詰め、BuildCharCells で文字セット別に構築）
        const int lead = (rowAddr < start) ? static_cast<int>(start - rowAddr) : 0;
        if (cs == 6) {   // UTF-8 はワイド描画（Issue #98）
            const stirling::ScopedSelectFont selUtf8(pDC, const_cast<CFont*>(&m_printFontUtf8));
            std::wstring wout;
            std::vector<INT> wdx;
            bool weof = false;
            int wcells = 0;
            std::wstring line(static_cast<size_t>(lead), L' ');
            std::vector<INT> ldx(static_cast<size_t>(lead), charW);
            BuildCharCellsUtf8(buf, gi, bpr - lead, carryCellsUtf8, pDC->GetSafeHdc(), charW,
                               &utf8CellCache, wout, wdx, wcells, weof);
            line += wout;
            ldx.insert(ldx.end(), wdx.begin(), wdx.end());
            pDC->SetTextColor(RGB(0, 0, 0));
            pDC->SetBkColor(RGB(255, 255, 255));
            if (!line.empty()) {
                ::ExtTextOutW(pDC->GetSafeHdc(), charX, y, 0, nullptr, line.c_str(),
                              static_cast<UINT>(line.size()), ldx.data());
            }
            continue;
        }
        std::string chars(static_cast<size_t>(lead), ' ');
        bool eof = false;
        chars += BuildCharCells(buf, gi, bpr - lead, cs, carry, beBig, ebc, eof);
        if (static_cast<int>(chars.size()) < bpr) {
            chars.append(static_cast<size_t>(bpr) - chars.size(), ' ');
        } else if (static_cast<int>(chars.size()) > bpr) {
            chars.resize(static_cast<size_t>(bpr));
        }
        pDC->SetTextColor(RGB(0, 0, 0));
        pDC->SetBkColor(RGB(255, 255, 255));
        // [byte層] 編集対象バイト列を CP932 として描画する。ワイド化しないこと。
        //   理由: dx 配列がバイト単位（16進欄と桁を揃える）／不正バイト列を保持する。
        //   印刷フォントも SHIFTJIS_CHARSET 固定で、画面と同一の写像で描く。
        //   詳細: analysis_artifacts/docs/20_unicode_layering.md §4.1
        ::ExtTextOutA(pDC->GetSafeHdc(), charX, y, 0, nullptr,
                      chars.c_str(), static_cast<UINT>(chars.size()), dx.data());
    }

    pDC->SetBkMode(oldBk);
    pDC->SetTextColor(oldTx);
    pDC->SetBkColor(oldBg);
}

// 範囲を指定して印刷（原 0x8064 FUN_0045c519→FUN_00447187）。
//   範囲指定ダイアログ（IDD 201）で印刷範囲＋プレビュー要否を選び、標準印刷コマンドへ委譲。
void CStirlingView::OnPrintRange() {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr) { return; }
    const stirling::FileOffset total = Total();
    if (total == 0) { ::MessageBeep(0); return; }   // 原 FUN_00447187: 空文書はビープ

    const bool hasSel = m_selActive;
    const stirling::FileOffset selStart = hasSel ? SelLo() : 0;
    const stirling::FileOffset selEnd   = hasSel ? (SelHi() - 1) : 0;   // 両端含む（SelHi は半開境界）

    CPrintRangeDlg dlg(this, total, hasSel, selStart, selEnd);
    if (dlg.DoModal() != IDOK) { return; }

    // 印刷範囲を確定（原 view+0x338 に {start,end} を確保）。
    m_printRangeActive = true;
    m_printRangeStart = dlg.Start();
    m_printRangeEnd   = dlg.End();

    // プレビュー経由 or 直接印刷（原: ID_FILE_PRINT_PREVIEW / ID_FILE_PRINT を送出）。
    SendMessage(WM_COMMAND, dlg.Preview() ? ID_FILE_PRINT_PREVIEW : ID_FILE_PRINT);
}

// 範囲を指定して選択（原 0x8065 FUN_0045baa3→FUN_0044956b）。
//   範囲指定ダイアログ（IDD 202）で[start,end]（両端含む）を選び、その範囲を選択状態にする。
//   空文書は何もしない（原: total==0 なら即return）。
void CStirlingView::OnSelectRange() {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr) { return; }
    const stirling::FileOffset total = Total();
    if (total == 0) { return; }        // 原 FUN_0044956b: 空文書は何もしない

    const bool hasSel = m_selActive;
    const stirling::FileOffset selStart = hasSel ? SelLo() : 0;
    const stirling::FileOffset selEnd   = hasSel ? (SelHi() - 1) : 0;   // 両端含む（SelHi は半開境界）

    CSelectRangeDlg dlg(this, total, hasSel, selStart, selEnd);
    if (dlg.DoModal() != IDOK) { return; }

    // 選択範囲を[start,end]（両端含む）に設定。アンカー=start、キャレット=end+1（終端境界）。
    const stirling::FileOffset start = dlg.Start();
    const stirling::FileOffset end   = dlg.End();
    m_selAnchor = start;
    m_caretPos  = (end + 1 <= total) ? (end + 1) : total;
    m_selActive = true;
    m_nibbleLow = false;
    EnsureCaretVisible();               // 原: キャレット(終端)を可視化
    Invalidate(FALSE);
    UpdateCaret();
    SetFocus();
}

namespace {
// ワイド文字列を UTF-8 バイト列へ（ダンプ保存の文字欄用。Issue #98）。
std::string Utf8BytesFromWide(const std::wstring& w) {
    const std::vector<unsigned char> b = stirling::Utf8FromWide(w.c_str(), w.size());
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}
}  // namespace

// 整形テキストダンプの書き出し（原 FUN_0045d3e2）。
//   1行 = アドレス(11) + 16進(bpr×"XX ") + 2空白 + 文字欄(bpr) + 1空白 + CRLF。
//   行頭が行境界に非整列の場合は先頭列を空白で詰める（原の部分先頭行処理）。
bool CStirlingView::WriteDumpImage(const CString& path, stirling::FileOffset startPos,
                                   stirling::FileOffset endPos) {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr) { return false; }
    if (endPos < startPos) { return true; }   // 空範囲は何もしない

    const int bpr = m_bytesPerRow;
    const int radix = m_addrRadix;             // 0=10進 / 非0=16進
    // アドレス欄の桁数（4GB 未満は原と同じ 8/10 桁＝欄幅 11。Issue #21）。
    const int addrDigits = AddrDigits();
    const int addrCols = (radix == 0) ? (addrDigits + 1) : (addrDigits + 3);
    const int cs = pDoc->GetCharset();
    const bool beBig = pDoc->IsByteOrderBig();
    const unsigned char* ebc = (cs == 4) ? EbcdicTable() : (cs == 5) ? EbcidkTable() : nullptr;

    CFile file;
    CFileException ex;
    if (!file.Open(path, CFile::modeCreate | CFile::modeWrite | CFile::shareExclusive, &ex)) {
        return false;   // 呼び元が原 1010 メッセージを表示
    }

    // 指定範囲のみを読む。UTF-8 でも範囲外へ先読みしない: 先読みしたバイトは
    //   BuildCharCellsUtf8 が消費して文字欄だけ範囲外の文字を描いてしまう（Issue #124）。
    //   ダンプは範囲全体を1つのバッファへ読むため、ページ境界の先読みも要らない。
    const stirling::FileOffset count = endPos - startPos + 1;
    const std::vector<unsigned char> buf = pDoc->ReadRange(startPos, count);
    const int nbuf = static_cast<int>(buf.size());

    const stirling::FileOffset rowStart = startPos - (startPos % bpr);
    int gi = 0;
    int carry = InitialCarry(cs, startPos, Total());
    // UTF-8（Issue #98）: 文字欄はセル整列を保ったままワイドで構築し、UTF-8 で書き出す。
    //   グリフ幅の実測に DC が要るので、画面と同じフォントを載せたクライアント DC を使う
    //   （表示とダンプで同じ見た目になるようにするため）。
    int carryCellsUtf8 = 0;
    std::vector<unsigned char> utf8CellCache;
    CClientDC dumpDC(this);
    CFont* pOldDumpFont = nullptr;
    if (cs == 6) {
        gi = InitialCarryUtf8(startPos);
        carryCellsUtf8 = gi;
        utf8CellCache.assign(0x10000, 0);
        if (m_fontUtf8.GetSafeHandle() != nullptr) {
            pOldDumpFont = dumpDC.SelectObject(&m_fontUtf8);
        }
    }

    std::string outAll;
    outAll.reserve(static_cast<size_t>((endPos - rowStart) / bpr + 2) *
                   (static_cast<size_t>(bpr) * 4 + 16));

    static const char* const kHexDigits = "0123456789ABCDEF";

    // ヘッダ（原 FUN_0045dd4e）: 1行目 " ADDRESS   " + 列見出し + "  " + 文字欄見出し + " " + CRLF、
    //   2行目 = 行幅分の区切り線 '-' + CRLF。行幅 = addrCols + bpr*3 + 2 + bpr + 1。
    //   addrCols は 4GB 未満なら原と同じ 11（Issue #21）。
    {
        std::string header = " ADDRESS   ";
        if (addrCols > 11) { header.append(static_cast<size_t>(addrCols - 11), ' '); }
        for (int col = 0; col < bpr; ++col) {
            char c[8];
            _snprintf_s(c, sizeof(c), _TRUNCATE, (radix == 0) ? "%02d " : "%02X ", col);
            header += c;
        }
        header += "  ";
        for (int col = 0; col < bpr; ++col) {
            header += (radix == 0) ? static_cast<char>('0' + (col % 10)) : kHexDigits[col & 0xf];
        }
        header += " \r\n";
        header.append(static_cast<size_t>(addrCols) + static_cast<size_t>(bpr) * 4 + 3, '-');
        header += "\r\n";
        outAll += header;
    }

    for (stirling::FileOffset rowAddr = rowStart; rowAddr <= endPos; rowAddr += bpr) {
        std::string line;
        // アドレス欄（既定 11 桁）: 16進 " XXXXXXXX  " / 10進 "DDDDDDDDDD "（原 FUN_0045dfb1）
        char addr[32];
        if (radix == 0) {
            _snprintf_s(addr, sizeof(addr), _TRUNCATE, "%0*llu ",
                        addrDigits, static_cast<long long>(rowAddr));
        } else {
            _snprintf_s(addr, sizeof(addr), _TRUNCATE, " %0*llX  ",
                        addrDigits, static_cast<long long>(rowAddr));
        }
        line += addr;

        // 16進欄（bpr セル "XX "。範囲外の列は空白）
        for (int col = 0; col < bpr; ++col) {
            const stirling::FileOffset p = rowAddr + col;
            if (p >= startPos && p <= endPos) {
                const int bi = static_cast<int>(p - startPos);
                const unsigned char b = (bi < nbuf) ? buf[bi] : 0;
                char h[4] = { kHexDigits[(b >> 4) & 0xf], kHexDigits[b & 0xf], ' ', 0 };
                line += h;
            } else {
                line += "   ";
            }
        }
        line += "  ";   // 16進欄と文字欄の区切り 2 空白

        // 文字欄（bpr セル + 末尾 1 空白 = bpr+1 幅。先頭の範囲外列は空白詰め）
        const int lead = (rowAddr < startPos) ? static_cast<int>(startPos - rowAddr) : 0;
        if (cs == 6) {   // UTF-8（Issue #98）
            std::wstring wout;
            std::vector<INT> wdx;
            bool weof = false;
            int wcells = 0;
            BuildCharCellsUtf8(buf, gi, bpr - lead, carryCellsUtf8, dumpDC.GetSafeHdc(),
                               m_charW, &utf8CellCache, wout, wdx, wcells, weof);
            std::wstring wline(static_cast<size_t>(lead), L' ');
            wline += wout;
            // セル数で右端を揃える（全角グリフは 1 文字で 2 セルぶんの幅を占める）。
            const int used = lead + wcells;
            if (used < bpr + 1) { wline.append(static_cast<size_t>(bpr + 1 - used), L' '); }
            line += Utf8BytesFromWide(wline);
        } else {
        std::string chars(static_cast<size_t>(lead), ' ');
        bool eof = false;
        chars += BuildCharCells(buf, gi, bpr - lead, cs, carry, beBig, ebc, eof);
        if (static_cast<int>(chars.size()) < bpr + 1) {
            chars.append(static_cast<size_t>(bpr + 1) - chars.size(), ' ');
        } else if (static_cast<int>(chars.size()) > bpr + 1) {
            chars.resize(static_cast<size_t>(bpr) + 1);   // DBCS 行末オーバーシュートを末尾空白位置に収める
        }
        line += chars;
        }
        line += "\r\n";

        outAll += line;
    }

    if (pOldDumpFont != nullptr) { dumpDC.SelectObject(pOldDumpFont); }

    file.Write(outAll.data(), static_cast<UINT>(outAll.size()));
    file.Close();
    return true;
}

// ===========================================================================
// 検索（原 CSearchDlg→FindNextImpl→BlockCursor_SearchPattern）
// ===========================================================================

// 文字列入力→現文字セットのバイト列。
//   入力欄はワイド（wide 層）。1文字ずつ CP932 のコード値へ写し、入力変換で再エンコードする。
//   CP932 に無い文字（€ やサロゲートペア等）は WideCharToMultiByte の既定置換で '?' になる。
//   MBCS ビルドでは ANSI 編集コントロールが同じ置換をしていたため、原と同じ結果になる。
std::vector<unsigned char> CStirlingView::BuildTextBytes(LPCWSTR text) const {
    std::vector<unsigned char> out;
    CStirlingDoc* pDoc = GetDocument();
    const int cs = (pDoc != nullptr) ? pDoc->GetCharset() : 1;
    if (text == nullptr) { return out; }
    if (cs == 6) { return stirling::Utf8FromWide(text, ::wcslen(text)); }   // Issue #98
    for (LPCWSTR pw = text; *pw != L'\0'; ++pw) {
        const wchar_t wc = *pw;
        char mb[8] = {0};
        const int mn = ::WideCharToMultiByte(932, 0, &wc, 1, mb, sizeof(mb), nullptr, nullptr);
        unsigned int ch;
        if (mn == 2) { ch = (static_cast<unsigned char>(mb[0]) << 8) | static_cast<unsigned char>(mb[1]); }
        else if (mn == 1) { ch = static_cast<unsigned char>(mb[0]); }
        else { continue; }
        std::vector<unsigned char> b = TranslateInputChar(cs, ch);
        out.insert(out.end(), b.begin(), b.end());
    }
    return out;
}

// ビュー非依存の文字列→バイト列変換（原 FUN_004600f6 相当）。BuildTextBytes と
//   同じくワイド入力を1文字ずつ TranslateInputChar で再エンコードする。
std::vector<unsigned char> CStirlingView::EncodeText(int charset, LPCWSTR text) {
    std::vector<unsigned char> out;
    if (text == nullptr) { return out; }
    if (charset == 6) { return stirling::Utf8FromWide(text, ::wcslen(text)); }   // Issue #98
    for (LPCWSTR pw = text; *pw != L'\0'; ++pw) {
        const wchar_t wc = *pw;
        char mb[8] = {0};
        const int mn = ::WideCharToMultiByte(932, 0, &wc, 1, mb, sizeof(mb), nullptr, nullptr);
        unsigned int ch;
        if (mn == 2) { ch = (static_cast<unsigned char>(mb[0]) << 8) | static_cast<unsigned char>(mb[1]); }
        else if (mn == 1) { ch = static_cast<unsigned char>(mb[0]); }
        else { continue; }
        std::vector<unsigned char> b = TranslateInputChar(charset, ch);
        out.insert(out.end(), b.begin(), b.end());
    }
    return out;
}

bool CStirlingView::DoSearch(const std::vector<unsigned char>& pattern, bool forward, int rangeMode) {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr || pattern.empty()) { return false; }
    const stirling::FileOffset total = Total();
    const int plen = static_cast<int>(pattern.size());
    if (plen > total) { return false; }

    // 検索範囲 [start, end] の決定。core SearchPattern:
    //   forward: start=一致先頭の下限, end=走査上限(0で全長)。backward: start=一致末尾側の上限, end=下限。
    stirling::FileOffset start = 0, end = 0;

    if (rangeMode == 2) {
        // 選択範囲内: 最初の検索で対象範囲を確定し、以降は固定（一致で選択が変わっても不変）。
        if (!m_findSelCaptured) {
            if (m_selActive) { m_findSelLo = SelLo(); m_findSelHi = SelHi(); }
            else { m_findSelLo = 0; m_findSelHi = total; }
            m_findSelCaptured = true;
            // 初回: 前方=範囲先頭 / 後方=範囲末尾 から検索。
            if (forward) { start = m_findSelLo; end = m_findSelHi; }
            else         { start = m_findSelHi - 1; end = m_findSelLo; }
        } else {
            // 2回目以降: 固定範囲内でカーソル継続。
            if (forward) {
                start = (m_caretPos + 1 > m_findSelLo) ? (m_caretPos + 1) : m_findSelLo;
                end = m_findSelHi;
            } else {
                start = (m_caretPos - 1 < m_findSelHi - 1) ? (m_caretPos - 1) : (m_findSelHi - 1);
                end = m_findSelLo;
            }
        }
    } else if (rangeMode == 1 && !m_wholeSearchStarted) {
        // データ全体の初回: 先頭(前方)/末尾(後方)から。以降はカーソル継続。
        m_wholeSearchStarted = true;
        start = forward ? 0 : (total - 1);
        end = 0;
    } else {
        // カーソル位置から（またはデータ全体の2回目以降）。
        start = forward ? (m_caretPos + 1) : (m_caretPos - 1);
        end = 0;
    }
    if (forward && start < 0) { start = 0; }

    stirling::BlockCursor cur(&pDoc->Blocks());
    stirling::FileOffset foundPos = -1;   // core は 64bit 位置を返す（Issue #19）
    const int dir = forward ? stirling::BlockCursor::kForward
                            : stirling::BlockCursor::kBackward;
    const bool found = cur.SearchPattern(pattern.data(), plen, &foundPos, dir, start, end);
    if (!found) {
        return false;   // 未発見は無反応（原は beep しない）
    }
    const stirling::FileOffset outPos = foundPos;
    // 一致範囲 [outPos, outPos+plen) を選択。キャレットは一致先頭に置き、次回検索を継続可能に。
    m_selActive = true;
    m_selAnchor = outPos + plen;
    m_caretPos  = outPos;
    m_nibbleLow = false;
    CenterCaretRow();   // 画面外なら一致箇所を縦中央へ（原挙動）
    Invalidate(FALSE);
    UpdateCaret();
    return true;
}

// 不一致検索の実体（原 FUN_0044b654 の不一致分岐 + コア FUN_0041d6bf）。
//   範囲/セッション決定は DoSearch と同一（対象は長さ1のバイト value）。
bool CStirlingView::DoMismatchSearch(unsigned char value, bool forward, int rangeMode) {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr) { return false; }
    const stirling::FileOffset total = Total();
    if (total <= 0) { return false; }

    // 検索範囲 [start, end] の決定（DoSearch と同一ロジック。plen=1 相当）。
    stirling::FileOffset start = 0, end = 0;
    if (rangeMode == 2) {
        // 選択範囲内: 初回に対象範囲を確定し以降固定（一致で選択が変わっても不変）。
        if (!m_findSelCaptured) {
            if (m_selActive) { m_findSelLo = SelLo(); m_findSelHi = SelHi(); }
            else { m_findSelLo = 0; m_findSelHi = total; }
            m_findSelCaptured = true;
            if (forward) { start = m_findSelLo; end = m_findSelHi; }
            else         { start = m_findSelHi - 1; end = m_findSelLo; }
        } else {
            if (forward) {
                start = (m_caretPos + 1 > m_findSelLo) ? (m_caretPos + 1) : m_findSelLo;
                end = m_findSelHi;
            } else {
                start = (m_caretPos - 1 < m_findSelHi - 1) ? (m_caretPos - 1) : (m_findSelHi - 1);
                end = m_findSelLo;
            }
        }
    } else if (rangeMode == 1 && !m_wholeSearchStarted) {
        m_wholeSearchStarted = true;
        start = forward ? 0 : (total - 1);
        end = 0;
    } else {
        start = forward ? (m_caretPos + 1) : (m_caretPos - 1);
        end = 0;
    }
    if (forward && start < 0) { start = 0; }

    stirling::BlockCursor cur(&pDoc->Blocks());
    stirling::FileOffset foundPos = -1;   // core は 64bit 位置を返す（Issue #19）
    const int dir = forward ? stirling::BlockCursor::kForward
                            : stirling::BlockCursor::kBackward;
    if (!cur.SearchMismatch(value, &foundPos, dir, start, end)) {
        return false;   // 未発見は無反応（原は beep しない）
    }
    const stirling::FileOffset outPos = foundPos;
    // 不一致箇所 [outPos, outPos+1) を選択。キャレットは不一致先頭に置き継続可能に。
    m_selActive = true;
    m_selAnchor = outPos + 1;
    m_caretPos  = outPos;
    m_nibbleLow = false;
    CenterCaretRow();
    Invalidate(FALSE);
    UpdateCaret();
    return true;
}

// キャレット行が可視範囲外なら、その行が縦中央に来るようスクロール（検索ジャンプ用）。
void CStirlingView::CenterCaretRow() {
    const stirling::FileOffset row = CaretRow();
    const int vis = VisibleRows();
    if (vis <= 0) { return; }
    if (row >= m_topLine && row < m_topLine + vis) {
        return;   // 既に可視ならスクロールしない（原挙動）
    }
    stirling::FileOffset newTop = row - vis / 2;
    if (newTop < 0) { newTop = 0; }
    if (newTop != m_topLine) {
        m_topLine = newTop;
        UpdateScrollInfo();
        Invalidate(FALSE);
        SyncPropagate();   // ジャンプ/検索等による縦中央スクロールも同期
    }
}

void CStirlingView::FindWithBytes(const std::vector<unsigned char>& pattern,
                                  int rangeMode, bool forward) {
    if (pattern.empty()) { return; }
    // 検索条件が変わったら（種別=不一致→通常の切替含む）「データ全体」初回フラグをリセット。
    if (m_lastFindMismatch || pattern != m_lastFindPattern || rangeMode != m_lastFindRange) {
        m_wholeSearchStarted = false;
    }
    // 検索範囲を切り替えたら「選択範囲内」の固定範囲を破棄（次回選択時に再確定）。
    if (rangeMode != m_lastFindRange) {
        m_findSelCaptured = false;
    }
    m_lastFindMismatch = false;   // 直近は通常検索（繰り返しは通常で継続）
    m_lastFindPattern = pattern;
    m_lastFindRange = rangeMode;
    NotifySearchResult(DoSearch(pattern, forward, rangeMode));
}

// 検索不一致時のフィードバック（原 FUN_0044b654: view+0x264 有効=メッセージ / 無効=ビープ）。
void CStirlingView::NotifySearchResult(bool found) {
    if (found) { return; }
    if (theApp.AppSettings().searchNotFoundMsg) {
        wchar_t msg[128] = {0};
        ::LoadStringW(AfxGetResourceHandle(), IDS_SEARCH_NOTFOUND, msg, 128);   // "見つかりませんでした"
        ui::MsgBox(GetSafeHwnd(), msg, MB_OK | MB_ICONINFORMATION);
    } else {
        ::MessageBeep(0);
    }
}

// 不一致検索（原 0x8032）: 直近条件を記録し、条件変更時は全体検索フラグをリセット。
void CStirlingView::FindMismatchWithByte(unsigned char value, int rangeMode, bool forward) {
    // 条件（値/範囲/種別=通常→不一致の切替）が変わったら「データ全体」初回フラグをリセット。
    if (!m_lastFindMismatch || value != m_lastFindByte || rangeMode != m_lastFindRange) {
        m_wholeSearchStarted = false;
    }
    if (rangeMode != m_lastFindRange) {
        m_findSelCaptured = false;
    }
    m_lastFindMismatch = true;   // 直近は不一致検索（繰り返しは不一致で継続）
    m_lastFindByte = value;
    m_lastFindRange = rangeMode;
    NotifySearchResult(DoMismatchSearch(value, forward, rangeMode));
}

void CStirlingView::ResetFindSession() {
    // ダイアログを開くたびに検索セッションを初期化（原: 閉じるまで範囲を固定）。
    m_wholeSearchStarted = false;
    m_findSelCaptured = false;
}

void CStirlingView::OnEditFind() {
    ResetFindSession();   // 新規セッション: 範囲固定を初期化（原は閉じるまで固定）
    CFindDlg dlg(this);   // モーダル（原と同じ。Next/Prev で検索、閉じるで終了）
    dlg.DoModal();
}

// 不一致検索ダイアログを開く（0x8032, 原 CStirlingView_OnFindMismatch）。
void CStirlingView::OnFindMismatch() {
    ResetFindSession();
    CFindMismatchDlg dlg(this);   // モーダル（CFindDlg と同じ挙動）
    dlg.DoModal();
}

// [lo,hi) 内の search を repl で前方一括置換（原 FUN_0044c0d1）。返り値=置換件数。
int CStirlingView::ReplaceAll(const std::vector<unsigned char>& search,
                              const std::vector<unsigned char>& repl,
                              stirling::FileOffset lo, stirling::FileOffset hi) {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr || search.empty()) { return 0; }
    const int slen = static_cast<int>(search.size());
    int count = 0;
    stirling::FileOffset p = (lo < 0) ? 0 : lo;
    stirling::FileOffset endBound = hi;
    for (;;) {
        const stirling::FileOffset total = Total();
        if (endBound > total) { endBound = total; }
        if (p + slen > endBound) { break; }
        stirling::BlockCursor cur(&pDoc->Blocks());   // 置換で構造が変わるため毎回作り直す
        stirling::FileOffset foundPos = -1;   // core は 64bit 位置を返す（Issue #19）
        if (!cur.SearchPattern(search.data(), slen, &foundPos,
                               stirling::BlockCursor::kForward, p, endBound)) {
            break;
        }
        const stirling::FileOffset outPos = foundPos;
        if (outPos + slen > endBound) { break; }
        if (!pDoc->ReplaceRange(outPos, slen, repl)) {
            break;   // 置換できなかった（中止等）。ここまでの件数で打ち切る
        }
        ++count;
        endBound += static_cast<int>(repl.size()) - slen;   // 範囲終端は長さ差だけ移動
        p = outPos + static_cast<int>(repl.size());          // 置換後の直後から継続
    }
    if (count > 0) {
        m_selActive = false;
        m_nibbleLow = false;
        m_caretPos = (p > Total()) ? Total() : p;
        AfterEdit(m_caretPos);
    }
    return count;
}

void CStirlingView::OnEditReplace() {
    CStirlingDoc* pDoc = GetDocument();
    if (pDoc == nullptr) { return; }
    if (!pDoc->CanEdit()) { ::MessageBeep(0); return; }   // 置換はデータ変更のため編集禁止中は不可
    ResetFindSession();
    CReplaceDlg dlg(this);
    if (dlg.DoModal() != IDOK || dlg.GetAction() == CReplaceDlg::kNone) {
        return;
    }
    const std::vector<unsigned char> sbytes = dlg.SearchBytes();
    const std::vector<unsigned char> rbytes = dlg.ReplaceBytes();
    const int range = dlg.GetRange();
    const CReplaceDlg::Action act = dlg.GetAction();
    m_lastFindPattern = sbytes;
    m_lastFindRange = range;

    // 置換対象範囲 [rlo, rhi) を決定。
    const stirling::FileOffset total = Total();
    stirling::FileOffset rlo = 0, rhi = 0;
    if (range == 2) {   // 選択範囲内
        if (m_selActive) { rlo = SelLo(); rhi = SelHi(); }
        else { rlo = 0; rhi = total; }
    } else if (range == 1) {   // データ全体
        rlo = 0; rhi = total;
    } else {   // カーソル位置から
        rlo = m_caretPos; rhi = total;
    }

    if (act == CReplaceDlg::kAll) {
        const int n = ReplaceAll(sbytes, rbytes, rlo, rhi);
        CStringW msg;
        msg.Format(ui::LoadW(IDS_REPLACE_COUNT), n);   // "%d個置換しました"
        ui::MsgBox(GetSafeHwnd(), msg, MB_OK | MB_ICONINFORMATION);
        return;
    }

    // 対話置換（次検索=前方 / 前検索=後方）。一致→確認→実行/スキップ/一括/キャンセルのループ。
    const bool forward = (act == CReplaceDlg::kNext);
    for (;;) {
        if (!DoSearch(sbytes, forward, range)) {
            break;   // これ以上一致が無い
        }
        CReplaceConfirmDlg confirm(this);
        const int r = static_cast<int>(confirm.DoModal());
        if (r == CReplaceConfirmDlg::kExec) {
            const stirling::FileOffset pos = SelLo();
            if (!pDoc->ReplaceRange(pos, static_cast<int>(sbytes.size()), rbytes)) {
                break;   // 置換できなかった（中止等）
            }
            m_selActive = false;
            m_nibbleLow = false;
            // 次検索の継続位置。前方は置換後の直後、後方は置換位置の手前から。
            m_caretPos = forward ? (pos + static_cast<int>(rbytes.size()) - 1) : pos;
            if (m_caretPos < 0) { m_caretPos = 0; }
            AfterEdit(m_caretPos);
        } else if (r == CReplaceConfirmDlg::kSkip) {
            // 何もしない（キャレットは一致先頭。次の DoSearch が先へ進む）。
        } else if (r == CReplaceConfirmDlg::kAll) {
            // 残りを一括置換（現在位置から範囲終端まで、前方）。
            const int n = ReplaceAll(sbytes, rbytes, m_caretPos, rhi);
            CStringW msg;   // 原 1017「%d個置換しました」（wsprintf は長さ制限なしのため不使用）
            msg.Format(ui::LoadW(IDS_REPLACE_COUNT), n);
            ui::MsgBox(GetSafeHwnd(), msg, MB_OK | MB_ICONINFORMATION);
            break;
        } else {
            break;   // キャンセル/閉じる
        }
    }
}

// 繰り返し検索（0x8037/0x8036）。原は ON_UPDATE_COMMAND_UI を持たず常に実行でき、
//   直近の検索条件が無い場合は検索ダイアログを開いてから検索する
//   （原 CStirlingView_FindNextImpl 0x44b486: view+0x1d0==0 で CSearchDlg を DoModal）。
//   移植版は一時期この 2 コマンドへ「条件がある時のみ活性」の更新ハンドラを付けていたが、
//   MFC は無効なコマンドの WM_COMMAND を配送しないため、キー割り当てから実行しても
//   何も起きなかった（Issue #72）。原に合わせて更新ハンドラを持たない。
void CStirlingView::OnFindNextCmd() {
    if (m_lastFindMismatch) { NotifySearchResult(DoMismatchSearch(m_lastFindByte, true, m_lastFindRange)); return; }
    if (m_lastFindPattern.empty()) { OnEditFind(); return; }
    NotifySearchResult(DoSearch(m_lastFindPattern, true, m_lastFindRange));
}

void CStirlingView::OnFindPrevCmd() {
    if (m_lastFindMismatch) { NotifySearchResult(DoMismatchSearch(m_lastFindByte, false, m_lastFindRange)); return; }
    if (m_lastFindPattern.empty()) { OnEditFind(); return; }
    NotifySearchResult(DoSearch(m_lastFindPattern, false, m_lastFindRange));
}
