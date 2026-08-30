// CStirlingDoc 実装。core(BlockList/BlockFileIO/BlockCursor) を用いた Load/Save/編集。
#include "pch.h"
#include "doc/StirlingDoc.h"
#include "frame/ChildFrame.h"
#include "frame/MainFrame.h"
#include "core/BlockFileIO.h"
#include "core/BlockCursor.h"
#include "core/UndoBudget.h"   // Undo 履歴の容量管理ポリシー（Issue #30）
#include "app/StirlingApp.h"
#include "app/UiStrings.h"   // ui::MsgBox / ui::LoadW（表題はアプリ名で統一）

#include <limits>
#include <utility>   // std::move（ScopedHandle の所有権移動）
#include <new>        // std::bad_alloc（巨大範囲の退避失敗を捕捉する。Issue #30）
#include <stdexcept>   // std::length_error（Undo 記録の予約長が max_size を超える場合。Issue #153）

namespace {

// FileOffset(64bit) の長さがこのビルドのバッファ長(size_t)に収まるかの共通検査
//   （実体は core/CoreTypes.h。ビュー層とも共用する。Issue #154）。
using stirling::FitsInBuffer;

}  // namespace

namespace {

// 巨大ファイルを開く前に確認する閾値（原には無い移植独自の保護。Issue #20）。
//   BlockList はファイルサイズとほぼ同量のメモリを消費するため、事前に確認する。
//   しきい値は環境設定「ファイル」ページで変更でき、確認そのものも無効にできる
//   （Issue #101）。0 以下は設定ファイルを手で編集した場合にのみ起こりうるため、
//   既定値へ倒して確認を出す側に寄せる。
stirling::FileOffset LargeFileConfirmBytes(const CAppSettings& settings) {
    if (!settings.largeFileWarn) { return 0; }   // 0 = 確認しない
    const int mb = (settings.largeFileWarnMB > 0) ? settings.largeFileWarnMB : 512;
    return static_cast<stirling::FileOffset>(mb) * 1024 * 1024;
}

// バイト数を3桁区切りの文字列にする（"バイト" 等の単位語はリソース側に持たせる）。
CStringW FormatBytesW(stirling::FileOffset bytes) {
    if (bytes < 0) { return CStringW(L"0"); }   // 負値は想定外（桁区切りが崩れるため弾く）
    CStringW raw;
    raw.Format(L"%lld", static_cast<long long>(bytes));
    CStringW out;
    const int len = raw.GetLength();
    for (int i = 0; i < len; ++i) {
        if (i > 0 && ((len - i) % 3) == 0) { out += L','; }
        out += raw[i];
    }
    return out;
}

// システムエラーコードの説明文（OS の言語設定に従う）。取得できなければ空。
CStringW SystemErrorTextW(unsigned long err) {
    if (err == 0) { return CStringW(); }
    LPWSTR buf = nullptr;
    const DWORD n = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(err), 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    CStringW text;
    if (n != 0 && buf != nullptr) {
        text = CStringW(buf, static_cast<int>(n));
        text.Trim(L"\r\n ");
    }
    if (buf != nullptr) { ::LocalFree(buf); }
    return text;
}

// I/O 失敗の理由行。メモリ不足はリソース文字列、それ以外は OS のエラー説明を用いる。
CStringW FileIoReasonW(const stirling::FileIoResult& r) {
    if (r.status == stirling::FileIoStatus::kOutOfMemory) {
        CStringW msg;
        msg.Format(ui::LoadW(IDS_ERR_FILE_OUT_OF_MEMORY), (LPCWSTR)FormatBytesW(r.fileSize));
        return msg;
    }
    return SystemErrorTextW(r.systemError);
}

// 環境設定「ファイルの排他制御」→ 読み込みハンドルの共有モード（原 ReadFileIntoBlocks
//   の 0→shareDenyNone / 1→shareDenyWrite / 2→shareExclusive）。Issue #120。
stirling::FileShareMode ExclusiveControlToShareMode(int exclusiveControl) {
    switch (exclusiveControl) {
    case 1:  return stirling::FileShareMode::kDenyWrite;   // 書込禁止
    case 2:  return stirling::FileShareMode::kExclusive;   // 読書禁止
    default: return stirling::FileShareMode::kDenyNone;    // しない
    }
}

// 他プロセスが共有を許していないためのオープン失敗か（原 CFileException::sharingViolation）。
bool IsSharingViolation(const stirling::FileIoResult& r) {
    if (r.status != stirling::FileIoStatus::kOpenFailed) { return false; }
    return r.systemError == ERROR_SHARING_VIOLATION || r.systemError == ERROR_LOCK_VIOLATION;
}

// 読み取り専用属性のファイルか（属性を取得できない場合は false）。
bool IsReadOnlyFile(const CStringW& path) {
    const DWORD attr = ::GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY) != 0;
}

HWND MainWndHandle() {
    CWnd* main = AfxGetMainWnd();
    return (main != nullptr) ? main->GetSafeHwnd() : nullptr;
}

// 「見出し（パス入り）＋失敗理由」でメッセージを表示する（エラーを握りつぶさない）。
void ShowFileIoError(UINT headlineId, LPCTSTR path, const stirling::FileIoResult& r) {
    CStringW text = ui::LoadW(headlineId);
    if (text.Find(L"%s") >= 0) {
        CStringW formatted;
        formatted.Format(text, (LPCWSTR)CStringW(path));
        text = formatted;
    }
    const CStringW reason = FileIoReasonW(r);
    if (!reason.IsEmpty()) {
        text += L"\n";
        text += reason;
    }
    ui::MsgBox(MainWndHandle(), text, MB_OK | MB_ICONEXCLAMATION);
}

}  // namespace

IMPLEMENT_DYNCREATE(CStirlingDoc, CDocument)

BEGIN_MESSAGE_MAP(CStirlingDoc, CDocument)
END_MESSAGE_MAP()

CStirlingDoc::CStirlingDoc() {}

CStirlingDoc::~CStirlingDoc() { ReleaseLock(); }

void CStirlingDoc::DeleteContents() {
    ReleaseLock();   // 内容破棄（閉じる/再読込）時は排他ハンドルも解放
    m_blocks.Clear();
    ClearUndoHistory(false);
    m_cleanUndoSize = 0;   // 内容クリア＝未変更の保存点は深さ0
    m_marks.clear();
    CDocument::DeleteContents();
}

// 現在のパスから拡張子レコードを解決して m_settings を確定する。
void CStirlingDoc::ResolveSettings() {
    m_settings = theApp.SettingsForPath(CStringW(GetPathName()));
}

// オープン時の既定を設定から適用（原 表示状態ページ 158: 文字セット/バイトオーダ/
//   挿入モード/編集禁止）。fileOpen=true のときのみ「編集禁止」既定を適用する。
void CStirlingDoc::ApplyOpenDefaults(bool fileOpen) {
    const CStirlingSettings& s = m_settings;   // この文書の解決済み設定
    if (s.defCharset >= 0 && s.defCharset <= 6) { m_charset = s.defCharset; }
    m_byteOrderBig  = (s.defByteOrderBig != 0);
    m_overwriteMode = !s.openInsertMode;    // 挿入モード既定ON → 上書きOFF
    if (fileOpen) {
        m_editState = s.openReadOnly ? 1 : 2;   // 1=編集禁止 / 2=編集可
    } else {
        // 新規文書（原ヘルプ「新規ドキュメントは常に編集可能として開く」newDocEditable）:
        //   ON なら openReadOnly を無視して常に編集可。OFF のときのみ openReadOnly を適用。
        m_editState = (!theApp.AppSettings().newDocEditable && s.openReadOnly) ? 1 : 2;
    }
}

