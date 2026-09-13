// CChecksumDlg — チェックサム・ハッシュ計算ダイアログ（Issue #226）。
// 計算を短い単位に分け、進捗・中止・結果コピーを提供する。
#include "pch.h"
#include "dialog/ChecksumDlg.h"
#include "doc/StirlingDoc.h"
#include "app/UiStrings.h"
#include "app/ClipboardUtil.h"

namespace {
struct AlgorithmOption {
    UINT label;
    stirling::ChecksumAlgorithm algorithm;
};
constexpr AlgorithmOption kAlgorithms[] = {
    {IDS_CHECKSUM_CRC32, stirling::ChecksumAlgorithm::Crc32},
    {IDS_CHECKSUM_MD5, stirling::ChecksumAlgorithm::Md5},
    {IDS_CHECKSUM_SHA1, stirling::ChecksumAlgorithm::Sha1},
    {IDS_CHECKSUM_SHA256, stirling::ChecksumAlgorithm::Sha256},
};

bool SelectedAlgorithm(CComboBox& combo, stirling::ChecksumAlgorithm& algorithm) {
    const int index = combo.GetCurSel();
    if (index == CB_ERR) return false;
    const DWORD_PTR data = combo.GetItemData(index);
    for (const auto& option : kAlgorithms) {
        if (data == static_cast<DWORD_PTR>(option.algorithm)) {
            algorithm = option.algorithm;
            return true;
        }
    }
    return false;
}
} // namespace

BEGIN_MESSAGE_MAP(CChecksumDlg, CDialog)
    ON_BN_CLICKED(IDC_CHECKSUM_ALL, &CChecksumDlg::OnConditionsChanged)
    ON_BN_CLICKED(IDC_CHECKSUM_SELECTION, &CChecksumDlg::OnConditionsChanged)
    ON_CBN_SELCHANGE(IDC_CHECKSUM_ALGORITHM, &CChecksumDlg::OnConditionsChanged)
    ON_BN_CLICKED(IDC_CHECKSUM_COPY, &CChecksumDlg::OnCopy)
    ON_BN_CLICKED(IDC_CHECKSUM_STOP, &CChecksumDlg::OnStop)
    ON_WM_TIMER()
    ON_WM_DESTROY()
END_MESSAGE_MAP()

CChecksumDlg::CChecksumDlg(CWnd* parent, CStirlingDoc& doc, bool selected,
                           stirling::FileOffset start, stirling::FileOffset end)
    : CDialog(IDD_CHECKSUM, parent), doc_(doc), selected_(selected),
      selectionStart_(start), selectionEnd_(end) {}

BOOL CChecksumDlg::OnInitDialog() {
    CDialog::OnInitDialog();
    auto* algorithms = static_cast<CComboBox*>(GetDlgItem(IDC_CHECKSUM_ALGORITHM));
    for (const auto& option : kAlgorithms) {
        const int index = algorithms->AddString(ui::LoadW(option.label));
        // 表示順や enum の宣言順に依存せず、各項目に計算方式を保持する。
        if (index < 0 || algorithms->SetItemData(index,
                static_cast<DWORD_PTR>(option.algorithm)) == CB_ERR) {
            ui::MsgBoxRes(m_hWnd, IDS_CHECKSUM_ALGORITHM_ERROR);
            EndDialog(IDCANCEL);
            return FALSE;
        }
        if (option.algorithm == stirling::ChecksumAlgorithm::Sha256)
            algorithms->SetCurSel(index);
    }
    CheckRadioButton(IDC_CHECKSUM_ALL, IDC_CHECKSUM_SELECTION,
                     selected_ ? IDC_CHECKSUM_SELECTION : IDC_CHECKSUM_ALL);
    UpdateRunning(false);
    OnConditionsChanged();
    return TRUE;
}

void CChecksumDlg::UpdateRange() {
    const bool selection = IsDlgButtonChecked(IDC_CHECKSUM_SELECTION) == BST_CHECKED;
    start_ = selection ? selectionStart_ : 0;
    length_ = selection ? selectionEnd_ - selectionStart_ : doc_.GetTotalLength();
    CString text;
    text.Format(L"0x%llX", static_cast<unsigned long long>(start_));
    SetDlgItemText(IDC_CHECKSUM_START, text);
    text.Format(L"%lld", static_cast<long long>(length_));
    SetDlgItemText(IDC_CHECKSUM_LENGTH, text);
}

void CChecksumDlg::OnConditionsChanged() {
    if (checksum_.State() == stirling::ChecksumState::Running) return;
    SetDlgItemText(IDC_CHECKSUM_RESULT, L"");
    SetDlgItemText(IDC_CHECKSUM_STATUS, L"");
    GetDlgItem(IDC_CHECKSUM_COPY)->EnableWindow(FALSE);
    UpdateRange();
}

void CChecksumDlg::UpdateRunning(bool running) {
    GetDlgItem(IDC_CHECKSUM_ALL)->EnableWindow(!running);
    GetDlgItem(IDC_CHECKSUM_SELECTION)->EnableWindow(!running && selected_);
    GetDlgItem(IDC_CHECKSUM_ALGORITHM)->EnableWindow(!running);
    GetDlgItem(IDOK)->EnableWindow(!running);
    GetDlgItem(IDC_CHECKSUM_STOP)->EnableWindow(running);
    // 中止ボタンを無効化した後も、有効なボタンへ必ずフォーカスを戻す。
    GetDlgItem(running ? IDC_CHECKSUM_STOP : IDOK)->SetFocus();
}

