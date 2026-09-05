// Utf16Text 実装（Utf16Text.h 参照）。
#include "core/Utf16Text.h"

namespace stirling {

namespace {

// p[0..1] を 1 コード単位として読む。
inline unsigned int ReadUnit(const unsigned char* p, bool bigEndian) {
    return bigEndian ? ((static_cast<unsigned int>(p[0]) << 8) | p[1])
                     : (static_cast<unsigned int>(p[0]) | (static_cast<unsigned int>(p[1]) << 8));
}

inline void PushUnit(unsigned int unit, bool bigEndian, std::vector<unsigned char>& out) {
    if (bigEndian) {
        out.push_back(static_cast<unsigned char>((unit >> 8) & 0xFF));
        out.push_back(static_cast<unsigned char>(unit & 0xFF));
    } else {
        out.push_back(static_cast<unsigned char>(unit & 0xFF));
        out.push_back(static_cast<unsigned char>((unit >> 8) & 0xFF));
    }
}

}  // namespace

Utf16Decoded DecodeUtf16(const unsigned char* p, size_t n, bool bigEndian) {
    Utf16Decoded r;
    if (p == nullptr || n == 0) { r.length = 1; return r; }
    if (n == 1) { r.length = 1; return r; }   // 端数 1 バイトは 1 セルの不正

    const unsigned int unit = ReadUnit(p, bigEndian);
    if (!IsUtf16Surrogate(unit)) {
        r.ok = true;
        r.codePoint = unit;
        return r;                              // length = 2
    }
    if (IsUtf16LowSurrogate(unit)) { return r; }   // 単独の下位サロゲート＝不正
    // 上位サロゲート。続く 2 バイトが下位サロゲートならペアとして 1 文字になる。
    if (n < 4) {
        // ペアの相方がバッファの外にある。ここで不正と決めると、次の窓で読み直した
        //   ときにセル数が変わるため、呼び出し側へ知らせる。
        r.truncated = true;
        return r;
    }
    const unsigned int low = ReadUnit(p + 2, bigEndian);
    if (!IsUtf16LowSurrogate(low)) { return r; }   // 相方が無い＝不正

    r.ok = true;
    r.codePoint = 0x10000u + ((unit - 0xD800u) << 10) + (low - 0xDC00u);
    r.length = 4;
    return r;
}

bool EncodeUtf16(unsigned int cp, bool bigEndian, std::vector<unsigned char>& out) {
    if (IsUtf16Surrogate(cp) || cp > 0x10FFFF) { return false; }
    if (cp < 0x10000) {
        PushUnit(cp, bigEndian, out);
        return true;
    }
    const unsigned int v = cp - 0x10000u;
    PushUnit(0xD800u + (v >> 10), bigEndian, out);
    PushUnit(0xDC00u + (v & 0x3FFu), bigEndian, out);
    return true;
}

std::vector<unsigned char> Utf16FromWide(const wchar_t* w, size_t length, bool bigEndian) {
    std::vector<unsigned char> out;
    if (w == nullptr) { return out; }
    for (size_t i = 0; i < length; ++i) {
        const unsigned int u = static_cast<unsigned int>(w[i]) & 0xFFFFu;
        if (IsUtf16HighSurrogate(u)) {
            if (i + 1 < length) {
                const unsigned int low = static_cast<unsigned int>(w[i + 1]) & 0xFFFFu;
                if (IsUtf16LowSurrogate(low)) {
                    PushUnit(u, bigEndian, out);
                    PushUnit(low, bigEndian, out);
                    ++i;
                    continue;
                }
            }
            continue;   // 対になっていない上位サロゲートは読み飛ばす
        }
        if (IsUtf16LowSurrogate(u)) { continue; }   // 対になっていない下位サロゲート
        PushUnit(u, bigEndian, out);
    }
    return out;
}

int Utf16CarryBytesAt(const unsigned char* buf, size_t size, size_t startIndex, bool bigEndian) {
    if (buf == nullptr || startIndex < 2 || startIndex + 2 > size) { return 0; }
    const unsigned int cur = ReadUnit(buf + startIndex, bigEndian);
    if (!IsUtf16LowSurrogate(cur)) { return 0; }
    const unsigned int prev = ReadUnit(buf + startIndex - 2, bigEndian);
    return IsUtf16HighSurrogate(prev) ? 2 : 0;
}

}  // namespace stirling
