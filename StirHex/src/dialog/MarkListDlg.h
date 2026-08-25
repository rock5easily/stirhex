// CMarkListDlg — マーク一覧ダイアログ（原 CMarkListDlg、IDD_MARK_LIST=140。モーダル）。
//   登録済みマークを位置昇順に一覧表示し、実行(ジャンプ)/編集/新規登録/解除/全解除を行う。
//   一覧末尾には「＜マークの新規登録＞」行があり、実行で新規マークを追加する（原の仕様）。
#pragma once

#include "core/CoreTypes.h"   // stirling::FileOffset（アドレスの 64bit 化。Issue #21）

#include "resource.h"
#include <vector>

class CStirlingView;

// マーク一覧リストボックス（原 dlg+0x5c のオーナードロー CListBox）。
//   各行: [種別番号スウォッチ(マーク色)] 8桁16進アドレス。ItemData=種別(0/1/2)。
//   末尾「新規登録」行は ItemData=-1（スウォッチ無し）。
class CMarkListBox : public CListBox {
public:
    void SetColors(const COLORREF* fg, const COLORREF* bg, int count);
    void SetNewEntryText(const CStringW& s) { m_newEntry = s; }   // 末尾行の日本語（ワイド描画）
protected:
    virtual void DrawItem(LPDRAWITEMSTRUCT lpDIS);
    virtual void MeasureItem(LPMEASUREITEMSTRUCT lpMIS);
    COLORREF m_fg[3] = { 0, 0, 0 };
    COLORREF m_bg[3] = { 0, 0, 0 };
    int      m_count = 0;
    CStringW m_newEntry;   // "＜マークの新規登録＞"（末尾 ItemData=-1 行）
};

class CMarkListDlg : public CDialog {
public:
    explicit CMarkListDlg(CStirlingView* pView);

    bool m_doJump  = false;   // 実行で有効マークが選択された（呼び元がジャンプ）
    stirling::FileOffset m_jumpPos = 0;       // 実行対象の絶対アドレス

protected:
    virtual void DoDataExchange(CDataExchange* pDX);
    virtual BOOL OnInitDialog();
    virtual void OnOK();          // 実行（IDOK=1）
    afx_msg void OnRemove();      // 解除 1000
    afx_msg void OnClearAll();    // 全解除 1001
    afx_msg void OnEditMark();    // 編集 1002
    afx_msg void OnSelChange();   // リスト選択変更 → ボタン活性更新
    afx_msg void OnDblClk();      // リストダブルクリック → 実行
    DECLARE_MESSAGE_MAP()

    void RebuildList();               // doc のマークから一覧を再構築（末尾に新規登録行）
    void UpdateButtons();             // 解除/編集/全解除 の活性
    void SelectMarkPos(stirling::FileOffset pos);      // 指定アドレスのマーク行を選択
    bool IsNewEntryRow(int index) const;   // 末尾「新規登録」行か
    stirling::FileOffset MarkPosAt(int index) const;  // 一覧 index → マーク位置（-1=新規登録行/無効）
    void OpenAddressDlg(int index);   // 編集(index>=0)/新規登録(index<0) の共通処理

    CMarkListBox   m_list;
    CStirlingView* m_pView;
    // 一覧の実マーク位置（末尾行を除く。index対応）
    std::vector<stirling::FileOffset> m_positions;
    bool m_hasNewRow = false;         // 末尾「新規登録」行を持つか（データ0件時は無し）
};
