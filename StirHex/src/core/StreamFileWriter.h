// StreamFileWriter — 一時ファイルへ逐次書き出し、完了時に出力先へ置換する（Issue #155）。
//   原 CMirrorFile（temp→置換の安全保存）に相当する役割を、MFC 非依存で用意したもの。
//
// 選択範囲の保存・ダンプ保存は、以前は対象範囲全体をメモリへ読み込んでから
// CFile(modeCreate) で書いていた。modeCreate は開いた時点で出力先を切り詰めるため、
// 読取や確保に失敗すると「既存ファイルが空になったまま正常終了する」経路があった。
// 本クラスを介すと、Commit() に到達しない限り出力先は一切変更されない。
//
// 使い方:
//   StreamFileWriter w;
//   if (!w.Open(path).Ok()) { ... }
//   if (!w.Write(p, n).Ok()) { ... }   // 失敗したら Abort（デストラクタでも可）
//   if (!w.Commit().Ok()) { ... }
#pragma once

#include "BlockFileIO.h"   // FileIoResult / FileIoStatus
#include "CoreTypes.h"

#include <string>

namespace stirling {

class StreamFileWriter {
public:
    StreamFileWriter() = default;
    ~StreamFileWriter();

    StreamFileWriter(const StreamFileWriter&) = delete;
    StreamFileWriter& operator=(const StreamFileWriter&) = delete;

    // 出力先と同じディレクトリに一時ファイルを作って開く。出力先には触れない。
    //   既に開いている場合は、先に Abort() して開き直す。
    FileIoResult Open(const wchar_t* path);

    bool IsOpen() const { return handle_ != nullptr; }

    // 一時ファイルへ追記する。1 回の WriteFile 長は内部で上限へ分割する。
    //   失敗しても一時ファイルは残るため、呼出側は Abort()（またはデストラクタ）で破棄する。
    FileIoResult Write(const void* data, size_t size);

    // フラッシュ・クローズしてから出力先へ置換する。ここで初めて出力先が変わる。
    //   成功時は fileSize に書き込んだ総バイト数を格納する。
    FileIoResult Commit();

    // 一時ファイルを閉じて削除する（出力先は変更しない）。
    void Abort();

    // これまでに書き込んだバイト数。
    FileOffset Written() const { return written_; }

private:
    void*        handle_ = nullptr;   // HANDLE（windows.h をヘッダへ持ち込まない）
    std::wstring tempPath_;
    std::wstring targetPath_;
    FileOffset   written_ = 0;
};

}  // namespace stirling
