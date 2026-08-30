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

}  // namespace

StreamFileWriter::~StreamFileWriter() {
    Abort();
}

FileIoResult StreamFileWriter::Open(const wchar_t* path) {
    Abort();   // 開きっぱなしの一時ファイルを残さない
    written_ = 0;
    if (path == nullptr || *path == L'\0') {
        return MakeResult(FileIoStatus::kOpenFailed, ERROR_INVALID_NAME, 0);
    }
    targetPath_ = path;

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
    // 遅延書込エラーはクローズで初めて表面化することがあるため戻り値を確認する。
    if (!::CloseHandle(handle_)) {
        const DWORD err = ::GetLastError();
        handle_ = nullptr;
        Abort();
        return MakeResult(FileIoStatus::kWriteFailed, err, written_);
    }
    handle_ = nullptr;

    // 一時ファイルを FILE_ATTRIBUTE_TEMPORARY のまま残さない（置換後は通常ファイル）。
    ::SetFileAttributesW(tempPath_.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!::MoveFileExW(tempPath_.c_str(), targetPath_.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        const DWORD err = ::GetLastError();
        Abort();   // 置換できなければ出力先は元のまま
        return MakeResult(FileIoStatus::kWriteFailed, err, written_);
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
