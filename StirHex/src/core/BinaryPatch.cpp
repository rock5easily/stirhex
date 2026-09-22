#include "BinaryPatch.h"

#include "StreamFileWriter.h"
#include "Win32FileHooks.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace stirling {
namespace {

constexpr DWORD kFileShareRead = FILE_SHARE_READ;
constexpr size_t kIoChunk = 1024u * 1024u;
constexpr size_t kMatchChunk = 64u * 1024u;
constexpr size_t kMatchProbe = 16u;
constexpr FileOffset kLiteralSearchStride = 16;
constexpr std::uint32_t kCrcInit = 0xffffffffu;

BinaryPatchResult Result(BinaryPatchStatus status, DWORD error = 0,
                         FileOffset processed = 0) {
    BinaryPatchResult result;
    result.status = status;
    result.systemError = error;
    result.bytesProcessed = processed;
    return result;
}

bool AddWouldOverflow(FileOffset a, FileOffset b, FileOffset* out) {
    if (a < 0 || b < 0 || a > (std::numeric_limits<FileOffset>::max)() - b) {
        return true;
    }
    if (out != nullptr) { *out = a + b; }
    return false;
}

bool MultiplyWouldOverflow(FileOffset value, FileOffset multiplier,
                           FileOffset* out) {
    if (value < 0 || multiplier < 0 ||
        (value != 0 && multiplier > (std::numeric_limits<FileOffset>::max)() / value)) {
        return true;
    }
    if (out != nullptr) { *out = value * multiplier; }
    return false;
}

FileOffset SaturatingCeilDiv(FileOffset value, FileOffset divisor) {
    if (value <= 0) { return 0; }
    return value / divisor + ((value % divisor) != 0 ? 1 : 0);
}

FileOffset EstimatedPatchBytes(FileOffset targetSize, BinaryPatchFormat format) {
    // Conservative upper bounds for the streaming generators: IPS emits at
    // most one 16-bit record per run; BPS literals are capped at kIoChunk and
    // each instruction/offset uses no more than ten bytes.
    FileOffset estimate = targetSize;
    const FileOffset records = format == BinaryPatchFormat::kIps
        ? SaturatingCeilDiv(targetSize, 0xffff)
        : SaturatingCeilDiv(targetSize, static_cast<FileOffset>(kIoChunk));
    FileOffset overhead = 0;
    if (MultiplyWouldOverflow(records, format == BinaryPatchFormat::kIps ? 5 : 20,
                              &overhead) || AddWouldOverflow(estimate, overhead, &estimate) ||
        AddWouldOverflow(estimate, format == BinaryPatchFormat::kIps ? 8 : 64, &estimate)) {
        return (std::numeric_limits<FileOffset>::max)();
    }
    return estimate;
}

bool GenerationFitsTemporaryBudget(FileOffset targetSize,
                                   BinaryPatchFormat format,
                                   const BinaryPatchOptions& options) {
    if (options.limits.maxTemporaryBytes <= 0) { return false; }
    if (format == BinaryPatchFormat::kBps &&
        targetSize > options.limits.bpsGenerateMaxTargetBytes) {
        return false;
    }
    const FileOffset patchEstimate = EstimatedPatchBytes(targetSize, format);
    FileOffset twoTarget = 0;
    FileOffset twoPatch = 0;
    if (MultiplyWouldOverflow(targetSize, 2, &twoTarget) ||
        MultiplyWouldOverflow(patchEstimate, 2, &twoPatch)) {
        return false;
    }
    FileOffset applyPeak = 0;
    if (AddWouldOverflow(patchEstimate, twoTarget, &applyPeak)) { return false; }
    const FileOffset peak = (std::max)(applyPeak, twoPatch);
    return peak <= options.limits.maxTemporaryBytes;
}

bool Report(const BinaryPatchOptions& options, FileOffset processed,
            FileOffset total) {
    return !options.progress || options.progress(processed, total);
}

struct FileIdentity {
    DWORD volume = 0;
    DWORD indexHigh = 0;
    DWORD indexLow = 0;

    bool operator==(const FileIdentity& other) const {
        return volume == other.volume && indexHigh == other.indexHigh &&
               indexLow == other.indexLow;
    }
};

class DiskFile {
public:
    DiskFile() = default;
    ~DiskFile() { Close(); }
    DiskFile(const DiskFile&) = delete;
    DiskFile& operator=(const DiskFile&) = delete;

    bool Open(const wchar_t* path) {
        Close();
        if (path == nullptr || *path == L'\0') {
            error_ = ERROR_INVALID_NAME;
            return false;
        }
        handle_ = ::CreateFileW(path, GENERIC_READ, kFileShareRead, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL |
                                    FILE_FLAG_RANDOM_ACCESS, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            handle_ = nullptr;
            error_ = ::GetLastError();
            return false;
        }
        LARGE_INTEGER size = {};
        if (!::GetFileSizeEx(handle_, &size) || size.QuadPart < 0) {
            error_ = ::GetLastError();
            Close();
            return false;
        }
        BY_HANDLE_FILE_INFORMATION info = {};
        if (!::GetFileInformationByHandle(handle_, &info)) {
            error_ = ::GetLastError();
            Close();
            return false;
        }
        path_ = path;
        size_ = static_cast<FileOffset>(size.QuadPart);
        identity_.volume = info.dwVolumeSerialNumber;
        identity_.indexHigh = info.nFileIndexHigh;
        identity_.indexLow = info.nFileIndexLow;
        error_ = ERROR_SUCCESS;
        return true;
    }

    void Close() {
        if (handle_ != nullptr) { ::CloseHandle(handle_); }
        handle_ = nullptr;
        path_.clear();
        size_ = 0;
    }

    bool Valid() const { return handle_ != nullptr; }
    FileOffset Size() const { return size_; }
    DWORD Error() const { return error_; }
    const std::wstring& Path() const { return path_; }
    const FileIdentity& Identity() const { return identity_; }

    bool ReadAt(FileOffset offset, void* destination, size_t count) const {
        if (count == 0) { return true; }
        if (destination == nullptr || offset < 0 ||
            offset > size_ || static_cast<FileOffset>(count) > size_ - offset) {
            error_ = ERROR_INVALID_PARAMETER;
            return false;
        }
        unsigned char* out = static_cast<unsigned char*>(destination);
        size_t left = count;
        FileOffset position = offset;
        while (left > 0) {
            const DWORD want = static_cast<DWORD>((std::min)(left,
                                                              static_cast<size_t>(0x40000000u)));
            LARGE_INTEGER li = {};
            li.QuadPart = position;
            if (!::SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN)) {
                error_ = ::GetLastError();
                return false;
            }
            DWORD got = 0;
            if (!io::Read(handle_, out, want, &got)) {
                error_ = ::GetLastError();
                return false;
            }
            if (got == 0) {
                error_ = ERROR_HANDLE_EOF;
                return false;
            }
            out += got;
            left -= got;
            position += static_cast<FileOffset>(got);
        }
        return true;
    }

private:
    HANDLE handle_ = nullptr;
    std::wstring path_;
    FileOffset size_ = 0;
    FileIdentity identity_;
    mutable DWORD error_ = ERROR_SUCCESS;
};

class CachedReader {
public:
    explicit CachedReader(const DiskFile& file, size_t capacity = kIoChunk)
        : file_(file), cache_((std::max)(capacity, static_cast<size_t>(4096))) {}

    bool ReadAt(FileOffset offset, void* out, size_t count) {
        if (count == 0) { return true; }
        if (count > cache_.size()) { return file_.ReadAt(offset, out, count); }
        FileOffset cacheEnd = 0;
        if (cacheLength_ > 0 && !AddWouldOverflow(cacheOffset_,
                                                   static_cast<FileOffset>(cacheLength_),
                                                   &cacheEnd) &&
            offset >= cacheOffset_ &&
            static_cast<FileOffset>(count) <= cacheEnd - offset) {
            std::memcpy(out, cache_.data() + static_cast<size_t>(offset - cacheOffset_), count);
            return true;
        }
        const size_t toRead = (std::min)(cache_.size(),
            static_cast<size_t>(file_.Size() - offset));
        if (!file_.ReadAt(offset, cache_.data(), toRead)) { return false; }
        cacheOffset_ = offset;
        cacheLength_ = toRead;
        std::memcpy(out, cache_.data(), count);
        return true;
    }

    bool ByteAt(FileOffset offset, unsigned char& out) {
        return ReadAt(offset, &out, 1);
    }

    DWORD Error() const { return file_.Error(); }

private:
    const DiskFile& file_;
    std::vector<unsigned char> cache_;
    FileOffset cacheOffset_ = 0;
    size_t cacheLength_ = 0;
};

class Crc32 {
public:
    Crc32() : value_(kCrcInit) {}

    void Add(const void* data, size_t size) {
        const unsigned char* p = static_cast<const unsigned char*>(data);
        const auto& table = Table();
        for (size_t i = 0; i < size; ++i) {
            value_ = (value_ >> 8) ^ table[(value_ ^ p[i]) & 0xffu];
        }
    }

    std::uint32_t Final() const { return value_ ^ 0xffffffffu; }

private:
    static const std::array<std::uint32_t, 256>& Table() {
        static const std::array<std::uint32_t, 256> table = [] {
            std::array<std::uint32_t, 256> result{};
            for (unsigned i = 0; i < result.size(); ++i) {
                std::uint32_t value = i;
                for (int bit = 0; bit < 8; ++bit) {
                    value = (value >> 1) ^ ((value & 1u) ? 0xedb88320u : 0u);
                }
                result[i] = value;
            }
            return result;
        }();
        return table;
    }

    std::uint32_t value_;
};

