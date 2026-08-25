// SettingsCodec 実装（MFC / Win32 非依存。仕様は SettingsCodec.h を参照）。
#include "app/SettingsCodec.h"

#include <cstdint>

namespace stirling {
namespace settings {

namespace {
const char kDigits[] = "0123456789ABCDEF";
const int  kMaxDigits = 16;   // 64bit = 16 桁

// 16進1文字を 0..15 へ。16進でなければ -1。
//   ナロー/ワイド共通。ASCII 層なので、ワイドでも比較する文字コードは同じでよい。
template <typename Ch>
int HexValue(Ch c) {
    if (c >= Ch('0') && c <= Ch('9')) { return static_cast<int>(c - Ch('0')); }
    if (c >= Ch('a') && c <= Ch('f')) { return static_cast<int>(c - Ch('a')) + 10; }
    if (c >= Ch('A') && c <= Ch('F')) { return static_cast<int>(c - Ch('A')) + 10; }
    return -1;
}

// 2の補数のビット列をそのまま符号なし16進で表記する（負値も往復可能）。
template <typename Str>
Str FormatHex(FileOffset value) {
    typedef typename Str::value_type Ch;
    std::uint64_t bits = static_cast<std::uint64_t>(value);
    if (bits == 0) { return Str(1, Ch('0')); }
    Ch buf[kMaxDigits];
    int n = 0;
    while (bits != 0) {
        buf[n++] = static_cast<Ch>(kDigits[bits & 0xF]);
        bits >>= 4;
    }
    Str s;
    s.reserve(static_cast<std::size_t>(n));
    for (int i = n - 1; i >= 0; --i) { s.push_back(buf[i]); }
    return s;
}

template <typename Ch>
bool ParseHex(const Ch* text, FileOffset& out) {
    if (text == nullptr) { return false; }
    const Ch* p = text;
    if (p[0] == Ch('0') && (p[1] == Ch('x') || p[1] == Ch('X'))) { p += 2; }   // 手編集への寛容（省略可）
    std::uint64_t bits = 0;
    int digits = 0;
    for (; *p != Ch('\0'); ++p) {
        const int v = HexValue(*p);
        if (v < 0) { return false; }              // 16進以外（空白・符号を含む）は不正
        if (++digits > kMaxDigits) { return false; }   // 64bit に収まらない桁数は不正
        bits = (bits << 4) | static_cast<std::uint64_t>(v);
    }
    if (digits == 0) { return false; }            // 空文字列・接頭辞のみ
    out = static_cast<FileOffset>(bits);
    return true;
}
}

std::string FormatOffsetHex(FileOffset value) {
    return FormatHex<std::string>(value);
}

std::wstring FormatOffsetHexW(FileOffset value) {
    return FormatHex<std::wstring>(value);
}

bool ParseOffsetHex(const char* text, FileOffset& out) {
    return ParseHex(text, out);
}

bool ParseOffsetHex(const wchar_t* text, FileOffset& out) {
    return ParseHex(text, out);
}

}  // namespace settings
}  // namespace stirling
