// CAccelInputDlg — 「アクセラレータの指定」ダイアログ（原 IDD_ACCEL_INPUT=184 / FUN_004013f0 系。モーダル）。
//   ユーザーメニュー項目に割り当てるアクセラレータを 1 文字入力する。
//   環境設定「ユーザーメニュー」ページの「追加」（新規指定）と、項目のダブルクリック（変更）から呼ぶ。
//   原の仕様: 入力は 1 文字固定（LimitText(1)）・大文字強制（テンプレートの ES_UPPERCASE）、
//             キー種別の制限や重複チェックは無い。空欄の間は OK を無効化する。
#pragma once

#include "resource.h"

class CAccelInputDlg : public CDialog {
public:
    // initial : 変更モードでの初期アクセラレータ（追加モードでは未使用）。
    // addMode : true=追加（空欄で開始し OK は初期無効） / false=変更（現在値を表示して全選択）。
    CAccelInputDlg(CWnd* pParent, UINT initial, bool addMode);

    // OK 確定後のアクセラレータ（1 文字ぶんのコード。CAppSettings::UmMake へ渡す）。
    UINT ResultAccel() const { return m_accel; }

protected:
    virtual void DoDataExchange(CDataExchange* pDX);
    virtual BOOL OnInitDialog();
    virtual void OnOK();

    afx_msg void OnAccelChanged();   // EN_CHANGE: 空欄なら OK を無効化（原 FUN_00401570）
    DECLARE_MESSAGE_MAP()

    CEdit   m_edit;
    CString m_text;      // エディットの内容（DDX）
    UINT    m_accel;     // 初期値／確定値（0=未指定）
    bool    m_addMode;   // 追加モードか（原 dlg+0xa0 の反転）
};
