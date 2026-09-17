// CFindResultDlg — 検索結果一覧ダイアログ（移植版で追加。IDD_FIND_RESULT=261、モードレス。Issue #236）。
//   [検索] ダイアログの [全て検索] で開き、文書内の一致箇所をすべて一覧にする。
//   ビューごとに1つで、所有ビューが生成・破棄する。検索はタイマーで少しずつ進め、
//   文書の内容が変わったら検索を中止して一覧を「変更あり」にする（ジャンプ不可、[再検索] で更新）。
#pragma once

#include "core/CoreTypes.h"
#include "core/FindAll.h"
#include "core/HexPattern.h"
#include "resource.h"

#include <vector>

class CStirlingView;

class CFindResultDlg : public CDialog {
public:
    // 検索条件。rangeMode は CFindDlg::Range。lo/hi は検索範囲 [lo, hi)。
    struct Condition {
        stirling::HexPattern pattern;
        CStringW display;
        bool isHex = true;
        int rangeMode = 0;
        stirling::FileOffset lo = 0;
        stirling::FileOffset hi = 0;
    };

    explicit CFindResultDlg(CStirlingView* view);

    BOOL CreateModeless(CWnd* parent);
    // 条件を差し替えて検索を始める（既存の結果は破棄する）。
    void StartSearch(const Condition& condition);
    // 所有ビューの破棄時に呼ぶ。以降ビューへアクセスせず、自分を破棄する。
    void OnViewDestroyed();

protected:
    virtual void DoDataExchange(CDataExchange* pDX);
    virtual BOOL OnInitDialog();
    virtual void OnOK();          // ジャンプ（閉じない）
    virtual void OnCancel();      // 閉じる
    virtual void PostNcDestroy(); // モードレスのため delete this
    afx_msg void OnDestroy();
    afx_msg void OnTimer(UINT_PTR nIDEvent);
    afx_msg void OnResearch();
    afx_msg void OnStop();
    afx_msg void OnGetDispInfo(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnDblclkList(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnItemChanged(NMHDR* pNMHDR, LRESULT* pResult);
    DECLARE_MESSAGE_MAP()

private:
    enum : UINT_PTR { kStepTimer = 1, kWatchTimer = 2 };
    static constexpr size_t kPreviewBytes = 16;   // [データ] 列に表示する最大バイト数

    bool ViewAlive() const;
    void RunSteps();              // 1回のタイマーで時間を区切って検索を進める
    void CollectPreviews();       // 新しく見つかった一致のバイト列を控える
    void FinishSearch();          // 終了状態に応じて表示を確定する
    void MarkOutdated();          // 文書の変更を検知したときの表示
    void UpdateTitle();           // 「検索結果一覧 - 文書名」
    void UpdateCondition();
    void UpdateStatus();
    void UpdateButtons();
    int  SelectedIndex() const;

    CStirlingView* m_view;
    CListCtrl m_list;
    Condition m_condition;
    stirling::FindAll m_finder;
    std::vector<unsigned char> m_previews;   // 一致ごとに kPreviewBytes バイト（不足分は 0）
    size_t m_previewCount = 0;               // m_previews に控えた一致の数
    long m_dataSeq = 0;                      // 検索開始時の文書のデータ内容変更シーケンス
    bool m_outdated = false;
    bool m_timerError = false;               // タイマーを張れず検索・変更監視ができない
};