// Returns kOk, kCancelled, kOutOfMemory, or kReadFailed carrying file's error.
BinaryPatchResult ComputeCrc(const DiskFile& file, const BinaryPatchOptions& options,
                             std::uint32_t& out) {
    Crc32 crc;
    std::vector<unsigned char> buffer;
    try { buffer.resize(kIoChunk); }
    catch (const std::bad_alloc&) { return Result(BinaryPatchStatus::kOutOfMemory); }
    for (FileOffset offset = 0; offset < file.Size();) {
        const size_t count = static_cast<size_t>((std::min)(
            static_cast<FileOffset>(buffer.size()), file.Size() - offset));
        if (!file.ReadAt(offset, buffer.data(), count)) {
            return Result(BinaryPatchStatus::kReadFailed, file.Error());
        }
        crc.Add(buffer.data(), count);
        offset += static_cast<FileOffset>(count);
        if (!Report(options, offset, file.Size())) {
            return Result(BinaryPatchStatus::kCancelled);
        }
    }
    out = crc.Final();
    return Result(BinaryPatchStatus::kOk);
}

bool PathExists(const wchar_t* path) {
    return path != nullptr && *path != L'\0' &&
           ::GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

std::wstring DirectoryOf(const std::wstring& path) {
    const size_t sep = path.find_last_of(L"\\/");
    if (sep == std::wstring::npos) { return L"."; }
    if (sep == 0) { return path.substr(0, 1); }
    return path.substr(0, sep);
}

bool MakeTempPath(const std::wstring& target, const wchar_t* prefix,
                  std::wstring& out, DWORD& error) {
    wchar_t buffer[MAX_PATH] = {};
    const std::wstring directory = DirectoryOf(target);
    if (::GetTempFileNameW(directory.c_str(), prefix, 0, buffer) == 0) {
        error = ::GetLastError();
        return false;
    }
    out = buffer;
    if (!::DeleteFileW(out.c_str())) {
        error = ::GetLastError();
        out.clear();
        return false;
    }
    error = ERROR_SUCCESS;
    return true;
}

bool SameExistingFile(const wchar_t* path, const FileIdentity& identity) {
    DiskFile probe;
    return probe.Open(path) && probe.Identity() == identity;
}

bool CheckOutputConflicts(const DiskFile& source, const DiskFile* other,
                          const wchar_t* output, DWORD& error) {
    if (output == nullptr || *output == L'\0') {
        error = ERROR_INVALID_NAME;
        return false;
    }
    if (SameExistingFile(output, source.Identity()) ||
        (other != nullptr && SameExistingFile(output, other->Identity()))) {
        error = ERROR_SHARING_VIOLATION;
        return false;
    }
    return true;
}

class TempOutput {
public:
    TempOutput() = default;
    ~TempOutput() { Abort(); }
    TempOutput(const TempOutput&) = delete;
    TempOutput& operator=(const TempOutput&) = delete;

    bool Open(const std::wstring& target, DWORD& error) {
        Abort();
        if (!MakeTempPath(target, L"STP", path_, error)) { return false; }
        handle_ = ::CreateFileW(path_.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            error = ::GetLastError();
            handle_ = nullptr;
            ::DeleteFileW(path_.c_str());
            path_.clear();
            return false;
        }
        written_ = 0;
        return true;
    }

    bool Append(const void* data, size_t size, DWORD& error) {
        if (handle_ == nullptr || (size > 0 && data == nullptr)) {
            error = ERROR_INVALID_HANDLE;
            return false;
        }
        const unsigned char* p = static_cast<const unsigned char*>(data);
        size_t left = size;
        while (left > 0) {
            const DWORD want = static_cast<DWORD>((std::min)(left,
                                                              static_cast<size_t>(0x40000000u)));
            LARGE_INTEGER li = {};
            li.QuadPart = written_;
            if (!::SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN)) {
                error = ::GetLastError();
                return false;
            }
            DWORD wrote = 0;
            if (!io::Write(handle_, p, want, &wrote) || wrote == 0) {
                error = ::GetLastError();
                if (error == ERROR_SUCCESS) { error = ERROR_WRITE_FAULT; }
                return false;
            }
            p += wrote;
            left -= wrote;
            written_ += static_cast<FileOffset>(wrote);
        }
        return true;
    }

    bool ReadAt(FileOffset offset, void* data, size_t size, DWORD& error) const {
        if (handle_ == nullptr || offset < 0 ||
            static_cast<FileOffset>(size) > written_ - offset) {
            error = ERROR_INVALID_PARAMETER;
            return false;
        }
        LARGE_INTEGER li = {};
        li.QuadPart = offset;
        if (!::SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN)) {
            error = ::GetLastError();
            return false;
        }
        unsigned char* p = static_cast<unsigned char*>(data);
        size_t left = size;
        while (left > 0) {
            const DWORD want = static_cast<DWORD>((std::min)(left,
                                                              static_cast<size_t>(0x40000000u)));
            DWORD got = 0;
            if (!io::Read(handle_, p, want, &got) || got == 0) {
                error = ::GetLastError();
                if (error == ERROR_SUCCESS) { error = ERROR_HANDLE_EOF; }
                return false;
            }
            p += got;
            left -= got;
        }
        return true;
    }

    bool Close(DWORD& error) {
        if (handle_ == nullptr) { return true; }
        if (!io::Flush(handle_)) {
            error = ::GetLastError();
            ::CloseHandle(handle_);
            handle_ = nullptr;
            return false;
        }
        if (!::CloseHandle(handle_)) {
            error = ::GetLastError();
            handle_ = nullptr;
            return false;
        }
        handle_ = nullptr;
        ::SetFileAttributesW(path_.c_str(), FILE_ATTRIBUTE_NORMAL);
        return true;
    }

    void Abort() {
        if (handle_ != nullptr) { ::CloseHandle(handle_); }
        handle_ = nullptr;
        if (!path_.empty()) { ::DeleteFileW(path_.c_str()); }
        path_.clear();
        written_ = 0;
    }

    const std::wstring& Path() const { return path_; }
    FileOffset Written() const { return written_; }

private:
    HANDLE handle_ = nullptr;
    std::wstring path_;
    FileOffset written_ = 0;
};

bool PublishTemp(const std::wstring& temp, const wchar_t* target,
                 const BinaryPatchOptions& options, BinaryPatchResult& result) {
    DiskFile source;
    if (!source.Open(temp.c_str())) {
        result = Result(BinaryPatchStatus::kOpenFailed, source.Error());
        return false;
    }
    FileOffset publishPeak = 0;
    if (MultiplyWouldOverflow(source.Size(), 2, &publishPeak) ||
        publishPeak > options.limits.maxTemporaryBytes) {
        result = Result(BinaryPatchStatus::kLimitExceeded);
        return false;
    }
    DWORD error = 0;
    if (!CheckOutputConflicts(source, nullptr, target, error)) {
        result = Result(BinaryPatchStatus::kConflict, error);
        return false;
    }
    StreamFileWriter writer;
    FileIoResult opened = writer.Open(target);
    if (!opened.Ok()) {
        result = Result(BinaryPatchStatus::kOpenFailed, opened.systemError,
                        opened.fileSize);
        result.keptTempPath = opened.keptTempPath;
        return false;
    }
    std::vector<unsigned char> buffer;
    try { buffer.resize(kIoChunk); } catch (const std::bad_alloc&) {
        writer.Abort();
        result = Result(BinaryPatchStatus::kOutOfMemory);
        return false;
    }
    for (FileOffset offset = 0; offset < source.Size();) {
        const size_t count = static_cast<size_t>((std::min)(
            static_cast<FileOffset>(buffer.size()), source.Size() - offset));
        if (!source.ReadAt(offset, buffer.data(), count)) {
            writer.Abort();
            result = Result(BinaryPatchStatus::kReadFailed, source.Error(), offset);
            return false;
        }
        const FileIoResult copied = writer.Write(buffer.data(), count);
        if (!copied.Ok()) {
            writer.Abort();
            result = Result(BinaryPatchStatus::kWriteFailed, copied.systemError, offset);
            return false;
        }
        offset += static_cast<FileOffset>(count);
        if (!Report(options, offset, source.Size())) {
            writer.Abort();
            result = Result(BinaryPatchStatus::kCancelled, 0, offset);
            return false;
        }
    }
    const FileIoResult committed = writer.Commit();
    if (!committed.Ok()) {
        result = Result(BinaryPatchStatus::kWriteFailed, committed.systemError,
                        committed.fileSize);
        result.keptTempPath = committed.keptTempPath;
        return false;
    }
    const FileOffset publishedSize = source.Size();
    source.Close();
    ::DeleteFileW(temp.c_str());
    result = Result(BinaryPatchStatus::kOk, 0, publishedSize);
    result.targetSize = publishedSize;
    return true;
}

class PatchSink {
public:
    bool Open(const wchar_t* path, FileOffset maxBytes = (std::numeric_limits<FileOffset>::max)()) {
        maxBytes_ = maxBytes;
        written_ = 0;
        const FileIoResult result = writer_.Open(path);
        error_ = result.systemError;
        return result.Ok();
    }

    bool Write(const void* data, size_t size) {
        if (size > 0 && (written_ > maxBytes_ ||
                         static_cast<FileOffset>(size) > maxBytes_ - written_)) {
            error_ = ERROR_DISK_FULL;
            return false;
        }
        const FileIoResult result = writer_.Write(data, size);
        if (!result.Ok()) {
            error_ = result.systemError;
            return false;
        }
        patchCrc_.Add(data, size);
        written_ += static_cast<FileOffset>(size);
        return true;
    }

    bool Commit() {
        const FileIoResult result = writer_.Commit();
        error_ = result.systemError;
        return result.Ok();
    }

