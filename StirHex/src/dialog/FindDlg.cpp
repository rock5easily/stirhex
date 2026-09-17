// CFindDlg 実装（モーダル検索ダイアログ）。
#include "pch.h"
#include "app/UiStrings.h"   // ui::MsgBox（表題はアプリ名で統一）
#include "dialog/FindDlg.h"
#include "dialog/DlgHexInput.h"
#include "view/StirlingView.h"
#include "core/HexPattern.h"

BEGIN_MESSAGE_MAP(CFindDlg, CDialog)
    ON_BN_CLICKED(IDC_FIND_NEXT, &CFindDlg::OnFindNext)
    ON_BN_CLICKED(IDC_FIND_PREV, &CFindDlg::OnFindPrev)
    ON_BN_CLICKED(IDC_FIND_ALL, &CFindDlg::OnFindAll)
END_MESSAGE_MAP()

CFindDlg::CFindDlg(CStirlingView* pView)
    : CDialog(IDD_FIND, pView)
    , m_pView(pView) {
}

BOOL CFindDlg::OnInitDialog() {
    CDialog::OnInitDialog();
    // 既定: 16進種別・カーソル位置から（原の初期選択）。
    CheckRadioButton(IDC_FIND_TYPE_HEX, IDC_FIND_TYPE_TEXT, IDC_FIND_TYPE_HEX);
    CheckRadioButton(IDC_FIND_RANGE_CURSOR, IDC_FIND_RANGE_SEL, IDC_FIND_RANGE_CURSOR);
    // 範囲選択が無いときは「選択範囲内」を選べない（原の仕様）。
    if (m_pView != nullptr && !m_pView->HasSelection()) {
        if (CWnd* pSel = GetDlgItem(IDC_FIND_RANGE_SEL)) {
            pSel->EnableWindow(FALSE);
        }
    }
    GotoDlgCtrl(GetDlgItem(IDC_FIND_COMBO));
    return FALSE;   // フォーカスを明示設定したので FALSE
}

int CFindDlg::CurrentRange() const {
    if (IsDlgButtonChecked(IDC_FIND_RANGE_ALL)) { return kWholeData; }
    if (IsDlgButtonChecked(IDC_FIND_RANGE_SEL)) { return kSelection; }
    return kFromCursor;
}

bool CFindDlg::IsHexType() const {
    return IsDlgButtonChecked(IDC_FIND_TYPE_HEX) != 0;
}

void CFindDlg::OnFindNext() { DoFind(true); }
void CFindDlg::OnFindPrev() { DoFind(false); }

// [全て検索]（移植版で追加。Issue #236）: 入力を確定してダイアログを閉じる。
//   結果一覧はモードレスのため、モーダルの検索ダイアログを開いたままでは操作できない。
//   検索の開始は、戻り値 IDC_FIND_ALL を受けた所有ビューが行う。
void CFindDlg::OnFindAll() {
    stirling::HexPattern pattern;
    CStringW display;
    if (!ResolvePattern(pattern, display)) { return; }
    m_findAll.pattern = pattern;
    m_findAll.display = display;
    m_findAll.isHex = IsHexType();
    m_findAll.rangeMode = CurrentRange();
    EndDialog(IDC_FIND_ALL);
}

bool CFindDlg::ResolvePattern(stirling::HexPattern& pattern, CStringW& display) {
    if (m_pView == nullptr) { return false; }
    CStringW text;
    GetDlgItemText(IDC_FIND_COMBO, text);

    // 検索データ未入力（空白のみ含む）は原と同じくエラー表示。
    CStringW trimmed(text);
    trimmed.Trim(L" \t");
    if (trimmed.IsEmpty()) {
        ui::MsgBox(GetSafeHwnd(), dlg::LoadWStr(IDS_SEARCH_EMPTY), MB_OK | MB_ICONEXCLAMATION);
        return false;
    }

    if (IsHexType()) {
        // 16進データはワイルドカード `??` を受け付ける（Issue #233）。
        if (!stirling::ParseHexPattern(text.GetString(), static_cast<size_t>(text.GetLength()),
                                       true, pattern)) {
            ui::MsgBox(GetSafeHwnd(), dlg::LoadWStr(IDS_INVALID_DATA), MB_OK | MB_ICONEXCLAMATION);
            return false;
        }
        // 正規化した16進をコンボへ反映（大文字・2桁スペース区切り）。
        display = stirling::FormatHexPattern(pattern).c_str();
        SetDlgItemText(IDC_FIND_COMBO, display);
    } else {
        // 文字列は `?` も文字として扱う（ワイルドカードは16進データのみ）。
        pattern = stirling::MakeExactPattern(m_pView->BuildTextBytes(text));
        if (pattern.Empty()) {
            ui::MsgBox(GetSafeHwnd(), dlg::LoadWStr(IDS_INVALID_DATA), MB_OK | MB_ICONEXCLAMATION);
            return false;
        }
        display = text;
    }
    return true;
}

void CFindDlg::DoFind(bool forward) {
    stirling::HexPattern pattern;
    CStringW display;
    if (!ResolvePattern(pattern, display)) { return; }
    m_pView->FindWithPattern(pattern, CurrentRange(), forward);
}
