// CharConv — 構造体編集バーの char/byte 配列を文字セット別に文字列化する純粋関数
//   （原 FUN_0045fecb 忠実移植）。MFC 非依存の core レイヤ。
//   詳細: analysis_artifacts/docs/18_struct_edit.md §6。
#pragma once

#include <string>

namespace stirling {

// bytes（先頭 n バイト）を文字セット charset に従い CP932 バイト列へ変換する（0..5 用）。
//   原は先頭 min(n, 256) バイトのみを使う（StructRow_BuildColumns の 0x100 上限）。
//   charset: 0=ASCII / 1=SJIS / 2=EUC-JP / 3=Unicode(UTF-16LE) / 4=EBCDIC / 5=EBCIDK。
//   基本は生バイトを連結（フォントが SJIS 等を描画）。ASCII は非印字を '.'、
//   EUC は主プレーン対を SJIS へ変換、Unicode は CP932 へ変換、EBCDIC/EBCIDK は
//   変換表で写像し無効バイトを '.' とする（原の各分岐に忠実）。
//   charset 6(UTF-8) は CP932 で表せない文字が落ちるため、この関数では扱わない
//   （FormatStructCharArrayW を使うこと）。
std::string FormatStructCharArrayCp932(int charset, const unsigned char* p, int n);

// bytes（先頭 n バイト）を文字セット charset に従い表示用のワイド文字列へ変換する。
//   charset: 0=ASCII / 1=SJIS / 2=EUC-JP / 3=Unicode(UTF-16LE) / 4=EBCDIC / 5=EBCIDK /
//            6=UTF-8（移植で追加。Issue #98）。
//   0..5 は FormatStructCharArrayCp932 の結果をワイドへ変換するだけなので、表示結果は
//   従来（表示直前に StructBar が WideFromCp932 していた頃）と同じ。
//   6 は UTF-8 を復号してワイドを直接組み立てるため、CP932 に無い文字も残る
//   （Issue #107）。不正・不完全な列は 1 バイト = 1 文字の '.'。
//   文字欄（CStirlingView）と違いセル整列の制約が無いので、空白詰めはしない。
std::wstring FormatStructCharArrayW(int charset, const unsigned char* p, int n);

}  // namespace stirling
