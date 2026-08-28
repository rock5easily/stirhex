// CDiffListDlg 実装（原 IDD_DIFF_LIST=167, FUN_00409930/FUN_00409b02 系）。
#include "pch.h"
#include "app/UiStrings.h"   // UI文字列はリソースから
#include "dialog/DiffListDlg.h"
#include "view/StirlingView.h"

#include <algorithm>

namespace {
constexpr UINT kDiffListProxyId = 0x7F84;
constexpr int kDiffListProxyWidth = 180;
constexpr UINT kDiffListProxyRestoreMessage = WM_APP + 0x140;
constexpr UINT kDiffListProxyCloseMessage = WM_APP + 0x141;
}

// 実ダイアログを隠している間だけMDICLIENT直下に置く復元用ウィンドウ。
// モードレスの実ダイアログを親子関係ごと切り替えるのではなく、proxyだけを
// WS_CHILDとして生成することで、通常時のpopupの移動範囲とZオーダーを維持する。
class CDiffListMinimizedProxy final : public CWnd {
public:
    explicit CDiffListMinimizedProxy(CDiffListDlg* owner)
        : m_owner(owner) {}

    BOOL Create(CWnd* pParent, const CString& title, const CRect& rect) {
        LPCTSTR cls = AfxRegisterWndClass(
            CS_DBLCLKS,
            ::LoadCursor(nullptr, IDC_ARROW),
            reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1),
            ::LoadIcon(AfxGetInstanceHandle(), MAKEINTRESOURCE(IDR_MAINFRAME)));
        if (cls == nullptr) { return FALSE; }
        return CWnd::CreateEx(
            0, cls, title,
            WS_CHILD | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPSIBLINGS,
            rect, pParent, static_cast<UINT>(kDiffListProxyId));
    }

    void Detach() { m_owner = nullptr; }

protected:
    afx_msg void OnDestroy();
    afx_msg void OnSysCommand(UINT nID, LPARAM lParam);
    afx_msg void OnNcLButtonDblClk(UINT nHitTest, CPoint point);
    afx_msg void OnLButtonDblClk(UINT nFlags, CPoint point);
    virtual void PostNcDestroy() override { delete this; }
    DECLARE_MESSAGE_MAP()

private:
    void PostOwnerMessage(UINT message) {
        CDiffListDlg* owner = m_owner;
        if (owner == nullptr) { return; }
        const HWND hDlg = owner->GetSafeHwnd();
        if (hDlg != nullptr && ::IsWindow(hDlg)) {
            ::PostMessage(hDlg, message, 0, 0);
        }
    }

    CDiffListDlg* m_owner;
};

BEGIN_MESSAGE_MAP(CDiffListMinimizedProxy, CWnd)
    ON_WM_DESTROY()
    ON_WM_SYSCOMMAND()
    ON_WM_NCLBUTTONDBLCLK()
    ON_WM_LBUTTONDBLCLK()
END_MESSAGE_MAP()

void CDiffListMinimizedProxy::OnDestroy() {
    CDiffListDlg* owner = m_owner;
    m_owner = nullptr;
    if (owner != nullptr) {
        owner->OnMinimizedProxyDestroyed(this);
    }
    CWnd::OnDestroy();
}

void CDiffListMinimizedProxy::OnSysCommand(UINT nID, LPARAM lParam) {
    const UINT command = nID & 0xFFF0U;
    if (m_owner != nullptr && command == SC_RESTORE) {
        PostOwnerMessage(kDiffListProxyRestoreMessage);
        return;
    }
    if (m_owner != nullptr && command == SC_CLOSE) {
        PostOwnerMessage(kDiffListProxyCloseMessage);
        return;
    }
    CWnd::OnSysCommand(nID, lParam);
}

void CDiffListMinimizedProxy::OnNcLButtonDblClk(UINT nHitTest, CPoint point) {
    if (m_owner != nullptr && nHitTest == HTCAPTION) {
        PostOwnerMessage(kDiffListProxyRestoreMessage);
        return;
    }
    CWnd::OnNcLButtonDblClk(nHitTest, point);
}

void CDiffListMinimizedProxy::OnLButtonDblClk(UINT nFlags, CPoint point) {
    if (m_owner != nullptr) {
        PostOwnerMessage(kDiffListProxyRestoreMessage);
        return;
    }
    CWnd::OnLButtonDblClk(nFlags, point);
}

