// CStirlingApp 実装。MDI ドキュメントテンプレートを登録して起動する。
#include "pch.h"
#include "app/UiStrings.h"   // ui::MsgBox（表題はアプリ名で統一）
#include <afxadv.h>   // CRecentFileList（ファイル履歴 MRU）の完全定義
#include <afxole.h>   // AfxOleInit（MRUのシェル項目生成に必要）
#include <shobjidl.h> // SetCurrentProcessExplicitAppUserModelID
#include "resource.h"
#include "app/StirlingApp.h"
#include "app/SettingsCodec.h"       // 64bit 設定値の保存形式（Issue #22）
#include "app/MarkFile.h"        // マークの1行表現（自動復元ストア。Issue #100）
#include "app/SettingsFile.h"        // 設定ファイルの保存先解決・入出力（Issue #96）
#include "app/ShellUtil.h"           // ui::FullPath（MAX_PATH 非依存のパス解決）
#include "frame/MainFrame.h"
#include "frame/ChildFrame.h"
#include "doc/StirlingDoc.h"
#include "view/StirlingView.h"

BEGIN_MESSAGE_MAP(CStirlingApp, CWinApp)
    ON_COMMAND(ID_APP_ABOUT, &CStirlingApp::OnAppAbout)
    ON_COMMAND(ID_HELP_TOPICS, &CStirlingApp::OnHelpTopics)
    ON_COMMAND(ID_FILE_NEW, &CWinApp::OnFileNew)
    ON_COMMAND(ID_FILE_OPEN, &CStirlingApp::OnFileOpen)   // リンク非解決/既定フォルダ対応の独自オープン
    // プリンタの設定（標準MFC。CWinApp が印刷設定ダイアログを表示）
    ON_COMMAND(ID_FILE_PRINT_SETUP, &CWinApp::OnFilePrintSetup)
END_MESSAGE_MAP()

CStirlingApp::CStirlingApp() {
    // 既定レコード（拡張子="*"→表示は"(*.*)"、すべてのファイル）を常設。
    //   Settings() が常に有効になるよう先頭に置く。
    // コメント文字列はここでは読まない。theApp は静的初期化で構築され、その時点では
    //   MFC のリソースハンドル（afxCurrentResourceHandle）が未設定のため、
    //   AfxGetResourceHandle() の ASSERT を踏むうえ文字列も取得できない（Issue #34）。
    //   実際の文字列は InitInstance の LoadDefaultExtComment() で埋める。
    CExtRecord def;
    def.ext = L"*";
    m_extRecords.push_back(def);
}

// 既定レコード（先頭の "*"）のコメントが空なら、リソースの既定文字列で補う。
//   MFC がリソースハンドルを設定した後（InitInstance 以降）かつ LoadSettings() の後に呼ぶ
//   （Issue #34）。LoadSettings() は保存済み設定があると m_extRecords を作り直すため、
//   先に呼ぶと結果が捨てられる。
//   空のときだけ埋めるので、利用者が編集したコメントを上書きしない。また、修正前の
//   ビルドが空コメントのまま保存したレジストリも、この経路で既定値へ復帰する。
void CStirlingApp::LoadDefaultExtComment() {
    if (!m_extRecords.empty() && m_extRecords[0].ext == L"*" &&
        m_extRecords[0].comment.IsEmpty()) {
        m_extRecords[0].comment = ui::LoadW(6030);   // 原 6030「すべてのファイル」
    }
}

CStirlingApp theApp;

namespace {
// MFC標準 CCommandLineInfo は最初のファイル名しか保持しない。原版は解析後の
// ファイル引数配列を順に開くため、通常の非フラグ引数を別途保持する。
class CStirlingCommandLineInfo : public CCommandLineInfo {
public:
    std::vector<CString> fileNames;

