// CSyncScrollDlg 実装（原 IDD_SYNC_SCROLL=193, 0x805f のクラス）。
#include "pch.h"
#include "dialog/SyncScrollDlg.h"
#include "view/StirlingView.h"
#include "doc/StirlingDoc.h"

BEGIN_MESSAGE_MAP(CSyncScrollDlg, CDialog)
    ON_BN_CLICKED(IDC_SYNC_ADD, &CSyncScrollDlg::OnAdd)
    ON_BN_CLICKED(IDC_SYNC_REMOVE, &CSyncScrollDlg::OnRemove)
    ON_BN_CLICKED(IDC_SYNC_RESET, &CSyncScrollDlg::OnResetAll)
    ON_BN_CLICKED(IDHELP, &CSyncScrollDlg::OnHelpButton)
END_MESSAGE_MAP()

CSyncScrollDlg::CSyncScrollDlg(CStirlingView* owner)
    : CDialog(IDD_SYNC_SCROLL, owner)
    , m_owner(owner) {
}

void CSyncScrollDlg::DoDataExchange(CDataExchange* pDX) {
    CDialog::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_SYNC_REGISTERED, m_listRegistered);
    DDX_Control(pDX, IDC_SYNC_CANDIDATE, m_listCandidate);
}

// 全ウィンドウを列挙し、所有ビュー以外を「現在の同期グループ所属→登録側 / 非所属→候補側」へ振分ける。
//   原 FUN_00463833: base + マスタ構築(FUN_004639a9) + リスト振分(FUN_0046385b)。
BOOL CSyncScrollDlg::OnInitDialog() {
    CDialog::OnInitDialog();
    if (m_owner == nullptr) { return TRUE; }

    // マスタエントリ構築（全ビュー, 列挙順, フレームタイトル付き）。
    std::vector<CStirlingView*> all;
    CStirlingView::EnumAllViews(all);
    m_entries.clear();
    m_entries.reserve(all.size());
    for (CStirlingView* pv : all) {
        Entry e;
        e.view = pv;
        if (CFrameWnd* pFrame = pv->GetParentFrame()) {
            pFrame->GetWindowText(e.title);
        }
        if (e.title.IsEmpty()) {
            if (CDocument* pDoc = pv->GetDocument()) { e.title = pDoc->GetTitle(); }
        }
        m_entries.push_back(e);
    }

    // 所有ビューの現在の同期相手を集合として参照。
    const std::vector<CStirlingView*>& group = m_owner->SyncGroup();

    // 列挙順に、所有ビューを除く各エントリを登録側/候補側へ振分（ItemData=索引）。
    for (int i = 0; i < static_cast<int>(m_entries.size()); ++i) {
        CStirlingView* pv = m_entries[i].view;
        if (pv == m_owner) { continue; }   // 自分は一覧に出さない
        bool synced = false;
        for (CStirlingView* g : group) {
            if (g == pv) { synced = true; break; }
        }
        CListBox& box = synced ? m_listRegistered : m_listCandidate;
        const int idx = box.AddString(m_entries[i].title);
        box.SetItemData(idx, static_cast<DWORD_PTR>(i));
    }
    m_listCandidate.SetCurSel(0);
    m_listRegistered.SetCurSel(0);
    UpdateButtons();
    return TRUE;
}

// ボタン活性: 候補が空でなければ追加可、登録が空でなければ解除/全解除可（原 FUN_00463f00）。
void CSyncScrollDlg::UpdateButtons() {
    const BOOL hasCandidate = (m_listCandidate.GetCount() > 0) ? TRUE : FALSE;
    const BOOL hasRegistered = (m_listRegistered.GetCount() > 0) ? TRUE : FALSE;
    if (CWnd* p = GetDlgItem(IDC_SYNC_ADD))    { p->EnableWindow(hasCandidate); }
    if (CWnd* p = GetDlgItem(IDC_SYNC_REMOVE)) { p->EnableWindow(hasRegistered); }
    if (CWnd* p = GetDlgItem(IDC_SYNC_RESET))  { p->EnableWindow(hasRegistered); }
}

