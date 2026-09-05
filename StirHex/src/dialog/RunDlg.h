// CRunDlg — 「名前を指定して実行」ダイアログ（原 IDD_RUN=168。コマンド 0x804f）。
//   コマンドラインを履歴コンボで入力し、OK で確定する。起動そのものは呼び出し側
//   （RunApp）が行う。原の実測挙動:
//     - 起動時のコンボは空。ドロップダウンに履歴（新しい順）が入る。
//     - 空のまま OK → 「ファイル名が入力されていません」を表示し、ダイアログは開いたまま。
//     - 非空で OK → 履歴へ追加してダイアログを閉じ、その後に起動する（失敗時はエラー表示）。
//     - 「参照」→ 「プログラム|*.exe|すべてのファイル(*.*)」のファイル選択（タイトルは本ダイアログ名）。
//   履歴は設定ストア [History] の Execute0..N（新しい順）に保存し、設定ファイルへ永続化する。
#pragma once

#include "resource.h"

class CRunDlg : public CDialog {
public:
    explicit CRunDlg(CWnd* pParent);

    const CString& Command() const { return m_cmd; }   // OK 確定時のコマンドライン

protected:
    virtual BOOL OnInitDialog();
    virtual void OnOK();
    afx_msg void OnBrowse();          // 「参照(&B)...」実行ファイル選択
    DECLARE_MESSAGE_MAP()

private:
    CString m_cmd;
};

// 「名前を指定して実行」コマンドの実体（ダイアログ→起動→失敗時エラー表示）。
//   原と同じく、入力文字列全体を実行対象として渡す（引数の分割はしない）。
void RunAppCommand(CWnd* pOwner);