    virtual void ParseParam(const TCHAR* pszParam, BOOL bFlag, BOOL bLast) override {
        if (!bFlag && pszParam != nullptr && *pszParam != _T('\0')) {
            fileNames.push_back(pszParam);   // CRTが引用符を除去済み。空白入りパスも1要素になる。
        }
        CCommandLineInfo::ParseParam(pszParam, bFlag, bLast);
    }
};

struct FindMainFrameContext {
    HWND hwnd = nullptr;
};

BOOL CALLBACK FindMainFrameProc(HWND hwnd, LPARAM lParam) {
    FindMainFrameContext* context = reinterpret_cast<FindMainFrameContext*>(lParam);
    const HANDLE value = ::GetPropW(hwnd, stirling::single_instance::kMainFrameProperty);
    if (reinterpret_cast<ULONG_PTR>(value) == stirling::single_instance::kMainFrameMagic) {
        context->hwnd = hwnd;
        return FALSE;
    }
    return TRUE;
}

HWND FindExistingMainFrame(DWORD timeoutMs) {
    // 49.7 日で折り返す GetTickCount ではなく 64bit 版を使う（C28159）。
    const ULONGLONG started = ::GetTickCount64();
    do {
        FindMainFrameContext context;
        ::EnumWindows(FindMainFrameProc, reinterpret_cast<LPARAM>(&context));
        if (context.hwnd != nullptr) return context.hwnd;
        ::Sleep(50);
    } while (::GetTickCount64() - started < timeoutMs);
    return nullptr;
}

CStringW ToWidePath(const CString& path) {
#ifdef _UNICODE
    return CStringW(path);
#else
    CStringW wide;
    const int length = ::MultiByteToWideChar(CP_ACP, 0, path, -1, nullptr, 0);
    if (length > 0) {
        ::MultiByteToWideChar(CP_ACP, 0, path, -1, wide.GetBuffer(length), length);
        wide.ReleaseBuffer();
    }
    return wide;
#endif
}

CStringW AbsolutePathForTransfer(const CString& path) {
    // MAX_PATH 非依存のパス解決は ui::FullPath に集約（解決できなければ入力をそのまま返す）。
    return ui::FullPath(ToWidePath(path));
}

std::vector<wchar_t> BuildFileTransferPayload(const std::vector<CString>& fileNames) {
    std::vector<wchar_t> payload;
    for (const CString& fileName : fileNames) {
        const CStringW fullPath = AbsolutePathForTransfer(fileName);
        if (fullPath.IsEmpty()) continue;
        payload.insert(payload.end(), fullPath.GetString(),
                       fullPath.GetString() + fullPath.GetLength());
        payload.push_back(L'\0');
    }
    if (!payload.empty()) payload.push_back(L'\0');   // MULTI_SZ終端
    return payload;
}

bool ForwardToExistingInstance(const CStirlingCommandLineInfo& cmdInfo) {
    HWND hwnd = FindExistingMainFrame(10000);
    if (hwnd == nullptr) return false;

    if (::IsIconic(hwnd)) ::ShowWindow(hwnd, SW_RESTORE);
    HWND foreground = ::GetLastActivePopup(hwnd);
    if (foreground == nullptr) foreground = hwnd;
    ::SetForegroundWindow(foreground);

    if (cmdInfo.m_nShellCommand != CCommandLineInfo::FileOpen || cmdInfo.fileNames.empty()) {
        return true;   // ファイル指定なしの2回目起動は前面化のみ
    }

    std::vector<wchar_t> payload = BuildFileTransferPayload(cmdInfo.fileNames);
    if (payload.empty() || payload.size() > (16u * 1024u * 1024u) / sizeof(wchar_t)) {
        return false;
    }
    COPYDATASTRUCT copyData = { 0 };
    copyData.dwData = stirling::single_instance::kCopyDataId;
    copyData.cbData = static_cast<DWORD>(payload.size() * sizeof(wchar_t));
    copyData.lpData = payload.data();
    DWORD_PTR receiverResult = 0;
    const LRESULT sent = ::SendMessageTimeoutW(
        hwnd, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&copyData),
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 30000, &receiverResult);
    return sent != 0 && receiverResult != 0;
}

// レコード i の設定セクション名 "Rec<i>"。
CString RecSection(int i) { CString s; s.Format(_T("Rec%d"), i); return s; }
}

// 拡張子レコードを設定ストアから読み込む（無ければ既定 "*.*" 1件のまま）。
void CStirlingApp::LoadSettings() {
    const int count = GetProfileInt(_T("Extensions"), _T("Count"), 0);
    if (count <= 0) {
        // 未保存: 既定レコードの設定のみ復元（旧 D-2a 互換で Rec0 から）。
        m_extRecords[0].s.Load(RecSection(0));
        return;
    }
    m_extRecords.clear();
    for (int i = 0; i < count; ++i) {
        CExtRecord rec;
        CString ekey; ekey.Format(_T("Ext%d"), i);
        CString ckey; ckey.Format(_T("Comment%d"), i);
        rec.ext     = GetProfileString(_T("Extensions"), ekey, _T("*.*"));
        rec.comment = GetProfileString(_T("Extensions"), ckey, _T(""));
        rec.s.Load(RecSection(i));
        m_extRecords.push_back(rec);
    }
    // 既定 "*"（すべてのファイル）が先頭に無ければ補う。
    if (m_extRecords.empty() || m_extRecords[0].ext != L"*") {
        CExtRecord def; def.ext = L"*"; def.comment = ui::LoadW(6030);   // 原 6030「すべてのファイル」
        m_extRecords.insert(m_extRecords.begin(), def);
    }
}

// 拡張子レコードを設定ストアへ保存する。
void CStirlingApp::SaveSettings() {
    const int count = (int)m_extRecords.size();
    WriteProfileInt(_T("Extensions"), _T("Count"), count);
    for (int i = 0; i < count; ++i) {
        CString ekey; ekey.Format(_T("Ext%d"), i);
        CString ckey; ckey.Format(_T("Comment%d"), i);
        WriteProfileString(_T("Extensions"), ekey, m_extRecords[i].ext);
        WriteProfileString(_T("Extensions"), ckey, m_extRecords[i].comment);
        m_extRecords[i].s.Save(RecSection(i));
    }
}

