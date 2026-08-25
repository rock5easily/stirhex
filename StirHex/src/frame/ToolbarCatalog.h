// ツールバー機能カタログ: rawID → 実コマンドID / ビットマップ画像索引。
//   原データ由来（image base 0x400000）:
//     rawID→cmdID       = DAT_004b51e6（FUN_00409439）
//     cmdID→画像索引    = DAT_004b6ad0（FUN_00464195。54エントリ, 索引0..53）
//   画像は StirHex 独自生成リソース128（CToolBar 用ビットマップ, 16px×54, 高さ15）。
//   rawID = (カテゴリ<<8)|項目番号。セパレータ 0xFFFF はカタログ外。
#pragma once

#include <windows.h>

// rawID に対応する実コマンドID（0 = 未定義）。
UINT ToolbarRawToCmd(UINT rawId);

// rawID に対応するビットマップ画像索引（0..53。-1 = 未定義）。
int  ToolbarRawToImage(UINT rawId);
