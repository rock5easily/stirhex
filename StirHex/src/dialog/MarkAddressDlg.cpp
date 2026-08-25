// CMarkAddressDlg / CMarkColorCombo 実装（原 IDD_MARK_ADDRESS=190）。
#include "pch.h"
#include "app/UiStrings.h"   // ui::MsgBox（表題はアプリ名で統一）
#include "dialog/MarkAddressDlg.h"
#include "dialog/DlgHexInput.h"   // dlg::LoadWStr

// ===========================================================================
// CMarkColorCombo — マーク色プレビューのオーナードローコンボ
// ===========================================================================
void CMarkColorCombo::SetColors(const COLORREF* fg, const COLORREF* bg, int count) {
    m_count = (count > 3) ? 3 : count;
    for (int i = 0; i < m_count; ++i) {
        m_fg[i] = fg[i];
        m_bg[i] = bg[i];
    }
}

void CMarkColorCombo::MeasureItem(LPMEASUREITEMSTRUCT lpMIS) {
    lpMIS->itemHeight = 14;   // ダイアログフォント(9pt)に見合う行高
}

void CMarkColorCombo::DrawItem(LPDRAWITEMSTRUCT lpDIS) {
    if (static_cast<int>(lpDIS->itemID) < 0) { return; }   // 空コンボ
    const int idx = static_cast<int>(lpDIS->itemID);
    const bool valid = (idx >= 0 && idx < m_count);
    const COLORREF bg = valid ? m_bg[idx] : ::GetSysColor(COLOR_WINDOW);
    const COLORREF fg = valid ? m_fg[idx] : ::GetSysColor(COLOR_WINDOWTEXT);

    CDC dc;
    dc.Attach(lpDIS->hDC);
    CRect rc = lpDIS->rcItem;
    dc.FillSolidRect(&rc, bg);            // マーク背景色で塗る
    CString s;
    GetLBText(idx, s);
    dc.SetBkMode(TRANSPARENT);
    dc.SetTextColor(fg);                  // マーク文字色で "MARKn"
    CRect tr = rc;
    tr.left += 4;
    dc.DrawText(s, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (lpDIS->itemState & ODS_SELECTED) {
        dc.DrawFocusRect(&rc);
    }
    dc.Detach();
}

// ===========================================================================
// CMarkAddressDlg
// ===========================================================================
BEGIN_MESSAGE_MAP(CMarkAddressDlg, CDialog)
    ON_CONTROL_RANGE(BN_CLICKED, IDC_MARKADDR_BASE_DEC, IDC_MARKADDR_BASE_HEX,
                     &CMarkAddressDlg::OnBaseChanged)
END_MESSAGE_MAP()

CMarkAddressDlg::CMarkAddressDlg(CWnd* pParent, const COLORREF* fg, const COLORREF* bg,
                                 int initType, stirling::FileOffset maxAddr, stirling::FileOffset initAddr, bool prefill)
    : CDialog(IDD_MARK_ADDRESS, pParent)
    , m_type(initType)
    , m_maxAddr(maxAddr)
    , m_addr(initAddr)
    , m_prefill(prefill)
    , m_baseHex(1)          // 原既定 = 16進（設定+0xe0=1）
{
    for (int i = 0; i < 3; ++i) { m_fg[i] = fg[i]; m_bg[i] = bg[i]; }
    if (m_type < 0 || m_type > 2) { m_type = 0; }
}

void CMarkAddressDlg::DoDataExchange(CDataExchange* pDX) {
    CDialog::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_MARKADDR_COLOR, m_combo);
    DDX_Text(pDX, IDC_MARKADDR_EDIT, m_addrText);
    DDX_Radio(pDX, IDC_MARKADDR_BASE_DEC, m_baseHex);   // 0=10進(1016) / 1=16進(1017)
}