// 文書パスに対応する設定を拡張子で解決する（原 FUN_00427965 後の各ビュー再解決に相当）。
//   拡張子（最後の '.' 以降、大文字）を、既定[0]以外の各レコードの ext（';' 区切り複数可）と
//   照合し、最初に一致したレコードの設定を返す。一致なしは既定[0]。
const CStirlingSettings& CStirlingApp::SettingsForPath(LPCWSTR path) const {
    if (path != nullptr && *path != L'\0') {
        CStringW p(path);
        const int dot = p.ReverseFind(L'.');
        const int sep = p.ReverseFind(L'\\');
        if (dot >= 0 && dot > sep) {
            CStringW ext = p.Mid(dot + 1);
            ext.MakeUpper();
            for (size_t i = 1; i < m_extRecords.size(); ++i) {
                CStringW pat = m_extRecords[i].ext;
                pat.MakeUpper();
                int pos = 0;
                for (CStringW tok = pat.Tokenize(L";", pos); !tok.IsEmpty();
                     tok = pat.Tokenize(L";", pos)) {
                    tok.Trim();
                    if (tok == ext) { return m_extRecords[i].s; }
                }
            }
        }
    }
    return m_extRecords[0].s;   // 既定 "*"
}

// ドラッグ＆ドロップ／「送る」経由のオープン。原ヘルプ: この経路では常にリンク
//   ファイル(.lnk)そのものを開く（リンク先へ解決しない）。CWinApp::OpenDocumentFile は
//   CDocManager がショートカットを解決してしまうため使わず、ドキュメントテンプレートの
//   OpenDocumentFile を直接呼ぶ（解決は行われない）。既に同一パスを開いていれば前面化。
CDocument* CStirlingApp::OpenDroppedFile(LPCTSTR path) {
    if (path == nullptr || *path == _T('\0')) { return nullptr; }
    // MAX_PATH 非依存で解決する。解決できなければ入力パスのまま扱う（原の挙動）。
    const CStringW full = ui::FullPath(path);
    POSITION posT = GetFirstDocTemplatePosition();
    if (posT == nullptr) { return nullptr; }
    CDocTemplate* pTemplate = GetNextDocTemplate(posT);
    if (pTemplate == nullptr) { return nullptr; }

    // 既に開いていれば重複させずアクティブ化（CDocManager のリンク解決を伴わない版）。
    POSITION posD = pTemplate->GetFirstDocPosition();
    while (posD != nullptr) {
        CDocument* pDoc = pTemplate->GetNextDoc(posD);
        if (pDoc != nullptr && pDoc->GetPathName().CompareNoCase(full) == 0) {
            POSITION posV = pDoc->GetFirstViewPosition();
            if (posV != nullptr) {
                CView* pView = pDoc->GetNextView(posV);
                if (pView != nullptr && pView->GetParentFrame() != nullptr) {
                    pView->GetParentFrame()->ActivateFrame();
                }
            }
            return pDoc;
        }
    }
    // 未オープン: リンク解決せずそのまま開く。
    CDocument* pDoc = pTemplate->OpenDocumentFile(full);
    if (pDoc != nullptr) { AddToRecentFileList(full); }
    return pDoc;
}

// === 設定の永続化（Issue #96: レジストリ→設定ファイル） ===
namespace {
// プロファイル API の引数検査。MFC 版は ASSERT で落とすが、ここでは無効な呼び出しを
//   既定値で受け流す（設定ファイル層の都合でアプリを落とさない）。
bool ValidProfileEntry(LPCTSTR section, LPCTSTR entry) {
    return section != nullptr && *section != _T('\0') &&
           entry != nullptr && *entry != _T('\0');
}
}  // namespace

// 保存先を決めて設定を読み込む。読み込みに失敗した場合はそのファイルを上書きしない。
void CStirlingApp::InitSettingsStore() {
    const stirling::settings::SettingsLocation location =
        stirling::settings::ResolveSettingsLocation();
    m_settingsPath = location.path.c_str();
    m_settingsSource = location.source;
    // 設定ファイルがまだ無い＝初回起動。読み込みの前に見ておく。
    const bool firstRun = !stirling::settings::SettingsFileExists(location.path);

    std::wstring error;
    if (!stirling::settings::LoadSettingsFile(location.path, m_settingsStore, error)) {
        // 壊れた（あるいは読めない）設定ファイルは書き換えない。利用者が中身を確認して
        //   直せるよう温存し、この起動は既定値で動かす。
        m_settingsReadOnly = true;
        m_settingsStore.Clear();
        m_settingsStore.ClearDirty();
        CStringW message;
        message.Format(ui::LoadW(IDS_ERR_SETTINGS_LOAD), error.c_str());
        ui::MsgBox(nullptr, message);
        return;
    }
    m_settingsStore.ClearDirty();   // 読み込みは「変更」ではない

    if (firstRun) {
        // 初回起動。1.1.0 以前がレジストリへ保存していた設定を引き継ぐ。
        //   旧キーは消さない（旧バージョンへ戻せるようにする）。取り込めた場合は
        //   ストアが dirty のままになり、この後の書き出しで設定ファイルが作られる。
        stirling::settings::ImportFromRegistry(
            stirling::settings::kLegacyRegistryKey, m_settingsStore);
    }
}

