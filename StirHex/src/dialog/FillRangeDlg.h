// CFillRangeDlg — 指定範囲の初期化ダイアログ（原 IDD_FILL_RANGE=165, FUN_00410c80。モーダル）。
//   選択範囲を単一バイト値で埋める。範囲を静的 1046 に "指定範囲 : %08X ～ %08X" で表示、
//   edit 1007 に初期化バイトを 2桁16進で入力（0～FF）。空/不正はメッセージで拒否。
#pragma once

#include "core/CoreTypes.h"   // stirling::FileOffset（アドレスの 64bit 化。Issue #21）

#include "resource.h"

class CFillRangeDlg : public CDialog {
public:
    // startAbs/endAbs : 初期化対象の絶対範囲（両端含む＝原の start/end 表示に一致）。
    CFillRangeDlg(CWnd* pParent, stirling::FileOffset startAbs, stirling::FileOffset endAbs);

    unsigned char Value() const { return m_value; }   // OK確定後の初期化バイト値

protected:
    virtual void DoDataExchange(CDataExchange* pDX);
    virtual BOOL OnInitDialog();
    virtual void OnOK();

    stirling::FileOffset m_start;      // 範囲先頭（表示用）
    stirling::FileOffset m_end;        // 範囲末尾（表示用, 両端含む）
    unsigned char m_value;      // 確定バイト値
    CString       m_valueText;  // edit 1007 の入力（2桁16進）
};
