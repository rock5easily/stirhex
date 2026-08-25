// Undo/Redo 履歴の容量管理ポリシー（Issue #30）。
//   巨大な範囲操作の逆操作レコードは削除データと同容量のメモリを保持するため、
//   保持合計に上限を設けて古いものから破棄する。ここは MFC/Win32 に依存しない純関数として
//   切り出し、CStirlingDoc から使う（単体テスト可能にするため）。
#pragma once

#include <cstddef>
#include <vector>

namespace stirling {

// トリム計画: 各スタックの「先頭（最古側）」から破棄する件数と、破棄後の保持合計。
struct UndoTrimPlan {
    std::size_t dropUndoFront = 0;   // Undo スタック先頭＝最も古い編集から破棄する件数
    std::size_t dropRedoFront = 0;   // Redo スタック先頭＝Redo 連鎖の末端から破棄する件数
    unsigned long long remainingBytes = 0;   // 破棄後の保持合計
};

// 保持合計が limit を超えている間、Undo スタック先頭 → Redo スタック先頭の順に破棄する
// 計画を立てる。limit==0 は無制限（何も破棄しない）。
//   各スタックの最後の 1 件は上限を超えていても残す。直近の取り消し／やり直しは
//   常に可能であることを優先するため（上限は目安であり厳密な上界ではない）。
inline UndoTrimPlan PlanUndoTrim(const std::vector<unsigned long long>& undoBytes,
                                 const std::vector<unsigned long long>& redoBytes,
                                 unsigned long long limit) {
    UndoTrimPlan plan;
    unsigned long long total = 0;
    for (unsigned long long n : undoBytes) { total += n; }
    for (unsigned long long n : redoBytes) { total += n; }
    plan.remainingBytes = total;
    if (limit == 0) {
        return plan;   // 無制限
    }
    while (plan.remainingBytes > limit && undoBytes.size() - plan.dropUndoFront > 1) {
        plan.remainingBytes -= undoBytes[plan.dropUndoFront];
        ++plan.dropUndoFront;
    }
    while (plan.remainingBytes > limit && redoBytes.size() - plan.dropRedoFront > 1) {
        plan.remainingBytes -= redoBytes[plan.dropRedoFront];
        ++plan.dropRedoFront;
    }
    return plan;
}

// 保存点（未変更を表す Undo 深さ。負値は到達不能）を、Undo スタック先頭から
// dropUndoFront 件を破棄した後の深さへ付け替える。
//   捨てた範囲に保存点があった場合は到達不能(-1)を返す。
inline int ShiftSavePoint(int cleanUndoSize, std::size_t dropUndoFront) {
    if (cleanUndoSize < 0 || dropUndoFront == 0) {
        return cleanUndoSize;   // 既に到達不能、または破棄なし
    }
    const long long shifted =
        static_cast<long long>(cleanUndoSize) - static_cast<long long>(dropUndoFront);
    return (shifted >= 0) ? static_cast<int>(shifted) : -1;
}

}  // namespace stirling
