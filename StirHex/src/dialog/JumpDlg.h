// CJumpDlg — 指定アドレスへ移動ダイアログ（原 IDD_JUMP=137。モーダル）。
//   アドレス欄・アドレスベース(10進/16進)。先頭 '+'/'-' でカーソル相対移動。
//   静的表示: 有効アドレス範囲(1029) / 現在アドレス(1019)。移動先は [0, total]。
#pragma once

#include "core/CoreTypes.h"   // stirling::FileOffset（アドレスの 64bit 化。Issue #21）

#include "resource.h"

class CJumpDlg : public CDialog {
public:
    // total  : 有効アドレス上限（末尾位置 total まで移動可）。current: 現在キャレット位置。
    CJumpDlg(CWnd* pParent, stirling::FileOffset total, stirling::FileOffset current);

    stirling::FileOffset ResultAddr() const { return m_addr; }   // OK確定後の移動先アドレス

protected:
    virtual void DoDataExchange(CDataExchange* pDX);
    virtual BOOL OnInitDialog();
    virtual void OnOK();
    afx_msg void OnBaseChanged(UINT nID);        // 10進/16進ラジオ切替
    DECLARE_MESSAGE_MAP()

    void UpdateHints();                          // 有効アドレス/現在アドレスの静的を現基数で更新
    bool ParseAddr(const CString& text, int base, long long& out) const;

    stirling::FileOffset m_total;      // 有効アドレス上限
    stirling::FileOffset m_current;    // 現在アドレス（相対移動の基点／表示）
    stirling::FileOffset m_addr;       // 確定アドレス
    int  m_baseHex;    // アドレスベース DDX_Radio（0=10進 / 1=16進）
    CString m_addrText;
};
