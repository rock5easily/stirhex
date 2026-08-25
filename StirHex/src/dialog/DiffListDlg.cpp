// CDiffListDlg 実装（原 IDD_DIFF_LIST=167, FUN_00409930/FUN_00409b02 系）。
#include "pch.h"
#include "app/UiStrings.h"   // UI文字列はリソースから
#include "dialog/DiffListDlg.h"
#include "view/StirlingView.h"

BEGIN_MESSAGE_MAP(CDiffListDlg, CDialog)
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
        }
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

void CDiffListDlg::Cleanup() {
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
