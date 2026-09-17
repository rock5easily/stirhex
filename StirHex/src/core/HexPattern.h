// HexPattern — 16進データ検索のパターン（ワイルドカード `??` 対応。Issue #233）。
//   検索/置換ダイアログの16進入力を、バイト値と「任意の1バイトに一致する位置」の組へ解析する。
//   書式は VS Code Hex Editor 形式で、`??` だけを1バイトのワイルドカードとして受け付ける。
//   MFC 非依存の core レイヤ（コア機能テストから単体で検証できるようにする）。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace stirling {

// 検索パターン。bytes と wildcard は同じ長さで、wildcard[i] != 0 の位置は bytes[i] を
//   照合せず任意のバイトに一致する（そのとき bytes[i] は 0）。
//   ワイルドカードを含まないパターンでは wildcard を空にし、従来の完全一致検索と区別する。
struct HexPattern {
    std::vector<unsigned char> bytes;
    std::vector<unsigned char> wildcard;

    bool   Empty() const { return bytes.empty(); }
    size_t Size() const { return bytes.size(); }
    bool   HasWildcard() const { return !wildcard.empty(); }
    // SearchPattern へ渡すワイルドカード表（含まなければ nullptr = 完全一致）。
    const unsigned char* WildcardData() const { return HasWildcard() ? wildcard.data() : nullptr; }

    bool operator==(const HexPattern& other) const {
        return bytes == other.bytes && wildcard == other.wildcard;
    }
    bool operator!=(const HexPattern& other) const { return !(*this == other); }
};

// ワイルドカードを含まないパターン（文字列検索の変換結果など）を作る。
inline HexPattern MakeExactPattern(const std::vector<unsigned char>& bytes) {
    HexPattern p;
    p.bytes = bytes;
    return p;
}

// 16進入力をパターンへ解析する。受理形式は検索ダイアログの従来の16進入力と同じ。
//   前後の半角空白・タブは無視する。
//   (1) 半角空白を含む場合: 空白で区切った各かたまりが、ちょうど2文字であること。
//   (2) 半角空白を含まない場合: 全体が偶数文字で、先頭から2文字ずつに区切る。
//   2文字の単位は、16進数字2桁（大文字・小文字可）か、allowWildcard のときに限り `??`。
//   `4?` `?4` のように16進数字と `?` を混ぜた単位は不正。
//   すべての単位がワイルドカードのパターンは不正（どの位置にも一致して意味を持たないため）。
//   失敗した場合 false を返し、out は空になる（部分的な結果は返さない）。
bool ParseHexPattern(const wchar_t* text, size_t length, bool allowWildcard, HexPattern& out);

inline bool ParseHexPattern(const std::wstring& text, bool allowWildcard, HexPattern& out) {
    return ParseHexPattern(text.c_str(), text.size(), allowWildcard, out);
}

// パターンを "41 ?? 43" 形式（大文字2桁、半角空白区切り）の文字列にする。
std::wstring FormatHexPattern(const HexPattern& pattern);

}  // namespace stirling
