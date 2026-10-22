// DiffScan 実装（DiffScan.h 参照）。
#include "core/DiffScan.h"

#include "core/AllocFailureScope.h"   // 確保失敗を std::bad_alloc で受ける区間（Issue #306）
#include "core/BlockCursor.h"
#include "core/BlockList.h"

#include <algorithm>
#include <new>

namespace stirling {

namespace {
// 1 回に読み込んで比べる最大長。BlockCursor::Seek は先頭からノードをたどるため、
//   読み込みごとに Seek し直すと全体読み込み（16KB ブロック）の大きな文書では
//   ノード数の二乗に近い時間がかかる。大きめに読んで Seek の回数を抑える。
constexpr FileOffset kReadChunk = 4 * 1024 * 1024;
}  // namespace

DiffScanState DiffScan::Start(BlockList& a, BlockList& b, FileOffset size, size_t limit) {
    a_ = &a;
    b_ = &b;
    size_ = (size > 0) ? size : 0;
    pos_ = 0;
    runStart_ = -1;
    limit_ = limit;
    diffs_.clear();

    if (size_ > a.GetTotalLength() || size_ > b.GetTotalLength()) {
        state_ = DiffScanState::ReadError;
        return state_;
    }
    if (size_ == 0 || limit == 0) {
        state_ = DiffScanState::Complete;
        return state_;
    }
    const size_t chunk = static_cast<size_t>((std::min)(size_, kReadChunk));
    try {
        const StdAllocScope allocScope;
        bufA_.resize(chunk);
        bufB_.resize(chunk);
    } catch (const std::bad_alloc&) {
        state_ = DiffScanState::OutOfMemory;
        return state_;
    }
    state_ = DiffScanState::Running;
    return state_;
}

DiffScanState DiffScan::Step(FileOffset byteBudget) {
    if (state_ != DiffScanState::Running) {
        return state_;
    }
    // Step の合間に文書が縮んでいたら、範囲の外を読まずに打ち切る。
    if (a_ == nullptr || b_ == nullptr ||
        size_ > a_->GetTotalLength() || size_ > b_->GetTotalLength()) {
        state_ = DiffScanState::ReadError;
        return state_;
    }

    BlockCursor ca(a_);
    BlockCursor cb(b_);
    FileOffset budget = (byteBudget > 0) ? byteBudget : 1;
    while (budget > 0 && pos_ < size_) {
        const FileOffset want = (std::min)({size_ - pos_, budget, kReadChunk});
        if (!ca.Seek(pos_, BlockCursor::kBegin, nullptr) ||
            !cb.Seek(pos_, BlockCursor::kBegin, nullptr) ||
            ca.Read(want, bufA_.data()) != want ||
            cb.Read(want, bufB_.data()) != want) {
            state_ = DiffScanState::ReadError;   // 遅延読み込みの元ファイルの読取失敗など
            return state_;
        }
        const size_t n = static_cast<size_t>(want);
        for (size_t i = 0; i < n; ++i) {
            if (bufA_[i] != bufB_[i]) {
                if (runStart_ < 0) {
                    // 新しい相違範囲が始まる。上限件数に達していれば、その手前で打ち切る。
                    if (diffs_.size() >= limit_) {
                        state_ = DiffScanState::Truncated;
                        return state_;
                    }
                    runStart_ = pos_ + static_cast<FileOffset>(i);
                }
            } else if (runStart_ >= 0) {
                if (!PushRange(pos_ + static_cast<FileOffset>(i) - 1)) { return state_; }
            }
        }
        pos_ += want;
        budget -= want;
    }
    if (pos_ >= size_) {
        if (runStart_ >= 0 && !PushRange(size_ - 1)) { return state_; }
        state_ = DiffScanState::Complete;
    }
    return state_;
}

bool DiffScan::PushRange(FileOffset end) {
    try {
        const StdAllocScope allocScope;
        diffs_.push_back(Range(runStart_, end));
    } catch (const std::bad_alloc&) {
        state_ = DiffScanState::OutOfMemory;
        return false;
    }
    runStart_ = -1;
    return true;
}

void DiffScan::Cancel() {
    if (state_ == DiffScanState::Running) {
        state_ = DiffScanState::Cancelled;
    }
}

}  // namespace stirling
