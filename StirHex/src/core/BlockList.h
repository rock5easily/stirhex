// BlockList / BlockNode — Stirling のブロックロープ（ピーステーブル的）編集モデルの土台。
// 原 Stirling 1.31 の独自データ構造（MFC 非依存）を忠実に移植したもの。
//   原アドレス: BlockNode(0x0040d9c0..), BlockList(0x0040da47..) 系
//   参照: analysis_artifacts/docs/04_BlockList_and_IO.md
//
// ファイル全体を最大 16KB のブロック列（循環双方向リスト）として保持し、
// 挿入・削除をブロック単位のノード操作＋ブロック内シフトで実現する。
// これにより大容量ファイルでも全体バイトシフトを避けて高速に編集できる。
#pragma once

#include <cstddef>

#include "CoreTypes.h"

namespace stirling {

// 1ブロックの確保容量（原 exe と同じ 16KB）。
constexpr int kBlockCapacity = 0x4000;

// 16KB ブロック1個を表すノード（原 24 バイト構造・vtable は移植では省略）。
//   原レイアウト: +0x04 prev, +0x08 next, +0x0c data, +0x10 capacity, +0x14 usedLen
struct BlockNode {
    BlockNode*      prev = nullptr;   // 前ノード（原 +0x04）
    BlockNode*      next = nullptr;   // 次ノード（原 +0x08）
    unsigned char*  data = nullptr;   // ブロックデータ（ヒープ, capacity バイト確保）（原 +0x0c）
    int             capacity = 0;     // 確保容量（通常 kBlockCapacity）（原 +0x10）
    int             usedLen = 0;      // 有効バイト数（原 +0x14）
};

// センチネルノードを内包する循環双方向リスト。
// 原 Stirling では BlockList 自身がセンチネル BlockNode を兼ねる（+0x04=tail, +0x08=head, +0x18=count）。
// 移植では専用センチネルを内部に持ち、走査系はセンチネルを検出して nullptr を返す。
class BlockList {
public:
    BlockList();
    ~BlockList();

    BlockList(const BlockList&) = delete;
    BlockList& operator=(const BlockList&) = delete;

    // 全ノードとブロックデータを解放し、空リストに戻す（原 BlockList_Clear 0x0040db02）。
    void Clear();

    // 空リストか（原 FUN_0041da00 = count==0）。
    bool IsEmpty() const { return count_ == 0; }
    int  Count() const { return count_; }

    // 走査系。末尾・先頭を越えると nullptr を返す。
    BlockNode* GetHead() const;                    // 原 BlockList_GetHead 0x0040dbf0
    BlockNode* GetTail() const;                    // 原 FUN_0041d9d0（空なら nullptr）
    BlockNode* GetNext(const BlockNode* n) const;  // 原 BlockList_GetNext 0x0040dbc0
    BlockNode* GetPrev(const BlockNode* n) const;  // 原 BlockList_GetPrev 0x0041d9a0

    // 末尾（センチネル直前）へノードを追加（原 BlockList_AppendBlock 0x00403430）。
    BlockNode* AppendBlock(unsigned char* data, int capacity, int usedLen);

    // 指定ノードの直後／直前へ新ノードを挿入して返す
    //   （原 BlockList_InsertNodeAfter 0x0041dc90 / InsertNodeBefore 0x0041dba0）。
    // data/capacity/usedLen がすべて非ゼロのときのみデータを設定する（原挙動）。
    BlockNode* InsertNodeAfter(BlockNode* at, unsigned char* data, int capacity, int usedLen);
    BlockNode* InsertNodeBefore(BlockNode* at, unsigned char* data, int capacity, int usedLen);

    // ノードをリストから除去し、保持していたブロックデータのポインタを返す
    //   （原 BlockList_RemoveNode 0x0041dd80。呼出側が data を解放する契約）。
    unsigned char* RemoveNode(BlockNode* n);

    // 全ノードの usedLen 合計（原 BlockList_GetTotalLength 0x0041d288）。
    // 原は 32bit 合計だが、2GB 超のファイルを扱えるよう FileOffset(64bit) で返す。
    FileOffset GetTotalLength() const;

    // センチネル（テスト・カーソル境界判定用の内部公開）。
    const BlockNode* Sentinel() const { return &sentinel_; }

private:
    BlockNode sentinel_;  // prev=tail, next=head。空状態は prev=next=self。
    int       count_ = 0;
};

}  // namespace stirling
