// CBitImageBar / CBitImageWnd 実装。文書バイト列を 128px 幅の 8bpp パレット画像で可視化する。
//   原 CMainFrame+0x2b8（ビットイメージ窓）の忠実移植。DIB は CreateDIBSection で確保し、
//   各ピクセル=元バイト値（=パレット索引）。パレットが色を与える（原 FUN_00436640 の既定色）。
#include "pch.h"
#include "app/UiStrings.h"   // UI文字列はリソースから
#include "frame/BitImageBar.h"
#include "frame/MainFrame.h"
#include "doc/StirlingDoc.h"
#include "app/StirlingSettings.h"
#include "resource.h"
#include <afxpriv.h>   // CDockContext::StartDrag（原ペイン上部ドラッグの再現）

// ===========================================================================
// CBitImageWnd
// ===========================================================================
BEGIN_MESSAGE_MAP(CBitImageWnd, CWnd)
    ON_WM_PAINT()
    ON_WM_ERASEBKGND()
    ON_WM_SIZE()
    ON_WM_VSCROLL()
    ON_WM_MOUSEMOVE()
    ON_WM_MOUSELEAVE()
    ON_WM_CONTEXTMENU()
END_MESSAGE_MAP()

CBitImageWnd::CBitImageWnd()
    : m_pBits(nullptr), m_hOldBitmap(nullptr), m_imgHeight(0), m_dataLen(0),
      m_scrollPos(0), m_bg(RGB(255, 255, 255)), m_tracking(false) {}

CBitImageWnd::~CBitImageWnd() {
    ReleaseDib();
}

BOOL CBitImageWnd::Create(CWnd* pParent) {
    // 独自クラス（背景ブラシ無し＝OnPaint で塗る。再描画で全域無効化）。
    LPCTSTR cls = AfxRegisterWndClass(CS_HREDRAW | CS_VREDRAW,
                                      ::LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr);
    return CreateEx(0, cls, _T(""), WS_CHILD | WS_VISIBLE | WS_VSCROLL,
                    CRect(0, 0, 10, 10), pParent, 0);
}

void CBitImageWnd::ReleaseDib() {
    if (m_memDC.GetSafeHdc() != nullptr) {
        // DIB を削除する前に、DC が元々持っていたビットマップへ必ず戻す
        //   （選択中のオブジェクトは DeleteObject が失敗し、リークになる）。
        if (m_hOldBitmap != nullptr) {
            ::SelectObject(m_memDC.GetSafeHdc(), m_hOldBitmap);
            m_hOldBitmap = nullptr;
        }
        m_memDC.DeleteDC();
    }
    m_dib.Reset();
    m_pBits = nullptr;
    m_imgHeight = 0;
    m_dataLen = 0;
}

// パレット決定（原 FUN_00436640）。COLORREF 0x00BBGGRR を RGBQUAD へ。戻り値=使用背景色。
//   反映OFF（既定）: 0x00=白 / 0x01-0x1F=シアン / 0x20-0x7F=赤 / 0x80-0xFF=黒、背景=白。
//   反映ON: データ文字色＋強調表示コード（BuildByteColorTable）、背景=データ背景色。
COLORREF CBitImageWnd::FillPalette(RGBQUAD* colors, const CStirlingSettings* s) const {
    auto fromColorRef = [](RGBQUAD& q, COLORREF c) {
        q.rgbRed = GetRValue(c); q.rgbGreen = GetGValue(c); q.rgbBlue = GetBValue(c);
        q.rgbReserved = 0;
    };
    if (s != nullptr && s->bimgReflect) {
        COLORREF table[256];
        s->BuildByteColorTable(table);
        for (int i = 0; i < 0x100; ++i) { fromColorRef(colors[i], table[i]); }
        return s->dataBack;   // 背景（余白/最終行末尾）はデータ背景色
    }
    auto set = [](RGBQUAD& q, BYTE r, BYTE g, BYTE b) {
        q.rgbRed = r; q.rgbGreen = g; q.rgbBlue = b; q.rgbReserved = 0;
    };
    set(colors[0], 255, 255, 255);                 // 0x00: 白
    for (int i = 0x01; i < 0x20; ++i) set(colors[i], 0, 255, 255);   // シアン
    for (int i = 0x20; i < 0x80; ++i) set(colors[i], 255, 0, 0);     // 赤
    for (int i = 0x80; i < 0x100; ++i) set(colors[i], 0, 0, 0);      // 黒
    return RGB(255, 255, 255);                      // 背景=白
}

