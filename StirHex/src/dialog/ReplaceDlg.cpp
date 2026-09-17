// CReplaceDlg 実装（モーダル置換ダイアログ）。
#include "pch.h"
#include "app/UiStrings.h"   // ui::MsgBox（表題はアプリ名で統一）
#include "dialog/ReplaceDlg.h"
#include "dialog/DlgHexInput.h"
#include "view/StirlingView.h"

BEGIN_MESSAGE_MAP(CReplaceDlg, CDialog)
    ON_BN_CLICKED(IDC_REPL_NEXT, &CReplaceDlg::OnNext)
    ON_BN_CLICKED(IDC_REPL_PREV, &CReplaceDlg::OnPrev)
    ON_BN_CLICKED(IDC_REPL_ALL,  &CReplaceDlg::OnAllBtn)
END_MESSAGE_MAP()

CReplaceDlg::CReplaceDlg(CStirlingView* pView)
    : CDialog(IDD_REPLACE, pView)
    , m_pView(pView)
    , m_action(kNone)
    , m_range(kFromCursor) {
}

BOOL CReplaceDlg::OnInitDialog() {
    CDialog::OnInitDialog();
    // 既定: 検索/置換とも16進種別・カーソル位置から。
    CheckRadioButton(IDC_REPL_SEARCH_HEX, IDC_REPL_SEARCH_TEXT, IDC_REPL_SEARCH_HEX);
    CheckRadioButton(IDC_REPL_REPLACE_HEX, IDC_REPL_REPLACE_TEXT, IDC_REPL_REPLACE_HEX);
    CheckRadioButton(IDC_REPL_RANGE_CURSOR, IDC_REPL_RANGE_SEL, IDC_REPL_RANGE_CURSOR);
    // 範囲選択が無いときは「選択範囲内」を選べない（原の仕様）。
    if (m_pView != nullptr && !m_pView->HasSelection()) {
        if (CWnd* pSel = GetDlgItem(IDC_REPL_RANGE_SEL)) {
            pSel->EnableWindow(FALSE);
        }
    }
    GotoDlgCtrl(GetDlgItem(IDC_REPL_SEARCH_COMBO));
    return FALSE;
}

int CReplaceDlg::CurrentRange() const {
    if (IsDlgButtonChecked(IDC_REPL_RANGE_ALL)) { return kWholeData; }
    if (IsDlgButtonChecked(IDC_REPL_RANGE_SEL)) { return kSelection; }
    return kFromCursor;
}

// comboId のフィールドを解決。空/不正はメッセージを出して false。isReplace=true は
// 「空なら削除モード確認」を出す（Yes で空バイト列を許容）。
bool CReplaceDlg::ResolveField(int comboId, bool isHex, stirling::HexPattern& out) {
    CStringW text;
    GetDlgItemText(comboId, text);
    const bool isReplace = (comboId == IDC_REPL_REPLACE_COMBO);

    CStringW trimmed(text);
    trimmed.Trim(L" \t");
    if (trimmed.IsEmpty()) {
        if (isReplace) {
            // 置換データ未入力: 削除モードで実行するか確認（Yes で空バイト列）。
            const int yn = ui::MsgBox(GetSafeHwnd(), dlg::LoadWStr(IDS_REPLACE_EMPTY), MB_YESNO | MB_ICONQUESTION);
            if (yn != IDYES) { return false; }
            out = stirling::HexPattern();
            return true;
        }
        // 検索データ未入力。
        ui::MsgBox(GetSafeHwnd(), dlg::LoadWStr(IDS_SEARCH_EMPTY), MB_OK | MB_ICONEXCLAMATION);
        return false;
    }

    if (isHex) {
        // ワイルドカード `??` は検索データだけで受け付ける（Issue #233）。
        const bool allowWildcard = !isReplace;
        if (!stirling::ParseHexPattern(text.GetString(), static_cast<size_t>(text.GetLength()),
                                       allowWildcard, out)) {
            ui::MsgBox(GetSafeHwnd(), dlg::LoadWStr(IDS_INVALID_DATA), MB_OK | MB_ICONEXCLAMATION);
            return false;
        }
        SetDlgItemText(comboId, stirling::FormatHexPattern(out).c_str());
        return true;
    }
    out = stirling::MakeExactPattern(m_pView->BuildTextBytes(text));
    if (out.Empty()) {
        ui::MsgBox(GetSafeHwnd(), dlg::LoadWStr(IDS_INVALID_DATA), MB_OK | MB_ICONEXCLAMATION);
        return false;
    }
    return true;
}

void CReplaceDlg::Commit(Action action) {
    if (m_pView == nullptr) { return; }
    const bool searchHex  = IsDlgButtonChecked(IDC_REPL_SEARCH_HEX) != 0;
    const bool replaceHex = IsDlgButtonChecked(IDC_REPL_REPLACE_HEX) != 0;

    stirling::HexPattern spattern, rpattern;
    if (!ResolveField(IDC_REPL_SEARCH_COMBO, searchHex, spattern)) {
        return;
    }
    if (!ResolveField(IDC_REPL_REPLACE_COMBO, replaceHex, rpattern)) {
        return;   // 置換データ不正、または削除モード確認で No
    }

    m_searchPattern = spattern;
    m_replaceBytes = rpattern.bytes;   // 置換データはワイルドカードを含まない
    m_range = CurrentRange();
    m_action = action;
    EndDialog(IDOK);
}
