// CBgrepDlg 実装（BGREP 設定ダイアログ）。
#include "pch.h"
#include "app/ShellUtil.h"   // ui::BrowseForFolder（IFileDialog ベースのフォルダ選択）
#include "app/UiStrings.h"   // ui::MsgBox（表題はアプリ名で統一）
#include "dialog/BgrepDlg.h"
#include "dialog/DlgHexInput.h"
#include "view/StirlingView.h"

BEGIN_MESSAGE_MAP(CBgrepDlg, CDialog)
    ON_BN_CLICKED(IDC_BGREP_BROWSE, &CBgrepDlg::OnBrowse)
    ON_BN_CLICKED(IDC_BGREP_TYPE_HEX, &CBgrepDlg::OnTypeChanged)
    ON_BN_CLICKED(IDC_BGREP_TYPE_TEXT, &CBgrepDlg::OnTypeChanged)
END_MESSAGE_MAP()

CBgrepDlg::CBgrepDlg(BgrepSettings* settings, CWnd* pParent)
    : CDialog(IDD_BGREP, pParent)
    , m_settings(settings) {
}

namespace {

// コンボの表示順（索引=コンボの位置、値=文字セットID）。
//   UTF-8(6) は文字コード体系の近い Unicode(3) の直後に置く（メニューと同じ並び）。
const int kCharsetOrder[] = {0, 1, 2, 3, 6, 4, 5};

// 文字セットID → コンボの位置（見つからなければシフトJISの位置）。
int CharsetToComboIndex(int charset) {
    for (int i = 0; i < (int)_countof(kCharsetOrder); ++i) {
        if (kCharsetOrder[i] == charset) { return i; }
    }
    return 1;
}

}  // namespace

BOOL CBgrepDlg::OnInitDialog() {
    CDialog::OnInitDialog();

    // キャラクタセット。並びは [設定]→[キャラクターセット] メニューと同じにするため、
    //   コンボの位置と文字セットIDは一致しない（kCharsetOrder で対応付ける）。
    if (CComboBox* pcs = (CComboBox*)GetDlgItem(IDC_BGREP_CHARSET)) {
        int cs = m_settings->charset;
        if (ui::CharsetNameW(cs).IsEmpty()) { cs = 1; }
        // 文字セット名は ui::CharsetNameW（文字列 6040-6046）へ集約する（Issue #125）。
        for (int id : kCharsetOrder) { pcs->AddString(ui::CharsetNameW(id)); }
        pcs->SetCurSel(CharsetToComboIndex(cs));
    }

    // 種別ラジオ（16進/文字列）。
    CheckRadioButton(IDC_BGREP_TYPE_HEX, IDC_BGREP_TYPE_TEXT,
                     m_settings->isHex ? IDC_BGREP_TYPE_HEX : IDC_BGREP_TYPE_TEXT);

    // 各入力欄へ前回設定を反映。
    SetDlgItemText(IDC_BGREP_DATA_COMBO, m_settings->searchData);
    SetDlgItemText(IDC_BGREP_FILE_COMBO, m_settings->fileMask);

    // フォルダ既定＝前回値、無ければカレントディレクトリ。
    CStringW folder = m_settings->folder;
    if (folder.IsEmpty()) { folder = ui::CurrentDirectory(); }
    SetDlgItemText(IDC_BGREP_FOLDER, folder);

    CheckDlgButton(IDC_BGREP_RECURSE, m_settings->recurse ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_BGREP_SKIPSYS, m_settings->skipSystem ? BST_CHECKED : BST_UNCHECKED);

    UpdateCharsetEnable();
    GotoDlgCtrl(GetDlgItem(IDC_BGREP_DATA_COMBO));
    return FALSE;   // フォーカスを明示設定
}

bool CBgrepDlg::IsHexType() const {
    return IsDlgButtonChecked(IDC_BGREP_TYPE_HEX) != 0;
}

