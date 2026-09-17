// HexPattern 実装（HexPattern.h 参照）。
#include "core/HexPattern.h"

namespace stirling {

namespace {

// 16進1桁の値（ASCII のみ。非16進は -1）。
int HexDigit(wchar_t c) {
    if (c >= L'0' && c <= L'9') { return static_cast<int>(c - L'0'); }
    if (c >= L'a' && c <= L'f') { return static_cast<int>(c - L'a') + 10; }
    if (c >= L'A' && c <= L'F') { return static_cast<int>(c - L'A') + 10; }
    return -1;
}

bool IsTrimmed(wchar_t c) { return c == L' ' || c == L'\t'; }

// 2文字の単位を1バイト分として追加する。不正な単位なら false。
bool AppendUnit(wchar_t hiChar, wchar_t loChar, bool allowWildcard,
                std::vector<unsigned char>& bytes, std::vector<unsigned char>& wildcard) {
    if (hiChar == L'?' && loChar == L'?') {
        if (!allowWildcard) { return false; }
        bytes.push_back(0);
        wildcard.push_back(1);
        return true;
    }
    const int hi = HexDigit(hiChar);
    const int lo = HexDigit(loChar);
    if (hi < 0 || lo < 0) { return false; }
    bytes.push_back(static_cast<unsigned char>((hi << 4) | lo));
    wildcard.push_back(0);
    return true;
}

}  // namespace

bool ParseHexPattern(const wchar_t* text, size_t length, bool allowWildcard, HexPattern& out) {
    out = HexPattern();
    if (text == nullptr) { return false; }

    size_t first = 0;
    size_t last = length;
    while (first < last && IsTrimmed(text[first])) { ++first; }
    while (last > first && IsTrimmed(text[last - 1])) { --last; }
    if (first == last) { return false; }

    bool hasSpace = false;
    for (size_t i = first; i < last; ++i) {
        if (text[i] == L' ') { hasSpace = true; break; }
    }

    std::vector<unsigned char> bytes;
    std::vector<unsigned char> wildcard;
    if (hasSpace) {
        size_t i = first;
        while (i < last) {
            while (i < last && text[i] == L' ') { ++i; }
            if (i >= last) { break; }
            const size_t start = i;
            while (i < last && text[i] != L' ') { ++i; }
            if (i - start != 2) { return false; }
            if (!AppendUnit(text[start], text[start + 1], allowWildcard, bytes, wildcard)) {
                return false;
            }
        }
    } else {
        if (((last - first) % 2) != 0) { return false; }
        for (size_t i = first; i < last; i += 2) {
            if (!AppendUnit(text[i], text[i + 1], allowWildcard, bytes, wildcard)) {
                return false;
            }
        }
    }

    bool anyWildcard = false;
    bool anyFixed = false;
    for (unsigned char w : wildcard) {
        if (w != 0) { anyWildcard = true; } else { anyFixed = true; }
    }
    if (!anyFixed) { return false; }   // 空、またはすべてワイルドカード

    out.bytes.swap(bytes);
    if (anyWildcard) { out.wildcard.swap(wildcard); }
    return true;
}

std::wstring FormatHexPattern(const HexPattern& pattern) {
    static const wchar_t kDigits[] = L"0123456789ABCDEF";
    std::wstring s;
    s.reserve(pattern.Size() * 3);
    for (size_t i = 0; i < pattern.Size(); ++i) {
        if (i != 0) { s += L' '; }
        if (pattern.HasWildcard() && pattern.wildcard[i] != 0) {
            s += L"??";
        } else {
            s += kDigits[pattern.bytes[i] >> 4];
            s += kDigits[pattern.bytes[i] & 0x0F];
        }
    }
    return s;
}

}  // namespace stirling
