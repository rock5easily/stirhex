// CStructBar 実装（上/下ドッキングおよびフローティング対応の CDialogBar）。
#include "pch.h"
#include "app/UiStrings.h"   // UI文字列はリソースから
#include "frame/StructBar.h"
#include "view/StirlingView.h"
#include "doc/StirlingDoc.h"
#include "app/StirlingApp.h"
#include "app/ShellUtil.h"   // ui::ModuleDirectory（MAX_PATH 非依存の exe ディレクトリ）
#include "core/Cp932Text.h"
#include "dialog/StructAddressDlg.h"
#include "resource.h"
#include "util/ScopedGdi.h"   // GDI オブジェクトの RAII（Issue #48）

#include <strsafe.h>   // StringCchCopyW

namespace {
const UINT kRefreshTimer = 1;   // 状態変化検出タイマー
const UINT kDeferredLayoutMessage = WM_APP + 0x122;
const UINT kHeaderControlIds[] = {
    IDC_STRUCT_RELOAD, IDC_STRUCT_COMBO, IDC_STRUCT_ADDR,
    IDC_STRUCT_PREVREC, IDC_STRUCT_PREVBYTE, IDC_STRUCT_GOTO,
    IDC_STRUCT_NEXTBYTE, IDC_STRUCT_NEXTREC, IDCANCEL, IDC_STRUCT_LIST,
};
const UINT kStatusControlIds[] = {
    IDC_STRUCT_STATUS_EDIT, IDC_STRUCT_STATUS_CS, IDC_STRUCT_STATUS_ORDER,
};
}

BEGIN_MESSAGE_MAP(CStructResizeGrip, CStatic)
    ON_WM_SETCURSOR()
    ON_WM_LBUTTONDOWN()
END_MESSAGE_MAP()

BOOL CStructResizeGrip::OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message) {
    UNREFERENCED_PARAMETER(pWnd);
    UNREFERENCED_PARAMETER(nHitTest);
    UNREFERENCED_PARAMETER(message);
    ::SetCursor(AfxGetApp()->LoadStandardCursor(IDC_SIZENS));
    return TRUE;
}

void CStructResizeGrip::OnLButtonDown(UINT nFlags, CPoint point) {
    UNREFERENCED_PARAMETER(nFlags);
    UNREFERENCED_PARAMETER(point);
    if (m_owner != nullptr) m_owner->BeginDockedResize();
}

CStructBar::CStructBar() {}

BEGIN_MESSAGE_MAP(CStructBar, CDialogBar)
    ON_BN_CLICKED(IDC_STRUCT_RELOAD, &CStructBar::OnReload)
    ON_CBN_SELCHANGE(IDC_STRUCT_COMBO, &CStructBar::OnSelChange)
    ON_BN_CLICKED(IDC_STRUCT_PREVREC, &CStructBar::OnNavPrevRec)
    ON_BN_CLICKED(IDC_STRUCT_PREVBYTE, &CStructBar::OnNavPrevByte)
    ON_BN_CLICKED(IDC_STRUCT_GOTO, &CStructBar::OnNavGoto)
    ON_BN_CLICKED(IDC_STRUCT_NEXTBYTE, &CStructBar::OnNavNextByte)
    ON_BN_CLICKED(IDC_STRUCT_NEXTREC, &CStructBar::OnNavNextRec)
    ON_BN_CLICKED(IDCANCEL, &CStructBar::OnCloseButton)
    ON_WM_SIZE()
    ON_WM_SHOWWINDOW()
    ON_WM_WINDOWPOSCHANGED()
    ON_WM_SETCURSOR()
    ON_WM_LBUTTONDOWN()
    ON_WM_LBUTTONUP()
    ON_WM_MOUSEMOVE()
    ON_WM_CAPTURECHANGED()
    ON_MESSAGE(kDeferredLayoutMessage, &CStructBar::OnDeferredLayout)
    ON_WM_TIMER()
    ON_NOTIFY(LVN_GETDISPINFO, IDC_STRUCT_LIST, &CStructBar::OnGetDispInfo)
    ON_NOTIFY(NM_CUSTOMDRAW, IDC_STRUCT_LIST, &CStructBar::OnCustomDraw)
    ON_NOTIFY(NM_CLICK, IDC_STRUCT_LIST, &CStructBar::OnClickList)
    ON_NOTIFY(NM_RCLICK, IDC_STRUCT_LIST, &CStructBar::OnRClickList)
    ON_NOTIFY(NM_DBLCLK, IDC_STRUCT_LIST, &CStructBar::OnDblClkList)
    ON_NOTIFY(LVN_KEYDOWN, IDC_STRUCT_LIST, &CStructBar::OnKeyDownList)
    ON_EN_KILLFOCUS(IDC_STRUCT_EDITBOX, &CStructBar::OnEditKillFocus)
END_MESSAGE_MAP()

namespace {
const int kIndentPx = 16;   // 階層 1 段のインデント幅
const int kGlyphPx  = 9;    // [+]/[-] ボックスの辺長
}

CListCtrl* CStructBar::List() { return reinterpret_cast<CListCtrl*>(GetDlgItem(IDC_STRUCT_LIST)); }
CComboBox* CStructBar::Combo() { return reinterpret_cast<CComboBox*>(GetDlgItem(IDC_STRUCT_COMBO)); }

BOOL CStructBar::CreateBar(CWnd* pParent) {
    // CDialogBar テンプレート（WS_CHILD・非 WS_VISIBLE）として生成する。
    if (!Create(pParent, IDD_STRUCT_EDIT_BAR,
                CBRS_TOP | CBRS_TOOLTIPS | CBRS_FLYBY | CBRS_SIZE_DYNAMIC,
                IDW_STRUCT_BAR)) {
        return FALSE;
    }
    ::SetWindowTextW(GetSafeHwnd(), ui::CommandNameW(0x030E));   // 原 5414「構造体編集」   // フローティングミニフレームのタイトル
    SetupList();
    // 値のインプレース編集ボックス（バーの子として生成し、既定は非表示）。
    if (!m_editCtrl.Create(WS_CHILD | ES_AUTOHSCROLL | WS_BORDER, CRect(0, 0, 0, 0),
                           this, IDC_STRUCT_EDITBOX)) {
        return FALSE;
    }
    if (!m_resizeGrip.Create(_T(""), WS_CHILD | SS_NOTIFY | SS_ETCHEDHORZ,
                             CRect(0, 0, 0, 0), this, IDC_STRUCT_RESIZE_GRIP)) {
        return FALSE;
    }
    m_resizeGrip.SetOwner(this);
    if (CListCtrl* list = List()) m_editCtrl.SetFont(list->GetFont());
    CaptureInitialLayout();
    ReloadDefs();
    SetTimer(kRefreshTimer, 150, nullptr);   // 可視時のみ Refresh（OnTimer で判定）
    EnableDocking(CBRS_ALIGN_TOP | CBRS_ALIGN_BOTTOM);
    return TRUE;
}

CSize CStructBar::CalcFixedLayout(BOOL bStretch, BOOL bHorz) {
    CSize size = CDialogBar::CalcFixedLayout(bStretch, bHorz);
    if (bHorz && !IsFloating()) {
        // MFC固定バーでドック行の残り幅まで伸ばすためのセンチネル。
        // 子一覧・列幅はLayoutChildrenで実可視幅へクランプし、この値を使用しない。
        size.cx = 32767;
    }
    if (bHorz && DockPosition() != 2 && m_dockedHeight > 0) {
        size.cy = m_dockedHeight;
    }
    return size;
}

