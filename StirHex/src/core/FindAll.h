// FindAll — 範囲内の一致位置をすべて集める全件検索（Issue #236）。
//   大きい文書でも UI を止めないよう、Start で条件を受け取り、Step で少しずつ走査する。
//   Step のたびに BlockCursor を作り直すため、呼出側が Step の合間に文書を編集しても
//   解放済みノードを参照しない（編集を検知して中止するのは呼出側の責務）。
//   MFC 非依存の core レイヤ（コア機能テストから単体で検証できるようにする）。
#pragma once

#include "core/CoreTypes.h"
#include "core/HexPattern.h"

#include <cstddef>
#include <vector>

namespace stirling {

class BlockList;

enum class FindAllState {
    Idle,        // 未開始
    Running,     // 走査中
    Complete,    // 範囲の末尾まで走査した
    Truncated,   // 上限件数を超える一致があったため打ち切った
    Cancelled,   // Cancel された
    ReadError,   // データを読み取れなかった（範囲が文書の外、走査中の文書変更など）
};

class FindAll {
public:
    // 既定の上限件数。
    static constexpr size_t kDefaultLimit = 10000;

    // [lo, hi) に完全に収まる一致をすべて探す。一致の重なりも1件ずつ数える
    //   （先頭位置を1バイトずつ進めて判定するのと同じ結果）。
    //   pattern が空、範囲が文書の外、limit が 0 のときは走査せず終了状態になる。
    FindAllState Start(BlockList& list, const HexPattern& pattern,
                       FileOffset lo, FileOffset hi, size_t limit = kDefaultLimit);

    // 一致の先頭候補を最大 positionBudget 個ぶん走査する。現在の状態を返す。
    FindAllState Step(FileOffset positionBudget);

    void Cancel();

    FindAllState State() const { return state_; }
    bool Running() const { return state_ == FindAllState::Running; }
    const std::vector<FileOffset>& Hits() const { return hits_; }
    size_t PatternSize() const { return pattern_.Size(); }
    const HexPattern& Pattern() const { return pattern_; }
    FileOffset RangeBegin() const { return lo_; }
    FileOffset RangeEnd() const { return hi_; }
    // 走査済みの先頭候補の数（進捗表示用）。
    FileOffset Scanned() const { return next_ - lo_; }
    FileOffset ScanTotal() const { return (lastStart_ >= lo_) ? (lastStart_ - lo_ + 1) : 0; }

private:
    BlockList* list_ = nullptr;   // 検索は読み取りのみ（BlockCursor が非 const を要求する）
    HexPattern pattern_;
    FileOffset lo_ = 0;
    FileOffset hi_ = 0;
    FileOffset next_ = 0;        // 次に調べる先頭候補
    FileOffset lastStart_ = -1;  // 一致の先頭として取り得る最後の位置（hi - パターン長）
    size_t limit_ = kDefaultLimit;
    std::vector<FileOffset> hits_;
    FindAllState state_ = FindAllState::Idle;
};

}  // namespace stirling
