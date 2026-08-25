// CReplaceDlg — 置換ダイアログ（原 CReplaceDlg、IDD_REPLACE=162。モーダル）。
//   検索データ/置換データ(各16進/文字列)・置換範囲・前検索/次検索/一括置換。
//   前検索/次検索/一括置換のいずれかで検証成功したら EndDialog(IDOK) し、
//   選んだ動作(m_action)と解決済みバイト列/範囲を呼び出し側(ビュー)へ渡す。
#pragma once

#include "resource.h"
#include <vector>

class CStirlingView;

class CReplaceDlg : public CDialog {
public:
    explicit CReplaceDlg(CStirlingView* pView);

    enum Action { kNone = 0, kNext = 1, kPrev = 2, kAll = 3 };
    enum Range  { kFromCursor = 0, kWholeData = 1, kSelection = 2 };

    Action GetAction() const { return m_action; }
    int    GetRange()  const { return m_range; }
    const std::vector<unsigned char>& SearchBytes()  const { return m_searchBytes; }
    const std::vector<unsigned char>& ReplaceBytes() const { return m_replaceBytes; }

protected:
    virtual BOOL OnInitDialog();

    void OnNext() { Commit(kNext); }
    void OnPrev() { Commit(kPrev); }
    void OnAllBtn() { Commit(kAll); }
    // 入力を検証・解決し、成功なら m_action を設定して EndDialog(IDOK)。失敗はエラー表示。
    void Commit(Action action);
    // 検索/置換データを解決（16進は検証+正規化、文字列は文字セット変換）。失敗で false。
    bool ResolveField(int comboId, bool isHex, std::vector<unsigned char>& out);
    int  CurrentRange() const;

    CStirlingView* m_pView;
    Action m_action;
    int    m_range;
    std::vector<unsigned char> m_searchBytes;
    std::vector<unsigned char> m_replaceBytes;
    DECLARE_MESSAGE_MAP()
};