void CBitImageWnd::Clear() {
    ReleaseDib();
    m_scrollPos = 0;
    m_bg = RGB(255, 255, 255);
    if (GetSafeHwnd() != nullptr) {
        UpdateScrollInfo();
        Invalidate();
    }
}

void CBitImageWnd::BuildFromDoc(CStirlingDoc* pDoc) {
    ReleaseDib();
    m_scrollPos = 0;
    m_bg = RGB(255, 255, 255);

    const long long total = (pDoc != nullptr) ? pDoc->GetTotalLength() : 0;
    if (total <= 0) {
        if (GetSafeHwnd() != nullptr) { UpdateScrollInfo(); Invalidate(); }
        return;
    }
    const int len = (total > 0x7fffffff) ? 0x7fffffff : static_cast<int>(total);
    const int rows = (len + kImageWidth - 1) / kImageWidth;   // ceil(len/128)

    // 8bpp・256色・トップダウン（biHeight 負）DIB。ストライド=128（4の倍数=無パディング）。
    struct { BITMAPINFOHEADER h; RGBQUAD colors[256]; } bmi = {0};
    bmi.h.biSize = sizeof(BITMAPINFOHEADER);
    bmi.h.biWidth = kImageWidth;
    bmi.h.biHeight = -rows;                 // トップダウン（先頭バイト=最上行）
    bmi.h.biPlanes = 1;
    bmi.h.biBitCount = 8;
    bmi.h.biCompression = BI_RGB;
    bmi.h.biClrUsed = 256;
    bmi.h.biClrImportant = 256;
    // 反映ON時はデータ文字色＋強調表示コード、背景=データ背景色。OFF時は既定パレット/白。
    const CStirlingSettings* s = (pDoc != nullptr) ? &pDoc->Settings() : nullptr;
    m_bg = FillPalette(bmi.colors, s);

    HDC hScreen = ::GetDC(nullptr);
    m_dib.Reset(::CreateDIBSection(hScreen, reinterpret_cast<BITMAPINFO*>(&bmi),
                                   DIB_RGB_COLORS, &m_pBits, nullptr, 0));
    ::ReleaseDC(nullptr, hScreen);

    if (!m_dib.Valid() || m_pBits == nullptr) {
        m_dib.Reset();   // ビット列だけ取れなかった場合もセクションを解放する
        m_pBits = nullptr;
        ui::MsgBox(GetSafeHwnd(), ui::LoadW(IDS_BITIMAGE_FAILED),
                   MB_ICONWARNING | MB_OK);
        if (GetSafeHwnd() != nullptr) { UpdateScrollInfo(); Invalidate(); }
        return;
    }

    // ピクセル=元バイト値。末尾の端数行以降は 0x00（=パレットで白＝背景）で埋める。
    const size_t bitsSize = static_cast<size_t>(kImageWidth) * rows;
    // CreateDIBSection は 8bpp・幅 kImageWidth(4の倍数)・高さ rows で確保済みのため
    // bitsSize ちょうどが有効範囲。解析器は確保サイズを追えず誤検知する（C6386）。
    #pragma warning(suppress: 6386)
    memset(m_pBits, 0x00, bitsSize);
    const std::vector<unsigned char> bytes = pDoc->ReadRange(0, len);
    if (!bytes.empty()) {
        memcpy(m_pBits, bytes.data(),
               (bytes.size() < bitsSize) ? bytes.size() : bitsSize);
    }
    m_imgHeight = rows;
    m_dataLen = len;

    // メモリDC に DIB を選択（BitBlt 元）。
    CClientDC dc(this);
    if (m_memDC.CreateCompatibleDC(&dc)) {
        m_hOldBitmap = static_cast<HBITMAP>(
            ::SelectObject(m_memDC.GetSafeHdc(), m_dib.Get()));
    }

    if (GetSafeHwnd() != nullptr) {
        UpdateScrollInfo();
        Invalidate();
    }
}

