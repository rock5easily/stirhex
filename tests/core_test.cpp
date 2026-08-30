// core 層（BlockList / BlockCursor）単体テスト。
// 線形参照モデル（std::vector）と並行操作し、毎回バイト一致・構造不変条件を検証する。
// ビルド: porting/tests/build_core_test.ps1（cl.exe）または任意の C++17 コンパイラ。
#include "../StirHex/src/core/BlockCursor.h"
#include "../StirHex/src/core/BlockFileIO.h"
#include "../StirHex/src/core/StreamFileWriter.h"
#include "../StirHex/src/core/BgrepNotify.h"
#include "../StirHex/src/core/BlockList.h"
#include "../StirHex/src/app/SettingsCodec.h"
#include "../StirHex/src/app/SettingsMigration.h"
#include "../StirHex/src/app/SettingsStore.h"
#include "../StirHex/src/app/SettingsFile.h"
#include "../StirHex/src/util/PathParts.h"
#include "../StirHex/src/app/MarkFile.h"
#include "../StirHex/src/core/Cp932Text.h"
#include "../StirHex/src/core/CharConv.h"
#include "../StirHex/src/core/StructDef.h"
#include "../StirHex/src/core/UndoBudget.h"
#include "../StirHex/src/core/HexText.h"
#include "../StirHex/src/core/Utf8Text.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <random>
#include <share.h>
#include <thread>
#define NOMINMAX
#include <windows.h>   // GetACP / IsDBCSLeadByte（ACP=932 環境での等価確認に使う）
#undef small           // rpcndr.h の `#define small char` がローカル変数 small と衝突する
#include <string>
#include <utility>
#include <vector>

// windows.h を NOMINMAX 付きで取り込んだ後に含める（このヘッダも windows.h に依存する）。
#include "../StirHex/src/app/ClipboardUtil.h"   // クリップボード転送の RAII（Issue #47）

using stirling::BlockCursor;
using stirling::BlockList;
using stirling::BlockNode;
using stirling::FileOffset;
using stirling::kBlockCapacity;
using stirling::kReadChunk;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(cond)) {                                                        \
            ++g_failures;                                                     \
            std::printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__);   \
        }                                                                     \
    } while (0)

// 新規ドキュメント相当（原 OnNewDocument: 空の16KBブロック1個）。
static BlockNode* NewEmptyDoc(BlockList& list) {
    unsigned char* buf = new unsigned char[kBlockCapacity];
    return list.AppendBlock(buf, kBlockCapacity, 0);
}

// 全内容を先頭から読み出す。
static std::vector<unsigned char> ReadAll(BlockList& list) {
    const FileOffset total = list.GetTotalLength();
    std::vector<unsigned char> buf(total > 0 ? static_cast<size_t>(total) : 0);
    if (total > 0) {
        BlockCursor c(&list);
        bool ok = c.Seek(0, BlockCursor::kBegin, nullptr);
        if (!ok) { std::printf("  FAIL: ReadAll seek failed\n"); ++g_failures; return buf; }
        const FileOffset n = c.Read(total, buf.data());
        if (n != total) {
            std::printf("  FAIL: ReadAll short read %lld/%lld\n",
                        static_cast<long long>(n), static_cast<long long>(total));
            ++g_failures;
        }
    }
    return buf;
}

// 構造不変条件: 各ノード capacity==kBlockCapacity, 0<=usedLen<=capacity, 合計==size。
static void CheckInvariants(BlockList& list, size_t expectedLen, const char* where) {
    FileOffset sum = 0;
    for (BlockNode* n = list.GetHead(); n != nullptr; n = list.GetNext(n)) {
        CHECK(n->capacity == kBlockCapacity, where);
        CHECK(n->usedLen >= 0 && n->usedLen <= n->capacity, where);
        sum += n->usedLen;
    }
    CHECK(sum == static_cast<FileOffset>(expectedLen), where);
    CHECK(list.GetTotalLength() == static_cast<FileOffset>(expectedLen), where);
}

static void CheckEqual(BlockList& list, const std::vector<unsigned char>& ref, const char* where) {
    std::vector<unsigned char> got = ReadAll(list);
    bool eq = (got.size() == ref.size()) &&
              (ref.empty() || std::memcmp(got.data(), ref.data(), ref.size()) == 0);
    if (!eq) {
        ++g_failures;
        std::printf("  FAIL: content mismatch at %s (got %zu bytes, ref %zu)\n",
                    where, got.size(), ref.size());
        size_t lim = got.size() < ref.size() ? got.size() : ref.size();
        for (size_t i = 0; i < lim; ++i) {
            if (got[i] != ref[i]) { std::printf("    first diff at %zu: got %02X ref %02X\n",
                                                i, got[i], ref[i]); break; }
        }
    }
    ++g_checks;
}

// ---- BlockList 基本操作 ----
static void TestBlockListBasics() {
    std::printf("TestBlockListBasics\n");
    BlockList list;
    CHECK(list.IsEmpty(), "new list empty");
    CHECK(list.Count() == 0, "new list count 0");
    CHECK(list.GetHead() == nullptr, "empty head null");
    CHECK(list.GetTail() == nullptr, "empty tail null");
    CHECK(list.GetTotalLength() == 0, "empty total 0");

    BlockNode* a = list.AppendBlock(new unsigned char[kBlockCapacity], kBlockCapacity, 10);
    BlockNode* b = list.AppendBlock(new unsigned char[kBlockCapacity], kBlockCapacity, 20);
    CHECK(list.Count() == 2, "count 2");
    CHECK(list.GetHead() == a, "head a");
    CHECK(list.GetTail() == b, "tail b");
    CHECK(list.GetNext(a) == b, "next(a)=b");
    CHECK(list.GetNext(b) == nullptr, "next(b)=null");
    CHECK(list.GetPrev(b) == a, "prev(b)=a");
    CHECK(list.GetPrev(a) == nullptr, "prev(a)=null");
    CHECK(list.GetTotalLength() == 30, "total 30");

    // 中間挿入
    BlockNode* m = list.InsertNodeAfter(a, new unsigned char[kBlockCapacity], kBlockCapacity, 5);
    CHECK(list.GetNext(a) == m, "next(a)=m");
    CHECK(list.GetNext(m) == b, "next(m)=b");
    CHECK(list.Count() == 3, "count 3");

    BlockNode* pre = list.InsertNodeBefore(a, new unsigned char[kBlockCapacity], kBlockCapacity, 7);
    CHECK(list.GetHead() == pre, "head pre");
    CHECK(list.GetNext(pre) == a, "next(pre)=a");
    CHECK(list.Count() == 4, "count 4");

    // 除去（データは呼出側が解放）
    unsigned char* data = list.RemoveNode(m);
    delete[] data;
    CHECK(list.GetNext(a) == b, "after remove next(a)=b");
    CHECK(list.Count() == 3, "count 3 after remove");
}

// ---- Insert(多バイト) の well-defined 経路 ----
static void TestMultiByteInsert() {
    std::printf("TestMultiByteInsert\n");
    BlockList list;
    NewEmptyDoc(list);
    std::vector<unsigned char> ref;
    BlockCursor c(&list);

    // 1) 空ブロックへ収まる挿入
    const char* s1 = "Hello, Stirling";
    CHECK(c.Insert(0, s1, 15), "insert s1");
    ref.insert(ref.end(), s1, s1 + 15);
    CheckEqual(list, ref, "after s1");
    CheckInvariants(list, ref.size(), "after s1");

    // 2) 途中へ挿入（ブロック内右シフト）
    const char* s2 = "[MID]";
    CHECK(c.Insert(5, s2, 5), "insert s2");
    ref.insert(ref.begin() + 5, s2, s2 + 5);
    CheckEqual(list, ref, "after s2");

    // 3) 末尾へ挿入
    const char* s3 = "!END";
    CHECK(c.Insert(static_cast<int>(ref.size()), s3, 4), "insert s3 at end");
    ref.insert(ref.end(), s3, s3 + 4);
    CheckEqual(list, ref, "after s3");

    // 4) 容量超過を伴う大量挿入（複数16KBブロックへ分割される）
    std::vector<unsigned char> big(40000);
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<unsigned char>(i * 37 + 11);
    CHECK(c.Insert(3, big.data(), static_cast<int>(big.size())), "insert big");
    ref.insert(ref.begin() + 3, big.begin(), big.end());
    CheckEqual(list, ref, "after big");
    CheckInvariants(list, ref.size(), "after big");
    CHECK(list.Count() >= 3, "multiple blocks after big insert");

    // 5) 途中(ブロック分割: curOffset がブロック内部)への挿入
    std::vector<unsigned char> mid(1000);
    for (size_t i = 0; i < mid.size(); ++i) mid[i] = static_cast<unsigned char>(200 - (i & 0x3F));
    int at = 8000;  // big の内部（curOffset は usedLen-1 未満で分割経路）
    CHECK(c.Insert(at, mid.data(), static_cast<int>(mid.size())), "insert mid split");
    ref.insert(ref.begin() + at, mid.begin(), mid.end());
    CheckEqual(list, ref, "after mid split");
    CheckInvariants(list, ref.size(), "after mid split");
}

// ---- Read の跨ぎ読取・部分読取 ----
static void TestRead() {
    std::printf("TestRead\n");
    BlockList list;
    NewEmptyDoc(list);
    std::vector<unsigned char> ref;
    BlockCursor c(&list);

    std::vector<unsigned char> data(50000);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<unsigned char>((i * 7) ^ 0xA5);
    CHECK(c.Insert(0, data.data(), static_cast<int>(data.size())), "insert 50k");
    ref = data;

    // 任意位置から任意長の部分読取
    int positions[] = {0, 1, 100, 16383, 16384, 16385, 30000, 49999};
    for (int p : positions) {
        int len = 1234;
        if (p + len > static_cast<int>(ref.size())) len = static_cast<int>(ref.size()) - p;
        std::vector<unsigned char> buf(len);
        BlockCursor rc(&list);
        CHECK(rc.Seek(p, BlockCursor::kBegin, nullptr), "read seek");
        const FileOffset n = rc.Read(len, buf.data());
        CHECK(n == len, "read length");
        bool eq = std::memcmp(buf.data(), ref.data() + p, len) == 0;
        CHECK(eq, "read content");
    }

    // 全長を超える読取要求は残り全部だけ返す
    BlockCursor rc(&list);
    CHECK(rc.Seek(49990, BlockCursor::kBegin, nullptr), "seek near end");
    std::vector<unsigned char> buf(100);
    const FileOffset n = rc.Read(100, buf.data());
    CHECK(n == 10, "read clamps to remaining");
}

// ---- Seek origin=0 の絶対位置解決 & origin=1(pos=0) の絶対位置算出 ----
static void TestSeek() {
    std::printf("TestSeek\n");
    BlockList list;
    NewEmptyDoc(list);
    BlockCursor c(&list);
    std::vector<unsigned char> data(40000, 0);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<unsigned char>(i);
    CHECK(c.Insert(0, data.data(), static_cast<int>(data.size())), "insert 40k");

    int probe[] = {0, 1, 16383, 16384, 16385, 32768, 39999, 40000 /*EOF追記位置*/};
    for (int p : probe) {
        BlockCursor sc(&list);
        FileOffset abs = -1;
        CHECK(sc.Seek(p, BlockCursor::kBegin, &abs), "seek ok");
        CHECK(abs == p, "seek outAbs matches pos");
        // 解決したノード/オフセットの絶対位置を origin=1,pos=0 で逆算
        FileOffset abs2 = -1;
        CHECK(sc.Seek(0, BlockCursor::kCurrent, &abs2), "current seek ok");
        CHECK(abs2 == p, "current abs matches");
    }

    // 範囲外は失敗
    BlockCursor sc(&list);
    CHECK(!sc.Seek(40001, BlockCursor::kBegin, nullptr), "seek past end fails");
    CHECK(!sc.Seek(-1, BlockCursor::kBegin, nullptr), "seek negative fails");
}

// 満杯ブロックを構築するヘルパ（16384バイトちょうど）。
static void FillFullBlock(BlockList& list, BlockCursor& c, std::vector<unsigned char>& ref) {
    std::vector<unsigned char> full(kBlockCapacity);
    for (int i = 0; i < kBlockCapacity; ++i) full[i] = static_cast<unsigned char>(i * 3 + 1);
    CHECK(c.Insert(0, full.data(), kBlockCapacity), "fill full block");
    ref = full;
    CHECK(list.Count() == 1 && list.GetHead()->usedLen == kBlockCapacity, "one full block");
}

// ---- InsertByte のブロック分割（満杯ブロックの各位置への挿入）----
static void TestInsertByteSplit() {
    std::printf("TestInsertByteSplit\n");
    // Issue #93 の修正で、最終バイト上(16383)と EOF 追記位置(16384)を含む
    // 全位置が素直な挿入と一致する。
    for (int pos : {0, 1, 4000, 8191, 8192, 12000, 16382, 16383, 16384}) {
        BlockList list;
        NewEmptyDoc(list);
        BlockCursor c(&list);
        std::vector<unsigned char> ref;
        FillFullBlock(list, c, ref);

        CHECK(c.InsertByte(pos, 0xEE), "insert byte into full block");
        ref.insert(ref.begin() + pos, 0xEE);
        char where[64];
        std::snprintf(where, sizeof(where), "InsertByte split pos=%d", pos);
        CheckEqual(list, ref, where);
        CheckInvariants(list, ref.size(), where);
        CHECK(list.Count() == 2, "split produced 2 blocks");
    }
}

// ---- 満杯ブロックの最終バイト上への InsertByte（Issue #93 回帰）----
// 原 BlockCursor_InsertByte(0x0041c238) は curOffset==used-1 に特殊分岐を持ち、
// 「後半全部 + b」と組み立てて挿入バイトをブロック末尾へ後置していた。結果、挿入バイトと
// 既存の最終バイトが入れ替わる（無警告のデータ破壊）。移植では特殊分岐を廃したため、
// 通常の分割式どおり素直な挿入位置へ収まることを固定する。
static void TestInsertByteFullBlockLastPos() {
    std::printf("TestInsertByteFullBlockLastPos\n");
    BlockList list;
    NewEmptyDoc(list);
    BlockCursor c(&list);
    std::vector<unsigned char> ref;
    FillFullBlock(list, c, ref);

    const unsigned char lastByte = ref[kBlockCapacity - 1];  // 元ブロックの最終バイト
    CHECK(c.InsertByte(kBlockCapacity - 1, 0xEE), "insert byte at used-1");

    // 期待: 位置16383 が 0xEE、元の最終バイトは 16384 へ押し出される。
    std::vector<unsigned char> expected = ref;
    expected.insert(expected.begin() + (kBlockCapacity - 1), 0xEE);
    CheckEqual(list, expected, "InsertByte at used-1");
    CheckInvariants(list, expected.size(), "InsertByte at used-1");
    CHECK(list.Count() == 2, "InsertByte at used-1 splits into 2 blocks");
    CHECK(expected[kBlockCapacity] == lastByte, "original last byte pushed right");
}

// ---- 容量超過 Insert がブロック最終バイト上でも順序を保つ（Issue #93 回帰）----
// 原 BlockCursor_InsertWorker(0x0041cd40) の分割条件 `curOffset < usedLen-1` では、
// curOffset==usedLen-1 かつ現ブロックに収まらない挿入が分割にも空ブロック分岐にも入らず、
// 最終バイトを右へずらさないまま後続ブロックへ追記していた（挿入内容が最終バイトの前に残る）。
static void TestInsertOverflowAtLastByte() {
    std::printf("TestInsertOverflowAtLastByte\n");
    // 満杯ブロック / 半端な末尾ブロックの双方で、収まらない量を最終バイト上へ挿入する。
    for (int used : {kBlockCapacity, kBlockCapacity - 1, 10000}) {
        for (int count : {1, 10, kBlockCapacity, kBlockCapacity * 2 + 7}) {
            if (used + count <= kBlockCapacity) continue;  // 収まる場合は別経路
            BlockList list;
            NewEmptyDoc(list);
            BlockCursor c(&list);
            std::vector<unsigned char> ref(used);
            for (int i = 0; i < used; ++i) ref[i] = static_cast<unsigned char>(i * 7 + 3);
            CHECK(c.Insert(0, ref.data(), used), "seed block");

            std::vector<unsigned char> src(count);
            for (int i = 0; i < count; ++i) src[i] = static_cast<unsigned char>(0xE0 + (i % 16));

            const int pos = used - 1;  // ブロック最終データバイト上
            CHECK(c.Insert(pos, src.data(), count), "overflow insert at last byte");
            ref.insert(ref.begin() + pos, src.begin(), src.end());

            char where[96];
            std::snprintf(where, sizeof(where), "Insert overflow used=%d count=%d", used, count);
            CheckEqual(list, ref, where);
            CheckInvariants(list, ref.size(), where);
        }
    }
}

// ---- DeleteByte（末尾ブロック除去含む）----
static void TestDelete() {
    std::printf("TestDelete\n");
    BlockList list;
    NewEmptyDoc(list);
    BlockCursor c(&list);
    std::vector<unsigned char> data(100);
    for (int i = 0; i < 100; ++i) data[i] = static_cast<unsigned char>(i);
    CHECK(c.Insert(0, data.data(), 100), "insert 100");
    std::vector<unsigned char> ref(data.begin(), data.end());

    unsigned char b;
    // 先頭削除
    CHECK(c.DeleteByte(0, &b), "delete front");
    CHECK(b == 0, "deleted byte value front");
    ref.erase(ref.begin());
    CheckEqual(list, ref, "after delete front");
    // 末尾削除
    CHECK(c.DeleteByte(static_cast<int>(ref.size()) - 1, &b), "delete back");
    ref.erase(ref.end() - 1);
    CheckEqual(list, ref, "after delete back");
    // 全削除で空ブロックが残ること
    while (!ref.empty()) {
        CHECK(c.DeleteByte(0, &b), "delete all");
        ref.erase(ref.begin());
    }
    CheckEqual(list, ref, "after delete all");
    CheckInvariants(list, 0, "empty after delete all");
    // 空になっても再挿入できる
    CHECK(c.Insert(0, "X", 1), "reinsert after empty");
    ref.push_back('X');
    CheckEqual(list, ref, "after reinsert");
}

// 単一ノードを1バイト削除で除去→空ブロック維持の確認（used==1 の relink=false 経路）。
static void TestDeleteLastByteSingleBlock() {
    std::printf("TestDeleteLastByteSingleBlock\n");
    BlockList list;
    NewEmptyDoc(list);
    BlockCursor c(&list);
    CHECK(c.Insert(0, "A", 1), "insert A");
    unsigned char b;
    CHECK(c.DeleteByte(0, &b), "delete only byte");
    CHECK(b == 'A', "value A");
    CHECK(list.Count() == 1, "single empty block remains");
    CHECK(list.GetHead()->usedLen == 0, "usedLen 0");
    CHECK(list.GetTotalLength() == 0, "total 0");
}

// ---- ファズ: InsertByte / DeleteByte を参照モデルと突合（全位置でクリーン動作）----
static void TestFuzz() {
    std::printf("TestFuzz\n");
    BlockList list;
    NewEmptyDoc(list);
    BlockCursor c(&list);
    std::vector<unsigned char> ref;

    std::mt19937 rng(0xC0FFEE);
    const int kOps = 200000;  // 数万バイト(複数16KBブロック)へ成長させ分割・除去・跨ぎを網羅
    int mismatchAt = -1;
    for (int op = 0; op < kOps; ++op) {
        int size = static_cast<int>(ref.size());
        // 挿入偏重(約62%)で複数ブロックまで成長させつつ削除も混在させる
        bool doInsert = (size == 0) || (rng() % 100 < 62);
        if (doInsert) {
            int pos = static_cast<int>(rng() % (size + 1));
            unsigned char v = static_cast<unsigned char>(rng() & 0xFF);
            bool ok = c.InsertByte(pos, v);
            if (!ok) { std::printf("  FAIL: fuzz InsertByte failed op=%d pos=%d\n", op, pos); ++g_failures; break; }
            ref.insert(ref.begin() + pos, v);
        } else {
            int pos = static_cast<int>(rng() % size);
            unsigned char b;
            bool ok = c.DeleteByte(pos, &b);
            if (!ok) { std::printf("  FAIL: fuzz DeleteByte failed op=%d pos=%d\n", op, pos); ++g_failures; break; }
            if (b != ref[pos]) { std::printf("  FAIL: fuzz deleted byte mismatch op=%d\n", op); ++g_failures; break; }
            ref.erase(ref.begin() + pos);
        }
        // 全突合は高コストなので周期的に実施
        if ((op & 0x7FF) == 0x7FF) {
            std::vector<unsigned char> got = ReadAll(list);
            if (got.size() != ref.size() ||
                (!ref.empty() && std::memcmp(got.data(), ref.data(), ref.size()) != 0)) {
                mismatchAt = op;
                break;
            }
        }
    }
    // 最終突合
    CheckEqual(list, ref, "fuzz final");
    CheckInvariants(list, ref.size(), "fuzz final");
    CHECK(mismatchAt == -1, "fuzz periodic compare");
    CHECK(list.Count() >= 3, "fuzz exercised multiple blocks");  // 跨ぎ読取・分割の網羅を担保
    std::printf("  fuzz grew to %zu bytes, %d blocks\n", ref.size(), list.Count());
}

// ---- BlockFileIO: Load/Save ラウンドトリップ ----
namespace fs = std::filesystem;

static fs::path TempFile(const char* tag) {
    static int counter = 0;
    fs::path p = fs::temp_directory_path() /
                 (std::string("stirling_core_test_") + tag + "_" + std::to_string(counter++) + ".bin");
    return p;
}

// core の I/O はワイドパス（Issue #20）。テスト側のヘルパも _wfopen に揃える。
static void WriteFile(const fs::path& p, const std::vector<unsigned char>& data) {
    std::FILE* f = _wfopen(p.wstring().c_str(), L"wb");
    if (!f) { std::printf("  FAIL: cannot create temp %s\n", p.string().c_str()); ++g_failures; return; }
    if (!data.empty()) std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
}

static std::vector<unsigned char> ReadFileBytes(const fs::path& p) {
    std::vector<unsigned char> out;
    std::FILE* f = _wfopen(p.wstring().c_str(), L"rb");
    if (!f) { std::printf("  FAIL: cannot open temp %s\n", p.string().c_str()); ++g_failures; return out; }
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::rewind(f);
    if (n > 0) { out.resize(n); std::fread(out.data(), 1, n, f); }
    std::fclose(f);
    return out;
}

// ロード後のブロック構造検証（全ブロック16KB, 末尾のみ端数）。
static void VerifyLoadedStructure(BlockList& list, size_t size, const char* where) {
    if (size == 0) {
        CHECK(list.Count() == 1, where);
        CHECK(list.GetHead() != nullptr && list.GetHead()->usedLen == 0, where);
        CHECK(list.GetTotalLength() == 0, where);
        return;
    }
    size_t expectBlocks = (size + kBlockCapacity - 1) / kBlockCapacity;
    CHECK(list.Count() == static_cast<int>(expectBlocks), where);
    size_t idx = 0;
    for (BlockNode* n = list.GetHead(); n != nullptr; n = list.GetNext(n), ++idx) {
        bool isLast = (idx + 1 == expectBlocks);
        int expectUsed = isLast
            ? (size % kBlockCapacity == 0 ? kBlockCapacity : static_cast<int>(size % kBlockCapacity))
            : kBlockCapacity;
        CHECK(n->usedLen == expectUsed, where);
    }
    CHECK(list.GetTotalLength() == static_cast<FileOffset>(size), where);
}

static void TestFileRoundTrip() {
    std::printf("TestFileRoundTrip\n");
    size_t sizes[] = {0, 1, 100, 16383, 16384, 16385, 40000,
                      static_cast<size_t>(kReadChunk),            // ちょうど1チャンク
                      static_cast<size_t>(kReadChunk) + 40000};   // マルチチャンク
    for (size_t sz : sizes) {
        std::vector<unsigned char> data(sz);
        for (size_t i = 0; i < sz; ++i) data[i] = static_cast<unsigned char>((i * 131 + 7) & 0xFF);
        fs::path in = TempFile("rt");
        WriteFile(in, data);

        BlockList list;
        bool ok = stirling::LoadFileIntoBlocks(list, in.wstring().c_str()).Ok();
        char where[64];
        std::snprintf(where, sizeof(where), "load size=%zu", sz);
        CHECK(ok, where);
        VerifyLoadedStructure(list, sz, where);
        CheckEqual(list, data, where);

        fs::path out = TempFile("rt_out");
        CHECK(stirling::SaveBlocksToFile(list, out.wstring().c_str()).Ok(), "save ok");
        std::vector<unsigned char> saved = ReadFileBytes(out);
        bool eq = (saved.size() == data.size()) &&
                  (data.empty() || std::memcmp(saved.data(), data.data(), data.size()) == 0);
        CHECK(eq, "round-trip byte identical");

        fs::remove(in);
        fs::remove(out);
    }
}

