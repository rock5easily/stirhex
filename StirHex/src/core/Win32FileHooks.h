// Win32FileHooks — ファイル I/O の Win32 API を 1 枚だけ薄く包むラッパ（Issue #180）。
//
// 目的は「テストからだけ I/O を失敗させられるようにする」こと。保存・読込には、
//   実環境でしか起きない分岐（要求より短い WriteFile 成功、途中の ReadFile 失敗、
//   FlushFileBuffers 失敗、置換に失敗して出力先が消える経路）がある。OS 側で自然に
//   起こせないため、対象 API に限定した差込口をここへ置く。
//
// 本体ビルド（STIRLING_TEST_IO_HOOK 未定義）ではフックの API 自体が存在せず、
//   各ラッパは対応する Win32 API の直呼びへ畳まれる。既存の確保失敗注入
//   （STIRLING_TEST_ALLOC_HOOK, Issue #153）と同じ方針。
//
// フックの契約（テスト側）:
//   戻り値 true  = フックが処理を肩代わりした。out 引数を埋めること。本物の API は呼ばない。
//   戻り値 false = 素通し。本物の API を呼ぶ。
//   いずれのフックも nullptr で解除される。ClearFileHooks() で一括解除する。
//   読み書きのバイト数は本物の API と同じく「要求を超えない」ことを守らせる（超えた値は
//   ラッパ側で切り詰める）。呼出側は残り長を size_t で減算するため、破ると巨大値へ反転する。
#pragma once

#include <windows.h>

namespace stirling {
namespace io {

#ifdef STIRLING_TEST_IO_HOOK

// WriteFile の肩代わり。*outWrote に「書けたことにするバイト数」、*outError に
//   失敗時の GetLastError 相当、*outResult に WriteFile の戻り値を入れる。
//   短い成功（*outResult=TRUE かつ *outWrote<want）や 0 バイト成功も表現できる。
using WriteHook = bool (*)(HANDLE h, const void* buf, DWORD want,
                           DWORD* outWrote, DWORD* outError, BOOL* outResult);
// ReadFile の肩代わり。契約は WriteHook と同じ（*outRead=0 は EOF）。
using ReadHook = bool (*)(HANDLE h, void* buf, DWORD want,
                          DWORD* outRead, DWORD* outError, BOOL* outResult);
// FlushFileBuffers の肩代わり。
using FlushHook = bool (*)(HANDLE h, DWORD* outError, BOOL* outResult);
// ReplaceFileW / MoveFileExW の肩代わり。出力先を実際に削除してから失敗を返す等、
//   ファイルシステムの状態も含めて再現できるようにテスト側へ委ねる。
using ReplaceHook = bool (*)(const wchar_t* target, const wchar_t* temp,
                             DWORD* outError, BOOL* outResult);
using MoveHook = bool (*)(const wchar_t* temp, const wchar_t* target,
                          DWORD* outError, BOOL* outResult);

inline WriteHook   g_writeHook = nullptr;
inline ReadHook    g_readHook = nullptr;
inline FlushHook   g_flushHook = nullptr;
inline ReplaceHook g_replaceHook = nullptr;
inline MoveHook    g_moveHook = nullptr;

inline void SetWriteHook(WriteHook h) { g_writeHook = h; }
inline void SetReadHook(ReadHook h) { g_readHook = h; }
inline void SetFlushHook(FlushHook h) { g_flushHook = h; }
inline void SetReplaceHook(ReplaceHook h) { g_replaceHook = h; }
inline void SetMoveHook(MoveHook h) { g_moveHook = h; }

inline void ClearFileHooks() {
    g_writeHook = nullptr;
    g_readHook = nullptr;
    g_flushHook = nullptr;
    g_replaceHook = nullptr;
    g_moveHook = nullptr;
}

#endif  // STIRLING_TEST_IO_HOOK

// ---- ラッパ（呼び出し側はこちらを使う。本体ビルドでは直呼びと同じ）----

inline BOOL Write(HANDLE h, const void* buf, DWORD want, DWORD* outWrote) {
#ifdef STIRLING_TEST_IO_HOOK
    if (g_writeHook != nullptr) {
        DWORD wrote = 0, err = 0;
        BOOL result = FALSE;
        if (g_writeHook(h, buf, want, &wrote, &err, &result)) {
            // 本物の WriteFile は「書けたバイト数 <= 要求」を保証する。フックの実装ミスで
            //   これを破ると、呼出側の `left -= wrote`（size_t）が巨大値へ反転して
            //   バッファ外を触りに行く。差込口の側で切り詰めて不変条件を守る。
            if (wrote > want) { wrote = want; }
            *outWrote = wrote;
            if (!result) { ::SetLastError(err); }
            return result;
        }
    }
#endif
    return ::WriteFile(h, buf, want, outWrote, nullptr);
}

inline BOOL Read(HANDLE h, void* buf, DWORD want, DWORD* outRead) {
#ifdef STIRLING_TEST_IO_HOOK
    if (g_readHook != nullptr) {
        DWORD got = 0, err = 0;
        BOOL result = FALSE;
        if (g_readHook(h, buf, want, &got, &err, &result)) {
            if (got > want) { got = want; }   // Write と同じ理由（要求を超えさせない）
            *outRead = got;
            if (!result) { ::SetLastError(err); }
            return result;
        }
    }
#endif
    return ::ReadFile(h, buf, want, outRead, nullptr);
}

inline BOOL Flush(HANDLE h) {
#ifdef STIRLING_TEST_IO_HOOK
    if (g_flushHook != nullptr) {
        DWORD err = 0;
        BOOL result = FALSE;
        if (g_flushHook(h, &err, &result)) {
            if (!result) { ::SetLastError(err); }
            return result;
        }
    }
#endif
    return ::FlushFileBuffers(h);
}

inline BOOL Replace(const wchar_t* target, const wchar_t* temp) {
#ifdef STIRLING_TEST_IO_HOOK
    if (g_replaceHook != nullptr) {
        DWORD err = 0;
        BOOL result = FALSE;
        if (g_replaceHook(target, temp, &err, &result)) {
            if (!result) { ::SetLastError(err); }
            return result;
        }
    }
#endif
    return ::ReplaceFileW(target, temp, nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS,
                          nullptr, nullptr);
}

inline BOOL Move(const wchar_t* temp, const wchar_t* target) {
#ifdef STIRLING_TEST_IO_HOOK
    if (g_moveHook != nullptr) {
        DWORD err = 0;
        BOOL result = FALSE;
        if (g_moveHook(temp, target, &err, &result)) {
            if (!result) { ::SetLastError(err); }
            return result;
        }
    }
#endif
    return ::MoveFileExW(temp, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
}

}  // namespace io
}  // namespace stirling
