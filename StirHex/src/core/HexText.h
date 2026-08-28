// HexText — 16進表記のテキストをバイト列へ解析する（クリップボードの16進テキスト貼り付け用）。
//   入口は wide 層（クリップボードの CF_UNICODETEXT／CP932 から変換した文字列）だが、
//   受理するのは ASCII の16進数字と区切りのみで、非 ASCII は一律に不正文字として扱う。
//   MFC 非依存の core レイヤ（コア機能テストから単体で検証できるようにする）。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace stirling {

// 解析結果の種別。
enum class HexTextError {
    None = 0,        // 成功
    Empty,           // 16進数字が 1 つも無い（空文字列／区切りのみ）
    InvalidChar,     // 16進数字でも区切りでもない文字がある（errorPos に位置）
    OddDigits,       // 桁数が奇数のトークンがある（errorPos にそのトークンの先頭位置）
};

// 解析の詳細。errorPos は入力先頭からの 0 起点の文字位置（None のときは 0）。
struct HexTextParseResult {
    HexTextError error = HexTextError::None;
    size_t errorPos = 0;

    bool Ok() const { return error == HexTextError::None; }
};

// 16進テキストをバイト列へ解析する（寛容形式）。
//   区切り : 半角空白 / タブ / CR / LF / カンマ。
//            区切りは位置を問わず読み飛ばすため、入力の先頭・末尾に空白やタブ、CR/LF、
//            カンマが付いていても無視される（別途 Trim する必要は無い）。連続する区切りも
//            1 つの区切りとして扱う。クリップボードのテキストは末尾に改行が付くことが
//            多いので、この扱いを仕様として保証する。
//   接頭辞 : 各トークン先頭の "0x" "0X" "\x" "\X" を除去する。
//   トークン: 除去後は16進数字のみで、かつ偶数桁であること。全トークンを連結して
//             バイト列とする（"41 42" と "4142" は同じ結果）。
//   トークン単位で偶数桁を要求するのは、"41 4 43" のような入力を 0x41,0x44,0x03 と
//   解釈して黙って別のデータを貼り付けてしまうのを防ぐため。
//   ':' を区切りに含めないのも同じ理由で、16進ダンプ（"0000: 41 42  AB"）を
//   アドレス列ごと取り込むことを構造的に避ける。
//   失敗した場合 out は空になる（部分的な結果は返さない）。
HexTextParseResult ParseHexText(const wchar_t* text, size_t length,
                                std::vector<unsigned char>& out);

// std::wstring 版の簡便形。
inline HexTextParseResult ParseHexText(const std::wstring& text,
                                       std::vector<unsigned char>& out) {
    return ParseHexText(text.c_str(), text.size(), out);
}

}  // namespace stirling
