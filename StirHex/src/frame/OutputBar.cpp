// CStirlingOutputBar 実装。上下ドッキング／フローティング対応の出力バー（BGREP 結果表示）。
#include "pch.h"
#include "app/UiStrings.h"   // UI文字列はリソースから
#include "app/ClipboardUtil.h"   // クリップボード転送の RAII（#47）
#include "app/ShellUtil.h"   // ui::AppendErrorReason（失敗理由の付記）
#include <afxpriv.h>   // CDockContext / LM_*（動的レイアウト）
#include "frame/OutputBar.h"
#include "view/StirlingView.h"
#include "resource.h"

BEGIN_MESSAGE_MAP(CStirlingOutputBar, CDialogBar)
    ON_WM_SIZE()
    ON_WM_NCHITTEST()
    ON_WM_NCLBUTTONDOWN()
    ON_WM_SETCURSOR()
    ON_WM_LBUTTONDOWN()
    ON_WM_LBUTTONDBLCLK()
    ON_WM_MOUSEMOVE()
    ON_WM_LBUTTONUP()
    ON_WM_CAPTURECHANGED()
    ON_WM_CONTEXTMENU()
    ON_LBN_DBLCLK(IDC_OUTPUT_LIST, &CStirlingOutputBar::OnListDblClk)
END_MESSAGE_MAP()

CStirlingOutputBar::CStirlingOutputBar() {}

BOOL CStirlingOutputBar::CreateBar(CWnd* pParent) {
    // CDialogBar テンプレート（WS_CHILD・非 WS_VISIBLE）を横向き動的バーとして生成。
    if (!Create(pParent, IDD_OUTPUT_BAR,
                CBRS_BOTTOM | CBRS_TOOLTIPS | CBRS_FLYBY | CBRS_SIZE_DYNAMIC,
                IDW_OUTPUT_BAR)) {
        return FALSE;
    }
    // テンプレート内のリストボックスを取り込む（以降 m_list で操作）。
    if (!m_list.SubclassDlgItem(IDC_OUTPUT_LIST, this)) {
        return FALSE;
    }
    ::SetWindowTextW(GetSafeHwnd(), ui::LoadW(IDS_BAR_OUTPUT));
    EnableDocking(CBRS_ALIGN_TOP | CBRS_ALIGN_BOTTOM);
    return TRUE;
}

// 横ドッキング（上/下端）はフレーム全幅＋変更可能な高さ。フローティング時は
//   動的レイアウトで保持するサイズを返す。
CSize CStirlingOutputBar::CalcFixedLayout(BOOL /*bStretch*/, BOOL bHorz) {
    if (bHorz) {
        return CSize(32767, m_dockedHeight);
    }
    return m_floatingSize;
}

CSize CStirlingOutputBar::CalcDynamicLayout(int nLength, DWORD dwMode) {
    if ((dwMode & LM_HORZDOCK) != 0) {
        return CSize(32767, m_dockedHeight);
    }
    if ((dwMode & LM_VERTDOCK) != 0) {
        return CSize(m_floatingSize.cx, 32767);
    }

    if (nLength > 0) {
        if ((dwMode & LM_LENGTHY) != 0) {
            m_floatingSize.cy = max(kMinHeight, nLength);
        } else {
            m_floatingSize.cx = max(kMinWidth, nLength);
        }
    }
    return m_floatingSize;
}

// バーのクライアント全域へリストボックスを追従させる（原の出力リストと同じ挙動）。
//   WM_SIZE の cx/cy はドッキング直後に旧サイズのことがあるため、実クライアント
//   矩形（バー全幅）を GetClientRect で取り直して合わせる。
void CStirlingOutputBar::OnSize(UINT nType, int cx, int cy) {
    CDialogBar::OnSize(nType, cx, cy);
    if (IsFloating() && cx >= kMinWidth && cy >= kMinHeight) {
        m_floatingSize = CSize(cx, cy);
    }
    FitList();
}

