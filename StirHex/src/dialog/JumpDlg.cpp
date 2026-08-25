// CJumpDlg 実装（原 IDD_JUMP=137）。
#include "pch.h"
#include "app/UiStrings.h"   // ui::MsgBox（表題はアプリ名で統一）
#include "dialog/JumpDlg.h"
#include "dialog/DlgHexInput.h"   // dlg::LoadWStr

#include <limits>

BEGIN_MESSAGE_MAP(CJumpDlg, CDialog)
    ON_CONTROL_RANGE(BN_CLICKED, IDC_JUMP_BASE_DEC, IDC_JUMP_BASE_HEX,
                     &CJumpDlg::OnBaseChanged)
END_MESSAGE_MAP()

CJumpDlg::CJumpDlg(CWnd* pParent, stirling::FileOffset total, stirling::FileOffset current)
    : CDialog(IDD_JUMP, pParent)
    , m_total(total)
    , m_current(current)
    , m_addr(current)
    , m_baseHex(1)          // 原既定 = 16進
{
}

void CJumpDlg::DoDataExchange(CDataExchange* pDX) {
    CDialog::DoDataExchange(pDX);
    DDX_Text(pDX, IDC_JUMP_EDIT, m_addrText);
    DDX_Radio(pDX, IDC_JUMP_BASE_DEC, m_baseHex);   // 0=10進(1016) / 1=16進(1017)
}

BOOL CJumpDlg::OnInitDialog() {
    CDialog::OnInitDialog();
    UpdateHints();
    // アドレス欄は空欄で開始（原はカーソル位置を現在アドレス静的に表示し、入力は空）。
    return TRUE;
}

// 文字列を base(10/16) で厳密解析（原の判定に忠実。前後空白は不可、桁あふれは範囲外扱い）。
//   符号は呼び出し側で取り除いてから渡す。
bool CJumpDlg::ParseAddr(const CString& text, int base, long long& out) const {
    return dlg::ParseAddrStrict(text, base, out);
}

void CJumpDlg::UpdateHints() {
    // MBCS＋/utf-8 の CP932 化を避けるためワイドで生成・設定。
    CStringW range, cur;
    range.Format(ui::LoadW(m_baseHex ? IDS_ADDR_RANGE_HEX : IDS_ADDR_RANGE_DEC), m_total);
    cur.Format(ui::LoadW(m_baseHex ? IDS_ADDR_CURRENT_HEX : IDS_ADDR_CURRENT_DEC),
               static_cast<long long>(m_current));
    ::SetDlgItemTextW(GetSafeHwnd(), IDC_JUMP_HINT_RANGE, range);
    ::SetDlgItemTextW(GetSafeHwnd(), IDC_JUMP_HINT_CURRENT, cur);
}

// アドレスベース切替: 現在値を旧基数で解析し新基数へ再整形（先頭 +/- は保持、不正は空）。
void CJumpDlg::OnBaseChanged(UINT nID) {
    const int newHex = (nID == IDC_JUMP_BASE_HEX) ? 1 : 0;
    if (newHex == m_baseHex) { return; }

    CString text;
    GetDlgItemText(IDC_JUMP_EDIT, text);   // 原は前後の空白を受け付けない（トリムしない）
    CString sign;
    if (!text.IsEmpty() && (text[0] == _T('+') || text[0] == _T('-'))) {
        sign = text.Left(1);
        text = text.Mid(1);
    }
    CString reformatted;
    long long v = 0;
    if (ParseAddr(text, m_baseHex ? 16 : 10, v) && v < dlg::kAddrOverflow) {
        CString num;
        num.Format(newHex ? _T("%llX") : _T("%lld"), static_cast<long long>(v));
        reformatted = sign + num;
    }
    SetDlgItemText(IDC_JUMP_EDIT, reformatted);

    m_baseHex = newHex;
    UpdateHints();
}

void CJumpDlg::OnOK() {
    UpdateData(TRUE);   // m_addrText / m_baseHex

    CString text = m_addrText;   // 原は前後の空白を受け付けない（トリムしない）
    int rel = 0;   // 0=絶対 / +1=カーソル相対前方 / -1=後方
    if (!text.IsEmpty() && (text[0] == _T('+') || text[0] == _T('-'))) {
        rel = (text[0] == _T('-')) ? -1 : 1;
        text = text.Mid(1);
    }

    long long v = 0;
    if (!ParseAddr(text, m_baseHex ? 16 : 10, v)) {
        ui::MsgBox(GetSafeHwnd(), dlg::LoadWStr(m_baseHex ? IDS_MARK_ADDR_HEX : IDS_MARK_ADDR_DEC), MB_OK | MB_ICONEXCLAMATION);
        if (CWnd* pEdit = GetDlgItem(IDC_JUMP_EDIT)) {
            pEdit->SetFocus();
            static_cast<CEdit*>(pEdit)->SetSel(0, -1);
        }
        return;
    }

    // 範囲判定はドキュメントサイズのみ（Issue #32）。
    //   原 Stirling(FUN_0041839a @0x0041854c) は前方相対のみ 0x0FFFFFFF(256MB) を上限とするが、
    //   32bit 時代の内部上限の名残と判断し、x64 化に伴い意図的に原と乖離させる。
    long long target;
    bool ok;
    if (rel > 0) {
        // 加算前に符号付きオーバーフローを弾く（現実の値域では到達しないが型上の安全のため）。
        if (v > (std::numeric_limits<long long>::max)() - static_cast<long long>(m_current)) {
            target = 0;
            ok = false;
        } else {
            target = static_cast<long long>(m_current) + v;
            ok = (target <= m_total);
        }
    } else if (rel < 0) {
        target = static_cast<long long>(m_current) - v;
        ok = (v <= m_current);
    } else {
        target = v;
        ok = (v <= m_total);
    }
    if (!ok) {
        ui::MsgBox(GetSafeHwnd(), dlg::LoadWStr(IDS_MARK_ADDR_RANGE), MB_OK | MB_ICONEXCLAMATION);
        if (CWnd* pEdit = GetDlgItem(IDC_JUMP_EDIT)) {
            pEdit->SetFocus();
            static_cast<CEdit*>(pEdit)->SetSel(0, -1);
        }
        return;
    }
    m_addr = target;
    CDialog::OnOK();
}
