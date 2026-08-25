// CChildFrame 実装（最小）。
#include "pch.h"
#include "frame/ChildFrame.h"
#include "app/StirlingApp.h"
#include "util/ScopedGdi.h"   // GDI オブジェクトの RAII（Issue #48）

IMPLEMENT_DYNCREATE(CChildFrame, CMDIChildWnd)

BEGIN_MESSAGE_MAP(CChildFrame, CMDIChildWnd)
END_MESSAGE_MAP()

CChildFrame::CChildFrame() {}

CChildFrame::~CChildFrame() {}

// 新規子フレームの既定横幅を算出して cs.cx に設定する（原 FUN_0040560f→FUN_00405807）。
//   幅 = charW*(bpr*4 + 15) + 2*CXFRAME + 4*CXBORDER + CXVSCROLL、MDIクライアント幅で上限クランプ。
//   charW は表示フォント（ＭＳゴシック, 設定の fontHeight）の平均文字幅を画面DCで測る（原と同じ）。
//   cy（高さ）は原同様に変更しない（MFC 既定のまま）。
BOOL CChildFrame::PreCreateWindow(CREATESTRUCT& cs) {
    if (!CMDIChildWnd::PreCreateWindow(cs)) {
        return FALSE;
    }

    const CStirlingSettings& s = theApp.Settings();
    const int bpr = (s.lineSize > 0) ? s.lineSize : 16;

    // 表示フォントの平均文字幅を画面DCで測定（ビュー EnsureFont と同一設定）。
    LOGFONTW lf = {0};
    lf.lfHeight = s.fontHeight;
    lf.lfCharSet = SHIFTJIS_CHARSET;
    lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    wcscpy_s(lf.lfFaceName, LF_FACESIZE, L"ＭＳ ゴシック");

    int charW = 8;   // フォント生成失敗時のフォールバック
    stirling::ScopedGdiObject font(::CreateFontIndirectW(&lf));
    if (font.Valid()) {
        HDC hdc = ::GetDC(nullptr);
        if (hdc != nullptr) {
            {
                stirling::ScopedSelectHdc selFont(hdc, font.Get());
                TEXTMETRICW tm = {0};
                if (::GetTextMetricsW(hdc, &tm) && tm.tmAveCharWidth > 0) {
                    charW = tm.tmAveCharWidth;
                }
            }
            ::ReleaseDC(nullptr, hdc);
        }
    }

    // 実レイアウト幅（ビュー ContentWidthPx と同式。右余白2セル）。アドレス欄桁数=16進8桁/10進10桁。
    //   必要クライアント幅 = charW*(addrDigits + bpr*4 + 7) + 縦SB を AdjustWindowRectEx でウィンドウ幅へ。
    const int addrDigits = s.addressBase ? 8 : 10;
    CRect rc(0, 0, charW * (addrDigits + bpr * 4 + 7) + ::GetSystemMetrics(SM_CXVSCROLL), 0);
    ::AdjustWindowRectEx(&rc, cs.style & ~(WS_HSCROLL | WS_VSCROLL), FALSE, cs.dwExStyle);
    int width = rc.Width();

    // MDIクライアント幅を上限にクランプ（原 FUN_00405807 の local_b4 相当）。
    if (CMDIFrameWnd* pMainFrame = DYNAMIC_DOWNCAST(CMDIFrameWnd, AfxGetMainWnd())) {
        if (pMainFrame->m_hWndMDIClient != nullptr) {
            CRect rcClient;
            ::GetClientRect(pMainFrame->m_hWndMDIClient, &rcClient);
            if (rcClient.Width() > 0 && width > rcClient.Width()) {
                width = rcClient.Width();
            }
        }
    }

    cs.cx = width;   // 横幅のみ設定（高さは原同様に既定のまま）
    return TRUE;
}

// ドキュメントを最大化で開く（原ヘルプ docMaximize）。設定ONのとき最大化状態で活性化する。
//   MFC の MDI は最大化状態が「粘着」するため、OFF でも既に最大化中なら MFC 既定で最大化継承する。
void CChildFrame::ActivateFrame(int nCmdShow) {
    if (theApp.AppSettings().docMaximize) {
        nCmdShow = SW_SHOWMAXIMIZED;
    }
    CMDIChildWnd::ActivateFrame(nCmdShow);
}

// タイトル末尾に編集マーク「 *」を付与（原挙動: 変更あり時。例 "stir131.lzh *"）。
void CChildFrame::OnUpdateFrameTitle(BOOL bAddToTitle) {
    CMDIChildWnd::OnUpdateFrameTitle(bAddToTitle);   // 通常のタイトル（ファイル名）を構築
    CDocument* pDoc = GetActiveDocument();
    if (pDoc != nullptr && pDoc->IsModified()) {
        CString title;
        GetWindowText(title);
        if (!title.IsEmpty() && title.Right(2) != _T(" *")) {   // 二重付与を防ぐ
            SetWindowText(title + _T(" *"));
        }
    }
}