void CStirlingOutputBar::FitList() {
    if (m_list.GetSafeHwnd() == nullptr) {
        return;
    }
    CRect rc;
    GetClientRect(&rc);
    // ドック時は全幅センチネル(32767)がバーのクライアント幅へ残る場合があるため、
    // 実ドックバーの幅へクランプする。
    if (!IsFloating()) {
        if (CWnd* pDockBar = GetParent()) {
            CRect dockClient;
            pDockBar->GetClientRect(&dockClient);
            if (dockClient.Width() > 0 && rc.Width() > dockClient.Width()) {
                // バーはドックバー境界から左右2pxずつ外側へ張り出すため、その4pxも含める。
                rc.right = rc.left + dockClient.Width() + 4;
            }
        }
    }
    rc.left += kLeftMargin;
    rc.top += kTopMargin;
    rc.right -= kRightMargin;
    rc.bottom -= kBottomMargin;
    if (rc.right < rc.left) rc.right = rc.left;
    if (rc.bottom < rc.top) rc.bottom = rc.top;
    m_list.MoveWindow(&rc);
}

int CStirlingOutputBar::DockPosition() const {
    if (IsFloating()) return 2;
    if (CWnd* pDockBar = GetParent()) {
        const int id = ::GetDlgCtrlID(pDockBar->GetSafeHwnd());
        if (id == AFX_IDW_DOCKBAR_BOTTOM) return 0;
        if (id == AFX_IDW_DOCKBAR_TOP) return 1;
    }
    // ドッキング遷移中など親IDが確定していない場合のフォールバック。
    if ((m_dwStyle & CBRS_ALIGN_BOTTOM) != 0) return 0;
    if ((m_dwStyle & CBRS_ALIGN_TOP) != 0) return 1;
    return 2;
}

bool CStirlingOutputBar::IsDockResizeHit(UINT nHitTest) const {
    const int pos = DockPosition();
    return (pos == 0 && nHitTest == HTTOP) || (pos == 1 && nHitTest == HTBOTTOM);
}

LRESULT CStirlingOutputBar::OnNcHitTest(CPoint point) {
    if (!IsFloating()) {
        CRect window;
        GetWindowRect(&window);
        const int pos = DockPosition();
        if (pos == 0 && point.y < window.top + kResizeBorder) return HTTOP;
        if (pos == 1 && point.y >= window.bottom - kResizeBorder) return HTBOTTOM;
    }
    return CDialogBar::OnNcHitTest(point);
}

void CStirlingOutputBar::BeginDockedResize() {
    m_resizeDockPosition = DockPosition();
    if (m_resizeDockPosition == 2) return;
    CPoint cursor;
    ::GetCursorPos(&cursor);
    m_resizeStartY = cursor.y;
    m_resizeStartHeight = m_dockedHeight;
    m_resizingDockedHeight = true;
    SetCapture();
}

void CStirlingOutputBar::PrepareFloatingSizeFromDock() {
    if (IsFloating()) return;
    if (CWnd* pDockBar = GetParent()) {
        CRect dockRect;
        pDockBar->GetWindowRect(&dockRect);
        // ドックバーの左右境界（各2px）まで含めると原版のバー実寸と一致する。
        m_floatingSize.cx = max(kInitialWidth, dockRect.Width() + 4);
    }
    m_floatingSize.cy = m_dockedHeight;
}

void CStirlingOutputBar::OnNcLButtonDown(UINT nHitTest, CPoint point) {
    if (IsDockResizeHit(nHitTest)) {
        BeginDockedResize();
        return;
    }
    CDialogBar::OnNcLButtonDown(nHitTest, point);
}

BOOL CStirlingOutputBar::OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message) {
    if (IsDockResizeHit(nHitTest) || m_resizingDockedHeight) {
        ::SetCursor(AfxGetApp()->LoadStandardCursor(IDC_SIZENS));
        return TRUE;
    }
    return CDialogBar::OnSetCursor(pWnd, nHitTest, message);
}

void CStirlingOutputBar::OnLButtonDown(UINT nFlags, CPoint point) {
    PrepareFloatingSizeFromDock();
    CDialogBar::OnLButtonDown(nFlags, point);
}

void CStirlingOutputBar::OnLButtonDblClk(UINT nFlags, CPoint point) {
    PrepareFloatingSizeFromDock();
    CDialogBar::OnLButtonDblClk(nFlags, point);
}