// 排他制御用ハンドルを解放。
void CStirlingDoc::ReleaseLock() {
    m_lockHandle.Close();
    m_lockPath.Empty();
}

// 排他制御（原 exclusiveControl）の再取得: 共有モード付きの参照ハンドルを保持する。
//   0=しない（保持しない）/ 1=書込禁止（FILE_SHARE_READ で他プロセスの書込を拒否）/
//   2=読書禁止（共有なし＝排他）。
//   オープン時のロックは読み込みハンドルをそのまま保持する（OnOpenDocument。Issue #120）。
//   ここは保存の前後で一旦手放したロックを取り直す経路。保存直後は自分が書き込んだ直後で
//   あり、失敗するのは他プロセスが割り込んだ場合に限られる。利用者へ選択肢を出せる場面では
//   ないため、取得できなければロックなしで続行する。
void CStirlingDoc::AcquireLock(LPCTSTR path) {
    ReleaseLock();
    if (path == nullptr || *path == _T('\0')) { return; }
    const int mode = theApp.AppSettings().exclusiveControl;
    if (mode == 0) { return; }                       // 排他しない
    const DWORD share = (mode == 1) ? FILE_SHARE_READ : 0;   // 1=書込禁止 / 2=読書禁止
    stirling::ScopedHandle h(::CreateFile(path, GENERIC_READ, share, nullptr,
                                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (h.Valid()) {
        m_lockHandle = std::move(h);
        m_lockPath = path;
    } else {
        TRACE(_T("排他制御ハンドルを取り直せませんでした（保存後）"));
    }
}

// 保存前バックアップ（原 FUN_004340bf）。命名 dir\name.bak（最新）＋ name.bk1..bk{世代-1}（古い）。
//   name は拡張子込みのファイル名。backupFolderSpecify 時は backupFolder を dir に用いる。
void CStirlingDoc::CreateBackup(LPCTSTR path) {
    const CAppSettings& s = theApp.AppSettings();
    if (!s.backupCreate) { return; }
    if (::GetFileAttributes(path) == INVALID_FILE_ATTRIBUTES) { return; }   // 既存ファイル無し→不要
    CString full(path);
    const int sep = full.ReverseFind(_T('\\'));
    if (sep < 0) { return; }
    CString dir  = full.Left(sep);
    CString name = full.Mid(sep + 1);
    if (s.backupFolderSpecify && !s.backupFolder.IsEmpty()) {
        dir = CString(s.backupFolder);   // 指定バックアップフォルダ（CStringW→CString 変換）
    }
    int gen = s.backupGenerations;
    if (gen < 1) { gen = 1; }
    CString bak;
    bak.Format(_T("%s\\%s.bak"), (LPCTSTR)dir, (LPCTSTR)name);
    if (gen == 1) {
        ::CopyFile(path, bak, FALSE);   // 上書きコピー（原 CopyFileA(...,FALSE)）
        return;
    }
    // 世代ローテーション: 最古 .bk{gen-1} を削除→ .bk{k}→.bk{k+1} 繰り下げ→ .bak→.bk1→ 現ファイル→.bak。
    CString oldest;
    oldest.Format(_T("%s\\%s.bk%d"), (LPCTSTR)dir, (LPCTSTR)name, gen - 1);
    ::DeleteFile(oldest);
    for (int k = gen - 2; k >= 1; --k) {
        CString src, dst;
        src.Format(_T("%s\\%s.bk%d"), (LPCTSTR)dir, (LPCTSTR)name, k);
        dst.Format(_T("%s\\%s.bk%d"), (LPCTSTR)dir, (LPCTSTR)name, k + 1);
        ::MoveFile(src, dst);   // 対象が無ければ失敗するが無害
    }
    CString bk1;
    bk1.Format(_T("%s\\%s.bk1"), (LPCTSTR)dir, (LPCTSTR)name);
    ::MoveFile(bak, bk1);           // .bak → .bk1
    ::CopyFile(path, bak, FALSE);   // 現ファイル → .bak
}

BOOL CStirlingDoc::OnNewDocument() {
    if (!CDocument::OnNewDocument()) {  // 内部で DeleteContents を呼ぶ
        return FALSE;
    }
    // 原 OnNewDocument: 空の16KBブロック1個（新規は常に編集可能）
    // 確保できなければ空文書を作らず失敗させる（例外を投げない。Issue #153）。
    unsigned char* buf = stirling::AllocBlockData();
    if (buf == nullptr) {
        return FALSE;
    }
    if (m_blocks.AppendBlock(buf, stirling::kBlockCapacity, 0) == nullptr) {
        delete[] buf;   // ノード確保失敗時は所有権が移らない
        return FALSE;
    }
    m_settings = theApp.SettingsForPath(nullptr);   // 新規は既定 "*" レコード
    ApplyOpenDefaults(false);   // 新規は編集可のまま（文字セット/バイトオーダ/挿入既定のみ）
    return TRUE;
}

// 閉じる直前にマークを記録する（Issue #100）。
//   環境設定 markAutoRestore が OFF の間は RecordMarks 側で何もしない。OFF のときは
//   復元もしないため、ここで書き戻すと「利用者が消した」のか「復元しなかった」のか
//   区別できないまま記録を失う。
//   未保存の変更を破棄して閉じる場合も同じ理由で記録しない（Issue #129）。破棄される
//   メモリ上の大きさとマークで上書きすると、ディスク上の内容と食い違って次回は復元されず、
//   それ以前の有効な記録まで失われる。
void CStirlingDoc::OnCloseDocument() {
    const CString path = GetPathName();
    if (!path.IsEmpty() && !IsModified()) {
        std::map<stirling::FileOffset, int> marks;
        for (const auto& kv : m_marks) {
            marks[kv.first] = kv.second + 1;   // 内部種別 0..2 → ストア上の 1..3
        }
        theApp.RecordMarks(path, GetTotalLength(), marks);
    }
    CDocument::OnCloseDocument();
}

BOOL CStirlingDoc::OnOpenDocument(LPCTSTR lpszPathName) {
    DeleteContents();
    // core は 64bit オフセット・ワイドパスの API（Issue #20）。MBCS ビルドのため境界で変換する。
    const CStringW wpath(lpszPathName);

    // 巨大ファイルは読み込み前に確認する（ファイルサイズ相当のメモリを消費するため）。
    const stirling::FileOffset confirmBytes = LargeFileConfirmBytes(theApp.AppSettings());
    stirling::FileOffset probeSize = 0;
    if (confirmBytes > 0 &&
        stirling::QueryFileSize(wpath, &probeSize, nullptr) &&
        probeSize >= confirmBytes) {
        CStringW confirm;
        confirm.Format(ui::LoadW(IDS_CONFIRM_LARGE_FILE), (LPCWSTR)FormatBytesW(probeSize));
        if (ui::MsgBox(MainWndHandle(), confirm, MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return FALSE;   // 利用者が中止（MFC 側の追加メッセージは出ない）
        }
    }

    // 排他制御は読み込みハンドルの共有モードとして適用し、有効な間はそのハンドルを
    //   保持し続ける（原 ReadFileIntoBlocks と同じ形。別ハンドルで後からロックを取ると、
    //   排他モードでは自分自身の読み込みを弾いてしまう。Issue #120）。
    ReleaseLock();
    const int exclusive = theApp.AppSettings().exclusiveControl;
    const stirling::FileShareMode share = ExclusiveControlToShareMode(exclusive);

    // 読み取り専用属性のファイルは閲覧モードで開く（原挙動）。
    bool viewMode = IsReadOnlyFile(wpath);

    void* keepHandle = nullptr;
    stirling::FileIoResult loaded =
        stirling::LoadFileIntoBlocks(m_blocks, wpath, share,
                                     (exclusive != 0) ? &keepHandle : nullptr);
    if (!loaded.Ok() && IsSharingViolation(loaded)) {
        // 他のアプリケーションが排他制御している。原と同じく閲覧モードで開くか確認する。
        //   [キャンセル] は追加のメッセージを出さずに中止（原 IDCANCEL 経路）。
        if (ui::MsgBox(MainWndHandle(), ui::LoadW(IDS_CONFIRM_VIEW_MODE),
                       MB_OKCANCEL | MB_ICONQUESTION) != IDOK) {
            return FALSE;
        }
        // 閲覧モードは共有を全許可で開き直す（原 modeRead | shareDenyNone）。
        //   何も排他しないハンドルを保持する意味は無いため、ここでは保持しない。
        keepHandle = nullptr;
        loaded = stirling::LoadFileIntoBlocks(m_blocks, wpath,
                                              stirling::FileShareMode::kDenyNone, nullptr);
        if (loaded.Ok()) { viewMode = true; }
    }
    if (!loaded.Ok()) {
        // MFC は OnOpenDocument が FALSE のとき自前のメッセージを出さない契約のため、
        // ここで理由付きのメッセージを表示する。
        ShowFileIoError(IDS_ERR_LOAD_FAILED, lpszPathName, loaded);
        return FALSE;
    }
    if (keepHandle != nullptr) {
        m_lockHandle.Reset(static_cast<HANDLE>(keepHandle));   // 読み込みハンドル＝排他ロック
        m_lockPath = lpszPathName;
    }
    // 拡張子で表示設定レコードを解決（この時点で GetPathName は未設定のため引数パスを使う）。
    m_settings = theApp.SettingsForPath(CStringW(lpszPathName));
    ApplyOpenDefaults(true);    // ファイルオープン時は編集禁止既定も適用
    // 閲覧モード（原 doc+0x64 = 0）。編集できず、[編集禁止]の切替もできない状態。
    //   拡張子別設定の「ファイルオープン時に編集禁止とする」より強い（Issue #120）。
    if (viewMode) { m_editState = 0; }

    // マークの自動復元（Issue #100）。環境設定 markAutoRestore が OFF のとき、および
    //   記録時とデータの大きさが違うときは LookupMarks が false を返す（復元しない）。
    std::map<stirling::FileOffset, int> restored;
    if (theApp.LookupMarks(lpszPathName, GetTotalLength(), restored)) {
        const stirling::FileOffset total = GetTotalLength();
        for (const auto& kv : restored) {
            if (kv.first < 0 || kv.first >= total) { continue; }
            SetMark(kv.first, kv.second - 1);   // ストア上の 1..3 → 内部種別 0..2
        }
    }
    m_diskTimeValid = ReadDiskTime(lpszPathName, m_diskTime);   // 外部変更検知の基準時刻
    SetModifiedFlag(FALSE);
    return TRUE;
}

BOOL CStirlingDoc::OnSaveDocument(LPCTSTR lpszPathName) {
    CreateBackup(lpszPathName);   // 保存前バックアップ（backupCreate 時。既存ファイルを世代保存）
    ReleaseLock();                // 排他ハンドルを一旦解放（保持中は書込オープンが失敗するため）
    const stirling::FileIoResult saved = stirling::SaveBlocksToFile(m_blocks, CStringW(lpszPathName));
    if (!saved.Ok()) {
        AcquireLock(lpszPathName);   // 書込失敗時もロックを復帰
        // オープン失敗（使用中・権限）と書込失敗（ディスク不足等）で見出しを分ける。
        const UINT headline = (saved.status == stirling::FileIoStatus::kOpenFailed)
                                  ? IDS_ERR_SAVE_FAILED : IDS_ERR_WRITE_FAILED;
        ShowFileIoError(headline, lpszPathName, saved);
        return FALSE;
    }
    // 保存時にアンドゥバッファをクリア（環境設定 clearUndoOnSave）。
    //   クリア後は空スタックが保存点となる（m_cleanUndoSize=0）。
    if (theApp.AppSettings().clearUndoOnSave) {
        ClearUndoHistory(false);
    }
    m_cleanUndoSize = static_cast<int>(m_undoStack.size());   // 現在の編集状態を保存点に
    AcquireLock(lpszPathName);   // 保存後に共有モードで再ロック（別名保存時は新パス）
    m_diskTimeValid = ReadDiskTime(lpszPathName, m_diskTime);   // 保存後の時刻を基準に更新
    SetModifiedFlag(FALSE);
    return TRUE;
}

void CStirlingDoc::Serialize(CArchive& ar) {
    // Load/Save は OnOpenDocument/OnSaveDocument で直接処理するため未使用。
    UNREFERENCED_PARAMETER(ar);
}

// 変更フラグを設定。変化した時は子フレームのタイトル（編集マーク「*」）を更新する。
void CStirlingDoc::SetModifiedFlag(BOOL bModified) {
    ++m_changeSeq;   // データ変更検出用（構造体編集バー等）。無条件に増加
    const bool was = (IsModified() != FALSE);
    CDocument::SetModifiedFlag(bModified);
    if ((bModified != FALSE) != was) {
        RefreshFrameTitles();
    }
    // ビットイメージのリアルタイム反映（原は AdjustPositionsAfterEdit 内で増分更新）。
    //   ビュー経由の編集だけでなく、構造体編集バーからの書換えや再読込にも追従させるため、
    //   データ変更の集約点であるここから通知する。
    if (CMainFrame* pFrame = DYNAMIC_DOWNCAST(CMainFrame, AfxGetMainWnd())) {
        pFrame->QueueBitImageRefresh();
    }
}

// パス設定時のタイトル決定（原ヘルプ docFullPath）。基底で短いファイル名を設定した後、
//   docFullPath ON ならタイトルをフルパスへ差し替える。
void CStirlingDoc::SetPathName(LPCTSTR lpszPathName, BOOL bAddToMRU) {
    CDocument::SetPathName(lpszPathName, bAddToMRU);   // m_strPathName とMRU、既定タイトル(短名)を設定
    if (theApp.AppSettings().docFullPath && lpszPathName != nullptr && *lpszPathName != _T('\0')) {
        SetTitle(lpszPathName);   // フルパスをタイトルに（SetTitle が全フレームのタイトルを更新）
    }
}

// docFullPath 変更後、現在のパスからタイトルを再決定する（環境設定 OK 後に全文書で呼ぶ）。
void CStirlingDoc::RefreshDocTitle() {
    const CString path = GetPathName();
    if (path.IsEmpty()) { return; }   // 無題（新規）はそのまま
    if (theApp.AppSettings().docFullPath) {
        SetTitle(path);
    } else {
        int slash = path.ReverseFind(_T('\\'));
        SetTitle((slash >= 0) ? path.Mid(slash + 1) : path);   // 短いファイル名へ戻す
    }
}

// この文書の全ビューの子フレームのタイトルを更新（変更マーク反映）。
void CStirlingDoc::RefreshFrameTitles() {
    POSITION pos = GetFirstViewPosition();
    while (pos != nullptr) {
        CView* pView = GetNextView(pos);
        if (pView != nullptr) {
            CChildFrame* pFrame = DYNAMIC_DOWNCAST(CChildFrame, pView->GetParentFrame());
            if (pFrame != nullptr) { pFrame->UpdateTitle(); }
        }
    }
    // メインフレームのタイトル（"アプリ名 - ファイル名 *"）も更新する。
    CMainFrame* pMain = DYNAMIC_DOWNCAST(CMainFrame, AfxGetMainWnd());
    if (pMain != nullptr) { pMain->UpdateTitle(); }
}

// 前方編集の後処理（push_back 済み前提）: 保存点が破棄される Redo 側にあれば到達不能化し、
//   Redo を破棄して変更フラグを更新する。
void CStirlingDoc::CommitForwardEdit() {
    // 旧 Undo 深さ = size-1。保存点がそれより先（未来=Redo側）なら破棄で到達不能。
    if (m_cleanUndoSize > static_cast<int>(m_undoStack.size()) - 1) {
        m_cleanUndoSize = -1;
    }
    for (const EditRecord& r : m_redoStack) { m_undoBytes -= RecordBytes(r); }
    m_redoStack.clear();
    TrimUndoHistory();   // Redo を捨てた後の保持量で上限判定する（Issue #30）
    UpdateModifiedByUndo();
}

// Undo 深さと保存点の一致で変更フラグを更新（Undoで保存点に戻れば未変更＝「*」除去）。
void CStirlingDoc::UpdateModifiedByUndo() {
    SetModifiedFlag(static_cast<int>(m_undoStack.size()) != m_cleanUndoSize ? TRUE : FALSE);
}

// ディスク上の更新日時を取得（原 FUN_00436450: modeRead|shareDenyNone で開いて GetStatus）。
//   削除済み・他プロセスが排他中で開けない場合は false（原はこのとき通知しない）。
bool CStirlingDoc::ReadDiskTime(LPCTSTR path, CTime& out) {
    if (path == nullptr || *path == _T('\0')) { return false; }
    CFile file;
    if (!file.Open(path, CFile::modeRead | CFile::shareDenyNone)) { return false; }
    CFileStatus status;
    if (!file.GetStatus(status)) { return false; }
    out = status.m_mtime;
    return true;
}

// 保持している更新日時とディスク上の更新日時を突き合わせる。
bool CStirlingDoc::HasExternalChange() const {
    if (!m_diskTimeValid) { return false; }
    const CString path = GetPathName();
    if (path.IsEmpty()) { return false; }
    CTime now;
    if (!ReadDiskTime(path, now)) { return false; }   // 開けない＝通知しない（原の実測挙動）
    return now != m_diskTime;
}

// 通知後に再表示させないため、現在のディスク上の更新日時を控え直す。
void CStirlingDoc::SyncDiskTime() {
    m_diskTimeValid = ReadDiskTime(GetPathName(), m_diskTime);
}

// 編集前に戻す（原 FUN_0043621f）: 保存済みファイルを再読込（DeleteContents→再読込→
//   SetModifiedFlag(FALSE)）。パスが無ければ false。
bool CStirlingDoc::RevertToSaved() {
    const CString path = GetPathName();
    if (path.IsEmpty()) { return false; }
    return OnOpenDocument(path) != FALSE;
}

// ---- 編集プリミティブ（絶対バイト位置。Undo記録付き） ----
// 各 forward 編集は「実操作を行い、それを打ち消す逆レコードを Undo スタックへ積む＋
// Redo スタックを破棄する」（原 mode==0 の通常編集）。単バイト挿入は忠実な
// BlockCursor::InsertByte を、上書きは in-place SetByteAt を用いる。

bool CStirlingDoc::InsertByteAt(stirling::FileOffset pos, unsigned char b) {
    if (!ReserveUndoSlot()) { return false; }   // 変更前に記録領域を確保（Issue #153）
    stirling::BlockCursor c(&m_blocks);
    if (!c.InsertByte(pos, b)) {
        return false;
    }
    EditRecord rec; rec.kind = EditRecord::kDelete; rec.pos = pos; rec.count = 1;
    PushUndoRecord(std::move(rec));
    AdjustMarksForSplice(pos, 0, 1);   // ダイナミックマーク: 1バイト挿入に追従
    CommitForwardEdit();   // Redo破棄＋保存点比較で変更フラグ更新
    return true;
}

bool CStirlingDoc::OverwriteByteAt(stirling::FileOffset pos, unsigned char b) {
    if (!ReserveUndoSlot()) { return false; }   // 変更前に記録領域を確保（Issue #153）
    const stirling::FileOffset total = GetTotalLength();
    if (pos < total) {
        unsigned char old = 0;
        GetByteAt(pos, &old);
        stirling::BlockCursor c(&m_blocks);
        if (!c.SetByteAt(pos, b)) {
            return false;
        }
        EditRecord rec; rec.kind = EditRecord::kOverwrite; rec.pos = pos; rec.bytes = {old};
        PushUndoRecord(std::move(rec));
    } else {
        // EOF は追記（原「上書きモード時の末尾自動挿入」）。
        stirling::BlockCursor c(&m_blocks);
        if (!c.InsertByte(pos, b)) {
            return false;
        }
        EditRecord rec; rec.kind = EditRecord::kDelete; rec.pos = pos; rec.count = 1;
        PushUndoRecord(std::move(rec));
        AdjustMarksForSplice(pos, 0, 1);   // EOF追記＝1バイト挿入に追従
    }
    CommitForwardEdit();   // Redo破棄＋保存点比較で変更フラグ更新
    return true;
}

bool CStirlingDoc::DeleteByteAt(stirling::FileOffset pos, unsigned char* outByte) {
    if (!ReserveUndoSlot()) { return false; }   // 変更前に記録領域を確保（Issue #153）
    unsigned char old = 0;
    if (!GetByteAt(pos, &old)) {
        return false;   // pos が実データ外
    }
    stirling::BlockCursor c(&m_blocks);
    unsigned char tmp = 0;
    if (!c.DeleteByte(pos, &tmp)) {
        return false;
    }
    if (outByte) { *outByte = old; }
    EditRecord rec; rec.kind = EditRecord::kInsert; rec.pos = pos; rec.bytes = {old};
    PushUndoRecord(std::move(rec));
    AdjustMarksForSplice(pos, 1, 0);   // ダイナミックマーク: 1バイト削除に追従
    CommitForwardEdit();   // Redo破棄＋保存点比較で変更フラグ更新
    return true;
}

bool CStirlingDoc::DeleteRange(stirling::FileOffset pos, stirling::FileOffset count) {
    if (count <= 0) {
        return false;
    }
    // 先に実データ長へ丸める（上限判定は実際に退避する量に対して行う）。
    const stirling::FileOffset total = GetTotalLength();
    if (pos < 0 || pos >= total) {
        return false;
    }
    if (count > total - pos) { count = total - pos; }
    // 削除前に対象バイトを退避（逆＝再挿入用）。退避は削除量と同容量のメモリを使うため、
    //   上限（環境設定 undoMemoryLimitMB）を超える場合は Undo を諦めるか確認する（Issue #30）。
    if (!FitsInBuffer(count)) {
        return false;   // このビルドのバッファに載らない長さ（データは無傷）
    }
    bool undoless = false;
    switch (CheckUndoCapacity(count)) {
    case UndoCapacityDecision::kCancel:   return false;   // 中止（データは無傷）
    case UndoCapacityDecision::kUndoless: undoless = true; break;
    case UndoCapacityDecision::kNormal:   break;
    }
    std::vector<unsigned char> captured;   // undoless のときは空のまま（退避しない）
    if (!undoless) {
        try {
            captured.resize(static_cast<size_t>(count));
        } catch (const std::bad_alloc&) {
            return false;   // 退避できないなら編集自体を行わない（データは無傷）
        }
        stirling::BlockCursor c(&m_blocks);
        if (!c.Seek(pos, stirling::BlockCursor::kBegin, nullptr)) {
            return false;
        }
        const stirling::FileOffset n = c.Read(count, captured.data());
        if (n < count) { count = n; }
        captured.resize(static_cast<size_t>(count));
        if (count <= 0) {
            return false;
        }
    }
    if (!undoless && !ReserveUndoSlot()) {
        return false;   // 変更前に記録領域を確保（Issue #153）。データは無傷
    }
    stirling::FileOffset deleted = 0;
    {
        stirling::BlockCursor d(&m_blocks);
        deleted = d.DeleteRange(pos, count);   // ブロック単位の一括削除（Issue #62）
    }
    if (deleted < count) {
        // 途中で失敗したら実削除数へ合わせる（逆レコードとマーク追従の齟齬を防ぐ）。
        count = deleted;
        if (!undoless) { captured.resize(static_cast<size_t>(count)); }
    }
    if (count <= 0) {
        return false;   // 1バイトも削除できなかった（ドキュメントは無変更）
    }
    if (undoless) {
        ClearUndoHistory(true);   // この操作は取り消せない＝保存点も到達不能にする
    } else {
        EditRecord rec;
        rec.kind = EditRecord::kInsert; rec.pos = pos; rec.bytes = std::move(captured);
        PushUndoRecord(std::move(rec));
    }
    AdjustMarksForSplice(pos, count, 0);   // ダイナミックマーク: count バイト削除に追従
    CommitForwardEdit();   // Redo破棄＋容量トリム＋保存点比較で変更フラグ更新
    return true;
}

bool CStirlingDoc::ReplaceRange(stirling::FileOffset pos, stirling::FileOffset delLen,
                               const std::vector<unsigned char>& ins) {
    if (delLen < 0) {
        return false;
    }
    // 位置の妥当性と削除長を先に確定する（pos==総長は末尾への挿入として許す）。
    //   退避しない経路では Seek 失敗で弾けないため、ここで一度だけ検査・丸めを行う。
    const stirling::FileOffset total = GetTotalLength();
    if (pos < 0 || pos > total) {
        return false;
    }
    if (delLen > total - pos) { delLen = total - pos; }
    // 置換前に削除対象を退避（逆＝元データ再挿入用）。上限超過時の扱いは DeleteRange と同じ。
    std::vector<unsigned char> removed;
    bool undoless = false;
    stirling::FileOffset target = 0;   // 削除を試みるバイト数
    if (delLen > 0) {
        if (!FitsInBuffer(delLen)) {
            return false;   // このビルドのバッファに載らない長さ（データは無傷）
        }
        switch (CheckUndoCapacity(delLen)) {
        case UndoCapacityDecision::kCancel:   return false;   // 中止（データは無傷）
        case UndoCapacityDecision::kUndoless: undoless = true; break;
        case UndoCapacityDecision::kNormal:   break;
        }
        target = delLen;
        if (!undoless) {
            try {
                removed.resize(static_cast<size_t>(delLen));
            } catch (const std::bad_alloc&) {
                return false;   // 退避できないなら編集自体を行わない（データは無傷）
            }
            stirling::BlockCursor c(&m_blocks);
            if (!c.Seek(pos, stirling::BlockCursor::kBegin, nullptr)) {
                return false;
            }
            target = c.Read(delLen, removed.data());
            removed.resize(static_cast<size_t>(target));
        }
    }
    // Undo レコードの領域も、ドキュメントを変更する前に確保しておく（Issue #153）。
    if (!undoless && !ReserveUndoSlot()) {
        return false;   // データは無傷
    }
    // Issue #153: 「挿入 → 削除」の順で行う。挿入は全ブロックの確保に成功したときだけ
    //   成立する全か無かの操作であり、削除は確保を伴わないため失敗しない。
    //   この順序なら、確保失敗時にドキュメント・マーク・履歴のいずれも変化しない
    //   （逆順だと削除だけが成立した部分編集が残る）。
    stirling::FileOffset insLen = 0;   // 実際に挿入できたバイト数
    if (!ins.empty()) {
        stirling::BlockCursor c(&m_blocks);
        if (!c.Insert(pos, ins.data(), static_cast<stirling::FileOffset>(ins.size()))) {
            return false;   // メモリ不足。ドキュメントは無変更
        }
        insLen = static_cast<stirling::FileOffset>(ins.size());
    }
    stirling::FileOffset actualDel = 0;   // 実際に削除できたバイト数
    if (target > 0) {
        stirling::BlockCursor d(&m_blocks);
        // 挿入した分だけ削除開始位置が後ろへずれる。
        actualDel = d.DeleteRange(pos + insLen, target);   // ブロック単位の一括削除（Issue #62）
    }
    if (actualDel < target && !undoless) {
        removed.resize(static_cast<size_t>(actualDel));   // 実削除数へ合わせる
    }
    if (actualDel == 0 && insLen == 0) {
        return false;   // ドキュメントは無変更（履歴にも触れない）
    }
    if (undoless) {
        ClearUndoHistory(true);   // この操作は取り消せない＝保存点も到達不能にする
    } else {
        // 逆レコード: 挿入分(insLen)を削除し、退避した元データ(removed)を再挿入。
        EditRecord rec;
        rec.kind = EditRecord::kReplace;
        rec.pos = pos;
        rec.count = insLen;
        rec.bytes = std::move(removed);
        PushUndoRecord(std::move(rec));   // 予約済みのため失敗しない
    }
    AdjustMarksForSplice(pos, actualDel, insLen);   // ダイナミックマーク: 置換に追従
    CommitForwardEdit();   // Redo破棄＋容量トリム＋保存点比較で変更フラグ更新
    return true;
}

// 範囲初期化（Issue #154）。選択範囲と同容量の一時バッファを作らず、ブロックへ直接
//   定数値を書き込む。長さが変わらないため確保・挿入・削除は発生しない。
//   Undo は単一の kOverwrite レコード（退避量は範囲と同容量。上限判定は Issue #30 と共通）。
CStirlingDoc::FillRangeResult CStirlingDoc::FillRange(stirling::FileOffset pos,
                                                     stirling::FileOffset count,
                                                     unsigned char value) {
    if (count <= 0) {
        return FillRangeResult::kInvalid;
    }
    const stirling::FileOffset total = GetTotalLength();
    if (pos < 0 || pos >= total) {
        return FillRangeResult::kInvalid;
    }
    if (count > total - pos) { count = total - pos; }   // 実データ長へ丸める

    // Undo 用の退避。上限を超える場合は中止か「取り消しなしで続行」を選ばせる。
    bool undoless = false;
    switch (CheckUndoCapacity(count)) {
    case UndoCapacityDecision::kCancel:   return FillRangeResult::kCanceled;
    case UndoCapacityDecision::kUndoless: undoless = true; break;
    case UndoCapacityDecision::kNormal:   break;
    }
    std::vector<unsigned char> captured;   // undoless のときは空のまま（退避しない）
    if (!undoless) {
        if (!FitsInBuffer(count)) {
            return FillRangeResult::kOutOfMemory;   // このビルドのバッファに載らない長さ
        }
        try {
            captured.resize(static_cast<size_t>(count));
        } catch (const std::bad_alloc&) {
            return FillRangeResult::kOutOfMemory;   // 退避できないなら実行しない（データは無傷）
        } catch (const std::length_error&) {
            return FillRangeResult::kOutOfMemory;
        }
        stirling::BlockCursor c(&m_blocks);
        if (!c.Seek(pos, stirling::BlockCursor::kBegin, nullptr)) {
            return FillRangeResult::kInvalid;
        }
        const stirling::FileOffset n = c.Read(count, captured.data());
        if (n < count) { count = n; }
        captured.resize(static_cast<size_t>(count));
        if (count <= 0) {
            return FillRangeResult::kInvalid;
        }
        if (!ReserveUndoSlot()) {
            return FillRangeResult::kOutOfMemory;   // 変更前に記録領域を確保（Issue #153）
        }
    }
    stirling::FileOffset filled = 0;
    {
        stirling::BlockCursor c(&m_blocks);
        filled = c.FillRange(pos, count, value);   // 確保を伴わないため失敗しない
    }
    if (filled <= 0) {
        return FillRangeResult::kInvalid;   // ドキュメントは無変更
    }
    if (undoless) {
        ClearUndoHistory(true);   // この操作は取り消せない＝保存点も到達不能にする
    } else {
        if (filled < static_cast<stirling::FileOffset>(captured.size())) {
            captured.resize(static_cast<size_t>(filled));   // 実書込数へ合わせる
        }
        EditRecord rec;
        rec.kind = EditRecord::kOverwrite; rec.pos = pos; rec.bytes = std::move(captured);
        PushUndoRecord(std::move(rec));   // 予約済みのため失敗しない
    }
    // マークには触れない（Issue #161）。長さが変わらない上書きなので移動は起きず、
    //   範囲内のマークも消さない。原版 Stirling 1.31 で「マークを含む範囲を初期化しても
    //   マークは残る」ことを実測して確認済み（e2e test_issue_161_fill_marks.py）。
    //   移植版は初期化を ReplaceRange で実装していたため AdjustMarksForSplice が
    //   範囲内のマークを消しており、原版と食い違っていた。上書き入力
    //   （OverwriteByteAt / SetByteNoUndo）がマークを残すのとも整合する。
    CommitForwardEdit();   // Redo破棄＋容量トリム＋保存点比較で変更フラグ更新
    return FillRangeResult::kOk;
}

bool CStirlingDoc::GetByteAt(stirling::FileOffset pos, unsigned char* outByte) {
    stirling::BlockCursor c(&m_blocks);
    if (!c.Seek(pos, stirling::BlockCursor::kBegin, nullptr)) {
        return false;
    }
    return c.Read(1, outByte) == 1;
}

// 編集禁止/許可を切替（原 FUN_00436ce5）。状態0はロックで切替不可。2↔1 を反転し全ビュー更新。
bool CStirlingDoc::ToggleEditState() {
    if (m_editState == 0) {
        return false;                       // ロック状態は切替不可（原は beep）
    }
    m_editState = (m_editState == 2) ? 1 : 2;
    UpdateAllViews(nullptr);                // 読取専用表示の反映（原 UpdateAllViews hint 0x11）
    return true;
}

std::vector<unsigned char> CStirlingDoc::ReadRange(stirling::FileOffset pos,
                                                  stirling::FileOffset count) {
    std::vector<unsigned char> out;
    if (count <= 0) {
        return out;
    }
    if (!FitsInBuffer(count)) {
        return out;   // このビルドのバッファに載らない長さ（空を返す）
    }
    try {
        out.resize(static_cast<size_t>(count));   // 要求長がメモリに載らない場合は空を返す
    } catch (const std::bad_alloc&) {
        return out;
    }
    stirling::BlockCursor c(&m_blocks);
    if (!c.Seek(pos, stirling::BlockCursor::kBegin, nullptr)) {
        out.clear();
        return out;
    }
    const stirling::FileOffset n = c.Read(count, out.data());
    out.resize(static_cast<size_t>(n));
    return out;
}

// 範囲読取（呼出側バッファ版。Issue #155）。ReadRange と違い確保を行わないため、
//   大きな範囲でもチャンク単位で回せる。読めたバイト数を返す。
stirling::FileOffset CStirlingDoc::ReadInto(stirling::FileOffset pos,
                                            stirling::FileOffset count, void* dst) {
    if (count <= 0 || dst == nullptr) {
        return 0;
    }
    stirling::BlockCursor c(&m_blocks);
    if (!c.Seek(pos, stirling::BlockCursor::kBegin, nullptr)) {
        return 0;
    }
    return c.Read(count, dst);
}

bool CStirlingDoc::SetByteNoUndo(stirling::FileOffset pos, unsigned char b) {
    // Undo スタックには触れない（上位ニブルのレコードへ畳み込む）。変更フラグのみ更新。
    stirling::BlockCursor c(&m_blocks);
    if (!c.SetByteAt(pos, b)) {
        return false;
    }
    SetModifiedFlag(TRUE);
    return true;
}

// ---- Undo 履歴の容量管理（Issue #30） ----

// 上限のバイト換算。0 は無制限を意味する。
//   チェックを外している場合のほか、設定ファイルを手で編集して 0 以下にした場合も
//   無制限として扱う（Issue #30 当時からの「0=無制限」の意味を残す）。
unsigned long long CStirlingDoc::UndoMemoryLimitBytes() {
    const CAppSettings& s = theApp.AppSettings();
    if (!s.undoMemoryLimit || s.undoMemoryLimitMB <= 0) {
        return 0;   // 無制限
    }
    return static_cast<unsigned long long>(s.undoMemoryLimitMB) * 1024ull * 1024ull;
}

// 上限の変更を直ちに反映する（環境設定の確定時。Issue #102）。
//   次の編集まで待つと、利用者が上限を下げて編集をやめた場合に超過分を抱えたままに
//   なるため、その場で切り詰める。
void CStirlingDoc::ApplyUndoMemoryLimit() {
    TrimUndoHistory();
}

namespace {

// スタックへ 1 件積める空きを確保する（Issue #153）。
//   空きがあれば何もしない。無ければ倍々で伸ばし（push_back の償却計算量を保つ）、
//   それが確保できなければ最小限（+1）で再試行する。どちらも失敗したら false。
template <class Vec>
bool ReserveOneSlot(Vec& v) {
    if (v.size() < v.capacity()) {
        return true;
    }
    const size_t grown = (v.capacity() == 0) ? 8 : v.capacity() * 2;
    try {
        v.reserve(grown);
        return true;
    } catch (const std::bad_alloc&) {
        // 倍化に失敗しても、あと 1 件なら載る可能性がある。
    } catch (const std::length_error&) {
        // 要求長が max_size 超過。以下の最小要求で判定し直す。
    }
    try {
        v.reserve(v.size() + 1);
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::length_error&) {
        return false;
    }
    return true;
}

}  // namespace

bool CStirlingDoc::ReserveUndoSlot() {
    return ReserveOneSlot(m_undoStack);   // ドキュメントを変更する前に諦めるための予約
}

bool CStirlingDoc::ReserveRedoSlot() {
    return ReserveOneSlot(m_redoStack);
}

bool CStirlingDoc::PushUndoRecord(EditRecord&& r) {
    try {
        m_undoStack.push_back(std::move(r));       // 確保に失敗しても計上が狂わないよう
    } catch (const std::bad_alloc&) {
        // 予約済みならここへは来ない。万一記録できなければ履歴を捨て、
        // 「この編集は取り消せない＝保存点も到達不能」という一貫した状態へ倒す。
        ClearUndoHistory(true);
        return false;
    }
    m_undoBytes += RecordBytes(m_undoStack.back());   // 成功後に加算する
    return true;
}

bool CStirlingDoc::PushRedoRecord(EditRecord&& r) {
    try {
        m_redoStack.push_back(std::move(r));
    } catch (const std::bad_alloc&) {
        ClearUndoHistory(true);
        return false;
    }
    m_undoBytes += RecordBytes(m_redoStack.back());
    return true;
}

void CStirlingDoc::ClearUndoHistory(bool savePointLost) {
    m_undoStack.clear();
    m_redoStack.clear();
    m_undoBytes = 0;
    if (savePointLost) {
        m_cleanUndoSize = -1;   // 保存済み状態へは Undo で戻れない
    }
}

// 保持合計が上限を超えている間、Undo スタック先頭（最古の編集）→ Redo スタック先頭
//   （Redo 連鎖の末端）の順に破棄する。各スタックの最後の 1 件は、上限を超えていても
//   残す（直近の取り消し／やり直しは常に可能にするため）。
void CStirlingDoc::TrimUndoHistory() {
    const unsigned long long limit = UndoMemoryLimitBytes();
    if (limit == 0 || m_undoBytes <= limit) {
        return;   // 無制限、または上限内（通常はここで抜ける）
    }
    std::vector<unsigned long long> undoBytes;
    undoBytes.reserve(m_undoStack.size());
    for (const EditRecord& r : m_undoStack) { undoBytes.push_back(RecordBytes(r)); }
    std::vector<unsigned long long> redoBytes;
    redoBytes.reserve(m_redoStack.size());
    for (const EditRecord& r : m_redoStack) { redoBytes.push_back(RecordBytes(r)); }

    const stirling::UndoTrimPlan plan = stirling::PlanUndoTrim(undoBytes, redoBytes, limit);
    m_undoStack.erase(m_undoStack.begin(),
                      m_undoStack.begin() + static_cast<ptrdiff_t>(plan.dropUndoFront));
    m_redoStack.erase(m_redoStack.begin(),
                      m_redoStack.begin() + static_cast<ptrdiff_t>(plan.dropRedoFront));
    m_undoBytes = plan.remainingBytes;
    // 保存点は「Undo 深さ」なので、破棄した件数だけ手前へずらす（捨てた側なら到達不能）。
    m_cleanUndoSize = stirling::ShiftSavePoint(m_cleanUndoSize, plan.dropUndoFront);
    // Redo 側を捨てた場合、保存点が到達可能な最大深さを超えることがある（Redo で戻れない）。
    const int reachable = static_cast<int>(m_undoStack.size() + m_redoStack.size());
    if (m_cleanUndoSize > reachable) {
        m_cleanUndoSize = -1;
    }
}

// 退避量が上限を超えるなら確認を出す（上限内なら無確認）。原には無い移植独自の保護。
CStirlingDoc::UndoCapacityDecision
CStirlingDoc::CheckUndoCapacity(stirling::FileOffset captureBytes) const {
    const unsigned long long limit = UndoMemoryLimitBytes();
    if (limit == 0 || captureBytes <= 0 ||
        static_cast<unsigned long long>(captureBytes) <= limit) {
        return UndoCapacityDecision::kNormal;
    }
    CStringW msg;
    msg.Format(ui::LoadW(IDS_CONFIRM_UNDOLESS_EDIT),
               (LPCWSTR)FormatBytesW(captureBytes),
               (LPCWSTR)FormatBytesW(static_cast<stirling::FileOffset>(limit)));
    return (ui::MsgBox(MainWndHandle(), msg, MB_YESNO | MB_ICONQUESTION) == IDYES)
               ? UndoCapacityDecision::kUndoless
               : UndoCapacityDecision::kCancel;
}

// ---- Undo/Redo コア ----

bool CStirlingDoc::ApplyRecord(const EditRecord& r, EditRecord& outInv) {
    EditRecord inv;
    switch (r.kind) {
    case EditRecord::kOverwrite: {
        // in-place 書換え。旧値を退避して逆レコード（再度 kOverwrite）を作る。
        // Issue #154: 範囲初期化が大きな kOverwrite レコードを積むようになったため、
        //   1 バイトずつ Seek し直す実装（O(n^2)）をやめ、Read/Write で一括処理する。
        std::vector<unsigned char> old;
        try {
            old.resize(r.bytes.size());
        } catch (const std::bad_alloc&) {
            return false;   // 退避できないならドキュメントを変更しない（Issue #153）
        }
        const stirling::FileOffset n = static_cast<stirling::FileOffset>(r.bytes.size());
        if (n > 0) {
            stirling::BlockCursor c(&m_blocks);
            if (!c.Seek(r.pos, stirling::BlockCursor::kBegin, nullptr)) {
                return false;
            }
            const stirling::FileOffset got = c.Read(n, old.data());
            old.resize(static_cast<size_t>(got));
            const stirling::FileOffset wrote = c.Write(r.pos, r.bytes.data(), got);
            old.resize(static_cast<size_t>(wrote));   // 実書込数へ合わせる
        }
        inv.kind = EditRecord::kOverwrite; inv.pos = r.pos; inv.bytes = std::move(old);
        break;
    }
    case EditRecord::kInsert: {
        stirling::BlockCursor c(&m_blocks);
        // 確保失敗時はドキュメントを変更せずに中断する（Issue #153）。
        if (!c.Insert(r.pos, r.bytes.data(), static_cast<stirling::FileOffset>(r.bytes.size()))) {
            return false;
        }
        inv.kind = EditRecord::kDelete; inv.pos = r.pos;
        inv.count = static_cast<stirling::FileOffset>(r.bytes.size());
        // ダイナミックマーク追従
        AdjustMarksForSplice(r.pos, 0, static_cast<stirling::FileOffset>(r.bytes.size()));
        break;
    }
    case EditRecord::kDelete: {
        // 退避に失敗したらドキュメントを一切変更せずに中断する。
        //   ここで削除してしまうと、逆レコードにバイト列が残らず復元不能になる。
        if (!FitsInBuffer(r.count)) { return false; }
        std::vector<unsigned char> cap;
        try {
            cap.resize(static_cast<size_t>(r.count));   // 退避量は Issue #30 参照
        } catch (const std::bad_alloc&) {
            return false;
        }
        if (!cap.empty()) {
            stirling::BlockCursor c(&m_blocks);
            if (c.Seek(r.pos, stirling::BlockCursor::kBegin, nullptr)) {
                const stirling::FileOffset n = c.Read(r.count, cap.data());
                cap.resize(static_cast<size_t>(n));
            } else {
                cap.clear();
            }
        }
        stirling::FileOffset nDel = 0;
        {
            stirling::BlockCursor d(&m_blocks);
            nDel = d.DeleteRange(r.pos, r.count);   // ブロック単位の一括削除（Issue #62）
        }
        if (nDel < static_cast<stirling::FileOffset>(cap.size())) {
            cap.resize(static_cast<size_t>(nDel));   // 実削除数へ合わせる
        }
        inv.kind = EditRecord::kInsert; inv.pos = r.pos; inv.bytes = std::move(cap);
        AdjustMarksForSplice(r.pos, nDel, 0);   // ダイナミックマーク追従
        break;
    }
    case EditRecord::kReplace: {
        // スプライス: pos で count バイト削除→bytes を挿入。自己反転（逆も kReplace）。
        // kDelete と同じく、退避に失敗したら変更せずに中断する。
        if (!FitsInBuffer(r.count)) { return false; }
        std::vector<unsigned char> cap;
        try {
            cap.resize(static_cast<size_t>(r.count));   // 退避量は Issue #30 参照
        } catch (const std::bad_alloc&) {
            return false;
        }
        if (!cap.empty()) {
            stirling::BlockCursor c(&m_blocks);
            if (c.Seek(r.pos, stirling::BlockCursor::kBegin, nullptr)) {
                const stirling::FileOffset n = c.Read(r.count, cap.data());
                cap.resize(static_cast<size_t>(n));
            } else {
                cap.clear();
            }
        }
        // ReplaceRange と同じく「挿入 → 削除」の順で行う。挿入は全か無かで失敗し得るが、
        // 削除は確保を伴わないため、この順序なら失敗時にドキュメントが無変更で済む
        //（Issue #153）。
        const stirling::FileOffset insLen = static_cast<stirling::FileOffset>(r.bytes.size());
        if (!r.bytes.empty()) {
            stirling::BlockCursor c(&m_blocks);
            if (!c.Insert(r.pos, r.bytes.data(), insLen)) {
                return false;
            }
        }
        stirling::FileOffset nDel = 0;
        {
            stirling::BlockCursor d(&m_blocks);
            // 挿入した分だけ削除開始位置が後ろへずれる。
            nDel = d.DeleteRange(r.pos + insLen, r.count);   // ブロック単位の一括削除（Issue #62）
        }
        if (nDel < static_cast<stirling::FileOffset>(cap.size())) {
            cap.resize(static_cast<size_t>(nDel));   // 実削除数へ合わせる
        }
        inv.kind = EditRecord::kReplace; inv.pos = r.pos;
        inv.count = static_cast<stirling::FileOffset>(r.bytes.size());
        inv.bytes = std::move(cap);
        // ダイナミックマーク追従
        AdjustMarksForSplice(r.pos, nDel, static_cast<stirling::FileOffset>(r.bytes.size()));
        break;
    }
    }
    outInv = std::move(inv);
    return true;
}

stirling::FileOffset CStirlingDoc::Undo() {
    if (m_undoStack.empty()) {
        return -1;
    }
    if (!ReserveRedoSlot()) {          // 逆レコードの領域を先に確保（Issue #153）
        return -1;
    }
    EditRecord r = std::move(m_undoStack.back());
    m_undoStack.pop_back();
    EditRecord inv;
    if (!ApplyRecord(r, inv)) {        // 適用できず（退避不能）: 元の状態へ戻す
        m_undoStack.push_back(std::move(r));   // 保持量は未精算のため増減なし
        return -1;
    }
    const stirling::FileOffset pos = r.pos;
    PushRedoRecord(std::move(inv));
    m_undoBytes -= RecordBytes(r);     // 取り出したレコードの分を減算（積んだ後に精算）
    TrimUndoHistory();                 // 逆レコードを積んだ結果の保持量で上限判定
    UpdateModifiedByUndo();            // 保存点に戻れば未変更（「*」除去）
    return pos;
}

stirling::FileOffset CStirlingDoc::Redo() {
    if (m_redoStack.empty()) {
        return -1;
    }
    if (!ReserveUndoSlot()) {          // 逆レコードの領域を先に確保（Issue #153）
        return -1;
    }
    EditRecord r = std::move(m_redoStack.back());
    m_redoStack.pop_back();
    EditRecord inv;
    if (!ApplyRecord(r, inv)) {        // 適用できず（退避不能）: 元の状態へ戻す
        m_redoStack.push_back(std::move(r));   // 保持量は未精算のため増減なし
        return -1;
    }
    const stirling::FileOffset pos = r.pos;
    PushUndoRecord(std::move(inv));
    m_undoBytes -= RecordBytes(r);     // 取り出したレコードの分を減算（積んだ後に精算）
    TrimUndoHistory();                 // 逆レコードを積んだ結果の保持量で上限判定
    UpdateModifiedByUndo();
    return pos;
}

// ---- マーク（原 doc+0x748 マップ。FUN_0042943a のトグル規則を移植） ----

void CStirlingDoc::ToggleMark(stirling::FileOffset pos, int type) {
    if (type < 0 || type > 2) {
        return;
    }
    auto it = m_marks.find(pos);
    if (it == m_marks.end()) {
        m_marks[pos] = type;            // 未登録 → 登録
    } else if (it->second == type) {
        m_marks.erase(it);              // 同種別 → 解除
    } else {
        it->second = type;              // 別種別 → 種別変更
    }
}

stirling::FileOffset CStirlingDoc::NextMark(stirling::FileOffset pos) const {
    if (m_marks.empty()) {
        return -1;
    }
    auto it = m_marks.upper_bound(pos);          // pos より後の最初
    if (it == m_marks.end()) {
        it = m_marks.begin();                    // 無ければ先頭へラップ
    }
    return it->first;
}

stirling::FileOffset CStirlingDoc::PrevMark(stirling::FileOffset pos) const {
    if (m_marks.empty()) {
        return -1;
    }
    auto it = m_marks.lower_bound(pos);          // pos 以上の最初
    if (it == m_marks.begin()) {
        return m_marks.rbegin()->first;          // pos より前が無ければ末尾へラップ
    }
    --it;
    return it->first;
}

void CStirlingDoc::ClearMarks() {
    m_marks.clear();
}

bool CStirlingDoc::GetMark(stirling::FileOffset pos, int* outType) const {
    auto it = m_marks.find(pos);
    if (it == m_marks.end()) {
        return false;
    }
    if (outType) { *outType = it->second; }
    return true;
}

void CStirlingDoc::SetMark(stirling::FileOffset pos, int type) {
    if (type < 0 || type > 2) { return; }
    m_marks[pos] = type;   // 未登録→登録 / 既存→種別（色）変更
}

void CStirlingDoc::RemoveMark(stirling::FileOffset pos) {
    m_marks.erase(pos);
}

// ダイナミックマーク（原 StirlingDoc_AdjustMarksAfterEdit FUN_00436e84。ゲート doc+0x324）。
//   スプライス（pos で delCount 削除→insCount 挿入）に対しマーク位置を追従させる。
//   pos 未満は不変 / [pos, pos+delCount) は消滅 / それ以降は (insCount-delCount) だけ移動。
//   位置は distinct を保つ（前群<pos<=後群移動先 なので衝突なし）。dynamicMark 無効時は無処理。
void CStirlingDoc::AdjustMarksForSplice(stirling::FileOffset pos, stirling::FileOffset delCount,
                                       stirling::FileOffset insCount) {
    if (!theApp.AppSettings().dynamicMark) { return; }
    if (m_marks.empty()) { return; }
    if (delCount <= 0 && insCount <= 0) { return; }   // 上書き等、移動なし
    const stirling::FileOffset delEnd = pos + delCount;
    const stirling::FileOffset shift = insCount - delCount;
    std::map<stirling::FileOffset, int> updated;
    for (const auto& kv : m_marks) {
        const stirling::FileOffset mp = kv.first;
        if (mp < pos) {
            updated[mp] = kv.second;              // 影響なし
        } else if (mp < delEnd) {
            // 削除範囲内 → 消滅（挿入があっても復活させない）
        } else {
            updated[mp + shift] = kv.second;      // 後方へ移動
        }
    }
    m_marks.swap(updated);
}
