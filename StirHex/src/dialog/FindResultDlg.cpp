// CFindResultDlg 実装（検索結果一覧。Issue #236）。
#include "pch.h"
#include "app/UiStrings.h"
#include "dialog/FindDlg.h"
#include "dialog/FindResultDlg.h"
#include "doc/StirlingDoc.h"
#include "view/StirlingView.h"

#include <algorithm>

namespace {
// 1回のタイマー処理で走査する時間の上限（ミリ秒）と、1回の Step で調べる先頭候補の数。
constexpr ULONGLONG kStepMillis = 15;
constexpr stirling::FileOffset kStepPositions = 256 * 1024;
constexpr UINT kStepInterval = 10;
constexpr UINT kWatchInterval = 300;   // 検索後の文書変更を監視する間隔
}

BEGIN_MESSAGE_MAP(CFindResultDlg, CDialog)
    ON_WM_DESTROY()
    ON_WM_TIMER()
    ON_BN_CLICKED(IDC_FINDRESULT_RESEARCH, &CFindResultDlg::OnResearch)
    ON_BN_CLICKED(IDC_FINDRESULT_STOP, &CFindResultDlg::OnStop)
    ON_NOTIFY(LVN_GETDISPINFO, IDC_FINDRESULT_LIST, &CFindResultDlg::OnGetDispInfo)
    ON_NOTIFY(NM_DBLCLK, IDC_FINDRESULT_LIST, &CFindResultDlg::OnDblclkList)
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_FINDRESULT_LIST, &CFindResultDlg::OnItemChanged)
END_MESSAGE_MAP()

CFindResultDlg::CFindResultDlg(CStirlingView* view)
    : CDialog(IDD_FIND_RESULT)
    , m_view(view) {
}

BOOL CFindResultDlg::CreateModeless(CWnd* parent) {
    return Create(IDD_FIND_RESULT, parent);
}

void CFindResultDlg::DoDataExchange(CDataExchange* pDX) {
    CDialog::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_FINDRESULT_LIST, m_list);
}

BOOL CFindResultDlg::OnInitDialog() {
    CDialog::OnInitDialog();
    m_list.SetExtendedStyle(m_list.GetExtendedStyle() | LVS_EX_FULLROWSELECT);
    m_list.InsertColumn(0, ui::LoadW(IDS_FINDRESULT_COL_ADDRESS), LVCFMT_LEFT, 90);
    m_list.InsertColumn(1, ui::LoadW(IDS_FINDRESULT_COL_DATA), LVCFMT_LEFT, 180);
    UpdateButtons();
    return TRUE;
}

bool CFindResultDlg::ViewAlive() const {
    return m_view != nullptr && ::IsWindow(m_view->GetSafeHwnd()) && m_view->GetDocument() != nullptr;
}

void CFindResultDlg::StartSearch(const Condition& condition) {
    KillTimer(kStepTimer);
    m_finder.Cancel();
    m_condition = condition;
    m_previews.clear();
    m_previewCount = 0;
    m_outdated = false;
    m_timerError = false;
    m_list.SetItemCount(0);
    if (!ViewAlive()) { return; }

    CStirlingDoc* pDoc = m_view->GetDocument();
    UpdateTitle();
    UpdateCondition();

    m_dataSeq = pDoc->DataChangeSeq();
    m_finder.Start(pDoc->Blocks(), m_condition.pattern, m_condition.lo, m_condition.hi);
    // タイマーを張れない場合は検索を始めず、状態欄にエラーを表示する。
    //   ここでメッセージボックスを出すと入れ子のメッセージループの間に文書やビューが閉じられ、
    //   戻った後に破棄済みのこのダイアログを参照するおそれがあるため、モーダル表示は使わない。
    //   監視タイマー（再設定しても同じ ID を置き換えるだけ）も張れなければ、変更を検知できないため同様に扱う。
    const bool watchOk = SetTimer(kWatchTimer, kWatchInterval, nullptr) != 0;
    if (!watchOk || (m_finder.Running() && SetTimer(kStepTimer, kStepInterval, nullptr) == 0)) {
        m_finder.Cancel();
        m_timerError = true;
    }
    if (m_finder.Running()) {
        UpdateStatus();
        UpdateButtons();
    } else {
        FinishSearch();
    }
    m_list.SetFocus();
}

void CFindResultDlg::OnTimer(UINT_PTR nIDEvent) {
    if (nIDEvent == kStepTimer) {
        RunSteps();
        return;
    }
    if (nIDEvent == kWatchTimer) {
        if (!ViewAlive()) { return; }
        if (!m_outdated && m_view->GetDocument()->DataChangeSeq() != m_dataSeq) {
            MarkOutdated();
        }
        UpdateTitle();   // 名前を付けて保存などで文書名が変わった場合に追従する
        return;
    }
    CDialog::OnTimer(nIDEvent);
}

