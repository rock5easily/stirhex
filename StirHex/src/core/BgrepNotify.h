// BGREP のワーカスレッド → UI 通知で受け渡す情報（Issue #156）。
//
// 以前はファイルサイズ・ヒット位置を WPARAM へ直接格納していた。WPARAM は Win32 で
// 32bit のため、4GB 以上のサイズ・位置は下位32bitへ切り詰められ、
//   - 4GB の倍数は wParam==0 となり、受信側で「アクセス拒否」と誤判定される
//   - 4GB 以降のヒット位置が先頭 4GB 内の位置として記録・表示される
// という欠損が起きる。位置・長さは core と同じ FileOffset(64bit) のまま運ぶ。
//
// 寿命・所有権:
//   通知は同一プロセス内の SendMessageW（同期）で送る。受信側の処理が終わるまで
//   送信側は戻らないため、本構造体はワーカスレッドのスタック上に置けば足り、
//   動的確保も解放も要らない（キャンセル時の解放漏れも起こらない）。
//   PostMessage（非同期）へ変える場合は、この前提が崩れるため所有権の設計から
//   やり直すこと。
//
// MFC 非依存に保つこと（コアテストからそのまま検証できるようにするため）。
#pragma once

#include "CoreTypes.h"

namespace stirling {

// 1 ファイルの走査開始通知（WM_BGREP_SCAN の LPARAM）。
struct BgrepScanNotify {
    const wchar_t* path = nullptr;   // フルパス（送信側が保持する文字列を指す）
    FileOffset     size = 0;         // 対象ファイルのサイズ。0 は開けない／空
};

// 1 件のヒット通知（WM_BGREP_HIT の LPARAM）。
struct BgrepHitNotify {
    const wchar_t* path = nullptr;   // フルパス（送信側が保持する文字列を指す）
    FileOffset     pos = 0;          // ファイル先頭からのヒット位置
};

}  // namespace stirling
