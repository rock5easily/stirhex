// StreamFileWriter 実装（Issue #155）。
#include "StreamFileWriter.h"

#include <windows.h>

namespace stirling {
namespace {

FileIoResult MakeResult(FileIoStatus status, unsigned long systemError, FileOffset size) {
    FileIoResult r;
    r.status = status;
    r.systemError = systemError;
    r.fileSize = size;
    return r;
}

// 1 回の WriteFile へ渡す上限。DWORD の上限に近い値を渡すと環境によって短く書き込まれる
//   ことがあるため、扱いやすい 8MB で分割する（Win32/x64 とも同じ挙動にする）。
constexpr DWORD kWriteChunk = 8u * 1024u * 1024u;

// path のディレクトリ部（末尾の区切りを含む）。区切りが無ければカレントを表す "."。
std::wstring DirectoryOf(const std::wstring& path) {
    const size_t sep = path.find_last_of(L"\\/");
    if (sep == std::wstring::npos) { return L"."; }
    if (sep == 0) { return path.substr(0, 1); }        // "\file" → "\"
    return path.substr(0, sep);
}

// 出力先へ書き込めるかを、一時ファイルを作る前に確かめる（Issue #170）。
//   置換は最後に行うため、確認しないと「GB 単位を書き終えた後で権限不足に気付く」ことに
//   なる。読み取り専用属性・権限・共有違反（他プロセスが掴んでいる）をここで検出し、
//   原因を kOpenFailed として返す（呼出側は「保存できません」の見出しへ分岐できる）。
//   出力先が存在しないときは確認対象が無いので true（一時ファイル作成で判明する）。
bool CanReplaceTarget(const std::wstring& target, DWORD& err) {
    const DWORD attrs = ::GetFileAttributesW(target.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        // 「無い」以外の理由（親フォルダを辿れない・権限不足）を新規作成と混同しない。
        const DWORD code = ::GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return true;   // 新規作成（作れるかは一時ファイル作成で判明する）
        }
        err = code;
        return false;
    }
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        err = ERROR_ACCESS_DENIED;   // ディレクトリは置換対象にできない
        return false;
    }
    if ((attrs & FILE_ATTRIBUTE_READONLY) != 0) {
        err = ERROR_ACCESS_DENIED;   // 読み取り専用属性（置換も削除もできない）
        return false;
    }
    // 権限・共有状態は属性からは判らないため、開いて確かめる（OPEN_EXISTING。切り詰めない）。
    //   置換は出力先の削除を伴うため、書込権に加えて DELETE も要求する。
    HANDLE probe = ::CreateFileW(target.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (probe == INVALID_HANDLE_VALUE) {
        err = ::GetLastError();
        return false;
    }
    ::CloseHandle(probe);
    return true;
}

// 一時ファイルを出力先へ置き換える（Issue #170）。
//   出力先が既存なら ReplaceFileW を使う。MoveFileExW と違い、出力先の属性・代替データ
//   ストリーム・作成日時を引き継ぐため、付随情報を持つファイルを保存してもそれらが
//   失われない（アクセス権は引き継げる場合のみ。REPLACEFILE_IGNORE_MERGE_ERRORS を
//   付けているため、WRITE_DAC が無い等でアクセス権を移せなくても置換自体は成功する。
//   ファイル ID は置換で変わる）。共有違反で失敗することがあるので、設定ファイルの保存
//   （SettingsFile.cpp）と同じく短い間隔で数回だけ再試行する。
//   出力先が無い（新規作成）ときと ReplaceFileW が失敗したときは MoveFileExW へ退避する。
//   失敗したときは err に最初のエラーコードを返す。
bool ReplaceTargetWithTemp(const std::wstring& temp, const std::wstring& target, DWORD& err) {
    const int kAttempts = 5;
    const DWORD kWaitMs = 20;
    const bool exists = (::GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES);
    err = 0;
    if (exists) {
        for (int i = 0; i < kAttempts; ++i) {
            if (::ReplaceFileW(target.c_str(), temp.c_str(), nullptr,
                               REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
                return true;
            }
            const DWORD code = ::GetLastError();
            if (err == 0) { err = code; }
            if (code != ERROR_SHARING_VIOLATION && code != ERROR_LOCK_VIOLATION) { break; }
            ::Sleep(kWaitMs);
        }
    }
    // ReplaceFileW が使えない環境・状況（新規作成、ボリューム跨ぎ等）は移動で置き換える。
    //   ReplaceFileW が ERROR_UNABLE_TO_MOVE_REPLACEMENT で失敗した場合、出力先は既に
    //   削除され一時ファイルだけが残る（lpBackupFileName を渡していないため）。その状態を
    //   救えるのはこの移動だけなので、共有違反はここでも数回再試行する。
    for (int i = 0; i < kAttempts; ++i) {
        if (::MoveFileExW(temp.c_str(), target.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            return true;
        }
        const DWORD code = ::GetLastError();
        if (err == 0) { err = code; }
        if (code != ERROR_SHARING_VIOLATION && code != ERROR_LOCK_VIOLATION) { break; }
        ::Sleep(kWaitMs);
    }
    return false;
}

}  // namespace

StreamFileWriter::~StreamFileWriter() {
    Abort();
}

FileIoResult StreamFileWriter::Open(const wchar_t* path) {
    Abort();   // 開きっぱなしの一時ファイルを残さない
    written_ = 0;
    keptTempPath_.clear();   // 前回の取り残し情報は持ち越さない
    if (path == nullptr || *path == L'\0') {
        return MakeResult(FileIoStatus::kOpenFailed, ERROR_INVALID_NAME, 0);
    }
    targetPath_ = path;

    // 書き終えてから権限で弾かれないよう、先に出力先の置換可否を確かめる（Issue #170）。
    DWORD probeErr = 0;
    if (!CanReplaceTarget(targetPath_, probeErr)) {
        targetPath_.clear();
        return MakeResult(FileIoStatus::kOpenFailed, probeErr, 0);
    }

    // 出力先と同じディレクトリに一時ファイルを作る（置換を同一ボリューム内で行うため）。
    const std::wstring dir = DirectoryOf(targetPath_);
    wchar_t temp[MAX_PATH] = {0};
    if (::GetTempFileNameW(dir.c_str(), L"STH", 0, temp) == 0) {
        const DWORD err = ::GetLastError();
        targetPath_.clear();
        return MakeResult(FileIoStatus::kOpenFailed, err, 0);
    }
    tempPath_ = temp;   // GetTempFileNameW は空ファイルを作成済み

    HANDLE h = ::CreateFileW(tempPath_.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        ::DeleteFileW(tempPath_.c_str());
        tempPath_.clear();
        targetPath_.clear();
        return MakeResult(FileIoStatus::kOpenFailed, err, 0);
    }
    handle_ = h;
    return MakeResult(FileIoStatus::kOk, 0, 0);
}

FileIoResult StreamFileWriter::Write(const void* data, size_t size) {
    if (handle_ == nullptr) {
        return MakeResult(FileIoStatus::kWriteFailed, ERROR_INVALID_HANDLE, written_);
    }
    const unsigned char* p = static_cast<const unsigned char*>(data);
    size_t left = size;
    while (left > 0) {
        const DWORD want = (left < static_cast<size_t>(kWriteChunk))
                               ? static_cast<DWORD>(left) : kWriteChunk;
        DWORD wrote = 0;
        if (!::WriteFile(handle_, p, want, &wrote, nullptr)) {
            return MakeResult(FileIoStatus::kWriteFailed, ::GetLastError(), written_);
        }
        if (wrote == 0) {   // ディスク不足等で進まない
            return MakeResult(FileIoStatus::kWriteFailed, ERROR_WRITE_FAULT, written_);
        }
        p += wrote;
        left -= wrote;
        written_ += wrote;
    }
    return MakeResult(FileIoStatus::kOk, 0, written_);
}

FileIoResult StreamFileWriter::Commit() {
    if (handle_ == nullptr) {
        return MakeResult(FileIoStatus::kWriteFailed, ERROR_INVALID_HANDLE, written_);
    }
    // 遅延書込エラー（ディスク不足・リムーバブル/ネットワークの切断等）は、キャッシュから
    //   実デバイスへ書き出す時点で初めて表面化する。CloseHandle の戻り値では検出できない
    //   ため、置換の前に明示フラッシュして成否を確認する（Issue #166）。
    //   ここで失敗しても Abort() が一時ファイルを削除するだけで、出力先は元のまま。
    if (!::FlushFileBuffers(handle_)) {
        const DWORD err = ::GetLastError();
        Abort();   // handle_ もここで閉じる
        return MakeResult(FileIoStatus::kWriteFailed, err, written_);
    }
    // クローズ自体の失敗（無効ハンドル等）も握りつぶさない。
    if (!::CloseHandle(handle_)) {
        const DWORD err = ::GetLastError();
        handle_ = nullptr;
        Abort();
        return MakeResult(FileIoStatus::kWriteFailed, err, written_);
    }
    handle_ = nullptr;

    // 一時ファイルを FILE_ATTRIBUTE_TEMPORARY のまま残さない（置換後は通常ファイル）。
    ::SetFileAttributesW(tempPath_.c_str(), FILE_ATTRIBUTE_NORMAL);
    DWORD replaceErr = 0;
    if (!ReplaceTargetWithTemp(tempPath_, targetPath_, replaceErr)) {
        // 通常はここで出力先が元のまま残っているので、一時ファイルは削除して構わない。
        //   ただし ReplaceFileW が ERROR_UNABLE_TO_MOVE_REPLACEMENT で失敗し、続く移動も
        //   失敗した場合だけは、出力先が既に消えていて書いた内容は一時ファイルにしかない。
        //   その一時ファイルを消すとデータがどこにも残らないため、残して呼出側へ知らせる
        //   （keptTempPath_）。取り残しの掃除は利用者の判断に委ねる。
        if (::GetFileAttributesW(targetPath_.c_str()) == INVALID_FILE_ATTRIBUTES) {
            keptTempPath_ = tempPath_;
            tempPath_.clear();   // Abort() に削除させない
        }
        Abort();   // 置換できなければ出力先は元のまま
        return MakeResult(FileIoStatus::kWriteFailed, replaceErr, written_);
    }
    const FileOffset total = written_;
    tempPath_.clear();
    targetPath_.clear();
    return MakeResult(FileIoStatus::kOk, 0, total);
}

void StreamFileWriter::Abort() {
    if (handle_ != nullptr) {
        ::CloseHandle(handle_);
        handle_ = nullptr;
    }
    if (!tempPath_.empty()) {
        ::DeleteFileW(tempPath_.c_str());
        tempPath_.clear();
    }
    targetPath_.clear();
}

}  // namespace stirling