// 変更があれば設定ファイルへ書き出す。書き込みに失敗したら通知は1回だけ行う。
void CStirlingApp::FlushSettingsStore() {
    if (m_settingsReadOnly || !m_settingsStore.Dirty()) { return; }

    std::wstring error;
    // 複数インスタンスが同じ設定ファイルを使う場合に備え、最新の内容へこのプロセスの
    //   変更だけを適用して書き戻す（Issue #130）。ClearDirty は保存側が行う。
    if (stirling::settings::SaveSettingsFileMerged(
            static_cast<LPCWSTR>(m_settingsPath), m_settingsStore, error)) {
        return;
    }
    if (!m_settingsSaveErrorShown) {
        m_settingsSaveErrorShown = true;   // アイドル毎の再試行と重複通知を止める
        CStringW message;
        message.Format(ui::LoadW(IDS_ERR_SETTINGS_SAVE), error.c_str());
        ui::MsgBox(nullptr, message);
    }
}

// 設定変更をためこまず、アイドル時に書き戻す（異常終了で設定を失いにくくする）。
BOOL CStirlingApp::OnIdle(LONG lCount) {
    const BOOL more = CWinApp::OnIdle(lCount);
    if (lCount == 0 && !m_settingsSaveErrorShown) {
        FlushSettingsStore();
    }
    return more;
}

UINT CStirlingApp::GetProfileInt(LPCTSTR lpszSection, LPCTSTR lpszEntry, int nDefault) {
    if (!ValidProfileEntry(lpszSection, lpszEntry)) { return static_cast<UINT>(nDefault); }
    const std::wstring* value = m_settingsStore.Find(lpszSection, lpszEntry);
    if (value == nullptr) { return static_cast<UINT>(nDefault); }
    // 負値（キャレット位置の旧形式 -1 等）も往復するため符号付きで解釈する。
    return static_cast<UINT>(_wtoi(value->c_str()));
}

BOOL CStirlingApp::WriteProfileInt(LPCTSTR lpszSection, LPCTSTR lpszEntry, int nValue) {
    if (!ValidProfileEntry(lpszSection, lpszEntry)) { return FALSE; }
    m_settingsStore.Set(lpszSection, lpszEntry, std::to_wstring(nValue));
    return TRUE;
}

CString CStirlingApp::GetProfileString(LPCTSTR lpszSection, LPCTSTR lpszEntry,
                                       LPCTSTR lpszDefault) {
    const CString fallback = (lpszDefault != nullptr) ? CString(lpszDefault) : CString();
    if (!ValidProfileEntry(lpszSection, lpszEntry)) { return fallback; }
    const std::wstring* value = m_settingsStore.Find(lpszSection, lpszEntry);
    if (value == nullptr) { return fallback; }
    return CString(value->c_str());
}

BOOL CStirlingApp::WriteProfileString(LPCTSTR lpszSection, LPCTSTR lpszEntry,
                                      LPCTSTR lpszValue) {
    if (lpszSection == nullptr || *lpszSection == _T('\0')) { return FALSE; }
    if (lpszEntry == nullptr) {
        // MFC の規約: エントリ名 NULL はセクションごと削除する。
        m_settingsStore.RemoveSection(lpszSection);
        return TRUE;
    }
    if (*lpszEntry == _T('\0')) { return FALSE; }
    if (lpszValue == nullptr) {
        m_settingsStore.Remove(lpszSection, lpszEntry);   // 値の削除
        return TRUE;
    }
    m_settingsStore.Set(lpszSection, lpszEntry, lpszValue);
    return TRUE;
}

BOOL CStirlingApp::GetProfileBinary(LPCTSTR lpszSection, LPCTSTR lpszEntry,
                                    LPBYTE* ppData, UINT* pBytes) {
    if (ppData == nullptr || pBytes == nullptr) { return FALSE; }
    *ppData = nullptr;
    *pBytes = 0;
    if (!ValidProfileEntry(lpszSection, lpszEntry)) { return FALSE; }
    const std::wstring* value = m_settingsStore.Find(lpszSection, lpszEntry);
    if (value == nullptr) { return FALSE; }
    std::vector<unsigned char> bytes;
    if (!stirling::settings::HexToBytes(*value, bytes) || bytes.empty()) { return FALSE; }
    // 呼び出し側が delete[] で解放する（CWinApp::GetProfileBinary と同じ規約）。
    *ppData = new BYTE[bytes.size()];
    ::memcpy(*ppData, bytes.data(), bytes.size());
    *pBytes = static_cast<UINT>(bytes.size());
    return TRUE;
}

BOOL CStirlingApp::WriteProfileBinary(LPCTSTR lpszSection, LPCTSTR lpszEntry,
                                      LPBYTE pData, UINT nBytes) {
    if (!ValidProfileEntry(lpszSection, lpszEntry)) { return FALSE; }
    if (pData == nullptr && nBytes != 0) { return FALSE; }
    m_settingsStore.Set(lpszSection, lpszEntry,
                        stirling::settings::BytesToHex(pData, nBytes));
    return TRUE;
}

