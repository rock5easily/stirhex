// core 層の共通型定義。
//   参照: analysis_artifacts/docs/04_BlockList_and_IO.md, 05_BlockCursor_edit_ops.md
//
// 原 Stirling 1.31 は 32bit アプリであり、ファイル内の絶対位置・ファイル長を
// すべて int（32bit 符号付き）で保持していた（2GB 超のファイルは扱えない）。
// x64 化（Issue #19）にあたり、「ファイル内の絶対位置・ファイル長」を表す型を
// FileOffset へ集約し、原ドキュメント中の int とはここで意図的に乖離させる。
//
// 型の使い分け:
//   FileOffset ... ファイル内の絶対位置・ファイル全体に対する長さ（64bit）
//   int        ... ブロック内オフセット・ブロック長（kBlockCapacity=16KB 上限のため 32bit で十分）
// 両者の境界では必ず明示的に変換する（BlockCursor 実装内の static_cast<int> 参照）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace stirling {

// ファイル内の絶対位置およびファイル長（原実装の int を置換）。
using FileOffset = std::int64_t;

// FileOffset(64bit) の長さが、このビルドのバッファ長(size_t)に収まるか（Issue #154）。
//   Win32 では size_t が 32bit のため、4GB 超をそのままキャストすると下位32bitへ
//   黙って切り捨てられ、確保した領域より大きな読み書きでヒープを破壊しうる。
//   FileOffset をバッファ長へ落とす箇所は必ずこの検査を通すこと。
inline bool FitsInBuffer(FileOffset n) {
    if (n < 0) { return false; }
    return static_cast<unsigned long long>(n) <=
           static_cast<unsigned long long>((std::numeric_limits<size_t>::max)());
}

}  // namespace stirling