int CBitImageWnd::ClientHeightPx() const {
    CRect rc;
    GetClientRect(&rc);
    return rc.Height();
}

int CBitImageWnd::ContentHeight() const {
    // 仮想内容高 = 上マージン + 画像高 + 下マージン（原 nMax = imgHeight + 0x10）。
    return (m_imgHeight > 0) ? (m_imgHeight + kTopMargin * 2) : 0;
}

void CBitImageWnd::UpdateScrollInfo() {
    const int content = ContentHeight();
    const int clientH = ClientHeightPx();
    int maxPos = content - clientH;
    if (maxPos < 0) { maxPos = 0; }
    if (m_scrollPos > maxPos) { m_scrollPos = maxPos; }
    if (m_scrollPos < 0) { m_scrollPos = 0; }

    SCROLLINFO si = {0};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
    si.nMin = 0;
    si.nMax = (content > 0) ? (content - 1) : 0;
    si.nPage = (clientH > 0) ? static_cast<UINT>(clientH) : 1;
    si.nPos = m_scrollPos;
    SetScrollInfo(SB_VERT, &si, TRUE);
}

BOOL CBitImageWnd::OnEraseBkgnd(CDC* /*pDC*/) {
    return TRUE;   // OnPaint で全域塗り（ちらつき防止）
}

void CBitImageWnd::OnPaint() {
    CPaintDC dc(this);
    CRect rc;
    GetClientRect(&rc);
    dc.FillSolidRect(rc, m_bg);   // 背景（画像外・マージンを含む）

    if (!m_dib.Valid() || m_imgHeight <= 0 || m_memDC.GetSafeHdc() == nullptr) {
        return;
    }
    // 画像 row0 の描画Y = 上マージン − スクロール位置。可視部のみ BitBlt。
    int destY = kTopMargin - m_scrollPos;
    int srcY = 0;
    int copyH = m_imgHeight;
    if (destY < 0) { srcY = -destY; destY = 0; copyH = m_imgHeight - srcY; }
    const int clientH = rc.Height();
    if (destY + copyH > clientH) { copyH = clientH - destY; }
    if (copyH <= 0) { return; }

    int copyW = kImageWidth;
    if (copyW > rc.Width()) { copyW = rc.Width(); }
    dc.BitBlt(0, destY, copyW, copyH, &m_memDC, 0, srcY, SRCCOPY);

    // 最終行の末尾（データ長がストライドで割り切れない端数）は背景色で塗る（原 FUN_004052b0）。
    //   DIB 上は 0x00（=パレット索引0）で埋めてあるため、反映ON時は上書きが必要。
    const int tail = m_dataLen % kImageWidth;
    if (tail != 0) {
        const int lastY = (kTopMargin - m_scrollPos) + (m_imgHeight - 1);
        if (lastY >= 0 && lastY < clientH) {
            int x1 = tail, x2 = kImageWidth;
            if (x2 > rc.Width()) { x2 = rc.Width(); }
            if (x1 < x2) { dc.FillSolidRect(x1, lastY, x2 - x1, 1, m_bg); }
        }
    }
}

void CBitImageWnd::OnSize(UINT nType, int cx, int cy) {
    CWnd::OnSize(nType, cx, cy);
    UpdateScrollInfo();
}