// ロード→編集(挿入/削除)→保存 が参照モデルと一致するか。
static void TestLoadEditSave() {
    std::printf("TestLoadEditSave\n");
    std::vector<unsigned char> data(50000);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<unsigned char>(i * 5 + 3);
    fs::path in = TempFile("edit");
    WriteFile(in, data);

    BlockList list;
    CHECK(stirling::LoadFileIntoBlocks(list, in.wstring().c_str()).Ok(), "load 50k");
    std::vector<unsigned char> ref = data;
    BlockCursor c(&list);

    // 各種編集
    const char* ins = "INSERTED-DATA";
    CHECK(c.Insert(20000, ins, 13), "edit insert");
    ref.insert(ref.begin() + 20000, ins, ins + 13);
    unsigned char b;
    CHECK(c.DeleteByte(100, &b), "edit delete");
    ref.erase(ref.begin() + 100);
    CHECK(c.InsertByte(49000, 0x7E), "edit insert byte");
    ref.insert(ref.begin() + 49000, 0x7E);
    CheckEqual(list, ref, "after edits");

    fs::path out = TempFile("edit_out");
    CHECK(stirling::SaveBlocksToFile(list, out.wstring().c_str()).Ok(), "save edited");
    std::vector<unsigned char> saved = ReadFileBytes(out);
    bool eq = (saved.size() == ref.size()) &&
              std::memcmp(saved.data(), ref.data(), ref.size()) == 0;
    CHECK(eq, "edited save matches reference");

    fs::remove(in);
    fs::remove(out);
}

// ---- 検索（Boyer-Moore-Horspool）----
// ナイーブ参照検索（ブロック検索の意味論に合わせる）。
static int NaiveForward(const std::vector<unsigned char>& d, const std::vector<unsigned char>& pat,
                        int start, int end) {
    int n = static_cast<int>(d.size()), m = static_cast<int>(pat.size());
    if (m == 0) return -1;
    for (int s = (start < 0 ? 0 : start); s + m <= end && s + m <= n; ++s) {
        if (std::memcmp(d.data() + s, pat.data(), m) == 0) return s;
    }
    return -1;
}

// ナイーブ参照検索（後方）。start は走査開始位置で、SearchPattern(kBackward, start, end) と
//   同じく [end, start-m+1] の範囲にある最右の一致の先頭位置を返す。
static int NaiveBackward(const std::vector<unsigned char>& d, const std::vector<unsigned char>& pat,
                         int start, int end) {
    int n = static_cast<int>(d.size()), m = static_cast<int>(pat.size());
    if (m == 0) return -1;
    for (int s = start - (m - 1); s >= end; --s) {
        if (s < 0 || s + m > n) continue;
        if (std::memcmp(d.data() + s, pat.data(), m) == 0) return s;
    }
    return -1;
}


// data から BlockList を構築（Insert で16KBブロック化）。
static void BuildDoc(BlockList& list, const std::vector<unsigned char>& data) {
    NewEmptyDoc(list);
    if (!data.empty()) {
        BlockCursor c(&list);
        c.Insert(0, data.data(), static_cast<int>(data.size()));
    }
}

static void TestSearchBasic() {
    std::printf("TestSearchBasic\n");
    std::vector<unsigned char> data;
    const char* s = "abcXX abcYY abcZZ";  // "abc" が3箇所(0,6,12)
    for (const char* p = s; *p; ++p) data.push_back(static_cast<unsigned char>(*p));
    BlockList list;
    BuildDoc(list, data);
    BlockCursor c(&list);
    const unsigned char pat[] = {'a', 'b', 'c'};

    FileOffset pos = -1;
    // 前方: 先頭から最初の一致=0
    CHECK(c.SearchPattern(pat, 3, &pos, BlockCursor::kForward, 0, 0), "fwd find0");
    CHECK(pos == 0, "fwd pos0");
    // 前方: 位置1から → 次の一致=6
    CHECK(c.SearchPattern(pat, 3, &pos, BlockCursor::kForward, 1, 0), "fwd find6");
    CHECK(pos == 6, "fwd pos6");
    // 前方: 位置7から → 12
    CHECK(c.SearchPattern(pat, 3, &pos, BlockCursor::kForward, 7, 0), "fwd find12");
    CHECK(pos == 12, "fwd pos12");
    // 前方: 位置13から → 無し
    CHECK(!c.SearchPattern(pat, 3, &pos, BlockCursor::kForward, 13, 0), "fwd none");
    // 後方: 末尾付近から → 最後の一致=12
    CHECK(c.SearchPattern(pat, 3, &pos, BlockCursor::kBackward, static_cast<int>(data.size()) - 1, 0),
          "bwd find12");
    CHECK(pos == 12, "bwd pos12");
    // 単一バイト検索（"abcXX abcYY abcZZ" の 'Z' は位置15）
    const unsigned char one[] = {'Z'};
    CHECK(c.SearchPattern(one, 1, &pos, BlockCursor::kForward, 0, 0), "single find");
    CHECK(pos == 15, "single pos");
    // 無い文字列
    const unsigned char no[] = {'q', 'q'};
    CHECK(!c.SearchPattern(no, 2, &pos, BlockCursor::kForward, 0, 0), "no match");
}

// Issue #74: EOF は Seek では追記位置として有効だが、GetByteAt/SearchPattern の読取り範囲外。
// 特に満杯ブロックの EOF を読むと確保領域外アクセスになるため、境界で失敗することを固定する。
static void TestSearchEofBounds() {
    std::printf("TestSearchEofBounds\n");
    BlockList list;
    unsigned char* data = new unsigned char[kBlockCapacity];
    std::memset(data, 0x41, kBlockCapacity);
    data[kBlockCapacity - 1] = 0x5A;
    list.AppendBlock(data, kBlockCapacity, kBlockCapacity);

    const FileOffset total = list.GetTotalLength();
    FileOffset abs = -1;
    BlockCursor eofCursor(&list);
    CHECK(eofCursor.Seek(total, BlockCursor::kBegin, &abs), "Seek accepts EOF append position");
    CHECK(abs == total, "Seek resolves EOF append position");

    unsigned char value = 0xCC;
    CHECK(!eofCursor.GetByteAt(total, &value), "GetByteAt rejects resolved EOF");
    CHECK(value == 0xCC, "GetByteAt leaves output unchanged at EOF");

    BlockCursor c(&list);
    CHECK(c.Seek(total - 1, BlockCursor::kBegin, &abs), "Seek resolves last byte");
    CHECK(c.GetByteAt(total - 1, &value) && value == 0x5A, "GetByteAt reads last byte");
    CHECK(!c.GetByteAt(total, &value), "GetByteAt rejects EOF after a valid read");
    CHECK(!c.GetByteAt(total + 1, &value), "GetByteAt rejects past EOF");
    CHECK(c.GetByteAt(total - 1, &value) && value == 0x5A,
          "GetByteAt remains usable after rejected EOF reads");

    const unsigned char one[] = {0x5A};
    FileOffset found = -1;
    CHECK(!c.SearchPattern(one, 1, &found, BlockCursor::kForward, total, 0),
          "forward search rejects EOF start");
    CHECK(!c.SearchPattern(one, 1, &found, BlockCursor::kBackward, total, 0),
          "backward single-byte search rejects EOF start");
    CHECK(!c.SearchPattern(one, 1, &found, BlockCursor::kForward, total + 1, 0),
          "forward search rejects start past EOF");
    CHECK(!c.SearchPattern(one, 1, &found, BlockCursor::kBackward, total + 1, 0),
          "backward search rejects start past EOF");
}

// 不一致検索（SearchMismatch）: 指定バイトに一致しない最初の位置を前方/後方で検出。
static void TestSearchMismatch() {
    std::printf("TestSearchMismatch\n");
    // 位置: 0..2='A', 3='B', 4..5='A', 6..7='C'
    std::vector<unsigned char> data;
    for (const char* p = "AAABAACC"; *p; ++p) data.push_back(static_cast<unsigned char>(*p));
    const int n = static_cast<int>(data.size());

    BlockList list;
    BuildDoc(list, data);
    BlockCursor c(&list);
    FileOffset pos = -1;

    // 前方: 'A' に一致しない最初 = 位置3('B')
    CHECK(c.SearchMismatch('A', &pos, BlockCursor::kForward, 0, 0), "fwd mism find");
    CHECK(pos == 3, "fwd mism pos3");
    // 前方: 位置4から 'A' 不一致 = 位置6('C')
    CHECK(c.SearchMismatch('A', &pos, BlockCursor::kForward, 4, 0), "fwd mism find6");
    CHECK(pos == 6, "fwd mism pos6");
    // 後方: 末尾から 'C' 不一致 = 位置5('A')
    CHECK(c.SearchMismatch('C', &pos, BlockCursor::kBackward, n - 1, 0), "bwd mism find");
    CHECK(pos == 5, "bwd mism pos5");
    // 範囲 end 指定（前方）: [0,3) 内で 'A' 不一致は無し
    CHECK(!c.SearchMismatch('A', &pos, BlockCursor::kForward, 0, 3), "fwd mism none in range");

    // 全バイトが一致する場合は未発見（前方全長）。
    std::vector<unsigned char> same(20, 0xFF);
    BlockList list2;
    BuildDoc(list2, same);
    BlockCursor c2(&list2);
    CHECK(!c2.SearchMismatch(0xFF, &pos, BlockCursor::kForward, 0, 0), "all-equal none");

    // 1バイトでも違えば検出（位置10 を 0x00 に）。
    same[10] = 0x00;
    BlockList list3;
    BuildDoc(list3, same);
    BlockCursor c3(&list3);
    CHECK(c3.SearchMismatch(0xFF, &pos, BlockCursor::kForward, 0, 0), "one-diff find");
    CHECK(pos == 10, "one-diff pos10");
}

// ブロック境界(16KB)を跨ぐパターンの検出。
static void TestSearchAcrossBlocks() {
    std::printf("TestSearchAcrossBlocks\n");
    std::vector<unsigned char> data(40000);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<unsigned char>((i * 91) & 0xFF);
    // ブロック境界(16384)を跨ぐ位置へ既知パターンを埋め込む
    std::vector<unsigned char> pat = {0xDE, 0xAD, 0xBE, 0xEF, 0x11, 0x22};
    int at = 16384 - 3;  // 3バイトが1ブロック目、残りが2ブロック目
    for (size_t i = 0; i < pat.size(); ++i) data[at + i] = pat[i];
    BlockList list;
    BuildDoc(list, data);
    CHECK(list.Count() >= 3, "multi-block doc");
    BlockCursor c(&list);
    FileOffset pos = -1;
    CHECK(c.SearchPattern(pat.data(), static_cast<int>(pat.size()), &pos, BlockCursor::kForward, 0, 0),
          "cross-block fwd");
    CHECK(pos == at, "cross-block pos");
    CHECK(c.SearchPattern(pat.data(), static_cast<int>(pat.size()), &pos, BlockCursor::kBackward,
                          static_cast<int>(data.size()) - 1, 0), "cross-block bwd");
    CHECK(pos == at, "cross-block bwd pos");
}

// 前方・後方ともナイーブ参照と完全突合（=主経路 Find/FindNext/FindPrev の回帰基準）。
// 後方は end=0（末尾から全体）と end!=0（選択範囲内相当）の両方を突き合わせる。
// ※かつては後方を健全性（報告する一致が実在・範囲内か）だけで検証しており、
//   取りこぼしを検出できなかった（Issue #71）。完全突合に置き換えてある。
static void TestSearchFuzz() {
    std::printf("TestSearchFuzz\n");
    std::mt19937 rng(0x5EA6C4);
    int fwdCases = 0, fwdMism = 0;
    int bwdCases = 0, bwdFound = 0, bwdMism = 0;
    for (int iter = 0; iter < 200; ++iter) {
        int size = static_cast<int>(rng() % 24000) + 1;  // 単/複数ブロック
        std::vector<unsigned char> data(size);
        // 小さいアルファベットで一致頻度を上げる
        int alpha = 3 + static_cast<int>(rng() % 6);
        for (int i = 0; i < size; ++i) data[i] = static_cast<unsigned char>(rng() % alpha);
        BlockList list;
        BuildDoc(list, data);
        BlockCursor c(&list);

        for (int t = 0; t < 6; ++t) {
            int m = 1 + static_cast<int>(rng() % 6);
            std::vector<unsigned char> pat(m);
            if ((rng() & 1) && size >= m) {
                int src = static_cast<int>(rng() % (size - m + 1));  // 実在部分列（ヒット保証）
                std::memcpy(pat.data(), data.data() + src, m);
            } else {
                for (int i = 0; i < m; ++i) pat[i] = static_cast<unsigned char>(rng() % alpha);
            }
            // 前方: ナイーブ参照と完全一致
            int start = static_cast<int>(rng() % (size + 1));
            FileOffset got = -1;
            bool f = c.SearchPattern(pat.data(), m, &got, BlockCursor::kForward, start, 0);
            int exp = NaiveForward(data, pat, start, size);
            ++fwdCases;
            if ((f ? got : -1) != exp) {
                ++fwdMism;
                if (fwdMism <= 3) std::printf("  FAIL fwd: size=%d m=%d start=%d got=%lld exp=%d\n",
                                              size, m, start,
                                              static_cast<long long>(f ? got : -1), exp);
            }
            // 後方: ナイーブ参照と完全一致（取りこぼしも検出する。Issue #71）
            //   範囲は末尾から全体（end=0）と、選択範囲内相当の [lo, hi) の 2 通りを試す。
            for (int rangeCase = 0; rangeCase < 2; ++rangeCase) {
                int bstart = size - 1;
                int bend = 0;
                if (rangeCase == 1) {
                    int lo = static_cast<int>(rng() % size);
                    int hi = lo + 1 + static_cast<int>(rng() % (size - lo));   // (lo, size]
                    bend = lo;
                    bstart = hi - 1;
                    if (bstart < bend) continue;
                }
                FileOffset got2 = -1;
                bool f2 = c.SearchPattern(pat.data(), m, &got2, BlockCursor::kBackward, bstart, bend);
                int exp2 = NaiveBackward(data, pat, bstart, bend);
                ++bwdCases;
                if (f2) ++bwdFound;
                if ((f2 ? got2 : -1) != exp2) {
                    ++bwdMism;
                    if (bwdMism <= 3)
                        std::printf("  FAIL bwd: size=%d m=%d start=%d end=%d got=%lld exp=%d\n",
                                    size, m, bstart, bend,
                                    static_cast<long long>(f2 ? got2 : -1), exp2);
                }
            }
        }
    }
    CHECK(fwdMism == 0, "forward search matches naive reference");
    CHECK(bwdMism == 0, "backward search matches naive reference");
    std::printf("  search fuzz: fwd %d cases / %d mismatches, bwd %d cases / %d found / %d mismatches\n",
                fwdCases, fwdMism, bwdCases, bwdFound, bwdMism);
}

// ---- 後方検索の取りこぼし回帰（Issue #71）----
// 原実装の bad-character 表はパターン内の「最右」の出現位置を採っていたため、
//   シフトが過大になり、間にある一致を飛び越えて not-found を返していた。
//   ここでは実際に取りこぼしていた具体例を固定ケースとして押さえる。
static void TestSearchBackwardMissedMatch() {
    std::printf("TestSearchBackwardMissedMatch\n");
    struct Case {
        const char* name;
        std::vector<unsigned char> data;
        std::vector<unsigned char> pat;
        int expect;
    };
    const std::vector<Case> cases = {
        {"issue71-a",
         {0,2,2,2,0,0,2,0,1,1,2,2,2,1,2,2,0,0,1,0,1,1,2,0,2,0},
         {1,1,2,2}, 8},
        {"issue71-b",
         {0,0,0,1,2,1,2,0,1,1,2,2,0,1,1,0,1,0,0,0,2,2,2,0},
         {1,0,1,0}, 14},
        {"issue71-c",
         {1,1,1,0,2,0,1,0,1,0,1,1,0,2,1,1,1,0,0,0,2,0,0,2,2,1},
         {0,2,2}, 22},
    };
    for (const Case& c : cases) {
        BlockList list;
        BuildDoc(list, c.data);
        BlockCursor cur(&list);
        FileOffset got = -1;
        const int m = static_cast<int>(c.pat.size());
        const int start = static_cast<int>(c.data.size()) - 1;
        const bool found = cur.SearchPattern(c.pat.data(), m, &got, BlockCursor::kBackward, start, 0);
        CHECK(found && got == c.expect, c.name);
        // ナイーブ参照とも突き合わせる（期待値そのものの検算）。
        CHECK(NaiveBackward(c.data, c.pat, start, 0) == c.expect, "naive reference agrees");
    }
}


// ---- SetByteAt(上書き in-place) ----
static void TestSetByteAt() {
    std::printf("TestSetByteAt\n");
    BlockList list;
    NewEmptyDoc(list);
    std::vector<unsigned char> ref;
    BlockCursor c(&list);
    std::vector<unsigned char> data(40000);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<unsigned char>(i * 13 + 7);
    CHECK(c.Insert(0, data.data(), static_cast<int>(data.size())), "seed insert");
    ref.assign(data.begin(), data.end());
    CheckEqual(list, ref, "seed");

    // 先頭 / ブロック境界近傍(16KB) / 中間 / 末尾 を上書き。
    int positions[] = {0, 1, 16383, 16384, 16385, 20000, static_cast<int>(ref.size()) - 1};
    for (int p : positions) {
        unsigned char nb = static_cast<unsigned char>(p * 7 + 3);
        BlockCursor w(&list);
        CHECK(w.SetByteAt(p, nb), "SetByteAt ok");
        ref[p] = nb;
    }
    CheckEqual(list, ref, "after SetByteAt");
    CheckInvariants(list, ref.size(), "after SetByteAt");

    // 総長以上は false（実データ位置のみ許容）。
    {
        BlockCursor w(&list);
        CHECK(!w.SetByteAt(static_cast<int>(ref.size()), 0xAA), "SetByteAt at EOF fails");
        CHECK(!w.SetByteAt(static_cast<int>(ref.size()) + 100, 0xAA), "SetByteAt beyond fails");
    }
    CheckEqual(list, ref, "after oob attempts");
}


// ============================================================================
// x64 化（Issue #19）: 2GB 境界をまたぐ絶対位置の検証。
//
// 実データを 2GB 分確保するテストは常時実行に向かないため、既定では
// 「疎ブロック」（data を確保せず usedLen だけを持つノード）でアドレス空間を
// 2GB まで伸ばし、検証対象の範囲にだけ実データブロックを置く。
// Seek / GetTotalLength はノードの usedLen しか参照せずデータに触れないため、
// この構成でも 64bit アドレス演算をそのまま検証できる。
// 実データ 2GB 超の通し確認は TestLargeRealData（オプトイン）で行う。
// ============================================================================

// int32 で表現できない境界（原実装ではここで破綻していた）。
static const FileOffset k2GB = 0x80000000LL;

// data 実体を持たない疎ブロックを count 個追加する（テスト専用）。
// BlockList::Clear() の delete[] は nullptr に対して安全。
static void AppendSparseBlocks(BlockList& list, FileOffset count) {
    for (FileOffset i = 0; i < count; ++i) {
        list.AppendBlock(nullptr, kBlockCapacity, kBlockCapacity);
    }
}

// data を 16KB ブロック列として末尾へ追加する。
static void AppendRealBlocks(BlockList& list, const std::vector<unsigned char>& data) {
    for (size_t off = 0; off < data.size(); off += kBlockCapacity) {
        const size_t n = std::min<size_t>(kBlockCapacity, data.size() - off);
        unsigned char* buf = new unsigned char[kBlockCapacity];
        std::memcpy(buf, data.data() + off, n);
        list.AppendBlock(buf, kBlockCapacity, static_cast<int>(n));
    }
}

// GetByteAt は「位置解決済みカーソル」を前提とする（原の検索経路と同じ契約:
// SearchPattern が先頭で Seek してから増分アクセスする）。単発読取では
// Seek で始点を与えてから呼ぶ。
static bool ReadByteAt(BlockList& list, FileOffset pos, unsigned char* out) {
    BlockCursor c(&list);
    if (!c.Seek(pos, BlockCursor::kBegin, nullptr)) return false;
    return c.GetByteAt(pos, out);
}

// 2GB 超のアドレス空間で GetTotalLength / Seek が正しく解決するか。
static void TestLargeOffsetSeek() {
    std::printf("TestLargeOffsetSeek\n");
    BlockList list;
    AppendSparseBlocks(list, k2GB / kBlockCapacity);   // ちょうど 2GB
    const int kTailBlocks = 4;
    AppendSparseBlocks(list, kTailBlocks);
    const FileOffset total = k2GB + static_cast<FileOffset>(kTailBlocks) * kBlockCapacity;

    CHECK(list.GetTotalLength() == total, "total length beyond 2GB");
    // 32bit へ丸めると別物になる（＝原実装が破綻していた境界）ことを明示。
    CHECK(static_cast<int>(list.GetTotalLength()) != list.GetTotalLength(),
          "total length does not fit in int (regression anchor)");

    struct Probe { FileOffset pos; int expOff; const char* name; };
    const Probe probes[] = {
        {0,                        0,                  "pos 0"},
        {k2GB - 1,                 kBlockCapacity - 1, "just below 2GB"},
        {k2GB,                     0,                  "exactly 2GB"},
        {k2GB + 1,                 1,                  "just above 2GB"},
        {k2GB + kBlockCapacity,    0,                  "2GB + one block"},
        {total - 1,                kBlockCapacity - 1, "last byte"},
        {total,                    kBlockCapacity,     "EOF append position"},
    };
    for (const Probe& pr : probes) {
        BlockCursor sc(&list);
        FileOffset abs = -1;
        CHECK(sc.Seek(pr.pos, BlockCursor::kBegin, &abs), pr.name);
        CHECK(abs == pr.pos, pr.name);
        CHECK(sc.CurOffset() == pr.expOff, pr.name);
        // origin=kCurrent, pos=0 で絶対位置を逆算（64bit の累積加算経路）。
        FileOffset back = -1;
        CHECK(sc.Seek(0, BlockCursor::kCurrent, &back), pr.name);
        CHECK(back == pr.pos, pr.name);
    }

    // 範囲外
    BlockCursor sc(&list);
    CHECK(!sc.Seek(total + 1, BlockCursor::kBegin, nullptr), "seek past 2GB+ end fails");

    // 末尾起点シーク（原の逐語移植）。原は「現在位置を 1 歩と数える」ため
    // -delta の着地点は末尾から delta+1 バイト手前になる（縮退挙動の回帰アンカー）。
    FileOffset endAbs = -1;
    CHECK(sc.Seek(-5, BlockCursor::kEnd, &endAbs), "seek from end ok");
    CHECK(endAbs == total - 6, "seek from end lands at total-(delta+1) beyond 2GB");
    CHECK(endAbs > 0x7FFFFFFFLL, "seek from end resolves beyond int range");

    // origin=kCurrent の相対シーク（前方/後方）が 2GB 超でも破綻しないこと。
    // 原の相対シークは縮退挙動（後方でも GetNext を辿る等）を含むため、絶対値では
    // なく「同じブロック構成の小さいオフセットでの結果と変位が一致するか」で
    // 64bit 演算の正しさを検証する（＝32bit 桁溢れなら必ず食い違う）。
    {
        const int kTail = 8;
        BlockList small;
        AppendSparseBlocks(small, kTail);
        BlockList large;
        AppendSparseBlocks(large, k2GB / kBlockCapacity);
        AppendSparseBlocks(large, kTail);   // large の tail 構成は small と同一
        const FileOffset base = k2GB;

        const FileOffset starts[] = {100, kBlockCapacity - 10, 2 * kBlockCapacity + 5};
        const FileOffset deltas[] = {1, 50, kBlockCapacity, 3 * kBlockCapacity + 7,
                                     -1, -50, -kBlockCapacity};
        for (FileOffset st : starts) {
            for (FileOffset d : deltas) {
                BlockCursor cs(&small);
                BlockCursor cl(&large);
                CHECK(cs.Seek(st, BlockCursor::kBegin, nullptr), "kCurrent base seek (small)");
                CHECK(cl.Seek(base + st, BlockCursor::kBegin, nullptr), "kCurrent base seek (large)");
                FileOffset as = -1, al = -1;
                const bool okS = cs.Seek(d, BlockCursor::kCurrent, nullptr) &&
                                 cs.Seek(0, BlockCursor::kCurrent, &as);
                const bool okL = cl.Seek(d, BlockCursor::kCurrent, nullptr) &&
                                 cl.Seek(0, BlockCursor::kCurrent, &al);
                CHECK(okS == okL, "kCurrent relative seek: same outcome across 2GB");
                if (okS && okL) {
                    CHECK(al - base == as, "kCurrent relative seek: same displacement across 2GB");
                }
            }
        }
    }
}

