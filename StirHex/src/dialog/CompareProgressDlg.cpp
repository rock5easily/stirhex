// CCompareProgressDlg 実装（データ比較の進捗。Issue #307）。
#include "pch.h"
#include "dialog/CompareProgressDlg.h"

#include <utility>

namespace {
constexpr UINT_PTR kStepTimer = 1;
constexpr UINT kStepInterval = 10;
// 1 回のタイマー処理で比べる時間の上限（ミリ秒）と、1 回の Step で比べるバイト数。
constexpr ULONGLONG kStepMillis = 15;
constexpr stirling::FileOffset kStepBytes = 4 * 1024 * 1024;
constexpr int kProgressRange = 1000;
}  // namespace

BEGIN_MESSAGE_MAP(CCompareProgressDlg, CDialog)
    ON_WM_TIMER()
    ON_WM_DESTROY()
END_MESSAGE_MAP()

CCompareProgressDlg::CCompareProgressDlg(stirling::DiffScan& scan,
                                         std::function<bool()> canContinue, CWnd* pParent)
    : CDialog(IDD_COMPARE_PROGRESS, pParent)
    , m_scan(scan)
    , m_canContinue(std::move(canContinue)) {
}

BOOL CCompareProgressDlg::OnInitDialog() {
    CDialog::OnInitDialog();
    SendDlgItemMessage(IDC_COMPARE_PROGRESS, PBM_SETRANGE32, 0, kProgressRange);
    UpdateProgress();
    if (SetTimer(kStepTimer, kStepInterval, nullptr) == 0) {
        // タイマーを張れない場合は、中止できないが最後まで比べる（結果は失わない）。
        CWaitCursor wait;
        while (m_scan.Running()) {
            m_scan.Step(kStepBytes);
        }
        EndDialog(IDOK);
    }
    return TRUE;
}

void CCompareProgressDlg::OnTimer(UINT_PTR nIDEvent) {
    if (nIDEvent == kStepTimer) {
        RunSteps();
        return;
    }
    CDialog::OnTimer(nIDEvent);
}

void CCompareProgressDlg::RunSteps() {
    // 比べる前に確かめ、閉じられた文書や変更後のデータを読まない。
    if (m_canContinue && !m_canContinue()) {
        KillTimer(kStepTimer);
        m_scan.Cancel();
        EndDialog(IDCANCEL);
        return;
    }
    const ULONGLONG until = ::GetTickCount64() + kStepMillis;
    do {
        m_scan.Step(kStepBytes);
    } while (m_scan.Running() && ::GetTickCount64() < until);

    UpdateProgress();
    if (!m_scan.Running()) {
        KillTimer(kStepTimer);
        EndDialog(IDOK);
    }
}

void CCompareProgressDlg::UpdateProgress() {
    const stirling::FileOffset total = m_scan.ScanTotal();
    const int pos = (total > 0)
        ? static_cast<int>(m_scan.Scanned() * kProgressRange / total)
        : kProgressRange;
    SendDlgItemMessage(IDC_COMPARE_PROGRESS, PBM_SETPOS, pos, 0);
}

void CCompareProgressDlg::OnCancel() {
    KillTimer(kStepTimer);
    m_scan.Cancel();
    CDialog::OnCancel();
}

void CCompareProgressDlg::OnDestroy() {
    KillTimer(kStepTimer);
    CDialog::OnDestroy();
}
