// マークファイルの直列化／解析（Issue #99）。
//
// マーク（位置→種別）をテキストファイルへ書き出し、読み込む。別環境・別利用者との共有と
// バックアップが目的のため、人が読んで手で直せる形式にする。
//
// 形式は Issue #96 の設定ファイルと同じ INI 風で、SettingsStore をそのまま使う
// （パーサ・UTF-8 変換・値のエスケープを二重に持たない。日本語パスも化けない）:
//
//   ; StirHex mark file
//   [Mark]
//   Version=1
//   File=C:\data\sample.bin
//   Size=1048576
//
//   [Marks]
//   40=1
//   A0=2
//   1F400=3
//
// [Marks] のキーは16進アドレス（接頭辞なし）、値は 1..3。内部の種別 0..2 ではなく UI の
// 「マーク1/2/3」に合わせるのは、手で読み書きする人にとってそれが自然なため。
// [Mark] の File / Size は情報。File の不一致は問題としない（共有が目的のため）。
//
// この単位は MFC / Win32 に依存しない（単体テストは porting/tests/core_test.cpp）。
#pragma once

#include <map>
#include <string>

#include "core/CoreTypes.h"

namespace stirling {
namespace marks {

// ファイル上のマーク種別の範囲（内部種別 0..2 に対して 1..3）。
inline constexpr int kMinMarkNumber = 1;
inline constexpr int kMaxMarkNumber = 3;

struct MarkFileData {
    std::wstring sourcePath;              // 書き出し元のデータのパス（情報。空可）
    stirling::FileOffset sourceSize = -1; // 書き出し元のデータのサイズ（情報。-1=不明）
    std::map<stirling::FileOffset, int> marks;   // 位置 → 1..3
};

// マークファイルのテキストを作る（UTF-8 で保存する前提のワイド文字列）。
std::wstring SerializeMarks(const MarkFileData& data);

// マークファイルのテキストを解析する。
//   戻り値 : 全体を解釈できたら true。false のとき out は変更しない（途中まで適用しない）
//   error  : false のとき、原因を説明するメッセージ（問題のあるキーや値を含む）
bool ParseMarks(const std::wstring& text, MarkFileData& out, std::wstring& error);

// --- 1行表現（自動保存／自動復元。Issue #100） ---
//
// 設定ファイルの1つの値へ収めるための表現。"40:1,A0:2" のように、16進アドレスと
// 種別 1..3 を `:` で組にし `,` で並べる。アドレスは昇順。
// マークファイル（複数行のセクション形式）は1行の値には収まらないため別に用意するが、
// アドレスと種別の表記は揃えてあり、両者を見比べられる。

// 1件あたりに許す最大件数。設定ファイルが肥大しないよう上限を設ける。
//   超過分はアドレスの大きい側から捨てる（先頭 kMaxStoredMarks 件を残す）。
inline constexpr size_t kMaxStoredMarks = 256;

std::wstring EncodeMarkList(const std::map<stirling::FileOffset, int>& marks);
// 解釈できない要素が1つでもあれば false（out は変更しない）。
bool DecodeMarkList(const std::wstring& text, std::map<stirling::FileOffset, int>& out);

}  // namespace marks
}  // namespace stirling
