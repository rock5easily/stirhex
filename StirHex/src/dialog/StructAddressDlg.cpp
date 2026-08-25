// CStructAddressDlg 実装。
#include "pch.h"
#include "app/UiStrings.h"   // UI文字列はリソースから
#include "dialog/StructAddressDlg.h"
#include "dialog/DlgHexInput.h"   // dlg::ParseAddrStrict
#include "doc/StirlingDoc.h"
#include "util/ScopedGdi.h"   // GDI オブジェクトの RAII（Issue #48）

#include <limits>

BEGIN_MESSAGE_MAP(CStructAddressDlg, CDialog)
    ON_CONTROL_RANGE(BN_CLICKED, IDC_TOPADDR_MODE_ADDRESS, IDC_TOPADDR_MODE_MARK,
                     &CStructAddressDlg::OnModeChanged)
    ON_CONTROL_RANGE(BN_CLICKED, IDC_TOPADDR_BASE_DEC, IDC_TOPADDR_BASE_HEX,
                     &CStructAddressDlg::OnBaseChanged)
    ON_LBN_DBLCLK(IDC_TOPADDR_MARK_LIST, &CStructAddressDlg::OnMarkDblClk)
END_MESSAGE_MAP()

CStructAddressDlg::CStructAddressDlg(CWnd* pParent, CStirlingDoc* pDoc,
                                     stirling::FileOffset current, stirling::FileOffset total)
    : CDialog(IDD_TOP_ADDRESS, pParent)
    , m_pDoc(pDoc)
    , m_current(max(0, current))
    , m_total(max(0, total))
    , m_result(max(0, current))
{
}

void CStructAddressDlg::DoDataExchange(CDataExchange* pDX) {
    CDialog::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_TOPADDR_MARK_LIST, m_markList);
    DDX_Text(pDX, IDC_TOPADDR_EDIT, m_addressText);
    DDX_Radio(pDX, IDC_TOPADDR_MODE_ADDRESS, m_modeMark);
    DDX_Radio(pDX, IDC_TOPADDR_BASE_DEC, m_baseHex);
}

BOOL CStructAddressDlg::OnInitDialog() {
    CDialog::OnInitDialog();
    if (m_pDoc != nullptr) {
        const CStirlingSettings& settings = m_pDoc->Settings();
        m_markList.SetColors(settings.markText, settings.markBack, 3);
        // WM_MEASUREITEMはDDXサブクラス化前に届くため、フォントから固定高を明示する。
        CClientDC dc(&m_markList);
        CFont* font = m_markList.GetFont();
        TEXTMETRIC metrics = { 0 };
        {
            const stirling::ScopedSelectFont selFont(&dc, font);
            dc.GetTextMetrics(&metrics);
        }
        m_markList.SetItemHeight(0, metrics.tmHeight + 2);
        for (const auto& mark : m_pDoc->Marks()) {
            CString address;
            address.Format(_T("%08llX"), static_cast<long long>(mark.first));
            const int index = m_markList.AddString(address);
            if (index >= 0) {
                m_markList.SetItemData(index, static_cast<DWORD_PTR>(mark.second));
                m_markPositions.push_back(mark.first);
            }
        }
    }
    if (!m_markPositions.empty()) {
        m_markList.SetCurSel(static_cast<int>(m_markPositions.size()) - 1);   // 原は末尾マーク
    }
    UpdateHints();
    UpdateEnableState();
    return TRUE;
}

// 原の判定に忠実な解析（前後空白は不可、桁あふれは範囲外扱い＝「指定アドレスは無効です」）。
bool CStructAddressDlg::ParseAddress(const CString& text, int base, long long& value) const {
    return dlg::ParseAddrStrict(text, base, value);
}

void CStructAddressDlg::UpdateHints() {
    CStringW range;
    CStringW current;
    range.Format(ui::LoadW(m_baseHex ? IDS_ADDR_RANGE_HEX : IDS_ADDR_RANGE_DEC),
                 m_total);
    current.Format(ui::LoadW(m_baseHex ? IDS_ADDR_CURRENT_HEX : IDS_ADDR_CURRENT_DEC),
                   m_current);
    ::SetDlgItemTextW(GetSafeHwnd(), IDC_TOPADDR_HINT_RANGE, range);
    ::SetDlgItemTextW(GetSafeHwnd(), IDC_TOPADDR_HINT_CURRENT, current);
}

void CStructAddressDlg::UpdateEnableState() {
    const BOOL addressMode = (m_modeMark == 0);
    const UINT addressControls[] = {
        IDC_TOPADDR_HINT_RANGE, IDC_TOPADDR_HINT_CURRENT, IDC_TOPADDR_ADDR_LABEL,
        IDC_TOPADDR_EDIT, IDC_TOPADDR_BASE_GROUP, IDC_TOPADDR_BASE_DEC, IDC_TOPADDR_BASE_HEX,
    };
    for (UINT id : addressControls) {
        if (CWnd* wnd = GetDlgItem(id)) wnd->EnableWindow(addressMode);
    }
    m_markList.EnableWindow(!addressMode);
}

