#include "../../StirHex/src/core/BinaryPatch.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

namespace {

void PrintResult(const stirling::BinaryPatchResult& result) {
    std::fwprintf(stdout,
                  L"status=%d system_error=%lu processed=%lld source_size=%lld target_size=%lld source_verified=%d target_verified=%d\n",
                  static_cast<int>(result.status), result.systemError,
                  static_cast<long long>(result.bytesProcessed),
                  static_cast<long long>(result.sourceSize),
                  static_cast<long long>(result.targetSize),
                  result.sourceVerified ? 1 : 0,
                  result.targetVerified ? 1 : 0);
    if (!result.keptTempPath.empty()) {
        std::fwprintf(stdout, L"kept_temp=%ls\n", result.keptTempPath.c_str());
    }
}

bool Equal(const wchar_t* value, const wchar_t* expected) {
    return value != nullptr && std::wcscmp(value, expected) == 0;
}

bool ParseOptions(int argc, wchar_t** argv, int optionStart,
                  stirling::BinaryPatchOptions& options) {
    if (argc == optionStart) { return true; }
    if (argc != optionStart + 2 || !Equal(argv[optionStart], L"--cancel-after")) {
        return false;
    }
    wchar_t* end = nullptr;
    errno = 0;
    const long long limit = _wcstoi64(argv[optionStart + 1], &end, 10);
    if (errno != 0 || end == argv[optionStart + 1] || *end != L'\0' || limit <= 0) {
        return false;
    }
    options.progress = [limit](stirling::FileOffset processed,
                               stirling::FileOffset /*total*/) {
        return processed < static_cast<stirling::FileOffset>(limit);
    };
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if ((argc == 6 || argc == 8) && Equal(argv[1], L"generate")) {
        stirling::BinaryPatchFormat format;
        if (Equal(argv[2], L"ips")) {
            format = stirling::BinaryPatchFormat::kIps;
        } else if (Equal(argv[2], L"bps")) {
            format = stirling::BinaryPatchFormat::kBps;
        } else {
            std::fwprintf(stderr, L"unknown patch format: %ls\n", argv[2]);
            return 2;
        }
        stirling::BinaryPatchOptions options;
        if (!ParseOptions(argc, argv, 6, options)) {
            std::fwprintf(stderr, L"invalid generate options\n");
            return 2;
        }
        const auto result = stirling::GenerateBinaryPatch(
            argv[3], argv[4], argv[5], format, options);
        PrintResult(result);
        return result.Ok() ? 0 : 1;
    }
    if ((argc == 5 || argc == 7) && Equal(argv[1], L"apply")) {
        stirling::BinaryPatchOptions options;
        if (!ParseOptions(argc, argv, 5, options)) {
            std::fwprintf(stderr, L"invalid apply options\n");
            return 2;
        }
        const auto result = stirling::ApplyBinaryPatch(
            argv[2], argv[3], argv[4], options);
        PrintResult(result);
        return result.Ok() ? 0 : 1;
    }
    std::fwprintf(stderr,
                  L"usage: binary_patch_cli.exe generate <ips|bps> SOURCE TARGET PATCH\n"
                  L"       binary_patch_cli.exe apply SOURCE PATCH TARGET\n"
                  L"       append --cancel-after BYTES to either command\n");
    return 2;
}