// 選択中の1項目を from→to へ移動。to 内は ItemData（エントリ索引）の昇順を保つ。
//   原 FUN_00463b95 / FUN_00463cc7 と同一手順。
void CSyncScrollDlg::MoveSelected(CListBox& from, CListBox& to) {
    const int sel = from.GetCurSel();
    if (sel == LB_ERR) { return; }
    const int entryIndex = static_cast<int>(from.GetItemData(sel));
    from.DeleteString(sel);
    // from の選択を調整（末尾を超えたら最終要素へ）。
    int newSel = sel;
    const int fromCount = from.GetCount();
    if (fromCount - 1 <= newSel) { newSel = fromCount - 1; }
    if (newSel >= 0) { from.SetCurSel(newSel); }
    // to の挿入位置＝ItemData が entryIndex 未満の項目の直後（昇順維持）。
    int pos = 0;
    const int toCount = to.GetCount();
    while (pos < toCount &&
           static_cast<int>(to.GetItemData(pos)) < entryIndex) {
        ++pos;
    }
    const int ins = to.InsertString(pos, m_entries[entryIndex].title);
    to.SetItemData(ins, static_cast<DWORD_PTR>(entryIndex));
    to.SetCurSel(ins);
    UpdateButtons();
}

void CSyncScrollDlg::OnAdd()    { MoveSelected(m_listCandidate, m_listRegistered); }
void CSyncScrollDlg::OnRemove() { MoveSelected(m_listRegistered, m_listCandidate); }

// 全解除: 登録を空にし、候補を「所有ビュー以外の全エントリ」で再構築（原 FUN_00463dec）。
//   候補の選択は直前に選ばれていたエントリへ復元する。
void CSyncScrollDlg::OnResetAll() {
    m_listRegistered.ResetContent();
    // 復元用に現在の候補選択のエントリ索引を退避。
    int prevEntry = -1;
    const int prevSel = m_listCandidate.GetCurSel();
    if (prevSel != LB_ERR) {
        prevEntry = static_cast<int>(m_listCandidate.GetItemData(prevSel));
    }
    m_listCandidate.SetRedraw(FALSE);
    m_listCandidate.ResetContent();
    int restoreSel = 0;
    for (int i = 0; i < static_cast<int>(m_entries.size()); ++i) {
        if (m_entries[i].view == m_owner) { continue; }
        const int idx = m_listCandidate.AddString(m_entries[i].title);
        m_listCandidate.SetItemData(idx, static_cast<DWORD_PTR>(i));
        if (i == prevEntry) { restoreSel = idx; }
    }
    m_listCandidate.SetCurSel(restoreSel);
    m_listCandidate.SetRedraw(TRUE);
    m_listCandidate.Invalidate();
    UpdateButtons();
}

// ヘルプ: メインフレームへ ID_HELP を転送（原 FUN_00464075: WM_COMMAND 0xE146）。
void CSyncScrollDlg::OnHelpButton() {
    if (CWnd* pMain = AfxGetMainWnd()) {
        pMain->SendMessage(WM_COMMAND, ID_HELP, 0);
    }
}

bool CSyncScrollDlg::RegisteredContains(int entryIndex) const {
    const int n = m_listRegistered.GetCount();
    for (int i = 0; i < n; ++i) {
        if (static_cast<int>(m_listRegistered.GetItemData(i)) == entryIndex) {
            return true;
        }
    }
    return false;
}

// OK: 新グループ＝「所有ビュー＋登録リストの全ウィンドウ」を列挙順に構成し確定（原 FUN_00463f7a）。
void CSyncScrollDlg::OnOK() {
    if (m_owner != nullptr) {
        std::vector<CStirlingView*> members;
        for (int i = 0; i < static_cast<int>(m_entries.size()); ++i) {
            if (RegisteredContains(i) || m_entries[i].view == m_owner) {
                members.push_back(m_entries[i].view);
            }
        }
        m_owner->ApplySyncGroup(members);
    }
    CDialog::OnOK();
}
