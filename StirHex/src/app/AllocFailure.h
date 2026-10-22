// メモリ確保失敗の例外型を、受け取る側の前提に合わせる（Issue #306）。
//
// 静的リンクの MFC は global の operator new を置き換えており、確保に失敗すると
//   AfxNewHandler が CMemoryException* を投げる（std::bad_alloc ではない）。std::vector など
//   標準コンテナの確保もこの operator new を通るため、core 層や編集処理の
//   catch (const std::bad_alloc&)（Issue #153 の巻き戻し等）が素通りされていた。
//   一方 MFC 自身（AfxCallWndProc・CDialog::DoModal の CATCH_ALL など）とダイアログの
//   ハンドラは CMemoryException* を前提にしているため、全体を std::bad_alloc にはできない。
//   そこで、bad_alloc を捕捉する try ブロックの中（stirling::StdAllocScope の区間）だけ
//   std::bad_alloc を投げ、それ以外は MFC 既定どおり CMemoryException* を投げる。
#pragma once

#include <new>
#include <stdexcept>

#include "core/AllocFailureScope.h"

namespace ui {

// AfxSetNewHandler へ渡す new ハンドラ。0 を返さず必ず投げる（再試行しない）。
//   区間外は MFC 既定の AfxNewHandler と同じく AfxThrowMemoryException()。
inline int __cdecl ThrowAllocFailure(size_t /*size*/) {
    if (stirling::StdAllocScope::Active()) {
        throw std::bad_alloc();
    }
    AfxThrowMemoryException();
}

// f() を呼び、捕捉されなかった標準の確保失敗を CMemoryException へ変換して投げ直す。
//   new ハンドラを経由しない std::bad_alloc（new[] の長さあふれ等）と、vector::max_size()
//   超過の std::length_error が対象。MFC の AfxCallWndProc は CException* しか捕捉しないため、
//   標準例外のままウィンドウプロシージャを抜けるとプロセスが異常終了する。
template <class F>
LRESULT TranslateAllocFailure(F&& f) {
    try {
        return f();
    } catch (const std::bad_alloc&) {
        AfxThrowMemoryException();
    } catch (const std::length_error&) {
        AfxThrowMemoryException();
    }
}

}  // namespace ui
