// BlockFileIO — ファイルと BlockList の相互変換（原 CStirlingDoc の Load/Save 中核）。
//   原関数: CStirlingDoc_ReadFileIntoBlocks(0x433223 の読込部),
//           CStirlingDoc_SaveFile_WriteLoop(0x433f15), RecalcTotalLength(0x434a52)
//   参照: analysis_artifacts/docs/04_BlockList_and_IO.md
//
// MFC(CMirrorFile/CDocument)非依存。実 MFC シェル(CStirlingDoc)は本関数を呼び出す。
// 原の CMirrorFile(temp→置換の安全保存)はフェーズ4以降でシェル側に導入する。
//
// x64 化(Issue #20): 原および移植初期は fopen/fseek/ftell(long=32bit) を用いており
// 2GB 超のファイルでサイズ取得が破綻していた。CreateFileW + GetFileSizeEx +
// ReadFile/WriteFile へ移行し、オフセット・サイズをすべて FileOffset(64bit) で扱う。
// パスはワイド(UTF-16)に統一する（MBCS ビルドの呼出側は CStringW 等で変換して渡す）。
#pragma once

#include "BlockList.h"
#include "CoreTypes.h"

namespace stirling {

// 原の 1.6MB 読取チャンク（0x190000, 16KB の 100 倍）。
constexpr int kReadChunk = 0x190000;

// ファイル I/O の結果種別（呼出側が理由別のメッセージを出すために用いる）。
enum class FileIoStatus {
    kOk = 0,        // 成功
    kOpenFailed,    // オープン失敗（存在しない／権限／共有違反 など）
    kSizeFailed,    // サイズ取得失敗
    kReadFailed,    // 読取失敗
    kWriteFailed,   // 書込失敗（ディスク不足を含む）
    kOutOfMemory,   // ブロック確保に失敗（大容量ファイル）
};

// I/O の結果。失敗理由を握りつぶさず呼出側へ返す。
struct FileIoResult {
    FileIoStatus  status = FileIoStatus::kOk;
    unsigned long systemError = 0;   // GetLastError()（該当しない場合は 0）
    // 判明していればサイズ。Load 成功時は実際に読み込んだバイト数、
    // Save 成功時は書き込んだバイト数、失敗時は判明していれば対象ファイルのサイズ。
    FileOffset    fileSize = 0;

    bool Ok() const { return status == FileIoStatus::kOk; }
};

// ファイルサイズのみを取得する（開く前の事前確認用。例: 巨大ファイルの確認ダイアログ）。
//   成功で true、*outSize にサイズを格納する。失敗時は outErr に理由を格納する。
//   ディレクトリは「サイズを取得できる対象ではない」ため false を返す。
//   outSize / outErr は nullptr を許容する。
bool QueryFileSize(const wchar_t* path, FileOffset* outSize, FileIoResult* outErr);

// 読み込みハンドルの共有モード（原 CFile の share フラグに対応。Issue #120）。
//   原は環境設定「ファイルの排他制御」をこの共有モードとして読み込みハンドルへ適用し、
//   排他が有効な間はそのハンドルを保持し続ける（＝ロックの実体）。別ハンドルで後から
//   ロックを取ると、排他モードでは自分自身の読み込みを弾いてしまう。
enum class FileShareMode {
    kDenyNone = 0,   // 他プロセスの読み書きを許可（原 shareDenyNone）
    kDenyWrite,      // 他プロセスの書込を拒否（原 shareDenyWrite）
    kExclusive,      // 他プロセスの読み書きを拒否（原 shareExclusive）
};

// ファイルを読み込み、16KB ブロック列として list へ格納する（原 ReadFileIntoBlocks 読込部）。
//   - 空ファイルは空の 16KB ブロック 1 個（原挙動: 新規は常に編集可能）。
//   - それ以外は先頭から 16KB ブロックへ分割し、末尾のみ端数ブロック。
// list は空であることを前提とするが、防衛的に先頭でも空へ戻す。
// 失敗時も list を空へ戻してから結果を返す（中途半端なブロック列を残さない）。
// 成功時は必ず 1 個以上のブロックを持つ（空ファイルは空の 16KB ブロック 1 個）。
//
// share       : 開くときの共有モード。他プロセスと競合すると kOpenFailed（systemError は
//               ERROR_SHARING_VIOLATION 等）になる。呼出側が閲覧モードの確認へ分岐できる。
// outKeepHandle: 非 nullptr かつ成功したとき、開いたハンドルの所有権を呼出側へ渡す
//               （共有モードを保ったまま保持し続ける＝排他制御）。呼出側が CloseHandle
//               する責務を負う。型は windows.h をこのヘッダへ持ち込まないため void*。
//               nullptr のときは戻る前に閉じる（従来どおり）。
FileIoResult LoadFileIntoBlocks(BlockList& list, const wchar_t* path,
                                FileShareMode share = FileShareMode::kDenyNone,
                                void** outKeepHandle = nullptr);

// list の各ブロックの usedLen バイトを先頭から順次ファイルへ書き出す
//   （原 SaveFile_WriteLoop の本質: 全ブロックを head→tail で Write）。
//   書込先は StreamFileWriter 経由の一時ファイルで、明示フラッシュ後に出力先へ置換する
//   （原 CMirrorFile と同じ安全保存。Issue #166 / #170）。したがって成功しない限り出力先は
//   変更されない。既存の出力先へ置換できない場合（読み取り専用・権限・使用中）は、書き
//   始める前に kOpenFailed を返す。一時ファイルは出力先と同じディレクトリに作るため、
//   保存中は「元のファイル + 新しい内容」の分のディスク容量が必要になる。
FileIoResult SaveBlocksToFile(const BlockList& list, const wchar_t* path);

// 全ブロックの usedLen 合計（原 RecalcTotalLength = doc+0x84 更新）。
// 原は 32bit だが 2GB 超対応のため FileOffset(64bit)。
FileOffset RecalcTotalLength(const BlockList& list);

}  // namespace stirling
