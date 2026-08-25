// Cp932Text 実装。CP932 固定のワイド↔バイト列変換。
#include "core/Cp932Text.h"

#include <windows.h>

#include <string.h>
#include <wchar.h>

namespace stirling {
namespace {

// CP932 固定。システム ANSI コードページに依存しないことを型と名前で明示する。
constexpr UINT kCp932 = 932;

}  // namespace

std::wstring WideFromCp932(const char* bytes, int len) {
    if (bytes == nullptr) { return std::wstring(); }
    if (len < 0) { len = static_cast<int>(::strlen(bytes)); }
    if (len == 0) { return std::wstring(); }

    const int n = ::MultiByteToWideChar(kCp932, 0, bytes, len, nullptr, 0);
    if (n <= 0) { return std::wstring(); }

    std::wstring w(static_cast<size_t>(n), L'\0');
    const int got = ::MultiByteToWideChar(kCp932, 0, bytes, len, &w[0], n);
    if (got <= 0) { return std::wstring(); }
    w.resize(static_cast<size_t>(got));
    return w;
}

bool Cp932FromWide(const wchar_t* w, std::string& out, int len) {
    out.clear();
    if (w == nullptr) { return true; }
    if (len < 0) { len = static_cast<int>(::wcslen(w)); }
    if (len == 0) { return true; }

    // WC_NO_BEST_FIT_CHARS + usedDefaultChar で「表現できない文字」を検出する。
    // 既定文字への best-fit 置換を許すと欠落に気付けないため。
    BOOL usedDefault = FALSE;
    const int n = ::WideCharToMultiByte(kCp932, WC_NO_BEST_FIT_CHARS, w, len,
                                        nullptr, 0, nullptr, &usedDefault);
    if (n <= 0) { return false; }

    std::string a(static_cast<size_t>(n), '\0');
    usedDefault = FALSE;
    const int got = ::WideCharToMultiByte(kCp932, WC_NO_BEST_FIT_CHARS, w, len,
                                          &a[0], n, nullptr, &usedDefault);
    if (got <= 0 || usedDefault) { return false; }

    a.resize(static_cast<size_t>(got));
    out.swap(a);
    return true;
}

}  // namespace stirling
