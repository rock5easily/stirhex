// Utf8Text — UTF-8 バイト列の復号・符号化と、文字欄の「バイト→表示セル」写像。
//   文字欄の不変条件（1 ソースバイト = 1 表示セル）を UTF-8 でも保つための土台。
//   多バイト文字はバイト数ぶんのセルを占め、グリフが余らせたセルは空白で埋める。
//   MFC / GDI 非依存の core レイヤ（コア機能テストから単体で検証できるようにする）。
//   詳細: analysis_artifacts/docs/20_unicode_layering.md
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace stirling {

// 1 バイトが UTF-8 の後続バイト（10xxxxxx）か。
constexpr bool IsUtf8Continuation(unsigned char b) { return (b & 0xC0) == 0x80; }

// リードバイトが示す列の長さ（1..4）。後続バイトや不正なリードバイトは 0。
//   0xC0/0xC1 は overlong 専用で常に不正、0xF5 以降は U+10FFFF を超えるため不正。
inline int Utf8SeqLen(unsigned char b) {
    if (b < 0x80) { return 1; }
    if (b < 0xC2) { return 0; }   // 0x80-0xBF=後続 / 0xC0,0xC1=overlong
    if (b < 0xE0) { return 2; }
    if (b < 0xF0) { return 3; }
    if (b < 0xF5) { return 4; }
    return 0;
}

// 復号結果。ok が false のときは 1 バイトだけ不正として扱う（length は常に 1 以上）。
struct Utf8Decoded {
    bool ok = false;            // 妥当な列として復号できたか
    unsigned int codePoint = 0; // ok のときのコードポイント
    int length = 1;             // 消費したバイト数（不正時は 1）
    bool truncated = false;     // 列の途中でバッファが尽きた（表示側で継続判断に使う）
};

// p[0..n) の先頭から 1 文字を復号する。
//   overlong / サロゲート（U+D800-U+DFFF）/ U+10FFFF 超 / 後続バイト不足は不正とし、
//   ok=false・length=1 を返す（1 バイトずつ '.' へ落として桁を保つため）。
//   バッファ末尾で列が途切れた場合は truncated=true（呼び出し側が次の窓へ持ち越す）。
Utf8Decoded DecodeUtf8(const unsigned char* p, size_t n);

// コードポイントを UTF-8 へ符号化して out へ追加する。
//   サロゲート単体や U+10FFFF 超は書き込まず false を返す。
bool EncodeUtf8(unsigned int codePoint, std::vector<unsigned char>& out);

// ワイド文字列（UTF-16）を UTF-8 バイト列へ変換する。
//   サロゲートペアは結合して 1 コードポイントとして扱う。対になっていないサロゲートは
//   読み飛ばす（検索パターン等で不正な列を作らないため）。
std::vector<unsigned char> Utf8FromWide(const wchar_t* w, size_t length);

// 窓の先頭 startIndex が多バイト文字の途中にあるとき、読み飛ばすべきバイト数を返す。
//   途中でなければ 0。最大 3（4 バイト列の 2..4 バイト目から始まった場合）。
//   前方に最大 3 バイト遡ってリードバイトを探し、その列が startIndex を跨ぐ場合だけ
//   残りの後続バイト数を返す。跨がない（不正な列だった）場合は 0＝先頭から素直に読む。
//   buf には startIndex の手前 3 バイトと、startIndex から 4 バイトが入っている必要が
//   ある（列全体を復号して妥当性を確かめるため）。データ端でそれより短い場合は、
//   その範囲だけで判定する。
int Utf8CarryBytesAt(const unsigned char* buf, size_t size, size_t startIndex);

}  // namespace stirling
