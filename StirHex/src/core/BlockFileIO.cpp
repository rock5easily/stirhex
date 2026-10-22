// BlockFileIO 実装。原 CStirlingDoc の Load/Save 中核を MFC 非依存で移植。
//
// x64 化(Issue #20): 原(および移植初期)の fopen/fseek/ftell は long(32bit) オフセットで
// 2GB 超のファイルを扱えないため、CreateFileW + GetFileSizeEx + ReadFile/WriteFile へ
// 置き換えた。ブロック分割の手順（1.6MB チャンク読み→16KB ブロック列）は原と同一。
#include "BlockFileIO.h"

#include "FileSource.h"         // 遅延読み込みの読み出し元（Issue #300）
#include "StreamFileWriter.h"   // 保存は temp→置換で行う（Issue #170）
#include "Win32FileHooks.h"     // I/O 故障注入のための薄いラッパ（Issue #180）
#include "util/ScopedHandle.h"

#include <windows.h>

#include <cstring>
#include <memory>
#include <new>
#include <vector>
#include "core/AllocFailureScope.h"   // 確保失敗を std::bad_alloc で受ける区間（Issue #306）

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
        unsigned char* buf = AllocBlockData();
        if (buf == nullptr) {
            return MakeResult(FileIoStatus::kOutOfMemory, 0, 0);
        }
        // ノード確保に失敗すると所有権は移らない（Issue #153）。バッファを解放して報告する。
        if (list.AppendBlock(buf, kBlockCapacity, 0) == nullptr) {
            delete[] buf;
            return MakeResult(FileIoStatus::kOutOfMemory, 0, 0);
        }
        if (outKeepHandle != nullptr) { *outKeepHandle = h.Release(); }
        return MakeResult(FileIoStatus::kOk, 0, 0);
    }

    // 1.6MB チャンクで読み、16KB ブロックへ分割して順次 Append（原 ReadFileIntoBlocks）。
    std::vector<unsigned char> big;
    try {
        const stirling::StdAllocScope allocScope;
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
            if (!io::Read(h.Get(), big.data() + filled, want - filled, &got)) {
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
            unsigned char* blk = AllocBlockData();
            if (blk == nullptr) {
                list.Clear();
                return MakeResult(FileIoStatus::kOutOfMemory, 0, fileSize);
            }
            std::memcpy(blk, p, static_cast<size_t>(n));   // 端数は usedLen で境界を持つ
            if (list.AppendBlock(blk, kBlockCapacity, n) == nullptr) {
                delete[] blk;   // 所有権は移っていない（Issue #153）
                list.Clear();
                return MakeResult(FileIoStatus::kOutOfMemory, 0, fileSize);
            }
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
        unsigned char* buf = AllocBlockData();
        if (buf == nullptr) {
            return MakeResult(FileIoStatus::kOutOfMemory, 0, fileSize);
        }
        if (list.AppendBlock(buf, kBlockCapacity, 0) == nullptr) {
            delete[] buf;   // 所有権は移っていない（Issue #153）
            return MakeResult(FileIoStatus::kOutOfMemory, 0, fileSize);
        }
    }
    // 実際に読み込めたバイト数を返す（縮小されていた場合はサイズより少ない）。
    // 排他制御が有効なら、共有モードを保ったままハンドルを呼出側へ渡す（Issue #120）。
    if (outKeepHandle != nullptr) { *outKeepHandle = h.Release(); }
    return MakeResult(FileIoStatus::kOk, 0, fileSize - remaining);
}

