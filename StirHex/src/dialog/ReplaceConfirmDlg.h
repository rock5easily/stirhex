// CReplaceConfirmDlg — 置換確認ダイアログ（原 IDD_REPLACE_CONFIRM=163。モーダル）。
//   一致箇所ごとに 実行/スキップ/一括置換/キャンセル を選ばせる。DoModal の戻り値で判定。
#pragma once

#include "resource.h"

class CReplaceConfirmDlg : public CDialog {
public:
    // DoModal の戻り値（対話ループの分岐に使用）。
    enum Result { kExec = 100, kSkip = 101, kAll = 102, kCancel = IDCANCEL };

    CReplaceConfirmDlg(CWnd* pParent = nullptr) : CDialog(IDD_REPLACE_CONFIRM, pParent) {}

protected:
    void OnExec() { EndDialog(kExec); }
    void OnSkip() { EndDialog(kSkip); }
    void OnAll()  { EndDialog(kAll); }
    DECLARE_MESSAGE_MAP()
};
