// CMarkListDlg / CMarkListBox 実装（原 CMarkListDlg、IDD_MARK_LIST=140）。
#include "pch.h"
#include "dialog/MarkListDlg.h"
#include "dialog/MarkAddressDlg.h"
#include "dialog/DlgHexInput.h"      // dlg::LoadWStr
#include "view/StirlingView.h"
#include "doc/StirlingDoc.h"
#include "app/StirlingApp.h"
#include "util/ScopedGdi.h"   // GDI オブジェクトの RAII（Issue #48）

// ===========================================================================
// CMarkListBox — マーク色スウォッチ付きオーナードローリスト
// ===========================================================================
void CMarkListBox::SetColors(const COLORREF* fg, const COLORREF* bg, int count) {
    m_count = (count > 3) ? 3 : count;
    for (int i = 0; i < m_count; ++i) {
        m_fg[i] = fg[i];
        m_bg[i] = bg[i];
    }
}

void CMarkListBox::MeasureItem(LPMEASUREITEMSTRUCT lpMIS) {
    lpMIS->itemHeight = 15;   // 実高は OnInitDialog の SetItemHeight で確定（保険値）
}

void CMarkListBox::DrawItem(LPDRAWITEMSTRUCT lpDIS) {
    if (static_cast<int>(lpDIS->itemID) < 0) { return; }   // 空リスト
    CDC dc;
    dc.Attach(lpDIS->hDC);
    CRect rc = lpDIS->rcItem;
    const bool selected = (lpDIS->itemState & ODS_SELECTED) != 0;
    const bool enabled  = IsWindowEnabled() != FALSE;

    COLORREF back = selected ? ::GetSysColor(COLOR_HIGHLIGHT) : ::GetSysColor(COLOR_WINDOW);
    COLORREF textColor = selected ? ::GetSysColor(COLOR_HIGHLIGHTTEXT)
                                  : ::GetSysColor(COLOR_WINDOWTEXT);
    if (!enabled) { textColor = ::GetSysColor(COLOR_GRAYTEXT); }
    dc.FillSolidRect(&rc, back);
    dc.SetBkMode(TRANSPARENT);

    const int type = static_cast<int>(lpDIS->itemData);   // 0/1/2、末尾行=-1
    int textX = rc.left + 3;
    if (type >= 0 && type < m_count) {
        // マーク色スウォッチ: "0n" を種別背景色で塗り、種別文字色で描画（原 DrawItem）。
        CString sw;
        sw.Format(_T("%02d"), type + 1);
        const CSize ext = dc.GetTextExtent(sw);
        CRect box(rc.left + 2, rc.top + 1, rc.left + 2 + ext.cx + 4, rc.bottom - 1);
        dc.FillSolidRect(&box, m_bg[type]);
        dc.SetTextColor(m_fg[type]);
        CRect tb = box;
        dc.DrawText(sw, &tb, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        textX = box.right + 5;
    }

    dc.SetTextColor(textColor);
    CRect tr(textX, rc.top, rc.right, rc.bottom);
    if (type < 0) {
        // 新規登録行（リソース由来の文字列）。
        dc.DrawText(m_newEntry, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else {
        CString row;
        GetText(lpDIS->itemID, row);   // ASCII "%08X"
        dc.DrawText(row, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    if (lpDIS->itemState & ODS_FOCUS) { dc.DrawFocusRect(&rc); }
    dc.Detach();
}

// ===========================================================================
// CMarkListDlg
// ===========================================================================
BEGIN_MESSAGE_MAP(CMarkListDlg, CDialog)
    ON_BN_CLICKED(IDC_MARKLIST_REMOVE,   &CMarkListDlg::OnRemove)
    ON_BN_CLICKED(IDC_MARKLIST_CLEARALL, &CMarkListDlg::OnClearAll)
    ON_BN_CLICKED(IDC_MARKLIST_EDIT,     &CMarkListDlg::OnEditMark)
    ON_LBN_SELCHANGE(IDC_MARKLIST_LIST,  &CMarkListDlg::OnSelChange)
    ON_LBN_DBLCLK(IDC_MARKLIST_LIST,     &CMarkListDlg::OnDblClk)
END_MESSAGE_MAP()

CMarkListDlg::CMarkListDlg(CStirlingView* pView)
    : CDialog(IDD_MARK_LIST, pView)
    , m_pView(pView) {
}

void CMarkListDlg::DoDataExchange(CDataExchange* pDX) {
    CDialog::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_MARKLIST_LIST, m_list);
}

BOOL CMarkListDlg::OnInitDialog() {
    CDialog::OnInitDialog();

    CStirlingSettings& s = theApp.Settings();
    m_list.SetColors(s.markText, s.markBack, 3);
    m_list.SetNewEntryText(dlg::LoadWStr(IDS_MARK_NEW_ENTRY));

    // オーナードロー固定高をフォントから明示設定（WM_MEASUREITEM がサブクラス化前に届く問題を回避）。
    {
        CClientDC dc(&m_list);
        CFont* pFont = m_list.GetFont();
        TEXTMETRIC tm;
        {
            const stirling::ScopedSelectFont selFont(&dc, pFont);
            dc.GetTextMetrics(&tm);
        }
        m_list.SetItemHeight(0, tm.tmHeight + 2);
    }

    RebuildList();
    // 初期選択 = 末尾「新規登録」行（原 SetCurSel(markCount)）。
    const int cnt = m_list.GetCount();
    if (cnt > 0) { m_list.SetCurSel(cnt - 1); }
    UpdateButtons();
    return TRUE;
}

void CMarkListDlg::RebuildList() {
    m_positions.clear();
    m_list.ResetContent();
    CStirlingDoc* pDoc = (m_pView != nullptr) ? m_pView->GetDocument() : nullptr;
    const stirling::FileOffset total = (m_pView != nullptr) ? m_pView->TotalBytes() : 0;
    if (pDoc != nullptr) {
        for (const auto& kv : pDoc->Marks()) {
            CString row;
            row.Format(_T("%08llX"), static_cast<long long>(kv.first));
            const int idx = m_list.AddString(row);
            m_list.SetItemData(idx, static_cast<DWORD_PTR>(kv.second));
            m_positions.push_back(kv.first);
        }
    }
    // データが存在する時のみ末尾に「新規登録」行を置く（原 FUN_0042a590: this+0xd0!=0）。
    m_hasNewRow = (total > 0);
    if (m_hasNewRow) {
        const int idx = m_list.AddString(_T(""));   // 文字列はDrawItemがワイドで描画
        m_list.SetItemData(idx, static_cast<DWORD_PTR>(-1));
    }
}

bool CMarkListDlg::IsNewEntryRow(int index) const {
    return m_hasNewRow && index == static_cast<int>(m_positions.size());
}

stirling::FileOffset CMarkListDlg::MarkPosAt(int index) const {
    if (index >= 0 && index < static_cast<int>(m_positions.size())) {
        return m_positions[index];
    }
    return -1;
}

void CMarkListDlg::UpdateButtons() {
    const int sel = m_list.GetCurSel();
    const bool onMark = (sel != LB_ERR) && !IsNewEntryRow(sel);
    GetDlgItem(IDC_MARKLIST_REMOVE)->EnableWindow(onMark);
    GetDlgItem(IDC_MARKLIST_EDIT)->EnableWindow(onMark);
    GetDlgItem(IDC_MARKLIST_CLEARALL)->EnableWindow(!m_positions.empty());
}

void CMarkListDlg::SelectMarkPos(stirling::FileOffset pos) {
    for (size_t i = 0; i < m_positions.size(); ++i) {
        if (m_positions[i] == pos) { m_list.SetCurSel(static_cast<int>(i)); return; }
    }
    const int cnt = m_list.GetCount();
    if (cnt > 0) { m_list.SetCurSel(cnt - 1); }
}

void CMarkListDlg::OnSelChange() {
    UpdateButtons();
}

void CMarkListDlg::OnDblClk() {
    OnOK();   // ダブルクリック = 実行（原 FUN_0042a61b→OnOK）
}

void CMarkListDlg::OnOK() {
    const int sel = m_list.GetCurSel();
    if (sel == LB_ERR) { CDialog::OnOK(); return; }   // 選択無し→閉じる
    if (IsNewEntryRow(sel)) {
        OpenAddressDlg(-1);   // 新規登録（閉じない）
        return;
    }
    m_jumpPos = MarkPosAt(sel);
    m_doJump  = (m_jumpPos >= 0);
    CDialog::OnOK();          // 呼び元(view)が m_jumpPos へジャンプ
}

void CMarkListDlg::OnRemove() {
    const int sel = m_list.GetCurSel();
    if (sel == LB_ERR || IsNewEntryRow(sel)) { return; }
    const stirling::FileOffset pos = MarkPosAt(sel);
    if (pos < 0) { return; }
    CStirlingDoc* pDoc = m_pView->GetDocument();
    pDoc->RemoveMark(pos);
    pDoc->UpdateAllViews(nullptr);
    RebuildList();
    const int cnt = m_list.GetCount();
    if (cnt > 0) { m_list.SetCurSel(cnt - 1); }   // 原: 末尾行を選択
    UpdateButtons();
    m_list.SetFocus();
}

void CMarkListDlg::OnClearAll() {
    CStirlingDoc* pDoc = m_pView->GetDocument();
    pDoc->ClearMarks();
    pDoc->UpdateAllViews(nullptr);
    RebuildList();
    const int cnt = m_list.GetCount();
    if (cnt > 0) { m_list.SetCurSel(cnt - 1); }
    UpdateButtons();
    if (CWnd* p = GetDlgItem(IDCANCEL)) { p->SetFocus(); }   // 原: 閉じるボタンへフォーカス
}

void CMarkListDlg::OnEditMark() {
    const int sel = m_list.GetCurSel();
    if (sel == LB_ERR || IsNewEntryRow(sel)) { return; }
    OpenAddressDlg(sel);
}

// 編集(index>=0)/新規登録(index<0) の共通処理（原 FUN_0042a193 / FUN_0042a634 の追加分岐）。
void CMarkListDlg::OpenAddressDlg(int index) {
    CStirlingDoc* pDoc = m_pView->GetDocument();
    if (pDoc == nullptr) { return; }
    CStirlingSettings& s = theApp.Settings();
    const stirling::FileOffset total   = m_pView->TotalBytes();
    const stirling::FileOffset maxAddr = (total > 0) ? total - 1 : 0;

    const bool isNew = (index < 0);
    stirling::FileOffset initAddr = 0, oldPos = -1;
    int initType = 0;
    bool prefill = true;
    if (isNew) {
        initAddr = m_pView->CurrentPos();
        initType = 0;
        // 既にキャレット位置がマーク済みなら空欄開始（原 param_5=0 分岐）。
        prefill = !pDoc->GetMark(initAddr, nullptr);
    } else {
        oldPos   = MarkPosAt(index);
        initType = static_cast<int>(m_list.GetItemData(index));
        initAddr = oldPos;
        prefill  = true;
    }

    CMarkAddressDlg dlg(this, s.markText, s.markBack, initType, maxAddr, initAddr, prefill);
    if (dlg.DoModal() != IDOK) { return; }
    const stirling::FileOffset newAddr = dlg.ResultAddr();
    const int newType = dlg.ResultType();

    if (isNew) {
        if (pDoc->GetMark(newAddr, nullptr)) { return; }   // 同アドレスに既存 → 何もしない（原）
        pDoc->SetMark(newAddr, newType);
    } else {
        if (newAddr != oldPos && pDoc->GetMark(newAddr, nullptr)) {
            return;   // 別マークと衝突 → 何もしない
        }
        pDoc->RemoveMark(oldPos);
        pDoc->SetMark(newAddr, newType);
    }
    pDoc->UpdateAllViews(nullptr);
    RebuildList();
    SelectMarkPos(newAddr);
    UpdateButtons();
}
