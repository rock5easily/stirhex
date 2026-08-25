// CMarkAddressDlg — マークアドレス指定ダイアログ（原 IDD_MARK_ADDRESS=190。モーダル）。
//   マークの登録位置(アドレス)・マーク色(種別0/1/2)・アドレスベース(10進/16進)を指定する。
//   一覧ダイアログの「編集」「新規登録」から共用で呼ばれる（原 FUN_00428d70 系）。
#pragma once

#include "core/CoreTypes.h"   // stirling::FileOffset（アドレスの 64bit 化。Issue #21）

#include "resource.h"

// マーク色コンボ（原 dlg+0x5c のオーナードロー CComboBox）。
//   3項目それぞれをマークの背景色で塗り、前景色で "MARK<n>" を描画してマーク色をプレビュー。
class CMarkColorCombo : public CComboBox {
public:
    void SetColors(const COLORREF* fg, const COLORREF* bg, int count);
protected:
    virtual void DrawItem(LPDRAWITEMSTRUCT lpDIS);
    virtual void MeasureItem(LPMEASUREITEMSTRUCT lpMIS);
    COLORREF m_fg[3] = { 0, 0, 0 };
    COLORREF m_bg[3] = { 0, 0, 0 };
    int      m_count = 0;
};

class CMarkAddressDlg : public CDialog {
public:
    // fg/bg  : マーク色配列(3要素、COLORREF)。initType: 初期選択マーク種別(0/1/2)。
    // maxAddr: 有効アドレス上限(=total-1)。initAddr: 初期アドレス。
    // prefill: 初期アドレスをエディットへ反映するか（原 param_5。false=空欄開始）。
    CMarkAddressDlg(CWnd* pParent, const COLORREF* fg, const COLORREF* bg,
                    int initType, stirling::FileOffset maxAddr, stirling::FileOffset initAddr, bool prefill);

    stirling::FileOffset ResultAddr() const { return m_addr; }    // OK確定後: 指定アドレス
    int ResultType() const { return m_type; }    // OK確定後: マーク種別(0/1/2)

protected:
    virtual void DoDataExchange(CDataExchange* pDX);
    virtual BOOL OnInitDialog();
    virtual void OnOK();
    afx_msg void OnBaseChanged(UINT nID);          // 10進/16進ラジオ切替
    DECLARE_MESSAGE_MAP()

    void UpdateHint();                             // 静的「有効アドレス : 0 ～ N」を現基数で更新
    bool ParseAddr(const CString& text, int base, long long& out) const;  // 解析（失敗 false）

    CMarkColorCombo m_combo;
    COLORREF m_fg[3];
    COLORREF m_bg[3];
    int  m_type;       // マーク種別（コンボ選択）
    stirling::FileOffset m_maxAddr;    // 有効アドレス上限
    stirling::FileOffset m_addr;       // 初期/確定アドレス
    bool m_prefill;    // 初期アドレスを反映するか
    int  m_baseHex;    // アドレスベース DDX_Radio（0=10進 / 1=16進）
    CString m_addrText;   // アドレス文字列（DDX）
};