BEGIN_MESSAGE_MAP(CDiffListDlg, CDialog)
    ON_WM_DESTROY()
    ON_WM_SYSCOMMAND()
    ON_MESSAGE(kDiffListProxyRestoreMessage, &CDiffListDlg::OnMinimizedProxyRestore)
    ON_MESSAGE(kDiffListProxyCloseMessage, &CDiffListDlg::OnMinimizedProxyClose)
    ON_BN_CLICKED(IDC_DIFFLIST_SWITCH, &CDiffListDlg::OnSwitch)
    ON_BN_CLICKED(IDC_DIFFLIST_HILITE, &CDiffListDlg::OnHilite)
    ON_BN_CLICKED(IDC_DIFFLIST_SYNC, &CDiffListDlg::OnSync)
    ON_NOTIFY(NM_DBLCLK, IDC_DIFFLIST_LIST, &CDiffListDlg::OnDblclkList)
END_MESSAGE_MAP()

CDiffListDlg::CDiffListDlg()
    : CDialog(IDD_DIFF_LIST)
    , m_view1(nullptr)
    , m_view2(nullptr)
    , m_active(nullptr)
    , m_hilite(TRUE)
    , m_sync(TRUE)
{
}

BOOL CDiffListDlg::CreateModeless(CStirlingView* view1, CStirlingView* view2,
                                  const std::vector<std::pair<stirling::FileOffset, stirling::FileOffset>>& diffs, CWnd* pParent) {
    m_view1  = view1;
    m_view2  = view2;
    m_active = view1;    // 既定の活性ビューは比較元（原 this+0x2b0 = param_2 = view1）
    m_diffs  = diffs;
    return Create(IDD_DIFF_LIST, pParent);
}

bool CDiffListDlg::CreateMinimizedProxy() {
    if (m_proxy != nullptr) {
        if (::IsWindow(m_proxy->GetSafeHwnd())) { return true; }
        m_proxy = nullptr;
    }

    CMDIFrameWnd* pMainFrame = DYNAMIC_DOWNCAST(CMDIFrameWnd, AfxGetMainWnd());
    const HWND hMdiClient = (pMainFrame != nullptr) ? pMainFrame->m_hWndMDIClient : nullptr;
    if (hMdiClient == nullptr || !::IsWindow(hMdiClient)) { return false; }

    RECT clientRect = {};
    if (!::GetClientRect(hMdiClient, &clientRect)) { return false; }
    const int clientWidth = (std::max)(0, static_cast<int>(clientRect.right - clientRect.left));
    const int width = (std::min)(kDiffListProxyWidth,
                                 (std::max)(96, clientWidth - 8));
    const int height = (std::max)(24, ::GetSystemMetrics(SM_CYMINIMIZED));
    const CRect initialRect(0, 0, width, height);

    CString title;
    GetWindowText(title);
    if (title.IsEmpty()) { return false; }

    auto* proxy = new CDiffListMinimizedProxy(this);
    if (!proxy->Create(CWnd::FromHandle(hMdiClient), title, initialRect)) {
        delete proxy;
        return false;
    }
    m_proxy = proxy;

    // WS_CHILDでもMDIのアイコン化と同じ標準的なcaptionを使う。先に表示してから
    // SW_MINIMIZEを適用し、ArrangeIconicWindowsで他のアイコン化子と並べる。
    proxy->ShowWindow(SW_SHOWNOACTIVATE);
    proxy->ShowWindow(SW_MINIMIZE);
    ::ArrangeIconicWindows(hMdiClient);
    KeepMinimizedProxyInsideMdi();
    return true;
}

void CDiffListDlg::DestroyMinimizedProxy() {
    CDiffListMinimizedProxy* proxy = m_proxy;
    m_proxy = nullptr;
    if (proxy == nullptr) { return; }

    // DestroyWindow()からPostNcDestroy()へ同期的にdelete thisされるため、
    // detachとHWND退避を済ませ、呼び出し後はproxyへアクセスしない。
    proxy->Detach();
    const HWND hProxy = proxy->GetSafeHwnd();
    if (hProxy != nullptr && ::IsWindow(hProxy)) {
        ::DestroyWindow(hProxy);
    }
}

