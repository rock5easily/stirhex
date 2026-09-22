#pragma once

#include "CoreTypes.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace stirling {

// File-level IPS/BPS patch support.  Patch generation/application deliberately
// operates on stable disk files and never reads the in-memory document blocks.
enum class BinaryPatchFormat { kIps, kBps };

enum class BinaryPatchStatus {
    kOk = 0,
    kInvalidArgument,
    kOpenFailed,
    kReadFailed,
    kWriteFailed,
    kInvalidPatch,
    kSourceMismatch,
    kTargetMismatch,
    kCancelled,
    kLimitExceeded,
    kOutOfMemory,
    kConflict,
    kUnsupported,
};

struct BinaryPatchResult {
    BinaryPatchStatus status = BinaryPatchStatus::kOk;
    unsigned long systemError = 0;
    FileOffset bytesProcessed = 0;
    FileOffset sourceSize = 0;
    FileOffset targetSize = 0;
    bool sourceVerified = false;
    bool targetVerified = false;
    // A non-empty path means a safe publish failed after the temporary file
    // was fully written.  The caller owns cleanup/notification of this file.
    std::wstring keptTempPath;

    bool Ok() const { return status == BinaryPatchStatus::kOk; }
};

struct BinaryPatchLimits {
    // 16 MiB is the product's generation policy for interoperable basic IPS;
    // the format itself has a 24-bit offset and a 16-bit record length.
    static constexpr FileOffset kIpsGenerateMaxBytes = 16ll * 1024ll * 1024ll;
    // Applied IPS accepts larger source files; this is a product safety cap,
    // separate from the IPS format limit and intentionally configurable.
    FileOffset ipsApplyMaxSourceBytes = 512ll * 1024ll * 1024ll;
    FileOffset ipsApplyMaxTargetBytes = 512ll * 1024ll * 1024ll;
    FileOffset bpsMaxSourceBytes = 512ll * 1024ll * 1024ll;
    FileOffset bpsMaxTargetBytes = 512ll * 1024ll * 1024ll;
    // Generation is intentionally lower than apply: the verified
    // generate->apply->compare pipeline needs two temporary target copies.
    FileOffset bpsGenerateMaxSourceBytes = 256ll * 1024ll * 1024ll;
    FileOffset bpsGenerateMaxTargetBytes = 256ll * 1024ll * 1024ll;
    FileOffset maxPatchBytes = 512ll * 1024ll * 1024ll;
    std::uint64_t maxIpsRecords = 4ull * 1024ull * 1024ull;
    size_t ipsRecordMemoryBytes = 64u * 1024u * 1024u;
    FileOffset ipsExpandedBytes = 256ll * 1024ll * 1024ll;
    std::uint64_t maxBpsInstructions = 16ull * 1024ull * 1024ull;
    FileOffset maxTemporaryBytes = 1024ll * 1024ll * 1024ll;
    // Bound the in-memory source window used by BPS SourceCopy matching.
    size_t bpsHashMemoryBytes = 64u * 1024u * 1024u;
    // Bound the number of candidate hash hits tested for one target position.
    std::uint64_t bpsHashSearchLimit = 4096;
    // Bound the total source bytes compared while probing hash candidates.
    std::uint64_t bpsHashCompareBytes = 256ull * 1024ull * 1024ull;
};

// Return false from progress to cancel.  It is called periodically with the
// number of input/output bytes processed and the expected total (if known).
using BinaryPatchProgress = std::function<bool(FileOffset processed, FileOffset total)>;

struct BinaryPatchOptions {
    BinaryPatchLimits limits;
    BinaryPatchProgress progress;
};

// Create an IPS (same-size, basic records only) or BPS patch from two stable
// disk files.  The result is written to patchPath through StreamFileWriter.
BinaryPatchResult GenerateBinaryPatch(const wchar_t* sourcePath,
                                      const wchar_t* targetPath,
                                      const wchar_t* patchPath,
                                      BinaryPatchFormat format,
                                      const BinaryPatchOptions& options = {});

// Apply an IPS/BPS patch to sourcePath and safely publish targetPath.
// The patch format is selected by its header (IPS or BPS); callers may pass
// either extension.  Existing target files are never modified until the
// complete result has been validated.
BinaryPatchResult ApplyBinaryPatch(const wchar_t* sourcePath,
                                   const wchar_t* patchPath,
                                   const wchar_t* targetPath,
                                   const BinaryPatchOptions& options = {});

}  // namespace stirling
