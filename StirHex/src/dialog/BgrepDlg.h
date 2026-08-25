// CBgrepDlg — BGREP 設定ダイアログ（原 IDD_BGREP=172, モーダル）。
//   検索データ（16進/文字列＋キャラクタセット）・検索するファイルの種類（";" 区切りの
//   ワイルドカード）・検索フォルダ（"..." で選択）・サブフォルダ再帰・システム属性除外を
//   指定する。OK で設定を確定し、検索バイト列を組み立てる（原 FUN_00426955 前半）。
//   設定は呼び元（CMainFrame）が保持し、次回起動時の初期値となる。
#pragma once

#include "resource.h"

#include <vector>

// BGREP 設定（原 CMainFrame+0xb40.. 相当。ダイアログ跨ぎで保持）。
struct BgrepSettings {
    CStringW searchData;            // 検索データ（コンボのテキスト）
    bool     isHex = true;          // データ種別（true=16進 / false=文字列）
    int      charset = 1;           // キャラクタセット（文字列種別時。0..5, 既定=SHIFT-JIS）
    CStringW fileMask = L"*.*";     // 検索するファイルの種類（";" 区切り）
    CStringW folder;                // 検索対象フォルダ（既定=カレント。wide 層）
    bool     recurse = false;       // サブフォルダも検索する
    bool     skipSystem = false;    // システム属性ファイルは検索しない
};

class CBgrepDlg : public CDialog {
public:
    explicit CBgrepDlg(BgrepSettings* settings, CWnd* pParent = nullptr);

    // OK 後に確定した検索バイト列（16進解析 or 文字セット変換の結果）。
    const std::vector<unsigned char>& Pattern() const { return m_pattern; }

protected:
    virtual BOOL OnInitDialog();
    virtual void OnOK();

    afx_msg void OnBrowse();        // "..." → フォルダ選択（IFileDialog + FOS_PICKFOLDERS）
    afx_msg void OnTypeChanged();   // 種別変更 → キャラクタセットの有効/無効

    bool IsHexType() const;         // 16進種別が選択されているか
    void UpdateCharsetEnable();     // 文字列種別のときだけキャラクタセットを有効化

    BgrepSettings*             m_settings;   // 呼び元保持の設定（OK で書き戻し）
    std::vector<unsigned char> m_pattern;    // 確定した検索バイト列
    DECLARE_MESSAGE_MAP()
};
