// CDiffListDlg — 相違箇所一覧ダイアログ（原 IDD_DIFF_LIST=167, FUN_00409930。モードレス）。
//   データ比較で見つかった相違範囲を一覧表示し、ジャンプ／活性ビュー切替／強調表示ON-OFF／
//   シンクロスクロールON-OFFを行う。開始ビューが所有し、閉じると両ビューの比較状態を解除する。
//   ※原の一覧は独自コントロール DDS2CustomCtrl。本移植は標準 CListCtrl（レポート3カラム）で代替。
#pragma once

#include "core/CoreTypes.h"   // stirling::FileOffset（Issue #21）

#include "resource.h"
#include <vector>
#include <utility>

class CStirlingView;
class CDiffListMinimizedProxy;

class CDiffListDlg : public CDialog {
public:
    CDiffListDlg();

    // モードレス生成。view1=比較元, view2=比較先, diffs=相違範囲[start,end]（両端含む, 昇順）。
    BOOL CreateModeless(CStirlingView* view1, CStirlingView* view2,
                        const std::vector<std::pair<stirling::FileOffset, stirling::FileOffset>>& diffs, CWnd* pParent);
    void OnViewDestroyed(CStirlingView* view);

protected:
    virtual void DoDataExchange(CDataExchange* pDX);
    virtual BOOL OnInitDialog();
    afx_msg void OnDestroy();
    afx_msg void OnSysCommand(UINT nID, LPARAM lParam);
    afx_msg LRESULT OnMinimizedProxyRestore(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnMinimizedProxyClose(WPARAM wParam, LPARAM lParam);
    virtual void OnOK();          // ジャンプ（IDOK）: 選択相違へ移動（閉じない）
    virtual void OnCancel();      // 閉じる（IDCANCEL）: 比較状態を解除して破棄
    virtual void PostNcDestroy(); // モードレスのため delete this
    afx_msg void OnSwitch();      // 切替: 活性ビュー view1↔view2
    afx_msg void OnHilite();      // 強調表示チェック
    afx_msg void OnSync();        // シンクロスクロールチェック
    afx_msg void OnDblclkList(NMHDR* pNMHDR, LRESULT* pResult);
    DECLARE_MESSAGE_MAP()

    void JumpToSelected();        // 一覧の選択行の相違範囲へ活性ビューをジャンプ
    void ApplyHighlight();        // 強調表示チェックを両ビューへ反映
    void ApplySync();             // シンクロチェックを両ビューへ反映
    void Cleanup();               // 両ビューの比較状態・同期を解除
    bool ViewAlive(CStirlingView* v) const;   // ビューがまだ有効か
    bool CreateMinimizedProxy();
    void DestroyMinimizedProxy();
    void RestoreFromMinimizedProxy();
    void CloseFromMinimizedProxy();
    void OnMinimizedProxyDestroyed(CDiffListMinimizedProxy* proxy);
    void KeepMinimizedProxyInsideMdi();
    friend class CDiffListMinimizedProxy;

    CStirlingView* m_view1;
    CStirlingView* m_view2;
    CStirlingView* m_active;      // 活性ビュー（ジャンプ先。原 this+0x2b0）
    std::vector<std::pair<stirling::FileOffset, stirling::FileOffset>> m_diffs;
    CListCtrl m_list;
    BOOL m_hilite;                // 比較結果の強調表示（既定ON）
    BOOL m_sync;                  // シンクロスクロール（既定ON）
    bool m_cleaned = false;       // Cleanup の二重実行を防ぐ（外部 DestroyWindow 対応）
    bool m_minimized = false;
    bool m_normalRectValid = false;
    CRect m_normalRect;
    bool m_destroying = false;
    CDiffListMinimizedProxy* m_proxy = nullptr;
};
