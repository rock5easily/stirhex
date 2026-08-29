// パス文字列の分解（MFC / Win32 に依存しない。単体テストは porting/tests/core_test.cpp）。
//   シェル操作（ui::RevealInExplorer）が「どのフォルダを開くか」を決める部分を、
//   実際にフォルダを開く処理から切り離してテストできるようにするための単位（Issue #133）。
#pragma once

#include <string>

namespace stirling {
namespace path {

inline bool IsSeparator(wchar_t c) { return c == L'\\' || c == L'/'; }

// 絶対パス（またはドライブ相対のルート起点）か。
//   "C:\a" / "\server\share\a" / "\a" は true、"a.ini" / "sub\a.ini" / "C:a.ini" は false。
inline bool IsRooted(const std::wstring& path) {
    if (path.empty()) { return false; }
    if (IsSeparator(path[0])) { return true; }                       // ルート起点・UNC
    return path.size() >= 3 && path[1] == L':' && IsSeparator(path[2]);
}

// 親フォルダ。区切りが無ければ空文字列。ルート直下はルートの区切りを残す
//   （"C:\a.ini" → "C:\"、"\a.ini" → "\"、"sub\a.ini" → "sub"）。
inline std::wstring ParentFolder(const std::wstring& path) {
    const size_t sep = path.find_last_of(L"\\/");
    if (sep == std::wstring::npos) { return std::wstring(); }
    if (sep == 0) { return path.substr(0, 1); }                      // "\a.ini"
    if (sep == 2 && path[1] == L':') { return path.substr(0, 3); }   // "C:\a.ini"
    return path.substr(0, sep);
}

// エクスプローラで開くべきフォルダを決める。
//   path が親フォルダ部分を持たない相対ファイル名なら、保存先であるカレント
//   ディレクトリを返す。相対の親フォルダはカレントディレクトリからの絶対パスにする。
//   決められない場合（path が空）だけ空文字列を返す。
inline std::wstring FolderToReveal(const std::wstring& path, const std::wstring& currentDir) {
    if (path.empty()) { return std::wstring(); }
    const std::wstring parent = ParentFolder(path);
    if (parent.empty()) { return currentDir; }   // "StirHex.ini" のような相対ファイル名
    if (IsRooted(parent) || currentDir.empty()) { return parent; }

    std::wstring base = currentDir;
    while (base.size() > 1 && IsSeparator(base.back())) { base.pop_back(); }
    return base + L"\\" + parent;
}

}  // namespace path
}  // namespace stirling