void CBitImageWnd::OnVScroll(UINT nSBCode, UINT /*nPos*/, CScrollBar* /*pScrollBar*/) {
    SCROLLINFO si = {0};
    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL;
    GetScrollInfo(SB_VERT, &si);

    int pos = si.nPos;
    const int line = 16;                       // 1ライン移動量(px)
    const int page = (int)si.nPage;
    switch (nSBCode) {
    case SB_LINEUP:    pos -= line; break;
    case SB_LINEDOWN:  pos += line; break;
    case SB_PAGEUP:    pos -= page; break;
    case SB_PAGEDOWN:  pos += page; break;
    case SB_TOP:       pos = si.nMin; break;
    case SB_BOTTOM:    pos = si.nMax; break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION: pos = si.nTrackPos; break;
    default: break;
    }
    int maxPos = ContentHeight() - ClientHeightPx();
    if (maxPos < 0) { maxPos = 0; }
    if (pos > maxPos) { pos = maxPos; }
    if (pos < 0) { pos = 0; }
    if (pos != m_scrollPos) {
        m_scrollPos = pos;
        SetScrollPos(SB_VERT, m_scrollPos, TRUE);
        Invalidate();
    }
}

// カーソル位置→ファイルオフセット（範囲外は -1）。原 FUN_00404324/FUN_00404480 相当。
int CBitImageWnd::HitTestOffset(CPoint pt) const {
    if (!m_dib.Valid() || m_dataLen <= 0) { return -1; }
    if (pt.x < 0 || pt.x >= kImageWidth) { return -1; }
    // 画面Y → 画像行: row = (scrollPos - topMargin) + y
    const int row = (m_scrollPos - kTopMargin) + pt.y;
    if (row < 0 || row >= m_imgHeight) { return -1; }
    const int offset = row * kImageWidth + pt.x;
    return (offset < m_dataLen) ? offset : -1;
}

void CBitImageWnd::ShowOffsetInStatus(int offset) const {
    CFrameWnd* pMain = DYNAMIC_DOWNCAST(CFrameWnd, AfxGetMainWnd());
    if (pMain == nullptr) { return; }
    if (offset >= 0) {
        CString s;
        s.Format(_T("0x%08llX"), static_cast<long long>(offset));   // 原 s_0x_08X_004b5108
        pMain->SetMessageText(s);
    } else {
        pMain->SetMessageText(AFX_IDS_IDLEMESSAGE);   // レディ
    }
}

void CBitImageWnd::OnMouseMove(UINT /*nFlags*/, CPoint point) {
    if (!m_tracking) {   // WM_MOUSELEAVE 追跡を登録（ペインを離れたらレディへ戻す）
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, m_hWnd, 0 };
        ::TrackMouseEvent(&tme);
        m_tracking = true;
    }
    ShowOffsetInStatus(HitTestOffset(point));
}

void CBitImageWnd::OnMouseLeave() {
    m_tracking = false;
    ShowOffsetInStatus(-1);   // レディへ戻す
}

// 右クリック: ビットイメージ用ポップアップ（IDR_BITIMAGE_POPUP=188）。原 FUN_00404566 と同型:
//   オーナーをこのウィンドウにして自動コマンドUI更新を回避。コマンドは OnCommand でフレームへ委譲。
void CBitImageWnd::OnContextMenu(CWnd* /*pWnd*/, CPoint point) {
    CMenu menu;
    if (!menu.LoadMenu(IDR_BITIMAGE_POPUP)) { return; }
    CMenu* pPopup = menu.GetSubMenu(0);
    if (pPopup == nullptr) { return; }
    if (point.x == -1 && point.y == -1) {
        CRect rc; GetWindowRect(&rc);
        point = rc.CenterPoint();
    }
    pPopup->TrackPopupMenu(TPM_LEFTALIGN | TPM_RIGHTBUTTON, point.x, point.y, this);
}

BOOL CBitImageWnd::OnCommand(WPARAM wParam, LPARAM lParam) {
    if (lParam == 0 && HIWORD(wParam) == 0) {   // メニュー由来コマンドをフレームへ委譲
        if (CWnd* pMain = AfxGetMainWnd()) {
            pMain->SendMessage(WM_COMMAND, wParam, lParam);
            return TRUE;
        }
    }
    return CWnd::OnCommand(wParam, lParam);
}

