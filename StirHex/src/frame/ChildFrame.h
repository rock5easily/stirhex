// CChildFrame — MDI 子フレーム（原 CChildFrame : CMDIChildWnd）。
#pragma once

class CChildFrame : public CMDIChildWnd {
    DECLARE_DYNCREATE(CChildFrame)
public:
    CChildFrame();
    virtual ~CChildFrame();

    void UpdateTitle() { OnUpdateFrameTitle(TRUE); }   // 変更マーク反映のため外部から起動

    // 新規子フレームの既定横幅を原に合わせる（原 CChildFrame::PreCreateWindow FUN_0040560f）。
    //   MFC 既定の広い幅ではなく、表示フォント×1行バイト数から算出した幅を cs.cx に設定する。
    virtual BOOL PreCreateWindow(CREATESTRUCT& cs);

    // ドキュメントを最大化で開く（原ヘルプ「ドキュメントを最大化で開く」docMaximize）。
    //   設定ONなら初回表示時に SW_SHOWMAXIMIZED で活性化する。
    virtual void ActivateFrame(int nCmdShow = -1);

protected:
    // タイトル末尾に編集マーク「 *」を付与（原挙動: 変更あり時）。
    virtual void OnUpdateFrameTitle(BOOL bAddToTitle);
    DECLARE_MESSAGE_MAP()
};