    void Abort() { writer_.Abort(); }
    DWORD Error() const { return error_; }
    std::uint32_t Crc() const { return patchCrc_.Final(); }
    std::uint32_t CrcWith(const void* data, size_t size) const {
        Crc32 crc = patchCrc_;
        crc.Add(data, size);
        return crc.Final();
    }

private:
    StreamFileWriter writer_;
    Crc32 patchCrc_;
    DWORD error_ = ERROR_SUCCESS;
    FileOffset maxBytes_ = (std::numeric_limits<FileOffset>::max)();
    FileOffset written_ = 0;
};

void PutBe16(unsigned char* p, std::uint32_t value) {
    p[0] = static_cast<unsigned char>((value >> 8) & 0xffu);
    p[1] = static_cast<unsigned char>(value & 0xffu);
}
void PutBe24(unsigned char* p, std::uint64_t value) {
    p[0] = static_cast<unsigned char>((value >> 16) & 0xffu);
    p[1] = static_cast<unsigned char>((value >> 8) & 0xffu);
    p[2] = static_cast<unsigned char>(value & 0xffu);
}
void PutLe32(unsigned char* p, std::uint32_t value) {
    p[0] = static_cast<unsigned char>(value & 0xffu);
    p[1] = static_cast<unsigned char>((value >> 8) & 0xffu);
    p[2] = static_cast<unsigned char>((value >> 16) & 0xffu);
    p[3] = static_cast<unsigned char>((value >> 24) & 0xffu);
}

std::uint32_t GetBe16(const unsigned char* p) {
    return (static_cast<std::uint32_t>(p[0]) << 8) | p[1];
}
std::uint32_t GetBe24(const unsigned char* p) {
    return (static_cast<std::uint32_t>(p[0]) << 16) |
           (static_cast<std::uint32_t>(p[1]) << 8) | p[2];
}
std::uint32_t GetLe32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

bool WriteIpsRecord(PatchSink& sink, FileOffset offset,
                    const unsigned char* data, size_t length) {
    if (offset < 0 || offset > 0xffffff || length == 0 || length > 0xffff) {
        return false;
    }
    unsigned char header[5] = {};
    PutBe24(header, static_cast<std::uint64_t>(offset));
    PutBe16(header + 3, static_cast<std::uint32_t>(length));
    return sink.Write(header, sizeof(header)) && sink.Write(data, length);
}

bool WriteIpsRleRecord(PatchSink& sink, FileOffset offset,
                       unsigned char value, size_t length) {
    if (offset < 0 || offset > 0xffffff || length == 0 || length > 0xffff) {
        return false;
    }
    unsigned char header[8] = {};
    PutBe24(header, static_cast<std::uint64_t>(offset));
    // A zero normal length selects IPS RLE, followed by the RLE length/value.
    PutBe16(header + 3, 0);
    PutBe16(header + 5, static_cast<std::uint32_t>(length));
    header[7] = value;
    return sink.Write(header, sizeof(header));
}

bool EmitIpsSegment(PatchSink& sink, const DiskFile& target,
                    FileOffset offset, const unsigned char* data,
                    size_t length, BinaryPatchStatus& failure,
                    DWORD& failureError) {
    failure = BinaryPatchStatus::kWriteFailed;
    failureError = ERROR_SUCCESS;
    if (length == 0) { return true; }
    std::vector<unsigned char> adjusted;
    const unsigned char* current = data;
    FileOffset currentOffset = offset;
    size_t currentLength = length;
    while (currentLength > 0) {
        // "EOF" is a record marker, so a normal/RLE record cannot begin at
        // the byte offset whose big-endian representation is 45 4f 46.
        // Match Flips by moving that segment one byte earlier and including
        // the preceding target byte.  This is checked on every split too.
        if (currentOffset == 0x454f46) {
            try { adjusted.resize(currentLength + 1); }
            catch (const std::bad_alloc&) {
                failure = BinaryPatchStatus::kOutOfMemory;
                failureError = ERROR_NOT_ENOUGH_MEMORY;
                return false;
            }
            if (!target.ReadAt(currentOffset - 1, adjusted.data(), 1)) {
                failure = BinaryPatchStatus::kReadFailed;
                failureError = target.Error();
                return false;
            }
            std::memcpy(adjusted.data() + 1, current, currentLength);
            current = adjusted.data();
            --currentOffset;
            ++currentLength;
        }
        const size_t count = (std::min)(currentLength, static_cast<size_t>(0xffff));
        bool repeated = count >= 4;
        for (size_t i = 1; repeated && i < count; ++i) {
            repeated = current[i] == current[0];
        }
        const bool ok = repeated
            ? WriteIpsRleRecord(sink, currentOffset, current[0], count)
            : WriteIpsRecord(sink, currentOffset, current, count);
        if (!ok) {
            failureError = sink.Error();
            failure = (failureError == ERROR_DISK_FULL)
                ? BinaryPatchStatus::kLimitExceeded
                : BinaryPatchStatus::kWriteFailed;
            return false;
        }
        current += count;
        currentLength -= count;
        currentOffset += static_cast<FileOffset>(count);
        // A split caused by the EOF collision cannot land on another marker
        // unless a later caller starts a new segment there; handle that case
        // on the next call without weakening the format check.
    }
    return true;
}

struct IpsRecord {
    FileOffset offset = 0;
    std::vector<unsigned char> data;
    FileOffset length = 0;
    unsigned char rleValue = 0;
    bool rle = false;
    FileOffset Length() const { return length; }
};

class PatchReader {
public:
    explicit PatchReader(const DiskFile& file, FileOffset crcEnd = -1)
        : file_(file), crcEnd_(crcEnd < 0 ? file.Size() : crcEnd) {}

    FileOffset Position() const { return position_; }
    DWORD Error() const { return error_; }
    std::uint32_t Crc() const { return crc_.Final(); }

    bool ReadRaw(void* out, size_t size) {
        if (size > static_cast<size_t>(file_.Size() - position_)) {
            error_ = ERROR_HANDLE_EOF;
            return false;
        }
        if (!file_.ReadAt(position_, out, size)) {
            error_ = file_.Error();
            return false;
        }
        position_ += static_cast<FileOffset>(size);
        return true;
    }

    bool ReadPatch(void* out, size_t size) {
        if (size > static_cast<size_t>(crcEnd_ - position_)) {
            error_ = ERROR_HANDLE_EOF;
            return false;
        }
        if (!ReadRaw(out, size)) { return false; }
        crc_.Add(out, size);
        return true;
    }

    bool ReadByte(unsigned char& value) { return ReadPatch(&value, 1); }

private:
    const DiskFile& file_;
    FileOffset crcEnd_;
    FileOffset position_ = 0;
    Crc32 crc_;
    DWORD error_ = ERROR_SUCCESS;
};

bool ReadBpsVar(PatchReader& reader, std::uint64_t& value) {
    value = 0;
    std::uint64_t shift = 1;
    for (int i = 0; i < 10; ++i) {
        unsigned char byte = 0;
        if (!reader.ReadByte(byte)) { return false; }
        const std::uint64_t part = byte & 0x7fu;
        if (part != 0 && shift > (std::numeric_limits<std::uint64_t>::max)() / part) {
            return false;
        }
        const std::uint64_t add = part * shift;
        if (value > (std::numeric_limits<std::uint64_t>::max)() - add) { return false; }
        value += add;
        if ((byte & 0x80u) != 0) { return true; }
        if (shift > (std::numeric_limits<std::uint64_t>::max)() / 0x80u) {
            return false;
        }
        shift <<= 7;
        if (value > (std::numeric_limits<std::uint64_t>::max)() - shift) {
            return false;
        }
        value += shift;
    }
    return false;
}

void WriteBpsVar(std::vector<unsigned char>& out, std::uint64_t value) {
    for (;;) {
        unsigned char byte = static_cast<unsigned char>(value & 0x7fu);
        value >>= 7;
        if (value == 0) {
            out.push_back(static_cast<unsigned char>(byte | 0x80u));
            return;
        }
        out.push_back(byte);
        --value;
    }
}

std::uint64_t EncodeSigned(FileOffset value) {
    if (value < 0) {
        const auto magnitude = static_cast<std::uint64_t>(-(value + 1)) + 1;
        return (magnitude << 1) | 1u;
    }
    return static_cast<std::uint64_t>(value) << 1;
}

bool DecodeSigned(std::uint64_t encoded, FileOffset& value) {
    const std::uint64_t magnitude = encoded >> 1;
    if (magnitude > static_cast<std::uint64_t>((std::numeric_limits<FileOffset>::max)())) {
        return false;
    }
    if ((encoded & 1u) != 0) {
        if (magnitude == static_cast<std::uint64_t>((std::numeric_limits<FileOffset>::max)()) + 1u) {
            value = (std::numeric_limits<FileOffset>::min)();
        } else {
            value = -static_cast<FileOffset>(magnitude);
        }
    } else {
        value = static_cast<FileOffset>(magnitude);
    }
    return true;
}

struct HashEntry {
    std::uint32_t key = 0;
    FileOffset position = 0;
};