// ===========================================================================
// CBitImageBar
// ===========================================================================
BEGIN_MESSAGE_MAP(CBitImageBar, CDialogBar)
    ON_WM_SIZE()
    ON_WM_LBUTTONDOWN()
END_MESSAGE_MAP()

CBitImageBar::CBitImageBar() {}

BOOL CBitImageBar::CreateBar(CWnd* pParent) {
    if (!Create(pParent, IDD_BITIMAGE_BAR,
                CBRS_LEFT | CBRS_TOOLTIPS | CBRS_FLYBY | CBRS_SIZE_DYNAMIC,
                IDW_BITIMAGE_BAR)) {
        return FALSE;
    }
    if (!m_image.Create(this)) {
        return FALSE;
    }
    ::SetWindowTextW(GetSafeHwnd(), ui::LoadW(IDS_BAR_BITIMAGE));
    EnableDocking(CBRS_ALIGN_LEFT | CBRS_ALIGN_RIGHT);
    return TRUE;
}

CSize CBitImageBar::DefaultFloatingSize() {
    // 原 CMainFrame::OnCreate: 幅=SM_CXVSCROLL+0xA0+4*SM_CXEDGE、高さ=500。
    const int width = ::GetSystemMetrics(SM_CXVSCROLL) + 0xA0 +
                      ::GetSystemMetrics(SM_CXEDGE) * 4;
    return CSize(width, 500);
}

void CBitImageBar::SetDockable(bool dockable) {
    m_dockable = dockable;
    EnableDocking(dockable ? (CBRS_ALIGN_LEFT | CBRS_ALIGN_RIGHT) : 0);
}

// 縦ドッキング（左右）: 固定幅＋高さ伸長。フローティング時は原版初期寸法。
CSize CBitImageBar::CalcFixedLayout(BOOL bStretch, BOOL bHorz) {
    CSize sz = DefaultFloatingSize();
    // 左右ドック時はドロップY位置に関係なくドック領域の全高を占有する。
    if (!IsFloating() && !bHorz) {
        sz.cy = 32767;
    } else if (bStretch) {
        if (bHorz) { sz.cx = 32767; } else { sz.cy = 32767; }
    }
    return sz;
}

void CBitImageBar::OnSize(UINT nType, int cx, int cy) {
    CDialogBar::OnSize(nType, cx, cy);
    FitImage();
}

void CBitImageBar::OnLButtonDown(UINT nFlags, CPoint point) {
    // 原版はドック時のペイン上部余白をドラッグしてフローティングへ移行する。
    if (m_dockable && !IsFloating() && point.y < kPaneMargin && m_pDockContext != nullptr) {
        CPoint screenPoint = point;
        ClientToScreen(&screenPoint);
        m_pDockContext->StartDrag(screenPoint);
        return;
    }
    CDialogBar::OnLButtonDown(nFlags, point);
}

void CBitImageBar::FitImage() {
    if (m_image.GetSafeHwnd() != nullptr) {
        CRect rc;
        GetClientRect(&rc);
        // 縦ドック時のバー本体は全高センチネル(32767)を持つため、子画像までその高さに
        // するとスクロールバー下端が画面外へ出る。親ドックバーの実可視高へクランプする。
        if (!IsFloating()) {
            if (CWnd* dockBar = GetParent()) {
                CRect dockClient;
                dockBar->GetClientRect(&dockClient);
                if (dockClient.Width() > 0) rc.right = min(rc.right, dockClient.Width());
                if (dockClient.Height() > 0) rc.bottom = min(rc.bottom, dockClient.Height());
            }
        }
        rc.left += kPaneLeftMargin;
        rc.top += kPaneMargin;
        rc.right -= kPaneMargin;
        rc.bottom -= kPaneMargin;
        if (rc.right < rc.left) rc.right = rc.left;
        if (rc.bottom < rc.top) rc.bottom = rc.top;
        m_image.MoveWindow(&rc);
    }
}

void CBitImageBar::Refresh(CStirlingDoc* pDoc) {
    m_image.BuildFromDoc(pDoc);
    FitImage();
}
