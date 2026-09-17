// CFindDlg — 検索ダイアログ（原 CSearchDlg、IDD_FIND=161。モーダル）。
//   検索データ(16進/文字列)・検索範囲(カーソル位置/全体/選択範囲)・前/次検索。
//   Next/Prev は閉じずに検索を実行し、キャンセルで閉じる（原と同じモーダル挙動）。
//   検索の実処理は所有ビュー CStirlingView::FindFromDialog に委譲する。
//   [全て検索]（移植版で追加。Issue #236）は条件を確定して IDC_FIND_ALL で閉じ、
//   所有ビューが FindAllRequest() を受け取って検索結果一覧を開く。
#pragma once

#include "resource.h"
#include "core/HexPattern.h"

class CStirlingView;

class CFindDlg : public CDialog {
public:
    explicit CFindDlg(CStirlingView* pView);

    // 検索範囲モード（原 view+0x23c 相当のUI選択）。
    enum Range { kFromCursor = 0, kWholeData = 1, kSelection = 2 };

    // [全て検索] で確定した条件（DoModal が IDC_FIND_ALL を返したときだけ有効）。
    struct FindAllRequest {
        stirling::HexPattern pattern;
        CStringW display;        // 条件表示用の検索データ（16進は整形後、文字列は入力のまま）
        bool isHex = true;
        int rangeMode = kFromCursor;
    };
    const FindAllRequest& GetFindAllRequest() const { return m_findAll; }

protected:
    virtual BOOL OnInitDialog();

    void OnFindNext();     // 次検索（前方）
    void OnFindPrev();     // 前検索（後方）
    void OnFindAll();      // 全て検索（条件を確定して閉じる）
    void DoFind(bool forward);
    // 検索データを解決（16進は検証+正規化、文字列は文字セット変換）。失敗はエラー表示して false。
    bool ResolvePattern(stirling::HexPattern& pattern, CStringW& display);

    int  CurrentRange() const;      // ラジオ選択 → Range
    bool IsHexType() const;         // 16進種別か（false=文字列）

    CStirlingView* m_pView;
    FindAllRequest m_findAll;
    DECLARE_MESSAGE_MAP()
};
