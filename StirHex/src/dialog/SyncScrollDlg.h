// CSyncScrollDlg — シンクロスクロール手動登録ダイアログ（原 IDD_SYNC_SCROLL=193, 0x805f）。
//   開いている全ウィンドウのうち、所有ビューと連動（同期スクロール）させる相手を
//   2つのリストボックス間の移動で登録/解除する転送UI。
//     上: "シンクロしないウィンドウ" 候補リスト(IDC_SYNC_CANDIDATE)
//     下: "シンクロするウィンドウ"   登録リスト(IDC_SYNC_REGISTERED)
//     ↓追加 / ↑解除 / 全解除 / OK / キャンセル / ヘルプ
//   OK で「所有ビュー＋登録リストの全ウィンドウ」を同期グループとして確定する
//   （所有ビュー CStirlingView::ApplySyncGroup へ委譲）。
//   原の対応: ctor=FUN_00463740 / OnInitDialog=FUN_00463833 / OnOK=FUN_00463f7a。
#pragma once

#include "resource.h"
#include <vector>

class CStirlingView;

class CSyncScrollDlg : public CDialog {
public:
    explicit CSyncScrollDlg(CStirlingView* owner);

protected:
    virtual void DoDataExchange(CDataExchange* pDX);
    virtual BOOL OnInitDialog();
    virtual void OnOK();

    afx_msg void OnAdd();          // 候補→登録（原 FUN_00463b95）
    afx_msg void OnRemove();       // 登録→候補（原 FUN_00463cc7）
    afx_msg void OnResetAll();     // 登録を全て候補へ戻す（原 FUN_00463dec）
    afx_msg void OnHelpButton();   // ヘルプ（原 FUN_00464075）

    // マスタエントリ（全ウィンドウ, 列挙順）。ItemData はこの索引を格納する。
    struct Entry {
        CString        title;
        CStirlingView* view = nullptr;
    };

    void UpdateButtons();                                   // ボタン活性制御（原 FUN_00463f00）
    void MoveSelected(CListBox& from, CListBox& to);        // 選択項目を索引昇順を保って移動
    bool RegisteredContains(int entryIndex) const;          // 登録リストに索引が存在するか

    CStirlingView*     m_owner;
    std::vector<Entry> m_entries;
    CListBox           m_listRegistered;   // IDC_SYNC_REGISTERED (1024)
    CListBox           m_listCandidate;    // IDC_SYNC_CANDIDATE (1021)

    DECLARE_MESSAGE_MAP()
};