void CStructAddressDlg::ReformatAddress(int newBaseHex) {
    CString text;
    GetDlgItemText(IDC_TOPADDR_EDIT, text);
    CString sign;
    if (!text.IsEmpty() && (text[0] == _T('+') || text[0] == _T('-'))) {
        sign = text.Left(1);
        text = text.Mid(1);
    }
    long long value = 0;
    CString formatted;
    if (ParseAddress(text, m_baseHex ? 16 : 10, value) && value < dlg::kAddrOverflow) {
        CString number;
        number.Format(newBaseHex ? _T("%llX") : _T("%lld"), static_cast<long long>(value));
        formatted = sign + number;
    }
    SetDlgItemText(IDC_TOPADDR_EDIT, formatted);
}

void CStructAddressDlg::OnModeChanged(UINT nID) {
    m_modeMark = (nID == IDC_TOPADDR_MODE_MARK) ? 1 : 0;
    CheckRadioButton(IDC_TOPADDR_MODE_ADDRESS, IDC_TOPADDR_MODE_MARK, nID);
    UpdateEnableState();
}

void CStructAddressDlg::OnBaseChanged(UINT nID) {
    const int newBaseHex = (nID == IDC_TOPADDR_BASE_HEX) ? 1 : 0;
    if (newBaseHex == m_baseHex) return;
    ReformatAddress(newBaseHex);
    m_baseHex = newBaseHex;
    CheckRadioButton(IDC_TOPADDR_BASE_DEC, IDC_TOPADDR_BASE_HEX, nID);
    UpdateHints();
}

void CStructAddressDlg::FocusAddressEdit() {
    if (CWnd* edit = GetDlgItem(IDC_TOPADDR_EDIT)) {
        edit->SetFocus();
        static_cast<CEdit*>(edit)->SetSel(0, -1);
    }
}

void CStructAddressDlg::OnOK() {
    UpdateData(TRUE);
    if (m_modeMark != 0) {
        const int selected = m_markList.GetCurSel();
        if (selected == LB_ERR || selected >= static_cast<int>(m_markPositions.size())) {
            ui::MsgBox(GetSafeHwnd(), dlg::LoadWStr(IDS_NO_VALID_SELECTION),
                       MB_OK | MB_ICONEXCLAMATION);
            m_markList.SetFocus();
            return;
        }
        m_result = m_markPositions[selected];
        CDialog::OnOK();
        return;
    }

    CString text = m_addressText;
    int relative = 0;
    if (!text.IsEmpty() && (text[0] == _T('+') || text[0] == _T('-'))) {
        relative = (text[0] == _T('-')) ? -1 : 1;
        text = text.Mid(1);
    }
    long long value = 0;
    if (!ParseAddress(text, m_baseHex ? 16 : 10, value)) {
        ui::MsgBox(GetSafeHwnd(), dlg::LoadWStr(m_baseHex ? IDS_MARK_ADDR_HEX : IDS_MARK_ADDR_DEC),
                   MB_OK | MB_ICONEXCLAMATION);
        FocusAddressEdit();
        return;
    }

    // 範囲判定はドキュメントサイズのみで行う（Issue #32）。
    //   原 Stirling(FUN_00460bc1 @0x00460d8e) は前方相対のみ 0x0FFFFFFF(256MB) を上限とする
    //   非対称な判定を持つが、これは 32bit 時代の内部上限の名残と判断し、x64 化に伴い
    //   意図的に原と乖離させる。通常の移動ダイアログ(CJumpDlg)の判定とも揃う。
    long long target = value;
    if (relative > 0) {
        // 加算前に符号付きオーバーフローを弾く（現実の値域では到達しないが型上の安全のため）。
        if (value > (std::numeric_limits<long long>::max)() - static_cast<long long>(m_current)) {
            ui::MsgBox(GetSafeHwnd(), dlg::LoadWStr(IDS_MARK_ADDR_RANGE),
                       MB_OK | MB_ICONEXCLAMATION);
            FocusAddressEdit();
            return;
        }
        target = static_cast<long long>(m_current) + value;
    } else if (relative < 0) {
        target = static_cast<long long>(m_current) - value;
    }
    if (target < 0 || target > m_total) {
        ui::MsgBox(GetSafeHwnd(), dlg::LoadWStr(IDS_MARK_ADDR_RANGE),
                   MB_OK | MB_ICONEXCLAMATION);
        FocusAddressEdit();
        return;
    }
    m_result = target;
    CDialog::OnOK();
}

void CStructAddressDlg::OnMarkDblClk() {
    if (m_modeMark != 0) OnOK();
}
