// CRangeBarDlg — 範囲入力バー（原 IDD_PRINT_RANGE_BAR=200, FUN_00430630。子ダイアログ）。
//   ダンプ保存/印刷の「範囲指定」で親ダイアログに埋め込んで使う。開始/終了アドレス欄＋
//   アドレスベース(10進/16進)＋「選択範囲」チェック（ON で選択範囲を使い欄を無効化）＋
//   有効アドレスのヒント。親の OnOK から Validate() を呼び、確定した範囲[start,end]を得る。
#pragma once

#include "core/CoreTypes.h"   // stirling::FileOffset（アドレスの 64bit 化。Issue #21）

#include "resource.h"

class CRangeBarDlg : public CDialog {
public:
    explicit CRangeBarDlg(CWnd* pParent = nullptr);

    // 有効範囲と選択範囲を設定（親の OnInitDialog から生成直後に呼ぶ）。
    //   total : 有効アドレス上限（アドレスは 0..total-1）。selStart/selEnd : 選択範囲（両端含む）。
    void SetRange(stirling::FileOffset total, bool hasSel, stirling::FileOffset selStart, stirling::FileOffset selEnd);

    // 入力を検証し範囲[start,end]（両端含む）を返す（原 FUN_00430b1c）。
    //   解析失敗/範囲外はメッセージ表示＋該当欄へフォーカスし false。start>end は入替。
    bool Validate(stirling::FileOffset& start, stirling::FileOffset& end);

    // ペイン全体の有効/無効（原 FUN_00430cfc）。親のラジオ（範囲指定=有効）から呼ぶ。
    //   選択範囲チェック = enableWhole かつ 選択あり。欄/基数 = enableWhole かつ 選択未使用。
    void SetPaneEnabled(BOOL enableWhole);

protected:
    virtual void DoDataExchange(CDataExchange* pDX);
    virtual BOOL OnInitDialog();
    afx_msg void OnBaseChanged(UINT nID);   // 10進/16進 切替（値を再整形）
    afx_msg void OnUseSelection();          // "選択範囲" チェック切替
    DECLARE_MESSAGE_MAP()

    void UpdateHint();                       // "有効アドレス : 0 ～ total-1"（現基数）
    void FillFromSelection();                // 選択範囲を現基数で開始/終了欄へ
    void EnableInputs(BOOL enable);          // 欄/基数/ヒントの有効・無効
    bool ParseAddr(const CString& text, int base, long long& out) const;

    stirling::FileOffset m_total;
    bool    m_hasSel;
    stirling::FileOffset m_selStart, m_selEnd;   // 選択範囲（両端含む）
    int     m_base;                 // DDX_Radio: 0=10進 / 1=16進（原 this+0xe8, 既定16進）
    BOOL    m_useSel;               // "選択範囲" チェック（原 this+0xe4）
    BOOL    m_enableWhole;          // ペイン全体の有効状態（範囲指定=TRUE。チェック処理で参照）
    CString m_startText;            // 開始アドレス（edit 1013）
    CString m_endText;              // 終了アドレス（edit 1014）
};