std::uint32_t Hash4(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

// Returns kOk, kCancelled, kOutOfMemory, or kReadFailed carrying source's error.
BinaryPatchResult BuildHashIndex(const DiskFile& source, const BinaryPatchOptions& options,
                                 std::vector<HashEntry>& entries) {
    entries.clear();
    if (source.Size() < 4) { return Result(BinaryPatchStatus::kOk); }
    const size_t maxEntries = static_cast<size_t>((std::min)(
        static_cast<std::uint64_t>(source.Size() - 3),
        static_cast<std::uint64_t>(options.limits.bpsHashMemoryBytes / sizeof(HashEntry))));
    if (maxEntries == 0) { return Result(BinaryPatchStatus::kOk); }
    try { entries.reserve(maxEntries); }
    catch (const std::bad_alloc&) { return Result(BinaryPatchStatus::kOutOfMemory); }
    const FileOffset possible = source.Size() - 3;
    const FileOffset stride = (possible > static_cast<FileOffset>(maxEntries))
        ? (possible + static_cast<FileOffset>(maxEntries) - 1) /
              static_cast<FileOffset>(maxEntries)
        : 1;
    std::vector<unsigned char> buffer;
    try { buffer.resize(kIoChunk + 3); }
    catch (const std::bad_alloc&) { return Result(BinaryPatchStatus::kOutOfMemory); }
    for (FileOffset chunkStart = 0; chunkStart < source.Size();) {
        const size_t count = static_cast<size_t>((std::min)(
            static_cast<FileOffset>(kIoChunk + 3), source.Size() - chunkStart));
        if (!source.ReadAt(chunkStart, buffer.data(), count)) {
            return Result(BinaryPatchStatus::kReadFailed, source.Error());
        }
        const FileOffset last = chunkStart + static_cast<FileOffset>(count);
        FileOffset p = (chunkStart / stride) * stride;
        if (p < chunkStart) { p += stride; }
        for (; p + 3 < last && entries.size() < maxEntries; p += stride) {
            const size_t inChunk = static_cast<size_t>(p - chunkStart);
            entries.push_back({Hash4(buffer.data() + inChunk), p});
        }
        if (last == source.Size()) { break; }
        chunkStart += static_cast<FileOffset>(count - 3);
        if (!Report(options, chunkStart, source.Size())) {
            return Result(BinaryPatchStatus::kCancelled);
        }
    }
    std::sort(entries.begin(), entries.end(), [](const HashEntry& a, const HashEntry& b) {
        if (a.key != b.key) { return a.key < b.key; }
        return a.position < b.position;
    });
    return Result(BinaryPatchStatus::kOk);
}

// Records the error of whichever file (source or target) failed to read, so
// the caller does not report one file's stale error for the other's failure.
struct ReadFailure {
    bool failed = false;
    DWORD error = ERROR_SUCCESS;

    void From(const CachedReader& reader) {
        failed = true;
        error = reader.Error();
    }
};

bool ReadBoth(CachedReader& source, FileOffset sourcePos, void* sourceOut,
              CachedReader& target, FileOffset targetPos, void* targetOut,
              size_t count, ReadFailure& failure) {
    if (!source.ReadAt(sourcePos, sourceOut, count)) {
        failure.From(source);
        return false;
    }
    if (!target.ReadAt(targetPos, targetOut, count)) {
        failure.From(target);
        return false;
    }
    return true;
}

size_t MatchLength(CachedReader& source, CachedReader& target,
                   FileOffset sourcePos, FileOffset targetPos,
                   FileOffset maximum, ReadFailure& failure) {
    failure = ReadFailure();
    std::array<unsigned char, kMatchChunk> sourceBuffer;
    std::array<unsigned char, kMatchChunk> targetBuffer;
    const size_t probe = static_cast<size_t>((std::min)(
        maximum, static_cast<FileOffset>(kMatchProbe)));
    if (probe == 0) { return 0; }
    if (!ReadBoth(source, sourcePos, sourceBuffer.data(),
                  target, targetPos, targetBuffer.data(), probe, failure)) {
        return 0;
    }
    size_t same = 0;
    while (same < probe && sourceBuffer[same] == targetBuffer[same]) { ++same; }
    if (same != probe) { return same; }
    if (static_cast<FileOffset>(probe) == maximum) { return probe; }
    FileOffset matched = static_cast<FileOffset>(probe);
    while (matched < maximum) {
        const size_t count = static_cast<size_t>((std::min)(
            static_cast<FileOffset>(sourceBuffer.size()), maximum - matched));
        if (!ReadBoth(source, sourcePos + matched, sourceBuffer.data(),
                      target, targetPos + matched, targetBuffer.data(), count, failure)) {
            return static_cast<size_t>(matched);
        }
        size_t matchedChunk = 0;
        while (matchedChunk < count && sourceBuffer[matchedChunk] == targetBuffer[matchedChunk]) { ++matchedChunk; }
        matched += static_cast<FileOffset>(matchedChunk);
        if (matchedChunk != count) { break; }
    }
    return static_cast<size_t>(matched);
}

size_t SourceReadLength(CachedReader& source, CachedReader& target,
                        FileOffset targetPos, FileOffset sourceSize,
                        FileOffset targetSize, ReadFailure& failure) {
    failure = ReadFailure();
    if (targetPos >= sourceSize) { return 0; }
    return MatchLength(source, target, targetPos, targetPos,
                       (std::min)(sourceSize - targetPos, targetSize - targetPos),
                       failure);
}

size_t FindSourceCopy(CachedReader& source, CachedReader& target,
                      const std::vector<HashEntry>& entries,
                      FileOffset targetPos, FileOffset targetSize,
                      FileOffset sourceSize, std::uint64_t searchLimit,
                      std::uint64_t compareLimit,
                      std::uint64_t& comparedBytes,
                      FileOffset& bestPosition, ReadFailure& failure,
                      bool& cancelled, const BinaryPatchOptions& options) {
    bestPosition = 0;
    failure = ReadFailure();
    cancelled = false;
    if (targetSize - targetPos < 4 || sourceSize < 4 || entries.empty()) { return 0; }
    unsigned char bytes[4] = {};
    if (!target.ReadAt(targetPos, bytes, sizeof(bytes))) { failure.From(target); return 0; }
    const std::uint32_t key = Hash4(bytes);
    const auto begin = std::lower_bound(entries.begin(), entries.end(), key,
        [](const HashEntry& entry, std::uint32_t value) { return entry.key < value; });
    size_t best = 0;
    std::uint64_t tested = 0;
    for (auto it = begin; it != entries.end() && it->key == key && tested < searchLimit;
         ++it, ++tested) {
        if (it->position + 4 > sourceSize) { continue; }
        if (compareLimit < 4 || comparedBytes > compareLimit - 4) {
            break;
        }
        const std::uint64_t before = comparedBytes;
        const size_t length = MatchLength(source, target, it->position, targetPos,
            (std::min)(sourceSize - it->position, targetSize - targetPos),
            failure);
        comparedBytes += (std::max)(static_cast<std::uint64_t>(4),
                                    static_cast<std::uint64_t>(length));
        if (failure.failed) { return 0; }
        if (comparedBytes < before) { comparedBytes = compareLimit; }
        if (length > best) {
            best = length;
            bestPosition = it->position;
            if (best == static_cast<size_t>(targetSize - targetPos)) {
                break;
            }
        }
        if (!Report(options, targetPos, targetSize)) {
            cancelled = true;
            return best;
        }
    }
    return best;
}

bool WriteBpsInstruction(PatchSink& sink, unsigned action, FileOffset length) {
    if (length <= 0) { return false; }
    std::vector<unsigned char> bytes;
    WriteBpsVar(bytes, (static_cast<std::uint64_t>(length) - 1u) * 4u + action);
    return sink.Write(bytes.data(), bytes.size());
}

bool WriteBpsSigned(PatchSink& sink, FileOffset value) {
    std::vector<unsigned char> bytes;
    WriteBpsVar(bytes, EncodeSigned(value));
    return sink.Write(bytes.data(), bytes.size());
}

BinaryPatchResult GenerateIpsToStage(const DiskFile& source, const DiskFile& target,
                                     const wchar_t* stage,
                                     const BinaryPatchOptions& options) {
    if (source.Size() != target.Size() ||
        source.Size() > BinaryPatchLimits::kIpsGenerateMaxBytes ||
        source.Size() > 0xffffff + 1ll) {
        return Result(BinaryPatchStatus::kLimitExceeded);
    }
    PatchSink sink;
    if (!sink.Open(stage, (std::min)(options.limits.maxTemporaryBytes,
                                     options.limits.maxPatchBytes))) {
        return Result(BinaryPatchStatus::kOpenFailed, sink.Error());
    }
    if (!sink.Write("PATCH", 5)) {
        sink.Abort();
        return Result(BinaryPatchStatus::kWriteFailed, sink.Error());
    }
    std::vector<unsigned char> sourceBuffer;
    std::vector<unsigned char> targetBuffer;
    try {
        sourceBuffer.resize(kIoChunk);
        targetBuffer.resize(kIoChunk);
    } catch (const std::bad_alloc&) {
        sink.Abort();
        return Result(BinaryPatchStatus::kOutOfMemory);
    }
    for (FileOffset chunkStart = 0; chunkStart < source.Size();) {
        const size_t count = static_cast<size_t>((std::min)(
            static_cast<FileOffset>(sourceBuffer.size()), source.Size() - chunkStart));
        if (!source.ReadAt(chunkStart, sourceBuffer.data(), count)) {
            sink.Abort();
            return Result(BinaryPatchStatus::kReadFailed, source.Error(), chunkStart);
        }
        if (!target.ReadAt(chunkStart, targetBuffer.data(), count)) {
            sink.Abort();
            return Result(BinaryPatchStatus::kReadFailed, target.Error(), chunkStart);
        }
        size_t i = 0;
        while (i < count) {
            while (i < count && sourceBuffer[i] == targetBuffer[i]) { ++i; }
            const size_t start = i;
            while (i < count && sourceBuffer[i] != targetBuffer[i] && i - start < 0xffffu) {
                ++i;
            }
            if (i != start) {
                BinaryPatchStatus segmentFailure = BinaryPatchStatus::kWriteFailed;
                DWORD segmentError = ERROR_SUCCESS;
                if (!EmitIpsSegment(sink, target,
                                    chunkStart + static_cast<FileOffset>(start),
                                    targetBuffer.data() + start, i - start,
                                    segmentFailure, segmentError)) {
                    sink.Abort();
                    return Result(segmentFailure, segmentError,
                                  chunkStart + static_cast<FileOffset>(i));
                }
            }
        }
        chunkStart += static_cast<FileOffset>(count);
        if (!Report(options, chunkStart, source.Size())) {
            sink.Abort();
            return Result(BinaryPatchStatus::kCancelled, 0, chunkStart);
        }
    }
    if (!sink.Write("EOF", 3) || !sink.Commit()) {
        const DWORD error = sink.Error();
        sink.Abort();
        return Result(BinaryPatchStatus::kWriteFailed, error);
    }
    BinaryPatchResult result = Result(BinaryPatchStatus::kOk, 0, target.Size());
    result.sourceSize = source.Size();
    result.targetSize = target.Size();
    return result;
}

BinaryPatchResult GenerateBpsToStage(const DiskFile& source, const DiskFile& target,
                                     const wchar_t* stage,
                                     const BinaryPatchOptions& options) {
    if (source.Size() > options.limits.bpsGenerateMaxSourceBytes ||
        target.Size() > options.limits.bpsGenerateMaxTargetBytes) {
        return Result(BinaryPatchStatus::kLimitExceeded);
    }
    std::vector<HashEntry> entries;
    const BinaryPatchResult indexResult = BuildHashIndex(source, options, entries);
    if (!indexResult.Ok()) { return indexResult; }
    std::uint32_t sourceCrc = 0;
    std::uint32_t targetCrc = 0;
    const BinaryPatchResult sourceCrcResult = ComputeCrc(source, options, sourceCrc);
    if (!sourceCrcResult.Ok()) { return sourceCrcResult; }
    const BinaryPatchResult targetCrcResult = ComputeCrc(target, options, targetCrc);
    if (!targetCrcResult.Ok()) { return targetCrcResult; }
    PatchSink sink;
    if (!sink.Open(stage, (std::min)(options.limits.maxTemporaryBytes,
                                     options.limits.maxPatchBytes))) {
        return Result(BinaryPatchStatus::kOpenFailed, sink.Error());
    }
    if (!sink.Write("BPS1", 4)) { sink.Abort(); return Result(BinaryPatchStatus::kWriteFailed, sink.Error()); }
    std::vector<unsigned char> var;
    WriteBpsVar(var, static_cast<std::uint64_t>(source.Size()));
    WriteBpsVar(var, static_cast<std::uint64_t>(target.Size()));
    WriteBpsVar(var, 0); // metadata length; generated metadata is deliberately empty
    if (!sink.Write(var.data(), var.size())) { sink.Abort(); return Result(BinaryPatchStatus::kWriteFailed, sink.Error()); }

    CachedReader sourceReader(source);
    CachedReader targetReader(target);
    std::vector<unsigned char> literalBuffer;
    try { literalBuffer.resize(kIoChunk); }
    catch (const std::bad_alloc&) { sink.Abort(); return Result(BinaryPatchStatus::kOutOfMemory); }
    FileOffset targetPos = 0;
    FileOffset sourceRelative = 0;
    std::uint64_t comparedBytes = 0;
    std::uint64_t instructionCount = 0;
    const auto reserveInstruction = [&]() {
        return instructionCount < options.limits.maxBpsInstructions && ++instructionCount <= options.limits.maxBpsInstructions;
    };
    while (targetPos < target.Size()) {
        ReadFailure readFailure;
        const size_t sourceRead = SourceReadLength(sourceReader, targetReader,
                                                   targetPos, source.Size(), target.Size(),
                                                   readFailure);
        if (readFailure.failed) {
            sink.Abort(); return Result(BinaryPatchStatus::kReadFailed, readFailure.error, targetPos);
        }
        if (sourceRead > 0) {
            if (!reserveInstruction()) { sink.Abort(); return Result(BinaryPatchStatus::kLimitExceeded, 0, targetPos); }
            if (!WriteBpsInstruction(sink, 0, static_cast<FileOffset>(sourceRead))) {
                sink.Abort(); return Result(BinaryPatchStatus::kWriteFailed, sink.Error(), targetPos);
            }
            targetPos += static_cast<FileOffset>(sourceRead);
            if (!Report(options, targetPos, target.Size())) { sink.Abort(); return Result(BinaryPatchStatus::kCancelled, 0, targetPos); }
            continue;
        }
        FileOffset sourceCopyPosition = 0;
        bool candidateCancelled = false;
        const size_t sourceCopy = FindSourceCopy(sourceReader, targetReader, entries,
            targetPos, target.Size(), source.Size(), options.limits.bpsHashSearchLimit,
            options.limits.bpsHashCompareBytes, comparedBytes,
            sourceCopyPosition, readFailure, candidateCancelled, options);
        if (candidateCancelled) {
            sink.Abort(); return Result(BinaryPatchStatus::kCancelled, 0, targetPos);
        }
        if (readFailure.failed) {
            sink.Abort(); return Result(BinaryPatchStatus::kReadFailed, readFailure.error, targetPos);
        }
        if (sourceCopy >= 4) {
            if (!reserveInstruction()) { sink.Abort(); return Result(BinaryPatchStatus::kLimitExceeded, 0, targetPos); }
            if (!WriteBpsInstruction(sink, 2, static_cast<FileOffset>(sourceCopy)) ||
                !WriteBpsSigned(sink, sourceCopyPosition - sourceRelative)) {
                sink.Abort(); return Result(BinaryPatchStatus::kWriteFailed, sink.Error(), targetPos);
            }
            sourceRelative = sourceCopyPosition + static_cast<FileOffset>(sourceCopy);
            targetPos += static_cast<FileOffset>(sourceCopy);
            if (!Report(options, targetPos, target.Size())) { sink.Abort(); return Result(BinaryPatchStatus::kCancelled, 0, targetPos); }
            continue;
        }

        const FileOffset literalStart = targetPos;
        ++targetPos;
        while (targetPos < target.Size()) {
            if ((targetPos - literalStart) % kLiteralSearchStride != 0) {
                ++targetPos;
                continue;
            }
            const size_t literalSourceRead = SourceReadLength(sourceReader, targetReader,
                targetPos, source.Size(), target.Size(), readFailure);
            if (readFailure.failed) {
                sink.Abort(); return Result(BinaryPatchStatus::kReadFailed, readFailure.error, targetPos);
            }
            if (literalSourceRead > 0) {
                break;
            }
            FileOffset ignored = 0;
            const size_t literalSourceCopy = FindSourceCopy(sourceReader, targetReader,
                entries, targetPos, target.Size(), source.Size(),
                options.limits.bpsHashSearchLimit, options.limits.bpsHashCompareBytes,
                comparedBytes, ignored, readFailure, candidateCancelled, options);
            if (candidateCancelled) {
                sink.Abort(); return Result(BinaryPatchStatus::kCancelled, 0, targetPos);
            }
            if (readFailure.failed) {
                sink.Abort(); return Result(BinaryPatchStatus::kReadFailed, readFailure.error, targetPos);
            }
            if (literalSourceCopy >= 4) {
                break;
            }
            if (targetPos - literalStart >= 0x100000) { break; }
            ++targetPos;
        }
        const FileOffset literalLength = targetPos - literalStart;
        if (!reserveInstruction()) { sink.Abort(); return Result(BinaryPatchStatus::kLimitExceeded, 0, literalStart); }
        const bool literalFits = literalLength <= static_cast<FileOffset>(literalBuffer.size());
        if (literalFits &&
            !target.ReadAt(literalStart, literalBuffer.data(), static_cast<size_t>(literalLength))) {
            sink.Abort(); return Result(BinaryPatchStatus::kReadFailed, target.Error(), literalStart);
        }
        if (!literalFits ||
            !WriteBpsInstruction(sink, 1, literalLength) ||
            !sink.Write(literalBuffer.data(), static_cast<size_t>(literalLength))) {
            sink.Abort(); return Result(BinaryPatchStatus::kWriteFailed, sink.Error(), literalStart);
        }
        if (!Report(options, targetPos, target.Size())) { sink.Abort(); return Result(BinaryPatchStatus::kCancelled, 0, targetPos); }
    }
    unsigned char footer[12] = {};
    PutLe32(footer, sourceCrc);
    PutLe32(footer + 4, targetCrc);
    // BPS excludes only the final patch-CRC field.  The source and target
    // CRC fields are part of the CRC input (and are not metadata).
    PutLe32(footer + 8, sink.CrcWith(footer, 8));
    if (!sink.Write(footer, sizeof(footer)) || !sink.Commit()) {
        const DWORD error = sink.Error();
        sink.Abort();
        return Result(BinaryPatchStatus::kWriteFailed, error, targetPos);
    }
    BinaryPatchResult result = Result(BinaryPatchStatus::kOk, 0, targetPos);
    result.sourceSize = source.Size();
    result.targetSize = target.Size();
    return result;
}

BinaryPatchResult ApplyIps(const DiskFile& source, const DiskFile& patch,
                           const wchar_t* targetPath,
                           const BinaryPatchOptions& options) {
    if (source.Size() > options.limits.ipsApplyMaxSourceBytes ||
        patch.Size() > options.limits.maxPatchBytes) {
        return Result(BinaryPatchStatus::kLimitExceeded);
    }
    if (patch.Size() < 8) { return Result(BinaryPatchStatus::kInvalidPatch); }
    PatchReader reader(patch);
    unsigned char header[5] = {};
    if (!reader.ReadRaw(header, sizeof(header)) || std::memcmp(header, "PATCH", 5) != 0) {
        return Result(BinaryPatchStatus::kInvalidPatch, reader.Error());
    }
    std::vector<IpsRecord> records;
    try { records.reserve(32); } catch (const std::bad_alloc&) { return Result(BinaryPatchStatus::kOutOfMemory); }
    FileOffset maxRecordEnd = 0;
    size_t recordMemory = 0;
    FileOffset expandedBytes = 0;
    bool hasExtension = false;
    FileOffset extensionSize = 0;
    bool sawEof = false;
    while (reader.Position() + 3 <= patch.Size()) {
        unsigned char offsetBytes[3] = {};
        if (!reader.ReadRaw(offsetBytes, sizeof(offsetBytes))) { return Result(BinaryPatchStatus::kInvalidPatch, reader.Error()); }
        if (std::memcmp(offsetBytes, "EOF", 3) == 0) {
            sawEof = true;
            if (reader.Position() < patch.Size()) {
                if (patch.Size() - reader.Position() != 3) { return Result(BinaryPatchStatus::kInvalidPatch); }
                unsigned char lengthBytes[3] = {};
                if (!reader.ReadRaw(lengthBytes, sizeof(lengthBytes))) { return Result(BinaryPatchStatus::kInvalidPatch, reader.Error()); }
                hasExtension = true;
                extensionSize = static_cast<FileOffset>(GetBe24(lengthBytes));
            }
            break;
        }
        const FileOffset offset = static_cast<FileOffset>(GetBe24(offsetBytes));
        unsigned char lengthBytes[2] = {};
        if (!reader.ReadRaw(lengthBytes, sizeof(lengthBytes))) { return Result(BinaryPatchStatus::kInvalidPatch, reader.Error()); }
        const std::uint32_t length = GetBe16(lengthBytes);
        IpsRecord record;
        record.offset = offset;
        if (length != 0) {
            try { record.data.resize(length); }
            catch (const std::bad_alloc&) { return Result(BinaryPatchStatus::kOutOfMemory); }
            if (!reader.ReadRaw(record.data.data(), record.data.size())) { return Result(BinaryPatchStatus::kInvalidPatch, reader.Error()); }
            record.length = length;
        } else {
            unsigned char rleLengthBytes[2] = {};
            if (!reader.ReadRaw(rleLengthBytes, sizeof(rleLengthBytes))) { return Result(BinaryPatchStatus::kInvalidPatch, reader.Error()); }
            const std::uint32_t rleLength = GetBe16(rleLengthBytes);
            if (rleLength == 0) { return Result(BinaryPatchStatus::kInvalidPatch); }
            unsigned char value = 0;
            if (!reader.ReadRaw(&value, 1)) { return Result(BinaryPatchStatus::kInvalidPatch, reader.Error()); }
            record.rle = true;
            record.rleValue = value;
            record.length = rleLength;
        }
        FileOffset end = 0;
        if (AddWouldOverflow(record.offset, record.Length(), &end)) {
            return Result(BinaryPatchStatus::kInvalidPatch);
        }
        if (options.limits.ipsExpandedBytes <= 0 ||
            record.Length() > options.limits.ipsExpandedBytes -
                (std::min)(options.limits.ipsExpandedBytes, expandedBytes)) {
            return Result(BinaryPatchStatus::kLimitExceeded);
        }
        const size_t payloadBytes = record.data.size();
        if (recordMemory > options.limits.ipsRecordMemoryBytes ||
            payloadBytes > options.limits.ipsRecordMemoryBytes - recordMemory ||
            sizeof(IpsRecord) > options.limits.ipsRecordMemoryBytes - recordMemory - payloadBytes) {
            return Result(BinaryPatchStatus::kLimitExceeded);
        }
        recordMemory += payloadBytes + sizeof(IpsRecord);
        expandedBytes += record.Length();
        maxRecordEnd = (std::max)(maxRecordEnd, end);
        if (records.size() >= options.limits.maxIpsRecords) { return Result(BinaryPatchStatus::kLimitExceeded); }
        records.push_back(std::move(record));
        if (!Report(options, reader.Position(), patch.Size())) {
            return Result(BinaryPatchStatus::kCancelled, 0, reader.Position());
        }
    }
    if (!sawEof) { return Result(BinaryPatchStatus::kInvalidPatch); }
    const size_t auxiliaryBytes = records.size() * sizeof(std::uint32_t) * 2;
    if (auxiliaryBytes > options.limits.ipsRecordMemoryBytes -
            (std::min)(options.limits.ipsRecordMemoryBytes, recordMemory)) {
        return Result(BinaryPatchStatus::kLimitExceeded);
    }
    std::vector<std::uint32_t> orderByOffset;
    std::vector<std::uint32_t> activeRecords;
    try {
        orderByOffset.reserve(records.size());
        activeRecords.reserve(records.size());
    } catch (const std::bad_alloc&) {
        return Result(BinaryPatchStatus::kOutOfMemory);
    }
    for (std::uint32_t index = 0; index < records.size(); ++index) {
        orderByOffset.push_back(index);
    }
    std::stable_sort(orderByOffset.begin(), orderByOffset.end(),
                     [&records](std::uint32_t left, std::uint32_t right) {
                         return records[left].offset < records[right].offset;
                     });
    FileOffset targetSize = hasExtension ? extensionSize : (std::max)(source.Size(), maxRecordEnd);
    // An EOF-after-3-byte length is a final truncate/extend operation.  A
    // record beyond that final length is valid input and simply contributes no
    // output bytes (Flips/RomPatcher.js compatibility).
    if (targetSize > options.limits.ipsApplyMaxTargetBytes) {
        return Result(BinaryPatchStatus::kLimitExceeded);
    }
    FileOffset outputPeak = 0;
    if (MultiplyWouldOverflow(targetSize, 2, &outputPeak) ||
        outputPeak > options.limits.maxTemporaryBytes) {
        return Result(BinaryPatchStatus::kLimitExceeded);
    }
    DWORD error = 0;
    if (!CheckOutputConflicts(source, &patch, targetPath, error)) {
        return Result(BinaryPatchStatus::kConflict, error);
    }
    StreamFileWriter writer;
    const FileIoResult opened = writer.Open(targetPath);
    if (!opened.Ok()) { return Result(BinaryPatchStatus::kOpenFailed, opened.systemError); }
    std::vector<unsigned char> buffer;
    try { buffer.resize(kIoChunk); } catch (const std::bad_alloc&) {
        writer.Abort(); return Result(BinaryPatchStatus::kOutOfMemory);
    }
    CachedReader sourceReader(source);
    for (FileOffset position = 0; position < targetSize;) {
        const size_t count = static_cast<size_t>((std::min)(
            static_cast<FileOffset>(buffer.size()), targetSize - position));
        const size_t fromSource = position < source.Size()
            ? static_cast<size_t>((std::min)(static_cast<FileOffset>(count), source.Size() - position)) : 0;
        if (fromSource > 0 && !sourceReader.ReadAt(position, buffer.data(), fromSource)) {
            writer.Abort(); return Result(BinaryPatchStatus::kReadFailed, source.Error(), position);
        }
        if (fromSource < count) { std::memset(buffer.data() + fromSource, 0, count - fromSource); }
        const FileOffset end = position + static_cast<FileOffset>(count);
        activeRecords.clear();
        const FileOffset scanStart = position > 0xffff ? position - 0xffff : 0;
        const auto first = std::lower_bound(
            orderByOffset.begin(), orderByOffset.end(), scanStart,
            [&records](std::uint32_t index, FileOffset value) {
                return records[index].offset < value;
            });
        for (auto it = first; it != orderByOffset.end() && records[*it].offset < end; ++it) {
            const IpsRecord& record = records[*it];
            const FileOffset recordEnd = record.offset + record.Length();
            if (recordEnd > position) { activeRecords.push_back(*it); }
        }
        std::sort(activeRecords.begin(), activeRecords.end());
        for (size_t activeIndex = 0; activeIndex < activeRecords.size(); ++activeIndex) {
            const IpsRecord& record = records[activeRecords[activeIndex]];
            const FileOffset recordEnd = record.offset + record.Length();
            const FileOffset begin = (std::max)(position, record.offset);
            const FileOffset finish = (std::min)(end, recordEnd);
            for (FileOffset p = begin; p < finish; ++p) {
                const size_t index = static_cast<size_t>(p - record.offset);
                buffer[static_cast<size_t>(p - position)] = record.rle ? record.rleValue : record.data[index];
            }
            if ((activeIndex & 0x3ffu) == 0 &&
                !Report(options, position, targetSize)) {
                writer.Abort();
                return Result(BinaryPatchStatus::kCancelled, 0, position);
            }
        }
        const FileIoResult wrote = writer.Write(buffer.data(), count);
        if (!wrote.Ok()) { writer.Abort(); return Result(BinaryPatchStatus::kWriteFailed, wrote.systemError, position); }
        position = end;
        if (!Report(options, position, targetSize)) { writer.Abort(); return Result(BinaryPatchStatus::kCancelled, 0, position); }
    }
    const FileIoResult committed = writer.Commit();
    if (!committed.Ok()) {
        BinaryPatchResult failed = Result(BinaryPatchStatus::kWriteFailed,
                                          committed.systemError, committed.fileSize);
        failed.keptTempPath = committed.keptTempPath;
        return failed;
    }
    BinaryPatchResult result = Result(BinaryPatchStatus::kOk, 0, targetSize);
    result.sourceSize = source.Size();
    result.targetSize = targetSize;
    // IPS has no source checksum; this is intentionally reported as unverified.
    // IPS has no target checksum; successful writing is not a byte-level
    // verification against an expected target.
    result.targetVerified = false;
    return result;
}

bool CopyOutputBytes(TempOutput& output, const DiskFile& source,
                     FileOffset offset, FileOffset length, Crc32& crc,
                     DWORD& error, const BinaryPatchOptions& options,
                     FileOffset progressBase, FileOffset progressTotal,
                     std::vector<unsigned char>& buffer,
                     BinaryPatchStatus& failure) {
    failure = BinaryPatchStatus::kInvalidPatch;
    const FileOffset totalLength = length;
    while (length > 0) {
        const size_t count = static_cast<size_t>((std::min)(
            static_cast<FileOffset>(buffer.size()), length));
        if (!source.ReadAt(offset, buffer.data(), count)) {
            error = source.Error();
            failure = BinaryPatchStatus::kReadFailed;
            return false;
        }
        if (!output.Append(buffer.data(), count, error)) {
            failure = BinaryPatchStatus::kWriteFailed;
            return false;
        }
        crc.Add(buffer.data(), count);
        offset += static_cast<FileOffset>(count);
        length -= static_cast<FileOffset>(count);
        if (!Report(options, progressBase + (totalLength - length), progressTotal)) {
            error = ERROR_CANCELLED;
            failure = BinaryPatchStatus::kCancelled;
            return false;
        }
    }
    return true;
}

bool CopyTargetBytes(TempOutput& output, FileOffset sourceOffset,
                     FileOffset length, Crc32& crc, DWORD& error,
                     const BinaryPatchOptions& options,
                     FileOffset progressBase, FileOffset progressTotal,
                     std::vector<unsigned char>& buffer,
                     BinaryPatchStatus& failure) {
    failure = BinaryPatchStatus::kInvalidPatch;
    const FileOffset initialWritten = output.Written();
    if (sourceOffset < 0 || sourceOffset >= initialWritten) {
        error = ERROR_INVALID_DATA;
        return false;
    }
    const FileOffset distance = initialWritten - sourceOffset;
    FileOffset copied = 0;
    while (copied < length) {
        FileOffset readOffset = 0;
        if (copied >= distance) {
            readOffset = sourceOffset + (copied % distance);
        } else {
            readOffset = sourceOffset + copied;
        }
        const FileOffset available = initialWritten + copied - readOffset;
        if (available <= 0) { error = ERROR_INVALID_DATA; return false; }
        const FileOffset countMax = (std::min)(length - copied,
            (std::min)(static_cast<FileOffset>(buffer.size()), available));
        const size_t count = static_cast<size_t>(countMax);
        if (!output.ReadAt(readOffset, buffer.data(), count, error)) {
            failure = BinaryPatchStatus::kReadFailed;
            return false;
        }
        if (!output.Append(buffer.data(), count, error)) {
            failure = BinaryPatchStatus::kWriteFailed;
            return false;
        }
        crc.Add(buffer.data(), count);
        copied += countMax;
        if (!Report(options, progressBase + copied, progressTotal)) {
            error = ERROR_CANCELLED;
            failure = BinaryPatchStatus::kCancelled;
            return false;
        }
    }
    return true;
}

BinaryPatchResult ApplyBps(const DiskFile& source, const DiskFile& patch,
                           const wchar_t* targetPath,
                           const BinaryPatchOptions& options) {
    if (source.Size() > options.limits.bpsMaxSourceBytes ||
        patch.Size() > options.limits.maxPatchBytes) {
        return Result(BinaryPatchStatus::kLimitExceeded);
    }
    if (patch.Size() < 16) { return Result(BinaryPatchStatus::kInvalidPatch); }
    const FileOffset footerPosition = patch.Size() - 12;
    // Parser stops before source/target CRCs; the reader CRC limit includes
    // those eight bytes and excludes only the final patch CRC field.
    PatchReader reader(patch, patch.Size() - 4);
    unsigned char header[4] = {};
    if (!reader.ReadPatch(header, sizeof(header)) || std::memcmp(header, "BPS1", 4) != 0) {
        return Result(BinaryPatchStatus::kInvalidPatch, reader.Error());
    }
    std::uint64_t sourceSize = 0, targetSize = 0, metadataSize = 0;
    if (!ReadBpsVar(reader, sourceSize) || !ReadBpsVar(reader, targetSize) ||
        !ReadBpsVar(reader, metadataSize) || sourceSize > static_cast<std::uint64_t>((std::numeric_limits<FileOffset>::max)()) ||
        targetSize > static_cast<std::uint64_t>((std::numeric_limits<FileOffset>::max)())) {
        return Result(BinaryPatchStatus::kInvalidPatch, reader.Error());
    }
    if (sourceSize != static_cast<std::uint64_t>(source.Size())) {
        return Result(BinaryPatchStatus::kSourceMismatch);
    }
    if (static_cast<FileOffset>(targetSize) > options.limits.bpsMaxTargetBytes) {
        return Result(BinaryPatchStatus::kLimitExceeded);
    }
    if (reader.Position() > footerPosition ||
        metadataSize > static_cast<std::uint64_t>(footerPosition - reader.Position())) {
        return Result(BinaryPatchStatus::kInvalidPatch);
    }
    FileOffset outputPeak = 0;
    if (MultiplyWouldOverflow(static_cast<FileOffset>(targetSize), 2, &outputPeak) ||
        outputPeak > options.limits.maxTemporaryBytes) {
        return Result(BinaryPatchStatus::kLimitExceeded);
    }
    std::vector<unsigned char> metadataBuffer;
    try { metadataBuffer.resize(kIoChunk); }
    catch (const std::bad_alloc&) { return Result(BinaryPatchStatus::kOutOfMemory); }
    while (metadataSize > 0) {
        const size_t count = static_cast<size_t>((std::min)(
            metadataSize, static_cast<std::uint64_t>(metadataBuffer.size())));
        if (!reader.ReadPatch(metadataBuffer.data(), count)) {
            return Result(BinaryPatchStatus::kInvalidPatch, reader.Error());
        }
        metadataSize -= count;
    }
    std::uint32_t sourceCrc = 0;
    const BinaryPatchResult sourceCrcResult = ComputeCrc(source, options, sourceCrc);
    if (!sourceCrcResult.Ok()) { return sourceCrcResult; }
    unsigned char expectedFooter[12] = {};
    if (!patch.ReadAt(footerPosition, expectedFooter, sizeof(expectedFooter))) {
        return Result(BinaryPatchStatus::kReadFailed, patch.Error());
    }
    if (GetLe32(expectedFooter) != sourceCrc) {
        return Result(BinaryPatchStatus::kSourceMismatch);
    }

    DWORD error = 0;
    if (!CheckOutputConflicts(source, &patch, targetPath, error)) {
        return Result(BinaryPatchStatus::kConflict, error);
    }
    TempOutput output;
    if (!output.Open(targetPath, error)) { return Result(BinaryPatchStatus::kOpenFailed, error); }
    std::vector<unsigned char> transferBuffer;
    try { transferBuffer.resize(kIoChunk); }
    catch (const std::bad_alloc&) { output.Abort(); return Result(BinaryPatchStatus::kOutOfMemory); }
    Crc32 targetCrc;
    FileOffset targetPosition = 0;
    FileOffset sourceRelative = 0;
    FileOffset targetRelative = 0;
    std::uint64_t instructionCount = 0;
    while (reader.Position() < footerPosition) {
        if (++instructionCount > options.limits.maxBpsInstructions) {
            output.Abort(); return Result(BinaryPatchStatus::kLimitExceeded, 0, targetPosition);
        }
        std::uint64_t instruction = 0;
        if (!ReadBpsVar(reader, instruction)) { output.Abort(); return Result(BinaryPatchStatus::kInvalidPatch, reader.Error(), targetPosition); }
        const unsigned action = static_cast<unsigned>(instruction & 3u);
        const std::uint64_t encodedLength = instruction >> 2;
        if (encodedLength == (std::numeric_limits<std::uint64_t>::max)()) {
            output.Abort(); return Result(BinaryPatchStatus::kInvalidPatch, 0, targetPosition);
        }
        const std::uint64_t lengthValue = encodedLength + 1u;
        if (lengthValue > static_cast<std::uint64_t>(targetSize) - static_cast<std::uint64_t>(targetPosition)) {
            output.Abort(); return Result(BinaryPatchStatus::kInvalidPatch, 0, targetPosition);
        }
        const FileOffset length = static_cast<FileOffset>(lengthValue);
        if (action == 0 || action == 2) {
            FileOffset start = targetPosition;
            if (action == 2) {
                std::uint64_t encoded = 0;
                FileOffset delta = 0;
                if (!ReadBpsVar(reader, encoded) || !DecodeSigned(encoded, delta) ||
                    (delta < 0 && sourceRelative < -delta) ||
                    (delta > 0 && sourceRelative > (std::numeric_limits<FileOffset>::max)() - delta)) {
                    output.Abort(); return Result(BinaryPatchStatus::kInvalidPatch, reader.Error(), targetPosition);
                }
                sourceRelative += delta;
                start = sourceRelative;
                if (start < 0 || start > source.Size() || length > source.Size() - start) {
                    output.Abort(); return Result(BinaryPatchStatus::kInvalidPatch, reader.Error(), targetPosition);
                }
            } else if (start < 0 || start > source.Size() || length > source.Size() - start) {
                output.Abort(); return Result(BinaryPatchStatus::kInvalidPatch, 0, targetPosition);
            }
            BinaryPatchStatus copyFailure = BinaryPatchStatus::kInvalidPatch;
            if (!CopyOutputBytes(output, source, start, length, targetCrc, error,
                                 options, targetPosition,
                                 static_cast<FileOffset>(targetSize),
                                 transferBuffer, copyFailure)) {
                output.Abort();
                return Result(copyFailure, error, targetPosition);
            }
            if (action == 2) { sourceRelative = start + length; }
        } else if (action == 1) {
            FileOffset left = length;
            while (left > 0) {
                const size_t count = static_cast<size_t>((std::min)(
                    left, static_cast<FileOffset>(transferBuffer.size())));
                if (!reader.ReadPatch(transferBuffer.data(), count)) {
                    output.Abort(); return Result(BinaryPatchStatus::kInvalidPatch, reader.Error(), targetPosition);
                }
                if (!output.Append(transferBuffer.data(), count, error)) {
                    output.Abort(); return Result(BinaryPatchStatus::kWriteFailed, error, targetPosition);
                }
                targetCrc.Add(transferBuffer.data(), count);
                left -= static_cast<FileOffset>(count);
                if (!Report(options, targetPosition + (length - left),
                            static_cast<FileOffset>(targetSize))) {
                    output.Abort(); return Result(BinaryPatchStatus::kCancelled, 0, targetPosition + length - left);
                }
            }
        } else {
            std::uint64_t encoded = 0;
            FileOffset delta = 0;
            if (!ReadBpsVar(reader, encoded) || !DecodeSigned(encoded, delta)) {
                output.Abort(); return Result(BinaryPatchStatus::kInvalidPatch, reader.Error(), targetPosition);
            }
            if ((delta < 0 && targetRelative < -delta) ||
                (delta > 0 && targetRelative > (std::numeric_limits<FileOffset>::max)() - delta)) {
                output.Abort(); return Result(BinaryPatchStatus::kInvalidPatch, 0, targetPosition);
            }
            targetRelative += delta;
            if (targetRelative < 0 || targetRelative >= output.Written()) {
                output.Abort(); return Result(BinaryPatchStatus::kInvalidPatch, 0, targetPosition);
            }
            BinaryPatchStatus copyFailure = BinaryPatchStatus::kInvalidPatch;
            if (!CopyTargetBytes(output, targetRelative, length, targetCrc, error,
                                options, targetPosition,
                                static_cast<FileOffset>(targetSize),
                                transferBuffer, copyFailure)) {
                output.Abort(); return Result(copyFailure, error, targetPosition);
            }
            targetRelative += length;
        }
        targetPosition += length;
        if (!Report(options, targetPosition, static_cast<FileOffset>(targetSize))) {
            output.Abort(); return Result(BinaryPatchStatus::kCancelled, 0, targetPosition);
        }
    }
    unsigned char footer[12] = {};
    if (reader.Position() != footerPosition || !reader.ReadPatch(footer, 8) ||
        !reader.ReadRaw(footer + 8, 4)) {
        output.Abort(); return Result(BinaryPatchStatus::kInvalidPatch, reader.Error(), targetPosition);
    }
    if (targetPosition != static_cast<FileOffset>(targetSize) ||
        GetLe32(footer) != sourceCrc || GetLe32(footer + 4) != targetCrc.Final() ||
        GetLe32(footer + 8) != reader.Crc()) {
        output.Abort();
        const BinaryPatchStatus status = GetLe32(footer) != sourceCrc
            ? BinaryPatchStatus::kSourceMismatch
            : (GetLe32(footer + 4) != targetCrc.Final()
                ? BinaryPatchStatus::kTargetMismatch
                : BinaryPatchStatus::kInvalidPatch);
        return Result(status, 0, targetPosition);
    }
    if (!output.Close(error)) { output.Abort(); return Result(BinaryPatchStatus::kWriteFailed, error, targetPosition); }
    BinaryPatchResult published;
    if (!PublishTemp(output.Path(), targetPath, options, published)) {
        // PublishTemp leaves the staged file in place only when the writer
        // reports a kept temporary path.  The output path is otherwise safe to
        // remove here.
        if (published.keptTempPath.empty()) { ::DeleteFileW(output.Path().c_str()); }
        return published;
    }
    output.Abort();
    published.sourceSize = source.Size();
    published.targetSize = static_cast<FileOffset>(targetSize);
    published.sourceVerified = true;
    published.targetVerified = true;
    return published;
}

BinaryPatchResult CompareFiles(const DiskFile& left, const DiskFile& right,
                               const BinaryPatchOptions& options) {
    if (left.Size() != right.Size()) { return Result(BinaryPatchStatus::kTargetMismatch); }
    std::vector<unsigned char> a, b;
    try { a.resize(kIoChunk); b.resize(kIoChunk); }
    catch (const std::bad_alloc&) { return Result(BinaryPatchStatus::kOutOfMemory); }
    for (FileOffset offset = 0; offset < left.Size();) {
        const size_t count = static_cast<size_t>((std::min)(
            static_cast<FileOffset>(a.size()), left.Size() - offset));
        if (!left.ReadAt(offset, a.data(), count) || !right.ReadAt(offset, b.data(), count)) {
            return Result(BinaryPatchStatus::kReadFailed, left.Error(), offset);
        }
        if (std::memcmp(a.data(), b.data(), count) != 0) {
            return Result(BinaryPatchStatus::kTargetMismatch, 0, offset);
        }
        offset += static_cast<FileOffset>(count);
        if (!Report(options, offset, left.Size())) { return Result(BinaryPatchStatus::kCancelled, 0, offset); }
    }
    return Result(BinaryPatchStatus::kOk, 0, left.Size());
}

}  // namespace

