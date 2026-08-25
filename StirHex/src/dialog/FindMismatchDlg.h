// CFindMismatchDlg — 不一致検索ダイアログ（原 IDD_FIND_MISMATCH=160）。
//   指定した単一バイトに**一致しない**最初のバイトを検索する（前/次）。
//   不一致パターン(単一16進バイト)・検索範囲(カーソル位置/全体/選択範囲)を指定。
//   Next/Prev は閉じずに検索を実行し、キャンセルで閉じる（CFindDlg と同じモーダル挙動）。
//   検索の実処理は所有ビュー CStirlingView::FindMismatchWithByte に委譲する。
#pragma once

#include "resource.h"

class CStirlingView;

class CFindMismatchDlg : public CDialog {
public:
    explicit CFindMismatchDlg(CStirlingView* pView);

    // 検索範囲モード（CFindDlg と同一の割当）。
    enum Range { kFromCursor = 0, kWholeData = 1, kSelection = 2 };

protected:
    virtual BOOL OnInitDialog();

    void OnFindNext();     // 次検索（前方）
    void OnFindPrev();     // 前検索（後方）
    void DoFind(bool forward);

    int  CurrentRange() const;   // ラジオ選択 → Range

    CStirlingView* m_pView;
    DECLARE_MESSAGE_MAP()
};
