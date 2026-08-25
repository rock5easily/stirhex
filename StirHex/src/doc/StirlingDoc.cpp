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
#include <new>   // std::bad_alloc（巨大範囲の退避失敗を捕捉する。Issue #30）

namespace {

// FileOffset(64bit) の長さが、このビルドのバッファ長(size_t)に収まるか。
//   x86 では size_t が 32bit のため、4GB 超をそのままキャストすると下位32bitへ
//   黙って切り捨てられ、確保より大きな読取でヒープを破壊しうる。必ず先に弾く。
bool FitsInBuffer(stirling::FileOffset n) {
    if (n < 0) { return false; }
    return static_cast<unsigned long long>(n) <=
           static_cast<unsigned long long>((std::numeric_limits<size_t>::max)());
}

}  // namespace

namespace {

// 巨大ファイルを開く前に確認する閾値（原には無い移植独自の保護。Issue #20）。
//   BlockList はファイルサイズとほぼ同量のメモリを消費するため、事前に確認する。
constexpr stirling::FileOffset kLargeFileConfirmBytes = 512LL * 1024 * 1024;   // 512MB

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
    if (s.defCharset >= 0 && s.defCharset <= 5) { m_charset = s.defCharset; }
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

// 排他制御（原 exclusiveControl）: 共有モード付きの参照ハンドルをドキュメント存続期間中保持する。
//   0=しない（保持しない）/ 1=書込禁止（FILE_SHARE_READ で他プロセスの書込を拒否）/
//   2=読書禁止（共有なし＝排他）。取得失敗時は保持しない（他が既にロック中の場合など）。
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
    m_blocks.AppendBlock(new unsigned char[stirling::kBlockCapacity], stirling::kBlockCapacity, 0);
    m_settings = theApp.SettingsForPath(nullptr);   // 新規は既定 "*" レコード
    ApplyOpenDefaults(false);   // 新規は編集可のまま（文字セット/バイトオーダ/挿入既定のみ）
    return TRUE;
}

BOOL CStirlingDoc::OnOpenDocument(LPCTSTR lpszPathName) {
    DeleteContents();
    // core は 64bit オフセット・ワイドパスの API（Issue #20）。MBCS ビルドのため境界で変換する。
    const CStringW wpath(lpszPathName);

    // 巨大ファイルは読み込み前に確認する（ファイルサイズ相当のメモリを消費するため）。
    stirling::FileOffset probeSize = 0;
    if (stirling::QueryFileSize(wpath, &probeSize, nullptr) &&
        probeSize >= kLargeFileConfirmBytes) {
        CStringW confirm;
        confirm.Format(ui::LoadW(IDS_CONFIRM_LARGE_FILE), (LPCWSTR)FormatBytesW(probeSize));
        if (ui::MsgBox(MainWndHandle(), confirm, MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return FALSE;   // 利用者が中止（MFC 側の追加メッセージは出ない）
        }
    }

    const stirling::FileIoResult loaded = stirling::LoadFileIntoBlocks(m_blocks, wpath);
    if (!loaded.Ok()) {
        // MFC は OnOpenDocument が FALSE のとき自前のメッセージを出さない契約のため、
        // ここで理由付きのメッセージを表示する。
        ShowFileIoError(IDS_ERR_LOAD_FAILED, lpszPathName, loaded);
        return FALSE;
    }
    // 拡張子で表示設定レコードを解決（この時点で GetPathName は未設定のため引数パスを使う）。
    m_settings = theApp.SettingsForPath(CStringW(lpszPathName));
    ApplyOpenDefaults(true);    // ファイルオープン時は編集禁止既定も適用
    AcquireLock(lpszPathName);  // 排他制御（設定に応じて共有モード付きハンドルを保持）
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
    stirling::FileOffset actualDel = 0;   // 実際に削除できたバイト数
    if (delLen > 0) {
        if (!FitsInBuffer(delLen)) {
            return false;   // このビルドのバッファに載らない長さ（データは無傷）
        }
        switch (CheckUndoCapacity(delLen)) {
        case UndoCapacityDecision::kCancel:   return false;   // 中止（データは無傷）
        case UndoCapacityDecision::kUndoless: undoless = true; break;
        case UndoCapacityDecision::kNormal:   break;
        }
        stirling::FileOffset target = delLen;   // 削除を試みるバイト数
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
        {
            stirling::BlockCursor d(&m_blocks);
            actualDel = d.DeleteRange(pos, target);   // ブロック単位の一括削除（Issue #62）
        }
        if (actualDel < target && !undoless) {
            removed.resize(static_cast<size_t>(actualDel));   // 実削除数へ合わせる
        }
    }
    // 実際に挿入できたバイト数（失敗しても削除済みデータを復元できるよう記録に反映する）。
    stirling::FileOffset insLen = 0;
    if (!ins.empty()) {
        stirling::BlockCursor c(&m_blocks);
        if (c.Insert(pos, ins.data(), static_cast<stirling::FileOffset>(ins.size()))) {
            insLen = static_cast<stirling::FileOffset>(ins.size());
        }
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
        PushUndoRecord(std::move(rec));
    }
    AdjustMarksForSplice(pos, actualDel, insLen);   // ダイナミックマーク: 置換に追従
    CommitForwardEdit();   // Redo破棄＋容量トリム＋保存点比較で変更フラグ更新
    return true;
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

// 上限のバイト換算。0（および負値）は無制限を意味する。
unsigned long long CStirlingDoc::UndoMemoryLimitBytes() {
    const int mb = theApp.AppSettings().undoMemoryLimitMB;
    if (mb <= 0) {
        return 0;   // 無制限
    }
    return static_cast<unsigned long long>(mb) * 1024ull * 1024ull;
}

void CStirlingDoc::PushUndoRecord(EditRecord&& r) {
    m_undoStack.push_back(std::move(r));           // 確保に失敗しても計上が狂わないよう
    m_undoBytes += RecordBytes(m_undoStack.back());   // 成功後に加算する
}

void CStirlingDoc::PushRedoRecord(EditRecord&& r) {
    m_redoStack.push_back(std::move(r));
    m_undoBytes += RecordBytes(m_redoStack.back());
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
        std::vector<unsigned char> old(r.bytes.size());
        for (size_t i = 0; i < r.bytes.size(); ++i) {
            unsigned char o = 0;
            GetByteAt(r.pos + static_cast<stirling::FileOffset>(i), &o);
            old[i] = o;
            stirling::BlockCursor c(&m_blocks);
            c.SetByteAt(r.pos + static_cast<stirling::FileOffset>(i), r.bytes[i]);
        }
        inv.kind = EditRecord::kOverwrite; inv.pos = r.pos; inv.bytes = std::move(old);
        break;
    }
    case EditRecord::kInsert: {
        stirling::BlockCursor c(&m_blocks);
        c.Insert(r.pos, r.bytes.data(), static_cast<stirling::FileOffset>(r.bytes.size()));
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
        stirling::FileOffset nDel = 0;
        {
            stirling::BlockCursor d(&m_blocks);
            nDel = d.DeleteRange(r.pos, r.count);   // ブロック単位の一括削除（Issue #62）
        }
        if (nDel < static_cast<stirling::FileOffset>(cap.size())) {
            cap.resize(static_cast<size_t>(nDel));   // 実削除数へ合わせる
        }
        if (!r.bytes.empty()) {
            stirling::BlockCursor c(&m_blocks);
            c.Insert(r.pos, r.bytes.data(), static_cast<stirling::FileOffset>(r.bytes.size()));
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
