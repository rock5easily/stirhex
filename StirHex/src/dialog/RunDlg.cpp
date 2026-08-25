// CRunDlg 実装（原 IDD_RUN=168 「名前を指定して実行」。コマンド 0x804f）。
#include "pch.h"
#include "app/ShellUtil.h"   // ui::ShellExecuteFile / ui::AppendErrorReason
#include "app/UiStrings.h"   // ui::MsgBox（表題はアプリ名で統一）
#include "dialog/RunDlg.h"
#include "dialog/DlgHexInput.h"   // dlg::LoadWStr

#include <vector>

namespace {

const int kMaxHistory = 10;                       // 原の Execute0..9 に合わせる
LPCTSTR   kHistorySection = _T("History");        // 原と同じセクション名

// 履歴の読み出し（Execute0 が最新。空エントリで打ち切り）。
std::vector<CString> LoadHistory() {
    std::vector<CString> items;
    for (int i = 0; i < kMaxHistory; ++i) {
        CString key;
        key.Format(_T("Execute%d"), i);
        const CString v = AfxGetApp()->GetProfileString(kHistorySection, key, _T(""));
        if (v.IsEmpty()) { break; }
        items.push_back(v);
    }
    return items;
}

// 履歴の書き戻し（余った番号は削除して原と同じ連番を保つ）。
void SaveHistory(const std::vector<CString>& items) {
    for (int i = 0; i < kMaxHistory; ++i) {
        CString key;
        key.Format(_T("Execute%d"), i);
        if (i < static_cast<int>(items.size())) {
            AfxGetApp()->WriteProfileString(kHistorySection, key, items[i]);
        } else {
            AfxGetApp()->WriteProfileString(kHistorySection, key, nullptr);   // 値を削除
        }
    }
}

// 実行したコマンドラインを先頭へ（重複は除去）。原は起動の成否に依らず追加する。
void PushHistory(const CString& cmd) {
    std::vector<CString> items = LoadHistory();
    for (std::vector<CString>::iterator it = items.begin(); it != items.end(); ) {
        it = (it->CompareNoCase(cmd) == 0) ? items.erase(it) : it + 1;
    }
    items.insert(items.begin(), cmd);
    if (static_cast<int>(items.size()) > kMaxHistory) { items.resize(kMaxHistory); }
    SaveHistory(items);
}

}  // namespace

BEGIN_MESSAGE_MAP(CRunDlg, CDialog)
    ON_BN_CLICKED(IDC_RUN_BROWSE, &CRunDlg::OnBrowse)
END_MESSAGE_MAP()

CRunDlg::CRunDlg(CWnd* pParent) : CDialog(IDD_RUN, pParent) {}

BOOL CRunDlg::OnInitDialog() {
    CDialog::OnInitDialog();
    // 履歴をドロップダウンへ（新しい順）。編集欄は空のまま（原の実測挙動）。
    if (CComboBox* combo = static_cast<CComboBox*>(GetDlgItem(IDC_RUN_COMBO))) {
        const std::vector<CString> items = LoadHistory();
        for (size_t i = 0; i < items.size(); ++i) { combo->AddString(items[i]); }
        combo->SetWindowText(_T(""));
        combo->SetFocus();
    }
    return FALSE;   // フォーカスは自前で設定済み
}

void CRunDlg::OnOK() {
    CString cmd;
    GetDlgItemText(IDC_RUN_COMBO, cmd);
    cmd.Trim(_T(" \t"));
    if (cmd.IsEmpty()) {
        // 原: 文字列 1027「ファイル名が入力されていません」を表示し、ダイアログは閉じない。
        ui::MsgBox(GetSafeHwnd(), dlg::LoadWStr(IDS_RUN_NOFILE), MB_OK | MB_ICONEXCLAMATION);
        if (CWnd* combo = GetDlgItem(IDC_RUN_COMBO)) { combo->SetFocus(); }
        return;
    }
    m_cmd = cmd;
    PushHistory(cmd);
    CDialog::OnOK();
}

void CRunDlg::OnBrowse() {
    // "プログラム|*.exe|すべてのファイル(*.*)|*.*||"
    const CStringW filter = ui::LoadW(IDS_RUN_FILTER);
    CFileDialog dlg(TRUE, nullptr, nullptr,
                    OFN_HIDEREADONLY | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST, filter, this);
    CString title;
    GetWindowText(title);                     // 原: 参照ダイアログの表題は本ダイアログと同じ
    dlg.m_ofn.lpstrTitle = title;
    if (dlg.DoModal() == IDOK) {
        SetDlgItemText(IDC_RUN_COMBO, dlg.GetPathName());
    }
}

// 「名前を指定して実行」コマンドの実体。
//   原はダイアログを閉じてから起動し、失敗時に文字列 1026 を表示する。入力文字列は
//   分割せずそのまま実行対象として渡すため、引数付きの指定は原と同様に失敗する。
//   起動は ShellExecuteEx で行い、失敗時は原のメッセージへ Win32 の失敗理由を添える。
void RunAppCommand(CWnd* pOwner) {
    CRunDlg dlg(pOwner);
    if (dlg.DoModal() != IDOK) { return; }

    const CStringW cmd = dlg.Command();
    HWND owner = (pOwner != nullptr) ? pOwner->GetSafeHwnd() : nullptr;

    DWORD error = ERROR_SUCCESS;
    if (ui::ShellExecuteFile(owner, cmd, error)) { return; }

    CStringW msg;
    msg.Format(dlg::LoadWStr(IDS_RUN_FAILED), cmd.GetString());
    ui::MsgBox(owner, ui::AppendErrorReason(msg, error), MB_OK | MB_ICONEXCLAMATION);
}
