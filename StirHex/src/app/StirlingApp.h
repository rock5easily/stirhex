// CStirlingApp — アプリケーション本体（原 CStirlingApp : CWinApp）。
#pragma once

#include "core/CoreTypes.h"   // stirling::FileOffset（アドレスの 64bit 化。Issue #21）
#include "app/StirlingSettings.h"
#include "app/AppSettings.h"
#include "util/ScopedHandle.h"

#include <vector>
#include <utility>

class CStirlingApp : public CWinApp {
public:
    CStirlingApp();

    virtual BOOL InitInstance();
    virtual int  ExitInstance();   // 終了時に表示設定をレジストリへ保存

    // アプリ全体で共有する内部バイナリクリップボード。
    // 原は CMainFrame が保持し WM_USER+7(取得)/WM_USER+8(格納) で受け渡すが、
    // 生存期間はアプリ全体で同一のため theApp のメンバとして保持する。
    std::vector<unsigned char> m_binClipboard;

    // 拡張子別設定レコード（原 CMainFrame+0xba0 相当）。既定 "*.*" を [0] に常設。
    std::vector<CExtRecord> m_extRecords;
    // アプリ全体の動作環境設定（原 CMainFrame 保持・環境設定ダイアログ 0x8050 が編集）。
    CAppSettings m_appSettings;
    CAppSettings& AppSettings() { return m_appSettings; }

    // 既定レコードの設定（E-1では全ビューがこれを参照）。
    CStirlingSettings& Settings() { return m_extRecords[0].s; }
    std::vector<CExtRecord>& ExtRecords() { return m_extRecords; }
    // 拡張子レコードのレジストリ Load/Save（近代レイアウト）。
    void LoadSettings();
    void SaveSettings();
    // 既定レコード "*" のコメントが空なら既定文字列で補う。theApp は静的初期化で構築され
    //   その時点ではリソースハンドルが未設定のため、InitInstance 以降で、かつレコードを
    //   作り直す LoadSettings() の後に呼ぶ（Issue #34）。
    void LoadDefaultExtComment();
    // 文書パスに対応する設定を解決（E-1では既定を返す。E-2で拡張子解決）。
    const CStirlingSettings& SettingsForPath(LPCWSTR path) const;

    // ドラッグ＆ドロップ／「送る」経由のファイルオープン。原ヘルプ準拠で、リンク
    //   ファイル(.lnk)は解決せず常にそのまま開く（CWinApp::OpenDocumentFile は
    //   CDocManager がショートカットを解決するため使わず、ドキュメントテンプレート
    //   経由で直接開く）。既に開いている場合はそのウィンドウを前面化する。
    CDocument* OpenDroppedFile(LPCTSTR path);

    // ファイル履歴（MRU）数を環境設定 fileHistoryCount に合わせて反映する（原 +0xaac,
    //   既定5・範囲外→5）。環境設定変更時に MRU リストを再構築し既存項目を保持する。
    void ApplyFileHistoryCount();

    // キャレット位置の自動復元（原ヘルプ caretAutoRestore, 原 CMainFrame+0xab0）。
    //   原は MRU 並列の配列＋独自永続化。本移植は近代レイアウト方針に合わせ、レジストリ
    //   セクション "CaretPositions" にパス→位置を最大16件保持する。
    void LoadCaretStore();                       // 起動時にレジストリから読み込む
    void SaveCaretStore();                       // 終了時にレジストリへ保存する
    // 文書クローズ時: 先頭へ upsert（上限16）
    void RecordCaretPos(LPCTSTR path, stirling::FileOffset pos);
    // 復元時: 位置を返す（無ければ -1）
    stirling::FileOffset LookupCaretPos(LPCTSTR path) const;

private:
    // 多重起動禁止時にプロセス生存中保持するミューテックス（RAII。Issue #48）
    stirling::ScopedHandle m_singleInstanceMutex;

    // キャレット位置の自動復元ストア（パス→位置。先頭が最新。最大 kCaretStoreMax 件）。
    static const int kCaretStoreMax = 16;
    static constexpr LPCTSTR kCaretSection = _T("CaretPositions");
    std::vector<std::pair<CString, stirling::FileOffset>> m_caretStore;

    // 1件分のキャレット位置の読み書き（保存形式は app/SettingsCodec.h の共通規約）。
    //   読み込みは新形式 Addr%d を優先し、無ければ旧 32bit 形式 Pos%d から移行する。
    bool ReadCaretAddr(int index, stirling::FileOffset& out);
    void WriteCaretAddr(int index, stirling::FileOffset pos);

public:
    afx_msg void OnAppAbout();
    afx_msg void OnHelpTopics();
    // ファイル>開く（原ヘルプ準拠）: linkDirect で OFN_NODEREFERENCELINKS、defaultFolder で
    //   初期フォルダを設定し、リンク非解決で開く（CDocManager の解決を避ける）。
    afx_msg void OnFileOpen();
    DECLARE_MESSAGE_MAP()
};

extern CStirlingApp theApp;

// 多重起動防止のプロセス間識別子。MainFrameとStirlingAppで共用する。
namespace stirling::single_instance {
inline constexpr wchar_t kMutexName[] = L"Local\\StirHex.SingleInstance";
inline constexpr wchar_t kMainFrameProperty[] = L"StirHex.MainFrame";
inline constexpr ULONG_PTR kMainFrameMagic = 0x53545052;   // 'STPR'
inline constexpr ULONG_PTR kCopyDataId = 0x5354464C;       // 'STFL'（file list）
}
