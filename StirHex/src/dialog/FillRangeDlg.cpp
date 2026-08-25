// CFillRangeDlg 実装（原 IDD_FILL_RANGE=165, FUN_00410c80 系）。
#include "pch.h"
#include "app/UiStrings.h"   // ui::MsgBox（表題はアプリ名で統一）
#include "dialog/FillRangeDlg.h"
#include "dialog/DlgHexInput.h"   // dlg::LoadWStr / dlg::HexVal

CFillRangeDlg::CFillRangeDlg(CWnd* pParent, stirling::FileOffset startAbs, stirling::FileOffset endAbs)
    : CDialog(IDD_FILL_RANGE, pParent)
    , m_start(startAbs)
    , m_end(endAbs)
    , m_value(0)
{
}

void CFillRangeDlg::DoDataExchange(CDataExchange* pDX) {
    CDialog::DoDataExchange(pDX);
    DDX_Text(pDX, IDC_FILL_EDIT, m_valueText);
}

BOOL CFillRangeDlg::OnInitDialog() {
    CDialog::OnInitDialog();

    // 範囲静的（原 OnInitDialog は "指定範囲 : %08X ～ %08X" で丸ごと差し替え。16進固定）。
    // MBCS＋/utf-8 の CP932 化を避けるためワイドで生成・設定。
    CStringW range;
    range.Format(ui::LoadW(IDS_FILL_RANGE_LABEL),
                 static_cast<long long>(m_start), static_cast<long long>(m_end));
    ::SetDlgItemTextW(GetSafeHwnd(), IDC_FILL_RANGE, range);

    // 入力は 2桁16進（原 OnInitDialog は EM_LIMITTEXT 2）。
    if (CEdit* pEdit = static_cast<CEdit*>(GetDlgItem(IDC_FILL_EDIT))) {
        pEdit->LimitText(2);
        pEdit->SetFocus();
    }
    return FALSE;   // 明示的にフォーカスを設定したため FALSE
}

void CFillRangeDlg::OnOK() {
    UpdateData(TRUE);   // m_valueText

    CString text = m_valueText;
    text.Trim(_T(" \t"));

    // 空入力（原: 文字列 1021「初期化データが指定されていません」）。
    if (text.IsEmpty()) {
        ui::MsgBox(GetSafeHwnd(), dlg::LoadWStr(IDS_FILL_EMPTY), MB_OK | MB_ICONEXCLAMATION);
        if (CWnd* pEdit = GetDlgItem(IDC_FILL_EDIT)) {
            pEdit->SetFocus();
            static_cast<CEdit*>(pEdit)->SetSel(0, -1);
        }
        return;
    }

    // 16進解析（原 FUN_0041145e base=16）。全桁16進かつ値 <= 0xFF。
    unsigned int v = 0;
    bool ok = true;
    for (int i = 0; i < text.GetLength(); ++i) {
        const int d = dlg::HexVal(static_cast<char>(text[i]));
        if (d < 0) { ok = false; break; }
        v = v * 16 + static_cast<unsigned int>(d);
    }
    if (!ok || v > 0xFF) {
        // 原: 解析失敗/範囲外は文字列 1004「１６進数で入力してください」。
        ui::MsgBox(GetSafeHwnd(), dlg::LoadWStr(IDS_MARK_ADDR_HEX), MB_OK | MB_ICONEXCLAMATION);
        if (CWnd* pEdit = GetDlgItem(IDC_FILL_EDIT)) {
            pEdit->SetFocus();
            static_cast<CEdit*>(pEdit)->SetSel(0, -1);
        }
        return;
    }

    m_value = static_cast<unsigned char>(v);
    CDialog::OnOK();
}
