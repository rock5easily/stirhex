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
#include <memory>
#include <utility>

#include "CoreTypes.h"

namespace stirling {

class FileSource;

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
    // 遅延読み込み（Issue #300。移植独自）: 元ファイル上の位置。-1 は元ファイルと無関係。
    //   data == nullptr かつ 0 以上なら「ファイル参照ノード」で、元ファイルの
    //   [srcOffset, srcOffset + usedLen) をそのまま表す（capacity は 0）。
    //   data を持つノードでは、元ファイルから読み込んで作ったブロック（実体化ブロック）の
    //   読み込み元を表す。上書き保存（BlockFileIO）が書き戻し先の判定に使う。
    FileOffset      srcOffset = -1;
};

// ファイル参照ノード 1 個の最大長（1GB）。usedLen を int のまま扱えるようにする。
constexpr int kMaxFileRefLen = 0x40000000;

// ノードの連続した読み取り窓（BlockList::NodeWindow）。
//   ノード内オフセット [begin, end) のバイトが ptr[0 .. end-begin) にある。
struct NodeSpan {
    const unsigned char* ptr = nullptr;
    int begin = 0;
    int end = 0;
};

// ---- 確保ヘルパ（Issue #153） ----
// ブロックデータ／ノードの確保はいずれも例外を投げず、失敗時に nullptr を返す。
// メモリ不足は上位へ戻り値で伝え、UI 境界まで std::bad_alloc を伝播させない。

// kBlockCapacity バイトのブロックデータを確保する（失敗時 nullptr）。
unsigned char* AllocBlockData();

// リスト未連結のノードを確保する（失敗時 nullptr。失敗時は data の所有権を移さない）。
BlockNode* AllocBlockNode(unsigned char* data, int capacity, int usedLen);

// AllocBlockNode で得た未連結ノードを、保持データごと解放する（nullptr 可）。
void FreeBlockNode(BlockNode* node);

#ifdef STIRLING_TEST_ALLOC_HOOK
// テスト専用の確保失敗注入。n>0 のとき「n 回目の確保」を失敗させ、以後は無効化する。
// 0 以下を渡すと注入を解除する。本体ビルドではこの API 自体が存在しない。
void SetAllocFailCountdown(int n);
int  AllocFailCountdown();
#endif

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
    // 自分を空にしてから other のノード列と読み出し元を引き取る（other は空になる）。
    //   保存後に元ファイルを読み込み直す際、読み込みに成功したときだけ差し替えるために使う
    //   （Issue #300）。
    void TakeFrom(BlockList& other);

    // ---- 遅延読み込み（Issue #300。移植独自） ----
    // ファイル参照ノードか（data を持たず元ファイルの区間を表す）。
    static bool IsFileRef(const BlockNode* n) {
        return n != nullptr && n->data == nullptr && n->srcOffset >= 0;
    }
    // 参照ノードの読み出し元を設定する（Clear で外れる）。
    void AttachSource(std::shared_ptr<FileSource> source) { source_ = std::move(source); }
    FileSource* Source() const { return source_.get(); }
    bool HasSource() const { return source_ != nullptr; }
    // 元ファイルの [srcOffset, srcOffset+len) を表す参照ノードを末尾へ追加する
    //   （len は 1..kMaxFileRefLen。確保失敗で nullptr）。
    BlockNode* AppendFileRef(FileOffset srcOffset, int len);
    // ノード n のオフセット off を含む連続領域を返す（off は 0..usedLen-1）。
    //   データを持つノードはブロック全体、参照ノードは元ファイルのキャッシュページの範囲。
    //   読み取りに失敗したら false。参照ノードの窓は次に FileSource を使うまで有効。
    bool NodeWindow(const BlockNode* n, int off, NodeSpan* out) const;
    // ノード n の [off, off+len) を dst へ複写する。失敗で false。
    bool ReadNode(const BlockNode* n, int off, int len, void* dst) const;

    // 構造の世代番号。ノードの連結・除去・長さ変更のたびに進む（NodeWindow の結果を
    //   保持する側が、ノードの解放・再利用を検知するために使う）。
    unsigned long long Version() const { return version_; }
    // ノードの usedLen を直接変えた側（BlockCursor）が呼ぶ。総長のキャッシュも捨てる。
    void Touch() { ++version_; totalCache_ = -1; }

    // 空リストか（原 FUN_0041da00 = count==0）。
    bool IsEmpty() const { return count_ == 0; }
    int  Count() const { return count_; }

    // 走査系。末尾・先頭を越えると nullptr を返す。
    BlockNode* GetHead() const;                    // 原 BlockList_GetHead 0x0040dbf0
    BlockNode* GetTail() const;                    // 原 FUN_0041d9d0（空なら nullptr）
    BlockNode* GetNext(const BlockNode* n) const;  // 原 BlockList_GetNext 0x0040dbc0
    BlockNode* GetPrev(const BlockNode* n) const;  // 原 BlockList_GetPrev 0x0041d9a0

    // 末尾（センチネル直前）へノードを追加（原 BlockList_AppendBlock 0x00403430）。
    // ノード確保に失敗すると nullptr を返す。そのとき data の所有権は移らないため、
    // 呼出側が解放する責任を負う（Issue #153）。
    BlockNode* AppendBlock(unsigned char* data, int capacity, int usedLen);

    // 指定ノードの直後／直前へ新ノードを挿入して返す
    //   （原 BlockList_InsertNodeAfter 0x0041dc90 / InsertNodeBefore 0x0041dba0）。
    // data/capacity/usedLen がすべて非ゼロのときのみデータを設定する（原挙動）。
    // AppendBlock と同じく、ノード確保に失敗すると nullptr を返し data の所有権は移らない。
    BlockNode* InsertNodeAfter(BlockNode* at, unsigned char* data, int capacity, int usedLen);
    BlockNode* InsertNodeBefore(BlockNode* at, unsigned char* data, int capacity, int usedLen);

    // AllocBlockNode で先に確保しておいた未連結ノードを連結する（Issue #153）。
    // 確保を伴わないため失敗しない。複数ブロックの挿入をトランザクション化する際、
    // 「全ノードを確保してから、失敗しない連結だけを行う」ために使う。
    void LinkNodeAfter(BlockNode* at, BlockNode* node);
    void LinkNodeBefore(BlockNode* at, BlockNode* node);
    void AppendNode(BlockNode* node);

    // ノードをリストから除去し、保持していたブロックデータのポインタを返す
    //   （原 BlockList_RemoveNode 0x0041dd80。呼出側が data を解放する契約）。
    unsigned char* RemoveNode(BlockNode* n);

    // 全ノードの usedLen 合計（原 BlockList_GetTotalLength 0x0041d288）。
    // 原は 32bit 合計だが、2GB 超のファイルを扱えるよう FileOffset(64bit) で返す。
    // 移植では結果をキャッシュし、構造が変わったとき（Touch）だけ数え直す。
    FileOffset GetTotalLength() const;

    // センチネル（テスト・カーソル境界判定用の内部公開）。
    const BlockNode* Sentinel() const { return &sentinel_; }

private:
    BlockNode sentinel_;  // prev=tail, next=head。空状態は prev=next=self。
    int       count_ = 0;
    std::shared_ptr<FileSource> source_;   // 参照ノードの読み出し元（Issue #300）
    unsigned long long version_ = 1;
    mutable FileOffset totalCache_ = -1;   // -1 = 未計算
};

}  // namespace stirling
