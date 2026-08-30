// BlockCursor — BlockList への位置カーソル。原 Stirling 1.31 の編集モデル中核。
//   原構造: {+0x00 BlockList*, +0x04 現ノード, +0x08 ノード内オフセット}
//   参照: analysis_artifacts/docs/05_BlockCursor_edit_ops.md
//
// 絶対バイト位置 = 「先頭から現ノードまでの usedLen 合計 + ノード内オフセット」。
// ランダムアクセスは Seek で (node, intra-offset) に解決する。
//
// x64 化(Issue #19): 原ドキュメントでは絶対位置・長さがすべて int だが、
// 本移植では FileOffset(64bit) へ置換している（2GB 超のファイル対応）。
// ノード内オフセット(curOffset_)・ブロック長は 16KB 上限のため int のままとし、
// 境界で明示的に変換する。
#pragma once

#include "BlockList.h"
#include "CoreTypes.h"

namespace stirling {

class BlockCursor {
public:
    // Seek の起点（原: 0=先頭 / 1=現在 / 2=末尾）。
    enum Origin { kBegin = 0, kCurrent = 1, kEnd = 2 };
    // 検索方向（原 BlockCursor_SearchPattern の param_4: 0=前方 / 非0=後方）。
    enum SearchDir { kForward = 0, kBackward = 1 };

    explicit BlockCursor(BlockList* list);

    // 位置解決（原 BlockCursor_Seek 0x0041b237）。
    //   pos とorigin から絶対位置を求め、現ノード/オフセットへ格納する。
    //   outAbs != nullptr のとき解決した絶対位置を書き出す。成功で true。
    // origin==kBegin は編集プリミティブが使用する主経路で完全再現・テスト済。
    // origin==kCurrent は絶対位置算出を単体テスト済み。kEnd は逆コンパイルの逐語移植。
    bool Seek(FileOffset pos, int origin, FileOffset* outAbs);

    // カーソル位置から count バイトを複数ブロック跨ぎで dst へ読取り、読取バイト数を返す
    //   （原 BlockCursor_Read 0x0041b7fb）。カーソルは前進しない。
    // count はファイル全体に対する長さになり得るため FileOffset。
    // 呼出側は dst に count バイト分の領域を確保しておくこと。
    // 契約: count >= 0。負値の防御は意図的に行わない（原 0x0041b7fb も未防御）。
    //   呼出側から負値が渡る経路は存在しないため、ガードを追加していない:
    //     - CStirlingDoc::DeleteRange / ReadRange は先頭で count<=0 を弾く
    //     - CStirlingDoc::ReplaceRange は delLen<0 を弾き、delLen>0 のときのみ呼ぶ
    //     - CStirlingDoc::ApplyRecord の r.count は doc 自身が積む Undo レコード長で常に非負
    //     - CStirlingView の描画は 0..bytesPerRow(最大 sizeof(buf)) にクランプ
    //   （負値を渡すと memmove へ size_t 変換された巨大長が渡るため、契約側で担保する）
    FileOffset Read(FileOffset count, void* dst);

    // 絶対位置 pos へ count バイト挿入（原 BlockCursor_Insert 0x0041ccbd → Worker 0x0041cd40）。
    // メモリ確保に失敗した場合は false を返し、リストは呼出前のまま変化しない（Issue #153）。
    bool Insert(FileOffset pos, const void* src, FileOffset count);

    // 絶対位置 pos へ1バイト挿入。ブロック満杯時は半分に分割して挿入
    //   （原 BlockCursor_InsertByte 0x0041c238）。
    // Insert と同じく、確保失敗時は false を返しリストは無変更（Issue #153）。
    bool InsertByte(FileOffset pos, unsigned char b);

    // 絶対位置 pos の1バイトを削除。outByte に削除バイトを返す
    //   （原 BlockCursor_DeleteByte 0x0041c4ef）。
    bool DeleteByte(FileOffset pos, unsigned char* outByte);

