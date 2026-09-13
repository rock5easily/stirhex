// Checksum — BlockList の指定範囲を逐次計算する読み取り専用処理（Issue #226）。
// CRC-32 (ISO-HDLC) と Windows CNG の MD5・SHA-1・SHA-256 に対応する。
#pragma once

#include "BlockList.h"
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <cstdint>

namespace stirling {

enum class ChecksumAlgorithm { Crc32, Md5, Sha1, Sha256 };
enum class ChecksumState { Idle, Running, Complete, Cancelled, InvalidRange, ReadError, CryptoError };

// 呼び出し側は完了・中止までリストの寿命と内容を維持すること。
// 対象範囲と同容量のメモリ確保は行わない。
class Checksum {
public:
    ~Checksum();
    Checksum() = default;
    Checksum(const Checksum&) = delete;
    Checksum& operator=(const Checksum&) = delete;

    ChecksumState Start(const BlockList& list, FileOffset start, FileOffset length,
                        ChecksumAlgorithm algorithm);
    ChecksumState Step(size_t byteBudget);
    void Cancel();
    ChecksumState State() const { return state_; }
    FileOffset Processed() const { return processed_; }
    FileOffset Length() const { return length_; }
    const char* Hex() const { return hex_.data(); } // 完了するまでは空文字列。
    NTSTATUS ErrorCode() const { return error_; }

private:
    void CloseHash();
    ChecksumState Fail(ChecksumState state, NTSTATUS error = 0);
    ChecksumState Finish();
    const BlockList* list_ = nullptr;
    const BlockNode* node_ = nullptr;
    int offset_ = 0;
    FileOffset processed_ = 0, length_ = 0;
    ChecksumAlgorithm algorithm_ = ChecksumAlgorithm::Sha256;
    ChecksumState state_ = ChecksumState::Idle;
    std::uint32_t crc_ = 0xFFFFFFFFu;
    BCRYPT_ALG_HANDLE provider_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    NTSTATUS error_ = 0;
    std::array<char, 65> hex_{};
};
} // namespace stirling
