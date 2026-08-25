// CFindMismatchDlg 実装（モーダル不一致検索ダイアログ）。
#include "pch.h"
#include "app/UiStrings.h"   // ui::MsgBox（表題はアプリ名で統一）
#include "dialog/FindMismatchDlg.h"
#include "dialog/DlgHexInput.h"
#include "view/StirlingView.h"

BEGIN_MESSAGE_MAP(CFindMismatchDlg, CDialog)
    ON_BN_CLICKED(IDC_MISMATCH_NEXT, &CFindMismatchDlg::OnFindNext)
    ON_BN_CLICKED(IDC_MISMATCH_PREV, &CFindMismatchDlg::OnFindPrev)
END_MESSAGE_MAP()

CFindMismatchDlg::CFindMismatchDlg(CStirlingView* pView)
    : CDialog(IDD_FIND_MISMATCH, pView)
    , m_pView(pView) {
}

BOOL CFindMismatchDlg::OnInitDialog() {
    CDialog::OnInitDialog();
    // 既定: カーソル位置から（原の初期選択）。
    CheckRadioButton(IDC_MISMATCH_RANGE_CURSOR, IDC_MISMATCH_RANGE_SEL,
                     IDC_MISMATCH_RANGE_CURSOR);
    // 範囲選択が無いときは「選択範囲内」を選べない（原の仕様）。
    if (m_pView != nullptr && !m_pView->HasSelection()) {
        if (CWnd* pSel = GetDlgItem(IDC_MISMATCH_RANGE_SEL)) {
            pSel->EnableWindow(FALSE);
        }
    }
    GotoDlgCtrl(GetDlgItem(IDC_MISMATCH_BYTE));
    return FALSE;   // フォーカスを明示設定したので FALSE
}

int CFindMismatchDlg::CurrentRange() const {
    if (IsDlgButtonChecked(IDC_MISMATCH_RANGE_ALL)) { return kWholeData; }
    if (IsDlgButtonChecked(IDC_MISMATCH_RANGE_SEL)) { return kSelection; }
    return kFromCursor;
}

void CFindMismatchDlg::OnFindNext() { DoFind(true); }
void CFindMismatchDlg::OnFindPrev() { DoFind(false); }

void CFindMismatchDlg::DoFind(bool forward) {
    if (m_pView == nullptr) { return; }
    CStringW text;
    GetDlgItemText(IDC_MISMATCH_BYTE, text);
    text.Trim(L" \t");

    // 不一致パターン未入力はエラー。
    if (text.IsEmpty()) {
        ui::MsgBox(GetSafeHwnd(), dlg::LoadWStr(IDS_SEARCH_EMPTY), MB_OK | MB_ICONEXCLAMATION);
        return;
    }
    // 単一16進バイト（1〜2桁）として解析。
    if (text.GetLength() > 2) {
        ui::MsgBox(GetSafeHwnd(), dlg::LoadWStr(IDS_INVALID_DATA), MB_OK | MB_ICONEXCLAMATION);
        return;
    }
    int val = 0;
    for (int i = 0; i < text.GetLength(); ++i) {
        const int h = dlg::HexVal(text[i]);
        if (h < 0) {
            ui::MsgBox(GetSafeHwnd(), dlg::LoadWStr(IDS_INVALID_DATA), MB_OK | MB_ICONEXCLAMATION);
            return;
        }
        val = (val << 4) | h;
    }
    // 正規化した2桁16進を欄へ反映（大文字）。
    CStringW norm;
    norm.Format(L"%02X", val);
    SetDlgItemText(IDC_MISMATCH_BYTE, norm);

    m_pView->FindMismatchWithByte(static_cast<unsigned char>(val), CurrentRange(), forward);
}
