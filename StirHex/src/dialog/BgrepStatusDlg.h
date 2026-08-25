// CBgrepStatusDlg — BGREP 検索状況ダイアログ（原 IDD_BGREP_STATUS=175）。
//   モーダルで表示し、内部でワーカスレッド（原 FUN_00402d00 系のディレクトリ走査＋
//   ファイル毎検索）を起動する。ワーカは各ファイルの走査/ヒット/完了を本ダイアログへ
//   SendMessage で通知（原 0x414/0x415/0x417 相当）。ヒットはアウトプットペインへ追記。
//   キャンセルは停止フラグを落とすのみで、ワーカ完了通知を待って閉じる。
#pragma once

#include "resource.h"
#include "dialog/BgrepDlg.h"   // BgrepSettings

#include <vector>

class CStirlingOutputBar;

// ワーカ↔UI 通知メッセージ（原 0x414/0x415/0x417）。
#define WM_BGREP_SCAN (WM_APP + 0x20)   // wParam=ファイルサイズ, lParam=(LPCWSTR)フルパス
#define WM_BGREP_HIT  (WM_APP + 0x21)   // wParam=オフセット, lParam=(LPCWSTR)フルパス
#define WM_BGREP_DONE (WM_APP + 0x22)   // 走査完了（またはキャンセル）

// ワーカへ渡す走査ジョブ（ダイアログが所有し、ワーカ生存中は保持）。
struct BgrepJob {
    HWND notify = nullptr;                   // 通知先（本ダイアログ HWND）
    std::vector<unsigned char> pattern;      // 検索バイト列
    std::vector<CStringW> masks;             // 検索対象のワイルドカード群
    CStringW root;                           // 検索起点フォルダ（wide 層）
    bool     recurse = false;                // サブフォルダ再帰
    bool     skipSystem = false;             // システム属性ファイル除外
    volatile LONG running = 1;               // 1=継続 / 0=停止要求（キャンセル）
};

class CBgrepStatusDlg : public CDialog {
public:
    CBgrepStatusDlg(const std::vector<unsigned char>& pattern, const BgrepSettings& s,
                    CStirlingOutputBar* outbar, CWnd* pParent);

protected:
    virtual BOOL OnInitDialog();
    virtual void OnCancel();   // 停止要求のみ（完了通知で閉じる）

    afx_msg LRESULT OnScan(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnHit(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnDone(WPARAM wParam, LPARAM lParam);

    BgrepJob            m_job;
    CStirlingOutputBar* m_outbar;
    int                 m_hitCount;
    int                 m_fileCount;
    DECLARE_MESSAGE_MAP()
};
