// MBCS（ANSI）ビルドが書いたレジストリ設定値の、Unicode ビルドでの読み替え（Issue #43）。
//
// 背景:
//   レジストリは値を UTF-16 で保持し、ANSI 版 API（RegSetValueExA / RegQueryValueExA）が
//   REG_SZ の書き込み・読み出し時に **システム ANSI コードページ（ACP）** で変換する。
//   MFC の CWinApp::WriteProfileString は MBCS ビルドで RegSetValueExA を呼ぶため、
//   MBCS 版 Stirling が渡した narrow バイト列は「ACP として」UTF-16 化されて格納された。
//
//   本移植の MBCS 版は narrow リテラルが UTF-8 になる /utf-8 ビルドだったため、
//   レジストリへ渡す文字列だけを明示的に CP932 へ落としていた（ToCp932 シム）。
//   したがって MBCS 版が書いた値の解釈は次の 2 系統に分かれる。
//
//   (a) ToCp932 シム経由の値（Env\BackupFolder, Env\DefaultFolder,
//       Extensions\Ext%d, Extensions\Comment%d, Rec%d\FontFace）
//         → narrow の中身は **CP932**。ACP≠932 の環境では ACP として UTF-16 化され化ける。
//   (b) OS の A 版 API 由来の値（CaretPositions\Path%d, History\Execute%d,
//       Recent File List\File%d）
//         → narrow の中身は **ACP**。UTF-16 化は正しく、化けない。
//
//   よって移行が必要なのは (a) かつ ACP≠932 の場合だけである。ACP=932 の環境
//   （日本語 Windows）では (a) も恒等変換になり、MBCS 版の設定はそのまま読める。
//
// 復元できない場合は書き換えない:
//   RepairCp932ViaAcp は「ACP バイト列への巻き戻しが可逆であること」と「得られたバイト列が
//   CP932 として妥当であること」を検証し、どちらかが崩れたら false を返す。呼び出し側は
//   値を温存する。実測した ACP 別の挙動は次のとおり（porting/tests/core_test.cpp）。
//
//   - 932            : 変換が恒等。移行不要（false を返す）
//   - 1252 / 1250 等 : SBCS は全バイトが可逆に写るため完全に復元できる
//   - 936 / 949 等   : 別 DBCS も概ね復元できる。ただし **書き込み時** に ACP が写せなかった
//                      バイト対は既に '?' へ潰れており、その文字だけは戻らない（部分復元）。
//                      値全体が化けたままよりは良いので、部分復元でも書き戻す
//   - 65001 (UTF-8)  : 不正シーケンスが U+FFFD へ潰れ、バイト境界ごと失われるため復元不能。
//                      明示的に対象外とする（実測では復元後のバイト列が CP932 として妥当で
//                      ないと判定されて結局拒否されるが、意図を暗黙の挙動に頼らない）
//
// この単位は MFC に依存しない（Win32 の文字コード変換のみ）。単体テストは
// porting/tests/core_test.cpp。
#pragma once

#include <string>

namespace stirling {
namespace settings {

// MBCS 版が CP932 として書き、ACP として UTF-16 化された値を元のワイド文字列へ戻す。
//   stored : 現在レジストリに入っているワイド文字列
//   acp    : MBCS 版が動作していた環境の ANSI コードページ（通常は ::GetACP()）
//   out    : 復元できた場合のみ格納する
//   戻り値 : 復元して書き戻すべきとき true。次のいずれかでは false（out は触らない）
//            - acp が 932（変換が恒等なので移行不要）
//            - acp が UTF-8/UTF-7（書き込み時にバイト境界ごと失われており復元不能）
//            - stored → ACP バイト列の変換が非可逆（既定文字への置換が起きた等）
//            - 得られたバイト列が CP932 として妥当でない（元から正しいワイド値だった等）
//            - 復元結果が stored と同一（ASCII のみの値。書き戻す意味がない）
//            - 得られたバイト列が単バイトだけで読める（＝2バイトシーケンスを含まない）
//
// 「正しい値を壊さない」ことの限界:
//   上の検証は「ACP 変換として可逆」「CP932 として妥当」を示すだけで、その値が本当に
//   CP932 バイト列由来だったことまでは示さない。たとえば ACP=1252 で正しく書かれた
//   L"C:\¥"(U+00A5) は CP1252 で 43 3A 5C A5 となり、0xA5 は CP932 の半角カナ U+FF65
//   としても妥当に読めてしまう。この種の誤判定を防ぐため、最後の条件で「2バイト
//   シーケンスを1つ以上含むこと」を要求している。MBCS 版で実際に化ける値は必ず全角を
//   含む（ASCII と半角カナだけの値は ACP 変換でも化けない）ので、真に移行が要る値は
//   この条件を必ず満たす。
//   ただし単バイト文字が2つ並んで偶然 CP932 の2バイト対になる値（U+201A U+00A0 →
//   0x82 0xA0 → 「あ」）までは排除できない。呼び出し側が検査済みマーカで一度しか
//   走らせないこと（＝対象は MBCS 版が書いた設定に限られること）と併せて許容する。
bool RepairCp932ViaAcp(const std::wstring& stored, unsigned int acp, std::wstring& out);

}  // namespace settings
}  // namespace stirling
