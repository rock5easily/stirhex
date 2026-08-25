// Cp932Text — byte 層（編集対象・struct.def 等の CP932 バイト列）と wide 層（UI・パス）の
//   境界で使う変換ヘルパ。MFC 非依存の core レイヤ。
//   コードページは 932 固定とし、システム ANSI コードページ（CP_ACP）には依存しない。
//   詳細: analysis_artifacts/docs/20_unicode_layering.md §5。
#pragma once

#include <string>

namespace stirling {

// [byte層境界] CP932 バイト列 → 表示用ワイド文字列（best-effort）。
//   不正バイトは API 既定の置換に任せる。表示専用でラウンドトリップは保証しない。
//   len < 0 のときは bytes を NUL 終端として扱う。bytes == nullptr は空文字列。
std::wstring WideFromCp932(const char* bytes, int len = -1);

// [byte層境界] ワイド文字列 → CP932 バイト列。
//   CP932 で表現できない文字が 1 つでもあれば out を空にして false を返す
//   （検索パターン等、欠落が致命的になる用途向け）。
//   len < 0 のときは w を NUL 終端として扱う。w == nullptr は空文字列で true。
bool Cp932FromWide(const wchar_t* w, std::string& out, int len = -1);

// [byte層] CP932 の 2 バイト文字の先行バイトか。
//   `::IsDBCSLeadByte` はシステム ANSI コードページを参照するため、非日本語環境
//   （例: ACP=1252）では常に false を返す。編集対象バイト列の走査はロケールに
//   依存してはならないので、CP932 の先行バイト範囲を直接判定する。
//   詳細: analysis_artifacts/docs/20_unicode_layering.md §4.2
constexpr bool IsCp932LeadByte(unsigned char b) {
    return (b >= 0x81 && b <= 0x9f) || (b >= 0xe0 && b <= 0xfc);
}

}  // namespace stirling
