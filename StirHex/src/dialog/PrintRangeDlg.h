// CPrintRangeDlg — 範囲を指定して印刷ダイアログ（原 IDD_PRINT_RANGE=201, FUN_0042cee0。モーダル）。
//   範囲入力バー（IDD 200, CRangeBarDlg）を埋め込み、印刷範囲[start,end]を選ぶ。
//   「プレビュー経由で印刷」チェック（原 dlg+0x5c）で、OK後に印刷/プレビューのどちらへ進むかを返す。
//   原 OnOK（FUN_0042d0d2）は範囲バーの検証結果を dlg+0x16c/0x170 へ格納する。
#pragma once

#include "core/CoreTypes.h"   // stirling::FileOffset（アドレスの 64bit 化。Issue #21）

#include "resource.h"
#include "dialog/RangeBarDlg.h"

class CPrintRangeDlg : public CDialog {
public:
    // total      : データ総サイズ（有効アドレス上限＝total-1）。
    // hasSel     : 選択の有無。selStart/selEnd : 選択範囲（両端含む）。
    CPrintRangeDlg(CWnd* pParent, stirling::FileOffset total, bool hasSel,
                   stirling::FileOffset selStart, stirling::FileOffset selEnd);

    stirling::FileOffset Start()   const { return m_start; }   // 確定した印刷範囲 開始（両端含む）
    stirling::FileOffset End()     const { return m_end; }     // 確定した印刷範囲 終了（両端含む）
    bool Preview() const { return m_preview != 0; }   // true=プレビュー経由

protected:
    virtual void DoDataExchange(CDataExchange* pDX);
    virtual BOOL OnInitDialog();
    virtual void OnOK();
    DECLARE_MESSAGE_MAP()

    CRangeBarDlg m_rangeBar;     // 範囲入力バー（原 this+0x60, IDD 200 子ダイアログ）

    stirling::FileOffset m_total;
    bool m_hasSel;
    stirling::FileOffset m_selStart, m_selEnd;   // 選択範囲（両端含む）
    stirling::FileOffset m_start, m_end;         // 確定した印刷範囲（両端含む）
    int  m_preview;              // DDX_Check: 0=印刷 / 1=プレビュー経由（原 this+0x5c）
};