// 2GB 境界をまたぐ読取・検索・上書き・挿入・削除。
// 先頭側は疎ブロックで埋め、実データ 128KB を 2GB 境界の前後 64KB ずつに配置する。
// これにより走査・編集の各操作が 2GB をまたいで実行される。
static void TestLargeOffsetDataOps() {
    std::printf("TestLargeOffsetDataOps\n");
    const int kTailBlocks = 8;
    const size_t tailLen = static_cast<size_t>(kTailBlocks) * kBlockCapacity;
    const size_t kBnd = tailLen / 2;                                  // tail 内での 2GB 位置
    const FileOffset tailBase = k2GB - static_cast<FileOffset>(kBnd);  // 実データ先頭の絶対位置

    // フィラは 0x00..0x3F に収め、0x80 以上のみで構成した検索パターンと衝突させない。
    std::vector<unsigned char> tail(tailLen);
    for (size_t i = 0; i < tailLen; ++i) tail[i] = static_cast<unsigned char>((i * 7) & 0x3F);

    const unsigned char pat[8] = {0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7};
    const size_t occ1 = 60000;      // 2GB 未満
    const size_t occ2 = kBnd - 3;   // 2GB 境界（かつブロック境界）をまたぐ
    const size_t occ3 = 100000;     // 2GB 超
    std::memcpy(tail.data() + occ1, pat, sizeof(pat));
    std::memcpy(tail.data() + occ2, pat, sizeof(pat));
    std::memcpy(tail.data() + occ3, pat, sizeof(pat));

    BlockList list;
    AppendSparseBlocks(list, tailBase / kBlockCapacity);
    AppendRealBlocks(list, tail);
    const FileOffset total = tailBase + static_cast<FileOffset>(tailLen);
    CHECK(list.GetTotalLength() == total, "large doc total");
    CHECK(tailBase % kBlockCapacity == 0, "sparse prefix ends on a block boundary");

    // --- GetByteAt: 2GB 境界の前後（前進・後退の増分アクセス両方）---
    {
        const size_t offs[] = {0, kBnd - kBlockCapacity, kBnd - 1, kBnd, kBnd + 1,
                               kBnd + kBlockCapacity + 77, tailLen - 1};
        BlockCursor c(&list);
        CHECK(c.Seek(tailBase, BlockCursor::kBegin, nullptr), "seek to real region");
        for (size_t o : offs) {
            unsigned char b = 0;
            CHECK(c.GetByteAt(tailBase + static_cast<FileOffset>(o), &b), "GetByteAt around 2GB");
            CHECK(b == tail[o], "GetByteAt value around 2GB");
        }
        // 後退方向（増分アクセスの逆走）
        for (size_t i = sizeof(offs) / sizeof(offs[0]); i-- > 0; ) {
            unsigned char b = 0;
            CHECK(c.GetByteAt(tailBase + static_cast<FileOffset>(offs[i]), &b), "GetByteAt backward");
            CHECK(b == tail[offs[i]], "GetByteAt backward value");
        }
    }

    // --- Read: 2GB 境界（＝ブロック境界）をまたぐ読取 ---
    {
        BlockCursor c(&list);
        CHECK(c.Seek(k2GB - 8, BlockCursor::kBegin, nullptr), "seek for read");
        unsigned char buf[16] = {0};
        CHECK(c.Read(16, buf) == 16, "read 16 across the 2GB boundary");
        CHECK(std::memcmp(buf, tail.data() + kBnd - 8, 16) == 0, "read content across 2GB");
    }

    // --- SearchPattern（前方）: 2GB 未満から開始し境界をまたいで一致させる ---
    {
        BlockCursor c(&list);
        FileOffset pos = -1;
        CHECK(c.SearchPattern(pat, 8, &pos, BlockCursor::kForward, tailBase, 0), "fwd search");
        CHECK(pos == tailBase + static_cast<FileOffset>(occ1), "fwd search pos occ1 (below 2GB)");
        CHECK(c.SearchPattern(pat, 8, &pos, BlockCursor::kForward,
                              tailBase + static_cast<FileOffset>(occ1) + 1, 0), "fwd search occ2");
        CHECK(pos == tailBase + static_cast<FileOffset>(occ2), "fwd search pos occ2 (spans 2GB)");
        CHECK(pos < k2GB && pos + 8 > k2GB, "occ2 really straddles the 2GB boundary");
    }

    // --- SearchPattern（後方）: 2GB 超から下って境界をまたぐ ---
    {
        BlockCursor c(&list);
        FileOffset pos = -1;
        CHECK(c.SearchPattern(pat, 8, &pos, BlockCursor::kBackward, total - 1, tailBase),
              "bwd search beyond 2GB");
        CHECK(pos == tailBase + static_cast<FileOffset>(occ3), "bwd search pos occ3");
        CHECK(c.SearchPattern(pat, 8, &pos, BlockCursor::kBackward,
                              tailBase + static_cast<FileOffset>(occ3) - 1, tailBase),
              "bwd search across 2GB");
        CHECK(pos == tailBase + static_cast<FileOffset>(occ2), "bwd search pos occ2 (spans 2GB)");
    }

    // --- SearchMismatch: 走査が 2GB 境界をまたぐ前方／後方 ---
    // 境界を挟む [kBnd-16, kBnd+24) を同値で塗る（occ2 を上書きするため検索系の後に実施）。
    {
        const size_t runLo = kBnd - 16, runHi = kBnd + 24;
        for (size_t i = runLo; i < runHi; ++i) {
            BlockCursor w(&list);
            CHECK(w.SetByteAt(tailBase + static_cast<FileOffset>(i), 0xAA), "fill run byte");
            tail[i] = 0xAA;
        }
        BlockCursor c(&list);
        FileOffset pos = -1;
        CHECK(c.SearchMismatch(0xAA, &pos, BlockCursor::kForward,
                               tailBase + static_cast<FileOffset>(runLo), 0),
              "fwd mismatch across 2GB");
        CHECK(pos == tailBase + static_cast<FileOffset>(runHi), "fwd mismatch pos across 2GB");

        CHECK(c.SearchMismatch(0xAA, &pos, BlockCursor::kBackward,
                               tailBase + static_cast<FileOffset>(runHi) - 1,
                               tailBase + static_cast<FileOffset>(runLo) - 64),
              "bwd mismatch across 2GB");
        CHECK(pos == tailBase + static_cast<FileOffset>(runLo) - 1, "bwd mismatch pos across 2GB");
    }

    // --- SetByteAt: 2GB 境界の直前・直後 ---
    {
        const size_t offs[] = {kBnd - 1, kBnd};
        for (size_t o : offs) {
            const FileOffset at = tailBase + static_cast<FileOffset>(o);
            BlockCursor w(&list);
            CHECK(w.SetByteAt(at, 0x5A), "SetByteAt at the 2GB boundary");
            unsigned char b = 0;
            CHECK(ReadByteAt(list, at, &b) && b == 0x5A, "SetByteAt readback at the 2GB boundary");
            BlockCursor w2(&list);
            CHECK(w2.SetByteAt(at, tail[o]), "SetByteAt restore");
        }
    }

    // --- InsertByte / DeleteByte: ちょうど 2GB の位置（満杯ブロックの分割を伴う）---
    {
        const FileOffset at = k2GB;
        BlockCursor c(&list);
        CHECK(c.InsertByte(at, 0x99), "InsertByte at 2GB");
        CHECK(list.GetTotalLength() == total + 1, "total grew across 2GB");
        unsigned char b = 0;
        CHECK(ReadByteAt(list, at, &b) && b == 0x99, "inserted byte readback at 2GB");
        CHECK(ReadByteAt(list, at + 1, &b) && b == tail[kBnd], "byte after insertion shifted");
        CHECK(ReadByteAt(list, at - 1, &b) && b == tail[kBnd - 1], "byte before insertion intact");

        BlockCursor d(&list);
        unsigned char removed = 0;
        CHECK(d.DeleteByte(at, &removed), "DeleteByte at 2GB");
        CHECK(removed == 0x99, "deleted byte value at 2GB");
        CHECK(list.GetTotalLength() == total, "total restored across 2GB");
        CHECK(ReadByteAt(list, at, &b) && b == tail[kBnd], "content restored across 2GB");
    }

    // --- Insert: 2GB 境界をまたぐ複数ブロック分割挿入 ---
    {
        const FileOffset at = k2GB - 100;
        std::vector<unsigned char> ins(40000);
        for (size_t i = 0; i < ins.size(); ++i) ins[i] = static_cast<unsigned char>(0x80 | (i & 0x1F));
        BlockCursor c(&list);
        CHECK(c.Insert(at, ins.data(), static_cast<FileOffset>(ins.size())), "Insert across 2GB");
        CHECK(list.GetTotalLength() == total + static_cast<FileOffset>(ins.size()),
              "total after large insert across 2GB");
        BlockCursor r(&list);
        CHECK(r.Seek(at, BlockCursor::kBegin, nullptr), "seek to inserted region");
        std::vector<unsigned char> got(ins.size());
        CHECK(r.Read(static_cast<FileOffset>(ins.size()), got.data()) ==
                  static_cast<FileOffset>(ins.size()), "read back inserted region");
        CHECK(std::memcmp(got.data(), ins.data(), ins.size()) == 0, "inserted content across 2GB");
        // 挿入直後の既存データが後ろへずれていること
        unsigned char b = 0;
        CHECK(ReadByteAt(list, at + static_cast<FileOffset>(ins.size()), &b) &&
                  b == tail[kBnd - 100], "existing byte shifted after insert across 2GB");
    }
}

// 実データ 2GB 超の通し確認（オプトイン）。
// 約 2.1GB のメモリを確保するため、既定ではスキップする。
// 実行するには 64bit ビルドで環境変数 STIRLING_CORE_TEST_LARGE=1 を設定する。
static void TestLargeRealData() {
    std::printf("TestLargeRealData\n");
    const char* env = std::getenv("STIRLING_CORE_TEST_LARGE");
    if (env == nullptr || std::strcmp(env, "1") != 0) {
        std::printf("  skipped (set STIRLING_CORE_TEST_LARGE=1 to run)\n");
        return;
    }
    if (sizeof(void*) < 8) {
        std::printf("  skipped (needs a 64-bit build)\n");
        return;
    }

    // 絶対位置 off のバイト値（0x00..0x3F。検索パターンと衝突しない）。
    struct Filler {
        static unsigned char At(FileOffset off) {
            return static_cast<unsigned char>((off * 7) & 0x3F);
        }
    };

    BlockList list;
    const FileOffset totalBlocks = (k2GB / kBlockCapacity) + 8;   // 2GB + 128KB
    for (FileOffset i = 0; i < totalBlocks; ++i) {
        unsigned char* buf = new unsigned char[kBlockCapacity];
        const FileOffset base = i * kBlockCapacity;
        for (int j = 0; j < kBlockCapacity; ++j) buf[j] = Filler::At(base + j);
        list.AppendBlock(buf, kBlockCapacity, kBlockCapacity);
    }
    const FileOffset total = totalBlocks * kBlockCapacity;
    CHECK(list.GetTotalLength() == total, "real 2GB+ total length");
    std::printf("  built %lld bytes of real data\n", static_cast<long long>(total));

    // 2GB 境界の直前・直後を読取
    {
        const FileOffset probes[] = {k2GB - 1, k2GB, k2GB + 1, total - 1};
        for (FileOffset pr : probes) {
            unsigned char b = 0;
            CHECK(ReadByteAt(list, pr, &b), "real GetByteAt across 2GB");
            CHECK(b == Filler::At(pr), "real GetByteAt value across 2GB");
        }
    }

    // 2GB 境界を跨ぐパターンを書込み、前方検索で位置が一致するか
    {
        const unsigned char pat[8] = {0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7};
        const FileOffset at = k2GB - 3;   // 境界跨ぎ
        for (int i = 0; i < 8; ++i) {
            BlockCursor w(&list);
            CHECK(w.SetByteAt(at + i, pat[i]), "real SetByteAt across 2GB");
        }
        BlockCursor c(&list);
        FileOffset pos = -1;
        CHECK(c.SearchPattern(pat, 8, &pos, BlockCursor::kForward, k2GB - 100000, 0),
              "real fwd search across 2GB");
        CHECK(pos == at, "real fwd search pos across 2GB");

        FileOffset pos2 = -1;
        CHECK(c.SearchPattern(pat, 8, &pos2, BlockCursor::kBackward, total - 1, k2GB - 100000),
              "real bwd search across 2GB");
        CHECK(pos2 == at, "real bwd search pos across 2GB");
    }
}


// ============================================================================
// x64 化(Issue #20): 64bit オフセットのファイル I/O。
// ============================================================================

// 指定オフセットの 1 バイトをファイルから直接読む（保存結果の突合用）。
static bool ReadByteFromFile(const fs::path& p, FileOffset off, unsigned char* out) {
    std::FILE* f = _wfopen(p.wstring().c_str(), L"rb");
    if (f == nullptr) return false;
    const bool ok = (_fseeki64(f, off, SEEK_SET) == 0) && (std::fread(out, 1, 1, f) == 1);
    std::fclose(f);
    return ok;
}

// 絶対位置 off のバイト値（0x00..0x3F の決定的パターン）。
static unsigned char LargeFileByteAt(FileOffset off) {
    return static_cast<unsigned char>((off * 7) & 0x3F);
}

// total バイトのパターンファイルを生成する。
static bool WriteLargePatternFile(const fs::path& p, FileOffset total) {
    std::FILE* f = _wfopen(p.wstring().c_str(), L"wb");
    if (f == nullptr) return false;
    const size_t kChunk = 1u << 20;   // 1MB
    std::vector<unsigned char> buf(kChunk);
    FileOffset off = 0;
    while (off < total) {
        const size_t n =
            static_cast<size_t>(std::min<FileOffset>(static_cast<FileOffset>(kChunk), total - off));
        for (size_t i = 0; i < n; ++i) {
            buf[i] = LargeFileByteAt(off + static_cast<FileOffset>(i));
        }
        if (std::fwrite(buf.data(), 1, n, f) != n) { std::fclose(f); return false; }
        off += static_cast<FileOffset>(n);
    }
    return std::fclose(f) == 0;
}

// QueryFileSize と失敗時の status（エラーを握りつぶさないことの検証）。
static void TestFileIoStatus() {
    std::printf("TestFileIoStatus\n");
    using stirling::FileIoResult;
    using stirling::FileIoStatus;

    // 存在しないファイル: Load は kOpenFailed、QueryFileSize は false。
    const fs::path missing = TempFile("missing");
    {
        BlockList list;
        const FileIoResult r = stirling::LoadFileIntoBlocks(list, missing.wstring().c_str());
        CHECK(!r.Ok(), "load missing file fails");
        CHECK(r.status == FileIoStatus::kOpenFailed, "load missing file status");
        CHECK(r.systemError != 0, "load missing file reports a system error");
        CHECK(list.IsEmpty(), "failed load leaves the list empty");

        FileOffset sz = -1;
        FileIoResult qerr;
        CHECK(!stirling::QueryFileSize(missing.wstring().c_str(), &sz, &qerr),
              "QueryFileSize on missing file fails");
        CHECK(sz == 0, "QueryFileSize clears the size on failure");
        CHECK(!qerr.Ok(), "QueryFileSize reports a reason");
    }

    // 空パスは開かずに失敗する。
    {
        BlockList list;
        CHECK(!stirling::LoadFileIntoBlocks(list, L"").Ok(), "load empty path fails");
        CHECK(!stirling::SaveBlocksToFile(list, L"").Ok(), "save empty path fails");
        CHECK(!stirling::QueryFileSize(L"", nullptr, nullptr), "QueryFileSize empty path fails");
    }

    // 存在しないディレクトリへの保存は kOpenFailed。
    {
        BlockList list;
        NewEmptyDoc(list);
        BlockCursor c(&list);
        CHECK(c.Insert(0, "abc", 3), "seed for save");
        const fs::path bad = TempFile("nodir") / L"sub" / L"out.bin";
        const FileIoResult r = stirling::SaveBlocksToFile(list, bad.wstring().c_str());
        CHECK(!r.Ok(), "save into a missing directory fails");
        CHECK(r.status == FileIoStatus::kOpenFailed, "save into a missing directory status");
        CHECK(r.systemError != 0, "save failure reports a system error");
    }

    // ディレクトリは「サイズを取得できる対象」ではないため false（サイズ0のファイルと区別する）。
    {
        FileOffset sz = -1;
        FileIoResult qerr;
        const fs::path dir = fs::temp_directory_path();
        CHECK(!stirling::QueryFileSize(dir.wstring().c_str(), &sz, &qerr),
              "QueryFileSize on a directory fails");
        CHECK(sz == 0, "QueryFileSize clears the size for a directory");
        CHECK(!qerr.Ok(), "QueryFileSize reports a reason for a directory");
    }

    // 出力ポインタの nullptr を許容する。
    {
        const fs::path missing2 = TempFile("nullout");
        CHECK(!stirling::QueryFileSize(missing2.wstring().c_str(), nullptr, nullptr),
              "QueryFileSize tolerates null outputs (missing)");
        CHECK(!stirling::QueryFileSize(nullptr, nullptr, nullptr),
              "QueryFileSize tolerates a null path");
    }

    // 読み取り専用属性のファイルへの保存は kOpenFailed。
    {
        std::vector<unsigned char> data(64, 0x5A);
        const fs::path ro = TempFile("readonly");
        WriteFile(ro, data);
        fs::permissions(ro, fs::perms::owner_write | fs::perms::group_write |
                                fs::perms::others_write,
                        fs::perm_options::remove);

        BlockList list;
        NewEmptyDoc(list);
        BlockCursor c(&list);
        CHECK(c.Insert(0, "xyz", 3), "seed for readonly save");
        const FileIoResult r = stirling::SaveBlocksToFile(list, ro.wstring().c_str());
        CHECK(!r.Ok(), "save to a read-only file fails");
        CHECK(r.status == FileIoStatus::kOpenFailed, "read-only save status");
        CHECK(r.systemError != 0, "read-only save reports a system error");

        fs::permissions(ro, fs::perms::owner_write, fs::perm_options::add);
        fs::remove(ro);
    }

    // 排他オープン中のファイルは共有違反で開けない（Load の共有モードの検証）。
    {
        std::vector<unsigned char> data(128, 0x7E);
        const fs::path locked = TempFile("locked");
        WriteFile(locked, data);
        // _SH_DENYRW = 他プロセス/他ハンドルからの読み書きを拒否
        std::FILE* holder = _wfsopen(locked.wstring().c_str(), L"rb", _SH_DENYRW);
        CHECK(holder != nullptr, "exclusive holder opened");
        if (holder != nullptr) {
            BlockList list;
            const FileIoResult r = stirling::LoadFileIntoBlocks(list, locked.wstring().c_str());
            CHECK(!r.Ok(), "load of an exclusively held file fails");
            CHECK(r.status == FileIoStatus::kOpenFailed, "sharing violation status");
            CHECK(r.systemError != 0, "sharing violation reports a system error");
            CHECK(list.IsEmpty(), "failed load leaves no blocks");
            CHECK(r.systemError == ERROR_SHARING_VIOLATION ||
                  r.systemError == ERROR_LOCK_VIOLATION,
                  "the caller can tell a sharing violation apart");
            std::fclose(holder);
        }
        fs::remove(locked);
    }

    // 共有モードつきの読み込み（Issue #120）。原は環境設定「ファイルの排他制御」を
    //   読み込みハンドルの共有モードとして適用し、そのハンドルを保持し続ける。
    {
        std::vector<unsigned char> data(300, 0x5A);
        const fs::path shared = TempFile("sharemode");
        WriteFile(shared, data);

        // 他プロセスが共有ありで開いているだけなら、共有全許可では読める。
        std::FILE* holder = _wfsopen(shared.wstring().c_str(), L"rb", _SH_DENYNO);
        CHECK(holder != nullptr, "shared holder opened");
        if (holder != nullptr) {
            BlockList list;
            CHECK(stirling::LoadFileIntoBlocks(list, shared.wstring().c_str(),
                                               stirling::FileShareMode::kDenyNone).Ok(),
                  "deny-none load succeeds while another handle is open");
            CheckEqual(list, data, "deny-none load content");

            // 排他を要求すると、他のハンドルがある間は共有違反になる。
            BlockList excl;
            const FileIoResult r = stirling::LoadFileIntoBlocks(
                excl, shared.wstring().c_str(), stirling::FileShareMode::kExclusive);
            CHECK(!r.Ok(), "exclusive load fails while another handle is open");
            CHECK(r.status == FileIoStatus::kOpenFailed, "exclusive load status");
            CHECK(r.systemError == ERROR_SHARING_VIOLATION ||
                  r.systemError == ERROR_LOCK_VIOLATION, "exclusive load reports sharing violation");
            std::fclose(holder);
        }

        // 保持したハンドルが実際にロックとして働くこと（排他で開き、閉じるまで他から開けない）。
        {
            BlockList list;
            void* keep = nullptr;
            const FileIoResult r = stirling::LoadFileIntoBlocks(
                list, shared.wstring().c_str(), stirling::FileShareMode::kExclusive, &keep);
            CHECK(r.Ok(), "exclusive load succeeds when nobody else holds the file");
            CheckEqual(list, data, "exclusive load content");
            CHECK(keep != nullptr, "the handle is handed over to the caller");

            std::FILE* other = _wfsopen(shared.wstring().c_str(), L"rb", _SH_DENYNO);
            CHECK(other == nullptr, "the kept handle keeps other openers out");
            if (other != nullptr) { std::fclose(other); }

            ::CloseHandle(static_cast<HANDLE>(keep));
            std::FILE* after = _wfsopen(shared.wstring().c_str(), L"rb", _SH_DENYNO);
            CHECK(after != nullptr, "closing the kept handle releases the lock");
            if (after != nullptr) { std::fclose(after); }
        }

        // 書込禁止（原 shareDenyWrite）は他プロセスの読み取りを許す。
        {
            BlockList list;
            void* keep = nullptr;
            CHECK(stirling::LoadFileIntoBlocks(list, shared.wstring().c_str(),
                                               stirling::FileShareMode::kDenyWrite, &keep).Ok(),
                  "deny-write load succeeds");
            std::FILE* reader = _wfsopen(shared.wstring().c_str(), L"rb", _SH_DENYNO);
            CHECK(reader != nullptr, "deny-write still allows other readers");
            if (reader != nullptr) { std::fclose(reader); }
            if (keep != nullptr) { ::CloseHandle(static_cast<HANDLE>(keep)); }
        }

        // ハンドルを受け取らない呼び出しは、戻った時点でファイルを掴んでいない。
        {
            BlockList list;
            CHECK(stirling::LoadFileIntoBlocks(list, shared.wstring().c_str(),
                                               stirling::FileShareMode::kExclusive).Ok(),
                  "exclusive load without keeping the handle succeeds");
            std::FILE* after = _wfsopen(shared.wstring().c_str(), L"rb", _SH_DENYNO);
            CHECK(after != nullptr, "no handle is retained when the caller does not ask");
            if (after != nullptr) { std::fclose(after); }
        }

        fs::remove(shared);
    }

    // 非 ASCII（日本語）を含むパスの読み書き（ワイドパス化の検証）。
    {
        std::vector<unsigned char> data(5000);
        for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<unsigned char>(i * 11 + 5);
        const fs::path jp = fs::temp_directory_path() / L"stirling_core_test_日本語パス.bin";
        WriteFile(jp, data);

        FileOffset sz = 0;
        CHECK(stirling::QueryFileSize(jp.wstring().c_str(), &sz, nullptr),
              "QueryFileSize with a non-ASCII path");
        CHECK(sz == static_cast<FileOffset>(data.size()), "non-ASCII path size");

        BlockList list;
        CHECK(stirling::LoadFileIntoBlocks(list, jp.wstring().c_str()).Ok(),
              "load with a non-ASCII path");
        CheckEqual(list, data, "non-ASCII path content");

        const fs::path jpOut = fs::temp_directory_path() / L"stirling_core_test_日本語出力.bin";
        CHECK(stirling::SaveBlocksToFile(list, jpOut.wstring().c_str()).Ok(),
              "save with a non-ASCII path");
        const std::vector<unsigned char> saved = ReadFileBytes(jpOut);
        CHECK(saved.size() == data.size() &&
                  std::memcmp(saved.data(), data.data(), data.size()) == 0,
              "non-ASCII path round-trip");
        fs::remove(jp);
        fs::remove(jpOut);
    }

    // 成功した読み込みは必ず 1 個以上のブロックを持つ（空ファイルでも空ブロック 1 個）。
    {
        const fs::path empty = TempFile("empty");
        WriteFile(empty, std::vector<unsigned char>());
        BlockList list;
        const FileIoResult r = stirling::LoadFileIntoBlocks(list, empty.wstring().c_str());
        CHECK(r.Ok(), "load empty file ok");
        CHECK(!list.IsEmpty() && list.Count() == 1, "empty file yields exactly one block");
        CHECK(list.GetHead() != nullptr && list.GetHead()->usedLen == 0, "the block is empty");
        fs::remove(empty);
    }

    // 正常系: QueryFileSize が実サイズを返し、Load の結果にもサイズが入る。
    {
        std::vector<unsigned char> data(40000);
        for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<unsigned char>(i * 3);
        const fs::path p = TempFile("size");
        WriteFile(p, data);

        FileOffset sz = 0;
        CHECK(stirling::QueryFileSize(p.wstring().c_str(), &sz, nullptr), "QueryFileSize ok");
        CHECK(sz == static_cast<FileOffset>(data.size()), "QueryFileSize value");

        BlockList list;
        const FileIoResult r = stirling::LoadFileIntoBlocks(list, p.wstring().c_str());
        CHECK(r.Ok(), "load ok");
        CHECK(r.fileSize == static_cast<FileOffset>(data.size()), "load result carries the size");
        CheckEqual(list, data, "load content");
        fs::remove(p);
    }
}

