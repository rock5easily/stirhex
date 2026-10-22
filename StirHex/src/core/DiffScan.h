// DiffScan — 2つの文書の先頭 size バイトを比べ、相違の連続範囲を集める（Issue #307）。
//   データ比較（原 FUN_0044c96c）の本体。大きい文書でも UI を止めないよう、FindAll と同じく
//   Start で条件を受け取り、Step で少しずつ比べる。相違範囲は上限件数で打ち切る。
//   Step のたびに BlockCursor を作り直すため、呼出側が Step の合間に文書を編集しても
//   解放済みノードを参照しない（編集を検知して中止するのは呼出側の責務）。
//   MFC 非依存の core レイヤ（コア機能テストから単体で検証できるようにする）。
#pragma once

#include "core/CoreTypes.h"

#include <cstddef>
#include <utility>
#include <vector>

namespace stirling {

class BlockList;

enum class DiffScanState {
    Idle,        // 未開始
    Running,     // 比較中
    Complete,    // size バイトを比べ終えた
    Truncated,   // 上限件数を超える相違範囲があったため打ち切った
    Cancelled,   // Cancel された
    ReadError,   // データを読み取れなかった（文書が size より短い、比較中の文書変更など）
    OutOfMemory, // 読み込み用バッファか相違範囲の記録を確保できなかった
};

class DiffScan {
public:
    // 相違範囲 [first, second]（両端を含む。昇順）。
    using Range = std::pair<FileOffset, FileOffset>;

    // 既定の上限件数（[全て検索] と同じ）。
    static constexpr size_t kDefaultLimit = 10000;

    // a と b の [0, size) を比べる。size が 0 以下、limit が 0 のときは比べずに完了する。
    //   どちらかの文書が size より短いときは読み取りエラー。
    DiffScanState Start(BlockList& a, BlockList& b, FileOffset size, size_t limit = kDefaultLimit);

    // 最大 byteBudget バイトぶん比べる。現在の状態を返す。
    DiffScanState Step(FileOffset byteBudget);

    void Cancel();

    DiffScanState State() const { return state_; }
    bool Running() const { return state_ == DiffScanState::Running; }
    const std::vector<Range>& Diffs() const { return diffs_; }
    // 比べ終えたバイト数と、比べる総バイト数（進捗表示用）。
    FileOffset Scanned() const { return pos_; }
    FileOffset ScanTotal() const { return size_; }

private:
    // 比較中の相違範囲を [runStart_, end] で閉じて記録する。確保に失敗したら OutOfMemory。
    bool PushRange(FileOffset end);

    BlockList* a_ = nullptr;   // 比較は読み取りのみ（BlockCursor が非 const を要求する）
    BlockList* b_ = nullptr;
    FileOffset size_ = 0;
    FileOffset pos_ = 0;         // 次に比べる位置
    FileOffset runStart_ = -1;   // 比較中の相違範囲の先頭（相違の外では -1）
    size_t limit_ = kDefaultLimit;
    std::vector<Range> diffs_;
    std::vector<unsigned char> bufA_;
    std::vector<unsigned char> bufB_;
    DiffScanState state_ = DiffScanState::Idle;
};

}  // namespace stirling
