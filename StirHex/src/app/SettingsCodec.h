// 64bit 設定値の保存形式（プロジェクト共通規約。Issue #22）。
//
// 原 Stirling および本移植の 32bit 版は、キャレット位置などアドレス系の設定値を
// REG_DWORD（CWinApp::WriteProfileInt）で保存していたため 2GB 超の位置を保持できない。
// x64 化にあたり、64bit アドレスを保持する設定値の保存形式を16進文字列に統一する。
//
// 形式:
//   - 接頭辞なしの大文字16進、1〜16桁（例: 0 → "0"、0x1FFFFFFFF → "1FFFFFFFF"）
//   - 64bit を2の補数として符号なし16進で表記するため、負値を含む全 FileOffset を無損失に往復する
//   - 旧レジストリの REG_DWORD とは別の値名を使う（旧値名は移行読み込み専用とし、
//     保存時に削除する）
//
// この codec は MFC / Win32 に依存しない（porting/tests/core_test.cpp で単体テストする）。
//
// ナロー版とワイド版:
//   保存形式は 16進表記だけの ASCII 層（設計メモ 20_unicode_layering.md §2）であり、
//   文字コードの問題は本来生じない。ただし Unicode ビルドでは Profile API からワイド文字列で
//   得られるため、ナロー版しか無いと呼び出し側で CStringA を挟むことになり、ASCII 層の
//   値がシステム ANSI コードページを経由してしまう。無用な依存を避けるためワイド版を持つ
//   （Issue #43）。
#pragma once

#include <string>

#include "core/CoreTypes.h"

namespace stirling {
namespace settings {

// 64bit 値を保存形式（接頭辞なし大文字16進）へ変換する。
std::string  FormatOffsetHex(FileOffset value);
std::wstring FormatOffsetHexW(FileOffset value);

// 保存形式の文字列を 64bit 値へ復元する。復元できた場合のみ out を更新して true を返す。
//   受理: 1〜16桁の16進（大小文字可）。省略可能な "0x" / "0X" 接頭辞。
//   拒否: nullptr・空文字列・16進以外の文字を含む・17桁以上・接頭辞のみ（前後の空白も不可）。
bool ParseOffsetHex(const char* text, FileOffset& out);
bool ParseOffsetHex(const wchar_t* text, FileOffset& out);

}  // namespace settings
}  // namespace stirling