BOOL CStirlingApp::InitInstance() {
    CWinApp::InitInstance();

    if (FAILED(::SetCurrentProcessExplicitAppUserModelID(L"StirHexProject.StirHex"))) {
        AfxMessageBox(ui::LoadW(IDS_ERR_APP_ID), MB_OK | MB_ICONERROR);
        return FALSE;
    }

    // CWinApp::AddToRecentFileList は Windows 7 以降、ジャンプリスト連携のため
    // SHCreateItemFromParsingName を使用する。コマンドライン指定ファイルは通常の
    // ファイルダイアログより先にMRUへ追加されるため、ここでOLE/COMを初期化しておく。
    if (!AfxOleInit()) {
        AfxMessageBox(ui::LoadW(IDS_ERR_OLE_INIT),
                      MB_OK | MB_ICONERROR);
        return FALSE;
    }

    // 設定の永続化先（設定ファイル。Issue #96）。プロファイル API を差し替えているため
    //   SetRegistryKey は呼ばない。以降の Load 系は全てこのストア越しに読む。
    InitSettingsStore();
    LoadSettings();          // 起動時に拡張子レコード（表示設定）を復元（原 FUN_0041f2a5 相当）
    LoadDefaultExtComment(); // 既定レコードのコメント（静的初期化では読めない。Issue #34）
    m_appSettings.Load();    // アプリ全体の動作環境設定（環境設定 0x8050）を復元
    LoadCaretStore();        // キャレット位置の自動復元ストア（caretAutoRestore）を復元
    LoadMarkStore();         // マークの自動復元ストア（markAutoRestore）を復元

    // コマンドラインは単一起動判定前に解析し、2回目起動のファイル列を既存プロセスへ転送する。
    CStirlingCommandLineInfo cmdInfo;
    ParseCommandLine(cmdInfo);
    if (cmdInfo.m_nShellCommand == CCommandLineInfo::FileNew) {
        cmdInfo.m_nShellCommand = CCommandLineInfo::FileNothing;
    }

    if (!m_appSettings.allowMultipleInstances) {
        // GetLastError() は後続の呼び出しで壊れうるため、CreateMutexW 直後に退避する
        //   （既存インスタンスの判定 ERROR_ALREADY_EXISTS がこの値に依存する）。
        const HANDLE hMutex = ::CreateMutexW(
            nullptr, FALSE, stirling::single_instance::kMutexName);
        const DWORD createError = ::GetLastError();
        m_singleInstanceMutex.Reset(hMutex);
        if (!m_singleInstanceMutex.Valid()) {
            AfxMessageBox(ui::LoadW(IDS_ERR_MUTEX),
                          MB_OK | MB_ICONERROR);
            return FALSE;
        }
        if (createError == ERROR_ALREADY_EXISTS) {
            m_singleInstanceMutex.Close();
            if (!ForwardToExistingInstance(cmdInfo)) {
                AfxMessageBox(ui::LoadW(IDS_ERR_TRANSFER),
                              MB_OK | MB_ICONERROR);
            }
            return FALSE;
        }
    }

    // ファイル履歴（MRU）を有効化。数は環境設定 fileHistoryCount（原 +0xaac, 範囲外→5）。
    //   LoadStdProfileSettings が m_pRecentFileList を生成し、差し替え済み Profile API 経由で
    //   設定ストアから履歴を読み込む。初回起動時は旧レジストリから取り込んだ値も含む。
    {
        int mru = m_appSettings.fileHistoryCount;
        if (mru < 2 || mru > 16) { mru = 5; }   // 原 FUN_0041f2a5 のクランプに合わせる
        LoadStdProfileSettings(static_cast<UINT>(mru));
    }

    // 相違一覧ダイアログ（IDD_DIFF_LIST）等で使う SysListView32 のため共通コントロールを初期化。
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES };
    ::InitCommonControlsEx(&icc);

    // MDI ドキュメントテンプレート（Doc/View/ChildFrame の結線）
    CMultiDocTemplate* pDocTemplate = new CMultiDocTemplate(
        IDR_STIRLINGTYPE,
        RUNTIME_CLASS(CStirlingDoc),
        RUNTIME_CLASS(CChildFrame),
        RUNTIME_CLASS(CStirlingView));
    if (pDocTemplate == nullptr) {
        return FALSE;
    }
    AddDocTemplate(pDocTemplate);

    // メインフレーム生成
    CMainFrame* pMainFrame = new CMainFrame;
    if (pMainFrame == nullptr || !pMainFrame->LoadFrame(IDR_MAINFRAME)) {
        delete pMainFrame;
        return FALSE;
    }
    m_pMainWnd = pMainFrame;

    // 起動時に空ドキュメントは自動生成しない。コマンドラインは単一起動判定前に解析済み。
    // 「送る」/コマンドライン経由のファイルオープンは、原ヘルプ準拠でリンクファイル(.lnk)を
    //   解決せずそのまま開く（「送る」は選択ファイルのパスを引数として渡すため D&D と同じ扱い）。
    //   通常ファイルは OpenDroppedFile が OpenDocumentFile 相当に振る舞う（重複回避＋MRU登録）。
    if (cmdInfo.m_nShellCommand == CCommandLineInfo::FileOpen &&
        !cmdInfo.fileNames.empty()) {
        // 原版 CStirlingApp::InitInstance と同じく指定順に開く。各 OpenDroppedFile が
        // 対象MDI子を活性化するため、最後の引数が最終的なアクティブ文書になる。
        for (const CString& path : cmdInfo.fileNames) {
            if (OpenDroppedFile(path) == nullptr) {
                // 原: 開けないファイルを指定されたら「%sが見つかりません」(文字列1054)を
                //   フルパス付きで表示し、メインウィンドウを見せずに終了する（実測）。
                const CStringW full = ui::FullPath(path);
                CStringW msg;
                msg.Format(ui::LoadW(IDS_FILE_NOT_FOUND), full.GetString());
                ui::MsgBox(nullptr, msg, MB_OK | MB_ICONEXCLAMATION);
                return FALSE;
            }
        }
    } else if (!ProcessShellCommand(cmdInfo)) {
        return FALSE;
    }

    // メインウィンドウのサイズ・位置（原ヘルプ winPlacement, CMainFrame::OnCreate FUN_0041e8a1）:
    //   0=指定しない(既定表示) / 1=前回終了時 / 2=最大化 / 3=指定。
    //   原は 1/3 とも SW_SHOWNORMAL＋rcNormalPosition を SetWindowPlacement（最大化状態は復元しない）。
    //   1(前回) の rect は終了時に winLeft/Top/Width/Height へ保存したもの、3(指定) はダイアログ指定値。
    const CAppSettings& s = m_appSettings;
    switch (s.winPlacement) {
    case 2:   // 最大化
        pMainFrame->ShowWindow(SW_SHOWMAXIMIZED);
        break;
    case 1:   // 前回終了時
    case 3: { // 指定
        WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
        wp.showCmd = SW_SHOWNORMAL;
        wp.rcNormalPosition.left   = s.winLeft;
        wp.rcNormalPosition.top    = s.winTop;
        wp.rcNormalPosition.right  = s.winLeft + s.winWidth;
        wp.rcNormalPosition.bottom = s.winTop + s.winHeight;
        pMainFrame->SetWindowPlacement(&wp);
        break;
    }
    case 0:   // 指定しない
    default:
        pMainFrame->ShowWindow(m_nCmdShow);
        break;
    }
    // 原 CMainFrame::OnCreate はメインウィンドウ配置確定後にビットイメージを
    //   本体左側へフローティングし、その後 bitImageDockable を適用する。
    pMainFrame->FinalizeInitialBitImagePlacement();
    pMainFrame->UpdateWindow();
    return TRUE;
}