void CDiffListDlg::RestoreFromMinimizedProxy() {
    if (m_destroying) { return; }

    CDiffListMinimizedProxy* proxy = m_proxy;
    m_proxy = nullptr;
    if (proxy != nullptr) {
        proxy->Detach();
        const HWND hProxy = proxy->GetSafeHwnd();
        if (hProxy != nullptr && ::IsWindow(hProxy)) {
            ::DestroyWindow(hProxy);
        }
    }
    m_minimized = false;

    const HWND hDlg = GetSafeHwnd();
    if (hDlg == nullptr || !::IsWindow(hDlg)) { return; }
    CRect rect;
    if (m_normalRectValid) {
        rect = m_normalRect;
    } else if (!::GetWindowRect(hDlg, &rect)) {
        return;
    }

    SetWindowPos(&wndTop, rect.left, rect.top, rect.Width(), rect.Height(),
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ShowWindow(SW_RESTORE);
    SetForegroundWindow();
    BringWindowToTop();
}

void CDiffListDlg::CloseFromMinimizedProxy() {
    if (m_destroying) { return; }
    Cleanup();
    const HWND hDlg = GetSafeHwnd();
    if (hDlg != nullptr && ::IsWindow(hDlg)) {
        // OnDestroy()でproxyもdetachして破棄する。ここを最後の処理にして、
        // PostNcDestroy()のdelete this後にメンバーへアクセスしない。
        ::DestroyWindow(hDlg);
    }
}

void CDiffListDlg::OnMinimizedProxyDestroyed(CDiffListMinimizedProxy* proxy) {
    if (m_proxy != proxy) { return; }
    m_proxy = nullptr;
    if (m_destroying || !m_minimized) { return; }

    // MDICLIENT側の破棄などでproxyだけが先に消えた場合も、実ダイアログを
    // 操作不能な非表示状態に残さない。
    m_minimized = false;
    const HWND hDlg = GetSafeHwnd();
    if (hDlg == nullptr || !::IsWindow(hDlg)) { return; }
    if (m_normalRectValid) {
        SetWindowPos(&wndTop, m_normalRect.left, m_normalRect.top,
                     m_normalRect.Width(), m_normalRect.Height(),
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
        ShowWindow(SW_SHOWNA);
    }
}

void CDiffListDlg::KeepMinimizedProxyInsideMdi() {
    if (m_proxy == nullptr) { return; }
    CMDIFrameWnd* pMainFrame = DYNAMIC_DOWNCAST(CMDIFrameWnd, AfxGetMainWnd());
    const HWND hMdiClient = (pMainFrame != nullptr) ? pMainFrame->m_hWndMDIClient : nullptr;
    const HWND hProxy = m_proxy->GetSafeHwnd();
    if (hMdiClient == nullptr || !::IsWindow(hMdiClient) ||
        hProxy == nullptr || !::IsWindow(hProxy)) {
        return;
    }

    RECT clientRect = {};
    RECT proxyRect = {};
    if (!::GetClientRect(hMdiClient, &clientRect) ||
        !::GetWindowRect(hProxy, &proxyRect)) {
        return;
    }
    POINT points[2] = {
        {proxyRect.left, proxyRect.top},
        {proxyRect.right, proxyRect.bottom},
    };
    ::MapWindowPoints(HWND_DESKTOP, hMdiClient, points, 2);
    const int width = (std::max)(1, static_cast<int>(points[1].x - points[0].x));
    const int height = (std::max)(1, static_cast<int>(points[1].y - points[0].y));
    const int clientLeft = static_cast<int>(clientRect.left);
    const int clientTop = static_cast<int>(clientRect.top);
    const int clientRight = static_cast<int>(clientRect.right);
    const int clientBottom = static_cast<int>(clientRect.bottom);
    const int maxLeft = (std::max)(clientLeft, clientRight - width);
    const int maxTop = (std::max)(clientTop, clientBottom - height);
    const int pointLeft = static_cast<int>(points[0].x);
    const int pointTop = static_cast<int>(points[0].y);
    const int left = (std::min)(maxLeft, (std::max)(clientLeft, pointLeft));
    const int top = (std::min)(maxTop, (std::max)(clientTop, pointTop));
    if (left != points[0].x || top != points[0].y) {
        m_proxy->SetWindowPos(nullptr, left, top, width, height,
                              SWP_NOACTIVATE | SWP_NOZORDER);
    }
}

void CDiffListDlg::OnSysCommand(UINT nID, LPARAM lParam) {
    if ((nID & 0xFFF0U) == SC_MINIMIZE && !m_minimized && !m_destroying) {
        CRect normalRect;
        if (::GetWindowRect(GetSafeHwnd(), &normalRect)) {
            // proxy生成に失敗した場合は既定の最小化に委ね、ダイアログを
            // 操作不能な非表示状態にしない。
            if (CreateMinimizedProxy()) {
                m_normalRect = normalRect;
                m_normalRectValid = true;
                m_minimized = true;
                ShowWindow(SW_HIDE);
                return;
            }
        }
    }
    CDialog::OnSysCommand(nID, lParam);
}

LRESULT CDiffListDlg::OnMinimizedProxyRestore(WPARAM /*wParam*/, LPARAM /*lParam*/) {
    RestoreFromMinimizedProxy();
    return 0;
}

LRESULT CDiffListDlg::OnMinimizedProxyClose(WPARAM /*wParam*/, LPARAM /*lParam*/) {
    CloseFromMinimizedProxy();
    return 0;
}

void CDiffListDlg::OnViewDestroyed(CStirlingView* view) {
    if (view != m_view1 && view != m_view2) { return; }
    CStirlingView* survivor = (view == m_view1) ? m_view2 : m_view1;
    const HWND hDlg = GetSafeHwnd();
    if (hDlg == nullptr || !::IsWindow(hDlg)) { return; }
    Cleanup();
    // DestroyWindow() は PostNcDestroy() で delete this まで同期実行するため、
    // 自己破棄後に this のメンバーへアクセスしない。残存ビューの活性化を先に行う。
    if (ViewAlive(survivor)) {
        if (CFrameWnd* pFrame = survivor->GetParentFrame()) {
            pFrame->ActivateFrame();
            pFrame->SetActiveView(survivor);
            if (CMDIFrameWnd* pMainFrame =
                    DYNAMIC_DOWNCAST(CMDIFrameWnd, AfxGetMainWnd())) {
                if (CMDIChildWnd* pChild = DYNAMIC_DOWNCAST(CMDIChildWnd, pFrame)) {
                    pMainFrame->MDIActivate(pChild);
                }
            }
        }
    }
    // HWND を退避済みなので、PostNcDestroy() で this が解放された後に
    // CWnd メンバーへ戻る必要がない。これを関数内の最後の処理にする。
    ::DestroyWindow(hDlg);
}

void CDiffListDlg::DoDataExchange(CDataExchange* pDX) {
    CDialog::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_DIFFLIST_LIST, m_list);
    DDX_Check(pDX, IDC_DIFFLIST_HILITE, m_hilite);
    DDX_Check(pDX, IDC_DIFFLIST_SYNC, m_sync);
}

bool CDiffListDlg::ViewAlive(CStirlingView* v) const {
    return v != nullptr && ::IsWindow(v->GetSafeHwnd());
}

BOOL CDiffListDlg::OnInitDialog() {
    CDialog::OnInitDialog();

    // レポート3カラム（原 相違箇所/相違終了箇所/相違サイズ, 各%08X。DAT_004b5544 群）。
    //   MBCS＋/utf-8 で日本語見出しが CP932 化するため、ワイド(LVM_INSERTCOLUMNW)で挿入する。
    m_list.SetExtendedStyle(m_list.GetExtendedStyle() | LVS_EX_FULLROWSELECT);
    auto insertColW = [this](int col, const wchar_t* text, int width) {
        LVCOLUMNW lc = {0};
        lc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        lc.fmt = LVCFMT_LEFT;
        lc.cx = width;
        lc.pszText = const_cast<wchar_t*>(text);
        m_list.SendMessage(LVM_INSERTCOLUMNW, col, reinterpret_cast<LPARAM>(&lc));
    };
    insertColW(0, ui::LoadW(IDS_DIFF_COL_START), 90);
    insertColW(1, ui::LoadW(IDS_DIFF_COL_END),   90);
    insertColW(2, ui::LoadW(IDS_DIFF_COL_SIZE),  90);

    for (int i = 0; i < static_cast<int>(m_diffs.size()); ++i) {
        const stirling::FileOffset start = m_diffs[i].first;
        const stirling::FileOffset end   = m_diffs[i].second;
        CString s;
        s.Format(_T("%08llX"), static_cast<long long>(start));
        m_list.InsertItem(i, s);
        // 1バイトの相違（end==start）は終了箇所を空欄に（原挙動）。
        if (end != start) {
            s.Format(_T("%08llX"), static_cast<long long>(end));
            m_list.SetItemText(i, 1, s);
        }
        s.Format(_T("%08llX"), static_cast<long long>(end - start + 1));
        m_list.SetItemText(i, 2, s);
    }
    if (!m_diffs.empty()) {
        m_list.SetItemState(0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }

    UpdateData(FALSE);   // チェック既定（強調ON/シンクロON）を反映
    ApplyHighlight();
    ApplySync();
    return TRUE;
}

void CDiffListDlg::JumpToSelected() {
    const int sel = m_list.GetNextItem(-1, LVNI_SELECTED);
    if (sel < 0 || sel >= static_cast<int>(m_diffs.size())) { return; }
    if (!ViewAlive(m_active)) { return; }
    m_active->GotoCompareDiff(m_diffs[sel].first, m_diffs[sel].second);   // 原 msg 0x410
}

void CDiffListDlg::OnOK() {
    JumpToSelected();   // ジャンプ（モードレスなので閉じない）
}

void CDiffListDlg::OnDblclkList(NMHDR* /*pNMHDR*/, LRESULT* pResult) {
    JumpToSelected();   // ダブルクリックでジャンプ（原 FUN_00409f13）
    *pResult = 0;
}

// 切替: 活性ビューを反転し、そのフレームを前面化（原 FUN_00409d73）。
void CDiffListDlg::OnSwitch() {
    m_active = (m_active == m_view1) ? m_view2 : m_view1;
    if (ViewAlive(m_active)) {
        if (CFrameWnd* pFrame = m_active->GetParentFrame()) {
            pFrame->ActivateFrame();
            pFrame->SetActiveView(m_active);
            if (CMDIFrameWnd* pMainFrame =
                    DYNAMIC_DOWNCAST(CMDIFrameWnd, AfxGetMainWnd())) {
                if (CMDIChildWnd* pChild = DYNAMIC_DOWNCAST(CMDIChildWnd, pFrame)) {
                    pMainFrame->MDIActivate(pChild);
                }
            }
        }
    }
    if (::IsWindow(GetSafeHwnd())) {
        // MDIActivate() may raise the document child above this utility window.
        // Restore only z-order; do not steal focus from the selected document.
        SetWindowPos(&wndTop, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

// 強調表示チェック → 両ビューの強調ON/OFF（原 FUN_00409f46: FUN_0045d228）。
void CDiffListDlg::OnHilite() {
    UpdateData(TRUE);
    ApplyHighlight();
}

void CDiffListDlg::ApplyHighlight() {
    if (ViewAlive(m_view1)) { m_view1->SetCompareHighlight(m_hilite != FALSE); }
    if (ViewAlive(m_view2)) { m_view2->SetCompareHighlight(m_hilite != FALSE); }
}

// シンクロチェック → 両ビューの同期相手を設定/解除。
void CDiffListDlg::OnSync() {
    UpdateData(TRUE);
    ApplySync();
}

void CDiffListDlg::ApplySync() {
    const bool on = (m_sync != FALSE) && ViewAlive(m_view1) && ViewAlive(m_view2);
    if (ViewAlive(m_view1)) { m_view1->SetSyncPartner(on ? m_view2 : nullptr, on); }
    if (ViewAlive(m_view2)) { m_view2->SetSyncPartner(on ? m_view1 : nullptr, on); }
}

// 閉じる（IDCANCEL）: 比較状態・同期を解除して破棄（原 FUN_00409e67）。
void CDiffListDlg::OnCancel() {
    Cleanup();
    DestroyWindow();
}

void CDiffListDlg::OnDestroy() {
    // ビュー終了・再比較・MDICLIENT破棄など、OnCancel を経由しない破棄でも比較状態を解除する。
    m_destroying = true;
    DestroyMinimizedProxy();
    m_minimized = false;
    Cleanup();
    CDialog::OnDestroy();
}

void CDiffListDlg::Cleanup() {
    if (m_cleaned) { return; }
    m_cleaned = true;
    if (ViewAlive(m_view1)) {
        m_view1->SetSyncPartner(nullptr, false);
        m_view1->ClearCompareResult();
        m_view1->OnDiffDlgClosed();     // 所有ビューの参照を切る
    }
    if (ViewAlive(m_view2)) {
        m_view2->SetSyncPartner(nullptr, false);
        m_view2->ClearCompareResult();
    }
}

void CDiffListDlg::PostNcDestroy() {
    CDialog::PostNcDestroy();
    delete this;   // モードレスは自己破棄
}
