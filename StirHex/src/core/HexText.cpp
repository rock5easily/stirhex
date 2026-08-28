// HexText 実装（HexText.h 参照）。
#include "core/HexText.h"

namespace stirling {

namespace {

// 区切り文字か（半角空白 / タブ / CR / LF / カンマ）。
//   ':' や ';' は含めない（HexText.h の設計理由を参照）。
bool IsDelimiter(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L',';
}

// 16進1桁の値（ASCII のみ。非16進は -1）。
int HexDigit(wchar_t c) {
    if (c >= L'0' && c <= L'9') { return static_cast<int>(c - L'0'); }
    if (c >= L'a' && c <= L'f') { return static_cast<int>(c - L'a') + 10; }
    if (c >= L'A' && c <= L'F') { return static_cast<int>(c - L'A') + 10; }
    return -1;
}

HexTextParseResult MakeError(HexTextError error, size_t pos) {
    HexTextParseResult r;
    r.error = error;
    r.errorPos = pos;
    return r;
}

}  // namespace

HexTextParseResult ParseHexText(const wchar_t* text, size_t length,
                                std::vector<unsigned char>& out) {
    out.clear();
    if (text == nullptr) {
        return MakeError(HexTextError::Empty, 0);
    }

    std::vector<unsigned char> bytes;
    size_t i = 0;
    while (i < length) {
        if (IsDelimiter(text[i])) { ++i; continue; }

        const size_t tokenStart = i;
        // 接頭辞 "0x" / "0X" / "\x" / "\X" を除去する。"0x" 単独（後続が16進でない）は
        // 除去せず、そのまま16進数字として解釈させて奇数桁／不正文字で弾く。
        if (i + 2 < length && (text[i] == L'0' || text[i] == L'\\') &&
            (text[i + 1] == L'x' || text[i + 1] == L'X') && HexDigit(text[i + 2]) >= 0) {
            i += 2;
        }

        const size_t digitStart = i;
        while (i < length && !IsDelimiter(text[i])) {
            const int d = HexDigit(text[i]);
            if (d < 0) {
                out.clear();
                return MakeError(HexTextError::InvalidChar, i);
            }
            ++i;
        }
        const size_t digitCount = i - digitStart;
        if ((digitCount % 2) != 0) {
            out.clear();
            return MakeError(HexTextError::OddDigits, tokenStart);
        }
        bytes.reserve(bytes.size() + digitCount / 2);
        for (size_t p = digitStart; p < i; p += 2) {
            const int hi = HexDigit(text[p]);
            const int lo = HexDigit(text[p + 1]);
            bytes.push_back(static_cast<unsigned char>((hi << 4) | lo));
        }
    }

    if (bytes.empty()) {
        return MakeError(HexTextError::Empty, 0);
    }
    out.swap(bytes);
    return HexTextParseResult();
}

}  // namespace stirling
