// CFindDlg — 検索ダイアログ（原 CSearchDlg、IDD_FIND=161。モーダル）。
//   検索データ(16進/文字列)・検索範囲(カーソル位置/全体/選択範囲)・前/次検索。
//   Next/Prev は閉じずに検索を実行し、キャンセルで閉じる（原と同じモーダル挙動）。
//   検索の実処理は所有ビュー CStirlingView::FindFromDialog に委譲する。
#pragma once

#include "resource.h"

class CStirlingView;

class CFindDlg : public CDialog {
public:
    explicit CFindDlg(CStirlingView* pView);

    // 検索範囲モード（原 view+0x23c 相当のUI選択）。
    enum Range { kFromCursor = 0, kWholeData = 1, kSelection = 2 };

protected:
    virtual BOOL OnInitDialog();

    void OnFindNext();     // 次検索（前方）
    void OnFindPrev();     // 前検索（後方）
    void DoFind(bool forward);

    int  CurrentRange() const;      // ラジオ選択 → Range
    bool IsHexType() const;         // 16進種別か（false=文字列）

    CStirlingView* m_pView;
    DECLARE_MESSAGE_MAP()
};
