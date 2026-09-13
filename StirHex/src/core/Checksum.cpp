// Checksum 実装（Issue #226）。CRC-32 と CNG の逐次計算・リソース管理。
#include "Checksum.h"
#include <algorithm>

namespace stirling {
namespace {
constexpr std::array<std::uint32_t, 256> CrcTable() {
    std::array<std::uint32_t, 256> table{};
    for (unsigned i = 0; i < 256; ++i) {
        std::uint32_t value = i;
        for (int bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^ ((value & 1) ? 0xEDB88320u : 0);
        table[i] = value;
    }
    return table;
}
constexpr auto kCrcTable = CrcTable();
}

Checksum::~Checksum() { CloseHash(); }
void Checksum::CloseHash() {
    if (hash_) { BCryptDestroyHash(hash_); hash_ = nullptr; }
    if (provider_) { BCryptCloseAlgorithmProvider(provider_, 0); provider_ = nullptr; }
}
ChecksumState Checksum::Fail(ChecksumState state, NTSTATUS error) {
    CloseHash();
    hex_.fill(0);
    error_ = error;
    return state_ = state;
}
void Checksum::Cancel() {
    if (state_ == ChecksumState::Running) Fail(ChecksumState::Cancelled);
}

ChecksumState Checksum::Start(const BlockList& list, FileOffset start,
                              FileOffset length, ChecksumAlgorithm algorithm) {
    CloseHash();
    hex_.fill(0);
    error_ = 0;
    processed_ = 0;
    length_ = length;
    algorithm_ = algorithm;
    crc_ = 0xFFFFFFFFu;
    list_ = &list;
    node_ = nullptr;
    offset_ = 0;
    const FileOffset total = list.GetTotalLength();
    if (start < 0 || length < 0 || start > total || length > total - start)
        return Fail(ChecksumState::InvalidRange);

    LPCWSTR name = nullptr;
    switch (algorithm) {
    case ChecksumAlgorithm::Crc32: break;
    case ChecksumAlgorithm::Md5: name = BCRYPT_MD5_ALGORITHM; break;
    case ChecksumAlgorithm::Sha1: name = BCRYPT_SHA1_ALGORITHM; break;
    case ChecksumAlgorithm::Sha256: name = BCRYPT_SHA256_ALGORITHM; break;
    default: return Fail(ChecksumState::CryptoError, static_cast<NTSTATUS>(0xC000000Du));
    }
    if (name) {
        NTSTATUS status = BCryptOpenAlgorithmProvider(&provider_, name, nullptr, 0);
        if (status < 0) return Fail(ChecksumState::CryptoError, status);
        // Windows 7 以降はバッファ省略時に CNG がハッシュ用領域を管理する。
        status = BCryptCreateHash(provider_, &hash_, nullptr, 0, nullptr, 0, 0);
        if (status < 0) return Fail(ChecksumState::CryptoError, status);
    }
    state_ = ChecksumState::Running;
    if (length == 0) return Finish();
    node_ = list.GetHead();
    while (node_ && start >= node_->usedLen) {
        start -= node_->usedLen;
        node_ = list.GetNext(node_);
    }
    offset_ = static_cast<int>(start);
    return state_;
}

ChecksumState Checksum::Step(size_t byteBudget) {
    if (state_ != ChecksumState::Running) return state_;
    while (byteBudget && processed_ < length_) {
        if (!node_) return Fail(ChecksumState::ReadError);
        if (node_->usedLen < 0 || node_->usedLen > node_->capacity ||
            offset_ < 0 || offset_ > node_->usedLen)
            return Fail(ChecksumState::ReadError);
        const int available = node_->usedLen - offset_;
        if (!available) {
            node_ = list_->GetNext(node_);
            offset_ = 0;
            continue;
        }
        if (!node_->data) return Fail(ChecksumState::ReadError);
        const size_t count = (std::min)(byteBudget, static_cast<size_t>(
            (std::min)(length_ - processed_, static_cast<FileOffset>(available))));
        const auto* data = node_->data + offset_;
        if (algorithm_ == ChecksumAlgorithm::Crc32) {
            for (size_t i = 0; i < count; ++i)
                crc_ = (crc_ >> 8) ^ kCrcTable[(crc_ ^ data[i]) & 0xFF];
        } else {
            const NTSTATUS status = BCryptHashData(hash_, const_cast<PUCHAR>(data),
                                                   static_cast<ULONG>(count), 0);
            if (status < 0) return Fail(ChecksumState::CryptoError, status);
        }
        offset_ += static_cast<int>(count);
        processed_ += static_cast<FileOffset>(count);
        byteBudget -= count;
    }
    return processed_ == length_ ? Finish() : state_;
}

ChecksumState Checksum::Finish() {
    std::array<unsigned char, 32> digest{};
    ULONG size = 0;
    if (algorithm_ == ChecksumAlgorithm::Crc32) {
        const std::uint32_t value = crc_ ^ 0xFFFFFFFFu;
        size = 4;
        for (unsigned i = 0; i < 4; ++i)
            digest[i] = static_cast<unsigned char>(value >> (24 - 8 * i));
    } else {
        ULONG written = 0;
        NTSTATUS status = BCryptGetProperty(hash_, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&size), sizeof(size), &written, 0);
        if (status < 0) return Fail(ChecksumState::CryptoError, status);
        if (size > digest.size())
            return Fail(ChecksumState::CryptoError, static_cast<NTSTATUS>(0xC000000Du));
        status = BCryptFinishHash(hash_, digest.data(), size, 0);
        if (status < 0) return Fail(ChecksumState::CryptoError, status);
    }
    constexpr char digits[] = "0123456789ABCDEF";
    for (ULONG i = 0; i < size; ++i) {
        hex_[2 * i] = digits[digest[i] >> 4];
        hex_[2 * i + 1] = digits[digest[i] & 15];
    }
    CloseHash();
    return state_ = ChecksumState::Complete;
}
} // namespace stirling
