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
    //   出力先が既存のときは、書き始める前に置換可否（読み取り専用属性・権限・共有違反）を
    //   確認し、駄目なら kOpenFailed を返す。GB 単位を書き終えた後で権限不足に気付く事態を
    //   避けるため（Issue #170）。ディレクトリを指定した場合も kOpenFailed。
    FileIoResult Open(const wchar_t* path);

    bool IsOpen() const { return handle_ != nullptr; }

    // 一時ファイルへ追記する。1 回の WriteFile 長は内部で上限へ分割する。
    //   失敗しても一時ファイルは残るため、呼出側は Abort()（またはデストラクタ）で破棄する。
    FileIoResult Write(const void* data, size_t size);

    // 明示フラッシュ（FlushFileBuffers）・クローズしてから出力先へ置換する。
    //   ここで初めて出力先が変わる。成功時は fileSize に書き込んだ総バイト数を格納する。
    //   フラッシュ失敗（遅延書込エラー）は kWriteFailed として返し、一時ファイルを破棄する
    //   ため出力先は元のまま（Issue #166）。失敗後は Abort() 済みの状態になる。
    //   置換は既存の出力先に対しては ReplaceFileW（属性・代替データストリーム・作成日時を
    //   引き継ぐ。アクセス権は引き継げる場合のみ。共有違反は数回再試行）、新規作成や
    //   ReplaceFileW が失敗した場合は MoveFileExW で行う（Issue #170）。
    //   置換の途中で失敗して出力先が消えた場合は、書いた内容を残すため一時ファイルを
    //   削除せず KeptTempPath() に残す。
    FileIoResult Commit();

    // 一時ファイルを閉じて削除する（出力先は変更しない）。
    void Abort();

    // これまでに書き込んだバイト数。
    FileOffset Written() const { return written_; }

    // Commit が「出力先が消えた状態」で失敗したときだけ、残した一時ファイルのパス。
    //   ReplaceFileW が ERROR_UNABLE_TO_MOVE_REPLACEMENT で失敗し、続く MoveFileExW も
    //   失敗した場合に限る。この一時ファイルは書き込んだ内容の唯一の実体なので削除せず、
    //   呼出側が利用者へ知らせられるようにパスを残す（それ以外の失敗では空。Issue #170）。
    const std::wstring& KeptTempPath() const { return keptTempPath_; }

private:
    void*        handle_ = nullptr;   // HANDLE（windows.h をヘッダへ持ち込まない）
    std::wstring tempPath_;
    std::wstring targetPath_;
    std::wstring keptTempPath_;       // 置換失敗で出力先が消えたときに残した一時ファイル

    FileOffset   written_ = 0;
};

}  // namespace stirling