void CStructBar::CaptureInitialLayout() {
    GetClientRect(&m_initialClientRect);
    for (size_t i = 0; i < m_headerRects.size(); ++i) {
        if (CWnd* wnd = GetDlgItem(kHeaderControlIds[i])) {
            wnd->GetWindowRect(&m_headerRects[i]);
            ScreenToClient(&m_headerRects[i]);
        }
    }
    m_initialListRect = m_headerRects.back();
    for (size_t i = 0; i < m_statusWidths.size(); ++i) {
        if (CWnd* wnd = GetDlgItem(kStatusControlIds[i])) {
            CRect rc;
            wnd->GetWindowRect(&rc);
            ScreenToClient(&rc);
            m_statusWidths[i] = rc.Width();
            m_statusHeight = max(m_statusHeight, rc.Height());
            wnd->ShowWindow(SW_HIDE);
        }
    }
    m_dockedHeight = m_initialClientRect.Height();
    m_layoutReady = true;
    LayoutChildren();
}

void CStructBar::ApplyDisplaySettings(int statusPos, bool keepColumnRatio) {
    Invalidate(TRUE);   // 旧位置のステータス／操作列を消去してから再配置する
    m_statusPos = (statusPos >= 0 && statusPos <= 2) ? statusPos : 2;
    const bool enablingRatio = keepColumnRatio && !m_keepColumnRatio;
    m_keepColumnRatio = keepColumnRatio;
    // 有効化直後のフロート/ドック遷移中サイズを基準にしない。
    // LayoutChildren 内で一覧を安定サイズへ移動した後に再取得する。
    if (enablingRatio) m_ratioListWidth = 0;
    LayoutChildren();
    UpdateStructStatus();
    RedrawWindow(nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void CStructBar::FitToFrame() {
    LayoutChildren();
    QueueDeferredLayout();
}

void CStructBar::QueueDeferredLayout() {
    if (m_deferredLayoutPosted || GetSafeHwnd() == nullptr) return;
    m_deferredLayoutPosted = true;
    if (!PostMessage(kDeferredLayoutMessage)) {
        m_deferredLayoutPosted = false;
    }
}

LRESULT CStructBar::OnDeferredLayout(WPARAM wParam, LPARAM lParam) {
    UNREFERENCED_PARAMETER(wParam);
    UNREFERENCED_PARAMETER(lParam);
    m_deferredLayoutPosted = false;
    LayoutChildren();
    RedrawWindow(nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    return 0;
}

void CStructBar::CaptureColumnRatio() {
    if (m_applyingColumnRatio) return;
    CListCtrl* list = List();
    if (list == nullptr) return;
    CRect client;
    list->GetClientRect(&client);
    if (client.Width() < 100) return;
    std::array<int, 3> widths;
    for (int i = 0; i < 3; ++i) {
        const int width = list->GetColumnWidth(i);
        if (width <= 0 || width > client.Width() * 4) return;
        widths[i] = width;
    }
    m_ratioColumnWidths = widths;
    m_ratioListWidth = widths[0] + widths[1] + widths[2];
}

void CStructBar::CaptureManualColumnRatioIfChanged() {
    if (!m_keepColumnRatio || m_applyingColumnRatio ||
        m_ratioListWidth <= 0 || m_lastLaidOutListWidth <= 0) {
        return;
    }
    CListCtrl* list = List();
    if (list == nullptr) return;
    CRect client;
    list->GetClientRect(&client);
    // コンテナのサイズ遷移中は列幅変更とみなさない。
    if (abs(client.Width() - m_lastLaidOutListWidth) > 2) return;
    for (int i = 0; i < 3; ++i) {
        const int expected = max(24, ::MulDiv(m_ratioColumnWidths[i],
                                              client.Width(), m_ratioListWidth));
        if (abs(list->GetColumnWidth(i) - expected) > 2) {
            CaptureColumnRatio();
            return;
        }
    }
}

void CStructBar::ApplyColumnRatio(int newListWidth) {
    if (newListWidth <= 0) return;
    CListCtrl* list = List();
    if (list == nullptr) return;

    if (!m_columnsInitialized) {
        const int first = max(24, ::MulDiv(newListWidth, 30, 100));
        const int second = max(24, ::MulDiv(newListWidth, 40, 100));
        const int third = max(24, newListWidth - first - second);
        m_applyingColumnRatio = true;
        list->SetColumnWidth(0, first);
        list->SetColumnWidth(1, second);
        list->SetColumnWidth(2, third);
        m_applyingColumnRatio = false;
        m_columnsInitialized = true;
        CaptureColumnRatio();
        return;
    }

    if (!m_keepColumnRatio) {
        // 比率保持OFFでは先頭2列の手動幅を維持し、値列で残幅を埋める。
        const int first = list->GetColumnWidth(0);
        const int second = list->GetColumnWidth(1);
        const int third = max(24, newListWidth - first - second);
        m_applyingColumnRatio = true;
        list->SetColumnWidth(2, third);
        m_applyingColumnRatio = false;
        return;
    }

    if (m_ratioListWidth <= 0) CaptureColumnRatio();
    if (m_ratioListWidth <= 0) return;
    m_applyingColumnRatio = true;
    const int first = max(24, ::MulDiv(m_ratioColumnWidths[0], newListWidth, m_ratioListWidth));
    const int second = max(24, ::MulDiv(m_ratioColumnWidths[1], newListWidth, m_ratioListWidth));
    const int third = max(24, newListWidth - first - second);
    list->SetColumnWidth(0, first);
    list->SetColumnWidth(1, second);
    list->SetColumnWidth(2, third);
    m_applyingColumnRatio = false;
}

void CStructBar::OnUpdateCmdUI(CFrameWnd* pTarget, BOOL bDisableIfNoHndler) {
    UNREFERENCED_PARAMETER(pTarget);
    UNREFERENCED_PARAMETER(bDisableIfNoHndler);
    // CDialogBar既定は、通知先メインフレームにハンドラがないボタンを自動無効化する。
    // 本バーは自身で通知処理するため、自身を対象にし自動無効化を止める。
    UpdateDialogControls(this, FALSE);
}

void CStructBar::LayoutChildren() {
    if (!m_layoutReady) return;
    CRect client;
    GetClientRect(&client);
    if (client.Width() <= 0 || client.Height() <= 0) return;

    const int dockPos = DockPosition();
    CaptureManualColumnRatioIfChanged();
    // DPI非対応のCDialogBarでは GetClientRect と画面上の可視範囲が異なる場合がある。
    // バーの画面矩形を高さ基準にし、横ドック時はフレーム可視範囲で幅を切る。
    CRect barWindow;
    GetWindowRect(&barWindow);
    if (barWindow.Height() > 0) {
        client.bottom = client.top + barWindow.Height();
    }
    if (dockPos != 2) {
        if (CWnd* frame = AfxGetMainWnd()) {
            CRect frameWindow;
            frame->GetWindowRect(&frameWindow);
            const int visibleWidth = min(barWindow.right, frameWindow.right) -
                                     max(barWindow.left, frameWindow.left);
            if (visibleWidth > 0) {
                client.right = client.left + visibleWidth;
            }
        }
    } else if (barWindow.Width() > 0) {
        client.right = client.left + barWindow.Width();
    }

    const int gap = 2;
    const int resizeGrip = 6;
    const int leftMargin = m_initialListRect.left;
    // 初期SysListView32はDPI仮想化によりテンプレートクライアントを越えることがあるため、
    // 右・下マージンを初期right/bottomから逆算せず、原RCと同じ四辺余白を使う。
    const int rightMargin = leftMargin;
    const int bottomMargin = leftMargin;
    m_layoutDockPosition = dockPos;
    const int topGrip = (dockPos == 0) ? resizeGrip : 0;
    const int bottomGrip = (dockPos == 1) ? resizeGrip : 0;
    const bool statusTop = (m_statusPos == 1);
    const bool statusBottom = (m_statusPos == 0);
    const bool statusVisible = statusTop || statusBottom;
    const int headerOffset = topGrip;

    // 原版どおり操作列は常に先頭。その直下へ「上」ステータスを置く。
    for (size_t i = 0; i + 1 < m_headerRects.size(); ++i) {
        if (CWnd* wnd = GetDlgItem(kHeaderControlIds[i])) {
            CRect rc = m_headerRects[i];
            rc.OffsetRect(0, headerOffset);
            wnd->MoveWindow(&rc);
        }
    }

    int listTop = m_initialListRect.top + headerOffset;
    int listBottom = client.bottom - bottomMargin - bottomGrip;
    int statusY = 0;
    if (statusTop) {
        statusY = listTop;
        listTop += m_statusHeight + gap;
    } else if (statusBottom) {
        statusY = listBottom - m_statusHeight;
        listBottom = statusY - gap;
    }

    CRect listRect(leftMargin, listTop,
                   max(leftMargin + 1, client.right - rightMargin),
                   max(listTop + 1, listBottom));

    if (CListCtrl* list = List()) {
        list->MoveWindow(&listRect);
        CRect listClient;
        list->GetClientRect(&listClient);
        const int newListWidth = listClient.Width();
        ApplyColumnRatio(newListWidth);
        m_lastLaidOutListWidth = newListWidth;
    }

    int statusX = leftMargin;
    for (size_t i = 0; i < m_statusWidths.size(); ++i) {
        if (CWnd* wnd = GetDlgItem(kStatusControlIds[i])) {
            if (statusVisible) {
                const int width = m_statusWidths[i];
                wnd->MoveWindow(statusX, statusY, width, m_statusHeight);
                wnd->ShowWindow(SW_SHOW);
                wnd->BringWindowToTop();
                statusX += width + gap;
            } else {
                wnd->ShowWindow(SW_HIDE);
            }
        }
    }

    if (dockPos != 2) {
        const int gripY = (dockPos == 1) ? max(0, client.bottom - resizeGrip) : 0;
        m_resizeGrip.MoveWindow(0, gripY, client.Width(), resizeGrip);
        m_resizeGrip.ShowWindow(SW_SHOW);
        m_resizeGrip.BringWindowToTop();
    } else {
        m_resizeGrip.ShowWindow(SW_HIDE);
    }
}

void CStructBar::UpdateStructStatus() {
    CStirlingView* view = ActiveView();
    CStirlingDoc* doc = (view != nullptr) ? view->GetDocument() : nullptr;
    CStringW editText;   // 一時 CStringW から生ポインタを取らない（C26815）
    const wchar_t* charsetText = L"";
    const wchar_t* orderText = L"";
    if (doc != nullptr) {
        if (!doc->CanEdit()) editText = ui::LoadW(IDS_INDICATOR_EDITLOCK_TEXT);
        static const wchar_t* kCharsets[] = {
            L"ASCII", L"SHIFT-JIS", L"EUC", L"Unicode", L"EBCDIC", L"EBCIDK",
        };
        const int charset = doc->GetCharset();
        if (charset >= 0 && charset < _countof(kCharsets)) {
            charsetText = kCharsets[charset];
        }
        orderText = doc->IsByteOrderBig() ? L"BigEndian" : L"LittleEndian";
    }
    ::SetDlgItemTextW(GetSafeHwnd(), IDC_STRUCT_STATUS_EDIT, editText);
    ::SetDlgItemTextW(GetSafeHwnd(), IDC_STRUCT_STATUS_CS, charsetText);
    ::SetDlgItemTextW(GetSafeHwnd(), IDC_STRUCT_STATUS_ORDER, orderText);
}

void CStructBar::OnSize(UINT nType, int cx, int cy) {
    CDialogBar::OnSize(nType, cx, cy);
    if (!m_resizingDockedHeight && DockPosition() != 2 &&
        m_dockedHeight <= 0 && cy > 0) {
        m_dockedHeight = cy;
    }
    LayoutChildren();
    QueueDeferredLayout();
}

void CStructBar::OnShowWindow(BOOL bShow, UINT nStatus) {
    CDialogBar::OnShowWindow(bShow, nStatus);
    if (!bShow) {
        ClearViewHighlight();
        CancelEdit();
    }
}

void CStructBar::OnWindowPosChanged(WINDOWPOS* lpwndpos) {
    CDialogBar::OnWindowPosChanged(lpwndpos);
    LayoutChildren();
    QueueDeferredLayout();
}

int CStructBar::DockPosition() const {
    if (IsFloating()) return 2;
    if ((m_dwStyle & CBRS_ALIGN_BOTTOM) != 0) return 0;
    if ((m_dwStyle & CBRS_ALIGN_TOP) != 0) return 1;
    return 2;
}

bool CStructBar::IsResizeGrip(CPoint point) const {
    const int dockPos = DockPosition();
    if (dockPos == 2) return false;
    if (m_resizeGrip.GetSafeHwnd() != nullptr) {
        CPoint screenPoint = point;
        const_cast<CStructBar*>(this)->ClientToScreen(&screenPoint);
        if (::WindowFromPoint(screenPoint) == m_resizeGrip.GetSafeHwnd()) return true;
    }
    CRect client;
    GetClientRect(&client);
    const int grip = 6;
    return (dockPos == 1) ? (point.y >= client.bottom - grip)
                          : (point.y <= client.top + grip);
}

void CStructBar::BeginDockedResize() {
    if (m_layoutDockPosition == 2) return;
    m_resizeDockPosition = m_layoutDockPosition;
    if (m_dockedHeight <= 0) {
        CRect current;
        GetClientRect(&current);
        m_dockedHeight = max(100, current.Height());
    }
    CPoint screenPoint;
    ::GetCursorPos(&screenPoint);
    m_resizingDockedHeight = true;
    m_resizeStartY = screenPoint.y;
    m_resizeStartHeight = m_dockedHeight;
    SetCapture();
}

BOOL CStructBar::OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message) {
    CPoint point;
    ::GetCursorPos(&point);
    ScreenToClient(&point);
    if (IsResizeGrip(point)) {
        ::SetCursor(AfxGetApp()->LoadStandardCursor(IDC_SIZENS));
        return TRUE;
    }
    return CDialogBar::OnSetCursor(pWnd, nHitTest, message);
}

void CStructBar::OnLButtonDown(UINT nFlags, CPoint point) {
    if (!IsResizeGrip(point)) {
        CDialogBar::OnLButtonDown(nFlags, point);
        return;
    }
    BeginDockedResize();
}

void CStructBar::OnMouseMove(UINT nFlags, CPoint point) {
    if (!m_resizingDockedHeight) {
        CDialogBar::OnMouseMove(nFlags, point);
        return;
    }
    CPoint screenPoint;
    ::GetCursorPos(&screenPoint);
    const int delta = screenPoint.y - m_resizeStartY;
    int newHeight = m_resizeStartHeight + ((m_resizeDockPosition == 1) ? delta : -delta);
    // 高DPI非対応プロセスではフレームの GetClientRect が仮想化され、バー自身の
    // 高さと異なる座標系になる。上限は固定の安全値とし、実際の収まりはMFCへ任せる。
    newHeight = min(2000, max(100, newHeight));
    if (newHeight != m_dockedHeight) {
        m_dockedHeight = newHeight;
        if (CFrameWnd* frame = DYNAMIC_DOWNCAST(CFrameWnd, AfxGetMainWnd())) {
            frame->RecalcLayout();
        }
        LayoutChildren();
    }
}

void CStructBar::OnLButtonUp(UINT nFlags, CPoint point) {
    if (m_resizingDockedHeight) {
        m_resizingDockedHeight = false;
        m_resizeDockPosition = -1;
        if (::GetCapture() == GetSafeHwnd()) ReleaseCapture();
        return;
    }
    CDialogBar::OnLButtonUp(nFlags, point);
}

void CStructBar::OnCaptureChanged(CWnd* pWnd) {
    m_resizingDockedHeight = false;
    m_resizeDockPosition = -1;
    CDialogBar::OnCaptureChanged(pWnd);
}



void CStructBar::SetupList() {
    CListCtrl* list = List();
    if (list == nullptr) return;
    list->SetExtendedStyle((list->GetExtendedStyle() | LVS_EX_FULLROWSELECT) & ~LVS_EX_GRIDLINES);
    // 空イメージリストの高さで、原DDS2グリッドに近い18px前後の行高へ揃える。
    if (m_rowHeightImages.Create(1, 16, ILC_COLOR32 | ILC_MASK, 1, 1)) {
        list->SetImageList(&m_rowHeightImages, LVSIL_SMALL);
    }
    // 原の列順・名称: 型 | シンボル名 | 値。日本語見出しは CP932 化回避でワイド挿入。
    const CStringW colTexts[3] = {
        ui::LoadW(IDS_STRUCT_COL_TYPE), ui::LoadW(IDS_STRUCT_COL_NAME), ui::LoadW(IDS_STRUCT_COL_VALUE),
    };
    struct { const wchar_t* text; int cx; } cols[] = {
        { colTexts[0], 70 }, { colTexts[1], 170 }, { colTexts[2], 150 },
    };
    for (int i = 0; i < 3; ++i) {
        LVCOLUMN col = { 0 };
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = const_cast<LPWSTR>(cols[i].text);
        col.cx = cols[i].cx;
        col.iSubItem = i;
        list->InsertColumn(i, &col);
    }
}

CStringW CStructBar::DefPath() {
    // exe ディレクトリの Struct.def（原 string res 6001 "Struct.def"）。
    // MAX_PATH 非依存で exe ディレクトリを得る（取得できない場合はカレント相対で扱う）。
    return ui::ModuleDirectory() + L"Struct.def";
}

void CStructBar::ReloadDefs() {
    std::wstring err;
    const CStringW path = DefPath();
    // 原ヘルプ「構造体テンプレートの書式に誤りがある場合には…読み込み時にエラーメッセージが
    //   表示されます」に従い、読み込みの失敗は利用者へ通知する（定義は読めた分だけ残る）。
    //   書式エラーだけでなくオープン失敗（共有違反・アクセス拒否等）もここに載るため、
    //   表題は原因を限定せず、具体的な理由は err の本文で示す。
    //   Struct.def を置いていない環境では通知しない（構造体編集を使わない利用者には無用）。
    if (!m_defs.ParseFile(path.GetString(), &err) && !err.empty() &&
        ::GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
        CStringW msg;
        msg.Format(ui::LoadW(IDS_STRUCT_DEF_PARSE_ERROR), err.c_str());
        ui::MsgBox(GetSafeHwnd(), msg, MB_OK | MB_ICONEXCLAMATION);
    }
    PopulateCombo();
    Refresh(true);
}

void CStructBar::PopulateCombo() {
    CComboBox* combo = Combo();
    if (combo == nullptr) return;
    const int prevSel = combo->GetCurSel();
    combo->ResetContent();
    const auto& defs = m_defs.Defs();
    for (size_t i = 0; i < defs.size(); ++i) {
        // 構造体名は ASCII 層。StructDef のレキサが識別子を [A-Za-z0-9_] に限定して
        //   おり（core/StructDef.cpp の Lexer::IsWord）、非 ASCII の名前はパース時点で
        //   弾かれる。したがってここは ACP 非依存で、変換ヘルパを通す必要がない。
        const int idx = combo->AddString(CString(defs[i].name.c_str()));
        if (idx >= 0) combo->SetItemData(idx, static_cast<DWORD_PTR>(i));
    }
    // 初期値は未選択（原挙動）。以前の選択があれば復元（再読込/再表示で保持）。
    //   モードレスのため閉じ→開き直しでは選択が自然に保持される（同一ウィンドウ）。
    if (prevSel >= 0 && prevSel < static_cast<int>(defs.size())) {
        combo->SetCurSel(prevSel);
    }
    combo->SetDroppedWidth(200);   // コンボ幅が狭いのでドロップダウンは広げる
}

CStirlingView* CStructBar::ActiveView() {
    CWnd* main = AfxGetMainWnd();
    if (main == nullptr) return nullptr;
    CMDIChildWnd* child = static_cast<CMDIFrameWnd*>(main)->MDIGetActive();
    if (child == nullptr) return nullptr;
    CView* view = child->GetActiveView();
    if (view != nullptr && view->IsKindOf(RUNTIME_CLASS(CStirlingView))) {
        return static_cast<CStirlingView*>(view);
    }
    return nullptr;
}

void CStructBar::Refresh(bool force) {
    CComboBox* combo = Combo();
    CListCtrl* list = List();
    if (combo == nullptr || list == nullptr) return;
    if (m_editRow >= 0) return;   // 編集中はリスト再構築を抑止（確定/取消後に再開）

    CStirlingView* view = ActiveView();
    const int sel = combo->GetCurSel();
    const int defIndex = (sel >= 0) ? static_cast<int>(combo->GetItemData(sel)) : -1;

    // アクティブビューが切替わったら base をそのビューへ再同期。
    //   ON=キャレット / OFF=永続アドレス（原 FUN_0045d2c8 の分岐）。
    if (view != m_boundView) {
        m_boundView = view;
        m_base = SyncBaseFor(view);
        force = true;
    }

    // シグネチャで変化検出（base はキャレットに追従せずナビでのみ動く）
    int charset = -1, big = -1;
    long seq = -1;
    CStirlingDoc* doc = (view != nullptr) ? view->GetDocument() : nullptr;
    if (view != nullptr && doc != nullptr) {
        charset = doc->GetCharset();
        big = doc->IsByteOrderBig() ? 1 : 0;
        seq = doc->ChangeSeq();
    }
    if (!force && view == m_lastView && m_base == m_lastBase && charset == m_lastCharset &&
        big == m_lastBig && seq == m_lastSeq && sel == m_lastSel) {
        return;
    }
    m_lastView = view; m_lastBase = m_base; m_lastCharset = charset;
    m_lastBig = big; m_lastSeq = seq; m_lastSel = sel;

    UpdateAddrStatic();

    if (view == nullptr || doc == nullptr || defIndex < 0) {
        m_root = stirling::StructNode();
        RebuildVisible();
        ClearViewHighlight();   // 構造体未選択/ビュー無し → データビューの青装飾を解除
        return;
    }

    const int size = m_defs.SizeOfStruct(defIndex);
    std::vector<unsigned char> bytes = doc->ReadRange(m_base, size);
    // 型ごとの既定基数で構築してから、一括／個別の上書きを反映する（個別指定を
    //   型既定へ戻せるよう、既定基数を StructNode::radix に残しておく必要がある）。
    m_defs.BuildTree(defIndex, bytes, big != 0, charset, m_root, -1);
    ApplyRadixOverrides(bytes, big != 0, m_root.children, std::string());
    RebuildVisible();

    // データビューの16進欄に構造体表示範囲[base, base+size-1]を青装飾（原挙動）。
    if (m_hiliteView != nullptr && m_hiliteView != view) {
        m_hiliteView->ClearStructHighlight();   // 旧ビューの装飾を解除
    }
    m_hiliteView = view;
    if (size > 0) {
        view->SetStructHighlight(m_base, m_base + size - 1);
    } else {
        view->ClearStructHighlight();
    }
}

// データビューの構造体範囲ハイライトを解除（バー非表示化時に呼ぶ）。
void CStructBar::ClearViewHighlight() {
    if (m_hiliteView != nullptr) {
        m_hiliteView->ClearStructHighlight();
        m_hiliteView = nullptr;
    }
}

// ビュー破棄時の参照無効化（v は破棄途中のためデリファレンスしない）。
void CStructBar::NotifyViewDestroyed(CStirlingView* view) {
    if (m_hiliteView == view) m_hiliteView = nullptr;
    if (m_boundView == view) m_boundView = nullptr;
    if (m_lastView == view) m_lastView = nullptr;
    m_savedBase.erase(view);   // 永続アドレスの残存キーを破棄（解放後アクセス防止）
}

// 同期点での base を決定する（原 FUN_0045d2c8）。
//   curPosToStructAddr ON=キャレット位置 / OFF=そのビューの永続アドレス。
stirling::FileOffset CStructBar::SyncBaseFor(CStirlingView* view) const {
    if (view == nullptr) return 0;
    if (theApp.AppSettings().curPosToStructAddr) {
        return view->CurrentPos();
    }
    auto it = m_savedBase.find(view);
    return (it != m_savedBase.end()) ? it->second : 0;
}

// ユーザ操作で確定した base を現ビューの永続アドレスへ保存する（原 view+0x304 更新）。
void CStructBar::RememberBase(CStirlingView* view) {
    if (view != nullptr) m_savedBase[view] = m_base;
}

// 表示化時: アクティブビューへ束縛し base を同期（原 FUN_00428720）。
//   ON=キャレット / OFF=永続アドレス（原 FUN_0045d2c8 の分岐）。
void CStructBar::SyncToCaret() {
    CStirlingView* view = ActiveView();
    m_boundView = view;
    m_base = SyncBaseFor(view);
    Refresh(true);
}

int CStructBar::CurStructSize() {
    CComboBox* combo = Combo();
    if (combo == nullptr) return 0;
    const int sel = combo->GetCurSel();
    if (sel < 0) return 0;
    return m_defs.SizeOfStruct(static_cast<int>(combo->GetItemData(sel)));
}

stirling::FileOffset CStructBar::MaxBase(stirling::FileOffset total, int structSize) const {
    return (total > structSize) ? (total - structSize) : 0;
}

// base をクランプして設定し再表示。境界で動けない場合はビープ（原 FUN_004840fb）。
void CStructBar::MoveBase(stirling::FileOffset newBase) {
    CStirlingView* view = ActiveView();
    if (view == nullptr) { ::MessageBeep(MB_ICONASTERISK); return; }
    const stirling::FileOffset total = view->TotalBytes();
    const int structSize = CurStructSize();
    const stirling::FileOffset maxBase = MaxBase(total, structSize);
    if (newBase < 0) newBase = 0;
    if (newBase > maxBase) newBase = maxBase;
    if (newBase == m_base) { ::MessageBeep(MB_ICONASTERISK); return; }
    m_base = newBase;
    RememberBase(view);   // OFF 時にビュー切替を跨いで保持（原 view+0x304）
    Refresh(true);
}

void CStructBar::UpdateAddrStatic() {
    CString s;
    s.Format(_T("%08llX"), static_cast<long long>(m_base));
    SetDlgItemText(IDC_STRUCT_ADDR, s);
}

// m_root＋展開状態から可視行 m_rows を平坦化し、仮想リストへ反映する。
void CStructBar::RebuildVisible() {
    m_rows.clear();
    Flatten(m_root.children, 0, std::string());
    CListCtrl* list = List();
    if (list != nullptr) {
        list->SetItemCountEx(static_cast<int>(m_rows.size()), LVSICF_NOSCROLL);
        list->Invalidate();
    }
}

void CStructBar::Flatten(const std::vector<stirling::StructNode>& nodes, int depth,
                         const std::string& parentPath) {
    for (const auto& n : nodes) {
        const std::string path = parentPath + "/" + n.name;
        const bool exp = n.hasChildren && (m_expanded.find(path) != m_expanded.end());
        DispRow r;
        r.type = n.type; r.name = n.name; r.value = n.value; r.path = path;
        r.depth = depth; r.hasChildren = n.hasChildren; r.expanded = exp;
        r.editable = n.editable; r.offset = n.offset; r.size = n.size; r.kind = n.kind;
        m_rows.push_back(std::move(r));
        if (exp) Flatten(n.children, depth + 1, path);
    }
}

void CStructBar::Toggle(const std::string& path) {
    if (m_expanded.find(path) != m_expanded.end()) m_expanded.erase(path);
    else m_expanded.insert(path);
    RebuildVisible();
}

void CStructBar::SelectListRow(int row) {
    CListCtrl* list = List();
    if (list == nullptr || row < 0 || row >= static_cast<int>(m_rows.size())) return;
    list->SetItemState(-1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    list->SetItemState(row, LVIS_SELECTED | LVIS_FOCUSED,
                       LVIS_SELECTED | LVIS_FOCUSED);
    list->SetSelectionMark(row);
    list->EnsureVisible(row, FALSE);
}

void CStructBar::OnReload() { ReloadDefs(); }

void CStructBar::OnSelChange() {
    m_radixByPath.clear();   // 別ツリーへ切替＝個別基数指定は失われる（原実測）
    Refresh(true);
}

// ナビ（原 FUN_0040ca57/cb60/cc3e/cd45/ce0a）。< > は±1バイト、<< >> は±構造体サイズ。
void CStructBar::OnNavPrevRec()  { MoveBase(m_base - CurStructSize()); }  // "<<"
void CStructBar::OnNavPrevByte() { MoveBase(m_base - 1); }                // "<"
void CStructBar::OnNavNextByte() { MoveBase(m_base + 1); }                // ">"
void CStructBar::OnNavNextRec()  { MoveBase(m_base + CurStructSize()); }  // ">>"

// "移動": 原 IDD_TOP_ADDRESS(196) でアドレス／マーク位置を指定（原 FUN_0040ce0a）。
void CStructBar::OnNavGoto() {
    if (m_editRow >= 0) CommitEdit();
    CStirlingView* view = ActiveView();
    if (view == nullptr) { ::MessageBeep(MB_ICONASTERISK); return; }
    CStirlingDoc* doc = view->GetDocument();
    CComboBox* combo = Combo();
    if (doc == nullptr || combo == nullptr || combo->GetCurSel() < 0) {
        ::MessageBeep(MB_ICONASTERISK);
        return;
    }
    const stirling::FileOffset total = view->TotalBytes();
    const stirling::FileOffset maxBase = MaxBase(total, CurStructSize());
    CStructAddressDlg dlg(this, doc, view->CurrentPos(), total);
    if (dlg.DoModal() != IDOK) return;
    const stirling::FileOffset newBase = dlg.ResultAddr();
    if (newBase < 0 || newBase > maxBase) {
        ::MessageBeep(MB_ICONASTERISK);   // 構造体全体が文書範囲に収まらない
        return;
    }
    m_base = newBase;
    RememberBase(view);
    Refresh(true);
}

void CStructBar::OnCloseButton() {
    ClearViewHighlight();
    if (CFrameWnd* frame = DYNAMIC_DOWNCAST(CFrameWnd, AfxGetMainWnd())) {
        frame->ShowControlBar(this, FALSE, FALSE);   // 破棄せず隠し、ドック領域も再計算
    } else {
        ShowWindow(SW_HIDE);
    }
}

void CStructBar::OnTimer(UINT_PTR nIDEvent) {
    if (nIDEvent == kRefreshTimer) {
        if (IsWindowVisible()) {
            UpdateStructStatus();
            Refresh(false);
        }
        return;
    }
    CDialogBar::OnTimer(nIDEvent);
}

void CStructBar::OnGetDispInfo(NMHDR* pNMHDR, LRESULT* pResult) {
    LV_DISPINFO* di = reinterpret_cast<LV_DISPINFO*>(pNMHDR);
    *pResult = 0;
    const int i = di->item.iItem;
    if (i < 0 || i >= static_cast<int>(m_rows.size())) return;
    if ((di->item.mask & LVIF_TEXT) == 0) return;
    const DispRow& r = m_rows[i];
    if (di->item.pszText == nullptr || di->item.cchTextMax <= 0) { return; }
    // [byte層境界] 型/名前は struct.def 由来の CP932 バイト列なので、ここでワイドへ変換する。
    //   値はワイドで持っている（UTF-8 の char 配列が CP932 に無い文字を含むため。Issue #107）。
    //   詳細: analysis_artifacts/docs/20_unicode_layering.md §6.2
    std::wstring w;
    switch (di->item.iSubItem) {
    case 0: w = stirling::WideFromCp932(r.type.c_str(), static_cast<int>(r.type.size())); break;
    case 1: w = stirling::WideFromCp932(r.name.c_str(), static_cast<int>(r.name.size())); break;
    case 2: w = r.value; break;   // 値（描画は OnCustomDraw が担当）
    default: return;
    }
    ::StringCchCopyW(di->item.pszText, static_cast<size_t>(di->item.cchTextMax), w.c_str());
}

// 原DDS2CustomCtrl相当の3列描画。データ行だけに罫線を描き、一覧下部は白地のまま残す。
void CStructBar::OnCustomDraw(NMHDR* pNMHDR, LRESULT* pResult) {
    NMLVCUSTOMDRAW* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(pNMHDR);
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        *pResult = CDRF_NOTIFYITEMDRAW;
        return;
    case CDDS_ITEMPREPAINT:
        *pResult = CDRF_NOTIFYSUBITEMDRAW;
        return;
    case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
        break;
    default:
        *pResult = CDRF_DODEFAULT;
        return;
    }

    CListCtrl* list = List();
    const int item = static_cast<int>(cd->nmcd.dwItemSpec);
    const int subItem = cd->iSubItem;
    if (list == nullptr || item < 0 || item >= static_cast<int>(m_rows.size()) ||
        subItem < 0 || subItem > 2) {
        *pResult = CDRF_DODEFAULT;
        return;
    }
    const DispRow& r = m_rows[item];
    HDC hdc = cd->nmcd.hdc;
    CRect rc;
    if (subItem == 0) {
        list->GetItemRect(item, rc, LVIR_BOUNDS);
        rc.right = rc.left + list->GetColumnWidth(0);
    } else {
        list->GetSubItemRect(item, subItem, LVIR_BOUNDS, rc);
    }

    const bool selected = (list->GetItemState(item, LVIS_SELECTED) & LVIS_SELECTED) != 0;
    const bool focused = (::GetFocus() == list->GetSafeHwnd());
    const COLORREF bg = selected ? ::GetSysColor(focused ? COLOR_HIGHLIGHT : COLOR_BTNFACE)
                                  : ::GetSysColor(COLOR_WINDOW);
    const COLORREF fg = selected ? ::GetSysColor(focused ? COLOR_HIGHLIGHTTEXT : COLOR_BTNTEXT)
                                  : ::GetSysColor(COLOR_WINDOWTEXT);
    const int saved = ::SaveDC(hdc);
    { CBrush b(bg); ::FillRect(hdc, &rc, static_cast<HBRUSH>(b.GetSafeHandle())); }

    CRect textRc = rc;
    textRc.left += 4;
    textRc.right -= 3;
    const std::string* text = &r.type;   // [byte層] 型/名前は CP932
    if (subItem == 1) {
        text = &r.name;
        const int indent = r.depth * kIndentPx;
        if (r.hasChildren) {
            const int bx = rc.left + indent + 2;
            const int by = rc.top + (rc.Height() - kGlyphPx) / 2;
            const stirling::ScopedGdiObject borderPen(
                ::CreatePen(PS_SOLID, 1, ::GetSysColor(COLOR_GRAYTEXT)));
            const stirling::ScopedGdiObject glyphPen(
                ::CreatePen(PS_SOLID, 1, ::GetSysColor(COLOR_WINDOWTEXT)));
            // ストックブラシは所有しない（DeleteObject 不要）。選択のみ復帰させる。
            const stirling::ScopedSelectHdc selBrush(hdc, ::GetStockObject(WHITE_BRUSH));
            {
                const stirling::ScopedSelectHdc selBorder(hdc, borderPen.Get());
                ::Rectangle(hdc, bx, by, bx + kGlyphPx, by + kGlyphPx);
            }
            {
                const stirling::ScopedSelectHdc selGlyph(hdc, glyphPen.Get());
                ::MoveToEx(hdc, bx + 2, by + kGlyphPx / 2, nullptr);
                ::LineTo(hdc, bx + kGlyphPx - 2, by + kGlyphPx / 2);
                if (!r.expanded) {
                    ::MoveToEx(hdc, bx + kGlyphPx / 2, by + 2, nullptr);
                    ::LineTo(hdc, bx + kGlyphPx / 2, by + kGlyphPx - 2);
                }
            }
        }
        textRc.left = rc.left + indent + kIndentPx;
    }

    ::SetBkMode(hdc, TRANSPARENT);
    ::SetTextColor(hdc, fg);
    // [byte層境界] 型/名前は CP932 バイト列なのでワイドへ変換して描く。値は既にワイド
    //   （UTF-8 の char 配列が CP932 に無い文字を含むため。Issue #107）。
    const std::wstring drawW =
        (subItem == 2) ? r.value
                       : stirling::WideFromCp932(text->c_str(), static_cast<int>(text->size()));
    ::DrawTextW(hdc, drawW.c_str(), -1, &textRc,
                DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);

    // データ行のセル境界。LVS_EX_GRIDLINESを使わないため空白行には線が残らない。
    {
        const stirling::ScopedGdiObject gridPen(
            ::CreatePen(PS_SOLID, 1, ::GetSysColor(COLOR_3DLIGHT)));
        const stirling::ScopedSelectHdc selGrid(hdc, gridPen.Get());
        ::MoveToEx(hdc, rc.right - 1, rc.top, nullptr);
        ::LineTo(hdc, rc.right - 1, rc.bottom);
        ::MoveToEx(hdc, rc.left, rc.bottom - 1, nullptr);
        ::LineTo(hdc, rc.right, rc.bottom - 1);
    }

    ::RestoreDC(hdc, saved);
    *pResult = CDRF_SKIPDEFAULT;
}

// [+]/[-] ボックス上のクリックで展開/折り畳み。
void CStructBar::OnClickList(NMHDR* pNMHDR, LRESULT* pResult) {
    NMITEMACTIVATE* ia = reinterpret_cast<NMITEMACTIVATE*>(pNMHDR);
    *pResult = 0;
    const int item = ia->iItem;
    if (item < 0 || item >= static_cast<int>(m_rows.size())) return;
    const DispRow& r = m_rows[item];
    if (!r.hasChildren) return;
    CListCtrl* list = List();
    CRect rc;
    list->GetSubItemRect(item, 1, LVIR_BOUNDS, rc);
    const int bx = rc.left + r.depth * kIndentPx + 2;
    if (ia->ptAction.x >= bx && ia->ptAction.x < bx + kGlyphPx) {
        Toggle(r.path);
    }
}

// BuildTree（型ごとの既定基数）の結果へ、一括／個別の基数上書きを反映する。
//   優先順は 個別指定 > 一括指定 > 型ごとの既定。個別指定の値 -1 は「その型の既定へ戻す」で、
//   一括指定より強い（原の「個別基数指定＞デフォルトに戻す」に一致）。
//   float/double は基数に依らず浮動小数表示のため対象外（原 FormatScalarValue も基数を無視）。
void CStructBar::ApplyRadixOverrides(const std::vector<unsigned char>& bytes, bool big,
                                     std::vector<stirling::StructNode>& nodes,
                                     const std::string& parentPath) {
    for (auto& n : nodes) {
        const std::string path = parentPath + "/" + n.name;   // Flatten と同一のキー
        if (n.hasChildren) {
            ApplyRadixOverrides(bytes, big, n.children, path);
            continue;
        }
        if (!n.editable || n.offset < 0 || n.size <= 0) continue;   // 範囲外（"----"）は対象外
        if (n.kind == stirling::FieldKind::Float || n.kind == stirling::FieldKind::Double) continue;
        if (n.offset + n.size > static_cast<int>(bytes.size())) continue;

        int radix = n.radix;   // BuildTree(-1) の結果＝型ごとの既定
        const auto it = m_radixByPath.find(path);
        if (it != m_radixByPath.end()) {
            if (it->second >= 0) radix = it->second;   // 個別指定（-1 は型既定のまま）
        } else if (m_radixOverride >= 0) {
            radix = m_radixOverride;                   // 一括指定
        }
        if (radix == n.radix) continue;
        n.value = stirling::FormatScalarValueW(n.kind, n.size, &bytes[n.offset], big, radix);
        n.radix = radix;
    }
}

// 右クリック: 構造体編集ポップアップ（原メニュー資源 189 = IDR_STRUCT_POPUP）。
//   原挙動: クリックした行が選択され、メニューはその行に効く。第1項目はスカラ葉なら
//   「編集(&E)」、コンテナなら「開く(&E)」／展開済みなら「閉じる(&C)」に差し替わり、
//   「個別基数指定」はコンテナ行でグレーアウトする。基数コマンドはこのメニュー内でのみ
//   処理される（原はフレーム等へ投函された WM_COMMAND には反応しない＝実測）。
void CStructBar::OnRClickList(NMHDR* pNMHDR, LRESULT* pResult) {
    NMITEMACTIVATE* ia = reinterpret_cast<NMITEMACTIVATE*>(pNMHDR);
    *pResult = 0;
    CancelEdit();   // 編集中なら破棄してからメニュー表示

    // クリックされた行を選択（行外のクリックは従来の選択を保つ＝原挙動）。
    if (ia != nullptr && ia->iItem >= 0 && ia->iItem < static_cast<int>(m_rows.size())) {
        SelectListRow(ia->iItem);
    }
    CListCtrl* list = List();
    const int row = (list != nullptr) ? list->GetNextItem(-1, LVNI_SELECTED) : -1;
    const DispRow* r = (row >= 0 && row < static_cast<int>(m_rows.size())) ? &m_rows[row] : nullptr;

    CMenu menu;
    if (!menu.LoadMenu(IDR_STRUCT_POPUP)) return;
    CMenu* pop = menu.GetSubMenu(0);
    if (pop == nullptr) return;

    // 第1項目のラベル（コンテナ行のみ差し替え。資源の既定は "編集(&E)"）。
    if (r != nullptr && r->hasChildren) {
        const CStringW label =
            ui::LoadW(r->expanded ? IDS_STRUCT_MENU_CLOSE : IDS_STRUCT_MENU_OPEN);
        pop->ModifyMenu(ID_STRUCT_EXEC, MF_BYCOMMAND | MF_STRING, ID_STRUCT_EXEC, label);
    }
    const bool leaf = (r != nullptr && !r->hasChildren);
    if (r == nullptr || (leaf && !r->editable)) {
        pop->EnableMenuItem(ID_STRUCT_EXEC, MF_BYCOMMAND | MF_GRAYED | MF_DISABLED);
    }
    // 「個別基数指定」（サブメニュー位置3）はスカラ葉のみ有効。
    const UINT perItem = (leaf && r->editable) ? MF_ENABLED : (MF_GRAYED | MF_DISABLED);
    pop->EnableMenuItem(3, MF_BYPOSITION | perItem);

    POINT pt; ::GetCursorPos(&pt);
    const UINT cmd = ::TrackPopupMenu(
        pop->GetSafeHmenu(), TPM_RETURNCMD | TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_NONOTIFY,
        pt.x, pt.y, 0, GetSafeHwnd(), nullptr);
    if (cmd == 0) return;

    switch (cmd) {
    case ID_STRUCT_EXEC:                       // 編集／開く／閉じる
        if (r == nullptr) break;
        if (r->hasChildren) Toggle(r->path);
        else if (r->editable) BeginEdit(row);
        break;
    // 一括指定は全項目への一律代入。個別指定はまとめて解除される（原実測）。
    case ID_STRUCT_RADIX_ALL_S:   SetRadixAll(stirling::kRadixSignedDec); break;
    case ID_STRUCT_RADIX_ALL_U:   SetRadixAll(stirling::kRadixDec1);      break;
    case ID_STRUCT_RADIX_ALL_H:   SetRadixAll(stirling::kRadixHex);       break;
    case ID_STRUCT_RADIX_ALL_DEF: SetRadixAll(-1);                        break;
    // 個別指定は選択行のみ。"デフォルトに戻す" は -1 を明示的に記録し一括指定より優先する。
    case ID_STRUCT_RADIX_ONE_S:   SetRadixItem(r, stirling::kRadixSignedDec); break;
    case ID_STRUCT_RADIX_ONE_U:   SetRadixItem(r, stirling::kRadixDec1);      break;
    case ID_STRUCT_RADIX_ONE_H:   SetRadixItem(r, stirling::kRadixHex);       break;
    case ID_STRUCT_RADIX_ONE_DEF: SetRadixItem(r, -1);                        break;
    default: break;
    }
}

// 「一括基数指定」: 全項目へ一律代入し、個別指定を解除する（原実測）。
void CStructBar::SetRadixAll(int radix) {
    m_radixOverride = radix;
    m_radixByPath.clear();
    Refresh(true);
}

// 「個別基数指定」: 選択中のスカラ葉のみ上書き（radix<0 = その型の既定へ戻す）。
void CStructBar::SetRadixItem(const DispRow* row, int radix) {
    if (row == nullptr || row->hasChildren || !row->editable) return;
    m_radixByPath[row->path] = radix;
    Refresh(true);
}

// 「キャレット位置を構造体編集」（原 0x8061）。基準アドレスをキャレット位置へ移動する。
void CStructBar::SetBaseToCaret() {
    if (m_editRow >= 0) CommitEdit();
    CStirlingView* view = ActiveView();
    if (view == nullptr) { ::MessageBeep(MB_ICONASTERISK); return; }
    const stirling::FileOffset newBase = view->CurrentPos();
    if (newBase > MaxBase(view->TotalBytes(), CurStructSize())) {
        ::MessageBeep(MB_ICONASTERISK);   // 構造体全体が文書範囲に収まらない
        return;
    }
    m_base = newBase;
    RememberBase(view);
    Refresh(true);
}

// 行ダブルクリック: コンテナ=展開/折り畳み、編集可の葉=値のインプレース編集。
void CStructBar::OnDblClkList(NMHDR* pNMHDR, LRESULT* pResult) {
    NMITEMACTIVATE* ia = reinterpret_cast<NMITEMACTIVATE*>(pNMHDR);
    *pResult = 0;
    const int item = ia->iItem;
    if (item < 0 || item >= static_cast<int>(m_rows.size())) return;
    const DispRow& r = m_rows[item];
    if (r.hasChildren) Toggle(r.path);
    else if (r.editable) BeginEdit(item);
}

// 標準ツリー相当の←/→親子移動、+/-展開、F2/Enter編集（フォーカス行）。
void CStructBar::OnKeyDownList(NMHDR* pNMHDR, LRESULT* pResult) {
    NMLVKEYDOWN* kd = reinterpret_cast<NMLVKEYDOWN*>(pNMHDR);
    *pResult = 0;
    CListCtrl* list = List();
    if (list == nullptr) return;
    const int item = list->GetNextItem(-1, LVNI_FOCUSED);
    if (item < 0 || item >= static_cast<int>(m_rows.size())) return;
    const DispRow& r = m_rows[item];
    if (!r.hasChildren) {
        if ((kd->wVKey == VK_F2 || kd->wVKey == VK_RETURN) && r.editable) BeginEdit(item);
        if (kd->wVKey == VK_LEFT && r.depth > 0) {
            for (int i = item - 1; i >= 0; --i) {
                if (m_rows[i].depth < r.depth) { SelectListRow(i); break; }
            }
        }
        return;
    }
    const bool isExpanded = (m_expanded.find(r.path) != m_expanded.end());
    switch (kd->wVKey) {
    case VK_RIGHT:
        if (!isExpanded) Toggle(r.path);
        else if (item + 1 < static_cast<int>(m_rows.size()) &&
                 m_rows[item + 1].depth > r.depth) SelectListRow(item + 1);
        break;
    case VK_LEFT:
        if (isExpanded) Toggle(r.path);
        else if (r.depth > 0) {
            for (int i = item - 1; i >= 0; --i) {
                if (m_rows[i].depth < r.depth) { SelectListRow(i); break; }
            }
        }
        break;
    case VK_ADD:
        if (!isExpanded) Toggle(r.path);
        break;
    case VK_SUBTRACT:
        if (isExpanded) Toggle(r.path);
        break;
    case VK_RETURN: case VK_SPACE:
        Toggle(r.path);
        break;
    default:
        break;
    }
}

// 値セルのインプレース編集を開始（編集可の葉のみ・doc が編集可のときのみ）。
void CStructBar::BeginEdit(int row) {
    if (row < 0 || row >= static_cast<int>(m_rows.size())) return;
    const DispRow& r = m_rows[row];
    if (!r.editable) return;
    CStirlingView* view = ActiveView();
    CStirlingDoc* doc = (view != nullptr) ? view->GetDocument() : nullptr;
    if (doc == nullptr || !doc->CanEdit()) { ::MessageBeep(MB_ICONASTERISK); return; }  // 編集禁止

    CListCtrl* list = List();
    if (list == nullptr) return;
    CRect rc;
    list->GetSubItemRect(row, 2, LVIR_BOUNDS, rc);   // 値列(subitem 2)
    list->ClientToScreen(&rc);
    ScreenToClient(&rc);                             // 編集ボックスはダイアログの子

    m_editRow = row;
    // 値はワイドで保持しているのでそのまま渡す（編集できるのはスカラ葉＝数値表記のみ）。
    m_editCtrl.SetWindowText(r.value.c_str());
    m_editCtrl.MoveWindow(&rc);
    m_editCtrl.ShowWindow(SW_SHOW);
    m_editCtrl.SetFocus();
    m_editCtrl.SetSel(0, -1);
}

// 編集確定: 値を型解釈しデータへ書き戻す（単一Undo単位）。
void CStructBar::CommitEdit() {
    if (m_editRow < 0 || m_committing) return;
    m_committing = true;
    const int row = m_editRow;
    m_editRow = -1;                     // 先にクリア（Refresh 抑止を解除）

    CString text;
    m_editCtrl.GetWindowText(text);
    m_editCtrl.ShowWindow(SW_HIDE);

    if (row < static_cast<int>(m_rows.size())) {
        const DispRow& r = m_rows[row];
        CStirlingView* view = ActiveView();
        CStirlingDoc* doc = (view != nullptr) ? view->GetDocument() : nullptr;
        if (doc != nullptr && doc->CanEdit() && r.editable) {
            const bool big = doc->IsByteOrderBig();
            std::vector<unsigned char> bytes;
            // [byte層境界] EncodeScalar は数値表記（ASCII）を解釈する。CP932 へ変換できない
            //   文字が入力された場合は空文字列となり、EncodeScalar が構文エラーで弾く。
            std::string entered;
            stirling::Cp932FromWide(text, entered, text.GetLength());
            if (stirling::EncodeScalar(r.kind, r.size, entered, big, bytes) &&
                static_cast<int>(bytes.size()) == r.size) {
                const stirling::FileOffset pos = m_base + r.offset;
                if (pos >= 0 && pos + r.size <= doc->GetTotalLength()) {
                    // 値が実際に変化した場合のみ書き戻す（無変更は Undo に積まない）。
                    const std::vector<unsigned char> cur = doc->ReadRange(pos, r.size);
                    if (cur != bytes) {
                        doc->ReplaceRange(pos, r.size, bytes);   // 同サイズ置換＝単一Undo単位
                        doc->UpdateAllViews(nullptr);            // データビュー（値/青装飾）を再描画
                    }
                } else {
                    ::MessageBeep(MB_ICONASTERISK);
                }
            } else {
                ::MessageBeep(MB_ICONASTERISK);   // 構文/範囲エラー
            }
        }
    }
    m_committing = false;
    Refresh(true);   // グリッド再構築（新しい値を反映）
}

void CStructBar::CancelEdit() {
    if (m_editRow < 0) return;
    m_editRow = -1;
    m_editCtrl.ShowWindow(SW_HIDE);
    if (CListCtrl* list = List()) list->SetFocus();
}

// 編集ボックスのフォーカス喪失で確定（他所クリック/ナビ操作等）。
void CStructBar::OnEditKillFocus() {
    if (m_editRow >= 0 && !m_committing) CommitEdit();
}

// Enter=確定 / Esc=取消 を編集ボックスで捕捉（既定ボタン起動・ダイアログ閉じを防ぐ）。
BOOL CStructBar::PreTranslateMessage(MSG* pMsg) {
    if (m_editRow >= 0 && pMsg->message == WM_KEYDOWN &&
        pMsg->hwnd == m_editCtrl.GetSafeHwnd()) {
        if (pMsg->wParam == VK_RETURN) {
            CommitEdit();
            if (CListCtrl* l = List()) l->SetFocus();
            return TRUE;
        }
        if (pMsg->wParam == VK_ESCAPE) { CancelEdit(); return TRUE; }
    }
    return CDialogBar::PreTranslateMessage(pMsg);
}
