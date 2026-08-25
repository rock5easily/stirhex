// CCompareDlg — 比較対象選択ダイアログ（原 IDD_COMPARE=166, FUN_00409600。モーダル）。
//   現在の文書以外の開いている文書（各ビュー）をフレームタイトルで一覧し、比較対象を選ぶ。
//   選択結果は対象ビュー（CStirlingView*）。呼び元はそのビューの文書と比較を行う。
#pragma once

#include "resource.h"

class CStirlingDoc;
class CStirlingView;

class CCompareDlg : public CDialog {
public:
    // selfDoc : 自分自身の文書（一覧から除外する）。
    CCompareDlg(CWnd* pParent, CStirlingDoc* selfDoc);

    CStirlingView* TargetView() const { return m_targetView; }   // OK確定後の比較対象ビュー

protected:
    virtual void DoDataExchange(CDataExchange* pDX);
    virtual BOOL OnInitDialog();
    virtual void OnOK();
    DECLARE_MESSAGE_MAP()

    CStirlingDoc*  m_selfDoc;
    CStirlingView* m_targetView;   // 選択された比較対象ビュー
};