// 2GB 超の実ファイルを開き、編集して保存し、内容が一致することを確認する（オプトイン）。
// ディスク約 4.3GB・メモリ約 2.1GB を消費するため、既定ではスキップする。
// 実行するには 64bit ビルドで環境変数 STIRLING_CORE_TEST_LARGE=1 を設定する。
static void TestLargeFileRoundTrip() {
    std::printf("TestLargeFileRoundTrip\n");
    const char* env = std::getenv("STIRLING_CORE_TEST_LARGE");
    if (env == nullptr || std::strcmp(env, "1") != 0) {
        std::printf("  skipped (set STIRLING_CORE_TEST_LARGE=1 to run)\n");
        return;
    }
    if (sizeof(void*) < 8) {
        std::printf("  skipped (needs a 64-bit build)\n");
        return;
    }

    const FileOffset total = k2GB + 128 * 1024;   // 2GB + 128KB
    const fs::path in = TempFile("bigin");
    if (!WriteLargePatternFile(in, total)) {
        std::printf("  FAIL: cannot create the 2GB+ source file (disk space?)\n");
        ++g_failures;
        fs::remove(in);
        return;
    }
    std::printf("  wrote %lld bytes to disk\n", static_cast<long long>(total));

    // fs::file_size は 64bit。ftell(long) では取得できないサイズであることの確認も兼ねる。
    CHECK(static_cast<FileOffset>(fs::file_size(in)) == total, "source file size beyond 2GB");

    BlockList list;
    const stirling::FileIoResult loaded = stirling::LoadFileIntoBlocks(list, in.wstring().c_str());
    fs::remove(in);   // 読み込み後は不要（ピーク時のディスク使用量を抑える）
    if (!loaded.Ok()) {
        std::printf("  FAIL: load failed (status=%d, err=%lu)\n",
                    static_cast<int>(loaded.status), loaded.systemError);
        ++g_failures;
        return;
    }
    CHECK(loaded.fileSize == total, "load result size beyond 2GB");
    CHECK(list.GetTotalLength() == total, "loaded total length beyond 2GB");

    // 2GB 境界の前後を突合。
    {
        const FileOffset probes[] = {0, k2GB - 1, k2GB, k2GB + 1, total - 1};
        for (FileOffset pr : probes) {
            unsigned char b = 0;
            CHECK(ReadByteAt(list, pr, &b), "loaded GetByteAt across 2GB");
            CHECK(b == LargeFileByteAt(pr), "loaded value across 2GB");
        }
    }

    // ちょうど 2GB の位置へ 1 バイト挿入してから保存する。
    {
        BlockCursor c(&list);
        CHECK(c.InsertByte(k2GB, 0x99), "InsertByte at 2GB on a loaded file");
    }
    CHECK(list.GetTotalLength() == total + 1, "total grew after the edit");

    const fs::path out = TempFile("bigout");
    const stirling::FileIoResult savedRes = stirling::SaveBlocksToFile(list, out.wstring().c_str());
    list.Clear();   // 保存後はメモリを解放してから照合する
    if (!savedRes.Ok()) {
        std::printf("  FAIL: save failed (status=%d, err=%lu)\n",
                    static_cast<int>(savedRes.status), savedRes.systemError);
        ++g_failures;
        fs::remove(out);
        return;
    }
    CHECK(savedRes.fileSize == total + 1, "save result reports the written size");
    CHECK(static_cast<FileOffset>(fs::file_size(out)) == total + 1, "saved file size beyond 2GB");

    // 保存されたファイルを直接読み、編集結果が正しい位置に入っているか確認する。
    {
        unsigned char b = 0;
        CHECK(ReadByteFromFile(out, k2GB - 1, &b) && b == LargeFileByteAt(k2GB - 1),
              "saved byte just below 2GB");
        CHECK(ReadByteFromFile(out, k2GB, &b) && b == 0x99, "saved inserted byte at 2GB");
        CHECK(ReadByteFromFile(out, k2GB + 1, &b) && b == LargeFileByteAt(k2GB),
              "saved byte shifted after the insertion");
        CHECK(ReadByteFromFile(out, total, &b) && b == LargeFileByteAt(total - 1),
              "saved last byte");
    }
    fs::remove(out);
}

// ---- 設定永続化コーデック（app/SettingsCodec.h。Issue #22）----
//   64bit アドレス設定値の 16進文字列往復・旧形式移行時の解釈・不正入力の拒否を検証する。
static void TestSettingsCodec() {
    std::printf("TestSettingsCodec\n");
    using stirling::settings::FormatOffsetHex;
    using stirling::settings::ParseOffsetHex;

    // 書式（接頭辞なし・大文字・冗長な先行ゼロなし）
    CHECK(FormatOffsetHex(0) == "0", "format 0");
    CHECK(FormatOffsetHex(0x40) == "40", "format 0x40");
    CHECK(FormatOffsetHex(0xABCDEF) == "ABCDEF", "format uppercase");
    CHECK(FormatOffsetHex(0x7FFFFFFF) == "7FFFFFFF", "format 2GB-1");
    CHECK(FormatOffsetHex(0x80000000LL) == "80000000", "format 2GB");
    CHECK(FormatOffsetHex(0x1FFFFFFFFLL) == "1FFFFFFFF", "format beyond 32bit");
    CHECK(FormatOffsetHex(INT64_MAX) == "7FFFFFFFFFFFFFFF", "format INT64_MAX");
    CHECK(FormatOffsetHex(-1) == "FFFFFFFFFFFFFFFF", "format -1 as two's complement");

    // 往復（境界値および 32bit を超える値）
    const FileOffset roundTrip[] = {
        0, 1, 0x7F, 0x80, 0xFFFF, 0x7FFFFFFF, 0x80000000LL, 0xFFFFFFFFLL,
        0x100000000LL, 0x1FFFFFFFFLL, 0x123456789ABCLL, INT64_MAX, -1, INT64_MIN,
    };
    for (FileOffset v : roundTrip) {
        FileOffset back = 0;
        const std::string text = FormatOffsetHex(v);
        CHECK(ParseOffsetHex(text.c_str(), back) && back == v, "round trip");
    }

    // 解釈（小文字・0x 接頭辞・先行ゼロも受理する）
    FileOffset out = -123;
    CHECK(ParseOffsetHex("1ffffffff", out) && out == 0x1FFFFFFFFLL, "parse lowercase");
    CHECK(ParseOffsetHex("0x1FFFFFFFF", out) && out == 0x1FFFFFFFFLL, "parse 0x prefix");
    CHECK(ParseOffsetHex("0X40", out) && out == 0x40, "parse 0X prefix");
    CHECK(ParseOffsetHex("0000000000000040", out) && out == 0x40, "parse 16 digits with leading zeros");

    // 不正入力は拒否し、出力先を書き換えない
    const char* invalid[] = {
        "", "0x", "0X", " 40", "40 ", "4 0", "0x 40", "-1", "+1", "40g", "g40", "4.0",
        "10000000000000000",             // 17 桁（64bit 超）
        "0x10000000000000000",           // 接頭辞付き 17 桁
    };
    for (const char* t : invalid) {
        FileOffset sentinel = 0x5A5A5A5A;
        CHECK(!ParseOffsetHex(t, sentinel), "reject invalid text");
        CHECK(sentinel == 0x5A5A5A5A, "invalid text leaves output untouched");
    }
    FileOffset nullOut = 7;
    // ナロー/ワイドの多重定義があるため、nullptr は型を明示して渡す
    CHECK(!ParseOffsetHex(static_cast<const char*>(nullptr), nullOut) && nullOut == 7,
          "reject nullptr");

    // 旧 32bit 形式（REG_DWORD）から移行した値も新形式で無損失に保存できる
    const FileOffset legacy = 0x7FFFFFFF;   // 32bit 版が保存しうる最大位置
    FileOffset migrated = 0;
    CHECK(ParseOffsetHex(FormatOffsetHex(legacy).c_str(), migrated) && migrated == legacy,
          "legacy 32bit value survives migration to the 64bit format");
}

// ---- 設定永続化コーデックのワイド版（Issue #43）----
//   Unicode ビルドではレジストリ値がワイドで得られる。ナロー版と同じ結果になること
//   （ASCII 層としての等価性）と、ワイド単体での往復・拒否を検証する。
static void TestSettingsCodecWide() {
    std::printf("TestSettingsCodecWide\n");
    using stirling::settings::FormatOffsetHex;
    using stirling::settings::FormatOffsetHexW;
    using stirling::settings::ParseOffsetHex;

    CHECK(FormatOffsetHexW(0) == L"0", "format 0 (wide)");
    CHECK(FormatOffsetHexW(0xABCDEF) == L"ABCDEF", "format uppercase (wide)");
    CHECK(FormatOffsetHexW(-1) == L"FFFFFFFFFFFFFFFF", "format -1 as two's complement (wide)");

    const FileOffset roundTrip[] = {
        0, 1, 0x7F, 0x80, 0xFFFF, 0x7FFFFFFF, 0x80000000LL, 0xFFFFFFFFLL,
        0x100000000LL, 0x1FFFFFFFFLL, 0x123456789ABCLL, INT64_MAX, -1, INT64_MIN,
    };
    for (FileOffset v : roundTrip) {
        const std::string  narrow = FormatOffsetHex(v);
        const std::wstring wide   = FormatOffsetHexW(v);
        // ナロー版と1文字ずつ一致する（ASCII 層なので符号化に依らない）
        CHECK(wide == std::wstring(narrow.begin(), narrow.end()),
              "wide format matches narrow format");
        FileOffset back = 0;
        CHECK(ParseOffsetHex(wide.c_str(), back) && back == v, "round trip (wide)");
    }

    FileOffset out = -123;
    CHECK(ParseOffsetHex(L"1ffffffff", out) && out == 0x1FFFFFFFFLL, "parse lowercase (wide)");
    CHECK(ParseOffsetHex(L"0x1FFFFFFFF", out) && out == 0x1FFFFFFFFLL, "parse 0x prefix (wide)");
    CHECK(ParseOffsetHex(L"0000000000000040", out) && out == 0x40,
          "parse 16 digits with leading zeros (wide)");

    const wchar_t* invalid[] = {
        L"", L"0x", L" 40", L"40 ", L"-1", L"40g", L"10000000000000000",
        L"４０",          // 全角数字（ASCII 層の外。全角は 16進とみなさない）
        L"\x0130",        // ラテン拡張（'I' の変種。ASCII 化して受理してはならない）
    };
    for (const wchar_t* t : invalid) {
        FileOffset sentinel = 0x5A5A5A5A;
        CHECK(!ParseOffsetHex(t, sentinel), "reject invalid text (wide)");
        CHECK(sentinel == 0x5A5A5A5A, "invalid text leaves output untouched (wide)");
    }
    FileOffset nullOut = 7;
    CHECK(!ParseOffsetHex(static_cast<const wchar_t*>(nullptr), nullOut) && nullOut == 7,
          "reject nullptr (wide)");
}

// ---- MBCS 版設定のエンコーディング移行（app/SettingsMigration.h。Issue #43）----
//   MBCS 版は文字列設定を CP932 バイト列として WriteProfileString へ渡していた。
//   RegSetValueExA はそれを「システム ANSI コードページ」として UTF-16 化するため、
//   ACP≠932 の環境では化けた値が格納されている。その巻き戻しを検証する。
static void TestSettingsMigration() {
    std::printf("TestSettingsMigration\n");
    using stirling::settings::RepairCp932ViaAcp;

    // MBCS 版の書き込みを再現する: CP932 バイト列を acp として UTF-16 化した結果を返す。
    //   RegSetValueExA の内部変換に相当（フラグなし ＝ 不正シーケンスも既定の置換で通す）。
    auto storedAs = [](const wchar_t* original, UINT acp) {
        std::string cp932;
        CHECK(stirling::Cp932FromWide(original, cp932), "test fixture: encodable in CP932");
        const int n = ::MultiByteToWideChar(acp, 0, cp932.c_str(),
                                            static_cast<int>(cp932.size()), nullptr, 0);
        std::wstring w(n > 0 ? static_cast<size_t>(n) : 0, L'\0');
        if (n > 0) {
            ::MultiByteToWideChar(acp, 0, cp932.c_str(), static_cast<int>(cp932.size()),
                                  &w[0], n);
        }
        return w;
    };

    const wchar_t* kFolder = L"C:\\作業\\バックアップ";
    const wchar_t* kFace   = L"ＭＳ ゴシック";
    std::wstring out;

    // ACP=932: 変換が恒等なので移行不要。値は既に正しく読めている
    out.clear();
    CHECK(storedAs(kFolder, 932) == kFolder, "ACP=932 stores the value correctly");
    CHECK(!RepairCp932ViaAcp(storedAs(kFolder, 932), 932, out) && out.empty(),
          "ACP=932 needs no repair");

    // ACP=1252 / 1250（SBCS 欧文）: 化けるが巻き戻せる
    const UINT sbcs[] = { 1252, 1250 };
    for (UINT acp : sbcs) {
        const std::wstring stored = storedAs(kFolder, acp);
        CHECK(stored != kFolder, "SBCS ACP mangles the value");
        out.clear();
        CHECK(RepairCp932ViaAcp(stored, acp, out) && out == kFolder,
              "SBCS ACP value is repaired");
        out.clear();
        CHECK(RepairCp932ViaAcp(storedAs(kFace, acp), acp, out) && out == kFace,
              "SBCS ACP font face is repaired");
    }

    // ACP=65001（UTF-8）: 書き込み時に U+FFFD へ潰れており復元不能。触らない
    out.clear();
    CHECK(!RepairCp932ViaAcp(storedAs(kFolder, 65001), 65001, out) && out.empty(),
          "UTF-8 ACP is not touched (unrecoverable)");

    // ACP=949（韓国語 DBCS）: バイト対が可逆に写る値は完全に復元できる
    if (::IsValidCodePage(949)) {
        out.clear();
        CHECK(RepairCp932ViaAcp(storedAs(kFolder, 949), 949, out) && out == kFolder,
              "DBCS ACP value is repaired when every byte pair maps reversibly");

        // 書き込み時点で CP949 が写せず '?' へ潰したバイト対は戻らない（部分復元）。
        //   ここでは実測した CP949 の挙動（本=CP932 0x967B が '?' になる）を固定する。
        //   将来 OS のコードページ表が変わればこの CHECK が落ち、再確認の契機になる。
        const wchar_t* kLossy = L"日本語";
        const std::wstring stored = storedAs(kLossy, 949);
        CHECK(stored.find(L'?') != std::wstring::npos,
              "CP949 already replaced an unmappable byte pair at write time");
        out.clear();
        CHECK(RepairCp932ViaAcp(stored, 949, out) && out == L"日?語",
              "DBCS ACP repair recovers everything except characters lost at write time");
    }

    // ASCII のみの値は化けないので、書き戻す必要がない
    out.clear();
    CHECK(!RepairCp932ViaAcp(L"C:\\Temp\\work", 1252, out) && out.empty(),
          "ASCII-only value needs no repair");
    out.clear();
    CHECK(!RepairCp932ViaAcp(L"", 1252, out) && out.empty(), "empty value needs no repair");

    // Unicode ビルドが既に正しく書いた値を壊さない（CP932 の外の文字を含む場合）
    out.clear();
    CHECK(!RepairCp932ViaAcp(L"C:\\한국어\\dir", 1252, out) && out.empty(),
          "value outside the ACP is left alone");
    out.clear();
    CHECK(!RepairCp932ViaAcp(kFolder, 1252, out) && out.empty(),
          "already-correct Japanese value is left alone");

    // ACP 内の単バイト文字だけで構成された正しい値を壊さない。
    //   ACP=1252 の L"C:\¥" は CP1252 で 43 3A 5C A5 になり、0xA5 は CP932 の半角カナ
    //   U+FF65 としても妥当に読めてしまう。2バイトシーケンスを要求する条件で弾く。
    const wchar_t* singleByteTraps[] = {
        L"C:\\\x00A5",        // ¥ → CP932 では半角カナ U+FF65
        L"C:\\\x00D7dir",     // × → CP932 では半角カナ U+FF57 相当の単バイト
        L"caf\x00E9",         // é（CP932 では 2 バイト対を作れず不正）
        L"\x00A3\x00A4",      // £¤ → いずれも CP932 の半角カナ範囲
    };
    for (const wchar_t* t : singleByteTraps) {
        out.clear();
        CHECK(!RepairCp932ViaAcp(t, 1252, out) && out.empty(),
              "single-byte-only value is not mistaken for mojibake");
    }
}

// ---- 設定ストア（Issue #96: 設定ファイルの INI 形式と UTF-8 往復） ----
static void TestSettingsStoreUtf8() {
    std::printf("TestSettingsStoreUtf8\n");
    using stirling::settings::Utf8ToWide;
    using stirling::settings::WideToUtf8;

    // ASCII・日本語・BMP 外（サロゲートペア）を往復する。
    const std::wstring samples[] = {
        L"",
        L"C:\\Users\\test\\backup",
        L"\u65e5\u672c\u8a9e\u306e\u30d5\u30a9\u30eb\u30c0",
        L"\U0001F600 emoji",
        L"mixed \u00e9 \u4e2d\u6587 123",
    };
    for (const std::wstring& sample : samples) {
        const std::string utf8 = WideToUtf8(sample);
        std::wstring back;
        CHECK(Utf8ToWide(utf8, back), "utf8 decode valid");
        CHECK(back == sample, "utf8 round trip");
    }

    // 既知のバイト列（UTF-8 として正しいこと）。
    CHECK(WideToUtf8(L"\u3042") == "\xE3\x81\x82", "utf8 encode HIRAGANA A");
    CHECK(WideToUtf8(L"\U0001F600") == "\xF0\x9F\x98\x80", "utf8 encode 4-byte");

    // 不正な UTF-8 は検出する（値は置換文字になるが処理は続く）。
    std::wstring decoded;
    CHECK(!Utf8ToWide(std::string("\xE3\x81"), decoded), "truncated sequence rejected");
    CHECK(!Utf8ToWide(std::string("\xC0\xAF"), decoded), "overlong sequence rejected");
    CHECK(!Utf8ToWide(std::string("\xED\xA0\x80"), decoded), "surrogate rejected");
    CHECK(Utf8ToWide(std::string("plain"), decoded) && decoded == L"plain", "ascii accepted");
}

static void TestSettingsStoreValueEscape() {
    std::printf("TestSettingsStoreValueEscape\n");
    using stirling::settings::DecodeValue;
    using stirling::settings::EncodeValue;

    // 普通の値は引用符を付けない（設定ファイルの可読性を保つ）。
    CHECK(EncodeValue(L"C:\\Program Files\\StirHex") == L"C:\\Program Files\\StirHex",
          "plain path stays raw");
    CHECK(EncodeValue(L"") == L"", "empty stays raw");
    CHECK(EncodeValue(L"\u65e5\u672c\u8a9e") == L"\u65e5\u672c\u8a9e", "japanese stays raw");

    // 前後の空白・制御文字・先頭の引用符だけを引用符付きにする。
    CHECK(EncodeValue(L" lead") == L"\" lead\"", "leading space quoted");
    CHECK(EncodeValue(L"trail ") == L"\"trail \"", "trailing space quoted");
    CHECK(EncodeValue(L"a\nb") == L"\"a\\nb\"", "newline escaped");
    CHECK(EncodeValue(L"\"quoted\"") == L"\"\\\"quoted\\\"\"", "leading quote escaped");

    // 往復（引用符が付く値・付かない値の両方）。
    const std::wstring samples[] = {
        L"", L"simple", L"C:\\path\\to\\file.bin", L" spaced ", L"tab\there",
        L"line1\r\nline2", L"\"q\"", L"back\\slash", L"\u3042\u3044\u3046",
        L"bell\x07end",
    };
    for (const std::wstring& sample : samples) {
        CHECK(DecodeValue(EncodeValue(sample)) == sample, "value escape round trip");
    }
}

// ---- マークファイル（Issue #99） ----

static void TestMarkFileRoundTrip() {
    std::printf("[TestMarkFileRoundTrip]\n");
    using stirling::marks::MarkFileData;
    using stirling::marks::ParseMarks;
    using stirling::marks::SerializeMarks;

    MarkFileData src;
    src.sourcePath = L"C:\\\u30c7\u30fc\u30bf\\sample.bin";   // 日本語パス
    src.sourceSize = 1048576;
    src.marks[0x40] = 1;
    src.marks[0xA0] = 2;
    src.marks[0x1F400] = 3;
    src.marks[0] = 1;                      // 先頭アドレス
    src.marks[0x7FFFFFFFFFFFFFFFLL] = 2;   // 64bit 上限（x64 化の確認）

    const std::wstring text = SerializeMarks(src);
    CHECK(text.find(L"[Mark]") != std::wstring::npos, "mark file has a header section");
    CHECK(text.find(L"[Marks]") != std::wstring::npos, "mark file has a marks section");
    CHECK(text.find(L"1F400=3") != std::wstring::npos, "address is uppercase hex without prefix");
    CHECK(text.find(L"7FFFFFFFFFFFFFFF=2") != std::wstring::npos, "64-bit address round trips");

    MarkFileData back;
    std::wstring error;
    CHECK(ParseMarks(text, back, error), "round trip parses");
    CHECK(error.empty(), "round trip reports no error");
    CHECK(back.marks == src.marks, "every mark survives the round trip");
    CHECK(back.sourcePath == src.sourcePath, "japanese path survives the round trip");
    CHECK(back.sourceSize == src.sourceSize, "size survives the round trip");
}

static void TestMarkFileEmptyAndComments() {
    std::printf("[TestMarkFileEmptyAndComments]\n");
    using stirling::marks::MarkFileData;
    using stirling::marks::ParseMarks;
    using stirling::marks::SerializeMarks;

    // マークが 0 件でも [Marks] を書く（読み込み側が「マークファイルか」を見分けるため）。
    MarkFileData empty;
    const std::wstring text = SerializeMarks(empty);
    CHECK(text.find(L"[Marks]") != std::wstring::npos, "empty export still has [Marks]");

    MarkFileData back;
    std::wstring error;
    CHECK(ParseMarks(text, back, error), "empty mark file parses");
    CHECK(back.marks.empty(), "empty mark file has no marks");

    // [Marks] が無いファイルはマーク 0 件として読む（識別は [Mark] の Version で行う）。
    MarkFileData headerOnly;
    CHECK(ParseMarks(L"[Mark]\nVersion=1\n", headerOnly, error),
          "a file without [Marks] is read as zero marks");
    CHECK(headerOnly.marks.empty(), "a file without [Marks] has no marks");

    // 手で書いた体裁（コメント・空行・小文字16進・前後の空白）も読めること。
    const std::wstring handwritten =
        L"; my marks\n"
        L"[Mark]\n"
        L"Version=1\n"
        L"\n"
        L"# section below\n"
        L"[marks]\n"
        L"  1f4 = 2 \n"
        L"0=1\n";
    MarkFileData hand;
    CHECK(ParseMarks(handwritten, hand, error), "hand written mark file parses");
    CHECK(hand.marks.size() == 2, "hand written file has two marks");
    CHECK(hand.marks[0x1F4] == 2, "lowercase hex and spaces are accepted");
    CHECK(hand.marks[0] == 1, "address zero is accepted");
    CHECK(hand.sourceSize == -1, "missing size reads as unknown");
}

static void TestMarkFileRejects() {
    std::printf("[TestMarkFileRejects]\n");
    using stirling::marks::MarkFileData;
    using stirling::marks::ParseMarks;

    MarkFileData out;
    std::wstring error;

    // 不正なファイルは「1件も適用しない」ことを併せて確認する。
    const std::wstring notAMarkFile = L"[Env]\nBackupFolder=C:\\tmp\n";
    CHECK(!ParseMarks(notAMarkFile, out, error), "settings file is not a mark file");
    CHECK(!error.empty(), "rejection explains itself");


    const std::wstring futureVersion = L"[Mark]\nVersion=2\n[Marks]\n40=1\n";
    CHECK(!ParseMarks(futureVersion, out, error), "unknown version is rejected");
    CHECK(error.find(L"2") != std::wstring::npos, "version error names the version");

    const std::wstring badAddress = L"[Mark]\nVersion=1\n[Marks]\n40=1\nXYZ=2\n";
    out.marks.clear();
    CHECK(!ParseMarks(badAddress, out, error), "non hex address is rejected");
    CHECK(error.find(L"XYZ") != std::wstring::npos, "address error names the offending key");
    CHECK(out.marks.empty(), "a rejected file applies nothing at all");

    const std::wstring badType = L"[Mark]\nVersion=1\n[Marks]\n40=4\n";
    CHECK(!ParseMarks(badType, out, error), "mark number out of 1..3 is rejected");
    const std::wstring zeroType = L"[Mark]\nVersion=1\n[Marks]\n40=0\n";
    CHECK(!ParseMarks(zeroType, out, error), "internal type 0 is not a valid file value");
}

