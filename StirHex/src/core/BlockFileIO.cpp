// BlockFileIO 実装。原 CStirlingDoc の Load/Save 中核を MFC 非依存で移植。
//
// x64 化(Issue #20): 原(および移植初期)の fopen/fseek/ftell は long(32bit) オフセットで
// 2GB 超のファイルを扱えないため、CreateFileW + GetFileSizeEx + ReadFile/WriteFile へ
// 置き換えた。ブロック分割の手順（1.6MB チャンク読み→16KB ブロック列）は原と同一。
#include "BlockFileIO.h"

#include "util/ScopedHandle.h"

#include <windows.h>

#include <cstring>
#include <new>
#include <vector>

namespace stirling {
namespace {

FileIoResult MakeResult(FileIoStatus status, unsigned long systemError, FileOffset fileSize) {
    FileIoResult r;
    r.status = status;
    r.systemError = systemError;
    r.fileSize = fileSize;
    return r;
}

}  // namespace

bool QueryFileSize(const wchar_t* path, FileOffset* outSize, FileIoResult* outErr) {
    if (outSize != nullptr) { *outSize = 0; }
    if (path == nullptr || *path == L'\0') {
        if (outErr != nullptr) {
            *outErr = MakeResult(FileIoStatus::kOpenFailed, ERROR_INVALID_NAME, 0);
        }
        return false;
    }
    // ファイルを開かずに属性のみ取得する（排他・共有の状態に影響しない）。
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!::GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) {
        if (outErr != nullptr) {
            *outErr = MakeResult(FileIoStatus::kSizeFailed, ::GetLastError(), 0);
        }
        return false;
    }
    // ディレクトリは GetFileAttributesExW が成功してしまうため明示的に弾く
    //   （サイズ 0 のファイルと区別できなくなるのを防ぐ）。
    if ((fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        if (outErr != nullptr) {
            *outErr = MakeResult(FileIoStatus::kOpenFailed, ERROR_DIRECTORY, 0);
        }
        return false;
    }
    // nFileSizeHigh/Low は符号なし。ULARGE_INTEGER で合成する。
    ULARGE_INTEGER uli;
    uli.HighPart = fad.nFileSizeHigh;
    uli.LowPart  = fad.nFileSizeLow;
    const FileOffset size = static_cast<FileOffset>(uli.QuadPart);
    if (outSize != nullptr) { *outSize = size; }
    if (outErr != nullptr) { *outErr = MakeResult(FileIoStatus::kOk, 0, size); }
    return true;
}

// 共有モード → CreateFileW の dwShareMode（原 CFile の share フラグに対応）。
static DWORD ToShareFlags(FileShareMode share) {
    switch (share) {
    case FileShareMode::kDenyWrite:  return FILE_SHARE_READ;   // 原 shareDenyWrite
    case FileShareMode::kExclusive:  return 0;                 // 原 shareExclusive
    case FileShareMode::kDenyNone:
    default:                         return FILE_SHARE_READ | FILE_SHARE_WRITE;
    }
}

FileIoResult LoadFileIntoBlocks(BlockList& list, const wchar_t* path,
                                FileShareMode share, void** outKeepHandle) {
    list.Clear();   // 契約上は空だが、途中失敗時と同じ状態から始めるため防衛的にクリアする
    if (outKeepHandle != nullptr) { *outKeepHandle = nullptr; }
    if (path == nullptr || *path == L'\0') {
        return MakeResult(FileIoStatus::kOpenFailed, ERROR_INVALID_NAME, 0);
    }
    // 読取専用で開く。書き込みは SaveBlocksToFile が別ハンドルで行うため要求しない。
    //   共有モードは呼出側（環境設定「ファイルの排他制御」）が決める。既定の kDenyNone は
    //   原 fopen("rb") = _SH_DENYNO 相当（Issue #120）。
    ScopedHandle h(::CreateFileW(path, GENERIC_READ, ToShareFlags(share),
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!h.Valid()) {
        return MakeResult(FileIoStatus::kOpenFailed, ::GetLastError(), 0);
    }

    // ファイルサイズを 64bit で取得（原は CMirrorFile::GetLength 結果を doc+0x84 へ）。
    LARGE_INTEGER li = {};
    if (!::GetFileSizeEx(h.Get(), &li)) {
        return MakeResult(FileIoStatus::kSizeFailed, ::GetLastError(), 0);
    }
    const FileOffset fileSize = static_cast<FileOffset>(li.QuadPart);

    if (fileSize == 0) {
        // 空ファイル: 空の 16KB ブロック 1 個（原挙動）。
        unsigned char* buf = new (std::nothrow) unsigned char[kBlockCapacity];
        if (buf == nullptr) {
            return MakeResult(FileIoStatus::kOutOfMemory, 0, 0);
        }
        list.AppendBlock(buf, kBlockCapacity, 0);
        if (outKeepHandle != nullptr) { *outKeepHandle = h.Release(); }
        return MakeResult(FileIoStatus::kOk, 0, 0);
    }

    // 1.6MB チャンクで読み、16KB ブロックへ分割して順次 Append（原 ReadFileIntoBlocks）。
    std::vector<unsigned char> big;
    try {
        big.resize(static_cast<size_t>(kReadChunk));
    } catch (const std::bad_alloc&) {
        return MakeResult(FileIoStatus::kOutOfMemory, 0, fileSize);
    }

    FileOffset remaining = fileSize;
    while (remaining > 0) {
        const DWORD want = (remaining < kReadChunk) ? static_cast<DWORD>(remaining)
                                                    : static_cast<DWORD>(kReadChunk);
        // ReadFile は要求より短く返り得る。チャンク境界を 16KB の倍数に保つため、
        // want バイト埋まるまで読み継ぐ（埋まらなければ EOF＝他プロセスによる短縮）。
        DWORD filled = 0;
        while (filled < want) {
            DWORD got = 0;
            if (!::ReadFile(h.Get(), big.data() + filled, want - filled, &got, nullptr)) {
                const DWORD err = ::GetLastError();
                list.Clear();   // 中途半端なブロック列を残さない
                return MakeResult(FileIoStatus::kReadFailed, err, fileSize);
            }
            if (got == 0) { break; }   // EOF
            filled += got;
        }

        // 読めた分を 16KB ブロックへ分割（最終チャンクの末尾のみ端数ブロックになる）。
        const unsigned char* p = big.data();
        DWORD left = filled;
        while (left > 0) {
            const int n = (left < static_cast<DWORD>(kBlockCapacity))
                              ? static_cast<int>(left) : kBlockCapacity;
            unsigned char* blk = new (std::nothrow) unsigned char[kBlockCapacity];
            if (blk == nullptr) {
                list.Clear();
                return MakeResult(FileIoStatus::kOutOfMemory, 0, fileSize);
            }
            std::memcpy(blk, p, static_cast<size_t>(n));   // 端数は usedLen で境界を持つ
            list.AppendBlock(blk, kBlockCapacity, n);
            p += n;
            left -= static_cast<DWORD>(n);
        }

        remaining -= filled;
        if (filled < want) { break; }   // EOF に到達（サイズ取得後に縮んだ場合）
    }

    // サイズ取得後に他プロセスがファイルを 0 バイトへ切り詰めた場合、ブロックが
    // 1 個も積まれないことがある。ドキュメントは「常に 1 個以上のブロックを持つ」
    //（空なら空の 16KB ブロック 1 個）が不変条件のため、ここで補う。
    if (list.IsEmpty()) {
        unsigned char* buf = new (std::nothrow) unsigned char[kBlockCapacity];
        if (buf == nullptr) {
            return MakeResult(FileIoStatus::kOutOfMemory, 0, fileSize);
        }
        list.AppendBlock(buf, kBlockCapacity, 0);
    }
    // 実際に読み込めたバイト数を返す（縮小されていた場合はサイズより少ない）。
    // 排他制御が有効なら、共有モードを保ったままハンドルを呼出側へ渡す（Issue #120）。
    if (outKeepHandle != nullptr) { *outKeepHandle = h.Release(); }
    return MakeResult(FileIoStatus::kOk, 0, fileSize - remaining);
}

FileIoResult SaveBlocksToFile(const BlockList& list, const wchar_t* path) {
    if (path == nullptr || *path == L'\0') {
        return MakeResult(FileIoStatus::kOpenFailed, ERROR_INVALID_NAME, 0);
    }
    ScopedHandle h(::CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!h.Valid()) {
        return MakeResult(FileIoStatus::kOpenFailed, ::GetLastError(), 0);
    }

    FileOffset written = 0;   // 総書込量は 2GB を超え得るため FileOffset
    for (BlockNode* n = list.GetHead(); n != nullptr; n = list.GetNext(n)) {
        const unsigned char* p = n->data;
        int left = n->usedLen;
        while (left > 0) {
            DWORD wrote = 0;
            if (!::WriteFile(h.Get(), p, static_cast<DWORD>(left), &wrote, nullptr)) {
                return MakeResult(FileIoStatus::kWriteFailed, ::GetLastError(), written);
            }
            if (wrote == 0) {   // ディスク不足等で進まない
                return MakeResult(FileIoStatus::kWriteFailed, ERROR_WRITE_FAULT, written);
            }
            p += wrote;
            left -= static_cast<int>(wrote);
            written += wrote;
        }
    }

    // 原は CMirrorFile::Close(temp→置換)。ここでは通常 close だが、
    // 遅延書込エラーを取りこぼさないよう戻り値を確認する。
    if (!h.Close()) {
        return MakeResult(FileIoStatus::kWriteFailed, ::GetLastError(), written);
    }
    return MakeResult(FileIoStatus::kOk, 0, written);
}

FileOffset RecalcTotalLength(const BlockList& list) {
    return list.GetTotalLength();
}

}  // namespace stirling