// バージョン情報ダイアログ
class CAboutDlg : public CDialog {
public:
    CAboutDlg() : CDialog(IDD_ABOUTBOX) {}
};

void CStirlingApp::OnAppAbout() {
    CAboutDlg dlg;
    dlg.DoModal();
}

void CStirlingApp::OnHelpTopics() {
    CStringW helpPath = ui::ModuleDirectory();
    helpPath += L"help\\index.html";
    const DWORD attributes = ::GetFileAttributesW(helpPath);
    HWND owner = AfxGetMainWnd() != nullptr ? AfxGetMainWnd()->GetSafeHwnd() : nullptr;
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        CStringW message;
        message.Format(ui::LoadW(IDS_ERR_HELP_NOT_FOUND), helpPath.GetString());
        ui::MsgBox(owner, message, MB_OK | MB_ICONERROR);
        return;
    }

    DWORD error = ERROR_SUCCESS;
    if (!ui::ShellExecuteFile(owner, helpPath, error)) {
        const CStringW message = ui::AppendErrorReason(ui::LoadW(IDS_ERR_HELP_OPEN), error);
        ui::MsgBox(owner, message, MB_OK | MB_ICONERROR);
    }
}

// ファイル>開く。原ヘルプ準拠でリンクファイル(.lnk)の扱いを linkDirect で切り替える。
//   linkDirect ON: OFN_NODEREFERENCELINKS を付与しダイアログがリンクを解決しない→ .lnk 自体を開く。
//   linkDirect OFF: 既定どおりダイアログがリンク先へ解決したパスを返す。
//   defaultFolderSpecify 時は初期フォルダを defaultFolder に設定する。
//   開くのは OpenDroppedFile（CDocManager の再解決を避ける）: OFF でも解決済パスなので結果は同一。
void CStirlingApp::OnFileOpen() {
    const CAppSettings& s = m_appSettings;
    DWORD flags = OFN_HIDEREADONLY | OFN_FILEMUSTEXIST | OFN_EXPLORER;
    if (s.linkDirect) { flags |= OFN_NODEREFERENCELINKS; }
    const CStringW filter = ui::LoadW(IDS_FILTER_ALL_FILES);
    CFileDialog dlg(TRUE, nullptr, nullptr, flags, filter, AfxGetMainWnd());
    const CStringW& initDir = s.defaultFolder;
    if (s.defaultFolderSpecify && !initDir.IsEmpty()) {
        dlg.m_ofn.lpstrInitialDir = initDir;   // DoModal 終了まで生存する必要がある
    }
    if (dlg.DoModal() != IDOK) { return; }
    OpenDroppedFile(dlg.GetPathName());
}

// ファイル履歴（MRU）数を環境設定に合わせて再構築する（環境設定 OK 時に呼ぶ）。
//   MFC の CRecentFileList は上限変更をサポートしないため、既存項目を退避して作り直す。
void CStirlingApp::ApplyFileHistoryCount() {
    int n = m_appSettings.fileHistoryCount;
    if (n < 2 || n > 16) { n = 5; }
    // 既存履歴を順序どおり退避（[0] が最新）。
    std::vector<CString> saved;
    if (m_pRecentFileList != nullptr) {
        for (int i = 0; i < m_pRecentFileList->GetSize(); ++i) {
            const CString& s = (*m_pRecentFileList)[i];
            if (!s.IsEmpty()) { saved.push_back(s); }
        }
    }
    delete m_pRecentFileList;
    // MFC 既定のプロファイル節・値名（_afxFileSection / _afxFileEntry）に合わせる。
    m_pRecentFileList = new CRecentFileList(0, _T("Recent File List"), _T("File%d"), n);
    // 退避分を末尾側から Add（Add は先頭挿入のため逆順で元の並びを復元）。上限超過分は自然に脱落。
    for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
        m_pRecentFileList->Add(*it);
    }
}

