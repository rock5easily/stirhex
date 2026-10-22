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
//
// 遅延読み込み(Issue #300): リストはファイル参照ノード（BlockList::IsFileRef。最大 1GB）を
// 含みうる。読み取り系は参照ノードを元ファイルから読む。変更系は、変更する位置を含む
// 参照ノードの区間を先に 16KB ブロックへ読み込んで（実体化して）から従来の処理を行う。
// 実体化はメモリ確保と元ファイルの読み取りを伴うため、変更系はその失敗でも false / 0 を
// 返す。その場合もリストの内容（バイト列）は呼出前のまま変わらない。
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
    // 編集でカーソルが動いてキャッシュが無効になっている場合は先頭から解決し直すため、
    //   任意の順序で呼んでも結果は Seek+Read と一致する（Issue #182）。
    bool GetByteAt(FileOffset pos, unsigned char* out);

    // 絶対位置 pos のバイトを in-place で書換える（原 上書き編集 OverwriteByteAtCaret 相当）。
    //   pos は既存データ内(pos<総長)であること。成功で true。
    bool SetByteAt(FileOffset pos, unsigned char b);

    // 絶対位置 pos から count バイトを、既存データ上へ一括で上書きする（Issue #154）。
    //   実際に上書きできたバイト数を返す。ドキュメント長は変わらない。
    //   データ末尾を越える分は書かずに打ち切る。確保は参照ノードの実体化でだけ発生し、
    //   失敗したら 1 バイトも書かずに 0 を返す（Issue #300）。
    // SetByteAt を count 回呼ぶのと結果は同じだが、ブロックを 1 度だけ辿るため
    //   （SetByteAt は 1 バイトごとに先頭から Seek し直す）長い範囲でも線形で済む。
    FileOffset Write(FileOffset pos, const void* src, FileOffset count);

    // 絶対位置 pos から count バイトを定数 value で埋める（範囲初期化。Issue #154）。
    //   Write と同じく長さ不変（確保は参照ノードの実体化のみ）。呼出側で count バイトの一時バッファを
    //   組み立てる必要が無いため、Win32 でも選択範囲の大きさに依存せず実行できる。
    FileOffset FillRange(FileOffset pos, FileOffset count, unsigned char value);

    // Boyer-Moore-Horspool 検索（原 BlockCursor_SearchPattern 0x0041d2d5）。
    //   direction=kForward/kBackward, start=開始位置, end=終端(0で全長, 前方時のみ自動補完)。
    //   一致すれば *outPos に位置を格納し true。対話検索/BGREP の共通コア。
    // patternLen はメモリ上の検索パターン長（ファイル位置ではない）ため int のまま。
    // wildcard は patternLen 要素の表で、非0の位置は任意のバイトに一致する（Issue #233）。
    //   nullptr なら従来どおりの完全一致検索（原と同一の動作）。
    bool SearchPattern(const unsigned char* pattern, int patternLen, FileOffset* outPos,
                       int direction, FileOffset start, FileOffset end,
                       const unsigned char* wildcard = nullptr);

    // 不一致検索（原 FUN_0041d6bf）: 指定 value に一致しない最初のバイトを走査。
    //   direction=kForward/kBackward, start=開始位置, end=終端(前方は0で全長)。
    //   後方は start!=0 のときのみ走査（原挙動）。一致すれば *outPos に格納し true。
    bool SearchMismatch(unsigned char value, FileOffset* outPos,
                        int direction, FileOffset start, FileOffset end);

    // 直近の SearchPattern / SearchMismatch が、実データ内の位置を読めずに打ち切ったか
    //   （遅延読み込みの読取エラー・ページキャッシュの確保失敗。Issue #312）。
    //   検索は false を返すため、呼出側はこれで「見つからない」と区別する。
    bool ReadFailed() const { return readFailed_; }

    BlockNode* CurNode() const { return curNode_; }
    int        CurOffset() const { return curOffset_; }  // ノード内オフセット(<=16KB)
    BlockList* List() const { return list_; }

private:
    // 挿入の実体（原 BlockCursor_InsertWorker 0x0041cd40）。
    // curUsedLen/capacity はブロック内の値のため int。insertCount のみ 64bit。
    // 必要なブロックを先に全て確保し、成功時のみリストを更新する（全か無か。Issue #153）。
    bool InsertWorker(int curUsedLen, int capacity, unsigned char* data,
                      FileOffset insertCount, const unsigned char* src);

    // ---- 遅延読み込み（Issue #300） ----
    // 絶対位置 [pos, pos+count) に重なる参照ノードの区間を 16KB ブロックへ読み込む。
    //   全か無か: 確保・読み取りに失敗したらリストを変更せずに false。
    bool MaterializeRange(FileOffset pos, FileOffset count);
    // 挿入位置 pos（総長と等しくてもよい）の前後を、InsertWorker / InsertByte が
    //   データを持つブロックとして扱えるようにする。失敗で false（リストは無変更）。
    bool PrepareInsertAt(FileOffset pos);
    // 現ノード/オフセットのバイトを読む（参照ノードは窓を介して読む）。
    bool ByteAtCursor(unsigned char* out);
    // 検索用の GetByteAt。実データ内（0 <= pos < total）で読めなければ readFailed_ を立てる。
    bool SearchByteAt(FileOffset pos, FileOffset total, unsigned char* out);

    BlockList* list_;
    BlockNode* curNode_;
    int        curOffset_;  // ノード内オフセット（0..capacity, 16KB 上限のため int）
    FileOffset curAbs_;     // 原 +0xc: 直近アクセスの絶対位置キャッシュ(GetByteAt/Search で使用)
    // curAbs_ が curNode_/curOffset_ と整合しているか（Issue #182）。
    //   原は絶対位置キャッシュの有効性を管理しておらず、編集で curNode_/curOffset_ だけが
    //   動くと GetByteAt の増分移動が誤った位置を読んでいた。移植では無効化フラグを持ち、
    //   無効なら GetByteAt が先頭から解決し直す。検索の内側ループは走査開始時の
    //   Seek(kBegin) で有効化されるため、増分アクセスの速度は保たれる。
    bool       curAbsValid_;
    bool       readFailed_ = false;   // ReadFailed() の値（検索の開始時に下ろす）
    // ByteAtCursor が直近に得た読み取り窓（Issue #300）。ノードの解放・再利用と
    //   キャッシュページの追い出しを、リストの世代と FileSource の世代で検知する。
    const BlockNode*   spanNode_ = nullptr;
    NodeSpan           span_;
    unsigned long long spanVersion_ = 0;
    unsigned long long spanEpoch_ = 0;
};

}  // namespace stirling
