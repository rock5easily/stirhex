// CRangeBarDlg 実装（原 IDD_PRINT_RANGE_BAR=200, FUN_00430630 系）。
#include "pch.h"
#include "app/UiStrings.h"   // ui::MsgBox（表題はアプリ名で統一）
#include "dialog/RangeBarDlg.h"
#include "dialog/DlgHexInput.h"   // dlg::LoadWStr

BEGIN_MESSAGE_MAP(CRangeBarDlg, CDialog)
    ON_CONTROL_RANGE(BN_CLICKED, IDC_RANGEBAR_BASE_DEC, IDC_RANGEBAR_BASE_HEX,
                     &CRangeBarDlg::OnBaseChanged)
    ON_BN_CLICKED(IDC_RANGEBAR_USESEL, &CRangeBarDlg::OnUseSelection)
END_MESSAGE_MAP()

CRangeBarDlg::CRangeBarDlg(CWnd* pParent)
    : CDialog(IDD_PRINT_RANGE_BAR, pParent)
    , m_total(0)
    , m_hasSel(false)
    , m_selStart(0)
    , m_selEnd(0)
    , m_base(1)          // 原既定 = 16進
    , m_useSel(FALSE)
    , m_enableWhole(FALSE)
{
}

void CRangeBarDlg::SetRange(stirling::FileOffset total, bool hasSel, stirling::FileOffset selStart, stirling::FileOffset selEnd) {
    m_total    = total;
    m_hasSel   = hasSel;
    m_selStart = selStart;
    m_selEnd   = selEnd;
}

void CRangeBarDlg::DoDataExchange(CDataExchange* pDX) {
    CDialog::DoDataExchange(pDX);
    DDX_Text(pDX, IDC_RANGEBAR_START, m_startText);
    DDX_Text(pDX, IDC_RANGEBAR_END,   m_endText);
    DDX_Radio(pDX, IDC_RANGEBAR_BASE_DEC, m_base);   // 0=10進(1016) / 1=16進(1017)
    DDX_Check(pDX, IDC_RANGEBAR_USESEL,   m_useSel);
}

BOOL CRangeBarDlg::OnInitDialog() {
    CDialog::OnInitDialog();

    // 選択があれば「選択範囲」を既定 ON にし、開始/終了欄へ選択値を入れる（原 FUN_00430857）。
    //   選択が無ければ欄は空のまま（0/0 は入れない）。有効/無効は親が SetPaneEnabled で設定。
    m_useSel = m_hasSel ? TRUE : FALSE;
    if (m_hasSel) { FillFromSelection(); }

    UpdateData(FALSE);
    UpdateHint();
    return TRUE;
}

// ペイン全体の有効/無効（原 FUN_00430cfc）。各コントロールを個別に明示設定する。
//   選択範囲チェック = enableWhole かつ 選択あり／欄・基数・ヒント = enableWhole かつ 選択未使用。
void CRangeBarDlg::SetPaneEnabled(BOOL enableWhole) {
    m_enableWhole = enableWhole;
    EnableWindow(enableWhole);   // ペイン（子ダイアログ）全体
    if (CWnd* p = GetDlgItem(IDC_RANGEBAR_USESEL)) {
        p->EnableWindow(enableWhole && m_hasSel);
    }
    EnableInputs(enableWhole && !m_useSel);
}

// 原の判定に忠実な解析（前後空白は不可、桁あふれは範囲外扱い＝「指定アドレスは無効です」）。
bool CRangeBarDlg::ParseAddr(const CString& text, int base, long long& out) const {
    return dlg::ParseAddrStrict(text, base, out);
}

void CRangeBarDlg::UpdateHint() {
    // MBCS＋/utf-8 の CP932 化を避けるためワイドで生成・設定（原 DAT_004b63f4/640c）。
    const stirling::FileOffset maxAddr = (m_total > 0) ? m_total - 1 : 0;
    CStringW s;
    s.Format(ui::LoadW(m_base ? IDS_ADDR_RANGE_HEX : IDS_ADDR_RANGE_DEC), static_cast<long long>(maxAddr));
    ::SetDlgItemTextW(GetSafeHwnd(), IDC_RANGEBAR_HINT, s);
}

void CRangeBarDlg::FillFromSelection() {
    // 選択範囲を現基数で開始/終了欄へ（原 FUN_00430857/FUN_004309f3 の "%X" 相当。基数対応）。
    m_startText.Format(m_base ? _T("%llX") : _T("%lld"), static_cast<long long>(m_selStart));
    m_endText.Format(m_base ? _T("%llX") : _T("%lld"), static_cast<long long>(m_selEnd));
}