// 手編集された長大な10進値（Issue #132）。符号付き乗算があふれてラップすると、
//   範囲検査をすり抜けて不正なファイルが受理されてしまう。
static void TestMarkFileHugeDecimals() {
    std::printf("[TestMarkFileHugeDecimals]\n");
    using stirling::marks::MarkFileData;
    using stirling::marks::ParseMarks;
    using stirling::marks::DecodeMarkList;

    MarkFileData out;
    std::wstring error;

    // 2^64+1。ラップすると 1（＝対応する形式版）になり、受理されてしまっていた。
    const std::wstring wrappedVersion =
        L"[Mark]\nVersion=18446744073709551617\n[Marks]\n40=1\n";
    out.marks.clear();
    CHECK(!ParseMarks(wrappedVersion, out, error), "a wrapping Version is rejected");
    CHECK(out.marks.empty(), "a rejected file applies nothing at all");

    // 上限ちょうど（LLONG_MAX）は解析でき、1桁超えたものは必ず拒否する。
    const std::wstring maxVersion = L"[Mark]\nVersion=9223372036854775807\n[Marks]\n40=1\n";
    CHECK(!ParseMarks(maxVersion, out, error), "LLONG_MAX parses but is not a known version");
    CHECK(error.find(L"9223372036854775807") != std::wstring::npos,
          "the version error still names the value it read");
    const std::wstring overMax = L"[Mark]\nVersion=9223372036854775808\n[Marks]\n40=1\n";
    CHECK(!ParseMarks(overMax, out, error), "one past LLONG_MAX is rejected");

    // Size は情報でしかないため読み込みは続くが、範囲外の値は「不明」(-1) に倒す。
    const std::wstring hugeSize =
        L"[Mark]\nVersion=1\nSize=18446744073709551617\n[Marks]\n40=1\n";
    CHECK(ParseMarks(hugeSize, out, error), "a broken Size does not fail the load");
    CHECK(out.sourceSize == -1, "an out-of-range Size becomes unknown");
    const std::wstring maxSize =
        L"[Mark]\nVersion=1\nSize=9223372036854775807\n[Marks]\n40=1\n";
    CHECK(ParseMarks(maxSize, out, error), "LLONG_MAX Size parses");
    CHECK(out.sourceSize == 9223372036854775807ll, "LLONG_MAX Size is kept as is");
    const std::wstring overSize =
        L"[Mark]\nVersion=1\nSize=9223372036854775808\n[Marks]\n40=1\n";
    CHECK(ParseMarks(overSize, out, error), "one past LLONG_MAX Size does not fail the load");
    CHECK(out.sourceSize == -1, "one past LLONG_MAX Size becomes unknown");

    // マーク種別の経路（マークファイル / 1行表現）にも同じ解析関数を使う。
    const std::wstring hugeType =
        L"[Mark]\nVersion=1\n[Marks]\n40=18446744073709551617\n";
    out.marks.clear();
    CHECK(!ParseMarks(hugeType, out, error), "a wrapping mark number is rejected");
    CHECK(out.marks.empty(), "a rejected file applies nothing at all");

    std::map<stirling::FileOffset, int> list;
    list[0x10] = 1;
    CHECK(!DecodeMarkList(L"40:18446744073709551617", list),
          "a wrapping mark number is rejected in the one-line form");
    CHECK(list.size() == 1 && list.count(0x10) == 1,
          "a rejected list leaves the target alone");
}

// ---- マークの1行表現（自動保存／自動復元。Issue #100） ----

static void TestMarkListRoundTrip() {
    std::printf("[TestMarkListRoundTrip]\n");
    using stirling::marks::DecodeMarkList;
    using stirling::marks::EncodeMarkList;

    std::map<stirling::FileOffset, int> marks;
    marks[0x40] = 1;
    marks[0xA0] = 2;
    marks[0x1F400] = 3;
    marks[0] = 1;

    const std::wstring text = EncodeMarkList(marks);
    CHECK(text == L"0:1,40:1,A0:2,1F400:3", "encoded in ascending address order");

    std::map<stirling::FileOffset, int> back;
    CHECK(DecodeMarkList(text, back), "the encoded list decodes");
    CHECK(back == marks, "every mark survives the round trip");

    // 空はエラーではない（マークが1件も無い状態を表す）。
    std::map<stirling::FileOffset, int> empty;
    CHECK(EncodeMarkList(empty).empty(), "no marks encode to an empty value");
    std::map<stirling::FileOffset, int> decoded;
    CHECK(DecodeMarkList(L"", decoded), "an empty value decodes");
    CHECK(decoded.empty(), "an empty value means no marks");

    // 64bit アドレス（x64 化の確認）。
    std::map<stirling::FileOffset, int> wide;
    wide[0x7FFFFFFFFFFFFFFFLL] = 3;
    std::map<stirling::FileOffset, int> wideBack;
    CHECK(DecodeMarkList(EncodeMarkList(wide), wideBack), "64-bit address round trips");
    CHECK(wideBack == wide, "64-bit address keeps its value");
}

static void TestMarkListLimitAndRejects() {
    std::printf("[TestMarkListLimitAndRejects]\n");
    using stirling::marks::DecodeMarkList;
    using stirling::marks::EncodeMarkList;
    using stirling::marks::kMaxStoredMarks;

    // 上限を超える分はアドレスの大きい側から捨てる（先頭 kMaxStoredMarks 件を残す）。
    std::map<stirling::FileOffset, int> many;
    for (size_t i = 0; i < kMaxStoredMarks + 10; ++i) {
        many[static_cast<stirling::FileOffset>(i)] = 1;
    }
    std::map<stirling::FileOffset, int> capped;
    CHECK(DecodeMarkList(EncodeMarkList(many), capped), "a capped list is still valid");
    CHECK(capped.size() == kMaxStoredMarks, "the list is capped");
    CHECK(capped.count(0) == 1, "the lowest address is kept");
    CHECK(capped.count(static_cast<stirling::FileOffset>(kMaxStoredMarks)) == 0,
          "addresses past the cap are dropped");

    // 壊れた値は1件も採らない（キャレットストアと同じく、その1件を捨てる判断は呼び出し側）。
    std::map<stirling::FileOffset, int> out;
    out[0x10] = 1;
    CHECK(!DecodeMarkList(L"40:1,ZZ:2", out), "a non hex address is rejected");
    CHECK(out.size() == 1 && out.count(0x10) == 1, "a rejected list leaves the target alone");
    CHECK(!DecodeMarkList(L"40:4", out), "a mark number out of 1..3 is rejected");
    CHECK(!DecodeMarkList(L"40:0", out), "the internal type 0 is not a valid stored value");
    CHECK(!DecodeMarkList(L"40", out), "an item without a type is rejected");

    // 区切りが続いた場合は空要素として読み飛ばす（手編集への耐性）。
    std::map<stirling::FileOffset, int> lenient;
    CHECK(DecodeMarkList(L"40:1,,A0:2,", lenient), "empty items are skipped");
    CHECK(lenient.size() == 2, "the surrounding items are still read");
}

static void TestSettingsStoreIni() {
    std::printf("TestSettingsStoreIni\n");
    using stirling::settings::SettingsStore;

    SettingsStore store;
    CHECK(store.Empty(), "new store is empty");
    CHECK(!store.Dirty(), "new store is clean");

    store.Set(L"Env", L"ScrollLines", L"3");
    CHECK(store.Dirty(), "set marks dirty");
    store.ClearDirty();

    // 同じ値の再設定では書き込みを起こさない。
    store.Set(L"Env", L"ScrollLines", L"3");
    CHECK(!store.Dirty(), "same value keeps clean");
    store.Set(L"Env", L"ScrollLines", L"4");
    CHECK(store.Dirty(), "changed value marks dirty");

    store.Set(L"Env", L"BackupFolder", L"C:\\backup\\\u65e5\u672c\u8a9e");
    store.Set(L"Recent File List", L"File1", L"D:\\data\\sample.bin");

    // 大文字小文字を区別せずに引ける（レジストリの挙動に合わせる）。
    const std::wstring* found = store.Find(L"env", L"scrolllines");
    CHECK(found != nullptr && *found == L"4", "lookup is case-insensitive");
    CHECK(store.Find(L"Env", L"Missing") == nullptr, "missing key returns null");
    CHECK(store.Find(L"Missing", L"ScrollLines") == nullptr, "missing section returns null");

    // シリアライズ→パースで内容が一致する。
    const std::wstring text = store.Serialize();
    CHECK(text.find(L"[Env]") != std::wstring::npos, "section header written");
    CHECK(text.find(L"BackupFolder=C:\\backup\\") != std::wstring::npos,
          "path value written unquoted");

    SettingsStore reloaded;
    CHECK(reloaded.ParseInto(text), "serialized text parses cleanly");
    CHECK(!reloaded.Dirty(), "parse does not mark dirty");
    for (const SettingsStore::Section& section : store.Sections()) {
        for (const SettingsStore::Entry& entry : section.entries) {
            const std::wstring* value = reloaded.Find(section.name, entry.key);
            CHECK(value != nullptr && *value == entry.value, "ini round trip");
        }
    }

    // 削除。
    store.Remove(L"Env", L"ScrollLines");
    CHECK(store.Find(L"Env", L"ScrollLines") == nullptr, "value removed");
    store.RemoveSection(L"Recent File List");
    CHECK(store.Find(L"Recent File List", L"File1") == nullptr, "section removed");

    // コメント・空行・前後の空白を含むファイルを読む。
    SettingsStore parsed;
    const std::wstring source =
        L"; StirHex settings\r\n"
        L"# another comment\r\n"
        L"\r\n"
        L"  [Env]  \r\n"
        L"  ScrollLines = 7 \r\n"
        L"BackupFolder=C:\\dir with space\\x\r\n"
        L"Quoted=\" padded \"\r\n"
        L"[Rec0]\r\n"
        L"FontFace=\uff2d\uff33 \u30b4\u30b7\u30c3\u30af\r\n";
    CHECK(parsed.ParseInto(source), "well-formed file parses cleanly");
    const std::wstring* scroll = parsed.Find(L"Env", L"ScrollLines");
    CHECK(scroll != nullptr && *scroll == L"7", "key and value are trimmed");
    const std::wstring* folder = parsed.Find(L"Env", L"BackupFolder");
    CHECK(folder != nullptr && *folder == L"C:\\dir with space\\x", "inner spaces kept");
    const std::wstring* quoted = parsed.Find(L"Env", L"Quoted");
    CHECK(quoted != nullptr && *quoted == L" padded ", "quoted value keeps outer spaces");
    const std::wstring* font = parsed.Find(L"Rec0", L"FontFace");
    CHECK(font != nullptr && *font == L"\uff2d\uff33 \u30b4\u30b7\u30c3\u30af", "japanese value");

    // 壊れた行は false を返しつつ、読める行は取り込む。
    SettingsStore lenient;
    const std::wstring broken =
        L"[Env\r\n"          // 閉じ括弧なし
        L"NoEquals\r\n"      // = なし
        L"=novalue\r\n"      // キーなし
        L"[Env]\r\n"
        L"Good=1\r\n";
    CHECK(!lenient.ParseInto(broken), "broken lines are reported");
    const std::wstring* good = lenient.Find(L"Env", L"Good");
    CHECK(good != nullptr && *good == L"1", "readable lines survive broken ones");
}

// --- 変更記録とマージ保存（Issue #130） ---
//   複数インスタンスが同じ設定ファイルを使うとき、終了時に自分の古いスナップショット
//   全体で置換すると別プロセスの更新が消える。ストアは「自分が加えた変更」を記録し、
//   保存側は最新のファイル内容へその変更だけを適用する。

static void TestSettingsStoreChangeLog() {
    std::printf("TestSettingsStoreChangeLog\n");
    using stirling::settings::SettingsStore;

    SettingsStore store;
    CHECK(store.Changes().empty(), "new store has no changes");

    store.Set(L"Env", L"A", L"1");
    store.Set(L"Env", L"B", L"2");
    CHECK(store.Changes().size() == 2, "each set is recorded");
    store.Set(L"Env", L"A", L"1");
    CHECK(store.Changes().size() == 2, "an unchanged value is not recorded");
    store.Remove(L"Env", L"B");
    CHECK(store.Changes().size() == 3, "remove is recorded");
    store.Remove(L"Env", L"Missing");
    CHECK(store.Changes().size() == 3, "removing a missing key is not recorded");

    // 記録を別のストアへ適用すると同じ状態になる。
    SettingsStore target;
    target.Set(L"Env", L"Other", L"9");   // 別プロセスが書いた値
    target.ApplyChanges(store.Changes());
    const std::wstring* a = target.Find(L"Env", L"A");
    CHECK(a != nullptr && *a == L"1", "applied set");
    CHECK(target.Find(L"Env", L"B") == nullptr, "applied remove");
    const std::wstring* other = target.Find(L"Env", L"Other");
    CHECK(other != nullptr && *other == L"9", "the other process value survives the merge");

    store.ClearDirty();
    CHECK(store.Changes().empty(), "ClearDirty drops the recorded changes");

    // 読み込みは変更として記録しない。
    SettingsStore parsed;
    CHECK(parsed.ParseInto(L"[Env]\r\nA=1\r\n"), "parses");
    CHECK(parsed.Changes().empty(), "parse records no change");

    // セクション削除も記録され、マージ先へ伝わる。
    SettingsStore remover;
    remover.Set(L"MarkStore", L"Count", L"1");
    remover.ClearDirty();
    remover.RemoveSection(L"MarkStore");
    SettingsStore target2;
    target2.Set(L"MarkStore", L"Count", L"1");
    target2.ApplyChanges(remover.Changes());
    CHECK(target2.Find(L"MarkStore", L"Count") == nullptr, "applied section removal");
}

// テスト用の一時設定ファイルパス（実ファイルを触るためテンポラリへ置く）。
static std::wstring MergeTestIniPath(const wchar_t* name) {
    wchar_t dir[MAX_PATH] = {0};
    const DWORD n = ::GetTempPathW(MAX_PATH, dir);
    CHECK(n > 0 && n < MAX_PATH, "temp path");
    return std::wstring(dir) + name;
}

static void TestSettingsFileMergedSave() {
    std::printf("TestSettingsFileMergedSave\n");
    using stirling::settings::SettingsStore;
    using stirling::settings::LoadSettingsFile;
    using stirling::settings::SaveSettingsFile;
    using stirling::settings::SaveSettingsFileMerged;

    const std::wstring path = MergeTestIniPath(L"stirhex_merge_test.ini");
    ::DeleteFileW(path.c_str());

    std::wstring error;
    SettingsStore seed;
    seed.Set(L"Env", L"Common", L"1");
    seed.Set(L"Recent File List", L"File1", L"D:\\data\\a.bin");
    CHECK(SaveSettingsFile(path, seed, error), "seed written");

    // 2プロセス相当。どちらも同じ時点のスナップショットを持つ。
    SettingsStore first, second;
    CHECK(LoadSettingsFile(path, first, error), "first snapshot");
    CHECK(LoadSettingsFile(path, second, error), "second snapshot");
    first.ClearDirty();
    second.ClearDirty();

    // 先に終了したプロセスが Env を更新する。
    first.Set(L"Env", L"FromFirst", L"10");
    first.Set(L"Env", L"Common", L"2");
    CHECK(SaveSettingsFileMerged(path, first, error), "first save");
    CHECK(!first.Dirty(), "first store is clean after saving");

    // 後から終了したプロセスは古い内容のまま別セクションを更新する。
    second.Set(L"CaretPositions", L"Addr0", L"20");
    CHECK(SaveSettingsFileMerged(path, second, error), "second save");

    SettingsStore merged;
    CHECK(LoadSettingsFile(path, merged, error), "reload");
    const std::wstring* fromFirst = merged.Find(L"Env", L"FromFirst");
    CHECK(fromFirst != nullptr && *fromFirst == L"10",
          "the earlier process update survives the later save");
    const std::wstring* common = merged.Find(L"Env", L"Common");
    CHECK(common != nullptr && *common == L"2",
          "an unchanged key is not reverted to the stale snapshot value");
    const std::wstring* fromSecond = merged.Find(L"CaretPositions", L"Addr0");
    CHECK(fromSecond != nullptr && *fromSecond == L"20", "the later update is written");
    const std::wstring* untouched = merged.Find(L"Recent File List", L"File1");
    CHECK(untouched != nullptr && *untouched == L"D:\\data\\a.bin", "untouched keys survive");

    // 変更が無ければ書きに行かない（ファイルの更新時刻も変えない）。
    SettingsStore clean;
    CHECK(LoadSettingsFile(path, clean, error), "clean snapshot");
    clean.ClearDirty();
    CHECK(SaveSettingsFileMerged(path, clean, error), "no-op save succeeds");

    // 一時ファイルを残さない。
    const std::wstring temp = path + L"." + std::to_wstring(::GetCurrentProcessId()) + L".tmp";
    CHECK(::GetFileAttributesW(temp.c_str()) == INVALID_FILE_ATTRIBUTES,
          "the temp file is gone after saving");

    ::DeleteFileW(path.c_str());
}

static void TestSettingsFileConcurrentSave() {
    std::printf("TestSettingsFileConcurrentSave\n");
    using stirling::settings::SettingsStore;
    using stirling::settings::LoadSettingsFile;
    using stirling::settings::SaveSettingsFile;
    using stirling::settings::SaveSettingsFileMerged;

    const std::wstring path = MergeTestIniPath(L"stirhex_concurrent_test.ini");
    ::DeleteFileW(path.c_str());

    std::wstring error;
    SettingsStore seed;
    seed.Set(L"Env", L"Common", L"1");
    CHECK(SaveSettingsFile(path, seed, error), "seed written");

    // 同じ設定ファイルへ同時に書き込んでも壊れず、どちらの更新も残ること。
    //   （プロセス間ロックは同一プロセスのスレッド間でも効く）
    const int kRounds = 25;
    auto writer = [&path](const wchar_t* section, int rounds) {
        for (int i = 0; i < rounds; ++i) {
            SettingsStore store;
            std::wstring err;
            if (!LoadSettingsFile(path, store, err)) { continue; }
            store.ClearDirty();
            store.Set(section, (L"Key" + std::to_wstring(i)).c_str(), std::to_wstring(i));
            SaveSettingsFileMerged(path, store, err);
        }
    };
    std::thread a(writer, L"WriterA", kRounds);
    std::thread b(writer, L"WriterB", kRounds);
    a.join();
    b.join();

    SettingsStore result;
    CHECK(LoadSettingsFile(path, result, error), "the file is still readable");
    const std::wstring* common = result.Find(L"Env", L"Common");
    CHECK(common != nullptr && *common == L"1", "the seed value survives");
    int foundA = 0, foundB = 0;
    for (int i = 0; i < kRounds; ++i) {
        const std::wstring key = L"Key" + std::to_wstring(i);
        if (result.Find(L"WriterA", key) != nullptr) { ++foundA; }
        if (result.Find(L"WriterB", key) != nullptr) { ++foundB; }
    }
    CHECK(foundA == kRounds, "every WriterA update is kept");
    CHECK(foundB == kRounds, "every WriterB update is kept");

    ::DeleteFileW(path.c_str());
}

// エクスプローラで開くフォルダの決定（Issue #133）。相対 /ini パスの未作成ファイルでも
//   保存先（カレントディレクトリ）へ辿り着けること。
static void TestFolderToReveal() {
    std::printf("[TestFolderToReveal]\n");
    using stirling::path::FolderToReveal;
    using stirling::path::IsRooted;
    using stirling::path::ParentFolder;

    // 親フォルダの取り出し。ルート直下は区切りを残す。
    CHECK(ParentFolder(L"C:\\dir\\StirHex.ini") == L"C:\\dir", "parent of a nested path");
    CHECK(ParentFolder(L"C:\\StirHex.ini") == L"C:\\", "parent at the drive root keeps the separator");
    CHECK(ParentFolder(L"\\StirHex.ini") == L"\\", "parent at the root keeps the separator");
    CHECK(ParentFolder(L"sub/StirHex.ini") == L"sub", "forward slashes are separators too");
    CHECK(ParentFolder(L"StirHex.ini").empty(), "a bare file name has no parent");

    CHECK(IsRooted(L"C:\\dir"), "drive absolute");
    CHECK(IsRooted(L"\\\\server\\share"), "UNC");
    CHECK(!IsRooted(L"sub"), "a relative folder is not rooted");
    CHECK(!IsRooted(L"C:sub"), "a drive relative path is not rooted");

    const std::wstring cwd = L"D:\\work";

    // この Issue の主眼: 親フォルダ部分の無い相対ファイル名は保存先＝カレントへ倒す。
    CHECK(FolderToReveal(L"StirHex.ini", cwd) == cwd,
          "a bare relative file name reveals the current directory");
    // 相対サブフォルダはカレントからの絶対パスにする。
    CHECK(FolderToReveal(L"sub\\StirHex.ini", cwd) == L"D:\\work\\sub",
          "a relative sub folder is resolved against the current directory");
    // 絶対パスは従来どおりその親フォルダ。
    CHECK(FolderToReveal(L"C:\\dir\\StirHex.ini", cwd) == L"C:\\dir",
          "an absolute path keeps its own parent");
    CHECK(FolderToReveal(L"C:\\StirHex.ini", cwd) == L"C:\\",
          "a file at the drive root reveals the root");

    // カレントの末尾区切りで区切りが重ならない。
    CHECK(FolderToReveal(L"sub\\StirHex.ini", L"D:\\work\\") == L"D:\\work\\sub",
          "a trailing separator on the current directory is not doubled");
    CHECK(FolderToReveal(L"StirHex.ini", L"D:\\") == L"D:\\",
          "the drive root as the current directory is kept as is");

    // 決められないのは対象パスが空のときだけ。
    CHECK(FolderToReveal(L"", cwd).empty(), "an empty path has no folder");
    CHECK(FolderToReveal(L"StirHex.ini", L"").empty(),
          "without a current directory a bare file name cannot be resolved");
}

static void TestSettingsStoreBinary() {
    std::printf("TestSettingsStoreBinary\n");
    using stirling::settings::BytesToHex;
    using stirling::settings::HexToBytes;

    const unsigned char blob[] = { 0x00, 0x01, 0x7F, 0x80, 0xFF, 0xA5 };
    const std::wstring hex = BytesToHex(blob, sizeof(blob));
    CHECK(hex == L"00017F80FFA5", "binary encoded as uppercase hex");

    std::vector<unsigned char> back;
    CHECK(HexToBytes(hex, back), "hex decodes");
    CHECK(back.size() == sizeof(blob) &&
          std::memcmp(back.data(), blob, sizeof(blob)) == 0, "binary round trip");

    // キーマップ相当（256 UINT = 1024 バイト）の往復。
    std::vector<unsigned char> keymap(1024);
    for (size_t i = 0; i < keymap.size(); ++i) {
        keymap[i] = static_cast<unsigned char>(i * 7 + 3);
    }
    std::vector<unsigned char> keymapBack;
    CHECK(HexToBytes(BytesToHex(keymap.data(), keymap.size()), keymapBack), "keymap decodes");
    CHECK(keymapBack == keymap, "keymap round trip");

    // 不正な16進は拒否する（壊れた設定ファイルで黙って値を作らない）。
    CHECK(!HexToBytes(L"ABC", back), "odd length rejected");
    CHECK(!HexToBytes(L"AXBC", back), "non-hex rejected");
    CHECK(HexToBytes(L"", back) && back.empty(), "empty accepted as empty");
}

static void TestCp932Text() {
    std::printf("TestCp932Text\n");
    using stirling::Cp932FromWide;
    using stirling::WideFromCp932;

    // CP932 の代表的な全角文字（「あ」= 0x82 0xA0、「漢」= 0x8A 0xBF）。
    const char kAiu[] = "\x82\xA0\x82\xA2\x82\xA4";          // あいう
    const wchar_t kAiuW[] = L"あいう";

    // バイト列 → ワイド
    CHECK(WideFromCp932(kAiu) == kAiuW, "WideFromCp932 zenkaku");
    CHECK(WideFromCp932("ABC") == L"ABC", "WideFromCp932 ascii");
    CHECK(WideFromCp932(kAiu, 2) == std::wstring(L"あ"), "WideFromCp932 honors len");
    CHECK(WideFromCp932(nullptr).empty(), "WideFromCp932 nullptr");
    CHECK(WideFromCp932("", 0).empty(), "WideFromCp932 empty");

    // 埋め込み NUL を含む区間も長さ指定で扱える（表示用途で切り出す場合）
    CHECK(WideFromCp932("A\0B", 3) == std::wstring(L"A\0B", 3), "WideFromCp932 embedded NUL");

    // ワイド → バイト列
    std::string out;
    CHECK(Cp932FromWide(kAiuW, out) && out == kAiu, "Cp932FromWide zenkaku");
    CHECK(Cp932FromWide(L"ABC", out) && out == "ABC", "Cp932FromWide ascii");
    CHECK(Cp932FromWide(kAiuW, out, 1) && out == std::string("\x82\xA0"), "Cp932FromWide honors len");
    CHECK(Cp932FromWide(nullptr, out) && out.empty(), "Cp932FromWide nullptr");
    CHECK(Cp932FromWide(L"", out) && out.empty(), "Cp932FromWide empty");

    // 往復（CP932 で表現できる範囲は無損失）
    const wchar_t* const roundTrip[] = {
        L"ABC", L"あいう", L"ｱｲｳ" /* 半角カナ */,
        L"①" /* ① NEC 特殊文字 */, L"aあbいc",
    };
    for (const wchar_t* w : roundTrip) {
        std::string bytes;
        CHECK(Cp932FromWide(w, bytes), "round trip encodes");
        CHECK(WideFromCp932(bytes.c_str(), static_cast<int>(bytes.size())) == std::wstring(w),
              "round trip decodes");
    }

    // CP932 に無い文字は best-fit で潰さず失敗させる（検索パターン生成での欠落防止）
    const wchar_t* const unmappable[] = {
        L"€",              // € (CP932 に無い)
        L"À",              // A grave
        L"OK€NG",          // 一部だけ変換不能でも全体を拒否する
        L"\U0001F600",          // 絵文字（サロゲートペア）
    };
    for (const wchar_t* w : unmappable) {
        std::string bytes = "sentinel";
        CHECK(!Cp932FromWide(w, bytes), "reject unmappable char");
        CHECK(bytes.empty(), "unmappable clears output");
    }

    // 不正バイト列は表示方向では best-effort（例外や中断にしない）
    const std::wstring broken = WideFromCp932("\x82", 1);   // 先行バイトのみ
    CHECK(broken.size() <= 1, "WideFromCp932 tolerates truncated lead byte");
}