BinaryPatchResult GenerateBinaryPatch(const wchar_t* sourcePath,
                                      const wchar_t* targetPath,
                                      const wchar_t* patchPath,
                                      BinaryPatchFormat format,
                                      const BinaryPatchOptions& options) {
    if (sourcePath == nullptr || targetPath == nullptr || patchPath == nullptr ||
        *sourcePath == L'\0' || *targetPath == L'\0' || *patchPath == L'\0') {
        return Result(BinaryPatchStatus::kInvalidArgument);
    }
    if (format != BinaryPatchFormat::kIps && format != BinaryPatchFormat::kBps) {
        return Result(BinaryPatchStatus::kInvalidArgument);
    }
    DiskFile source;
    DiskFile target;
    if (!source.Open(sourcePath)) { return Result(BinaryPatchStatus::kOpenFailed, source.Error()); }
    if (!target.Open(targetPath)) { return Result(BinaryPatchStatus::kOpenFailed, target.Error()); }
    DWORD error = 0;
    if (!CheckOutputConflicts(source, &target, patchPath, error)) {
        return Result(BinaryPatchStatus::kConflict, error);
    }
    if (format == BinaryPatchFormat::kBps &&
        source.Size() > options.limits.bpsGenerateMaxSourceBytes) {
        return Result(BinaryPatchStatus::kLimitExceeded);
    }
    if (!GenerationFitsTemporaryBudget(target.Size(), format, options)) {
        return Result(BinaryPatchStatus::kLimitExceeded);
    }
    std::wstring stagePatch;
    if (!MakeTempPath(patchPath, L"STP", stagePatch, error)) {
        return Result(BinaryPatchStatus::kOpenFailed, error);
    }
    BinaryPatchResult generated = format == BinaryPatchFormat::kIps
        ? GenerateIpsToStage(source, target, stagePatch.c_str(), options)
        : GenerateBpsToStage(source, target, stagePatch.c_str(), options);
    if (!generated.Ok()) {
        ::DeleteFileW(stagePatch.c_str());
        return generated;
    }

    std::wstring stageTarget;
    // Keep verification scratch space beside the patch output.  A read-only
    // source/target directory must not make an otherwise valid patch fail.
    if (!MakeTempPath(patchPath, L"STT", stageTarget, error)) {
        ::DeleteFileW(stagePatch.c_str());
        return Result(BinaryPatchStatus::kOpenFailed, error);
    }
    BinaryPatchResult applied = ApplyBinaryPatch(sourcePath, stagePatch.c_str(),
                                                 stageTarget.c_str(), options);
    if (!applied.Ok()) {
        ::DeleteFileW(stagePatch.c_str());
        ::DeleteFileW(stageTarget.c_str());
        return applied;
    }
    DiskFile verifiedTarget;
    DiskFile generatedTarget;
    if (!verifiedTarget.Open(targetPath) || !generatedTarget.Open(stageTarget.c_str())) {
        ::DeleteFileW(stagePatch.c_str());
        ::DeleteFileW(stageTarget.c_str());
        return Result(BinaryPatchStatus::kOpenFailed,
                      !verifiedTarget.Valid() ? verifiedTarget.Error() : generatedTarget.Error());
    }
    BinaryPatchResult compared = CompareFiles(verifiedTarget, generatedTarget, options);
    verifiedTarget.Close();
    generatedTarget.Close();
    ::DeleteFileW(stageTarget.c_str());
    if (!compared.Ok()) {
        ::DeleteFileW(stagePatch.c_str());
        return compared;
    }
    BinaryPatchResult published;
    if (!PublishTemp(stagePatch, patchPath, options, published)) {
        // The generated staging patch is no longer useful after a failed
        // publish; any writer-side kept temp path is reported separately.
        ::DeleteFileW(stagePatch.c_str());
        return published;
    }
    published.sourceSize = source.Size();
    published.targetSize = target.Size();
    published.sourceVerified = true;
    published.targetVerified = true;
    return published;
}

