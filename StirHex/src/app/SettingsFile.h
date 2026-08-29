// 設定ファイルの保存先解決・読み書き・レジストリからの移行（Issue #96）。
//
// 保存先の探索順（先に決まったものを使う）:
//   1. コマンドライン `/ini:<path>`（`-ini:` も可）。テストや一時的な設定切り替えに使う
//   2. 実行ファイルと同じフォルダの StirHex.ini が **存在すれば** それ（ポータブルモード）。
//      「存在すれば」の判定にしているのは、Program Files 配下など書き込めない場所に
//      インストールされた場合に書けないパスを掴まないため。ポータブルにしたい利用者は
//      空ファイルを置けばよい
//   3. %APPDATA%\StirHex\StirHex.ini（既定）
//
// レジストリ（HKCU\Software\StirHex\StirHex）は 1.1.0 以前の保存先。設定ファイルが
// 存在しない初回起動時に限り、そこから読み出して設定ファイルへ書き出す。移行後も
// 旧キーは削除しない（旧版へ戻せるようにする）。
//
// この単位は Win32 に依存するが MFC には依存しない。
#pragma once

#include <string>

#include "app/SettingsStore.h"

namespace stirling {
namespace settings {

enum class SettingsSource {
    CommandLine,     // /ini:<path> で明示指定された
    PortableExeDir,  // 実行ファイルと同じフォルダの ini
    AppData,         // %APPDATA%\StirHex\StirHex.ini（既定）
};

struct SettingsLocation {
    std::wstring path;
    SettingsSource source = SettingsSource::AppData;
};

// 設定ファイル名（実行ファイル隣・APPDATA 共通）。
extern const wchar_t kSettingsFileName[];
// 旧保存先のレジストリキー（HKCU 配下の相対パス）。
extern const wchar_t kLegacyRegistryKey[];

// コマンドライン（GetCommandLineW）から `/ini:<path>` を取り出す。無ければ空文字列。
std::wstring FindCommandLineIniPath();
// 実行ファイルと同じフォルダの設定ファイルパス。取得できなければ空文字列。
std::wstring ExeDirSettingsPath();
// %APPDATA%\StirHex\StirHex.ini。取得できなければ空文字列。
std::wstring AppDataSettingsPath();

// 上記の探索順で保存先を決める。path が空になるのは APPDATA も取得できない異常時のみ。
SettingsLocation ResolveSettingsLocation();

// 設定ファイルが存在するか。「初回起動か」（＝レジストリからの移行が要るか）の判定に使う。
bool SettingsFileExists(const std::wstring& path);

// UTF-8（BOM なし）のテキストファイルを読み書きする。
//   設定ファイル以外の付随ファイル（マークファイル。Issue #99）も同じ扱いにするため
//   公開する。読み込みは BOM 付きも受け付ける（利用者が手で保存した場合のため）。
//   戻り値 : 成功したら true。false のとき error に原因が入る。
bool ReadTextFileUtf8(const std::wstring& path, std::wstring& text, std::wstring& error);
bool WriteTextFileUtf8(const std::wstring& path, const std::wstring& text,
                       std::wstring& error);

// 設定ファイルを読み込む。
//   戻り値 : 読み込めたら true。ファイルが存在しない場合も true（store は変更しない）。
//   error  : false のとき、原因を説明するメッセージが入る。
bool LoadSettingsFile(const std::wstring& path, SettingsStore& store, std::wstring& error);

// 設定ファイルへ書き出す（UTF-8 / BOM なし）。一時ファイルへ書いてから置換するため、
// 書き込み中の異常終了で既存の設定ファイルが壊れることはない。
//   error : false のとき、原因を説明するメッセージが入る。
bool SaveSettingsFile(const std::wstring& path, const SettingsStore& store, std::wstring& error);

// 最新のファイル内容へ store の変更だけを適用して書き戻す（Issue #130）。
//   同じ設定ファイルを使う複数インスタンスが互いの更新を消さないようにするための入口。
//   保存の間は設定ファイル単位の名前付きミューテックスで直列化し、書けたら store の
//   変更記録を落とす。
//   error : false のとき、原因を説明するメッセージが入る。
bool SaveSettingsFileMerged(const std::wstring& path, SettingsStore& store, std::wstring& error);

// 旧保存先（HKCU\<subKey>）の全セクション・全値を store へ取り込む。
//   REG_DWORD は10進文字列、REG_SZ はそのまま、REG_BINARY は大文字16進文字列にする
//   （プロファイル API 側の読み出しと対になる表現）。
//   戻り値: 1件以上取り込めたら true。キーが無い場合は false。
bool ImportFromRegistry(const wchar_t* subKey, SettingsStore& store);

}  // namespace settings
}  // namespace stirling