// CP932 固定の先行バイト判定（Issue #42）。文字ペインの DBCS ペア認識に使う。
static void TestCp932LeadByte() {
    std::printf("TestCp932LeadByte\n");
    using stirling::IsCp932LeadByte;

    // 仕様: 0x81-0x9F / 0xE0-0xFC のみが 2 バイト文字の先行バイト。
    for (int i = 0; i < 256; ++i) {
        const unsigned char b = static_cast<unsigned char>(i);
        const bool expect = (b >= 0x81 && b <= 0x9f) || (b >= 0xe0 && b <= 0xfc);
        CHECK(IsCp932LeadByte(b) == expect, "IsCp932LeadByte matches CP932 range");
    }

    // 境界値（表の端）を明示的にも押さえる
    CHECK(!IsCp932LeadByte(0x80), "0x80 is not a lead byte");
    CHECK(IsCp932LeadByte(0x81), "0x81 is a lead byte");
    CHECK(IsCp932LeadByte(0x9f), "0x9f is a lead byte");
    CHECK(!IsCp932LeadByte(0xa0), "0xa0 is not a lead byte");
    CHECK(!IsCp932LeadByte(0xa1), "half-width katakana 0xa1 is single byte");
    CHECK(!IsCp932LeadByte(0xdf), "half-width katakana 0xdf is single byte");
    CHECK(IsCp932LeadByte(0xe0), "0xe0 is a lead byte");
    CHECK(IsCp932LeadByte(0xfc), "0xfc is a lead byte");
    CHECK(!IsCp932LeadByte(0xfd), "0xfd is not a lead byte");
    CHECK(!IsCp932LeadByte(0xff), "0xff is not a lead byte");

    // コンパイル時に評価できる（constexpr）
    static_assert(IsCp932LeadByte(0x82), "constexpr lead byte");
    static_assert(!IsCp932LeadByte(0x41), "constexpr non-lead byte");

    // 実 CP932 文字の先行バイトを拾えること（あ=0x82A0 / 漢=0x8ABF）
    CHECK(IsCp932LeadByte(static_cast<unsigned char>("\x82\xa0"[0])), "lead byte of hiragana A");
    CHECK(IsCp932LeadByte(static_cast<unsigned char>("\x8a\xbf"[0])), "lead byte of kanji");

    // CP932 を明示したシステム判定（::IsDBCSLeadByteEx）と全バイトで一致する。
    //   この比較はシステム ANSI コードページに依存しないため、非日本語環境でも成立する。
    for (int i = 0; i < 256; ++i) {
        const unsigned char b = static_cast<unsigned char>(i);
        CHECK(IsCp932LeadByte(b) == (::IsDBCSLeadByteEx(932, b) != FALSE),
              "matches IsDBCSLeadByteEx(932)");
    }

    // ACP==932 の環境では原版が使う ::IsDBCSLeadByte とも一致する（原との等価性）。
    //   非日本語環境では ::IsDBCSLeadByte が常に false を返す＝原版の不具合なので比較しない。
    if (::GetACP() == 932) {
        for (int i = 0; i < 256; ++i) {
            const unsigned char b = static_cast<unsigned char>(i);
            CHECK(IsCp932LeadByte(b) == (::IsDBCSLeadByte(b) != FALSE),
                  "matches IsDBCSLeadByte on ACP=932");
        }
    } else {
        std::printf("  (skip: ACP=%u, IsDBCSLeadByte comparison is JP-only)\n",
                    static_cast<unsigned int>(::GetACP()));
    }
}

// 構造体編集バーの char 配列文字列化（byte 層）。Unicode 文字セットの写像は CP932 固定。
static void TestFormatStructCharArrayCp932() {
    std::printf("[TestFormatStructCharArrayCp932]\n");
    using stirling::FormatStructCharArrayCp932;   // 0..5 の CP932 表現（Issue #107 で改称）

    // charset 0 (ASCII): 非印字は '.'
    const unsigned char ascii[] = { 'A', 0x00, 'B', 0x7f, 'C' };
    CHECK(FormatStructCharArrayCp932(0, ascii, 5) == "A.B.C", "ASCII maps non-printable to dot");

    // charset 1 (SJIS): 生バイトをそのまま連結（フォントが描画）
    const unsigned char sjis[] = { 0x82, 0xa0, 'A' };
    CHECK(FormatStructCharArrayCp932(1, sjis, 3) == std::string("\x82\xa0" "A"),
          "SJIS keeps raw bytes");

    // charset 3 (UTF-16LE): CP932 バイト列を返す（システム ANSI コードページ非依存）
    const unsigned char utf16[] = { 0x42, 0x30, 'A', 0x00 };   // U+3042 'あ', U+0041 'A'
    CHECK(FormatStructCharArrayCp932(3, utf16, 4) == std::string("\x82\xa0" "A"),
          "UTF-16LE maps to CP932 bytes");

    // CP932 に無い文字は既定文字 '.'（原の既定文字指定に一致）
    const unsigned char utf16Euro[] = { 0xac, 0x20 };          // U+20AC euro sign
    CHECK(FormatStructCharArrayCp932(3, utf16Euro, 2) == ".", "unmappable maps to dot");

    // 端数バイト（2 バイト未満）は空
    const unsigned char odd[] = { 0x42 };
    CHECK(FormatStructCharArrayCp932(3, odd, 1).empty(), "odd tail yields empty");

    // 引数の防御
    CHECK(FormatStructCharArrayCp932(0, nullptr, 4).empty(), "nullptr yields empty");
    CHECK(FormatStructCharArrayCp932(0, ascii, 0).empty(), "n=0 yields empty");

    // charset 2 (EUC-JP): 主プレーン対は SJIS へ、EUC 外は原と同じ写像（Issue #42）
    const unsigned char euc[] = { 0xa4, 0xa2 };                // EUC "あ" -> SJIS 0x82A0
    CHECK(FormatStructCharArrayCp932(2, euc, 2) == std::string("\x82\xa0"),
          "EUC main plane pair maps to SJIS");
    const unsigned char eucKana[] = { 0x8e, 0xb1 };            // 単一シフト + 半角カナ
    CHECK(FormatStructCharArrayCp932(2, eucKana, 2) == std::string("\xb1"),
          "EUC single shift yields the kana byte");
    const unsigned char eucKanaBad[] = { 0x8e, 0x41 };         // シフト対象外は 0x8e を生で
    CHECK(FormatStructCharArrayCp932(2, eucKanaBad, 2) == std::string("\x8e" "A"),
          "EUC single shift with bad trail keeps 0x8e");
    const unsigned char eucKanaEof[] = { 0x41, 0x8e };         // 末尾の 0x8e は打ち切り
    CHECK(FormatStructCharArrayCp932(2, eucKanaEof, 2) == "A",
          "EUC single shift at end truncates");
    const unsigned char eucLoneLead[] = { 0xa4 };              // 対にならない主プレーン先頭
    CHECK(FormatStructCharArrayCp932(2, eucLoneLead, 1) == ".", "EUC lone lead maps to dot");
    const unsigned char eucBadPair[] = { 0xa4, 0x20 };         // 主プレーン先頭 + 非主プレーン
    CHECK(FormatStructCharArrayCp932(2, eucBadPair, 2) == ". ", "EUC broken pair maps to dot");
    // EUC 外のバイトは印字可能 ASCII のみ通し、それ以外は '.'（SJIS 生バイトは化けない）
    const unsigned char eucSjis[] = { 0x82, 0x6c, 0x82, 0x72, 0x0a };   // SJIS "ＭＳ" + LF
    CHECK(FormatStructCharArrayCp932(2, eucSjis, 5) == ".l.r.", "EUC filters non-EUC bytes");

    // 256 バイト上限（原 StructRow_BuildColumns の 0x100 上限）
    const std::vector<unsigned char> big(0x180, 'A');
    CHECK(FormatStructCharArrayCp932(0, big.data(), static_cast<int>(big.size())).size() == 0x100,
          "caps at 256 bytes");
}

// 構造体編集バーの値列（ワイド）。Issue #107: UTF-8 を CP932 へ落とさず表示する。
//   0..5 は「CP932 版の結果をワイドへ変換したもの」と一致すること＝表示結果が
//   従来（表示直前に WideFromCp932 していた頃）と変わらないことを担保する。
static void TestFormatStructCharArrayW() {
    std::printf("[TestFormatStructCharArrayW]\n");
    using stirling::FormatStructCharArrayCp932;
    using stirling::FormatStructCharArrayW;
    using stirling::WideFromCp932;

    // --- 0..5 は CP932 版をワイド化したものと同じ（非退行の担保） ---
    struct Sample { int charset; std::vector<unsigned char> bytes; const char* what; };
    const Sample samples[] = {
        { 0, { 'A', 0x00, 'B', 0x7f, 'C' },        "ASCII" },
        { 1, { 0x82, 0xa0, 'A' },                  "SJIS" },
        { 2, { 0xa4, 0xa2 },                       "EUC main plane" },
        { 2, { 0x8e, 0xb1 },                       "EUC single shift" },
        { 3, { 0x42, 0x30, 'A', 0x00 },            "UTF-16LE" },
        { 3, { 0xac, 0x20 },                       "UTF-16LE unmappable" },
        { 4, { 0xc1, 0xc2, 0x40 },                 "EBCDIC" },
        { 5, { 0xc1, 0xc2, 0x40 },                 "EBCIDK" },
    };
    for (const Sample& sm : samples) {
        const int n = static_cast<int>(sm.bytes.size());
        const std::string mb = FormatStructCharArrayCp932(sm.charset, sm.bytes.data(), n);
        const std::wstring expect = WideFromCp932(mb.c_str(), static_cast<int>(mb.size()));
        CHECK(FormatStructCharArrayW(sm.charset, sm.bytes.data(), n) == expect, sm.what);
    }

    // --- charset 6 (UTF-8) ---
    {   // 日本語（CP932 にもある文字）
        const unsigned char utf8[] = { 0xE3, 0x81, 0x82, 'A' };   // "あA"
        CHECK(FormatStructCharArrayW(6, utf8, 4) == std::wstring(L"あA"),
              "UTF-8 decodes Japanese");
    }
    {   // CP932 に無い文字がそのまま残る（この Issue の主眼）
        const unsigned char hangul[] = { 0xED, 0x95, 0x9C };      // "한"
        CHECK(FormatStructCharArrayW(6, hangul, 3) == std::wstring(L"한"),
              "UTF-8 keeps characters outside CP932");
        // 参考: CP932 経由だと '.' に潰れていた
        const std::string mb = FormatStructCharArrayCp932(3, hangul, 3);
        CHECK(WideFromCp932(mb.c_str(), (int)mb.size()) != std::wstring(L"한"),
              "the CP932 route cannot represent it");
    }
    {   // 4 バイト文字はサロゲートペア 2 コード単位になる
        const unsigned char emoji[] = { 0xF0, 0x9F, 0x98, 0x80 };  // U+1F600
        const std::wstring w = FormatStructCharArrayW(6, emoji, 4);
        CHECK(w.size() == 2, "four byte sequence becomes a surrogate pair");
        CHECK(w[0] == 0xD83D && w[1] == 0xDE00, "surrogate pair values");
    }
    {   // 不正・不完全な列は 1 バイト = 1 文字の '.'
        const unsigned char broken[] = { 0xE3, 0x81, 'A' };        // 途中で壊れた 3 バイト列
        CHECK(FormatStructCharArrayW(6, broken, 3) == std::wstring(L"..A"),
              "broken sequence becomes one dot per byte");
        const unsigned char truncated[] = { 0xE3, 0x81 };          // 末尾で切れた列
        CHECK(FormatStructCharArrayW(6, truncated, 2) == std::wstring(L".."),
              "truncated sequence becomes dots");
        const unsigned char lone[] = { 0x80, 'A' };                // 単独の後続バイト
        CHECK(FormatStructCharArrayW(6, lone, 2) == std::wstring(L".A"),
              "lone continuation byte becomes a dot");
    }
    {   // 文字欄と違い、セル整列のための空白詰めはしない
        const unsigned char two[] = { 0xE3, 0x81, 0x82, 0xE3, 0x81, 0x84 };   // "あい"
        CHECK(FormatStructCharArrayW(6, two, 6) == std::wstring(L"あい"),
              "no padding for cell alignment");
    }
    {   // 256 バイト上限は UTF-8 でも同じ（3 バイト文字 85 個 + 1 バイト）
        std::vector<unsigned char> big;
        for (int i = 0; i < 100; ++i) { big.push_back(0xE3); big.push_back(0x81); big.push_back(0x82); }
        const std::wstring w = FormatStructCharArrayW(6, big.data(), static_cast<int>(big.size()));
        // 0x100 バイト = 85 文字(255 バイト) + 余り 1 バイトが不正扱いの '.'
        CHECK(w.size() == 86, "caps at 256 bytes");
        CHECK(w[85] == L'.', "the leftover byte becomes a dot");
    }

    // --- 引数の防御 ---
    {
        const unsigned char a[] = { 'A' };
        CHECK(FormatStructCharArrayW(6, nullptr, 4).empty(), "nullptr yields empty");
        CHECK(FormatStructCharArrayW(6, a, 0).empty(), "n=0 yields empty");
    }
}

// ---- struct.def パース（Issue #46: 配列要素数の検証）------------------------
static void TestStructDefParse() {
    std::printf("[TestStructDefParse]\n");
    using stirling::StructDefSet;

    // 正常: 1 次元・2 次元の配列要素数が読めること。
    {
        StructDefSet defs;
        std::wstring err;
        CHECK(defs.ParseText("struct S { byte a[4]; word b[2][3]; long c; };", &err),
              "valid def parses");
        CHECK(err.empty(), "valid def leaves err empty");
        CHECK(defs.Defs().size() == 1, "one struct parsed");
        if (defs.Defs().size() == 1) {
            const auto& f = defs.Defs()[0].fields;
            CHECK(f.size() == 3, "three fields");
            if (f.size() == 3) {
                CHECK(f[0].arrayCount == 4, "a[4] count");
                CHECK(f[1].arrayCount == 2 && f[1].arrayCount2 == 3, "b[2][3] counts");
                CHECK(f[2].arrayCount == 1 && f[2].arrayCount2 == 1, "scalar defaults to 1");
            }
        }
    }

    // 不正な要素数はパースエラー（原の atoi は 0 と変換失敗を区別できず黙って 1 要素にしていた）。
    const char* const kBad[] = {
        "struct S { byte a[abc]; };",              // 数値でない
        "struct S { byte a[10abc]; };",            // 末尾に余分な文字
        "struct S { byte a[0]; };",                // 0 要素
        "struct S { byte a[99999999999999]; };",   // long の範囲外
        "struct S { byte a[2147483647]; };",       // 上限（65536）超え: int 乗算がオーバーフローする
        "struct S { byte a[65537]; };",            // 上限のすぐ外
        "struct S { byte a[256][257]; };",         // 2 次元の積が上限超え
        "struct S { byte a[]; };",                 // 要素数なし
        "struct S { byte a[4][xyz]; };",           // 2 次元目が不正
    };
    for (const char* src : kBad) {
        StructDefSet defs;
        std::wstring err;
        CHECK(!defs.ParseText(src, &err), "invalid array count is a parse error");
        CHECK(!err.empty(), "invalid array count reports a message");
    }

    // 上限ちょうど（65536）と、その積が上限に収まる 2 次元配列は通ること。
    {
        StructDefSet defs;
        std::wstring err;
        CHECK(defs.ParseText("struct S { byte a[65536]; word b[256][256]; };", &err),
              "array counts at the cap are accepted");
        CHECK(err.empty(), "cap boundary leaves err empty");
    }

    // err を要求しない呼び出しでも落ちないこと。
    {
        StructDefSet defs;
        CHECK(!defs.ParseText("struct S { byte a[abc]; };", nullptr), "null err is allowed");
    }
}

// 読み出し検証のためにクリップボードを開く。他プロセスのロックは一時的なので短く待つ。
//   開けなかった場合は検証をスキップせず失敗として扱う（黙って通り抜けないため）。
static bool OpenClipboardForRead() {
    for (int i = 0; i < 20; ++i) {
        if (::OpenClipboard(nullptr)) { return true; }
        ::Sleep(50);
    }
    ++g_checks;
    ++g_failures;
    std::printf("  FAIL: could not open the clipboard for reading\n");
    return false;
}

// 16進テキストの寛容パーサ（Issue #97。クリップボードの16進テキスト貼り付け）。
//   受理形式・拒否条件と、失敗時に部分結果を返さないことを検証する。
static void TestHexTextParse() {
    std::printf("[TestHexTextParse]\n");
    using stirling::HexTextError;
    using stirling::ParseHexText;

    const std::vector<unsigned char> abc = {0x41, 0x42, 0x43};
    struct Accept { const wchar_t* text; const std::vector<unsigned char>* expect; const char* what; };
    const std::vector<unsigned char> ab = {0x41, 0x42};
    const std::vector<unsigned char> abcd = {0x41, 0x42, 0x43, 0x44};
    const Accept accepts[] = {
        { L"41 42 43",        &abc,  "space separated" },
        { L"414243",          &abc,  "no separator" },
        { L"41,42,43",        &abc,  "comma separated" },
        { L"41, 42, 43",      &abc,  "comma and space" },
        { L"0x41 0x42",       &ab,   "0x prefix" },
        { L"0X41 0X42",       &ab,   "0X prefix" },
        { L"\\x41 \\x42", &ab,   "backslash-x prefix" },
        { L"41 42\r\n43 44",  &abcd, "CRLF separated" },
        { L"\t41\t42\t",       &ab,   "tab separated" },
        { L"  41 42  ",       &ab,   "leading/trailing spaces" },
        { L"\r\n41 42\r\n",   &ab,   "leading/trailing CRLF is ignored" },
        { L" \t41 42\t ",   &ab,   "leading/trailing tabs are ignored" },
        { L",41 42,",         &ab,   "leading/trailing commas are ignored" },
        { L"4142 43",         &abc,  "mixed token widths" },
        { L"4a 4B",           nullptr, "mixed case" },
    };
    for (const Accept& a : accepts) {
        std::vector<unsigned char> out;
        const stirling::HexTextParseResult r = ParseHexText(a.text, std::wcslen(a.text), out);
        CHECK(r.Ok(), a.what);
        if (a.expect != nullptr) {
            CHECK(out == *a.expect, a.what);
        }
    }
    {   // 大小混在の値そのものも確認する
        std::vector<unsigned char> out;
        ParseHexText(L"4a 4B", 5, out);
        const std::vector<unsigned char> expect = {0x4A, 0x4B};
        CHECK(out == expect, "lower/upper case digits give the same value");
    }

    struct Reject { const wchar_t* text; HexTextError error; size_t pos; const char* what; };
    const Reject rejects[] = {
        { L"",                HexTextError::Empty,       0, "empty string" },
        { L"   ",             HexTextError::Empty,       0, "separators only" },
        { L"41 4",            HexTextError::OddDigits,   3, "odd digit token" },
        { L"41 4 43",         HexTextError::OddDigits,   3, "single digit token is not merged" },
        { L"414",             HexTextError::OddDigits,   0, "odd digit run" },
        { L"41 GG 43",        HexTextError::InvalidChar, 3, "non hex character" },
        { L"0000: 41 42  AB", HexTextError::InvalidChar, 4, "dump form is rejected at the colon" },
        { L"41 42\x3042", HexTextError::InvalidChar, 5, "non ASCII character" },
        { L"0x",              HexTextError::InvalidChar, 1, "prefix without digits" },
    };
    for (const Reject& r : rejects) {
        std::vector<unsigned char> out(4, 0xEE);   // 失敗時に空へ戻ることを見るため詰めておく
        const stirling::HexTextParseResult res = ParseHexText(r.text, std::wcslen(r.text), out);
        CHECK(!res.Ok(), r.what);
        CHECK(res.error == r.error, r.what);
        CHECK(res.errorPos == r.pos, r.what);
        CHECK(out.empty(), "failed parse leaves no partial result");
    }

    {   // nullptr と長さ 0 は空扱い
        std::vector<unsigned char> out;
        CHECK(ParseHexText(nullptr, 0, out).error == HexTextError::Empty, "null input is empty");
        CHECK(ParseHexText(L"41", 0, out).error == HexTextError::Empty, "zero length is empty");
    }

    {   // 長い入力（区切り無し）でも全バイトを取り出す
        std::wstring text;
        for (int i = 0; i < 4096; ++i) { text += L"7F"; }
        std::vector<unsigned char> out;
        CHECK(ParseHexText(text, out).Ok(), "long token parses");
        CHECK(out.size() == 4096, "long token yields every byte");
        CHECK(out.front() == 0x7F && out.back() == 0x7F, "long token values are correct");
    }
}

