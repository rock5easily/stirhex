// CSaveDumpDlg 実装（原 IDD_SAVE_DUMP=198, FUN_0040dc20 系）。
#include "pch.h"
#include "app/UiStrings.h"   // ui::MsgBox（表題はアプリ名で統一）
#include "dialog/SaveDumpDlg.h"
#include "dialog/DlgHexInput.h"   // dlg::LoadWStr

BEGIN_MESSAGE_MAP(CSaveDumpDlg, CDialog)
    ON_BN_CLICKED(IDC_SAVEDUMP_BROWSE, &CSaveDumpDlg::OnBrowse)
    ON_CONTROL_RANGE(BN_CLICKED, IDC_SAVEDUMP_WHOLE, IDC_SAVEDUMP_RANGE,
                     &CSaveDumpDlg::OnRangeMode)
END_MESSAGE_MAP()

CSaveDumpDlg::CSaveDumpDlg(CWnd* pParent, const CString& defName, stirling::FileOffset total,
                           bool hasSel, stirling::FileOffset selStart, stirling::FileOffset selEnd)
    : CDialog(IDD_SAVE_DUMP, pParent)
    , m_fileName(defName)
    , m_total(total)
    , m_hasSel(hasSel)
    , m_selStart(selStart)
    , m_selEnd(selEnd)
    , m_range(hasSel ? 1 : 0)   // 原 OnInitDialog: 選択ありで「範囲指定」既定
    , m_start(0)
    , m_end(total > 0 ? total - 1 : 0)
{
}

void CSaveDumpDlg::DoDataExchange(CDataExchange* pDX) {
    CDialog::DoDataExchange(pDX);
    DDX_Text(pDX, IDC_SAVEDUMP_FILE, m_fileName);
    DDX_Radio(pDX, IDC_SAVEDUMP_WHOLE, m_range);   // 0=データ全体(1016) / 1=範囲指定(1017)
}

BOOL CSaveDumpDlg::OnInitDialog() {
    CDialog::OnInitDialog();

    // 範囲入力バー（IDD 200）を子として生成し、範囲グループ内・ラジオの下へ配置（原 FUN_0040ddcd）。
    m_rangeBar.SetRange(m_total, m_hasSel, m_selStart, m_selEnd);
    if (m_rangeBar.Create(IDD_PRINT_RANGE_BAR, this)) {
        CRect pos(12, 68, 134, 140);   // ダイアログ単位（出力範囲グループ内）
        MapDialogRect(&pos);
        m_rangeBar.SetWindowPos(nullptr, pos.left, pos.top, 0, 0,
                                SWP_NOSIZE | SWP_NOZORDER);
        m_rangeBar.ShowWindow(SW_SHOW);
        m_rangeBar.SetPaneEnabled(m_range == 1);   // 範囲指定のときのみ有効（原 FUN_00430cfc）
    }

    if (CWnd* pEdit = GetDlgItem(IDC_SAVEDUMP_FILE)) {
        pEdit->SetFocus();
        static_cast<CEdit*>(pEdit)->SetSel(0, -1);
    }
    return FALSE;   // 明示的にフォーカスを設定したため FALSE
}

// データ全体/範囲指定 切替: 範囲入力バーの有効/無効を連動（原 FUN_0040deba→FUN_00430cfc）。
void CSaveDumpDlg::OnRangeMode(UINT /*nID*/) {
    UpdateData(TRUE);   // m_range
    if (m_rangeBar.GetSafeHwnd() != nullptr) {
        m_rangeBar.SetPaneEnabled(m_range == 1);
    }
}

// "..." 参照: 名前を付けて保存ダイアログで出力ファイル名を選ぶ（原 FUN_0040dee5→FUN_0043272c）。
void CSaveDumpDlg::OnBrowse() {
    UpdateData(TRUE);
    CFileDialog dlg(FALSE, nullptr, m_fileName,
                    OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT, nullptr, this);
    if (dlg.DoModal() == IDOK) {
        m_fileName = dlg.GetPathName();
        UpdateData(FALSE);
    }
}

void CSaveDumpDlg::OnOK() {
    UpdateData(TRUE);   // m_fileName / m_range

    CString name = m_fileName;
    name.Trim(_T(" \t"));
    if (name.IsEmpty()) {
        // 原: 空ファイル名は文字列 1027「ファイル名が入力されていません」。
        ui::MsgBox(GetSafeHwnd(), dlg::LoadWStr(IDS_SAVEDUMP_NOFILE), MB_OK | MB_ICONEXCLAMATION);
        if (CWnd* pEdit = GetDlgItem(IDC_SAVEDUMP_FILE)) {
            pEdit->SetFocus();
            static_cast<CEdit*>(pEdit)->SetSel(0, -1);
        }
        return;
    }
    m_fileName = name;

    if (m_range == 0) {                 // データ全体 → [0, total-1]
        m_start = 0;
        m_end   = (m_total > 0) ? m_total - 1 : 0;
    } else {                            // 範囲指定 → 範囲バーの入力を検証（原 FUN_00430b1c）
        stirling::FileOffset s = 0, e = 0;
        if (!m_rangeBar.Validate(s, e)) { return; }   // 不正なら閉じない
        m_start = s;
        m_end   = e;
    }

    // 上書き確認（原はファイルパス直接入力＋OK で無確認上書きする危険挙動のため追加）。
    //   参照ダイアログ経由は OFN_OVERWRITEPROMPT が働くが、欄に直接入力した既存パスは
    //   確認されないため、ここで存在チェックして確認する。
    const DWORD attr = ::GetFileAttributes(m_fileName);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        CStringW msg;
        msg.Format(ui::LoadW(IDS_SAVEDUMP_OVERWRITE), (LPCWSTR)CStringW(m_fileName));
        const int r = ui::MsgBox(GetSafeHwnd(), msg, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
        if (r != IDYES) {
            if (CWnd* pEdit = GetDlgItem(IDC_SAVEDUMP_FILE)) {
                pEdit->SetFocus();
                static_cast<CEdit*>(pEdit)->SetSel(0, -1);
            }
            return;   // 上書きしない（ダイアログは閉じない）
        }
    }
    CDialog::OnOK();
}