void CStirlingOutputBar::OnMouseMove(UINT nFlags, CPoint point) {
    if (!m_resizingDockedHeight) {
        CDialogBar::OnMouseMove(nFlags, point);
        return;
    }
    CPoint cursor;
    ::GetCursorPos(&cursor);
    const int delta = cursor.y - m_resizeStartY;
    int height = m_resizeStartHeight + ((m_resizeDockPosition == 1) ? delta : -delta);
    height = min(2000, max(kMinHeight, height));
    if (height != m_dockedHeight) {
        m_dockedHeight = height;
        if (CFrameWnd* frame = DYNAMIC_DOWNCAST(CFrameWnd, AfxGetMainWnd())) {
            frame->RecalcLayout();
        }
        FitList();
    }
}

void CStirlingOutputBar::OnLButtonUp(UINT nFlags, CPoint point) {
    if (m_resizingDockedHeight) {
        m_resizingDockedHeight = false;
        m_resizeDockPosition = -1;
        if (::GetCapture() == GetSafeHwnd()) ReleaseCapture();
        return;
    }
    CDialogBar::OnLButtonUp(nFlags, point);
}

void CStirlingOutputBar::OnCaptureChanged(CWnd* pWnd) {
    m_resizingDockedHeight = false;
    m_resizeDockPosition = -1;
    CDialogBar::OnCaptureChanged(pWnd);
}

int CStirlingOutputBar::AppendLine(const CStringW& text, const Hit& hit) {
    const int idx = (int)m_hits.size();
    m_hits.push_back(hit);
    const int lbIndex = m_list.AddString(text);
    if (lbIndex >= 0) {
        m_list.SetItemData(lbIndex, (DWORD_PTR)idx);
    }
    // 横スクロールバーは付けない（原の出力リストは縦スクロールのみ。長い行は末尾が
    //   切れるが原と同じ挙動。縦スクロールは LBS_DISABLENOSCROLL で常時表示）。
    return lbIndex;
}

void CStirlingOutputBar::AddResult(const CStringW& path, stirling::FileOffset offset) {
    CStringW line;
    line.Format(L"%s : %08llX", path.GetString(), static_cast<long long>(offset));   // 原 DAT_004b61ac
    Hit hit;
    hit.path = path;
    hit.offset = offset;
    hit.hasLoc = true;
    AppendLine(line, hit);
}

void CStirlingOutputBar::AddMessage(const CStringW& text) {
    Hit hit;
    hit.hasLoc = false;
    AppendLine(text, hit);
}

void CStirlingOutputBar::ClearResults() {
    m_hits.clear();
    if (m_list.GetSafeHwnd() != nullptr) {
        m_list.ResetContent();
    }
}

// 原 FUN_00426f09: ファイルを開き（既に開いていれば活性化）、ビューを該当
//   オフセットへ移動する。offset がサイズ超過でも GotoPos 側でクランプされる。
void CStirlingOutputBar::OpenHit(int index) {
    if (index < 0 || index >= (int)m_hits.size()) {
        return;
    }
    const Hit& hit = m_hits[index];
    if (!hit.hasLoc || hit.path.IsEmpty()) {
        ::MessageBeep(0);
        return;
    }
    CWinApp* pApp = AfxGetApp();
    if (pApp == nullptr) {
        return;
    }
    CDocument* pDoc = pApp->OpenDocumentFile(hit.path);
    if (pDoc == nullptr) {
        return;   // OpenDocumentFile 失敗時は既にメッセージ表示済み
    }
    POSITION pos = pDoc->GetFirstViewPosition();
    while (pos != nullptr) {
        CView* pView = pDoc->GetNextView(pos);
        CStirlingView* pSV = DYNAMIC_DOWNCAST(CStirlingView, pView);
        if (pSV != nullptr) {
            pSV->GotoPos(hit.offset);
            break;
        }
    }
}

void CStirlingOutputBar::TagJump() {
    if (m_list.GetSafeHwnd() == nullptr) {
        return;
    }
    const int sel = m_list.GetCurSel();
    if (sel == LB_ERR) {
        ::MessageBeep(0);
        return;
    }
    OpenHit((int)m_list.GetItemData(sel));
}