// UTF-8 の復号・符号化と持ち越し判定（Issue #98。キャラクターセット UTF-8 対応）。
//   文字欄の不変条件（1 ソースバイト = 1 表示セル）を保つための土台なので、
//   「不正な列は 1 バイトずつ独立して扱う」ことを重点的に確認する。
static void TestUtf8Text() {
    std::printf("[TestUtf8Text]\n");
    using stirling::DecodeUtf8;
    using stirling::EncodeUtf8;
    using stirling::Utf8CarryBytesAt;
    using stirling::Utf8FromWide;
    using stirling::Utf8SeqLen;

    // --- 列長 ---
    CHECK(Utf8SeqLen(0x41) == 1, "ASCII lead length");
    CHECK(Utf8SeqLen(0x80) == 0, "continuation byte is not a lead");
    CHECK(Utf8SeqLen(0xBF) == 0, "continuation byte is not a lead");
    CHECK(Utf8SeqLen(0xC0) == 0, "0xC0 is always overlong");
    CHECK(Utf8SeqLen(0xC1) == 0, "0xC1 is always overlong");
    CHECK(Utf8SeqLen(0xC2) == 2, "two byte lead");
    CHECK(Utf8SeqLen(0xE3) == 3, "three byte lead");
    CHECK(Utf8SeqLen(0xF0) == 4, "four byte lead");
    CHECK(Utf8SeqLen(0xF5) == 0, "0xF5 exceeds U+10FFFF");
    CHECK(Utf8SeqLen(0xFF) == 0, "0xFF is never valid");

    // --- 正常な復号 ---
    struct Good { const char* bytes; int len; unsigned int cp; const char* what; };
    const Good goods[] = {
        { "\x41",                 1, 0x41,    "ASCII A" },
        { "\xC3\xA9",             2, 0xE9,    "two byte e acute" },
        { "\xE3\x81\x82",         3, 0x3042,  "three byte HIRAGANA A" },
        { "\xED\x95\x9C",         3, 0xD55C,  "three byte HANGUL (outside CP932)" },
        { "\xF0\x9F\x98\x80",     4, 0x1F600, "four byte emoji" },
        { "\xC2\x80",             2, 0x80,    "smallest two byte" },
        { "\xE0\xA0\x80",         3, 0x800,   "smallest three byte" },
        { "\xF0\x90\x80\x80",     4, 0x10000, "smallest four byte" },
        { "\xF4\x8F\xBF\xBF",     4, 0x10FFFF,"largest code point" },
    };
    for (const Good& g : goods) {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(g.bytes);
        const stirling::Utf8Decoded d = DecodeUtf8(p, static_cast<size_t>(g.len));
        CHECK(d.ok, g.what);
        CHECK(d.codePoint == g.cp, g.what);
        CHECK(d.length == g.len, g.what);
    }

    // --- 不正な列は 1 バイトだけ消費する ---
    struct Bad { const char* bytes; size_t n; const char* what; };
    const Bad bads[] = {
        { "\x80\x41",         2, "lone continuation byte" },
        { "\xC0\xAF",         2, "overlong two byte" },
        { "\xC2\x41",         2, "missing continuation" },
        { "\xE0\x80\xAF",     3, "overlong three byte" },
        { "\xE3\x81\x41",     3, "broken three byte" },
        { "\xED\xA0\x80",     3, "UTF-16 surrogate is not valid UTF-8" },
        { "\xF5\x80\x80\x80", 4, "beyond U+10FFFF" },
        { "\xF0\x80\x80\x80", 4, "overlong four byte" },
    };
    for (const Bad& b : bads) {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(b.bytes);
        const stirling::Utf8Decoded d = DecodeUtf8(p, b.n);
        CHECK(!d.ok, b.what);
        CHECK(d.length == 1, b.what);
        CHECK(!d.truncated, b.what);
    }

    {   // バッファ端で列が途切れた場合は truncated（呼び出し側が次の窓で読み直す）
        const unsigned char p[] = {0xE3, 0x81};
        const stirling::Utf8Decoded d = DecodeUtf8(p, sizeof(p));
        CHECK(!d.ok && d.truncated, "truncated sequence is reported");
        CHECK(d.length == 1, "truncated sequence consumes one byte");
    }
    {   // 空・null
        CHECK(!DecodeUtf8(nullptr, 0).ok, "null input is not decodable");
        const unsigned char p[] = {0x41};
        CHECK(!DecodeUtf8(p, 0).ok, "zero length is not decodable");
    }

    // --- 符号化（復号との往復） ---
    for (const Good& g : goods) {
        std::vector<unsigned char> out;
        CHECK(EncodeUtf8(g.cp, out), g.what);
        CHECK(out.size() == static_cast<size_t>(g.len), g.what);
        CHECK(std::memcmp(out.data(), g.bytes, out.size()) == 0, g.what);
    }
    {
        std::vector<unsigned char> out;
        CHECK(!EncodeUtf8(0xD800, out), "surrogate is not encodable");
        CHECK(!EncodeUtf8(0x110000, out), "beyond U+10FFFF is not encodable");
        CHECK(out.empty(), "rejected code points write nothing");
    }

    // --- ワイド文字列 -> UTF-8（サロゲートペアの結合） ---
    {
        const wchar_t w[] = {0x41, 0x3042, 0xD55C, 0};
        const std::vector<unsigned char> b = Utf8FromWide(w, 3);
        const unsigned char expect[] = {0x41, 0xE3, 0x81, 0x82, 0xED, 0x95, 0x9C};
        CHECK(b.size() == sizeof(expect), "wide to utf8 length");
        CHECK(std::memcmp(b.data(), expect, b.size()) == 0, "wide to utf8 bytes");
    }
    {   // U+1F600 のサロゲートペアは 1 コードポイントへ結合する
        const wchar_t w[] = {0xD83D, 0xDE00, 0};
        const std::vector<unsigned char> b = Utf8FromWide(w, 2);
        const unsigned char expect[] = {0xF0, 0x9F, 0x98, 0x80};
        CHECK(b.size() == 4, "surrogate pair becomes one code point");
        CHECK(std::memcmp(b.data(), expect, b.size()) == 0, "surrogate pair bytes");
    }
    {   // 対になっていないサロゲートは捨てる（不正な列を作らない）
        const wchar_t w[] = {0x41, 0xD83D, 0x42, 0};
        const std::vector<unsigned char> b = Utf8FromWide(w, 3);
        const unsigned char expect[] = {0x41, 0x42};
        CHECK(b.size() == 2, "unpaired surrogate is dropped");
        CHECK(std::memcmp(b.data(), expect, b.size()) == 0, "unpaired surrogate bytes");
        CHECK(Utf8FromWide(nullptr, 0).empty(), "null wide input is empty");
    }

    // --- 窓の先頭が文字の途中のときの読み飛ばしバイト数 ---
    {
        // "A" + HIRAGANA A(3 bytes) + "B" = 41 E3 81 82 42
        const unsigned char buf[] = {0x41, 0xE3, 0x81, 0x82, 0x42};
        CHECK(Utf8CarryBytesAt(buf, sizeof(buf), 0) == 0, "start of data has no carry");
        CHECK(Utf8CarryBytesAt(buf, sizeof(buf), 1) == 0, "lead byte has no carry");
        CHECK(Utf8CarryBytesAt(buf, sizeof(buf), 2) == 2, "second byte carries two bytes");
        CHECK(Utf8CarryBytesAt(buf, sizeof(buf), 3) == 1, "third byte carries one byte");
        CHECK(Utf8CarryBytesAt(buf, sizeof(buf), 4) == 0, "next lead byte has no carry");
    }
    {
        // 4 バイト列 F0 9F 98 80 の 2..4 バイト目
        const unsigned char buf[] = {0xF0, 0x9F, 0x98, 0x80, 0x41};
        CHECK(Utf8CarryBytesAt(buf, sizeof(buf), 1) == 3, "four byte sequence: second byte");
        CHECK(Utf8CarryBytesAt(buf, sizeof(buf), 2) == 2, "four byte sequence: third byte");
        CHECK(Utf8CarryBytesAt(buf, sizeof(buf), 3) == 1, "four byte sequence: fourth byte");
    }
    {
        // 不正な列の途中は持ち越さない（各バイトが独立した 1 セルになる）
        const unsigned char buf[] = {0xE3, 0x41, 0x80, 0x42};
        CHECK(Utf8CarryBytesAt(buf, sizeof(buf), 2) == 0, "broken sequence does not carry");
        const unsigned char lone[] = {0x41, 0x80, 0x42};
        CHECK(Utf8CarryBytesAt(lone, sizeof(lone), 1) == 0, "lone continuation does not carry");
    }
    {
        // 後続バイトが 4 個以上続く場合、3 バイトより手前は探さない
        const unsigned char buf[] = {0x80, 0x80, 0x80, 0x80, 0x80};
        CHECK(Utf8CarryBytesAt(buf, sizeof(buf), 4) == 0, "no lead byte within three bytes");
    }
}

// クリップボード転送の RAII（Issue #47）。
//   グローバルメモリの所有権移譲・解放と、テキスト転送の往復を検証する。
//   実クリップボードを使うため、テスト実行でクリップボードの内容は置き換わる。
static void TestClipboardUtil() {
    std::printf("[TestClipboardUtil]\n");
    using ui::GlobalLockGuard;
    using ui::GlobalMemory;

    // 既定構築は無効。確保に成功したものは有効。
    {
        GlobalMemory empty;
        CHECK(!empty.IsValid() && empty.Get() == nullptr, "default GlobalMemory is invalid");
        GlobalMemory mem(64);
        CHECK(mem.IsValid(), "GlobalAlloc succeeds for a small block");
        CHECK(::GlobalSize(mem.Get()) >= 64, "allocated size is at least the request");
    }

    // ムーブで所有権が移り、元は無効になる。
    {
        GlobalMemory a(32);
        HGLOBAL raw = a.Get();
        GlobalMemory b(std::move(a));
        CHECK(!a.IsValid(), "moved-from GlobalMemory is invalid");
        CHECK(b.Get() == raw, "move transfers the handle");
        GlobalMemory c;
        c = std::move(b);
        CHECK(!b.IsValid() && c.Get() == raw, "move assignment transfers the handle");
    }

    // Release は所有権を手放す（デストラクタは解放しない）。手動で解放できること。
    {
        HGLOBAL raw = nullptr;
        {
            GlobalMemory mem(32);
            raw = mem.Release();
            CHECK(!mem.IsValid(), "Release clears ownership");
        }
        CHECK(raw != nullptr && ::GlobalSize(raw) >= 32, "released handle is still alive");
        CHECK(::GlobalFree(raw) == nullptr, "released handle can be freed by the caller");
    }

    // スコープを抜けたメモリは解放される（GlobalFlags が無効ハンドルを報告する）。
    {
        HGLOBAL raw = nullptr;
        {
            GlobalMemory mem(32);
            raw = mem.Get();
        }
        CHECK((::GlobalFlags(raw) & GMEM_INVALID_HANDLE) != 0, "scope exit frees the memory");
    }

    // ロックガードはスコープを抜けるとロックを解除する（ロック数が 0 に戻る）。
    {
        GlobalMemory mem(16);
        CHECK(mem.IsValid(), "alloc for lock guard");
        {
            GlobalLockGuard lock(mem.Get());
            CHECK(lock.IsLocked() && lock.Get() != nullptr, "GlobalLock succeeds");
            CHECK((::GlobalFlags(mem.Get()) & GMEM_LOCKCOUNT) == 1, "lock count is 1");
        }
        CHECK((::GlobalFlags(mem.Get()) & GMEM_LOCKCOUNT) == 0, "lock is released on scope exit");
        GlobalLockGuard invalid(nullptr);
        CHECK(!invalid.IsLocked() && invalid.Get() == nullptr, "locking a null handle fails safely");
    }

    // 引数不正はエラーコードを返し、クリップボードには触れない。
    {
        DWORD error = ERROR_SUCCESS;
        CHECK(!ui::PutClipboardTextW(nullptr, nullptr, 0, error), "null text is rejected");
        CHECK(error == ERROR_INVALID_PARAMETER, "null text reports ERROR_INVALID_PARAMETER");
        error = ERROR_SUCCESS;
        CHECK(!ui::PutClipboardTextA(nullptr, nullptr, 4, error), "null bytes are rejected");
        CHECK(error == ERROR_INVALID_PARAMETER, "null bytes report ERROR_INVALID_PARAMETER");
        error = ERROR_SUCCESS;
        // 長さがバイト数計算を溢れさせる場合は確保に進まずエラーになる（SIZE_MAX のラップ含む）。
        CHECK(!ui::PutClipboardTextW(nullptr, L"x", SIZE_MAX, error), "SIZE_MAX length is rejected");
        CHECK(error == ERROR_ARITHMETIC_OVERFLOW, "overflowing length reports the overflow");
        error = ERROR_SUCCESS;
        CHECK(!ui::PutClipboardTextW(nullptr, L"x", SIZE_MAX / sizeof(wchar_t), error),
              "length at the overflow boundary is rejected");
        error = ERROR_SUCCESS;
        GlobalMemory none;
        CHECK(!ui::PutClipboardOwned(nullptr, CF_TEXT, none, error), "invalid memory is rejected");
        CHECK(error == ERROR_NOT_ENOUGH_MEMORY, "invalid memory reports out-of-memory");
    }

    // SetClipboardData が失敗したときは所有権を手放さない（＝呼び出し元が解放する）。
    //   書式 0 は不正な書式番号なので、クリップボードを開いた状態でも設定に失敗する。
    {
        DWORD error = ERROR_SUCCESS;
        GlobalMemory mem(16);
        CHECK(mem.IsValid(), "alloc for the failing transfer");
        const HGLOBAL raw = mem.Get();
        CHECK(!ui::PutClipboardOwned(nullptr, 0, mem, error), "an invalid format fails");
        CHECK(error != ERROR_SUCCESS, "a failed transfer reports a reason");
        CHECK(mem.IsValid() && mem.Get() == raw, "a failed transfer keeps ownership");
    }   // ここで解放される（クリップボードへは渡っていない）

    // ワイド文字列の往復（CF_UNICODETEXT）。終端が付いていること。
    {
        const wchar_t kText[] = L"C:\\dir\\file.bin\t0x00001234\r\n";
        const size_t len = wcslen(kText);
        DWORD error = ERROR_SUCCESS;
        const bool ok = ui::PutClipboardTextW(nullptr, kText, len, error);
        CHECK(ok, "PutClipboardTextW succeeds");
        CHECK(!ok || error == ERROR_SUCCESS, "no error is reported on success");
        if (ok && OpenClipboardForRead()) {
            // 明示設定した書式は列挙の先頭に来る（後続は OS が合成したもの）。
            CHECK(::EnumClipboardFormats(0) == CF_UNICODETEXT,
                  "CF_UNICODETEXT is the format we set, not a synthesized one");
            HANDLE h = ::GetClipboardData(CF_UNICODETEXT);
            CHECK(h != nullptr, "CF_UNICODETEXT is available");
            if (h != nullptr) {
                GlobalLockGuard lock(h);   // 読み取り用。所有権はクリップボードのまま
                const wchar_t* p = static_cast<const wchar_t*>(lock.Get());
                CHECK(p != nullptr && wcscmp(p, kText) == 0, "wide text round-trips");
            }
            ::CloseClipboard();
        }
    }

    // 生バイト列の往復（CF_TEXT）。不正な多バイト列がそのまま渡ること（byte 層）。
    {
        const char kBytes[] = "\x82\xA0\x82\x3F\xE0\x41 raw";   // 壊れた 2 バイト文字を含む
        const size_t len = sizeof(kBytes) - 1;
        DWORD error = ERROR_SUCCESS;
        const bool ok = ui::PutClipboardTextA(nullptr, kBytes, len, error);
        CHECK(ok, "PutClipboardTextA succeeds");
        if (ok && OpenClipboardForRead()) {
            CHECK(::EnumClipboardFormats(0) == CF_TEXT,
                  "CF_TEXT is the format we set, not a synthesized one");
            HANDLE h = ::GetClipboardData(CF_TEXT);
            CHECK(h != nullptr, "CF_TEXT is available");
            if (h != nullptr) {
                GlobalLockGuard lock(h);
                const char* p = static_cast<const char*>(lock.Get());
                CHECK(p != nullptr && std::memcmp(p, kBytes, len) == 0,
                      "raw bytes round-trip unchanged");
                CHECK(p != nullptr && p[len] == '\0', "the copy is NUL terminated");
            }
            ::CloseClipboard();
        }
    }

    // 空文字列でも転送でき、終端だけが入ること。
    {
        DWORD error = ERROR_SUCCESS;
        const bool ok = ui::PutClipboardTextW(nullptr, L"", 0, error);
        CHECK(ok, "empty text is transferred");
        if (ok && OpenClipboardForRead()) {
            HANDLE h = ::GetClipboardData(CF_UNICODETEXT);
            CHECK(h != nullptr, "CF_UNICODETEXT is available for empty text");
            if (h != nullptr) {
                GlobalLockGuard lock(h);
                const wchar_t* p = static_cast<const wchar_t*>(lock.Get());
                CHECK(p != nullptr && p[0] == L'\0', "empty text is just a terminator");
            }
            ::CloseClipboard();
        }
    }
}

// Undo 履歴の容量管理（Issue #30）: PlanUndoTrim / ShiftSavePoint。
static void TestUndoBudget() {
    std::printf("[TestUndoBudget]\n");
    using stirling::PlanUndoTrim;
    using stirling::ShiftSavePoint;
    using stirling::UndoTrimPlan;

    // 上限内なら何も破棄しない。
    {
        const std::vector<unsigned long long> undo = {10, 20, 30};
        const UndoTrimPlan p = PlanUndoTrim(undo, {}, 100);
        CHECK(p.dropUndoFront == 0, "under limit: no drop");
        CHECK(p.dropRedoFront == 0, "under limit: no redo drop");
        CHECK(p.remainingBytes == 60, "under limit: total kept");
    }
    // limit==0 は無制限。
    {
        const std::vector<unsigned long long> undo = {1000, 2000};
        const UndoTrimPlan p = PlanUndoTrim(undo, {}, 0);
        CHECK(p.dropUndoFront == 0, "unlimited: no drop");
        CHECK(p.remainingBytes == 3000, "unlimited: total kept");
    }
    // 超過分だけ最古（先頭）から破棄する。
    {
        const std::vector<unsigned long long> undo = {50, 50, 50, 50};
        const UndoTrimPlan p = PlanUndoTrim(undo, {}, 100);
        CHECK(p.dropUndoFront == 2, "drop oldest until fit");
        CHECK(p.remainingBytes == 100, "remaining fits limit");
    }
    // Undo を最後の1件まで削っても収まらなければ Redo 先頭を破棄する。
    {
        const std::vector<unsigned long long> undo = {10, 10};
        const std::vector<unsigned long long> redo = {40, 40, 40};
        const UndoTrimPlan p = PlanUndoTrim(undo, redo, 50);
        CHECK(p.dropUndoFront == 1, "undo trimmed to last one");
        CHECK(p.dropRedoFront == 2, "redo trimmed from front");
        CHECK(p.remainingBytes == 50, "remaining fits limit");
    }
    // 各スタックの最後の1件は上限を超えても残す（直近の取り消しは常に可能）。
    {
        const std::vector<unsigned long long> undo = {1000};
        const std::vector<unsigned long long> redo = {1000};
        const UndoTrimPlan p = PlanUndoTrim(undo, redo, 10);
        CHECK(p.dropUndoFront == 0, "keep last undo record");
        CHECK(p.dropRedoFront == 0, "keep last redo record");
        CHECK(p.remainingBytes == 2000, "limit is advisory for last records");
    }
    // 保存点の付け替え。
    CHECK(ShiftSavePoint(5, 2) == 3, "save point shifts by dropped count");
    CHECK(ShiftSavePoint(2, 2) == 0, "save point at boundary stays reachable");
    CHECK(ShiftSavePoint(1, 2) == -1, "dropped save point becomes unreachable");
    CHECK(ShiftSavePoint(-1, 2) == -1, "already unreachable stays unreachable");
    CHECK(ShiftSavePoint(3, 0) == 3, "no drop keeps save point");
}

// ---- 範囲一括削除（Issue #62）----

// 各ノードの usedLen 列（ブロック構造のスナップショット）。
static std::vector<int> BlockShape(BlockList& list) {
    std::vector<int> shape;
    for (BlockNode* n = list.GetHead(); n != nullptr; n = list.GetNext(n)) {
        shape.push_back(n->usedLen);
    }
    return shape;
}

// 新規ドキュメント相当の空ブロックへ data を一括挿入する（両系で同じ構造を作る）。
static void FillDoc(BlockList& list, const std::vector<unsigned char>& data) {
    NewEmptyDoc(list);
    if (!data.empty()) {
        BlockCursor c(&list);
        CHECK(c.Insert(0, data.data(), static_cast<FileOffset>(data.size())), "FillDoc insert");
    }
}

// DeleteRange と「DeleteByte の反復」を同一データへ適用し、削除数・内容・ブロック構造の
// すべてが一致することを確かめる（忠実性の担保）。線形参照モデルとも突き合わせる。
static void CheckDeleteRangeEquivalence(const std::vector<unsigned char>& src,
                                        FileOffset pos, FileOffset count, const char* where) {
    BlockList bulk;
    BlockList byByte;
    FillDoc(bulk, src);
    FillDoc(byByte, src);

    FileOffset got = 0;
    {
        BlockCursor c(&bulk);
        got = c.DeleteRange(pos, count);
    }
    FileOffset expect = 0;
    {
        BlockCursor c(&byByte);
        for (FileOffset i = 0; i < count; ++i) {
            unsigned char t = 0;
            if (!c.DeleteByte(pos, &t)) { break; }
            ++expect;
        }
    }
    CHECK(got == expect, where);                       // 削除できたバイト数
    CHECK(BlockShape(bulk) == BlockShape(byByte), where);   // ブロック構造
    CHECK(ReadAll(bulk) == ReadAll(byByte), where);         // 内容

    // 線形参照モデル（std::vector）との突合
    std::vector<unsigned char> ref = src;
    if (expect > 0) {
        ref.erase(ref.begin() + static_cast<size_t>(pos),
                  ref.begin() + static_cast<size_t>(pos + expect));
    }
    CheckEqual(bulk, ref, where);
    CheckInvariants(bulk, ref.size(), where);
}

static void TestDeleteRange() {
    std::printf("[TestDeleteRange]\n");
    // 3ブロック強（16KB×2 を跨ぐ長さ）のデータを用意する。
    std::vector<unsigned char> src(40000);
    for (size_t i = 0; i < src.size(); ++i) {
        src[i] = static_cast<unsigned char>((i * 31 + 7) & 0xFF);
    }
    const FileOffset total = static_cast<FileOffset>(src.size());

    CheckDeleteRangeEquivalence(src, 0, 1, "delete 1 byte at front");
    CheckDeleteRangeEquivalence(src, 100, 200, "delete inside one block");
    CheckDeleteRangeEquivalence(src, 0, kBlockCapacity, "delete exactly one whole block");
    CheckDeleteRangeEquivalence(src, 10, kBlockCapacity, "delete across two blocks");
    CheckDeleteRangeEquivalence(src, kBlockCapacity - 5, 10, "delete over block boundary");
    CheckDeleteRangeEquivalence(src, total - 1, 1, "delete last byte");
    CheckDeleteRangeEquivalence(src, 0, total, "delete all");
    CheckDeleteRangeEquivalence(src, 5, total, "delete to EOF (count over)");
    CheckDeleteRangeEquivalence(src, total, 10, "delete at EOF (no-op)");

    // 単一ノード（空ブロックが残る経路）
    std::vector<unsigned char> one(1, 'A');
    CheckDeleteRangeEquivalence(one, 0, 1, "delete only byte of only block");

    // 中間ノードだけがちょうど消えるケース（3ブロックの2番目を丸ごと削除）
    CheckDeleteRangeEquivalence(src, kBlockCapacity, kBlockCapacity, "delete whole middle block");

    // 空ドキュメント（usedLen==0 の1ノードのみ）に対しては何も削除しない
    {
        BlockList list;
        NewEmptyDoc(list);
        BlockCursor c(&list);
        CHECK(c.DeleteRange(0, 10) == 0, "empty doc deletes nothing");
        CheckInvariants(list, 0, "empty doc unchanged");
        CHECK(list.Count() == 1, "empty doc keeps its single block");
    }

    // 全削除の直後、同じカーソルで挿入して再利用できる
    {
        BlockList list;
        FillDoc(list, src);
        BlockCursor c(&list);
        CHECK(c.DeleteRange(0, total) == total, "delete all before reinsert");
        CheckInvariants(list, 0, "empty after delete all");
        CHECK(c.Insert(0, "XYZ", 3), "reinsert with same cursor after delete all");
        std::vector<unsigned char> ref = {'X', 'Y', 'Z'};
        CheckEqual(list, ref, "content after reinsert");
        CheckInvariants(list, ref.size(), "structure after reinsert");
    }

    // 引数の縮退: count<=0 / 不正位置ではリストを変更しない
    {
        BlockList list;
        FillDoc(list, src);
        BlockCursor c(&list);
        CHECK(c.DeleteRange(0, 0) == 0, "count==0 deletes nothing");
        CHECK(c.DeleteRange(0, -5) == 0, "negative count deletes nothing");
        CHECK(c.DeleteRange(-1, 10) == 0, "negative pos deletes nothing");
        CHECK(c.DeleteRange(total + 1, 10) == 0, "pos beyond EOF deletes nothing");
        CheckEqual(list, src, "degenerate args keep data");
        CheckInvariants(list, src.size(), "degenerate args keep structure");
    }

    // 削除後もカーソルが使えること（削除位置から読み出せる）
    {
        BlockList list;
        FillDoc(list, src);
        BlockCursor c(&list);
        const FileOffset n = c.DeleteRange(1000, 5000);
        CHECK(n == 5000, "cursor reuse: deleted count");
        unsigned char buf[4] = {0, 0, 0, 0};
        CHECK(c.Read(4, buf) == 4, "cursor reuse: read after delete");
        for (int i = 0; i < 4; ++i) {
            CHECK(buf[i] == src[static_cast<size_t>(6000 + i)], "cursor reuse: read content");
        }
    }

    // 途中から始まりブロック境界ちょうどで終わる削除
    CheckDeleteRangeEquivalence(src, kBlockCapacity - 100, 100, "delete up to block boundary");

    // count が int の範囲を超えても総長へ丸めて削れる（64bit 経路）
    {
        BlockList list;
        FillDoc(list, src);
        BlockCursor c(&list);
        const FileOffset huge = static_cast<FileOffset>(0x7FFFFFFF) + 1000;
        CHECK(c.DeleteRange(0, huge) == total, "count over INT_MAX clamps to total");
        CheckInvariants(list, 0, "structure after huge count delete");
    }

    // 削除後、同じカーソルの GetByteAt / SearchPattern が正しく動く（curAbs_ キャッシュ）
    {
        BlockList list;
        FillDoc(list, src);
        BlockCursor c(&list);
        CHECK(c.DeleteRange(100, 20000) == 20000, "delete before cache check");
        std::vector<unsigned char> ref = src;
        ref.erase(ref.begin() + 100, ref.begin() + 20100);
        unsigned char got = 0;
        CHECK(c.GetByteAt(100, &got) && got == ref[100], "GetByteAt right after delete");
        CHECK(c.GetByteAt(0, &got) && got == ref[0], "GetByteAt backward after delete");
        CHECK(c.GetByteAt(15000, &got) && got == ref[15000], "GetByteAt forward after delete");
        const unsigned char pat[3] = {ref[9000], ref[9001], ref[9002]};
        FileOffset found = -1;
        CHECK(c.SearchPattern(pat, 3, &found, BlockCursor::kForward, 0, 0),
              "SearchPattern after delete");
        CHECK(found >= 0 && ref[static_cast<size_t>(found)] == pat[0],
              "SearchPattern hit after delete");
    }

    // 契約外の構造（非終端の空ノード）でも Read と同じく読み飛ばす。
    //   空ノードはリスト唯一のときだけ生じるため通常は作られないが、挙動を固定しておく。
    {
        BlockList list;
        for (int i = 0; i < 3; ++i) {
            unsigned char* buf = new unsigned char[kBlockCapacity];
            buf[0] = static_cast<unsigned char>('A' + i);
            list.AppendBlock(buf, kBlockCapacity, (i == 1) ? 0 : 1);   // 中央だけ空
        }
        BlockCursor c(&list);
        CHECK(c.DeleteRange(0, 2) == 2, "delete across an empty middle node");
        CheckInvariants(list, 0, "empty middle node: all deleted");
    }

    // ランダム位置・長さで反復比較（ブロック跨ぎ・部分残りを網羅）
    {
        std::mt19937 rng(0x62D1);
        for (int t = 0; t < 40; ++t) {
            const FileOffset pos = static_cast<FileOffset>(rng() % (src.size() + 1));
            const FileOffset count = static_cast<FileOffset>(rng() % 9000) + 1;
            CheckDeleteRangeEquivalence(src, pos, count, "random delete range");
        }
    }
}

