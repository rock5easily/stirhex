// ユーザーメニュー機能カタログ: rawID → 実コマンドID / 表示名、およびメニュー構築。
//   原データ由来（image base 0x400000）:
//     rawID→cmdID = DAT_004b51e6（FUN_00409439。8カテゴリ×0x48, cmdID=cat*0x48+item*4 の先頭u16）
//   名称は全カテゴリのフルコマンド集合（原 FUN_0042b225。文字列4000-58xx 相当）。
//   rawID = (カテゴリ<<8)|項目番号。セパレータは 0xFFFF。
//   ツールバー(ToolbarCatalog)は同 cmdID 表の部分集合だが、メニューは全項目を持つ。
#pragma once

#include <afxwin.h>

#include <vector>

// rawID に対応する実コマンドID（0 = 未定義）。
UINT UserMenuRawToCmd(UINT rawId);

// rawID に対応する表示名（文字列リソースから取得。未定義 rawID は空文字列）。
CStringW UserMenuRawToName(UINT rawId);

// userMenus[idx] 相当の rawID 列からポップアップメニューを構築する。
//   0xFFFF=セパレータ、未定義rawID=スキップ。名称は CP932(MBCSビルド)へ変換して追加。
//   構築に成功し 1 項目以上あれば true。
bool BuildUserPopup(const std::vector<UINT>& items, CMenu& outMenu);
