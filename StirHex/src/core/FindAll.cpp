// FindAll 実装（FindAll.h 参照）。
#include "core/FindAll.h"

#include "core/BlockCursor.h"
#include "core/BlockList.h"

#include <algorithm>

namespace stirling {

FindAllState FindAll::Start(BlockList& list, const HexPattern& pattern,
                            FileOffset lo, FileOffset hi, size_t limit) {
    list_ = &list;
    pattern_ = pattern;
    lo_ = lo;
    hi_ = hi;
    next_ = lo;
    lastStart_ = -1;
    limit_ = limit;
    hits_.clear();

    if (lo < 0 || hi < lo || hi > list.GetTotalLength()) {
        state_ = FindAllState::ReadError;
        return state_;
    }
    const FileOffset size = static_cast<FileOffset>(pattern.Size());
    if (size == 0 || limit == 0 || hi - lo < size) {
        state_ = FindAllState::Complete;
        return state_;
    }
    lastStart_ = hi - size;
    state_ = FindAllState::Running;
    return state_;
}

FindAllState FindAll::Step(FileOffset positionBudget) {
    if (state_ != FindAllState::Running) {
        return state_;
    }
    // Step の合間に文書が縮んでいたら、範囲の外を読まずに打ち切る。
    if (list_ == nullptr || hi_ > list_->GetTotalLength()) {
        state_ = FindAllState::ReadError;
        return state_;
    }

    const int size = static_cast<int>(pattern_.Size());
    BlockCursor cursor(list_);
    FileOffset budget = (positionBudget > 0) ? positionBudget : 1;
    while (budget > 0 && next_ <= lastStart_) {
        // 今回調べる先頭候補は [next_, startEnd)。SearchPattern の前方の終端は
        //   「一致の末尾がこの位置より前」なので、先頭候補の上限に長さ-1 を足して渡す。
        const FileOffset startEnd = (std::min)(lastStart_ + 1, next_ + budget);
        FileOffset hit = -1;
        if (cursor.SearchPattern(pattern_.bytes.data(), size, &hit, BlockCursor::kForward,
                                 next_, startEnd + size - 1, pattern_.WildcardData())) {
            if (hits_.size() >= limit_) {
                state_ = FindAllState::Truncated;
                return state_;
            }
            hits_.push_back(hit);
            budget -= hit - next_ + 1;
            next_ = hit + 1;   // 重なった一致も数えるため、1バイト先から続ける
        } else {
            budget -= startEnd - next_;
            next_ = startEnd;
        }
    }
    if (next_ > lastStart_) {
        state_ = FindAllState::Complete;
    }
    return state_;
}

void FindAll::Cancel() {
    if (state_ == FindAllState::Running) {
        state_ = FindAllState::Cancelled;
    }
}

}  // namespace stirling