void CChecksumDlg::OnOK() {
    if (checksum_.State() == stirling::ChecksumState::Running) return;
    OnConditionsChanged();
    changeSeq_ = doc_.ChangeSeq();
    auto* combo = static_cast<CComboBox*>(GetDlgItem(IDC_CHECKSUM_ALGORITHM));
    stirling::ChecksumAlgorithm algorithm;
    if (!SelectedAlgorithm(*combo, algorithm)) {
        ui::MsgBoxRes(m_hWnd, IDS_CHECKSUM_ALGORITHM_ERROR);
        combo->SetFocus();
        return;
    }
    checksum_.Start(doc_.Blocks(), start_, length_, algorithm);
    if (checksum_.State() != stirling::ChecksumState::Running) { PresentResult(); return; }
    UpdateRunning(true);
    if (!SetTimer(kTimer, 10, nullptr)) {
        checksum_.Cancel();
        UpdateRunning(false);
        ui::MsgBoxRes(m_hWnd, IDS_CHECKSUM_TIMER_ERROR);
    }
}

void CChecksumDlg::OnTimer(UINT_PTR timer) {
    if (timer != kTimer) { CDialog::OnTimer(timer); return; }
    if (checksum_.State() != stirling::ChecksumState::Running) return;
    // モーダル表示中は通常の編集を止める。想定外の変更も、保持した
    // ノードを参照する前に検出し、無効になったカーソルでの走査を防ぐ。
    if (doc_.ChangeSeq() != changeSeq_) {
        checksum_.Cancel();
        KillTimer(kTimer);
        UpdateRunning(false);
        SetDlgItemText(IDC_CHECKSUM_STATUS, L"");
        ui::MsgBoxRes(m_hWnd, IDS_CHECKSUM_CHANGED);
        return;
    }
    // 1回の処理量と時間を制限する。処理中にメッセージループを入れ子にしない。
    const ULONGLONG until = GetTickCount64() + 10;
    size_t budget = 1024 * 1024;
    do {
        checksum_.Step(64 * 1024);
        budget -= 64 * 1024;
    } while (budget && checksum_.State() == stirling::ChecksumState::Running && GetTickCount64() < until);
    if (checksum_.State() != stirling::ChecksumState::Running) { PresentResult(); return; }
    CString text;
    text.Format(ui::LoadW(IDS_CHECKSUM_PROGRESS),
        static_cast<long long>(checksum_.Processed()), static_cast<long long>(length_));
    SetDlgItemText(IDC_CHECKSUM_STATUS, text);
}

void CChecksumDlg::PresentResult() {
    KillTimer(kTimer);
    UpdateRunning(false);
    if (checksum_.State() == stirling::ChecksumState::Complete) {
        CStringW result(checksum_.Hex()); // 結果は ASCII の16進文字列のみ。
        SetDlgItemText(IDC_CHECKSUM_RESULT, result);
        SetDlgItemText(IDC_CHECKSUM_STATUS, ui::LoadW(IDS_CHECKSUM_DONE));
        GetDlgItem(IDC_CHECKSUM_COPY)->EnableWindow(TRUE);
        GetDlgItem(IDC_CHECKSUM_COPY)->SetFocus();
    } else {
        SetDlgItemText(IDC_CHECKSUM_STATUS, L"");
        CString text;
        if (checksum_.State() == stirling::ChecksumState::CryptoError)
            text.Format(ui::LoadW(IDS_CHECKSUM_CRYPTO_ERROR), static_cast<unsigned long>(checksum_.ErrorCode()));
        else if (checksum_.State() == stirling::ChecksumState::InvalidRange)
            text = ui::LoadW(IDS_CHECKSUM_INVALID_RANGE);
        else
            text = ui::LoadW(IDS_CHECKSUM_READ_ERROR);
        ui::MsgBox(m_hWnd, text);
    }
}

void CChecksumDlg::OnCopy() {
    CString text;
    GetDlgItemText(IDC_CHECKSUM_RESULT, text);
    if (text.IsEmpty() || checksum_.State() != stirling::ChecksumState::Complete) return;
    DWORD error = 0;
    if (!ui::PutClipboardTextW(m_hWnd, text, static_cast<size_t>(text.GetLength()), error))
        ui::MsgBoxRes(m_hWnd, IDS_ERR_CLIPBOARD_COPY);
}
void CChecksumDlg::OnStop() {
    if (checksum_.State() != stirling::ChecksumState::Running) return;
    KillTimer(kTimer);
    checksum_.Cancel();
    UpdateRunning(false);
    SetDlgItemText(IDC_CHECKSUM_STATUS, ui::LoadW(IDS_CHECKSUM_CANCELLED));
    GetDlgItem(IDOK)->SetFocus();
}
void CChecksumDlg::OnCancel() {
    KillTimer(kTimer);
    checksum_.Cancel();
    CDialog::OnCancel();
}
void CChecksumDlg::OnDestroy() {
    KillTimer(kTimer);
    checksum_.Cancel();
    CDialog::OnDestroy();
}