namespace {

// 遅延読み込みの本体（共有フラグ指定版）。保存後の開き直しは元の共有フラグを引き継ぐ。
FileIoResult LoadFileLazyWithFlags(BlockList& list, const wchar_t* path, DWORD shareFlags) {
    list.Clear();
    if (path == nullptr || *path == L'\0') {
        return MakeResult(FileIoStatus::kOpenFailed, ERROR_INVALID_NAME, 0);
    }
    ScopedHandle h(::CreateFileW(path, GENERIC_READ, shareFlags, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!h.Valid()) {
        return MakeResult(FileIoStatus::kOpenFailed, ::GetLastError(), 0);
    }
    LARGE_INTEGER li = {};
    if (!::GetFileSizeEx(h.Get(), &li)) {
        return MakeResult(FileIoStatus::kSizeFailed, ::GetLastError(), 0);
    }
    const FileOffset fileSize = static_cast<FileOffset>(li.QuadPart);

    std::shared_ptr<FileSource> source;
    try {
        const stirling::StdAllocScope allocScope;
        source = std::make_shared<FileSource>(h.Get(), std::wstring(path), fileSize,
                                              shareFlags, false);
    } catch (const std::bad_alloc&) {
        return MakeResult(FileIoStatus::kOutOfMemory, 0, fileSize);
    }
    h.Release();   // 所有権は FileSource へ移った

    if (fileSize == 0) {
        // 空ファイル: 全体読み込みと同じく空の 16KB ブロック 1 個（ロックは保持する）。
        unsigned char* buf = AllocBlockData();
        if (buf == nullptr) {
            return MakeResult(FileIoStatus::kOutOfMemory, 0, 0);
        }
        if (list.AppendBlock(buf, kBlockCapacity, 0) == nullptr) {
            delete[] buf;
            return MakeResult(FileIoStatus::kOutOfMemory, 0, 0);
        }
    } else {
        for (FileOffset at = 0; at < fileSize; at += kMaxFileRefLen) {
            const FileOffset rest = fileSize - at;
            const int len = (rest < kMaxFileRefLen) ? static_cast<int>(rest) : kMaxFileRefLen;
            if (list.AppendFileRef(at, len) == nullptr) {
                list.Clear();
                return MakeResult(FileIoStatus::kOutOfMemory, 0, fileSize);
            }
        }
    }
    list.AttachSource(std::move(source));
    return MakeResult(FileIoStatus::kOk, 0, fileSize);
}

// 保存で参照ノードを読み出す際のバッファ長（1MB）。
constexpr int kSaveChunk = 0x100000;

// list の内容を先頭から writer へ書き出す。参照ノードは元ファイルから kSaveChunk 単位で
//   読み出す（Issue #300）。失敗したら writer はそのまま（呼出側が Abort する）。
FileIoResult WriteListTo(StreamFileWriter& writer, const BlockList& list) {
    std::vector<unsigned char> buf;
    for (BlockNode* n = list.GetHead(); n != nullptr; n = list.GetNext(n)) {
        if (n->usedLen <= 0) { continue; }
        if (n->data != nullptr) {
            const FileIoResult wrote = writer.Write(n->data, static_cast<size_t>(n->usedLen));
            if (!wrote.Ok()) { return wrote; }
            continue;
        }
        if (!BlockList::IsFileRef(n) || list.Source() == nullptr) {
            return MakeResult(FileIoStatus::kReadFailed, ERROR_INVALID_DATA, writer.Written());
        }
        if (buf.empty()) {
            try {
                const stirling::StdAllocScope allocScope;
                buf.resize(static_cast<size_t>(kSaveChunk));
            } catch (const std::bad_alloc&) {
                return MakeResult(FileIoStatus::kOutOfMemory, 0, writer.Written());
            }
        }
        for (int off = 0; off < n->usedLen; off += kSaveChunk) {
            const int len = (n->usedLen - off < kSaveChunk) ? (n->usedLen - off) : kSaveChunk;
            if (!list.ReadNode(n, off, len, buf.data())) {
                return MakeResult(FileIoStatus::kReadFailed, list.Source()->LastError(),
                                  writer.Written());
            }
            const FileIoResult wrote = writer.Write(buf.data(), static_cast<size_t>(len));
            if (!wrote.Ok()) { return wrote; }
        }
    }
    return MakeResult(FileIoStatus::kOk, 0, writer.Written());
}

// handle が開いているファイルと path が同じファイルか（ボリュームとファイル ID で比べる。
//   別名・短い名前・大文字小文字の違いでも同一と判定するため）。path が無ければ false。
bool IsSameFile(void* handle, const wchar_t* path) {
    BY_HANDLE_FILE_INFORMATION a = {};
    if (handle == nullptr || !::GetFileInformationByHandle(static_cast<HANDLE>(handle), &a)) {
        return false;
    }
    // 属性の読み取りだけを要求する（共有モードの衝突を起こさない）。
    ScopedHandle h(::CreateFileW(path, FILE_READ_ATTRIBUTES,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!h.Valid()) { return false; }
    BY_HANDLE_FILE_INFORMATION b = {};
    if (!::GetFileInformationByHandle(h.Get(), &b)) { return false; }
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
           a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
}

// 元ファイルへその場で書き戻せる構造か（Issue #300）。
//   全ノードについて「元ファイル上の位置 == 文書上の位置」が成り立ち、総長が元ファイルと
//   同じなら、参照ノードは元ファイルの同じ位置そのもの、実体化ブロックは同じ位置へ書けば
//   よい。挿入・削除で位置がずれたノードや、挿入で作ったブロック（srcOffset<0）があれば
//   一時ファイル方式に任せる。
bool CanSaveInPlace(const BlockList& list) {
    const FileSource* src = list.Source();
    if (src == nullptr || list.GetTotalLength() != src->Size()) { return false; }
    FileOffset abs = 0;
    for (const BlockNode* n = list.GetHead(); n != nullptr; n = list.GetNext(n)) {
        if (n->usedLen > 0 && n->srcOffset != abs) { return false; }
        abs += n->usedLen;
    }
    return true;
}

// 上書き保存の本体: 実体化ブロックだけを元ファイルの同じ位置へ書き、フラッシュする。
FileIoResult WriteBlocksInPlace(BlockList& list) {
    FileSource* src = list.Source();
    FileOffset abs = 0;
    for (const BlockNode* n = list.GetHead(); n != nullptr; n = list.GetNext(n)) {
        if (n->data != nullptr && n->usedLen > 0) {
            if (!src->Write(abs, n->data, static_cast<size_t>(n->usedLen))) {
                FileIoResult r = MakeResult(FileIoStatus::kWriteFailed, src->LastError(), abs);
                r.partialInPlace = true;
                // それまでの書込で最終更新日時は変わっている。控え直しておかないと、続く
                //   Reopen が自分の書込を外部の変更と見なして読み出し元を失う。書いた区間は
                //   文書の内容と同じなので、参照ノードはそのまま読み続けられる（失敗は無視）。
                src->StampAfterWrite();
                return r;
            }
        }
        abs += n->usedLen;
    }
    // 書込後の最終更新日時を確定させて控え直してから（Reopen の同一性照合のため）フラッシュする。
    if (!src->StampAfterWrite() || !src->Flush()) {
        FileIoResult r = MakeResult(FileIoStatus::kWriteFailed, src->LastError(), abs);
        r.partialInPlace = true;
        return r;
    }
    return MakeResult(FileIoStatus::kOk, 0, abs);
}

// 上書き保存（Issue #300）。書込権付きで開き直して書き、読み取り専用へ戻す。
//   書込権付きで開けない（属性・権限・他プロセスの共有）ときは *outTried=false で戻り、
//   呼出側が一時ファイル方式へ切り替える。
FileIoResult SaveInPlace(BlockList& list, bool* outTried) {
    FileSource* src = list.Source();
    *outTried = false;
    if (!src->Reopen(true)) {
        const DWORD err = src->LastError();
        if (!src->Reopen(false)) {
            // 何も書いていないが、読み出し元を開き直せなかった（閉じている間に他プロセスが
            //   掴んだ・置き換えた）。文書の参照ノードはもう読めない。
            *outTried = true;
            FileIoResult r = MakeResult(FileIoStatus::kOpenFailed, src->LastError(), 0);
            r.sourceLost = true;
            return r;
        }
        return MakeResult(FileIoStatus::kOpenFailed, err, 0);
    }
    *outTried = true;
    FileIoResult r = WriteBlocksInPlace(list);
    // 書いた範囲のキャッシュは古いので捨て、読み取り専用で開き直す（書込権付きのハンドルは
    //   他プロセスの読み取りとも競合しうるため）。読み取り専用で開き直せなければ書込権付きで
    //   試す。どちらも駄目なら読み出し元を失っている。
    if (!src->Reopen(false) && !src->Reopen(true)) {
        if (r.Ok()) {
            // 保存は済んでいる（保存済みとして扱い、文書の開き直しを促す）。
            r = MakeResult(FileIoStatus::kReopenFailed, src->LastError(), r.fileSize);
        } else {
            r.sourceLost = true;
        }
    }
    return r;
}

}  // namespace

FileIoResult LoadFileLazy(BlockList& list, const wchar_t* path, FileShareMode share) {
    // 参照ノードが指す内容を他プロセスに書き換えられないよう、呼出側は通常 kDenyWrite
    //   以上を指定する（kDenyNone は閲覧モード用）。
    return LoadFileLazyWithFlags(list, path, ToShareFlags(share));
}

FileIoResult SaveBlocksToFile(BlockList& list, const wchar_t* path) {
    FileSource* src = list.Source();
    const bool sameFile = (src != nullptr && src->IsOpen() && IsSameFile(src->Handle(), path));

    // 遅延読み込みで元ファイルへ上書き保存する場合（Issue #300）: 長さも位置も変わって
    //   いなければ、実体化したブロックだけを書き戻す（全体を書き直さない）。
    if (sameFile && CanSaveInPlace(list)) {
        bool tried = false;
        const FileIoResult r = SaveInPlace(list, &tried);
        if (tried || !src->IsOpen()) { return r; }
        // 書込権付きで開けなかった。一時ファイル方式で試す（失敗理由はそちらで判明する）。
    }

    // 原 CMirrorFile と同じ temp→置換の安全保存（Issue #170）。以前は出力先を
    // CREATE_ALWAYS で直接開いていたため、書込・フラッシュに失敗すると出力先が中途状態で
    // 残った。StreamFileWriter を介すことで、Commit に到達しない限り出力先は変わらない。
    StreamFileWriter writer;
    // 元ファイル自身へ保存する場合は、自分の書込禁止ハンドルが置換可否の確認を阻むため
    //   確認を省く（置換の前にハンドルを閉じる）。
    const FileIoResult opened = writer.Open(path, !sameFile);
    if (!opened.Ok()) { return opened; }

    const FileIoResult wrote = WriteListTo(writer, list);
    if (!wrote.Ok()) {
        writer.Abort();   // 一時ファイルを破棄（出力先は元のまま）
        return wrote;
    }
    if (src == nullptr) {
        return writer.Commit();   // 明示フラッシュ → 置換
    }

    // ---- 遅延読み込み（Issue #300） ----
    // 書き終えたので元ファイルはもう読まない。元ファイル自身へ置換するなら先に閉じる。
    const unsigned long shareFlags = src->ShareFlags();
    if (sameFile) { src->Close(); }
    const FileIoResult committed = writer.Commit();
    if (!committed.Ok()) {
        FileIoResult r = committed;
        if (sameFile && !src->Reopen(false)) {
            // 元ファイルが置換の途中で消えた場合、内容は残された一時ファイルにある。
            //   それを読み出し元にし直す（内容は文書と同じ）。
            bool recovered = false;
            if (!committed.keptTempPath.empty()) {
                BlockList reloaded;
                if (LoadFileLazyWithFlags(reloaded, committed.keptTempPath.c_str(),
                                          shareFlags).Ok()) {
                    list.TakeFrom(reloaded);
                    recovered = true;
                }
            }
            r.sourceLost = !recovered;
        }
        return r;
    }
    // 保存先を読み出し元として開き直し、参照ノードだけの構造へ戻す（保存先の内容は文書と
    //   同じ）。別名保存では元ファイルのロックもここで外れる。
    BlockList reloaded;
    const FileIoResult reopened = LoadFileLazyWithFlags(reloaded, path, shareFlags);
    if (reopened.Ok()) {
        list.TakeFrom(reloaded);
    } else if (sameFile) {
        // 元ファイルは置き換わっており、残っている参照ノードは読めない。保存自体は済んで
        //   いるが、文書を読み続けられないことを呼出側へ伝える。
        FileIoResult r = reopened;
        r.status = FileIoStatus::kReopenFailed;
        return r;
    } else {
        // 別名保存: 保存前のファイルは変わっていないので読み続けられる。ただしロックは
        //   保存先へ移せていないことを呼出側へ伝える。
        FileIoResult r = committed;
        r.sourceNotSwitched = true;
        r.systemError = reopened.systemError;
        return r;
    }
    return committed;
}

FileIoResult CopyFileForBackup(const BlockList& list, const wchar_t* src, const wchar_t* dst) {
    if (::CopyFileW(src, dst, FALSE)) {
        return MakeResult(FileIoStatus::kOk, 0, 0);
    }
    const DWORD err = ::GetLastError();
    FileSource* source = list.Source();
    const bool sharing = (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION);
    if (!sharing || source == nullptr || !source->IsOpen() || !IsSameFile(source->Handle(), src)) {
        return MakeResult(FileIoStatus::kOpenFailed, err, 0);
    }
    StreamFileWriter writer;
    const FileIoResult opened = writer.Open(dst);
    if (!opened.Ok()) { return opened; }
    std::vector<unsigned char> buf;
    try {
        const stirling::StdAllocScope allocScope;
        buf.resize(static_cast<size_t>(kSaveChunk));
    } catch (const std::bad_alloc&) {
        return MakeResult(FileIoStatus::kOutOfMemory, 0, 0);
    }
    for (FileOffset at = 0; at < source->Size(); at += kSaveChunk) {
        const FileOffset rest = source->Size() - at;
        const size_t len = (rest < kSaveChunk) ? static_cast<size_t>(rest)
                                               : static_cast<size_t>(kSaveChunk);
        if (!source->Read(at, buf.data(), len)) {
            writer.Abort();
            return MakeResult(FileIoStatus::kReadFailed, source->LastError(), at);
        }
        const FileIoResult wrote = writer.Write(buf.data(), len);
        if (!wrote.Ok()) {
            writer.Abort();
            return wrote;
        }
    }
    return writer.Commit();
}

FileOffset RecalcTotalLength(const BlockList& list) {
    return list.GetTotalLength();
}

}  // namespace stirling