void CBgrepDlg::UpdateCharsetEnable() {
    if (CWnd* pcs = GetDlgItem(IDC_BGREP_CHARSET)) {
        pcs->EnableWindow(IsHexType() ? FALSE : TRUE);   // 文字列種別のときのみ有効
    }
}

void CBgrepDlg::OnTypeChanged() {
    UpdateCharsetEnable();
}

// "..." ボタン: フォルダ選択（原の独自ダイアログ IDD 176 の代替に IFileDialog を使う）。
void CBgrepDlg::OnBrowse() {
    CStringW cur;
    GetDlgItemText(IDC_BGREP_FOLDER, cur);

    CStringW selected;
    const HRESULT hr =
        ui::BrowseForFolder(GetSafeHwnd(), ui::LoadW(IDS_FOLDER_SELECT_TITLE), cur, selected);
    if (SUCCEEDED(hr)) {
        SetDlgItemText(IDC_BGREP_FOLDER, selected);
    } else if (!ui::IsUserCancelled(hr)) {
        // キャンセル以外の失敗は握りつぶさず理由を添えて知らせる。
        ui::MsgBox(GetSafeHwnd(),
                   ui::AppendErrorReason(ui::LoadW(IDS_FOLDER_SELECT_FAILED),
                                         static_cast<DWORD>(hr)));
    }
}

void CBgrepDlg::OnOK() {
    CStringW data, mask, folder;
    GetDlgItemText(IDC_BGREP_DATA_COMBO, data);
    GetDlgItemText(IDC_BGREP_FILE_COMBO, mask);
    GetDlgItemText(IDC_BGREP_FOLDER, folder);

    // 検索データ未入力（空白のみ含む）はエラー。
    CStringW trimmed(data);
    trimmed.Trim(L" \t");
    if (trimmed.IsEmpty()) {
        ui::MsgBox(GetSafeHwnd(), dlg::LoadWStr(IDS_SEARCH_EMPTY), MB_OK | MB_ICONEXCLAMATION);
        return;
    }

    const bool isHex = IsHexType();
    int charset = 1;
    if (CComboBox* pcs = (CComboBox*)GetDlgItem(IDC_BGREP_CHARSET)) {
        const int sel = pcs->GetCurSel();
        if (sel >= 0 && sel < (int)_countof(kCharsetOrder)) { charset = kCharsetOrder[sel]; }
    }

    // 検索バイト列を構築（16進解析 or 文字セット変換）。
    m_pattern.clear();
    if (isHex) {
        if (!dlg::ParseHexStrict(data, m_pattern)) {
            ui::MsgBox(GetSafeHwnd(), dlg::LoadWStr(IDS_INVALID_DATA), MB_OK | MB_ICONEXCLAMATION);
            return;
        }
    } else {
        m_pattern = CStirlingView::EncodeText(charset, data);
        if (m_pattern.empty()) {
            ui::MsgBox(GetSafeHwnd(), dlg::LoadWStr(IDS_INVALID_DATA), MB_OK | MB_ICONEXCLAMATION);
            return;
        }
    }

    // フォルダの存在確認。
    CStringW folderPath(folder);
    folderPath.TrimRight(L"\\/ \t");
    if (folderPath.IsEmpty() ||
        ::GetFileAttributesW(folderPath) == INVALID_FILE_ATTRIBUTES) {
        ui::MsgBoxRes(GetSafeHwnd(), IDS_FOLDER_NOT_FOUND);
        return;
    }

    // 設定を確定（次回初期値として保持）。
    m_settings->searchData = data;
    m_settings->isHex = isHex;
    m_settings->charset = charset;
    m_settings->fileMask = mask;
    m_settings->folder = folderPath;
    m_settings->recurse = IsDlgButtonChecked(IDC_BGREP_RECURSE) != 0;
    m_settings->skipSystem = IsDlgButtonChecked(IDC_BGREP_SKIPSYS) != 0;

    CDialog::OnOK();
}
