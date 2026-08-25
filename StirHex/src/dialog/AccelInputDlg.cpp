// CAccelInputDlg 実装（原 IDD_ACCEL_INPUT=184。ctor FUN_004013f0 / OnInitDialog FUN_004014e8 /
//   DoDataExchange FUN_00401493 / EN_CHANGE FUN_00401570 / OnOK FUN_004015aa）。
#include "pch.h"
#include "dialog/AccelInputDlg.h"

BEGIN_MESSAGE_MAP(CAccelInputDlg, CDialog)
    ON_EN_CHANGE(IDC_ACCEL_EDIT, &CAccelInputDlg::OnAccelChanged)
END_MESSAGE_MAP()

CAccelInputDlg::CAccelInputDlg(CWnd* pParent, UINT initial, bool addMode)
    : CDialog(IDD_ACCEL_INPUT, pParent)
    , m_accel(initial & 0xFF)
    , m_addMode(addMode)
{
}

void CAccelInputDlg::DoDataExchange(CDataExchange* pDX) {
    CDialog::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_ACCEL_EDIT, m_edit);
    DDX_Text(pDX, IDC_ACCEL_EDIT, m_text);
}

BOOL CAccelInputDlg::OnInitDialog() {
    CDialog::OnInitDialog();
    m_edit.LimitText(1);                       // 原: 1文字固定（大文字化はテンプレートの ES_UPPERCASE）
    if (!m_addMode && m_accel != 0) {
        // 変更モードは現在のアクセラレータを表示して全選択する。
        //   ※原は CDialog::OnInitDialog()（＝UpdateData(FALSE)）より後に代入していたため
        //     エディットが空のままになり、直後の SetSel(0,-1) が無意味になっていた。移植では表示する。
        m_text = static_cast<TCHAR>(m_accel);
        UpdateData(FALSE);
        m_edit.SetSel(0, -1);
    }
    // 追加モードは空欄で始まるので OK は無効。変更モードは現在値があるので有効（原と同じ）。
    if (CWnd* ok = GetDlgItem(IDOK)) { ok->EnableWindow(!m_addMode); }
    return TRUE;
}

void CAccelInputDlg::OnAccelChanged() {
    CString text;
    m_edit.GetWindowText(text);
    if (CWnd* ok = GetDlgItem(IDOK)) { ok->EnableWindow(!text.IsEmpty()); }
}

void CAccelInputDlg::OnOK() {
    UpdateData(TRUE);
    if (m_text.IsEmpty()) { return; }          // OK は無効化されているが念のため（空指定は受け付けない）
    m_accel = static_cast<UINT>(static_cast<unsigned char>(m_text[0]));
    CDialog::OnOK();
}
