// BlockList / BlockNode 実装。原 Stirling 1.31 の循環双方向リスト操作を忠実に移植。
#include "BlockList.h"

#include <new>

namespace stirling {

// ---- 確保ヘルパ（Issue #153） ----

#ifdef STIRLING_TEST_ALLOC_HOOK
namespace {
int g_allocFailCountdown = 0;   // >0 のとき、この回数目の確保を失敗させる
}  // namespace

void SetAllocFailCountdown(int n) { g_allocFailCountdown = (n > 0) ? n : 0; }
int  AllocFailCountdown() { return g_allocFailCountdown; }

// 注入が有効なら、この確保を失敗させるべきか判定して1つ消費する。
static bool TestAllocShouldFail() {
    if (g_allocFailCountdown <= 0) { return false; }
    return (--g_allocFailCountdown == 0);
}
#else
static bool TestAllocShouldFail() { return false; }
#endif

unsigned char* AllocBlockData() {
    if (TestAllocShouldFail()) { return nullptr; }
    return new (std::nothrow) unsigned char[kBlockCapacity];
}

BlockNode* AllocBlockNode(unsigned char* data, int capacity, int usedLen) {
    if (TestAllocShouldFail()) { return nullptr; }
    BlockNode* node = new (std::nothrow) BlockNode();
    if (node == nullptr) { return nullptr; }   // data の所有権は呼出側に残る
    node->data = data;
    node->capacity = capacity;
    node->usedLen = usedLen;
    return node;
}

void FreeBlockNode(BlockNode* node) {
    if (node == nullptr) { return; }
    delete[] node->data;
    delete node;
}

BlockList::BlockList() {
    // 空状態: センチネルの prev/next は自分自身（原 BlockList_ctor 0x0040da47）。
    sentinel_.prev = &sentinel_;
    sentinel_.next = &sentinel_;
    count_ = 0;
}

BlockList::~BlockList() {
    Clear();
}

void BlockList::Clear() {
    BlockNode* n = sentinel_.next;
    while (n != &sentinel_) {
        BlockNode* nx = n->next;
        delete[] n->data;
        delete n;
        n = nx;
    }
    sentinel_.prev = &sentinel_;
    sentinel_.next = &sentinel_;
    count_ = 0;
}

BlockNode* BlockList::GetHead() const {
    BlockNode* h = sentinel_.next;
    return (h == &sentinel_) ? nullptr : h;
}

BlockNode* BlockList::GetTail() const {
    BlockNode* t = sentinel_.prev;
    return (t == &sentinel_) ? nullptr : t;
}

BlockNode* BlockList::GetNext(const BlockNode* n) const {
    if (n == nullptr) return nullptr;
    BlockNode* nx = n->next;
    return (nx == &sentinel_) ? nullptr : nx;
}

BlockNode* BlockList::GetPrev(const BlockNode* n) const {
    if (n == nullptr) return nullptr;
    BlockNode* pv = n->prev;
    return (pv == &sentinel_) ? nullptr : pv;
}

// 内部: at の直後に node を連結（at はセンチネルでもよい）。
static void LinkAfter(BlockNode* at, BlockNode* node) {
    BlockNode* nx = at->next;
    node->prev = at;
    node->next = nx;
    nx->prev = node;
    at->next = node;
}

// 内部: 原挙動「3値すべて非ゼロのときのみデータ設定」を適用してノードを確保する。
static BlockNode* AllocNodeWithLegacyDataRule(unsigned char* data, int capacity, int usedLen) {
    const bool withData = (data != nullptr && capacity != 0 && usedLen != 0);
    return withData ? AllocBlockNode(data, capacity, usedLen) : AllocBlockNode(nullptr, 0, 0);
}

void BlockList::LinkNodeAfter(BlockNode* at, BlockNode* node) {
    LinkAfter(at, node);
    ++count_;
}

void BlockList::LinkNodeBefore(BlockNode* at, BlockNode* node) {
    LinkAfter(at->prev, node);  // at の前 = at->prev の後
    ++count_;
}

void BlockList::AppendNode(BlockNode* node) {
    LinkAfter(sentinel_.prev, node);  // 末尾（センチネル直前）へ
    ++count_;
}

BlockNode* BlockList::AppendBlock(unsigned char* data, int capacity, int usedLen) {
    BlockNode* node = AllocBlockNode(data, capacity, usedLen);
    if (node == nullptr) { return nullptr; }   // data の所有権は移さない（Issue #153）
    AppendNode(node);
    return node;
}

BlockNode* BlockList::InsertNodeAfter(BlockNode* at, unsigned char* data, int capacity, int usedLen) {
    BlockNode* node = AllocNodeWithLegacyDataRule(data, capacity, usedLen);
    if (node == nullptr) { return nullptr; }
    LinkNodeAfter(at, node);
    return node;
}

BlockNode* BlockList::InsertNodeBefore(BlockNode* at, unsigned char* data, int capacity, int usedLen) {
    BlockNode* node = AllocNodeWithLegacyDataRule(data, capacity, usedLen);
    if (node == nullptr) { return nullptr; }
    LinkNodeBefore(at, node);
    return node;
}

unsigned char* BlockList::RemoveNode(BlockNode* n) {
    if (count_ == 0 || n == nullptr) return nullptr;
    BlockNode* pv = n->prev;
    BlockNode* nx = n->next;
    pv->next = nx;
    nx->prev = pv;
    unsigned char* data = n->data;  // データは呼出側へ返し、ノード本体のみ破棄（原挙動）。
    delete n;
    --count_;
    return data;
}

FileOffset BlockList::GetTotalLength() const {
    FileOffset total = 0;
    for (const BlockNode* n = sentinel_.next; n != &sentinel_; n = n->next) {
        total += n->usedLen;
    }
    return total;
}

}  // namespace stirling
