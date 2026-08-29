// シェル系 API のラッパ（wide 層）。
//   フォルダ選択は Vista 以降で推奨される IFileDialog + FOS_PICKFOLDERS を使う。
//   プロセス起動は ShellExecuteEx を使い、失敗要因（Win32 エラーコード）を呼び出し側へ返す。
//   COM は CStirlingApp::InitInstance の AfxOleInit で STA 初期化済み（本ヘルパは初期化しない）。
#pragma once

#include <afxwin.h>

namespace ui {

// フォルダ選択ダイアログ。
//   title          : ダイアログ表題（リソース由来の文字列。nullptr ならシェル既定）
//   initialFolder  : 初期表示フォルダ。空／存在しないパスなら指定しない（シェル既定に委ねる）
//   outPath        : 選択されたフォルダのフルパス（S_OK のときのみ更新）
//   戻り値         : S_OK=選択された / 利用者のキャンセル / それ以外の失敗 を区別できる HRESULT。
//                    キャンセルの判定には IsUserCancelled を使うこと。
//   パス長は MAX_PATH に依存しない（シェルが返す文字列をそのまま受け取る）。
HRESULT BrowseForFolder(HWND owner, LPCWSTR title, LPCWSTR initialFolder, CStringW& outPath);

// 利用者によるキャンセルか（＝エラーメッセージを出すべきでない失敗か）。
bool IsUserCancelled(HRESULT hr);

// ファイル／コマンドラインの実行（既定の動詞で開く）。
//   outError : 失敗時の Win32 エラーコード（成功時は ERROR_SUCCESS）
//   戻り値   : 起動に成功した場合のみ true
bool ShellExecuteFile(HWND owner, LPCWSTR file, DWORD& outError);

// ファイルをエクスプローラで選択表示する（Issue #111）。
//   ファイルがまだ存在しない場合は選択できないため、親フォルダを開くだけにする。
//   outError : 失敗時の Win32 エラーコード（成功時は ERROR_SUCCESS）
//   戻り値   : エクスプローラを開けた場合のみ true
bool RevealInExplorer(HWND owner, LPCWSTR path, DWORD& outError);

// Win32 エラーコード／HRESULT の説明文（取得できない場合は空文字列）。末尾の改行は除去する。
CStringW FormatSystemError(DWORD error);

// エラー本文へ失敗理由を付記する（説明文が得られない場合は本文をそのまま返す）。
CStringW AppendErrorReason(const CStringW& body, DWORD error);

// カレントディレクトリのフルパス（MAX_PATH 非依存。取得できない場合は空文字列）。
CStringW CurrentDirectory();

// 相対パスをフルパスへ解決する（MAX_PATH 非依存）。
//   解決できない場合は入力をそのまま返す（呼び出し側は従来どおり元のパスで処理を続ける）。
CStringW FullPath(LPCWSTR path);

// ドロップされた i 番目のファイルのパス（MAX_PATH 非依存。取得できない場合は空文字列）。
CStringW DragQueryPath(HDROP drop, UINT index);

// 実行ファイルの置かれているディレクトリ（末尾は区切り文字付き。取得できない場合は空文字列）。
CStringW ModuleDirectory();

}  // namespace ui
