// CharConv — 構造体編集バーの char/byte 配列を文字セット別に文字列化する純粋関数
//   （原 FUN_0045fecb 忠実移植）。MFC 非依存の core レイヤ。
//   詳細: analysis_artifacts/docs/18_struct_edit.md §6。
#pragma once

#include <string>

namespace stirling {

// bytes（先頭 n バイト）を文字セット charset に従い表示用文字列へ変換する。
//   原は先頭 min(n, 256) バイトのみを使う（StructRow_BuildColumns の 0x100 上限）。
//   charset: 0=ASCII / 1=SJIS / 2=EUC-JP / 3=Unicode(UTF-16LE) / 4=EBCDIC / 5=EBCIDK。
//   基本は生バイトを連結（フォントが SJIS 等を描画）。ASCII は非印字を '.'、
//   EUC は主プレーン対を SJIS へ変換、Unicode は CP932 へ変換、EBCDIC/EBCIDK は
//   変換表で写像し無効バイトを '.' とする（原の各分岐に忠実）。
//   戻り値は byte 層（CP932 バイト列）。表示側が境界でワイドへ変換する。
std::string FormatStructCharArray(int charset, const unsigned char* p, int n);

}  // namespace stirling
