// CStirlingApp 実装。MDI ドキュメントテンプレートを登録して起動する。
#include "pch.h"
#include "app/UiStrings.h"   // ui::MsgBox（表題はアプリ名で統一）
#include <afxadv.h>   // CRecentFileList（ファイル履歴 MRU）の完全定義
#include <afxole.h>   // AfxOleInit（MRUのシェル項目生成に必要）
#include <shobjidl.h> // SetCurrentProcessExplicitAppUserModelID
#include "resource.h"
#include "app/StirlingApp.h"
#include "app/SettingsCodec.h"       // 64bit 設定値の保存形式（Issue #22）
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

// 拡張子レコードをレジストリから読み込む（無ければ既定 "*.*" 1件のまま）。
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

// 拡張子レコードをレジストリへ保存する。
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

    // 表示設定の永続化先（HKCU\Software\StirHex\StirHex\<section>）。
    //   SetRegistryKey 後は Profile API がレジストリを使う（INI ではなく）。
    //   旧 StirlingPort キーからは移行せず、StirHex の設定を新規作成する（Issue #66）。
    SetRegistryKey(_T("StirHex"));
    LoadSettings();          // 起動時に拡張子レコード（表示設定）を復元（原 FUN_0041f2a5 相当）
    LoadDefaultExtComment(); // 既定レコードのコメント（静的初期化では読めない。Issue #34）
    m_appSettings.Load();    // アプリ全体の動作環境設定（環境設定 0x8050）を復元
    LoadCaretStore();        // キャレット位置の自動復元ストア（caretAutoRestore）を復元

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
    //   LoadStdProfileSettings が m_pRecentFileList を生成しレジストリから履歴を読み込む。
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
    // MFC 既定のレジストリ節・値名（_afxFileSection / _afxFileEntry）に合わせる。
    m_pRecentFileList = new CRecentFileList(0, _T("Recent File List"), _T("File%d"), n);
    // 退避分を末尾側から Add（Add は先頭挿入のため逆順で元の並びを復元）。上限超過分は自然に脱落。
    for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
        m_pRecentFileList->Add(*it);
    }
}

// --- キャレット位置の自動復元ストア（原 caretAutoRestore。近代レイアウト: セクション
//     "CaretPositions" に Count / Path%d(SJIS) / Addr%d を最大16件保持） ---
//   Addr%d は 64bit アドレスの16進文字列（REG_SZ。形式は app/SettingsCodec.h の共通規約）。
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

stirling::FileOffset CStirlingApp::LookupCaretPos(LPCTSTR path) const {
    if (path == nullptr || *path == _T('\0')) { return -1; }
    for (const auto& e : m_caretStore) {
        if (e.first.CompareNoCase(path) == 0) { return e.second; }
    }
    return -1;
}

int CStirlingApp::ExitInstance() {
    SaveSettings();          // 終了時に拡張子レコードをレジストリへ保存
    m_appSettings.Save();    // アプリ全体の動作環境設定を保存
    SaveCaretStore();        // キャレット位置の自動復元ストアを保存
    if (m_pRecentFileList != nullptr) {
        m_pRecentFileList->WriteList();   // ファイル履歴（MRU）をレジストリへ保存
    }
    m_singleInstanceMutex.Close();   // 破棄時にも閉じるが、終了処理の順序を明示するため明示クローズ
    return CWinApp::ExitInstance();
}
