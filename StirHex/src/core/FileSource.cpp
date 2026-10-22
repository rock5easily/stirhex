// FileSource 実装（Issue #300）。
#include "FileSource.h"

#include "Win32FileHooks.h"   // I/O 故障注入のための薄いラッパ（Issue #180）

#include <windows.h>

#include <cstring>
#include <new>
#include <utility>
#include "core/AllocFailureScope.h"   // 確保失敗を std::bad_alloc で受ける区間（Issue #306）

namespace stirling {
namespace {

// 1 回の ReadFile / WriteFile へ渡す上限（StreamFileWriter と同じ 8MB）。
constexpr DWORD kIoChunk = 8u * 1024u * 1024u;

bool SeekTo(HANDLE h, FileOffset pos) {
    LARGE_INTEGER li;
    li.QuadPart = pos;
    return ::SetFilePointerEx(h, li, nullptr, FILE_BEGIN) != FALSE;
}

}  // namespace

FileSource::FileSource(void* handle, std::wstring path, FileOffset size,
                       unsigned long shareFlags, bool writable)
    : handle_(handle), path_(std::move(path)), size_(size),
      shareFlags_(shareFlags), writable_(writable) {
    if (handle_ == INVALID_HANDLE_VALUE) { handle_ = nullptr; }
    identity_ = IdentityOf(handle_);
}

FileSource::Identity FileSource::IdentityOf(void* handle) {
    Identity id;
    BY_HANDLE_FILE_INFORMATION info = {};
    if (handle == nullptr || !::GetFileInformationByHandle(static_cast<HANDLE>(handle), &info)) {
        return id;
    }
    id.volume = info.dwVolumeSerialNumber;
    id.indexHigh = info.nFileIndexHigh;
    id.indexLow = info.nFileIndexLow;
    id.lastWrite = (static_cast<unsigned long long>(info.ftLastWriteTime.dwHighDateTime) << 32) |
                   info.ftLastWriteTime.dwLowDateTime;
    id.valid = true;
    return id;
}

bool FileSource::StampAfterWrite() {
    if (handle_ == nullptr) {
        lastError_ = ERROR_INVALID_HANDLE;
        return false;
    }
    FILETIME now = {};
    ::GetSystemTimeAsFileTime(&now);
    if (!::SetFileTime(static_cast<HANDLE>(handle_), nullptr, nullptr, &now)) {
        lastError_ = ::GetLastError();
        return false;
    }
    identity_ = IdentityOf(handle_);
    return identity_.valid;
}

FileSource::~FileSource() {
    Close();
}

void FileSource::InvalidateCache() {
    for (Slot& s : slots_) {
        s.base = -1;
        s.len = 0;
    }
    ++epoch_;
}

void FileSource::Close() {
    if (handle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
    InvalidateCache();
}

bool FileSource::Reopen(bool writable) {
    Close();
    const DWORD access = GENERIC_READ | (writable ? GENERIC_WRITE : 0);
    HANDLE h = ::CreateFileW(path_.c_str(), access, shareFlags_, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        lastError_ = ::GetLastError();
        return false;
    }
    LARGE_INTEGER li = {};
    const Identity now = IdentityOf(h);
    const bool same = ::GetFileSizeEx(h, &li) && li.QuadPart == size_ && now.valid &&
                      identity_.valid && now.volume == identity_.volume &&
                      now.indexHigh == identity_.indexHigh && now.indexLow == identity_.indexLow &&
                      now.lastWrite == identity_.lastWrite;
    if (!same) {
        lastError_ = ERROR_FILE_INVALID;   // 閉じている間に置き換え・変更された（参照ノードの前提が崩れた）
        ::CloseHandle(h);
        return false;
    }
    handle_ = h;
    writable_ = writable;
    return true;
}

bool FileSource::ReadDirect(FileOffset pos, void* dst, size_t len) {
    if (!ReadDirectOnce(pos, dst, len)) {
        ++readFailures_;   // 読み取りの失敗（確保の失敗は数えない）。ReadFailures() 参照
        return false;
    }
    return true;
}

bool FileSource::ReadDirectOnce(FileOffset pos, void* dst, size_t len) {
    if (handle_ == nullptr) {
        lastError_ = ERROR_INVALID_HANDLE;
        return false;
    }
    if (pos < 0 || pos > size_ ||
        static_cast<unsigned long long>(len) > static_cast<unsigned long long>(size_ - pos)) {
        lastError_ = ERROR_HANDLE_EOF;
        return false;
    }
    HANDLE h = static_cast<HANDLE>(handle_);
    if (!SeekTo(h, pos)) {
        lastError_ = ::GetLastError();
        return false;
    }
    unsigned char* p = static_cast<unsigned char*>(dst);
    size_t left = len;
    while (left > 0) {
        const DWORD want = (left < kIoChunk) ? static_cast<DWORD>(left) : kIoChunk;
        DWORD got = 0;
        if (!io::Read(h, p, want, &got)) {
            lastError_ = ::GetLastError();
            return false;
        }
        if (got == 0) {   // 開いている間に縮むことは共有モードで防いでいるが、念のため
            lastError_ = ERROR_HANDLE_EOF;
            return false;
        }
        p += got;
        left -= got;
    }
    return true;
}

const unsigned char* FileSource::Page(FileOffset pos, FileOffset* outBase, int* outLen) {
    if (pos < 0 || pos >= size_) {
        lastError_ = ERROR_HANDLE_EOF;
        ++readFailures_;
        return nullptr;
    }
    const FileOffset base = pos - (pos % kPageSize);
    ++useClock_;
    Slot* victim = nullptr;
    for (Slot& s : slots_) {
        if (s.base == base) {
            s.lastUse = useClock_;
            *outBase = base;
            *outLen = s.len;
            return s.data.get();
        }
        if (victim == nullptr || s.lastUse < victim->lastUse) { victim = &s; }
    }
    if (slots_.size() < static_cast<size_t>(kPageCount)) {
        try {
            const stirling::StdAllocScope allocScope;
            slots_.emplace_back();
        } catch (const std::bad_alloc&) {
            lastError_ = ERROR_NOT_ENOUGH_MEMORY;
            return nullptr;
        }
        victim = &slots_.back();
    }
    if (!victim->data) {
        victim->data.reset(new (std::nothrow) unsigned char[kPageSize]);
        if (!victim->data) {
            lastError_ = ERROR_NOT_ENOUGH_MEMORY;
            return nullptr;
        }
    }
    if (victim->base >= 0) { ++epoch_; }   // 使用中のページを追い出す
    victim->base = -1;
    const FileOffset rest = size_ - base;
    const int len = (rest < kPageSize) ? static_cast<int>(rest) : kPageSize;
    if (!ReadDirect(base, victim->data.get(), static_cast<size_t>(len))) {
        return nullptr;
    }
    victim->base = base;
    victim->len = len;
    victim->lastUse = useClock_;
    *outBase = base;
    *outLen = len;
    return victim->data.get();
}

bool FileSource::Read(FileOffset pos, void* dst, size_t len) {
    if (len == 0) { return true; }
    if (len >= static_cast<size_t>(kPageSize)) {
        return ReadDirect(pos, dst, len);
    }
    unsigned char* p = static_cast<unsigned char*>(dst);
    while (len > 0) {
        FileOffset base = 0;
        int pageLen = 0;
        const unsigned char* page = Page(pos, &base, &pageLen);
        if (page == nullptr) { return false; }
        const int off = static_cast<int>(pos - base);
        const size_t avail = static_cast<size_t>(pageLen - off);
        const size_t n = (len < avail) ? len : avail;
        std::memcpy(p, page + off, n);
        p += n;
        pos += static_cast<FileOffset>(n);
        len -= n;
    }
    return true;
}

bool FileSource::Write(FileOffset pos, const void* src, size_t len) {
    if (handle_ == nullptr || !writable_) {
        lastError_ = ERROR_ACCESS_DENIED;
        return false;
    }
    HANDLE h = static_cast<HANDLE>(handle_);
    if (!SeekTo(h, pos)) {
        lastError_ = ::GetLastError();
        return false;
    }
    const unsigned char* p = static_cast<const unsigned char*>(src);
    size_t left = len;
    while (left > 0) {
        const DWORD want = (left < kIoChunk) ? static_cast<DWORD>(left) : kIoChunk;
        DWORD wrote = 0;
        if (!io::Write(h, p, want, &wrote)) {
            lastError_ = ::GetLastError();
            return false;
        }
        if (wrote == 0) {
            lastError_ = ERROR_WRITE_FAULT;
            return false;
        }
        p += wrote;
        left -= wrote;
    }
    return true;
}

bool FileSource::Flush() {
    if (handle_ == nullptr) {
        lastError_ = ERROR_INVALID_HANDLE;
        return false;
    }
    if (!io::Flush(static_cast<HANDLE>(handle_))) {
        lastError_ = ::GetLastError();
        return false;
    }
    return true;
}

}  // namespace stirling
