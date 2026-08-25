// CFileChangedDlg 実装（原 IDD_FILE_CHANGED=199）。
#include "pch.h"
#include "app/UiStrings.h"   // ui::MsgBox（表題はアプリ名で統一）
#include "dialog/FileChangedDlg.h"
#include "dialog/DlgHexInput.h"   // dlg::LoadWStr

BEGIN_MESSAGE_MAP(CFileChangedDlg, CDialog)
    ON_CONTROL_RANGE(BN_CLICKED, IDC_FILECHG_IGNORE, IDC_FILECHG_SAVEAS,
                     &CFileChangedDlg::OnChoiceChanged)
    ON_BN_CLICKED(IDC_FILECHG_BROWSE, &CFileChangedDlg::OnBrowse)
END_MESSAGE_MAP()

CFileChangedDlg::CFileChangedDlg(CWnd* pParent, const CString& curPath)
    : CDialog(IDD_FILE_CHANGED, pParent)
    , m_curPath(curPath) {
}

void CFileChangedDlg::DoDataExchange(CDataExchange* pDX) {
    CDialog::DoDataExchange(pDX);
    DDX_Radio(pDX, IDC_FILECHG_IGNORE, m_choice);   // 1016/1017/1018 → 0/1/2
    DDX_Text(pDX, IDC_FILECHG_NAME, m_saveAs);
    DDX_Check(pDX, IDC_FILECHG_COMPARE, m_compare);
}

BOOL CFileChangedDlg::OnInitDialog() {
    CDialog::OnInitDialog();
    // 警告アイコン（原はテンプレートの静的コントロールへ実行時に設定する）。
    if (CStatic* icon = static_cast<CStatic*>(GetDlgItem(IDC_FILECHG_ICON))) {
        icon->SetIcon(::LoadIcon(nullptr, IDI_EXCLAMATION));
    }
    UpdateEnableState();
    return TRUE;
}

// 別名保存が選ばれているときだけ、ファイル名・参照・比較実行を有効にする（原挙動）。
void CFileChangedDlg::UpdateEnableState() {
    const BOOL saveAs = (m_choice == kSaveAs) ? TRUE : FALSE;
    static const UINT kIds[] = {
        IDC_FILECHG_NAME_LABEL, IDC_FILECHG_NAME, IDC_FILECHG_BROWSE, IDC_FILECHG_COMPARE,
    };
    for (int i = 0; i < _countof(kIds); ++i) {
        if (CWnd* c = GetDlgItem(kIds[i])) { c->EnableWindow(saveAs); }
    }
}

void CFileChangedDlg::OnChoiceChanged(UINT nID) {
    m_choice = static_cast<int>(nID - IDC_FILECHG_IGNORE);
    UpdateEnableState();
}

void CFileChangedDlg::OnBrowse() {
    CString name;
    GetDlgItemText(IDC_FILECHG_NAME, name);
    if (name.IsEmpty()) { name = m_curPath; }
    CFileDialog dlg(FALSE, nullptr, name, OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT,
                    nullptr, this);
    CString title;
    GetWindowText(title);
    dlg.m_ofn.lpstrTitle = title;
    if (dlg.DoModal() == IDOK) {
        SetDlgItemText(IDC_FILECHG_NAME, dlg.GetPathName());
    }
}

void CFileChangedDlg::OnOK() {
    if (!UpdateData(TRUE)) { return; }
    m_saveAs.Trim(_T(" \t"));
    if (m_choice == kSaveAs && m_saveAs.IsEmpty()) {
        // 原: 別名保存でファイル名が空なら文字列 1027 を表示し、ダイアログは閉じない。
        ui::MsgBox(GetSafeHwnd(), dlg::LoadWStr(IDS_SAVEDUMP_NOFILE), MB_OK | MB_ICONEXCLAMATION);
        if (CWnd* edit = GetDlgItem(IDC_FILECHG_NAME)) { edit->SetFocus(); }
        return;
    }
    CDialog::OnOK();
}
