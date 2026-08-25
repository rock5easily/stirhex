// CSaveDumpDlg — ダンプイメージの保存ダイアログ（原 IDD_SAVE_DUMP=198, FUN_0040dc20。モーダル）。
//   出力ファイル名＋出力範囲（データ全体／範囲指定）を選ぶ。
//   範囲指定は原と同じ範囲バー（IDD_PRINT_RANGE_BAR=200, this+0x64 相当）を子ダイアログとして
//   載せる: 選択範囲チェック／開始～終了アドレス／アドレスベース(10進・16進)。
#pragma once

#include "core/CoreTypes.h"   // stirling::FileOffset（アドレスの 64bit 化。Issue #21）

#include "resource.h"
#include "dialog/RangeBarDlg.h"

class CSaveDumpDlg : public CDialog {
public:
    // defName    : 既定の出力ファイル名（原は元ファイル名の拡張子を .DMP へ）。
    // total      : データ総サイズ（データ全体＝[0, total-1]）。
    // hasSel     : 選択の有無。selStart/selEnd : 選択範囲（両端含む）。
    CSaveDumpDlg(CWnd* pParent, const CString& defName, stirling::FileOffset total,
                 bool hasSel, stirling::FileOffset selStart, stirling::FileOffset selEnd);

    CString m_fileName;                 // 出力ファイル名（OK確定）
    stirling::FileOffset Start() const { return m_start; }   // 出力範囲 開始（両端含む）
    stirling::FileOffset End()   const { return m_end; }     // 出力範囲 終了（両端含む）

protected:
    virtual void DoDataExchange(CDataExchange* pDX);
    virtual BOOL OnInitDialog();
    virtual void OnOK();
    afx_msg void OnBrowse();            // "..." 参照ボタン（保存ダイアログ）
    afx_msg void OnRangeMode(UINT nID); // データ全体/範囲指定 切替（範囲バーの有効/無効）
    DECLARE_MESSAGE_MAP()

    CRangeBarDlg m_rangeBar;     // 範囲指定の入力バー（原 this+0x64, IDD 200 子ダイアログ）

    stirling::FileOffset m_total;
    bool m_hasSel;
    stirling::FileOffset m_selStart, m_selEnd;   // 選択範囲（両端含む）
    int  m_range;                // DDX_Radio: 0=データ全体 / 1=範囲指定（原 this+0x5c）
    stirling::FileOffset m_start, m_end;         // 確定した出力範囲（両端含む）
};
