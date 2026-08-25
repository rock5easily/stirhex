// クリップボード転送のリソース管理（RAII）。
//   グローバルメモリの確保／ロック／解放と、クリップボードの開閉を型で対応付け、
//   エラー経路でのリークと、SetClipboardData 成功後の二重解放を構造的に防ぐ。
//   所有権の規則: SetClipboardData が成功した時点で HGLOBAL の所有権はクリップボードへ
//   移り、呼び出し側は GlobalFree してはならない。失敗・中断時のみ解放が必要となる。
//   MFC 非依存（Win32 のみ）にしてコア機能テストから単体で検証できるようにする。
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace ui {

// GlobalAlloc したメモリの所有権を持つ RAII ラッパ（move only）。
//   Release() で所有権を手放す（クリップボードへ渡すときに使う）。
class GlobalMemory {
public:
    GlobalMemory() = default;

    // bytes バイトを確保する。失敗しても例外は投げない（IsValid() で判定する）。
    explicit GlobalMemory(SIZE_T bytes, UINT flags = GMEM_MOVEABLE)
        : m_handle(::GlobalAlloc(flags, bytes)) {}

    ~GlobalMemory() { Reset(); }

    GlobalMemory(const GlobalMemory&) = delete;
    GlobalMemory& operator=(const GlobalMemory&) = delete;

    GlobalMemory(GlobalMemory&& other) noexcept : m_handle(other.m_handle) {
        other.m_handle = nullptr;
    }
    GlobalMemory& operator=(GlobalMemory&& other) noexcept {
        if (this != &other) {
            Reset();
            m_handle = other.m_handle;
            other.m_handle = nullptr;
        }
        return *this;
    }

    bool IsValid() const { return m_handle != nullptr; }
    HGLOBAL Get() const { return m_handle; }

    // 所有権を手放してハンドルを返す（以後このオブジェクトは解放しない）。
    HGLOBAL Release() {
        HGLOBAL h = m_handle;
        m_handle = nullptr;
        return h;
    }

    // 保持しているメモリを解放する。
    void Reset() {
        if (m_handle != nullptr) {
            ::GlobalFree(m_handle);
            m_handle = nullptr;
        }
    }

private:
    HGLOBAL m_handle = nullptr;
};

// GlobalLock / GlobalUnlock のスコープガード。
//   ロックに失敗した場合 Get() は nullptr を返す（呼び出し側でエラーとして扱う）。
class GlobalLockGuard {
public:
    explicit GlobalLockGuard(HGLOBAL handle)
        : m_handle(handle), m_p((handle != nullptr) ? ::GlobalLock(handle) : nullptr) {}

    ~GlobalLockGuard() { Unlock(); }

    GlobalLockGuard(const GlobalLockGuard&) = delete;
    GlobalLockGuard& operator=(const GlobalLockGuard&) = delete;

    bool IsLocked() const { return m_p != nullptr; }
    void* Get() const { return m_p; }

    // 明示的にロックを解除する（デストラクタでも解除されるので通常は不要）。
    void Unlock() {
        if (m_p != nullptr) {
            ::GlobalUnlock(m_handle);
            m_p = nullptr;
        }
    }

private:
    HGLOBAL m_handle = nullptr;
    void* m_p = nullptr;
};

// OpenClipboard / CloseClipboard の RAII。
//   他プロセスがロックしている場合 OpenClipboard は失敗する。IsOpen() で判定すること。
class ClipboardSession {
public:
    explicit ClipboardSession(HWND owner) : m_open(::OpenClipboard(owner) != FALSE) {
        if (!m_open) {
            m_error = ::GetLastError();
        }
    }

    ~ClipboardSession() {
        if (m_open) {
            ::CloseClipboard();
        }
    }

    ClipboardSession(const ClipboardSession&) = delete;
    ClipboardSession& operator=(const ClipboardSession&) = delete;

    bool IsOpen() const { return m_open; }

    // OpenClipboard 失敗時の Win32 エラーコード（成功時は ERROR_SUCCESS）。
    DWORD Error() const { return m_error; }

    // クリップボードを空にする（所有権の取得を伴う）。
    bool Empty() {
        if (!m_open) { return false; }
        if (::EmptyClipboard()) { return true; }
        m_error = ::GetLastError();
        return false;
    }

    // データを設定する。成功した場合のみ mem の所有権を手放す（クリップボードへ移譲）。
    //   失敗時は mem が所有したままとなり、スコープ終了で解放される。
    bool SetData(UINT format, GlobalMemory& mem) {
        if (!m_open || !mem.IsValid()) { return false; }
        if (::SetClipboardData(format, mem.Get()) == nullptr) {
            m_error = ::GetLastError();
            return false;
        }
        mem.Release();
        return true;
    }

private:
    bool m_open = false;
    DWORD m_error = ERROR_SUCCESS;
};


// 転送用バッファ（要素数 count + NUL 終端 1 個）を確保し、src を複写して終端を付ける。
//   確保・ロックに失敗した場合は無効な GlobalMemory を返す。
//   outError : 失敗理由（成功時は ERROR_SUCCESS）。GlobalAlloc / GlobalLock の直後に
//              取得する。解放（GlobalFree）で上書きされる前に退避するため、Reset の前に読む。
template <typename CharT>
inline GlobalMemory MakeClipboardTextBuffer(const CharT* src, size_t count, DWORD& outError) {
    outError = ERROR_SUCCESS;
    // count + 1 は count == SIZE_MAX でラップするため、加算前の形で判定する。
    if (count >= SIZE_MAX / sizeof(CharT)) {
        outError = ERROR_ARITHMETIC_OVERFLOW;   // バイト数の計算が溢れる長さは扱わない
        return GlobalMemory();
    }
    GlobalMemory mem((count + 1) * sizeof(CharT));
    if (!mem.IsValid()) {
        outError = ::GetLastError();
        return mem;
    }
    GlobalLockGuard lock(mem.Get());
    if (!lock.IsLocked()) {
        outError = ::GetLastError();   // GlobalFree で上書きされる前に退避する
        mem.Reset();                   // ロックできないメモリは渡せない。ここで解放する
        return mem;
    }
    CharT* p = static_cast<CharT*>(lock.Get());
    if (count != 0) {
        std::memcpy(p, src, count * sizeof(CharT));
    }
    p[count] = static_cast<CharT>(0);
    return mem;   // ロックはここで解除される（クリップボードへ渡すのは解除後）
}

// 確保済みメモリを 1 書式ぶんのクリップボード内容として転送する（既存内容は破棄する）。
//   成功時のみ mem の所有権がクリップボードへ移る。失敗時は mem が保持したまま
//   （＝呼び出し元のスコープ終了で解放される）。
//   outError : 失敗時の Win32 エラーコード（成功時は ERROR_SUCCESS）。
inline bool PutClipboardOwned(HWND owner, UINT format, GlobalMemory& mem, DWORD& outError) {
    outError = ERROR_SUCCESS;
    if (!mem.IsValid()) {
        outError = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
    // クリップボードを開くのは内容を組み立てた後にする（他プロセスを待たせる時間を最小化）。
    ClipboardSession clipboard(owner);
    if (!clipboard.IsOpen() || !clipboard.Empty() || !clipboard.SetData(format, mem)) {
        // API が理由を残さない場合もあるため、失敗時に成功コードを返さないようにする。
        outError = (clipboard.Error() != ERROR_SUCCESS) ? clipboard.Error() : ERROR_ACCESS_DENIED;
        return false;
    }
    return true;
}

// ワイド文字列をクリップボードへ設定する（CF_UNICODETEXT。wide 層／ASCII 層向け）。
//   CF_TEXT は OS が自動合成するため、別途設定する必要はない。
//   length は要素数（終端は含めない）。x64 で 2GB 超の選択範囲も扱えるよう size_t で受ける。
inline bool PutClipboardTextW(HWND owner, const wchar_t* text, size_t length, DWORD& outError) {
    if (text == nullptr) {
        outError = ERROR_INVALID_PARAMETER;
        return false;
    }
    GlobalMemory mem = MakeClipboardTextBuffer(text, length, outError);
    if (!mem.IsValid()) {
        return false;   // 失敗理由は MakeClipboardTextBuffer が設定済み
    }
    return PutClipboardOwned(owner, CF_UNICODETEXT, mem, outError);
}

// 生バイト列を CF_TEXT としてクリップボードへ設定する（byte 層）。
//   [byte層] 編集対象のバイト列をそのまま他アプリへ渡す。ワイド化しないこと。
//     理由: 不正な多バイト列が置換文字へ潰れると貼り付け先の内容が変わる。
//     詳細: analysis_artifacts/docs/20_unicode_layering.md §6.5
//   テキスト書式ゆえの制約（原版と同じ挙動として受け入れる）:
//     - 0x00 を含むバイト列は、受け側でそこまでの文字列として扱われる。
//     - 受け側が CF_UNICODETEXT を要求した場合、OS の自動合成はシステム ANSI
//       コードページを使うため、非日本語環境では CP932 として解釈されない。
//     どちらも「バイト列の同一性が仕様」という前提を崩さずに解消できないため、
//     バイナリとしての貼り付けが必要な場合は内部クリップボード（Stirling 間のコピー）を使う。
//   length はバイト数（終端は含めない）。x64 で 2GB 超の選択範囲も扱えるよう size_t で受ける。
inline bool PutClipboardTextA(HWND owner, const char* text, size_t length, DWORD& outError) {
    if (text == nullptr) {
        outError = ERROR_INVALID_PARAMETER;
        return false;
    }
    GlobalMemory mem = MakeClipboardTextBuffer(text, length, outError);
    if (!mem.IsValid()) {
        return false;   // 失敗理由は MakeClipboardTextBuffer が設定済み
    }
    return PutClipboardOwned(owner, CF_TEXT, mem, outError);
}

}  // namespace ui
