// Utf8Text 実装（Utf8Text.h 参照）。
#include "core/Utf8Text.h"

namespace stirling {

namespace {

// 列の長さごとの最小コードポイント（これ未満は overlong）。
const unsigned int kMinForLen[5] = {0, 0, 0x80, 0x800, 0x10000};

bool IsSurrogate(unsigned int cp) { return cp >= 0xD800 && cp <= 0xDFFF; }

}  // namespace

Utf8Decoded DecodeUtf8(const unsigned char* p, size_t n) {
    Utf8Decoded r;
    if (p == nullptr || n == 0) { return r; }

    const int len = Utf8SeqLen(p[0]);
    if (len == 0) { return r; }        // 後続バイト単独 / 常に不正なリードバイト
    if (len == 1) {
        r.ok = true;
        r.codePoint = p[0];
        return r;
    }
    if (n < static_cast<size_t>(len)) {
        // 列の途中でバッファが尽きた。ここで '.' 1 セルに落とすと、次の窓で同じ
        // 文字を頭から読み直したときにセル数が変わる。呼び出し側へ知らせる。
        r.truncated = true;
        return r;
    }

    unsigned int cp = static_cast<unsigned int>(p[0]) & (0x7Fu >> len);
    for (int i = 1; i < len; ++i) {
        if (!IsUtf8Continuation(p[i])) { return r; }   // 後続バイトが足りない＝不正
        cp = (cp << 6) | (static_cast<unsigned int>(p[i]) & 0x3Fu);
    }
    if (cp < kMinForLen[len]) { return r; }            // overlong
    if (IsSurrogate(cp) || cp > 0x10FFFF) { return r; }

    r.ok = true;
    r.codePoint = cp;
    r.length = len;
    return r;
}

bool EncodeUtf8(unsigned int cp, std::vector<unsigned char>& out) {
    if (IsSurrogate(cp) || cp > 0x10FFFF) { return false; }
    if (cp < 0x80) {
        out.push_back(static_cast<unsigned char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<unsigned char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<unsigned char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<unsigned char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<unsigned char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<unsigned char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<unsigned char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<unsigned char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<unsigned char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<unsigned char>(0x80 | (cp & 0x3F)));
    }
    return true;
}

std::vector<unsigned char> Utf8FromWide(const wchar_t* w, size_t length) {
    std::vector<unsigned char> out;
    if (w == nullptr) { return out; }
    for (size_t i = 0; i < length; ++i) {
        unsigned int cp = static_cast<unsigned int>(w[i]);
        if (cp >= 0xD800 && cp <= 0xDBFF) {          // 上位サロゲート
            if (i + 1 < length) {
                const unsigned int lo = static_cast<unsigned int>(w[i + 1]);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    ++i;
                } else {
                    continue;   // 対になっていない上位サロゲートは捨てる
                }
            } else {
                continue;
            }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            continue;           // 対になっていない下位サロゲートは捨てる
        }
        EncodeUtf8(cp, out);
    }
    return out;
}

int Utf8CarryBytesAt(const unsigned char* buf, size_t size, size_t startIndex) {
    if (buf == nullptr || startIndex == 0 || startIndex >= size) { return 0; }
    if (!IsUtf8Continuation(buf[startIndex])) { return 0; }   // 文字の先頭なら持ち越し無し

    // 後続バイトは最大 3 個。それを超えて遡っても妥当な列にはならない。
    for (size_t back = 1; back <= 3 && back <= startIndex; ++back) {
        const size_t lead = startIndex - back;
        const unsigned char b = buf[lead];
        if (IsUtf8Continuation(b)) { continue; }               // まだ列の内側
        const Utf8Decoded d = DecodeUtf8(buf + lead, size - lead);
        if (!d.ok) { return 0; }                               // 不正な列＝各バイトが独立
        const size_t end = lead + static_cast<size_t>(d.length);
        if (end <= startIndex) { return 0; }                   // 列は startIndex に届かない
        return static_cast<int>(end - startIndex);             // 残りの後続バイト数
    }
    return 0;
}

}  // namespace stirling