void CStirlingOutputBar::OnListDblClk() {
    TagJump();
}

// 全結果行を CR/LF 区切りのテキストとしてクリップボードへ（CF_UNICODETEXT, 0x80f7）。
//   内容は BGREP 結果行＝ファイルパス＋オフセットで wide 層（設計メモ §6.5）。
void CStirlingOutputBar::CopyToClipboard() {
    if (m_list.GetSafeHwnd() == nullptr) {
        return;
    }
    const int count = m_list.GetCount();
    if (count <= 0) {
        return;
    }
    // 結果が数万行に及ぶことがあるため、連結前に必要量を見積もって確保する。
    int total = 0;
    for (int i = 0; i < count; ++i) {
        total += m_list.GetTextLen(i) + 2;   // 各行 + CR/LF
    }
    CStringW all;
    all.Preallocate(total);
    for (int i = 0; i < count; ++i) {
        CStringW line;
        m_list.GetText(i, line);
        all += line;
        all += L"\r\n";
    }
    DWORD error = ERROR_SUCCESS;
    if (!ui::PutClipboardTextW(GetSafeHwnd(), all.GetString(),
                               static_cast<size_t>(all.GetLength()), error)) {
        // 他プロセスがクリップボードをロックしている等。無言で中断せず理由を提示する。
        ui::MsgBox(GetSafeHwnd(),
                   ui::AppendErrorReason(ui::LoadW(IDS_ERR_CLIPBOARD_COPY), error));
    }
}

// リスト上の右クリックでアウトプット用ポップアップ（IDR_OUTPUT_POPUP=177）を表示。
//   原 FUN_0042c953/FUN_0042ca09: オーナーをこのバー自身にして CFrameWnd の自動
//   コマンドUI更新（＝「非表示」へのチェック付与）を回避し、項目状態は手動設定する。
//   手動更新はタグジャンプ(0x80ea)の活性のみ（選択行がジャンプ可能な時だけ有効）。
//   コピー/クリア/非表示はテンプレート既定（有効・チェック無し）のまま。
//   選択コマンドは OnCommand でメインフレームへ委譲する。
void CStirlingOutputBar::OnContextMenu(CWnd* /*pWnd*/, CPoint point) {
    CMenu menu;
    if (!menu.LoadMenu(IDR_OUTPUT_POPUP)) {
        return;
    }
    CMenu* pPopup = menu.GetSubMenu(0);
    if (pPopup == nullptr) {
        return;
    }
    // タグジャンプの活性: 選択行がジャンプ可能なヒット（hasLoc）の時のみ有効。
    bool canJump = false;
    if (m_list.GetSafeHwnd() != nullptr) {
        const int sel = m_list.GetCurSel();
        if (sel != LB_ERR) {
            const int idx = (int)m_list.GetItemData(sel);
            if (idx >= 0 && idx < (int)m_hits.size() && m_hits[idx].hasLoc) {
                canJump = true;
            }
        }
    }
    pPopup->EnableMenuItem(ID_TAG_JUMP, MF_BYCOMMAND | (canJump ? MF_ENABLED : MF_GRAYED));

    if (point.x == -1 && point.y == -1) {   // キーボード起動時はバー中央へ
        CRect rc;
        GetWindowRect(&rc);
        point = rc.CenterPoint();
    }
    // オーナー＝このバー（原と同じ）。自動コマンドUI更新を走らせない。
    pPopup->TrackPopupMenu(TPM_LEFTALIGN | TPM_RIGHTBUTTON, point.x, point.y, this);
}

// ポップアップメニュー選択（オーナー＝このバー）のコマンドをメインフレームへ委譲。
//   メニュー由来コマンドは lParam==0 かつ 通知コード0。子コントロールの通知は素通しする。
BOOL CStirlingOutputBar::OnCommand(WPARAM wParam, LPARAM lParam) {
    if (lParam == 0 && HIWORD(wParam) == 0) {
        if (CWnd* pMain = AfxGetMainWnd()) {
            pMain->SendMessage(WM_COMMAND, wParam, lParam);
            return TRUE;
        }
    }
    return CDialogBar::OnCommand(wParam, lParam);
}
