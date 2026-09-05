// Utf16Text — UTF-16 バイト列の復号・符号化と、文字欄の「バイト→表示セル」写像の土台。
//   キャラクターセット Unicode（charset 3）を CP932 の範囲を超えて表示するために使う
//   （Issue #173）。UTF-8 側の Utf8Text と対になる core レイヤのモジュールで、
//   MFC / GDI には依存しない（コア機能テストから単体で検証できるようにする）。
//
//   文字欄の不変条件は UTF-8 と同じく「1 ソースバイト = 1 表示セル」。
//   コード単位 1 つ = 2 バイト = 2 セル、サロゲートペア = 4 バイト = 4 セルを占め、
//   グリフが余らせたセルは空白で埋める。
#pragma once

#include <cstddef>
#include <vector>

namespace stirling {

// 単独では文字にならないサロゲート領域か。
constexpr bool IsUtf16Surrogate(unsigned int u) { return u >= 0xD800 && u <= 0xDFFF; }
constexpr bool IsUtf16HighSurrogate(unsigned int u) { return u >= 0xD800 && u <= 0xDBFF; }
constexpr bool IsUtf16LowSurrogate(unsigned int u) { return u >= 0xDC00 && u <= 0xDFFF; }

// 復号結果。ok が false のときは length ぶんのバイトを不正として扱う。
struct Utf16Decoded {
    bool ok = false;            // 妥当なコードポイントとして復号できたか
    unsigned int codePoint = 0; // ok のときのコードポイント
    int length = 2;             // 消費したバイト数（2 または 4。端数は 1）
    bool truncated = false;     // サロゲートペアの途中でバッファが尽きた
};

// p[0..n) の先頭から 1 文字を復号する。bigEndian は文字セットのバイトオーダ設定。
//   ペアになっていないサロゲートは不正とし、ok=false・length=2 を返す
//   （2 バイト = 2 セルを保ったまま '.' へ落とすため）。
//   バッファに 1 バイトしか残っていない場合は length=1 の不正とする。
//   上位サロゲートの直後でバッファが尽きた場合は truncated=true（呼び出し側が
//   次の窓へ持ち越すか、その場で不正として描くかを決める）。
Utf16Decoded DecodeUtf16(const unsigned char* p, size_t n, bool bigEndian);

// コードポイントを UTF-16 のバイト列へ符号化して out へ追加する。
//   サロゲート単体や U+10FFFF 超は書き込まず false を返す。
bool EncodeUtf16(unsigned int codePoint, bool bigEndian, std::vector<unsigned char>& out);

// ワイド文字列（UTF-16）をバイト列へ変換する。
//   サロゲートペアは 1 コードポイントとして扱い、対になっていないサロゲートは
//   読み飛ばす（検索パターン等で不正な列を作らないため。Utf8FromWide と同じ方針）。
std::vector<unsigned char> Utf16FromWide(const wchar_t* w, size_t length, bool bigEndian);

// 窓の先頭 startIndex が文字の途中にあるとき、読み飛ばすべきバイト数（0..2）を返す。
//   startIndex にある 2 バイトが下位サロゲートで、その手前 2 バイトが上位サロゲートの
//   場合だけ 2 を返す。buf には startIndex の手前 2 バイトと、startIndex から 2 バイトが
//   入っている必要がある。データ端でそれより短い場合は 0（先頭から素直に読む）。
//   偶奇の整列（開始オフセットが奇数）は呼び出し側の担当。
int Utf16CarryBytesAt(const unsigned char* buf, size_t size, size_t startIndex, bool bigEndian);

}  // namespace stirling