    // 絶対位置 pos から count バイトを一括削除し、実際に削除できたバイト数を返す
    //   （移植独自。原は DeleteByte の反復のみ。Issue #62）。
    // ブロック境界単位でノードを除去し、部分的に残るブロックだけ memmove で詰めるため、
    // DeleteByte の反復（1バイトごとに先頭から Seek）と違い削除量に対して線形で済む。
    // 結果のブロック構造は DeleteByte を count 回呼んだ場合と一致する:
    //   - 完全に削除されるノードは除去する。ただし最後の 1 ノードは usedLen=0 で残す
    //   - 部分的に残るノードは左詰めし、末尾の空き領域を 0 クリアする
    // count<=0、または pos が解決できない場合は 0 を返す（リストは変更しない）。
    // 削除後、カーソルは pos（新しい総長を超える場合は総長）へ再解決される。
    FileOffset DeleteRange(FileOffset pos, FileOffset count);

    // 絶対位置 pos のバイトを取得（原 BlockCursor_GetByteAt 0x0041b949）。
    //   前回位置(curAbs_)からの増分移動でブロック内アクセスを高速化する。成功で true。
    bool GetByteAt(FileOffset pos, unsigned char* out);

    // 絶対位置 pos のバイトを in-place で書換える（原 上書き編集 OverwriteByteAtCaret 相当）。
    //   pos は既存データ内(pos<総長)であること。成功で true。
    bool SetByteAt(FileOffset pos, unsigned char b);

    // 絶対位置 pos から count バイトを、既存データ上へ一括で上書きする（Issue #154）。
    //   実際に上書きできたバイト数を返す。ドキュメント長は変わらず、確保も発生しない
    //   （＝メモリ不足で失敗しない）。データ末尾を越える分は書かずに打ち切る。
    // SetByteAt を count 回呼ぶのと結果は同じだが、ブロックを 1 度だけ辿るため
    //   （SetByteAt は 1 バイトごとに先頭から Seek し直す）長い範囲でも線形で済む。
    FileOffset Write(FileOffset pos, const void* src, FileOffset count);

    // 絶対位置 pos から count バイトを定数 value で埋める（範囲初期化。Issue #154）。
    //   Write と同じく長さ不変・確保なし。呼出側で count バイトの一時バッファを
    //   組み立てる必要が無いため、Win32 でも選択範囲の大きさに依存せず実行できる。
    FileOffset FillRange(FileOffset pos, FileOffset count, unsigned char value);

    // Boyer-Moore-Horspool 検索（原 BlockCursor_SearchPattern 0x0041d2d5）。
    //   direction=kForward/kBackward, start=開始位置, end=終端(0で全長, 前方時のみ自動補完)。
    //   一致すれば *outPos に位置を格納し true。対話検索/BGREP の共通コア。
    // patternLen はメモリ上の検索パターン長（ファイル位置ではない）ため int のまま。
    bool SearchPattern(const unsigned char* pattern, int patternLen, FileOffset* outPos,
                       int direction, FileOffset start, FileOffset end);

    // 不一致検索（原 FUN_0041d6bf）: 指定 value に一致しない最初のバイトを走査。
    //   direction=kForward/kBackward, start=開始位置, end=終端(前方は0で全長)。
    //   後方は start!=0 のときのみ走査（原挙動）。一致すれば *outPos に格納し true。
    bool SearchMismatch(unsigned char value, FileOffset* outPos,
                        int direction, FileOffset start, FileOffset end);

    BlockNode* CurNode() const { return curNode_; }
    int        CurOffset() const { return curOffset_; }  // ノード内オフセット(<=16KB)
    BlockList* List() const { return list_; }

private:
    // 挿入の実体（原 BlockCursor_InsertWorker 0x0041cd40）。
    // curUsedLen/capacity はブロック内の値のため int。insertCount のみ 64bit。
    // 必要なブロックを先に全て確保し、成功時のみリストを更新する（全か無か。Issue #153）。
    bool InsertWorker(int curUsedLen, int capacity, unsigned char* data,
                      FileOffset insertCount, const unsigned char* src);

    BlockList* list_;
    BlockNode* curNode_;
    int        curOffset_;  // ノード内オフセット（0..capacity, 16KB 上限のため int）
    FileOffset curAbs_;     // 原 +0xc: 直近アクセスの絶対位置キャッシュ(GetByteAt/Search で使用)
};

}  // namespace stirling