// --- キャレット位置の自動復元ストア（原 caretAutoRestore。近代レイアウト: セクション
//     "CaretPositions" に Count / Path%d(SJIS) / Addr%d を最大16件保持） ---
//   Addr%d は設定ストアに保存する64bitアドレスの16進文字列（app/SettingsCodec.h の共通規約）。
//   Pos%d は 32bit 版が使っていた旧形式（REG_DWORD）で、読み込み時の移行専用。

void CStirlingApp::LoadCaretStore() {
    m_caretStore.clear();
    int count = GetProfileInt(kCaretSection, _T("Count"), 0);
    if (count > kCaretStoreMax) { count = kCaretStoreMax; }
    for (int i = 0; i < count; ++i) {
        CString pkey; pkey.Format(_T("Path%d"), i);
        CString path = GetProfileString(kCaretSection, pkey, _T(""));
        if (path.IsEmpty()) { continue; }
        stirling::FileOffset pos = -1;
        if (!ReadCaretAddr(i, pos)) { continue; }
        if (pos >= 0) { m_caretStore.emplace_back(path, pos); }
    }
}

void CStirlingApp::SaveCaretStore() {
    const int count = static_cast<int>(m_caretStore.size());
    WriteProfileInt(kCaretSection, _T("Count"), count);
    for (int i = 0; i < count; ++i) {
        CString pkey; pkey.Format(_T("Path%d"), i);
        WriteProfileString(kCaretSection, pkey, m_caretStore[i].first);
        WriteCaretAddr(i, m_caretStore[i].second);
    }
}

// 保存済みキャレット位置を1件読む。新形式 Addr%d を優先し、無ければ旧形式 Pos%d から移行読み込みする。
//   戻り値: 値を読めたら true（out に位置。破損値は読めなかった扱い）。
bool CStirlingApp::ReadCaretAddr(int index, stirling::FileOffset& out) {
    CString akey; akey.Format(_T("Addr%d"), index);
    const CString addr = GetProfileString(kCaretSection, akey, _T(""));
    if (!addr.IsEmpty()) {
        // SettingsCodec は ASCII 層（16進表記のみ）だが、ワイド版を直接使う。
        //   CStringA を挟むとシステム ANSI コードページを経由してしまうため（Issue #43）。
        if (stirling::settings::ParseOffsetHex(static_cast<LPCWSTR>(addr), out)) { return true; }
        // 破損（手編集等）した値は既定へフォールバックせず、その1件を捨てる。
        TRACE(_T("CaretPositions %s: 16進として解釈できない値を無視しました\n"), (LPCTSTR)akey);
        return false;
    }
    // --- 旧形式（32bit 版が REG_DWORD で保存した Pos%d）からの移行読み込み ---
    CString vkey; vkey.Format(_T("Pos%d"), index);
    const int legacy = GetProfileInt(kCaretSection, vkey, -1);
    if (legacy < 0) { return false; }
    out = legacy;
    return true;
}

// キャレット位置を1件書く。新形式 Addr%d へ保存し、移行済みの旧形式 Pos%d は削除する
//   （旧値を残すと Addr%d が消えた際に古い位置が復活するため）。
void CStirlingApp::WriteCaretAddr(int index, stirling::FileOffset pos) {
    CString akey; akey.Format(_T("Addr%d"), index);
    WriteProfileString(kCaretSection, akey,
                       stirling::settings::FormatOffsetHexW(pos).c_str());
    CString vkey; vkey.Format(_T("Pos%d"), index);
    WriteProfileString(kCaretSection, vkey, nullptr);   // 旧形式の値を削除（型に依らず消える）
}

void CStirlingApp::RecordCaretPos(LPCTSTR path, stirling::FileOffset pos) {
    if (path == nullptr || *path == _T('\0') || pos < 0) { return; }
    // 既存の同一パスを除去してから先頭へ挿入（最新＝[0]）。上限超過分は末尾を落とす。
    for (auto it = m_caretStore.begin(); it != m_caretStore.end(); ++it) {
        if (it->first.CompareNoCase(path) == 0) { m_caretStore.erase(it); break; }
    }
    m_caretStore.emplace(m_caretStore.begin(), CString(path), pos);
    if (static_cast<int>(m_caretStore.size()) > kCaretStoreMax) {
        m_caretStore.resize(kCaretStoreMax);
    }
}

// --- マークの自動保存／自動復元（Issue #100） ---
//   セクション "MarkStore" に Count / Path%d / Size%d / Marks%d を最大16件。
//   Marks%d は "40:1,A0:2"（16進アドレス:種別1..3、アドレス昇順）。表記は #99 の
//   マークファイルと揃えてあり、両者を見比べられる。

