// 確保失敗を std::bad_alloc として受け取る区間（Issue #306）。
//
// core は標準どおり「確保失敗 = std::bad_alloc」を前提に catch (const std::bad_alloc&) で
//   巻き戻す（Issue #153 等）。一方 StirHex は MFC を静的リンクしており、MFC の operator new は
//   確保失敗で CMemoryException* を投げる。MFC 自身やダイアログのハンドラは CMemoryException* を
//   前提に後始末を書いている（AfxCallWndProc / CDialog::DoModal の CATCH_ALL）ため、
//   プロセス全体を std::bad_alloc に切り替えることはできない。
//   そこで、bad_alloc を捕捉する try ブロックの中だけこの区間を張り、ホスト（StirHex）の
//   new ハンドラが区間内では std::bad_alloc を、区間外では CMemoryException* を投げる。
//   MFC を使わない環境（コア機能テスト）では何もしない。
//
// 使い方: catch (const std::bad_alloc&) を持つ try ブロックの先頭に置く。
//   try { const StdAllocScope allocScope; buffer.resize(n); } catch (const std::bad_alloc&) { ... }
//   区間はスレッドごと（thread_local）。ワーカースレッドでもそのスレッドの try の中に置くこと。
#pragma once

namespace stirling {

class StdAllocScope {
public:
    StdAllocScope() noexcept { ++Depth(); }
    ~StdAllocScope() { --Depth(); }
    StdAllocScope(const StdAllocScope&) = delete;
    StdAllocScope& operator=(const StdAllocScope&) = delete;

    // このスレッドで区間の中にいるか（入れ子は数で管理する）。
    static bool Active() noexcept { return Depth() > 0; }

private:
    static int& Depth() noexcept {
        static thread_local int depth = 0;
        return depth;
    }
};

}  // namespace stirling