BOOL CMarkAddressDlg::OnInitDialog() {
    CDialog::OnInitDialog();

    // 色コンボに MARK1/2/3 を各マーク色で登録（原 FUN_00428eeb）。
    m_combo.SetColors(m_fg, m_bg, 3);
    // オーナードロー固定高を明示（WM_MEASUREITEM がサブクラス化前に届く問題を回避）。
    m_combo.SetItemHeight(-1, 14);
    m_combo.SetItemHeight(0, 14);
    for (int i = 0; i < 3; ++i) {
        CString s;
        s.Format(_T("MARK%d"), i + 1);
        m_combo.AddString(s);
    }
    m_combo.SetCurSel(m_type);

    UpdateHint();

    if (m_prefill) {
        // 初期アドレスを現基数でエディットへ反映し、フォーカス＋全選択（原 param_5!=0 分岐）。
        CString s;
        s.Format(m_baseHex ? _T("%llX") : _T("%lld"), static_cast<long long>(m_addr));
        SetDlgItemText(IDC_MARKADDR_EDIT, s);
        CWnd* pEdit = GetDlgItem(IDC_MARKADDR_EDIT);
        if (pEdit != nullptr) {
            pEdit->SetFocus();
            static_cast<CEdit*>(pEdit)->SetSel(0, -1);
        }
        return FALSE;   // フォーカスを明示設定
    }
    return TRUE;
}

// 文字列を base(10/16) で厳密解析（原の判定に忠実。前後空白は不可、桁あふれは範囲外扱い）。
bool CMarkAddressDlg::ParseAddr(const CString& text, int base, long long& out) const {
    return dlg::ParseAddrStrict(text, base, out);
}

void CMarkAddressDlg::UpdateHint() {
    // 静的 1029 に「有効アドレス : 0 ～ N」を現基数で表示（原 FUN_00429072）。
    // MBCS＋/utf-8 の CP932 化を避けるためワイドで生成・設定。
    CStringW s;
    s.Format(ui::LoadW(m_baseHex ? IDS_ADDR_RANGE_HEX : IDS_ADDR_RANGE_DEC), m_maxAddr);
    ::SetDlgItemTextW(GetSafeHwnd(), IDC_MARKADDR_HINT, s);
}

// アドレスベース切替（原 FUN_0042925c）。現在値を旧基数で解析し新基数で再整形（不正は空）。
void CMarkAddressDlg::OnBaseChanged(UINT nID) {
    const int newHex = (nID == IDC_MARKADDR_BASE_HEX) ? 1 : 0;
    if (newHex == m_baseHex) { return; }

    CString text;
    GetDlgItemText(IDC_MARKADDR_EDIT, text);
    long long v = 0;
    CString reformatted;
    if (ParseAddr(text, m_baseHex ? 16 : 10, v) && v < dlg::kAddrOverflow) {
        reformatted.Format(newHex ? _T("%llX") : _T("%lld"), static_cast<long long>(v));
    }   // 解析失敗時は空欄
    SetDlgItemText(IDC_MARKADDR_EDIT, reformatted);

    m_baseHex = newHex;
    UpdateHint();
}

void CMarkAddressDlg::OnOK() {
    UpdateData(TRUE);   // m_addrText / m_baseHex を取得

    long long v = 0;
    if (!ParseAddr(m_addrText, m_baseHex ? 16 : 10, v)) {
        // 解析失敗: 基数別メッセージ→エディットへ再フォーカス＋全選択（原 OnOK）。
        ui::MsgBox(GetSafeHwnd(), dlg::LoadWStr(m_baseHex ? IDS_MARK_ADDR_HEX : IDS_MARK_ADDR_DEC), MB_OK | MB_ICONEXCLAMATION);
        if (CWnd* pEdit = GetDlgItem(IDC_MARKADDR_EDIT)) {
            pEdit->SetFocus();
            static_cast<CEdit*>(pEdit)->SetSel(0, -1);
        }
        return;
    }
    if (v > m_maxAddr) {
        ui::MsgBox(GetSafeHwnd(), dlg::LoadWStr(IDS_MARK_ADDR_RANGE), MB_OK | MB_ICONEXCLAMATION);
        if (CWnd* pEdit = GetDlgItem(IDC_MARKADDR_EDIT)) {
            pEdit->SetFocus();
            static_cast<CEdit*>(pEdit)->SetSel(0, -1);
        }
        return;
    }
    m_addr = v;
    m_type = m_combo.GetCurSel();
    if (m_type < 0) { m_type = 0; }
    CDialog::OnOK();
}