// ---- メモリ確保失敗時のロールバック（Issue #153） ----
// SetAllocFailCountdown(n) は「n 回目のブロック／ノード確保」を失敗させる注入フック。
// Win32 ではメモリ不足が現実的に起こるため、失敗が戻り値で返り、かつリストが
// 操作前の内容・ブロック構造のまま保たれることを、失敗位置を変えながら確認する。
#ifdef STIRLING_TEST_ALLOC_HOOK
static void TestAllocFailureRollback() {
    std::printf("TestAllocFailureRollback\n");

    // 1) 複数ブロックへ跨る Insert の途中で確保が失敗する。
    for (int failAt = 1; failAt <= 8; ++failAt) {
        BlockList list;
        NewEmptyDoc(list);
        std::vector<unsigned char> seed(1000);
        for (size_t i = 0; i < seed.size(); ++i) { seed[i] = static_cast<unsigned char>(i); }
        {
            BlockCursor c(&list);
            CHECK(c.Insert(0, seed.data(), static_cast<FileOffset>(seed.size())), "seed insert");
        }
        const std::vector<unsigned char> before = ReadAll(list);
        const int nodesBefore = list.Count();

        // ブロック途中への挿入（分割＋複数ブロック追加）＝確保回数が最も多い経路。
        const std::vector<unsigned char> big(static_cast<size_t>(kBlockCapacity) * 3 + 7, 0xAB);
        stirling::SetAllocFailCountdown(failAt);
        bool ok = false;
        {
            BlockCursor c(&list);
            ok = c.Insert(500, big.data(), static_cast<FileOffset>(big.size()));
        }
        stirling::SetAllocFailCountdown(0);
        CHECK(!ok, "Insert reports failure when a block allocation fails");
        CHECK(list.Count() == nodesBefore, "failed Insert leaves the block count unchanged");
        CHECK(list.GetTotalLength() == static_cast<FileOffset>(before.size()),
              "failed Insert leaves the total length unchanged");
        CHECK(ReadAll(list) == before, "failed Insert leaves the content unchanged");

        // 注入解除後は同じ挿入が成功する（内部状態が壊れていない）。
        {
            BlockCursor c(&list);
            CHECK(c.Insert(500, big.data(), static_cast<FileOffset>(big.size())),
                  "Insert succeeds once allocation recovers");
        }
        std::vector<unsigned char> expect = before;
        expect.insert(expect.begin() + 500, big.begin(), big.end());
        CHECK(ReadAll(list) == expect, "content after the retried insert");
    }

    // 2) 満杯ブロックの分割を伴う InsertByte。データ確保・ノード確保の両方を失敗させる。
    {
        BlockList list;
        NewEmptyDoc(list);
        const std::vector<unsigned char> full(static_cast<size_t>(kBlockCapacity), 0x11);
        {
            BlockCursor c(&list);
            CHECK(c.Insert(0, full.data(), kBlockCapacity), "fill a whole block");
        }
        const std::vector<unsigned char> before = ReadAll(list);
        for (int failAt = 1; failAt <= 2; ++failAt) {
            stirling::SetAllocFailCountdown(failAt);
            bool ok = false;
            {
                BlockCursor c(&list);
                ok = c.InsertByte(100, 0x99);
            }
            stirling::SetAllocFailCountdown(0);
            CHECK(!ok, "InsertByte reports failure when the split allocation fails");
            CHECK(list.Count() == 1, "failed InsertByte adds no node");
            CHECK(ReadAll(list) == before, "failed InsertByte leaves the content unchanged");
        }
        {
            BlockCursor c(&list);
            CHECK(c.InsertByte(100, 0x99), "InsertByte succeeds once allocation recovers");
        }
        std::vector<unsigned char> expect = before;
        expect.insert(expect.begin() + 100, 0x99);
        CHECK(ReadAll(list) == expect, "content after the retried InsertByte");
    }

    // 3) ファイル読込。ノード確保／データ確保のどちらが失敗しても kOutOfMemory を返し、
    //    ブロックを残さない（例外を UI 境界へ伝播させない）。
    {
        using stirling::FileIoResult;
        using stirling::FileIoStatus;
        const fs::path in = TempFile("oom");
        std::vector<unsigned char> data(static_cast<size_t>(kBlockCapacity) * 3 + 5);
        for (size_t i = 0; i < data.size(); ++i) { data[i] = static_cast<unsigned char>(i * 7); }
        WriteFile(in, data);
        for (int failAt = 1; failAt <= 6; ++failAt) {
            BlockList list;
            stirling::SetAllocFailCountdown(failAt);
            const FileIoResult r = stirling::LoadFileIntoBlocks(list, in.wstring().c_str());
            stirling::SetAllocFailCountdown(0);
            CHECK(!r.Ok(), "load fails when a block allocation fails");
            CHECK(r.status == FileIoStatus::kOutOfMemory, "load reports kOutOfMemory");
            CHECK(list.IsEmpty(), "failed load leaves no blocks behind");
        }
        {
            BlockList list;
            CHECK(stirling::LoadFileIntoBlocks(list, in.wstring().c_str()).Ok(),
                  "load succeeds once allocation recovers");
            CHECK(ReadAll(list) == data, "loaded content after recovery");
        }
        fs::remove(in);

        // 空ファイルの「空ブロック 1 個」確保も同じ扱い。
        const fs::path empty = TempFile("oom_empty");
        WriteFile(empty, std::vector<unsigned char>());
        for (int failAt = 1; failAt <= 2; ++failAt) {
            BlockList list;
            stirling::SetAllocFailCountdown(failAt);
            const FileIoResult r = stirling::LoadFileIntoBlocks(list, empty.wstring().c_str());
            stirling::SetAllocFailCountdown(0);
            CHECK(!r.Ok(), "empty-file load fails when allocation fails");
            CHECK(r.status == FileIoStatus::kOutOfMemory, "empty-file load reports kOutOfMemory");
            CHECK(list.IsEmpty(), "failed empty-file load leaves no blocks behind");
        }
        fs::remove(empty);
    }

    // 4) 所有権規約: AppendBlock / InsertNode* が失敗したとき data の所有権は移らない。
    {
        BlockList list;
        unsigned char* buf = stirling::AllocBlockData();
        CHECK(buf != nullptr, "AllocBlockData for the ownership check");
        stirling::SetAllocFailCountdown(1);
        BlockNode* n = list.AppendBlock(buf, kBlockCapacity, 4);
        stirling::SetAllocFailCountdown(0);
        CHECK(n == nullptr, "AppendBlock returns nullptr when the node allocation fails");
        CHECK(list.IsEmpty(), "failed AppendBlock links nothing");
        delete[] buf;   // 所有権は呼出側に残る（二重解放にならないことを確認する）

        BlockNode* head = NewEmptyDoc(list);
        CHECK(head != nullptr, "seed node for the ownership check");
        unsigned char* buf2 = stirling::AllocBlockData();
        stirling::SetAllocFailCountdown(1);
        BlockNode* after = list.InsertNodeAfter(head, buf2, kBlockCapacity, 4);
        stirling::SetAllocFailCountdown(0);
        CHECK(after == nullptr, "InsertNodeAfter returns nullptr when the node allocation fails");
        CHECK(list.Count() == 1, "failed InsertNodeAfter links nothing");
        stirling::SetAllocFailCountdown(1);
        BlockNode* bef = list.InsertNodeBefore(head, buf2, kBlockCapacity, 4);
        stirling::SetAllocFailCountdown(0);
        CHECK(bef == nullptr, "InsertNodeBefore returns nullptr when the node allocation fails");
        CHECK(list.Count() == 1, "failed InsertNodeBefore links nothing");
        delete[] buf2;
    }
}
#endif  // STIRLING_TEST_ALLOC_HOOK

// ---- 一括上書き / 範囲初期化（Issue #154） ----
// 範囲初期化を「選択長と同容量の一時バッファ＋置換」から「ブロックへの直接 memset」へ
// 変えたため、ブロック跨ぎ・境界・末尾クランプが SetByteAt の反復と一致することを確認する。
static void TestWriteAndFillRange() {
    std::printf("TestWriteAndFillRange\n");

    // 複数ブロックに跨るデータを用意する（16KB ブロック 3 個 + 端数）。
    std::vector<unsigned char> ref(static_cast<size_t>(kBlockCapacity) * 3 + 1234);
    for (size_t i = 0; i < ref.size(); ++i) { ref[i] = static_cast<unsigned char>(i * 31 + 7); }
    BlockList list;
    NewEmptyDoc(list);
    {
        BlockCursor c(&list);
        CHECK(c.Insert(0, ref.data(), static_cast<FileOffset>(ref.size())), "seed for write/fill");
    }
    CHECK(ReadAll(list) == ref, "seed content");

    // Write: ブロック境界を跨ぐ上書き。
    {
        const FileOffset pos = kBlockCapacity - 100;
        std::vector<unsigned char> src(500);
        for (size_t i = 0; i < src.size(); ++i) { src[i] = static_cast<unsigned char>(0xC0 + i); }
        BlockCursor c(&list);
        const FileOffset n = c.Write(pos, src.data(), static_cast<FileOffset>(src.size()));
        CHECK(n == static_cast<FileOffset>(src.size()), "Write returns the written length");
        std::copy(src.begin(), src.end(), ref.begin() + static_cast<size_t>(pos));
        CHECK(ReadAll(list) == ref, "Write across a block boundary");
        CHECK(list.GetTotalLength() == static_cast<FileOffset>(ref.size()),
              "Write keeps the total length");
    }

    // Write: データ末尾を越える分は書かずに打ち切る。
    {
        const FileOffset pos = static_cast<FileOffset>(ref.size()) - 10;
        std::vector<unsigned char> src(100, 0x5A);
        BlockCursor c(&list);
        const FileOffset n = c.Write(pos, src.data(), static_cast<FileOffset>(src.size()));
        CHECK(n == 10, "Write clamps at the end of data");
        std::fill(ref.end() - 10, ref.end(), static_cast<unsigned char>(0x5A));
        CHECK(ReadAll(list) == ref, "clamped Write content");
        CHECK(list.GetTotalLength() == static_cast<FileOffset>(ref.size()),
              "clamped Write keeps the total length");
    }

    // FillRange: ブロック跨ぎの定数上書き。
    {
        const FileOffset pos = 5000;
        const FileOffset len = static_cast<FileOffset>(kBlockCapacity) * 2 + 3;
        BlockCursor c(&list);
        const FileOffset n = c.FillRange(pos, len, 0xE7);
        CHECK(n == len, "FillRange returns the filled length");
        std::fill(ref.begin() + static_cast<size_t>(pos),
                  ref.begin() + static_cast<size_t>(pos + len),
                  static_cast<unsigned char>(0xE7));
        CHECK(ReadAll(list) == ref, "FillRange across blocks");
        CHECK(list.GetTotalLength() == static_cast<FileOffset>(ref.size()),
              "FillRange keeps the total length");
    }

    // FillRange: 末尾クランプと境界値。
    {
        BlockCursor c(&list);
        CHECK(c.FillRange(0, 0, 0x00) == 0, "FillRange of zero length writes nothing");
        CHECK(c.FillRange(-1, 10, 0x00) == 0, "FillRange rejects a negative position");
        CHECK(c.FillRange(static_cast<FileOffset>(ref.size()), 10, 0x00) == 0,
              "FillRange past the end writes nothing");
        const FileOffset pos = static_cast<FileOffset>(ref.size()) - 3;
        CHECK(c.FillRange(pos, 100, 0x11) == 3, "FillRange clamps at the end of data");
        std::fill(ref.end() - 3, ref.end(), static_cast<unsigned char>(0x11));
        CHECK(ReadAll(list) == ref, "clamped FillRange content");
    }

    // FillRange は SetByteAt の反復と同じ結果になる（小さな範囲で突合）。
    {
        BlockList a;
        BlockList b;
        std::vector<unsigned char> seed(kBlockCapacity + 500);
        for (size_t i = 0; i < seed.size(); ++i) { seed[i] = static_cast<unsigned char>(i); }
        for (BlockList* l : {&a, &b}) {
            NewEmptyDoc(*l);
            BlockCursor c(l);
            CHECK(c.Insert(0, seed.data(), static_cast<FileOffset>(seed.size())), "seed pair");
        }
        const FileOffset pos = kBlockCapacity - 50;
        const FileOffset len = 200;
        {
            BlockCursor c(&a);
            c.FillRange(pos, len, 0x77);
        }
        for (FileOffset i = 0; i < len; ++i) {
            BlockCursor c(&b);
            c.SetByteAt(pos + i, 0x77);
        }
        CHECK(ReadAll(a) == ReadAll(b), "FillRange matches repeated SetByteAt");
    }

#ifdef STIRLING_TEST_ALLOC_HOOK
    // 確保を伴わないため、確保失敗の注入下でも成功しドキュメントは正しく更新される。
    {
        BlockList l;
        NewEmptyDoc(l);
        std::vector<unsigned char> seed(kBlockCapacity + 100, 0x01);
        {
            BlockCursor c(&l);
            CHECK(c.Insert(0, seed.data(), static_cast<FileOffset>(seed.size())), "seed no-alloc");
        }
        stirling::SetAllocFailCountdown(1);
        FileOffset n = 0;
        {
            BlockCursor c(&l);
            n = c.FillRange(0, static_cast<FileOffset>(seed.size()), 0x02);
        }
        stirling::SetAllocFailCountdown(0);
        CHECK(n == static_cast<FileOffset>(seed.size()), "FillRange needs no allocation");
        const std::vector<unsigned char> got = ReadAll(l);
        CHECK(got == std::vector<unsigned char>(seed.size(), 0x02), "FillRange content under injection");
    }
#endif
}

// ---- StreamFileWriter: 一時ファイル経由の逐次書込（Issue #155） ----
// 選択範囲の保存・ダンプ保存は、書き終えてから出力先を置換する。途中で失敗しても
// 既存ファイルが空や不完全な内容に置き換わらないことを確認する。
static void TestStreamFileWriter() {
    std::printf("TestStreamFileWriter\n");
    using stirling::StreamFileWriter;

    // 1) 新規作成。チャンクを分けて書いても内容が連結される。
    {
        const fs::path out = TempFile("sfw_new");
        fs::remove(out);
        StreamFileWriter w;
        CHECK(w.Open(out.wstring().c_str()).Ok(), "open for a new file");
        CHECK(w.IsOpen(), "writer is open");
        CHECK(!fs::exists(out), "the target is not created until commit");
        const std::string a = "ABCDE";
        const std::string b = "0123456789";
        CHECK(w.Write(a.data(), a.size()).Ok(), "write first chunk");
        CHECK(w.Write(b.data(), b.size()).Ok(), "write second chunk");
        CHECK(w.Written() == static_cast<FileOffset>(a.size() + b.size()), "written total");
        CHECK(w.Commit().Ok(), "commit");
        CHECK(!w.IsOpen(), "writer is closed after commit");
        const std::vector<unsigned char> got = ReadFileBytes(out);
        const std::string want = a + b;
        CHECK(got == std::vector<unsigned char>(want.begin(), want.end()), "committed content");
        fs::remove(out);
    }

    // 2) 既存ファイルの置換。Commit までは元の内容が残る。
    {
        const fs::path out = TempFile("sfw_replace");
        const std::vector<unsigned char> orig = { 'o', 'l', 'd' };
        WriteFile(out, orig);
        StreamFileWriter w;
        CHECK(w.Open(out.wstring().c_str()).Ok(), "open over an existing file");
        const std::string neu = "brand-new-content";
        CHECK(w.Write(neu.data(), neu.size()).Ok(), "write replacement");
        CHECK(ReadFileBytes(out) == orig, "the existing file is untouched before commit");
        CHECK(w.Commit().Ok(), "commit the replacement");
        const std::vector<unsigned char> got = ReadFileBytes(out);
        CHECK(got == std::vector<unsigned char>(neu.begin(), neu.end()), "replaced content");
        fs::remove(out);
    }

    // 3) Abort（途中失敗の代替）。既存ファイルは元のまま、一時ファイルも残らない。
    {
        const fs::path out = TempFile("sfw_abort");
        const std::vector<unsigned char> orig = { 'k', 'e', 'e', 'p' };
        WriteFile(out, orig);
        const size_t before = std::distance(fs::directory_iterator(out.parent_path()),
                                            fs::directory_iterator());
        {
            StreamFileWriter w;
            CHECK(w.Open(out.wstring().c_str()).Ok(), "open for abort");
            const std::string partial = "partial";
            CHECK(w.Write(partial.data(), partial.size()).Ok(), "write partial data");
            w.Abort();
            CHECK(!w.IsOpen(), "writer is closed after abort");
        }
        CHECK(ReadFileBytes(out) == orig, "aborting leaves the existing file untouched");
        const size_t after = std::distance(fs::directory_iterator(out.parent_path()),
                                           fs::directory_iterator());
        CHECK(after == before, "aborting leaves no temporary file behind");
        fs::remove(out);
    }

    // 4) デストラクタでも一時ファイルを片付ける（Commit を呼ばずに抜けた場合）。
    {
        const fs::path out = TempFile("sfw_dtor");
        const std::vector<unsigned char> orig = { 'k', 'e', 'e', 'p', '2' };
        WriteFile(out, orig);
        const size_t before = std::distance(fs::directory_iterator(out.parent_path()),
                                            fs::directory_iterator());
        {
            StreamFileWriter w;
            CHECK(w.Open(out.wstring().c_str()).Ok(), "open for the destructor case");
            const std::string partial = "partial";
            CHECK(w.Write(partial.data(), partial.size()).Ok(), "write partial data");
        }   // ここでデストラクタ＝Abort 相当
        CHECK(ReadFileBytes(out) == orig, "the destructor leaves the existing file untouched");
        const size_t after = std::distance(fs::directory_iterator(out.parent_path()),
                                           fs::directory_iterator());
        CHECK(after == before, "the destructor leaves no temporary file behind");
        fs::remove(out);
    }

    // 5) 8MB の書込分割（1 回の WriteFile 上限）を跨ぐサイズでも内容が一致する。
    {
        const fs::path out = TempFile("sfw_big");
        fs::remove(out);
        std::vector<unsigned char> big(9u * 1024u * 1024u + 12345u);
        for (size_t i = 0; i < big.size(); ++i) { big[i] = static_cast<unsigned char>(i * 13); }
        StreamFileWriter w;
        CHECK(w.Open(out.wstring().c_str()).Ok(), "open for a large write");
        CHECK(w.Write(big.data(), big.size()).Ok(), "write across the internal chunk limit");
        CHECK(w.Commit().Ok(), "commit the large write");
        CHECK(ReadFileBytes(out) == big, "large content round-trips");
        fs::remove(out);
    }

    // 6) 開けないパス（存在しないディレクトリ）は失敗を返す。
    {
        const fs::path bad = TempFile("sfw_nodir") / L"sub" / L"out.bin";
        StreamFileWriter w;
        const stirling::FileIoResult r = w.Open(bad.wstring().c_str());
        CHECK(!r.Ok(), "opening under a missing directory fails");
        CHECK(r.status == stirling::FileIoStatus::kOpenFailed, "open failure status");
        CHECK(!w.IsOpen(), "writer stays closed after a failed open");
        CHECK(!w.Write("x", 1).Ok(), "writing without an open target fails");
        CHECK(!w.Commit().Ok(), "committing without an open target fails");
    }

    // 7) 空パスは開かずに失敗する。
    {
        StreamFileWriter w;
        CHECK(!w.Open(L"").Ok(), "empty path fails");
        CHECK(!w.Open(nullptr).Ok(), "null path fails");
    }
}

// ---- BGREP 通知（Issue #156） ----
// ワーカ→UI の通知は、以前はファイルサイズ・ヒット位置を WPARAM へ直接入れていた。
// WPARAM は Win32 で 32bit のため 4GB 以上が切り詰められる（4GB の倍数は 0 に化けて
// 「アクセス拒否」と誤判定される）。LPARAM 経由で構造体を渡す形になったことで、
// 4GB 境界前後の値が欠損しないことを確認する。
static void TestBgrepNotify() {
    std::printf("TestBgrepNotify\n");
    using stirling::BgrepHitNotify;
    using stirling::BgrepScanNotify;

    // 通知が 64bit を保持できる型であること（WPARAM への逆戻りを型で防ぐ）。
    static_assert(sizeof(BgrepScanNotify::size) == 8, "scan size must stay 64-bit");
    static_assert(sizeof(BgrepHitNotify::pos) == 8, "hit position must stay 64-bit");

    const FileOffset kValues[] = {
        0,
        1,
        0x7FFFFFFFll,          // 2GB - 1
        0x80000000ll,          // 2GB（int なら符号反転する境界）
        0xFFFFFFFFll,          // 4GB - 1
        0x100000000ll,         // 4GB ちょうど（WPARAM 直接格納だと Win32 で 0 に化ける）
        0x100000001ll,         // 4GB + 1
        0x200000000ll,         // 8GB（同上）
        0x123456789Abcll,      // 任意の 4GB 超
    };
    const wchar_t* const kPath = L"C:\\dir\\sub\\target.bin";

    for (const FileOffset v : kValues) {
        // 走査通知: LPARAM 経由で往復しても値が欠けない。
        BgrepScanNotify scan;
        scan.path = kPath;
        scan.size = v;
        const LPARAM lp = (LPARAM)&scan;
        const BgrepScanNotify* rs = (const BgrepScanNotify*)lp;
        CHECK(rs->size == v, "scan size survives the LPARAM round trip");
        CHECK(std::wcscmp(rs->path, kPath) == 0, "scan path survives the LPARAM round trip");
        // 「サイズ 0＝アクセス拒否」の判定が 4GB の倍数で誤発火しない。
        CHECK((rs->size == 0) == (v == 0), "access-denied decision uses the full 64-bit size");

        // ヒット通知: 同上。
        BgrepHitNotify hit;
        hit.path = kPath;
        hit.pos = v;
        const LPARAM lph = (LPARAM)&hit;
        const BgrepHitNotify* rh = (const BgrepHitNotify*)lph;
        CHECK(rh->pos == v, "hit position survives the LPARAM round trip");
        CHECK(std::wcscmp(rh->path, kPath) == 0, "hit path survives the LPARAM round trip");
    }

    // 対比: WPARAM へ直接入れると Win32 では 4GB 以上が失われる（退行の検知用）。
    //   x64 では WPARAM も 64bit なので欠損しない。ビルド毎に期待値を切り替える。
    {
        const FileOffset v = 0x100000000ll;   // 4GB ちょうど
        const WPARAM packed = (WPARAM)v;
        const FileOffset unpacked = (FileOffset)packed;
        if (sizeof(WPARAM) == 4) {
            CHECK(unpacked == 0, "WPARAM truncates 4GB to 0 on Win32 (why the struct is needed)");
        } else {
            CHECK(unpacked == v, "WPARAM keeps 4GB on x64");
        }
    }
}

int main() {
    std::printf("=== Stirling core unit tests ===\n");
    TestBlockListBasics();
    TestMultiByteInsert();
    TestRead();
    TestSeek();
    TestInsertByteSplit();
    TestInsertByteFullBlockLastPos();
    TestInsertOverflowAtLastByte();
    TestDelete();
    TestDeleteLastByteSingleBlock();
    TestFuzz();
    TestFileRoundTrip();
    TestLoadEditSave();
    TestSearchBasic();
    TestSearchEofBounds();
    TestSearchMismatch();
    TestSearchAcrossBlocks();
    TestSearchFuzz();
    TestSearchBackwardMissedMatch();
    TestSetByteAt();
    TestLargeOffsetSeek();
    TestLargeOffsetDataOps();
    TestLargeRealData();
    TestFileIoStatus();
    TestLargeFileRoundTrip();
    TestSettingsCodec();
    TestSettingsCodecWide();
    TestSettingsMigration();
    TestSettingsStoreUtf8();
    TestSettingsStoreValueEscape();
    TestMarkFileRoundTrip();
    TestMarkFileEmptyAndComments();
    TestMarkFileRejects();
    TestMarkFileHugeDecimals();
    TestMarkListRoundTrip();
    TestMarkListLimitAndRejects();
    TestSettingsStoreIni();
    TestSettingsStoreChangeLog();
    TestSettingsFileMergedSave();
    TestSettingsFileConcurrentSave();
    TestFolderToReveal();
    TestSettingsStoreBinary();
    TestCp932Text();
    TestCp932LeadByte();
    TestFormatStructCharArrayCp932();
    TestFormatStructCharArrayW();
    TestStructDefParse();
    TestHexTextParse();
    TestUtf8Text();
    TestClipboardUtil();
    TestUndoBudget();
    TestDeleteRange();
    TestWriteAndFillRange();
    TestStreamFileWriter();
    TestBgrepNotify();
#ifdef STIRLING_TEST_ALLOC_HOOK
    TestAllocFailureRollback();
#endif

    std::printf("=== %d checks, %d failures ===\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("FAILED\n");
    return 1;
}