void CRangeBarDlg::EnableInputs(BOOL enable) {
    // 手入力欄・基数・ヒントの有効/無効（原 FUN_00430a76）。選択範囲 ON のとき無効化。
    static const UINT ids[] = { IDC_RANGEBAR_START, IDC_RANGEBAR_END, IDC_RANGEBAR_SEP,
                                IDC_RANGEBAR_HINT, IDC_RANGEBAR_BASE_GROUP,
                                IDC_RANGEBAR_BASE_DEC, IDC_RANGEBAR_BASE_HEX };
    for (UINT id : ids) {
        if (CWnd* p = GetDlgItem(id)) { p->EnableWindow(enable); }
    }
}

// 10進/16進 切替: 現在値を旧基数で解析し新基数へ再整形（原 FUN_004309a2→FUN_004171b1）。
//   UpdateData(TRUE) は m_base をラジオの新状態へ同期してしまい旧基数を失うため使わず、
//   JumpDlg と同様に Get/SetDlgItemText で直接扱う。
void CRangeBarDlg::OnBaseChanged(UINT nID) {
    const int newBase = (nID == IDC_RANGEBAR_BASE_HEX) ? 1 : 0;
    if (newBase == m_base) { return; }
    const int oldRadix = m_base ? 16 : 10;   // m_base はまだ旧基数

    CString startText, endText;
    GetDlgItemText(IDC_RANGEBAR_START, startText);
    GetDlgItemText(IDC_RANGEBAR_END,   endText);
    long long v = 0;
    if (ParseAddr(startText, oldRadix, v) && v < dlg::kAddrOverflow) {
        startText.Format(newBase ? _T("%llX") : _T("%lld"), static_cast<long long>(v));
    }
    if (ParseAddr(endText, oldRadix, v) && v < dlg::kAddrOverflow) {
        endText.Format(newBase ? _T("%llX") : _T("%lld"), static_cast<long long>(v));
    }
    SetDlgItemText(IDC_RANGEBAR_START, startText);
    SetDlgItemText(IDC_RANGEBAR_END,   endText);
    m_startText = startText;   // DDX/Validate 用にメンバも同期
    m_endText   = endText;
    m_base = newBase;
    UpdateHint();
}

// "選択範囲" チェック切替（原 FUN_004309f3）。ON で選択値を入れ手入力欄を無効化。
//   チェックボックスは選択ありのときのみ有効なので、ここに来る時点で m_hasSel は真。
void CRangeBarDlg::OnUseSelection() {
    UpdateData(TRUE);   // m_useSel
    if (m_useSel) { FillFromSelection(); UpdateData(FALSE); }
    EnableInputs(m_enableWhole && !m_useSel);
}

bool CRangeBarDlg::Validate(stirling::FileOffset& start, stirling::FileOffset& end) {
    UpdateData(TRUE);
    const int radix = m_base ? 16 : 10;

    // 終了欄（原は this+0xf0 を先に解析）。解析失敗→1000/1004、範囲外(>=total)→1003。
    long long vEnd = 0;
    if (!ParseAddr(m_endText, radix, vEnd)) {
        ui::MsgBox(GetSafeHwnd(), dlg::LoadWStr(m_base ? IDS_MARK_ADDR_HEX : IDS_MARK_ADDR_DEC), MB_OK | MB_ICONEXCLAMATION);
        if (CWnd* p = GetDlgItem(IDC_RANGEBAR_END)) {
            p->SetFocus(); static_cast<CEdit*>(p)->SetSel(0, -1);
        }
        return false;
    }
    if (vEnd >= m_total) {
        ui::MsgBox(GetSafeHwnd(), dlg::LoadWStr(IDS_MARK_ADDR_RANGE), MB_OK | MB_ICONEXCLAMATION);
        if (CWnd* p = GetDlgItem(IDC_RANGEBAR_END)) {
            p->SetFocus(); static_cast<CEdit*>(p)->SetSel(0, -1);
        }
        return false;
    }

    long long vStart = 0;
    if (!ParseAddr(m_startText, radix, vStart)) {
        ui::MsgBox(GetSafeHwnd(), dlg::LoadWStr(m_base ? IDS_MARK_ADDR_HEX : IDS_MARK_ADDR_DEC), MB_OK | MB_ICONEXCLAMATION);
        if (CWnd* p = GetDlgItem(IDC_RANGEBAR_START)) {
            p->SetFocus(); static_cast<CEdit*>(p)->SetSel(0, -1);
        }
        return false;
    }
    if (vStart >= m_total) {
        ui::MsgBox(GetSafeHwnd(), dlg::LoadWStr(IDS_MARK_ADDR_RANGE), MB_OK | MB_ICONEXCLAMATION);
        if (CWnd* p = GetDlgItem(IDC_RANGEBAR_START)) {
            p->SetFocus(); static_cast<CEdit*>(p)->SetSel(0, -1);
        }
        return false;
    }

    if (vStart > vEnd) { const long long t = vStart; vStart = vEnd; vEnd = t; }   // 原: start>end は入替
    start = vStart;
    end   = vEnd;
    return true;
}
