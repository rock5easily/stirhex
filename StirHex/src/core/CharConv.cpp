// CharConv 実装。原 FUN_0045fecb（charset 別 char 配列文字列化）と、その EUC 分岐
//   FUN_004603fa（原 FUN_0046903d = JIS→SJIS）を移植する。
#include "core/CharConv.h"
#include "core/Utf8Text.h"    // UTF-8 復号（Issue #98）
#include "core/Cp932Text.h"   // CP932 → ワイド（Issue #107）

#include <windows.h>

namespace stirling {
namespace {

// CP932 固定。システム ANSI コードページに依存しないことを名前で明示する。
constexpr UINT kCp932 = 932;

// EBCDIC / EBCIDK の 256 バイト変換表（原 DAT_004b56a8 / DAT_004b57a8）。
//   添字=元バイト、値=表示バイト（SJIS/ASCII）。0 は無効（'.' 表示）。
const char* const kEbcdicHex =
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000020000000000000000000002e3c282b7c2600000000000000000021242a293b002d2f0000000000000000002c255f3e3f000000000000000000603a2340273d2200616263646566676869000000000000006a6b6c6d6e6f707172000000000000007e737475767778797a000000000000000000000000000000000000000000007b4142434445464748490000000000007d4a4b4c4d4e4f5051520000000000005c00535455565758595a00000000000030313233343536373839000000000000";
const char* const kEbcidkHex =
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000020a1a2a3a4a5a6a7a8a9002e3c282b7c26aaabacadaeaf00b000215c2a293b002d2f0000000000000000002c255f3e3f000000000000000000603a2340273d2200b1b2b3b4b5b6b7b8b9ba00bbbcbdbebfc0c1c2c3c4c5c6c7c8c90000cacbcc0000cdcecfd0d1d2d3d4d500d6d7d8d900000000000000000000dadbdcdddedf00414243444546474849000000000000004a4b4c4d4e4f5051520000000000002400535455565758595a00000000000030313233343536373839000000000000";

int HexNyb(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

void DecodeHex256(const char* hex, unsigned char* out) {
    for (int i = 0; i < 256; ++i) {
        out[i] = static_cast<unsigned char>((HexNyb(hex[i * 2]) << 4) | HexNyb(hex[i * 2 + 1]));
    }
}

const unsigned char* EbcdicTable() {
    static unsigned char t[256];
    static bool init = false;
    if (!init) { DecodeHex256(kEbcdicHex, t); init = true; }
    return t;
}

const unsigned char* EbcidkTable() {
    static unsigned char t[256];
    static bool init = false;
    if (!init) { DecodeHex256(kEbcidkHex, t); init = true; }
    return t;
}

// JIS（EUC−0x8080）→ SJIS 変換（原 FUN_0046903d。ロケール判定は JP 前提で省略）。
//   非漢字域は 0 を返す。
unsigned int JisToSjis(unsigned int jis) {
    unsigned int hi = (jis >> 8) & 0xff;
    unsigned int lo = jis & 0xff;
    if (hi > 0x20 && hi < 0x7f && lo > 0x20 && lo < 0x7f) {
        if ((jis & 0x100) == 0)      lo += 0x7e;   // 上位バイトが偶数
        else if (lo < 0x60)          lo += 0x1f;
        else                         lo += 0x20;
        unsigned int t = (hi - 0x21) >> 1;
        unsigned int s = t + 0x81;
        if (s > 0x9f) s = t + 0xc1;
        return (s << 8) | lo;
    }
    return 0;
}

// EUC-JP を SJIS バイト列へ変換して連結（原 FUN_004603fa 忠実移植）。
//   主プレーン対（0xa1..0xfe, 0xa1..0xfe）→ JisToSjis で SJIS 2 バイト。
//   0x8e（半角カナシフト）＋（0xa1..0xdf）→ カナ 1 バイト。
//   対を成さない主プレーン先頭は '.'、EUC 外のバイトは印字可能 ASCII のみ通し
//   それ以外は '.'（原 0x46054a-0x460569 の分岐）。
//   0x8e がバッファ末尾に来た場合、原はそこで変換を打ち切る（-1 を返す）ため、
//   その 1 バイトは出力しない。
std::string FormatEuc(const unsigned char* p, int n) {
    std::string s;
    int i = 0;
    while (i < n) {
        const unsigned char b1 = p[i];
        if (b1 >= 0xa1 && b1 <= 0xfe) {                 // 主プレーン先頭
            if (i + 1 < n && p[i + 1] >= 0xa1 && p[i + 1] <= 0xfe) {
                const unsigned int jis =
                    ((static_cast<unsigned int>(b1) << 8) | p[i + 1]) - 0x8080;
                const unsigned int sj = JisToSjis(jis);
                s.push_back(static_cast<char>((sj >> 8) & 0xff));
                s.push_back(static_cast<char>(sj & 0xff));
                i += 2;
            } else {
                s.push_back('.');                       // 対にならない先頭は '.'
                ++i;
            }
        } else if (b1 == 0x8e) {                        // 半角カナ単一シフト
            if (i + 1 >= n) { break; }                  // 末尾の 0x8e は打ち切り
            if (p[i + 1] >= 0xa1 && p[i + 1] <= 0xdf) {
                s.push_back(static_cast<char>(p[i + 1]));
                i += 2;
            } else {
                s.push_back(static_cast<char>(b1));     // シフト対象外は 0x8e を生で
                ++i;
            }
        } else {                                        // EUC 外: 印字可能 ASCII のみ通す
            s.push_back((b1 >= 0x20 && b1 <= 0x7e) ? static_cast<char>(b1) : '.');
            ++i;
        }
    }
    return s;
}

}  // namespace

// 文字セット 0..5 の CP932 バイト列表現（原 FUN_0045fecb 忠実移植）。
//   charset 6(UTF-8) はここへ来ない。CP932 へ写すと CP932 に無い文字が落ちるため、
//   FormatStructCharArrayW が復号結果からワイドを直接組み立てる。
std::string FormatStructCharArrayCp932(int charset, const unsigned char* p, int n) {
    if (p == nullptr || n <= 0) return std::string();
    if (n > 0x100) n = 0x100;   // 原の 256 バイト上限

    switch (charset) {
    case 0: {   // ASCII: 非印字は '.'
        std::string s;
        for (int i = 0; i < n; ++i) {
            const unsigned char b = p[i];
            s.push_back((b >= 0x20 && b <= 0x7e) ? static_cast<char>(b) : '.');
        }
        return s;
    }
    case 2:     // EUC-JP
        return FormatEuc(p, n);
    case 3: {   // Unicode(UTF-16LE) → CP932
        // [byte層] 戻り値は CP932 バイト列として表示側（StructBar）が解釈するため、
        //   コードページは 932 固定にする。原は CP_ACP を使うが、日本語環境では
        //   ACP == 932 で等価であり、非日本語環境での化けを避ける修正になる。
        //   詳細: analysis_artifacts/docs/20_unicode_layering.md §4.2
        const int wn = n / 2;
        if (wn <= 0) return std::string();
        const int mb = ::WideCharToMultiByte(kCp932, 0, reinterpret_cast<const wchar_t*>(p),
                                             wn, nullptr, 0, ".", nullptr);
        if (mb <= 0) return std::string();
        std::string s(mb, '\0');
        ::WideCharToMultiByte(kCp932, 0, reinterpret_cast<const wchar_t*>(p), wn,
                              &s[0], mb, ".", nullptr);
        return s;
    }
    case 4:
    case 5: {   // EBCDIC / EBCIDK: 変換表で写像、無効は '.'
        const unsigned char* tbl = (charset == 4) ? EbcdicTable() : EbcidkTable();
        std::string s;
        for (int i = 0; i < n; ++i) {
            const unsigned char t = tbl[p[i]];
            s.push_back(t ? static_cast<char>(t) : '.');
        }
        return s;
    }
    case 1:     // SJIS: 生バイトをそのまま連結（フォントが描画）
    default: {
        std::string s;
        for (int i = 0; i < n; ++i) s.push_back(static_cast<char>(p[i]));
        return s;
    }
    }
}

// UTF-8 の列をワイドへ直接組み立てる（Issue #107）。
//   CP932 を経由しないので、CP932 に無い文字（ハングル・簡体字・絵文字など）も残る。
//   不正・不完全な列は 1 バイト = 1 文字の '.'（文字欄と同じ規則）。
//   文字欄と違ってセル整列の制約が無いため、バイト数ぶんの空白詰めはしない。
std::wstring FormatUtf8Wide(const unsigned char* p, int n) {
    std::wstring w;
    int i = 0;
    while (i < n) {
        const Utf8Decoded d = DecodeUtf8(p + i, static_cast<size_t>(n - i));
        if (!d.ok) { w.push_back(L'.'); ++i; continue; }
        if (d.codePoint < 0x10000) {
            w.push_back(static_cast<wchar_t>(d.codePoint));
        } else {
            const unsigned int v = d.codePoint - 0x10000;
            w.push_back(static_cast<wchar_t>(0xD800 + (v >> 10)));
            w.push_back(static_cast<wchar_t>(0xDC00 + (v & 0x3FF)));
        }
        i += d.length;
    }
    return w;
}

std::wstring FormatStructCharArrayW(int charset, const unsigned char* p, int n) {
    if (p == nullptr || n <= 0) { return std::wstring(); }
    if (n > 0x100) { n = 0x100; }   // 原の 256 バイト上限
    if (charset == 6) { return FormatUtf8Wide(p, n); }
    // 0..5 は従来どおり CP932 バイト列を作ってからワイドへ。表示直前に StructBar が
    //   していた変換をここへ移しただけで、出力は変わらない。
    const std::string mb = FormatStructCharArrayCp932(charset, p, n);
    return WideFromCp932(mb.c_str(), static_cast<int>(mb.size()));
}

}  // namespace stirling
