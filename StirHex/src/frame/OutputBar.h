// CStirlingOutputBar — アウトプットペイン（原 CMainFrame+0x1d8 のドッキング出力バー）。
//   原はコントロールバー内に CListBox を直接持ち、BGREP のヒットを 1 行
//   "フルパス : %08X" で追記する（item data に {offset, ファイル名長}）。
//   ダブルクリック/タグジャンプで該当ファイルを開き、オフセットへキャレット移動
//   （原 0x419 ハンドラ FUN_00426f09: OpenDocumentFile → ビュー GotoPos）。
//   移植では CDialogBar（テンプレート IDD_OUTPUT_BAR＝リストボックス1個）で、
//   上下ドッキング／フローティング／サイズ変更を再現する。ヒット情報は m_hits に保持し、
//   item data は索引とする。原版同様、起動時は下端100pxで配置し、配置は永続化しない。
#pragma once

#include "core/CoreTypes.h"   // stirling::FileOffset（Issue #21）

#include "resource.h"

#include <vector>

// 出力バーのコントロールバー ID（AFX_IDW_* と衝突しない値）。
#define IDW_OUTPUT_BAR  0xE820

class CStirlingOutputBar : public CDialogBar {
public:
    CStirlingOutputBar();

    // 上下ドッキング／フローティング可能なバーとして生成。既定は非表示（呼び元が制御）。
    BOOL CreateBar(CWnd* pParent);

    // BGREP 等からの結果追加/クリア（公開 API）。
    //   path=フルパス（wide 層）、offset=ヒット位置。表示は "path : %08X"。
    void AddResult(const CStringW& path, stirling::FileOffset offset);
    // item data を持たない情報行（見出し・集計メッセージ等）。
    void AddMessage(const CStringW& text);
    void ClearResults();

    void CopyToClipboard();   // 全行を CF_UNICODETEXT でクリップボードへ（0x80f7）
    void TagJump();           // 選択行のファイルを該当オフセットで開く（0x80ea/ダブルクリック）
    void FitList();           // リストボックスをバー全幅へ追従（表示化直後にも呼ぶ）

    bool HasResults() const { return !m_hits.empty(); }

protected:
    // 横ドック時は全幅＋m_dockedHeight、フローティング時はm_floatingSizeを返す。
    virtual CSize CalcFixedLayout(BOOL bStretch, BOOL bHorz);
    virtual CSize CalcDynamicLayout(int nLength, DWORD dwMode);
    // コンテキストメニュー（オーナー＝このバー）由来のコマンドをメインフレームへ委譲する。
    //   原はポップアップのオーナーをバー自身にして自動コマンドUI更新を回避するため、
    //   メニュー選択コマンドはフレームへ手動転送する必要がある。
    virtual BOOL OnCommand(WPARAM wParam, LPARAM lParam);

    // 1 ヒット（表示行に対応）。hasLoc=false は情報行（ジャンプ不可）。
    struct Hit {
        CStringW path;
        stirling::FileOffset offset = 0;
        bool hasLoc = false;
    };

    void OpenHit(int index);         // 原 FUN_00426f09 相当（開く＋オフセットジャンプ）
    int  AppendLine(const CStringW& text, const Hit& hit);  // リスト＋m_hits へ 1 行追加

    std::vector<Hit> m_hits;         // 表示行と 1:1（listbox item data＝この索引）
    CListBox         m_list;         // 結果一覧（テンプレートの IDC_OUTPUT_LIST を subclass）

    static const int kInitialWidth = 220;       // 原 CreateBar の初期幅 0xDC
    static const int kInitialHeight = 100;      // 原 CreateBar の初期高さ
    static const int kMinWidth = 120;
    static const int kMinHeight = 40;
    static const int kLeftMargin = 8;
    static const int kTopMargin = 14;           // 上部6px境界＋8pxドラッグ領域
    static const int kRightMargin = 8;
    static const int kBottomMargin = 8;
    static const int kResizeBorder = 6;

    CSize m_floatingSize = CSize(kInitialWidth, kInitialHeight);
    int   m_dockedHeight = kInitialHeight;
    bool  m_resizingDockedHeight = false;
    int   m_resizeDockPosition = -1;            // 0=下 / 1=上
    int   m_resizeStartY = 0;
    int   m_resizeStartHeight = 0;

    int  DockPosition() const;                  // 0=下 / 1=上 / 2=フローティング
    bool IsDockResizeHit(UINT nHitTest) const;
    void BeginDockedResize();
    void PrepareFloatingSizeFromDock();         // 原版同様、ドック時の全幅を初回フロート幅へ引き継ぐ

    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg LRESULT OnNcHitTest(CPoint point);
    afx_msg void OnNcLButtonDown(UINT nHitTest, CPoint point);
    afx_msg BOOL OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message);
    afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
    afx_msg void OnLButtonDblClk(UINT nFlags, CPoint point);
    afx_msg void OnMouseMove(UINT nFlags, CPoint point);
    afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
    afx_msg void OnCaptureChanged(CWnd* pWnd);
    afx_msg void OnListDblClk();
    afx_msg void OnContextMenu(CWnd* pWnd, CPoint point);
    DECLARE_MESSAGE_MAP()
};