BinaryPatchResult ApplyBinaryPatch(const wchar_t* sourcePath,
                                   const wchar_t* patchPath,
                                   const wchar_t* targetPath,
                                   const BinaryPatchOptions& options) {
    if (sourcePath == nullptr || patchPath == nullptr || targetPath == nullptr ||
        *sourcePath == L'\0' || *patchPath == L'\0' || *targetPath == L'\0') {
        return Result(BinaryPatchStatus::kInvalidArgument);
    }
    DiskFile source;
    DiskFile patch;
    if (!source.Open(sourcePath)) { return Result(BinaryPatchStatus::kOpenFailed, source.Error()); }
    if (!patch.Open(patchPath)) { return Result(BinaryPatchStatus::kOpenFailed, patch.Error()); }
    if (patch.Size() < 4) { return Result(BinaryPatchStatus::kInvalidPatch); }
    unsigned char magic[4] = {};
    if (!patch.ReadAt(0, magic, sizeof(magic))) { return Result(BinaryPatchStatus::kReadFailed, patch.Error()); }
    if (std::memcmp(magic, "PATCH", 4) == 0) {
        return ApplyIps(source, patch, targetPath, options);
    }
    if (std::memcmp(magic, "BPS1", 4) == 0) {
        return ApplyBps(source, patch, targetPath, options);
    }
    return Result(BinaryPatchStatus::kInvalidPatch);
}

}  // namespace stirling