void CStirlingApp::LoadMarkStore() {
    m_markStore.clear();
    // 設定の ON/OFF によらず読み込む。OFF のときの復元と記録は LookupMarks / RecordMarks 側で
    //   抑止しており、ここで読まずにいると OFF→ON の切り替え後の SaveMarkStore が空のストアで
    //   既存記録を上書きしてしまう（Issue #128）。
    int count = GetProfileInt(kMarkSection, _T("Count"), 0);
    if (count > kMarkStoreMax) { count = kMarkStoreMax; }
    for (int i = 0; i < count; ++i) {
        CString key;
        key.Format(_T("Path%d"), i);
        const CString path = GetProfileString(kMarkSection, key, _T(""));
        if (path.IsEmpty()) { continue; }

        key.Format(_T("Marks%d"), i);
        const CString marks = GetProfileString(kMarkSection, key, _T(""));
        MarkStoreEntry entry;
        if (!stirling::marks::DecodeMarkList(static_cast<LPCWSTR>(marks), entry.marks)) {
            // 手編集で壊れた1件は捨てる（既定へ倒さない。キャレットストアと同じ流儀）。
            TRACE(_T("MarkStore Marks%d: 解釈できない値を無視しました\n"), i);
            continue;
        }
        key.Format(_T("Size%d"), i);
        const CString size = GetProfileString(kMarkSection, key, _T(""));
        stirling::FileOffset parsedSize = -1;
        if (!size.IsEmpty() &&
            stirling::settings::ParseOffsetHex(static_cast<LPCWSTR>(size), parsedSize)) {
            entry.size = parsedSize;
        }
        entry.path = path;
        m_markStore.push_back(entry);
    }
}

void CStirlingApp::SaveMarkStore() {
    // OFF のまま終了した場合は書かない（既存記録をそのまま残す）。
    if (!m_appSettings.markAutoRestore) { return; }

    const int count = static_cast<int>(m_markStore.size());
    WriteProfileInt(kMarkSection, _T("Count"), count);
    for (int i = 0; i < count; ++i) {
        CString key;
        key.Format(_T("Path%d"), i);
        WriteProfileString(kMarkSection, key, m_markStore[i].path);
        key.Format(_T("Size%d"), i);
        WriteProfileString(kMarkSection, key,
                           stirling::settings::FormatOffsetHexW(m_markStore[i].size).c_str());
        key.Format(_T("Marks%d"), i);
        WriteProfileString(kMarkSection, key,
                           stirling::marks::EncodeMarkList(m_markStore[i].marks).c_str());
    }
}

void CStirlingApp::RecordMarks(LPCTSTR path, stirling::FileOffset size,
                               const std::map<stirling::FileOffset, int>& marks) {
    if (!m_appSettings.markAutoRestore) { return; }
    if (path == nullptr || *path == _T('\0')) { return; }

    // 既存の同一パスを除いてから先頭へ挿入（最新＝[0]）。上限超過分は末尾を落とす。
    for (auto it = m_markStore.begin(); it != m_markStore.end(); ++it) {
        if (it->path.CompareNoCase(path) == 0) { m_markStore.erase(it); break; }
    }
    if (marks.empty()) {
        // 全て解除して閉じた場合。設定 ON の間の操作なので、記録も消えるのが正しい。
        return;
    }
    MarkStoreEntry entry;
    entry.path = path;
    entry.size = size;
    entry.marks = marks;
    m_markStore.insert(m_markStore.begin(), entry);
    if (static_cast<int>(m_markStore.size()) > kMarkStoreMax) {
        m_markStore.resize(kMarkStoreMax);
    }
}

bool CStirlingApp::LookupMarks(LPCTSTR path, stirling::FileOffset size,
                               std::map<stirling::FileOffset, int>& out) const {
    if (!m_appSettings.markAutoRestore) { return false; }
    if (path == nullptr || *path == _T('\0')) { return false; }

    for (const auto& e : m_markStore) {
        if (e.path.CompareNoCase(path) != 0) { continue; }
        // 大きさが変わっていたら復元しない。ずれた位置に復元するのは、復元しないより悪い。
        if (e.size >= 0 && e.size != size) { return false; }
        out = e.marks;
        return true;
    }
    return false;
}

stirling::FileOffset CStirlingApp::LookupCaretPos(LPCTSTR path) const {
    if (path == nullptr || *path == _T('\0')) { return -1; }
    for (const auto& e : m_caretStore) {
        if (e.first.CompareNoCase(path) == 0) { return e.second; }
    }
    return -1;
}

int CStirlingApp::ExitInstance() {
    SaveSettings();          // 終了時に拡張子レコードをストアへ書き出す
    m_appSettings.Save();    // アプリ全体の動作環境設定を保存
    SaveCaretStore();        // キャレット位置の自動復元ストアを保存
    SaveMarkStore();         // マークの自動復元ストアを保存
    if (m_pRecentFileList != nullptr) {
        m_pRecentFileList->WriteList();   // ファイル履歴（MRU）を保存
    }
    FlushSettingsStore();    // ストアの内容を設定ファイルへ書き出す
    m_singleInstanceMutex.Close();   // 破棄時にも閉じるが、終了処理の順序を明示するため明示クローズ
    return CWinApp::ExitInstance();
}