void CFindResultDlg::RunSteps() {
    if (!m_finder.Running()) {
        KillTimer(kStepTimer);
        return;
    }
    if (!ViewAlive()) {
        KillTimer(kStepTimer);
        m_finder.Cancel();
        return;
    }
    // 走査の前に文書の変更を確かめ、変更後のデータを古い条件で読まない。
    CStirlingDoc* pDoc = m_view->GetDocument();
    if (pDoc->DataChangeSeq() != m_dataSeq) {
        MarkOutdated();
        return;
    }
    const ULONGLONG until = ::GetTickCount64() + kStepMillis;
    do {
        m_finder.Step(kStepPositions);
    } while (m_finder.Running() && ::GetTickCount64() < until);

    CollectPreviews();
    m_list.SetItemCountEx(static_cast<int>(m_previewCount), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    if (m_finder.Running()) {
        UpdateStatus();
    } else {
        FinishSearch();
    }
}

void CFindResultDlg::CollectPreviews() {
    if (!ViewAlive()) { return; }
    CStirlingDoc* pDoc = m_view->GetDocument();
    const std::vector<stirling::FileOffset>& hits = m_finder.Hits();
    const size_t length = (std::min)(m_finder.PatternSize(), kPreviewBytes);
    m_previews.resize(hits.size() * kPreviewBytes, 0);
    for (; m_previewCount < hits.size(); ++m_previewCount) {
        unsigned char* dst = m_previews.data() + m_previewCount * kPreviewBytes;
        pDoc->ReadInto(hits[m_previewCount], static_cast<stirling::FileOffset>(length), dst);
    }
}

void CFindResultDlg::FinishSearch() {
    KillTimer(kStepTimer);
    // 最後の Step の後に文書が変わっていたら、変更後のデータを一覧の表示用に控えない。
    if (!m_outdated && ViewAlive() && m_view->GetDocument()->DataChangeSeq() != m_dataSeq) {
        MarkOutdated();
        return;
    }
    if (!m_outdated) {
        CollectPreviews();
        m_list.SetItemCountEx(static_cast<int>(m_previewCount), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
        if (m_previewCount > 0 && SelectedIndex() < 0) {
            m_list.SetItemState(0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
    }
    UpdateStatus();
    UpdateButtons();
}

void CFindResultDlg::MarkOutdated() {
    KillTimer(kStepTimer);
    m_finder.Cancel();
    m_outdated = true;
    m_list.Invalidate(FALSE);
    UpdateStatus();
    UpdateButtons();
}

void CFindResultDlg::UpdateTitle() {
    if (!ViewAlive()) { return; }
    CString title;
    title.Format(ui::LoadW(IDS_FINDRESULT_TITLE), static_cast<LPCWSTR>(m_view->GetDocument()->GetTitle()));
    CString current;
    GetWindowText(current);
    if (current != title) { SetWindowText(title); }
}

void CFindResultDlg::UpdateCondition() {
    UINT rangeId = IDS_FINDRESULT_RANGE_CURSOR;
    if (m_condition.rangeMode == CFindDlg::kWholeData) { rangeId = IDS_FINDRESULT_RANGE_ALL; }
    if (m_condition.rangeMode == CFindDlg::kSelection) { rangeId = IDS_FINDRESULT_RANGE_SEL; }
    CString text;
    text.Format(ui::LoadW(m_condition.isHex ? IDS_FINDRESULT_COND_HEX : IDS_FINDRESULT_COND_TEXT),
                static_cast<LPCWSTR>(m_condition.display), static_cast<LPCWSTR>(ui::LoadW(rangeId)));
    SetDlgItemText(IDC_FINDRESULT_CONDITION, text);
}

void CFindResultDlg::UpdateStatus() {
    const unsigned long long count = static_cast<unsigned long long>(m_finder.Hits().size());
    CString text;
    if (m_timerError) {
        text = ui::LoadW(IDS_FINDRESULT_TIMER_ERROR);
    } else if (m_outdated) {
        text = ui::LoadW(IDS_FINDRESULT_CHANGED);
    } else {
        switch (m_finder.State()) {
        case stirling::FindAllState::Running:
            text.Format(ui::LoadW(IDS_FINDRESULT_RUNNING), count);
            break;
        case stirling::FindAllState::Complete:
            if (count == 0) {
                text = ui::LoadW(IDS_FINDRESULT_NONE);
            } else {
                text.Format(ui::LoadW(IDS_FINDRESULT_COMPLETE), count);
            }
            break;
        case stirling::FindAllState::Truncated:
            text.Format(ui::LoadW(IDS_FINDRESULT_TRUNCATED), count);
            break;
        case stirling::FindAllState::Cancelled:
            text.Format(ui::LoadW(IDS_FINDRESULT_CANCELLED), count);
            break;
        case stirling::FindAllState::ReadError:
            text = ui::LoadW(IDS_FINDRESULT_READ_ERROR);
            break;
        case stirling::FindAllState::Idle:
            break;
        }
    }
    SetDlgItemText(IDC_FINDRESULT_STATUS, text);
}

void CFindResultDlg::UpdateButtons() {
    const bool running = m_finder.Running();
    const bool canJump = !m_outdated && !m_timerError && SelectedIndex() >= 0;
    const HWND focus = ::GetFocus();
    auto enable = [this](int id, bool on) {
        if (CWnd* w = GetDlgItem(id)) { w->EnableWindow(on ? TRUE : FALSE); }
    };
    enable(IDOK, canJump);
    enable(IDC_FINDRESULT_RESEARCH, !running);
    enable(IDC_FINDRESULT_STOP, running);
    // 無効にしたボタンにフォーカスが残らないよう、一覧へ戻す。
    if (focus != nullptr && !::IsWindowEnabled(focus) && ::IsChild(GetSafeHwnd(), focus)) {
        m_list.SetFocus();
    }
}

int CFindResultDlg::SelectedIndex() const {
    if (m_list.GetSafeHwnd() == nullptr) { return -1; }
    const int index = m_list.GetNextItem(-1, LVNI_SELECTED);
    return (index >= 0 && static_cast<size_t>(index) < m_previewCount) ? index : -1;
}

void CFindResultDlg::OnOK() {
    const int index = SelectedIndex();
    if (m_outdated || m_timerError || index < 0 || !ViewAlive()) {
        ::MessageBeep(0);
        return;
    }
    // 監視タイマーより先に編集された場合も、ずれた位置を選択しない。
    if (m_view->GetDocument()->DataChangeSeq() != m_dataSeq) {
        MarkOutdated();
        ::MessageBeep(0);
        return;
    }
    m_view->SelectFoundRange(m_finder.Hits()[static_cast<size_t>(index)],
                             static_cast<stirling::FileOffset>(m_finder.PatternSize()));
}

void CFindResultDlg::OnCancel() {
    DestroyWindow();
}

void CFindResultDlg::OnResearch() {
    if (m_finder.Running() || !ViewAlive()) { return; }
    // 同じ検索データと範囲の種類で、現在の文書に合わせて範囲を決め直す。
    const stirling::FileOffset total = m_view->GetDocument()->GetTotalLength();
    Condition next = m_condition;
    next.lo = (std::min)(m_condition.lo, total);
    next.hi = (m_condition.rangeMode == CFindDlg::kSelection) ? (std::min)(m_condition.hi, total) : total;
    StartSearch(next);
}

void CFindResultDlg::OnStop() {
    if (!m_finder.Running()) { return; }
    m_finder.Cancel();
    FinishSearch();
    m_list.SetFocus();
}

void CFindResultDlg::OnGetDispInfo(NMHDR* pNMHDR, LRESULT* pResult) {
    *pResult = 0;
    NMLVDISPINFO* info = reinterpret_cast<NMLVDISPINFO*>(pNMHDR);
    if ((info->item.mask & LVIF_TEXT) == 0 || info->item.pszText == nullptr || info->item.cchTextMax <= 0) {
        return;
    }
    info->item.pszText[0] = L'\0';
    const int index = info->item.iItem;
    if (index < 0 || static_cast<size_t>(index) >= m_previewCount) { return; }

    CStringW text;
    if (info->item.iSubItem == 0) {
        if (ViewAlive()) {
            text = m_view->FormatListAddress(m_finder.Hits()[static_cast<size_t>(index)]);
        }
    } else {
        const size_t size = m_finder.PatternSize();
        const size_t shown = (std::min)(size, kPreviewBytes);
        const unsigned char* bytes = m_previews.data() + static_cast<size_t>(index) * kPreviewBytes;
        for (size_t i = 0; i < shown; ++i) {
            CStringW cell;
            cell.Format((i == 0) ? L"%02X" : L" %02X", bytes[i]);
            text += cell;
        }
        if (size > shown) { text += L" \x2026"; }   // 表示しきれないバイトがあることを示す
    }
    wcsncpy_s(info->item.pszText, static_cast<size_t>(info->item.cchTextMax), text, _TRUNCATE);
}

void CFindResultDlg::OnDblclkList(NMHDR* /*pNMHDR*/, LRESULT* pResult) {
    *pResult = 0;
    OnOK();
}

void CFindResultDlg::OnItemChanged(NMHDR* /*pNMHDR*/, LRESULT* pResult) {
    *pResult = 0;
    UpdateButtons();
}

void CFindResultDlg::OnViewDestroyed() {
    m_view = nullptr;
    const HWND hDlg = GetSafeHwnd();
    if (hDlg != nullptr && ::IsWindow(hDlg)) {
        ::DestroyWindow(hDlg);   // PostNcDestroy で delete this。以降メンバーへ触れない
    }
}

void CFindResultDlg::OnDestroy() {
    KillTimer(kStepTimer);
    KillTimer(kWatchTimer);
    m_finder.Cancel();
    if (ViewAlive()) {
        m_view->OnFindResultDlgClosed(this);
    }
    m_view = nullptr;
    CDialog::OnDestroy();
}

void CFindResultDlg::PostNcDestroy() {
    CDialog::PostNcDestroy();
    delete this;
}
