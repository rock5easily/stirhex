// CCompareProgressDlg — データ比較の進捗ダイアログ（移植版で追加。IDD_COMPARE_PROGRESS=263、
//   モーダル。Issue #307）。
//   呼出側が開始した stirling::DiffScan をタイマーで少しずつ進め、進捗バーを更新する。
//   [中止] で比較を中止する。比較を続けてよいか（文書が閉じられていないか、データが
//   変わっていないか）は、タイマーのたびに呼出側の canContinue で確かめる。
#pragma once

#include "resource.h"
#include "core/DiffScan.h"

#include <functional>

//   閉じた後の結果（完了・打ち切り・中止・読み取りエラー等）は scan の状態で判断する。
//   [中止] でも canContinue が false を返した場合でも、scan は Cancelled になる。
class CCompareProgressDlg : public CDialog {
public:
    CCompareProgressDlg(stirling::DiffScan& scan, std::function<bool()> canContinue, CWnd* pParent);

protected:
    virtual BOOL OnInitDialog();
    virtual void OnCancel();   // [中止]・Esc・閉じるボタン
    virtual void OnOK() {}     // Enter で閉じない

    afx_msg void OnTimer(UINT_PTR nIDEvent);
    afx_msg void OnDestroy();

private:
    void RunSteps();
    void UpdateProgress();

    stirling::DiffScan&   m_scan;
    std::function<bool()> m_canContinue;

    DECLARE_MESSAGE_MAP()
};
