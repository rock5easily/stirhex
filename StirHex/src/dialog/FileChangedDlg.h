// CFileChangedDlg — 外部プロセスによるファイル変更の通知ダイアログ（原 IDD_FILE_CHANGED=199）。
//   編集中のファイルが他プロセスに書き換えられたことをビューのアクティブ化時に検知して表示する。
//   原の実測挙動:
//     - ラジオ3択。既定は「現在編集中の内容を破棄して再読み込みする」。
//     - 「別ファイルに保存する」を選んだときだけ、ファイル名／参照／比較実行が有効になる。
//     - 別名保存でファイル名が空のまま OK すると「ファイル名が入力されていません」を表示し閉じない。
#pragma once

#include "resource.h"

class CFileChangedDlg : public CDialog {
public:
    enum Choice { kIgnore = 0, kReload = 1, kSaveAs = 2 };

    // curPath: 変更が検知されたファイルのフルパス（参照ダイアログの初期位置に使う）。
    CFileChangedDlg(CWnd* pParent, const CString& curPath);

    Choice         GetChoice() const { return static_cast<Choice>(m_choice); }
    const CString& SaveAsPath() const { return m_saveAs; }   // kSaveAs のときの保存先
    bool           CompareAfterSave() const { return m_compare != FALSE; }

protected:
    virtual void DoDataExchange(CDataExchange* pDX);
    virtual BOOL OnInitDialog();
    virtual void OnOK();
    afx_msg void OnChoiceChanged(UINT nID);   // ラジオ切替（ファイル名・比較の有効/無効）
    afx_msg void OnBrowse();                  // "..." 保存先の選択
    DECLARE_MESSAGE_MAP()

private:
    void UpdateEnableState();   // 別名保存が選ばれているときだけ関連コントロールを有効化

    CString m_curPath;
    int     m_choice = kReload;   // DDX_Radio（原の既定は再読み込み）
    CString m_saveAs;
    BOOL    m_compare = FALSE;
};
