// カーネルハンドルの RAII ラッパ（Issue #48 / 親 #16）。
//   早期 return や例外での解放漏れをなくすため、CloseHandle / FindClose を
//   スコープ離脱で自動実行する。UI 層・core 層で共有する。
#pragma once

#include <windows.h>

namespace stirling {

// CloseHandle で閉じるハンドル（ファイル・ミューテックス等）の所有権を持つ。
class ScopedHandle {
public:
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE h) : h_(h) {}
    ~ScopedHandle() { Close(); }

    ScopedHandle(ScopedHandle&& other) noexcept : h_(other.Release()) {}
    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) {
            Close();
            h_ = other.Release();
        }
        return *this;
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    // CreateFileW は INVALID_HANDLE_VALUE、CreateMutexW は nullptr を失敗値に使うため両方を無効扱いとする。
    bool   Valid() const { return h_ != INVALID_HANDLE_VALUE && h_ != nullptr; }
    HANDLE Get() const { return h_; }

    // 所有権を手放す（呼び出し側が閉じる責務を負う）。
    HANDLE Release() {
        HANDLE h = h_;
        h_ = INVALID_HANDLE_VALUE;
        return h;
    }

    // 現在のハンドルを閉じて置き換える。
    void Reset(HANDLE h = INVALID_HANDLE_VALUE) {
        if (h == h_) { return; }
        Close();
        h_ = h;
    }

    // 明示クローズ。クローズ自体の成否（無効ハンドル等）を返すのみで、遅延書込エラーの
    //   検出は保証しない。書込側は成功を返す前に FlushFileBuffers を呼ぶこと（Issue #166）。
    bool Close() {
        if (!Valid()) {
            h_ = INVALID_HANDLE_VALUE;
            return true;
        }
        const BOOL ok = ::CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
        return ok != FALSE;
    }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
};

// FindFirstFileW の検索ハンドル（解放は FindClose。CloseHandle ではない）。
class ScopedFindHandle {
public:
    ScopedFindHandle() = default;
    explicit ScopedFindHandle(HANDLE h) : h_(h) {}
    ~ScopedFindHandle() { Close(); }

    ScopedFindHandle(ScopedFindHandle&& other) noexcept : h_(other.Release()) {}
    ScopedFindHandle& operator=(ScopedFindHandle&& other) noexcept {
        if (this != &other) {
            Close();
            h_ = other.Release();
        }
        return *this;
    }

    ScopedFindHandle(const ScopedFindHandle&) = delete;
    ScopedFindHandle& operator=(const ScopedFindHandle&) = delete;

    bool   Valid() const { return h_ != INVALID_HANDLE_VALUE && h_ != nullptr; }
    HANDLE Get() const { return h_; }

    HANDLE Release() {
        HANDLE h = h_;
        h_ = INVALID_HANDLE_VALUE;
        return h;
    }

    void Close() {
        if (Valid()) { ::FindClose(h_); }
        h_ = INVALID_HANDLE_VALUE;
    }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
};

}  // namespace stirling
