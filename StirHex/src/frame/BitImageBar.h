// CBitImageBar / CBitImageWnd — ビットイメージ・ペイン（原 CMainFrame+0x2b8）。
//   文書のバイト列を 128px 幅の 8bpp パレット画像として可視化する（1バイト=1ピクセル、
//   色=パレット[バイト値]）。原は専用 CWnd＋左右ドッキング可能な窓で、移植では
//   CDialogBar（空テンプレート IDD_BITIMAGE_BAR）内に描画用の子ウィンドウ CBitImageWnd を
//   生成して再現する。配色反映ON時は拡張子別設定の色・強調コードをパレットへ反映する。
#pragma once

#include "resource.h"
#include "util/ScopedGdi.h"   // GDI オブジェクトの RAII（Issue #48）

#include <vector>

class CStirlingDoc;
class CStirlingSettings;

// ビットイメージ・バーのコントロールバー ID（AFX_IDW_* / IDW_OUTPUT_BAR と衝突しない値）。
#define IDW_BITIMAGE_BAR  0xE821

// 描画用の子ウィンドウ。8bpp DIB セクションを保持し、縦スクロールで可視領域を BitBlt する。
class CBitImageWnd : public CWnd {
public:
    CBitImageWnd();
    virtual ~CBitImageWnd();

    BOOL Create(CWnd* pParent);          // 親（バー）のクライアントに子ウィンドウを生成
    void BuildFromDoc(CStirlingDoc* pDoc);  // 文書バイト列から画像を再構築（空/NULL はクリア）
    void Clear();                         // 画像を破棄して背景のみに

    static const int kImageWidth = 128;   // 画像幅（原 DIB biWidth=0x80）
    static const int kTopMargin  = 8;     // 上部マージン（原 FUN_00404a68 の 8 行分）

protected:
    // パレット（バイト値→色）を設定から決定（原 FUN_00436640）。
    //   反映OFF: 既定パレット 0x00=白/0x01-0x1F=シアン/0x20-0x7F=赤/0x80-0xFF=黒。
    //   反映ON : データ文字色＋強調表示コード（BuildByteColorTable）。背景はデータ背景色。
    //   戻り値: 反映時に使う背景色（m_bg 更新用）。
    COLORREF FillPalette(RGBQUAD* colors, const CStirlingSettings* s) const;
    void ReleaseDib();                    // DIB/メモリDC を解放
    void UpdateScrollInfo();              // 縦スクロール範囲の更新（画像高＋上下マージン）
    int  ContentHeight() const;           // 仮想内容高(px) = 画像高 + 上下マージン
    int  ClientHeightPx() const;

    stirling::ScopedGdiObject m_dib;   // 8bpp DIB セクション（RAII。Issue #48）
    void*   m_pBits;     // DIB ピクセル（各バイト=パレット索引=元バイト値）
    CDC     m_memDC;     // m_dib を選択したメモリDC（BitBlt 元）
    HBITMAP m_hOldBitmap;// m_memDC が元々持っていたビットマップ（DeleteDC 前に必ず戻す）
    int     m_imgHeight; // 画像の行数（=ピクセル高）
    int     m_dataLen;   // 元データ長（ホバー時のオフセット判定用）
    int     m_scrollPos; // 縦スクロール位置(px)
    COLORREF m_bg;       // 背景色（原 +0x98。既定=白）
    bool    m_tracking;  // WM_MOUSELEAVE 追跡中か（ホバーオフセット表示のクリア用）

    // カーソル位置→ファイルオフセット（範囲外は -1）。原 FUN_00404324/FUN_00404480 相当。
    int  HitTestOffset(CPoint pt) const;
    void ShowOffsetInStatus(int offset) const;  // ステータスバーへ "0x%08X"／範囲外はレディ

    afx_msg void OnPaint();
    afx_msg BOOL OnEraseBkgnd(CDC* pDC);
    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg void OnVScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar);
    afx_msg void OnMouseMove(UINT nFlags, CPoint point);
    afx_msg void OnMouseLeave();
    afx_msg void OnContextMenu(CWnd* pWnd, CPoint point);
    virtual BOOL OnCommand(WPARAM wParam, LPARAM lParam);  // コンテキストコマンドをフレームへ委譲
    DECLARE_MESSAGE_MAP()
};

// 左右ドッキング／フローティング対応の CDialogBar。
class CBitImageBar : public CDialogBar {
public:
    CBitImageBar();

    BOOL CreateBar(CWnd* pParent);        // ドッキングバーとして生成（既定は非表示）
    void Refresh(CStirlingDoc* pDoc);     // 画像を再構築（表示時／最新イメージ時に呼ぶ）
    void Clear() { m_image.Clear(); }
    void SetDockable(bool dockable);      // ON=左右ドッキング可 / OFF=ドッキング不可
    static CSize DefaultFloatingSize();   // 原版の初期フローティング寸法
    static const int kPaneMargin = 8;     // 上・右・下の余白（上はドック時のドラッグ領域）
    static const int kPaneLeftMargin = 16; // 原版相当の描画開始位置

protected:
    // 縦ドッキング時は固定幅＋高さ伸長、フローティング時は原版の初期寸法。
    virtual CSize CalcFixedLayout(BOOL bStretch, BOOL bHorz);
    void FitImage();                      // 子ウィンドウをバーのクライアント全域へ

    CBitImageWnd m_image;
    bool m_dockable = false;

    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
    DECLARE_MESSAGE_MAP()
};
