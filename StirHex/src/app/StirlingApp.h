// CStirlingApp — アプリケーション本体（原 CStirlingApp : CWinApp）。
#pragma once

#include "core/CoreTypes.h"   // stirling::FileOffset（アドレスの 64bit 化。Issue #21）
#include "app/StirlingSettings.h"
#include "app/AppSettings.h"
#include "app/SettingsStore.h"
#include "app/SettingsFile.h"   // SettingsSource（保存先を決めた規則。Issue #111）
#include "util/ScopedHandle.h"

#include <map>
#include <vector>
#include <utility>

class CStirlingApp : public CWinApp {
public:
    CStirlingApp();

    virtual BOOL InitInstance();
    virtual int  ExitInstance();   // 終了時に設定を保存し、設定ファイルへ書き出す
    virtual BOOL OnIdle(LONG lCount);   // 変更があれば設定ファイルへ遅延書き出しする

    // --- 設定の永続化（Issue #96: レジストリ→設定ファイル） ---
    //   CWinApp のプロファイル API を差し替え、実体を設定ファイル（SettingsStore）にする。
    //   CAppSettings / CStirlingSettings / MRU / キャレットストア / 実行履歴は、この API
    //   越しに読み書きしているため、呼び出し側を変えずにまとめてファイル化される。
    virtual UINT    GetProfileInt(LPCTSTR lpszSection, LPCTSTR lpszEntry, int nDefault);
    virtual BOOL    WriteProfileInt(LPCTSTR lpszSection, LPCTSTR lpszEntry, int nValue);
    virtual CString GetProfileString(LPCTSTR lpszSection, LPCTSTR lpszEntry,
                                     LPCTSTR lpszDefault = NULL);
    virtual BOOL    WriteProfileString(LPCTSTR lpszSection, LPCTSTR lpszEntry,
                                       LPCTSTR lpszValue);
    virtual BOOL    GetProfileBinary(LPCTSTR lpszSection, LPCTSTR lpszEntry,
                                     LPBYTE* ppData, UINT* pBytes);
    virtual BOOL    WriteProfileBinary(LPCTSTR lpszSection, LPCTSTR lpszEntry,
                                       LPBYTE pData, UINT nBytes);

    // 実際に使用している設定ファイルのパス（環境設定「ファイル」ページに出す。Issue #111）。
    const CStringW& SettingsFilePath() const { return m_settingsPath; }
    // そのパスが探索順のどの規則で決まったか。パスだけでは「なぜそこなのか」が
    //   分からないため、UI では併せて出す（Issue #111）。
    stirling::settings::SettingsSource SettingsPathSource() const { return m_settingsSource; }
    // 読み込みに失敗した設定ファイルを温存している起動か。true の間は書き戻さない。
    bool SettingsReadOnly() const { return m_settingsReadOnly; }

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
    // 拡張子レコードの設定ストア Load/Save（近代レイアウト）。
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
    //   原は MRU 並列の配列＋独自永続化。本移植は近代レイアウト方針に合わせ、設定ストアの
    //   セクション "CaretPositions" にパス→位置を最大16件保持する。
    void LoadCaretStore();                       // 起動時に設定ストアから読み込む
    void SaveCaretStore();                       // 終了時に設定ストアへ保存する
    // 文書クローズ時: 先頭へ upsert（上限16）
    void RecordCaretPos(LPCTSTR path, stirling::FileOffset pos);
    // 復元時: 位置を返す（無ければ -1）
    stirling::FileOffset LookupCaretPos(LPCTSTR path) const;

    // マークの自動保存／自動復元（Issue #100）。設定ファイルのセクション "MarkStore" に
    //   パス→（データの大きさ, マーク）を最大 kMarkStoreMax 件保持する。
    //   OFF の間は記録も復元もしないが、読み込みだけは起動時に必ず行う。読まずにいると
    //   セッション途中で ON にしたときに空のストアで既存記録を上書きしてしまう（Issue #128）。
    void LoadMarkStore();    // 起動時に設定ファイルから読み込む（設定の ON/OFF によらず）
    void SaveMarkStore();    // 終了時に設定ファイルへ書き出す
    // 文書クローズ時: 先頭へ upsert（上限 kMarkStoreMax）。空のマークも記録する
    //   （設定 ON の間の「利用者が全て解除した」は正しく反映すべき状態のため）。
    void RecordMarks(LPCTSTR path, stirling::FileOffset size,
                     const std::map<stirling::FileOffset, int>& marks);
    // 復元時: パスが一致し、かつ記録時と大きさが同じ場合だけ true（out にマーク）。
    bool LookupMarks(LPCTSTR path, stirling::FileOffset size,
                     std::map<stirling::FileOffset, int>& out) const;

private:
    // 設定ファイルの実体（セクション→キー→値）。プロファイル API の読み書き先。
    stirling::settings::SettingsStore m_settingsStore;
    CStringW m_settingsPath;
    // 保存先を決めた規則（コマンドライン / 実行ファイル隣 / APPDATA）。
    stirling::settings::SettingsSource m_settingsSource =
        stirling::settings::SettingsSource::AppData;
    // 読み込みに失敗した設定ファイルは上書きしない（利用者の設定を壊さないため）。
    bool m_settingsReadOnly = false;
    // 保存失敗の通知は起動〜終了で1回だけにする（OnIdle 毎に出さない）。
    bool m_settingsSaveErrorShown = false;

    // 保存先を決めて設定を読み込む（初回はレジストリから移行する）。InitInstance の冒頭で呼ぶ。
    void InitSettingsStore();
    // 変更があれば設定ファイルへ書き出す。
    void FlushSettingsStore();

    // 多重起動禁止時にプロセス生存中保持するミューテックス（RAII。Issue #48）
    stirling::ScopedHandle m_singleInstanceMutex;

    // キャレット位置の自動復元ストア（パス→位置。先頭が最新。最大 kCaretStoreMax 件）。
    static const int kCaretStoreMax = 16;
    static constexpr LPCTSTR kCaretSection = _T("CaretPositions");
    std::vector<std::pair<CString, stirling::FileOffset>> m_caretStore;

    // マークの自動復元ストア（先頭が最新。最大 kMarkStoreMax 件）。
    static const int kMarkStoreMax = 16;
    static constexpr LPCTSTR kMarkSection = _T("MarkStore");
    struct MarkStoreEntry {
        CString path;
        stirling::FileOffset size = -1;   // 記録時のデータの大きさ（変化の検出用）
        std::map<stirling::FileOffset, int> marks;   // 位置 → 1..3
    };
    std::vector<MarkStoreEntry> m_markStore;

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
