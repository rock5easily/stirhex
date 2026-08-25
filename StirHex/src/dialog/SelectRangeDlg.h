// CSelectRangeDlg — 範囲を指定して選択ダイアログ（原 IDD_SELECT_RANGE=202, 生成=FUN_00405de0。モーダル）。
//   範囲入力バー（IDD 200, CRangeBarDlg）を埋め込み、選択範囲[start,end]（両端含む）を選ぶ。
//   印刷版（CPrintRangeDlg）からプレビュー要否を除いたもの。呼出元＝原 FUN_0044956b（0x8065）。
#pragma once

#include "core/CoreTypes.h"   // stirling::FileOffset（アドレスの 64bit 化。Issue #21）

#include "resource.h"
#include "dialog/RangeBarDlg.h"

class CSelectRangeDlg : public CDialog {
public:
    // total   : データ総サイズ（有効アドレス上限＝total-1）。
    // hasSel  : 現在選択の有無。selStart/selEnd : 現在の選択範囲（両端含む。範囲バーの初期値）。
    CSelectRangeDlg(CWnd* pParent, stirling::FileOffset total, bool hasSel,
                    stirling::FileOffset selStart, stirling::FileOffset selEnd);

    stirling::FileOffset Start() const { return m_start; }   // 確定した選択範囲 開始（両端含む）
    stirling::FileOffset End()   const { return m_end; }     // 確定した選択範囲 終了（両端含む）

protected:
    virtual BOOL OnInitDialog();
    virtual void OnOK();
    DECLARE_MESSAGE_MAP()

    CRangeBarDlg m_rangeBar;     // 範囲入力バー（原 this+0x5c, IDD 200 子ダイアログ）

    stirling::FileOffset m_total;
    bool m_hasSel;
    stirling::FileOffset m_selStart, m_selEnd;   // 現在の選択範囲（両端含む）
    stirling::FileOffset m_start, m_end;         // 確定した選択範囲（両端含む）
};
