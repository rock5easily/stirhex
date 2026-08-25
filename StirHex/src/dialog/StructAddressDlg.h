// CStructAddressDlg — 構造体編集「先頭アドレスの指定」（原 IDD 196 / FUN_00460680系）。
//   アドレス直接指定（10進/16進、キャレット相対 +/-）または登録マーク位置を選択する。
#pragma once

#include "core/CoreTypes.h"   // stirling::FileOffset（アドレスの 64bit 化。Issue #21）

#include "resource.h"
#include "dialog/MarkListDlg.h"   // CMarkListBox（マーク色付きオーナードロー）

#include <vector>

class CStirlingDoc;

class CStructAddressDlg : public CDialog {
public:
    CStructAddressDlg(CWnd* pParent, CStirlingDoc* pDoc, stirling::FileOffset current, stirling::FileOffset total);

    stirling::FileOffset ResultAddr() const { return m_result; }

protected:
    virtual void DoDataExchange(CDataExchange* pDX);
    virtual BOOL OnInitDialog();
    virtual void OnOK();

    afx_msg void OnModeChanged(UINT nID);
    afx_msg void OnBaseChanged(UINT nID);
    afx_msg void OnMarkDblClk();
    DECLARE_MESSAGE_MAP()

    bool ParseAddress(const CString& text, int base, long long& value) const;
    void UpdateHints();
    void UpdateEnableState();
    void ReformatAddress(int newBaseHex);
    void FocusAddressEdit();

    CStirlingDoc* m_pDoc;
    stirling::FileOffset m_current;
    stirling::FileOffset m_total;
    stirling::FileOffset m_result;
    int m_modeMark = 0;   // 0=アドレス指定 / 1=マーク登録位置
    int m_baseHex = 1;    // 0=10進 / 1=16進（原既定）
    CString m_addressText;
    CMarkListBox m_markList;
    std::vector<stirling::FileOffset> m_markPositions;
};
