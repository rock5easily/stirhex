// core 層（BlockList / BlockCursor）単体テスト。
// 線形参照モデル（std::vector）と並行操作し、毎回バイト一致・構造不変条件を検証する。
// ビルド: porting/tests/core/build_core_test.ps1（cl.exe）または任意の C++17 コンパイラ。
#include "../../StirHex/src/core/BlockCursor.h"
#include "../../StirHex/src/core/BlockFileIO.h"
#include "../../StirHex/src/core/StreamFileWriter.h"
#include "../../StirHex/src/core/BgrepNotify.h"
#include "../../StirHex/src/core/BlockList.h"
#include "../../StirHex/src/app/SettingsCodec.h"
#include "../../StirHex/src/app/SettingsMigration.h"
#include "../../StirHex/src/app/SettingsStore.h"
#include "../../StirHex/src/app/SettingsFile.h"
#include "../../StirHex/src/util/PathParts.h"
#include "../../StirHex/src/app/MarkFile.h"
#include "../../StirHex/src/core/Cp932Text.h"
#include "../../StirHex/src/core/CharConv.h"
#include "../../StirHex/src/core/StructDef.h"
#include "../../StirHex/src/core/UndoBudget.h"
#include "../../StirHex/src/core/HexText.h"
#include "../../StirHex/src/core/Utf8Text.h"
#include "../../StirHex/src/core/Utf16Text.h"

#include <algorithm>
#include <atomic>
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
#include "../../StirHex/src/app/ClipboardUtil.h"   // クリップボード転送の RAII（Issue #47）
#include "../../StirHex/src/core/Win32FileHooks.h"   // I/O 故障注入の差込口（Issue #180）

#include "core/Checksum.h"

using stirling::BlockCursor;
using stirling::BlockList;
using stirling::BlockNode;
using stirling::FileOffset;
using stirling::kBlockCapacity;
using stirling::kReadChunk;

#include "CoreTestReport.h"

static core_test::Report g_report;
static int TestPrintf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int result = g_report.Print(format, args);
    va_end(args);
    return result;
}

static int g_failures = 0;
static int g_checks = 0;
// スキップしたテストの記録。ALL PASS が「登録した全テストを実検証した」意味に
//   読めてしまわないよう、末尾でスキップ件数と理由を集計して表示する。
static std::vector<std::string> g_skipped;

static void SkipTest(const char* name, const char* reason) {
    g_report.Skip(name, reason);
    g_skipped.push_back(std::string(name) + " (" + reason + ")");
    TestPrintf("  skipped (%s)\n", reason);
}

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(cond)) {                                                        \
            ++g_failures;                                                     \
            TestPrintf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__);   \
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
        if (!ok) { TestPrintf("  FAIL: ReadAll seek failed\n"); ++g_failures; return buf; }
        const FileOffset n = c.Read(total, buf.data());
        if (n != total) {
            TestPrintf("  FAIL: ReadAll short read %lld/%lld\n",
                        static_cast<long long>(n), static_cast<long long>(total));
            ++g_failures;
        }
    }
    return buf;
}

// 構造不変条件: 各ノード capacity==kBlockCapacity, 0<=usedLen<=capacity, 合計==size に加え、
//   前後リンクの整合・Count と実走査数の一致・逆走査の同一性まで見る（Issue #178）。
//   走査は Count() の 2 倍で打ち切る（リンクが輪になっても無限ループしない）。
static void CheckInvariants(BlockList& list, size_t expectedLen, const char* where) {
    const int declared = list.Count();
    const int limit = (declared > 0 ? declared : 1) * 2 + 4;

    FileOffset sum = 0;
    int seen = 0;
    bool linksOk = true;
    std::vector<BlockNode*> forward;
    BlockNode* prev = nullptr;
    for (BlockNode* n = list.GetHead(); n != nullptr && seen < limit; n = list.GetNext(n)) {
        CHECK(n->capacity == kBlockCapacity, where);
        CHECK(n->usedLen >= 0 && n->usedLen <= n->capacity, where);
        if (list.GetPrev(n) != prev) { linksOk = false; }
        sum += n->usedLen;
        forward.push_back(n);
        prev = n;
        ++seen;
    }
    CHECK(seen == declared, where);                 // Count() と実走査数
    CHECK(list.GetTail() == prev, where);           // 末尾は最後に辿ったノード
    CHECK(linksOk, where);                          // prev リンクの整合

    // 逆走査が前方走査の逆順と一致する（片方向だけ壊れた破損を捕まえる）。
    std::vector<BlockNode*> backward;
    int back = 0;
    for (BlockNode* n = list.GetTail(); n != nullptr && back < limit; n = list.GetPrev(n), ++back) {
        backward.push_back(n);
    }
    std::reverse(backward.begin(), backward.end());
    CHECK(backward == forward, where);

    CHECK(sum == static_cast<FileOffset>(expectedLen), where);
    CHECK(list.GetTotalLength() == static_cast<FileOffset>(expectedLen), where);
}

static void CheckEqual(BlockList& list, const std::vector<unsigned char>& ref, const char* where) {
    std::vector<unsigned char> got = ReadAll(list);
    bool eq = (got.size() == ref.size()) &&
              (ref.empty() || std::memcmp(got.data(), ref.data(), ref.size()) == 0);
    if (!eq) {
        ++g_failures;
        TestPrintf("  FAIL: content mismatch at %s (got %zu bytes, ref %zu)\n",
                    where, got.size(), ref.size());
        size_t lim = got.size() < ref.size() ? got.size() : ref.size();
        for (size_t i = 0; i < lim; ++i) {
            if (got[i] != ref[i]) { TestPrintf("    first diff at %zu: got %02X ref %02X\n",
                                                i, got[i], ref[i]); break; }
        }
    }
    ++g_checks;
}

// ---- BlockList 基本操作 ----
static void TestBlockListBasics() {
    TestPrintf("TestBlockListBasics\n");
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
    TestPrintf("TestMultiByteInsert\n");
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
    TestPrintf("TestRead\n");
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
    TestPrintf("TestSeek\n");
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
    TestPrintf("TestInsertByteSplit\n");
    // Issue #93 の修正で、最終バイト上(16383)と EOF 追記位置(16384)を含む
    // 全位置が素直な挿入と一致する。
    //   最終バイト上(16383)は名前付きの回帰ケース TestInsertByteFullBlockLastPos が
    //   押し出し先まで含めて確認するため、ここでは重複させない（Issue #178）。
    for (int pos : {0, 1, 4000, 8191, 8192, 12000, 16382, 16384}) {
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
    TestPrintf("TestInsertByteFullBlockLastPos\n");
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

    // 押し出しの確認はリスト側から読み出す（参照 vector を見るだけでは、
    //   アプリの動作を一切通らないため回帰を検出できない。Issue #178）。
    unsigned char at16383 = 0, at16384 = 0;
    CHECK(c.Seek(kBlockCapacity - 1, BlockCursor::kBegin, nullptr) && c.Read(1, &at16383) == 1,
          "read the inserted position");
    CHECK(c.Seek(kBlockCapacity, BlockCursor::kBegin, nullptr) && c.Read(1, &at16384) == 1,
          "read the pushed position");
    CHECK(at16383 == 0xEE, "the inserted byte sits at the insert position");
    CHECK(at16384 == lastByte, "original last byte pushed right");
}

// ---- 容量超過 Insert がブロック最終バイト上でも順序を保つ（Issue #93 回帰）----
// 原 BlockCursor_InsertWorker(0x0041cd40) の分割条件 `curOffset < usedLen-1` では、
// curOffset==usedLen-1 かつ現ブロックに収まらない挿入が分割にも空ブロック分岐にも入らず、
// 最終バイトを右へずらさないまま後続ブロックへ追記していた（挿入内容が最終バイトの前に残る）。
static void TestInsertOverflowAtLastByte() {
    TestPrintf("TestInsertOverflowAtLastByte\n");
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
    TestPrintf("TestDelete\n");
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
    TestPrintf("TestDeleteLastByteSingleBlock\n");
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
    TestPrintf("TestFuzz\n");
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
            if (!ok) { TestPrintf("  FAIL: fuzz InsertByte failed op=%d pos=%d\n", op, pos); ++g_failures; break; }
            ref.insert(ref.begin() + pos, v);
        } else {
            int pos = static_cast<int>(rng() % size);
            unsigned char b;
            bool ok = c.DeleteByte(pos, &b);
            if (!ok) { TestPrintf("  FAIL: fuzz DeleteByte failed op=%d pos=%d\n", op, pos); ++g_failures; break; }
            if (b != ref[pos]) { TestPrintf("  FAIL: fuzz deleted byte mismatch op=%d\n", op); ++g_failures; break; }
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
    TestPrintf("  fuzz grew to %zu bytes, %d blocks\n", ref.size(), list.Count());
}

// ---- BlockFileIO: Load/Save ラウンドトリップ ----
namespace fs = std::filesystem;

// テスト実行ごとの専用一時ディレクトリ。%TEMP% 直下を共有すると、他プロセスや別の
//   テスト実行が作るファイルで「一時ファイルを残さない」検証（ディレクトリ内容の前後比較）
//   が揺れるため、実行単位で隔離する。
static const fs::path& TestTempRoot() {
    static const fs::path root = [] {
        wchar_t name[128];
        _snwprintf_s(name, _TRUNCATE, L"stirhex_core_test_%lu_%llu",
                     static_cast<unsigned long>(::GetCurrentProcessId()),
                     static_cast<unsigned long long>(::GetTickCount64()));
        const fs::path p = fs::temp_directory_path() / name;
        std::error_code ec;
        fs::create_directories(p, ec);
        return p;
    }();
    return root;
}

static fs::path TempFile(const char* tag) {
    static int counter = 0;
    return TestTempRoot() /
           (std::string("stirling_core_test_") + tag + "_" + std::to_string(counter++) + ".bin");
}

// ディレクトリ直下のエントリ名（ソート済み）。件数ではなく集合で比較することで、
//   「残骸が増えた」だけでなく「無関係なファイルが消えた」も検出できる。
static std::vector<std::wstring> DirEntryNames(const fs::path& dir) {
    std::vector<std::wstring> names;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec); it != fs::directory_iterator(); it.increment(ec)) {
        names.push_back(it->path().filename().wstring());
    }
    std::sort(names.begin(), names.end());
    return names;
}

// core の I/O はワイドパス（Issue #20）。テスト側のヘルパも _wfopen に揃える。
static void WriteFile(const fs::path& p, const std::vector<unsigned char>& data) {
    std::FILE* f = _wfopen(p.wstring().c_str(), L"wb");
    if (!f) { TestPrintf("  FAIL: cannot create temp %s\n", p.string().c_str()); ++g_failures; return; }
    if (!data.empty()) std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
}

static std::vector<unsigned char> ReadFileBytes(const fs::path& p) {
    std::vector<unsigned char> out;
    std::FILE* f = _wfopen(p.wstring().c_str(), L"rb");
    if (!f) { TestPrintf("  FAIL: cannot open temp %s\n", p.string().c_str()); ++g_failures; return out; }
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
    TestPrintf("TestFileRoundTrip\n");
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
    TestPrintf("TestLoadEditSave\n");
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
    TestPrintf("TestSearchBasic\n");
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
    TestPrintf("TestSearchEofBounds\n");
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
    TestPrintf("TestSearchMismatch\n");
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
    TestPrintf("TestSearchAcrossBlocks\n");
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
    TestPrintf("TestSearchFuzz\n");
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
            // パターン長は 1..6 を中心に、たまにブロック長前後の長さも混ぜる（Issue #178）。
            int m = 1 + static_cast<int>(rng() % 6);
            if ((rng() % 8) == 0) { m = 1 + static_cast<int>(rng() % (kBlockCapacity + 64)); }
            std::vector<unsigned char> pat(m);
            if ((rng() & 1) && size >= m) {
                int src = static_cast<int>(rng() % (size - m + 1));  // 実在部分列（ヒット保証）
                std::memcpy(pat.data(), data.data() + src, m);
            } else {
                for (int i = 0; i < m; ++i) pat[i] = static_cast<unsigned char>(rng() % alpha);
            }
            // 前方: ナイーブ参照と完全一致。範囲は全長（end=0）と有限の [start,end) の
            //   2 通りを試す（以前は end=0 のみで、範囲指定の取りこぼしを見ていなかった。
            //   Issue #178）。
            int start = static_cast<int>(rng() % (size + 1));
            for (int rangeCase = 0; rangeCase < 2; ++rangeCase) {
                int fend = 0;                       // 0 = 全長
                int naiveEnd = size;
                if (rangeCase == 1) {
                    if (start >= size) continue;    // 有限 end を作れない
                    fend = start + 1 + static_cast<int>(rng() % (size - start));   // (start, size]
                    naiveEnd = fend;
                }
                FileOffset got = -1;
                bool f = c.SearchPattern(pat.data(), m, &got, BlockCursor::kForward, start, fend);
                int exp = NaiveForward(data, pat, start, naiveEnd);
                ++fwdCases;
                if ((f ? got : -1) != exp) {
                    ++fwdMism;
                    if (fwdMism <= 3)
                        TestPrintf("  FAIL fwd: size=%d m=%d start=%d end=%d got=%lld exp=%d\n",
                                    size, m, start, fend,
                                    static_cast<long long>(f ? got : -1), exp);
                }
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
                        TestPrintf("  FAIL bwd: size=%d m=%d start=%d end=%d got=%lld exp=%d\n",
                                    size, m, bstart, bend,
                                    static_cast<long long>(f2 ? got2 : -1), exp2);
                }
            }
        }
    }
    CHECK(fwdMism == 0, "forward search matches naive reference");
    CHECK(bwdMism == 0, "backward search matches naive reference");
    TestPrintf("  search fuzz: fwd %d cases / %d mismatches, bwd %d cases / %d found / %d mismatches\n",
                fwdCases, fwdMism, bwdCases, bwdFound, bwdMism);
}

// ---- 後方検索の取りこぼし回帰（Issue #71）----
// 原実装の bad-character 表はパターン内の「最右」の出現位置を採っていたため、
//   シフトが過大になり、間にある一致を飛び越えて not-found を返していた。
//   ここでは実際に取りこぼしていた具体例を固定ケースとして押さえる。
static void TestSearchBackwardMissedMatch() {
    TestPrintf("TestSearchBackwardMissedMatch\n");
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
    TestPrintf("TestSetByteAt\n");
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
    TestPrintf("TestLargeOffsetSeek\n");
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
    TestPrintf("TestLargeOffsetDataOps\n");
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

// ---- 64bit 境界での一括操作（Issue #183）----------------------------------
// 一括操作（Issue #154 の Write / FillRange、Issue #62 の DeleteRange）は数万バイト
//   規模でしか検証されておらず、TestLargeOffsetDataOps も通していなかった。
//   疎ブロックモデル（data=nullptr の巨大ブロック + 境界をまたぐ実データ末尾）で
//   2GB / 4GB 境界前後を安価に確認する。
//
// 疎な領域のノードは data==nullptr なので、実際に読み書きが走る操作を掛けると落ちる。
//   - Write / FillRange は操作範囲を実データの末尾ブロック群へ限定する
//   - DeleteRange は「ノードごと除去される」形（off==0 かつ 1 ノード全体）でのみ
//     疎な領域へ掛ける。部分的に残す削除は memmove / memset が nullptr に対して走る
static void TestLargeOffsetBulkOps() {
    TestPrintf("TestLargeOffsetBulkOps\n");
    // 疎ブロックは data 実体を持たないため確保量はノード分だけで済む。既存の
    //   TestLargeOffsetSeek / TestLargeOffsetDataOps と同じく両アーキテクチャで実行する。
    const int kTailBlocks = 8;
    const size_t tailLen = static_cast<size_t>(kTailBlocks) * kBlockCapacity;   // 128KB
    const size_t kBnd = tailLen / 2;                                           // 実データ内の境界位置

    struct Boundary { FileOffset at; const char* name; };
    const Boundary boundaries[] = {
        { k2GB,     "2GB" },
        { k2GB * 2, "4GB" },   // 下位 32bit が 0 に化ける境界
    };

    for (const Boundary& bnd : boundaries) {
        TestPrintf("  boundary %s\n", bnd.name);
        std::vector<unsigned char> tail(tailLen);
        for (size_t i = 0; i < tailLen; ++i) {
            tail[i] = static_cast<unsigned char>((i * 11 + 3) & 0xFF);
        }

        BlockList list;
        const FileOffset tailBase = bnd.at - static_cast<FileOffset>(kBnd);
        CHECK(tailBase % kBlockCapacity == 0, "the sparse prefix ends on a block boundary");
        AppendSparseBlocks(list, tailBase / kBlockCapacity);
        AppendRealBlocks(list, tail);
        FileOffset total = tailBase + static_cast<FileOffset>(tailLen);
        CHECK(list.GetTotalLength() == total, "the sparse document has the expected length");
        if (bnd.at == k2GB * 2) {
            // 下位 32bit だけを見ると 0 に化ける位置であることを明示しておく。
            CHECK(static_cast<unsigned int>(bnd.at) == 0,
                  "the 4GB boundary truncates to zero in 32 bits (regression anchor)");
        }

        // 実データ領域を丸ごと読み出して参照モデルと突き合わせる。
        auto realTail = [&list](FileOffset base, size_t len) {
            std::vector<unsigned char> got(len);
            BlockCursor r(&list);
            if (!r.Seek(base, BlockCursor::kBegin, nullptr)) {
                CHECK(false, "seek to the real region");
                return got;
            }
            const FileOffset n = r.Read(static_cast<FileOffset>(len), got.data());
            CHECK(n == static_cast<FileOffset>(len), "read the whole real region");
            return got;
        };

        // --- Write: 境界をまたぐ上書き（長さは変わらない）---
        {
            const FileOffset at = bnd.at - 100;
            std::vector<unsigned char> src(200);
            for (size_t i = 0; i < src.size(); ++i) {
                src[i] = static_cast<unsigned char>(0xC0 | (i & 0x0F));
            }
            BlockCursor c(&list);
            const FileOffset n = c.Write(at, src.data(), static_cast<FileOffset>(src.size()));
            CHECK(n == static_cast<FileOffset>(src.size()), "Write across the boundary writes all");
            std::copy(src.begin(), src.end(), tail.begin() + (kBnd - 100));
            CHECK(list.GetTotalLength() == total, "Write does not change the length");
            CHECK(realTail(tailBase, tail.size()) == tail, "Write content across the boundary");
        }

        // --- Write: 複数ブロックにまたがる長い上書き ---
        {
            const FileOffset at = bnd.at - 20000;
            std::vector<unsigned char> src(40000);
            for (size_t i = 0; i < src.size(); ++i) {
                src[i] = static_cast<unsigned char>((i * 37 + 5) & 0xFF);
            }
            BlockCursor c(&list);
            const FileOffset n = c.Write(at, src.data(), static_cast<FileOffset>(src.size()));
            CHECK(n == static_cast<FileOffset>(src.size()), "a long Write across the boundary");
            std::copy(src.begin(), src.end(), tail.begin() + (kBnd - 20000));
            CHECK(realTail(tailBase, tail.size()) == tail, "long Write content across the boundary");
        }

        // --- FillRange: 境界をまたぐ範囲初期化と、末尾でのクランプ ---
        {
            const FileOffset at = bnd.at - 5000;
            BlockCursor c(&list);
            const FileOffset n = c.FillRange(at, 10000, 0x5A);
            CHECK(n == 10000, "FillRange across the boundary fills all");
            std::fill(tail.begin() + (kBnd - 5000), tail.begin() + (kBnd + 5000),
                      static_cast<unsigned char>(0x5A));
            CHECK(list.GetTotalLength() == total, "FillRange does not change the length");
            CHECK(realTail(tailBase, tail.size()) == tail, "FillRange content across the boundary");

            // 巨大な count は残り長さへ丸められる（32bit へ落ちない）。
            BlockCursor e(&list);
            const FileOffset filled = e.FillRange(total - 3, 8LL * 1024 * 1024 * 1024, 0x77);
            CHECK(filled == 3, "FillRange clamps a huge count at the end of a large document");
            tail[tailLen - 3] = tail[tailLen - 2] = tail[tailLen - 1] = 0x77;
            CHECK(realTail(tailBase, tail.size()) == tail, "the clamped fill touched only the tail");
        }

        // --- DeleteRange: 境界をまたぐ削除（実データ内に限定）---
        {
            const FileOffset at = bnd.at - 50;
            BlockCursor c(&list);
            const FileOffset n = c.DeleteRange(at, 100);
            CHECK(n == 100, "DeleteRange across the boundary removes every byte");
            tail.erase(tail.begin() + (kBnd - 50), tail.begin() + (kBnd + 50));
            total -= 100;
            CHECK(list.GetTotalLength() == total, "the length drops by the deleted amount");
            CHECK(realTail(tailBase, tail.size()) == tail, "DeleteRange content across the boundary");
        }

        // --- DeleteRange: 疎な領域をノードごと除去する（data=nullptr でも安全な経路）---
        //     2GB / 4GB 超のリストからノードを外す 64bit 経路の確認でもある。
        {
            const FileOffset span = static_cast<FileOffset>(kBlockCapacity) * 2;
            const int nodesBefore = list.Count();
            BlockCursor c(&list);
            const FileOffset n = c.DeleteRange(0, span);
            CHECK(n == span, "two whole sparse nodes are deleted");
            CHECK(list.Count() == nodesBefore - 2, "the two nodes left the list");
            total -= span;
            CHECK(list.GetTotalLength() == total, "the length drops by two blocks");
            // 実データはそのぶん手前へ寄るが、内容は変わらない。
            CHECK(realTail(tailBase - span, tail.size()) == tail,
                  "the real tail moved down intact");

            // 4GB 側だけ: 2GB を超える量を一括削除し、削除数の累積が 32bit へ
            //   落ちないことを確かめる。上の 2 ノード分だけでは int へ変えても
            //   桁溢れしないため、この経路がないと戻り値の幅を固定できない。
            //   疎ノードの全体除去なので data==nullptr でも安全、かつ確保も伴わない。
            if (bnd.at == k2GB * 2) {
                const FileOffset huge = 3LL * 1024 * 1024 * 1024;   // 16KB の倍数
                CHECK(huge % kBlockCapacity == 0, "the bulk delete stays on block boundaries");
                BlockCursor big(&list);
                const FileOffset removed = big.DeleteRange(0, huge);
                CHECK(removed == huge, "DeleteRange returns a count above 2GB intact");
                CHECK(removed > 0x7FFFFFFFLL,
                      "the returned count really exceeds the signed 32bit range");
                total -= huge;
                CHECK(list.GetTotalLength() == total, "the length drops by the deleted amount");
                CHECK(realTail(tailBase - span - huge, tail.size()) == tail,
                      "the real tail survived the bulk delete");
            }
        }

        // 注: Write の written と FillRange の filled が 32bit へ落ちる変異は、
        //   1 回の呼び出しで 2GB 超を書く必要があり、疎ブロックモデルでは実データを
        //   その量だけ用意しなければ再現できない（疎な領域は書き込み対象にできない）。
        //   安価に押さえられないため、ここでは扱わない。
    }
}

// 実データ 2GB 超の通し確認（オプトイン）。
// 約 2.1GB のメモリを確保するため、既定ではスキップする。
// 実行するには 64bit ビルドで環境変数 STIRLING_CORE_TEST_LARGE=1 を設定する。
static void TestLargeRealData() {
    TestPrintf("TestLargeRealData\n");
    const char* env = std::getenv("STIRLING_CORE_TEST_LARGE");
    if (env == nullptr || std::strcmp(env, "1") != 0) {
        SkipTest("TestLargeRealData", "set STIRLING_CORE_TEST_LARGE=1 to run");
        return;
    }
    if (sizeof(void*) < 8) {
        SkipTest("TestLargeRealData", "needs a 64-bit build");
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
    TestPrintf("  built %lld bytes of real data\n", static_cast<long long>(total));

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
    TestPrintf("TestFileIoStatus\n");
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
        const fs::path jp = TestTempRoot() / L"stirling_core_test_日本語パス.bin";
        WriteFile(jp, data);

        FileOffset sz = 0;
        CHECK(stirling::QueryFileSize(jp.wstring().c_str(), &sz, nullptr),
              "QueryFileSize with a non-ASCII path");
        CHECK(sz == static_cast<FileOffset>(data.size()), "non-ASCII path size");

        BlockList list;
        CHECK(stirling::LoadFileIntoBlocks(list, jp.wstring().c_str()).Ok(),
              "load with a non-ASCII path");
        CheckEqual(list, data, "non-ASCII path content");

        const fs::path jpOut = TestTempRoot() / L"stirling_core_test_日本語出力.bin";
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

    // 明示フラッシュ（Issue #166）を挟んだ後の保存正常系の回帰。既存の大きいファイルへ
    //   小さい内容を保存し、報告サイズ・実サイズ・内容が一致することを見る。
    //   フラッシュ呼出の有無そのものは検証できない（ローカル FS ではキャッシュ経由でも
    //   直後の読み出しは一致するため）。失敗パスの検証には障害注入が必要で、そのための
    //   I/O モック層は過剰なため入れていない。
    {
        const fs::path out = TempFile("flush_save");
        WriteFile(out, std::vector<unsigned char>(70000, 0xEE));

        BlockList list;
        NewEmptyDoc(list);
        const std::string body = "flushed-and-truncated";
        {
            BlockCursor c(&list);
            CHECK(c.Insert(0, body.data(), static_cast<FileOffset>(body.size())),
                  "seed for the flush check");
        }
        const FileIoResult r = stirling::SaveBlocksToFile(list, out.wstring().c_str());
        CHECK(r.Ok(), "save reports success after the explicit flush");
        CHECK(r.fileSize == static_cast<FileOffset>(body.size()), "save reports the written size");
        CHECK(fs::file_size(out) == body.size(), "the file on disk matches the reported size");
        const std::vector<unsigned char> saved = ReadFileBytes(out);
        CHECK(saved == std::vector<unsigned char>(body.begin(), body.end()),
              "the flushed content replaces the larger original");
        fs::remove(out);
    }
}

// 2GB 超の実ファイルを開き、編集して保存し、内容が一致することを確認する（オプトイン）。
// ディスク約 4.3GB・メモリ約 2.1GB を消費するため、既定ではスキップする。
// 実行するには 64bit ビルドで環境変数 STIRLING_CORE_TEST_LARGE=1 を設定する。
static void TestLargeFileRoundTrip() {
    TestPrintf("TestLargeFileRoundTrip\n");
    const char* env = std::getenv("STIRLING_CORE_TEST_LARGE");
    if (env == nullptr || std::strcmp(env, "1") != 0) {
        SkipTest("TestLargeFileRoundTrip", "set STIRLING_CORE_TEST_LARGE=1 to run");
        return;
    }
    if (sizeof(void*) < 8) {
        SkipTest("TestLargeFileRoundTrip", "needs a 64-bit build");
        return;
    }

    const FileOffset total = k2GB + 128 * 1024;   // 2GB + 128KB
    const fs::path in = TempFile("bigin");
    if (!WriteLargePatternFile(in, total)) {
        TestPrintf("  FAIL: cannot create the 2GB+ source file (disk space?)\n");
        ++g_failures;
        fs::remove(in);
        return;
    }
    TestPrintf("  wrote %lld bytes to disk\n", static_cast<long long>(total));

    // fs::file_size は 64bit。ftell(long) では取得できないサイズであることの確認も兼ねる。
    CHECK(static_cast<FileOffset>(fs::file_size(in)) == total, "source file size beyond 2GB");

    BlockList list;
    const stirling::FileIoResult loaded = stirling::LoadFileIntoBlocks(list, in.wstring().c_str());
    fs::remove(in);   // 読み込み後は不要（ピーク時のディスク使用量を抑える）
    if (!loaded.Ok()) {
        TestPrintf("  FAIL: load failed (status=%d, err=%lu)\n",
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
        TestPrintf("  FAIL: save failed (status=%d, err=%lu)\n",
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
    TestPrintf("TestSettingsCodec\n");
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
    TestPrintf("TestSettingsCodecWide\n");
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
    TestPrintf("TestSettingsMigration\n");
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
    TestPrintf("TestSettingsStoreUtf8\n");
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
    TestPrintf("TestSettingsStoreValueEscape\n");
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
    TestPrintf("[TestMarkFileRoundTrip]\n");
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
    TestPrintf("[TestMarkFileEmptyAndComments]\n");
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
    TestPrintf("[TestMarkFileRejects]\n");
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
    TestPrintf("[TestMarkFileHugeDecimals]\n");
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
    TestPrintf("[TestMarkListRoundTrip]\n");
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
    TestPrintf("[TestMarkListLimitAndRejects]\n");
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
    TestPrintf("TestSettingsStoreIni\n");
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
    TestPrintf("TestSettingsStoreChangeLog\n");
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
//   %TEMP% 直下の固定名だと複数のテストプロセスが同じファイルを奪い合うため、
//   実行ごとに一意な TestTempRoot() 配下へ置く。
static std::wstring MergeTestIniPath(const wchar_t* name) {
    return (TestTempRoot() / name).wstring();
}

static void TestSettingsFileMergedSave() {
    TestPrintf("TestSettingsFileMergedSave\n");
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

    // 変更が無ければ書きに行かない（内容も更新時刻も変えず、一時ファイルも作らない）。
    //   以前は戻り値が成功であることしか見ておらず、「書きに行かない」ことは未確認だった
    //   （Issue #178）。
    SettingsStore clean;
    CHECK(LoadSettingsFile(path, clean, error), "clean snapshot");
    clean.ClearDirty();
    const std::vector<unsigned char> beforeBytes = ReadFileBytes(fs::path(path));
    std::error_code tec;
    const fs::file_time_type beforeTime = fs::last_write_time(fs::path(path), tec);
    CHECK(!tec, "the modification time is readable");
    ::Sleep(50);   // 更新時刻の分解能（システムクロックの刻み）より長く待つ
    CHECK(SaveSettingsFileMerged(path, clean, error), "no-op save succeeds");
    CHECK(ReadFileBytes(fs::path(path)) == beforeBytes, "a no-op save does not rewrite the file");
    CHECK(fs::last_write_time(fs::path(path), tec) == beforeTime,
          "a no-op save leaves the modification time alone");

    // 一時ファイルを残さない（成功した保存でも、書きに行かなかった no-op でも）。
    const std::wstring temp = path + L"." + std::to_wstring(::GetCurrentProcessId()) + L".tmp";
    CHECK(::GetFileAttributesW(temp.c_str()) == INVALID_FILE_ATTRIBUTES,
          "the temp file is gone after saving");

    ::DeleteFileW(path.c_str());
}

static void TestSettingsFileConcurrentSave() {
    TestPrintf("TestSettingsFileConcurrentSave\n");
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
    //   スレッドは (1) 開始前に同じ時点のスナップショットを読み、(2) バリアで揃えてから
    //   一斉に書き始める。同期が無いとスケジューリング次第で直列実行になり、ロックが
    //   壊れていても競合しないまま通ってしまう（Issue #178）。
    //   CHECK は スレッド安全ではないので、判定は join 後にまとめて行う。
    const int kRounds = 25;
    struct WriterResult {
        bool snapshotOk = false;
        int  saved = 0;
        int  saveFailures = 0;
        std::wstring firstError;
    };
    WriterResult resultA, resultB;
    std::atomic<int> ready(0);

    auto writer = [&path, &ready, kRounds](const wchar_t* section, WriterResult* out) {
        std::wstring err;
        // 古いスナップショットを持ったまま書き続ける（マージ保存の本来の使われ方）。
        SettingsStore store;
        out->snapshotOk = LoadSettingsFile(path, store, err);
        if (!out->snapshotOk) { out->firstError = err; }
        store.ClearDirty();

        ready.fetch_add(1);
        while (ready.load() < 2) { std::this_thread::yield(); }   // 同時開始を揃える

        for (int i = 0; i < kRounds; ++i) {
            store.Set(section, (L"Key" + std::to_wstring(i)).c_str(), std::to_wstring(i));
            if (SaveSettingsFileMerged(path, store, err)) {
                ++out->saved;
            } else {
                ++out->saveFailures;
                if (out->firstError.empty()) { out->firstError = err; }
            }
        }
    };
    std::thread a(writer, L"WriterA", &resultA);
    std::thread b(writer, L"WriterB", &resultB);
    a.join();
    b.join();

    for (const WriterResult* r : { &resultA, &resultB }) {
        CHECK(r->snapshotOk, "each writer could read its snapshot");
        CHECK(r->saveFailures == 0, "no writer failed to save");
        CHECK(r->saved == kRounds, "every save reported success");
        if (!r->firstError.empty()) {
            TestPrintf("  first writer error: %ls\n", r->firstError.c_str());
        }
    }

    SettingsStore result;
    CHECK(LoadSettingsFile(path, result, error), "the file is still readable");
    const std::wstring* common = result.Find(L"Env", L"Common");
    CHECK(common != nullptr && *common == L"1", "the seed value survives");
    // キーの存在だけでなく値まで突き合わせる（欠落だけでなく取り違えも検出する）。
    int mismatchA = 0, mismatchB = 0;
    for (int i = 0; i < kRounds; ++i) {
        const std::wstring key = L"Key" + std::to_wstring(i);
        const std::wstring want = std::to_wstring(i);
        const std::wstring* va = result.Find(L"WriterA", key);
        const std::wstring* vb = result.Find(L"WriterB", key);
        if (va == nullptr || *va != want) { ++mismatchA; }
        if (vb == nullptr || *vb != want) { ++mismatchB; }
    }
    CHECK(mismatchA == 0, "every WriterA key kept its own value");
    CHECK(mismatchB == 0, "every WriterB key kept its own value");

    ::DeleteFileW(path.c_str());
}

// ---- 設定ファイルの異常経路（Issue #178）----------------------------------
// SettingsStore の UTF-8 / INI 単体テストとは別の層。ここでは実ファイルを相手に、
//   読めない・壊れた設定ファイルを掴んだときの戻り値と、保存に失敗したときに
//   変更履歴（Changes / Dirty）が保持されて再保存で反映されることを確認する。
static void TestSettingsFileErrors() {
    TestPrintf("TestSettingsFileErrors\n");
    using stirling::settings::LoadSettingsFile;
    using stirling::settings::SaveSettingsFile;
    using stirling::settings::SaveSettingsFileMerged;
    using stirling::settings::SettingsStore;

    // 生バイト列をそのまま設定ファイルとして置く（BOM や不正 UTF-8 を作るため）。
    auto putBytes = [](const std::wstring& path, const std::string& bytes) {
        std::vector<unsigned char> raw(bytes.begin(), bytes.end());
        WriteFile(fs::path(path), raw);
    };
    auto rawBytes = [](const std::wstring& path) {
        const std::vector<unsigned char> v = ReadFileBytes(fs::path(path));
        return std::string(v.begin(), v.end());
    };

    // 1) BOM 付きでも読める（利用者が手で保存し直した場合）。
    {
        const std::wstring path = MergeTestIniPath(L"stirhex_bom_test.ini");
        putBytes(path, "\xEF\xBB\xBF[Env]\r\nName=\xE3\x81\x82\r\n");
        SettingsStore store;
        std::wstring error;
        CHECK(LoadSettingsFile(path, store, error), "a BOM prefixed settings file loads");
        CHECK(error.empty(), "a BOM prefixed file reports no error");
        const std::wstring* v = store.Find(L"Env", L"Name");
        CHECK(v != nullptr && *v == L"あ", "the value after the BOM is decoded");
        ::DeleteFileW(path.c_str());
    }

    // 2) 存在しないファイルは「初回起動」として成功扱い（store は変更しない）。
    {
        const std::wstring path = MergeTestIniPath(L"stirhex_absent_test.ini");
        ::DeleteFileW(path.c_str());
        SettingsStore store;
        std::wstring error;
        CHECK(LoadSettingsFile(path, store, error), "a missing settings file is not an error");
        CHECK(error.empty(), "a missing settings file reports no error");
        CHECK(!store.Dirty(), "a missing settings file leaves the store untouched");
    }

    // 3) 壊れた UTF-8 は理由付きで失敗する。
    {
        const std::wstring path = MergeTestIniPath(L"stirhex_badutf8_test.ini");
        putBytes(path, "[Env]\r\nName=A\xC3\x28\x42\r\n");   // 継続バイトが来ない 2 バイト列
        SettingsStore store;
        std::wstring error;
        CHECK(!LoadSettingsFile(path, store, error), "broken UTF-8 fails to load");
        CHECK(!error.empty(), "broken UTF-8 reports a reason");
        CHECK(error.find(path) != std::wstring::npos, "the reason names the file");
        ::DeleteFileW(path.c_str());
    }

    // 4) 解釈できない INI 行も理由付きで失敗する。
    {
        const std::wstring path = MergeTestIniPath(L"stirhex_badini_test.ini");
        putBytes(path, "[Env]\r\nName=1\r\nthis line has no equals sign\r\n");
        SettingsStore store;
        std::wstring error;
        CHECK(!LoadSettingsFile(path, store, error), "an unparsable line fails to load");
        CHECK(!error.empty(), "an unparsable line reports a reason");
        ::DeleteFileW(path.c_str());
    }

    // 5) 読めない設定ファイルへのマージ保存は拒否され、元ファイルを上書きしない。
    //    変更記録は落とさない（次の保存機会に持ち越せること）。
    {
        const std::wstring path = MergeTestIniPath(L"stirhex_mergebad_test.ini");
        const std::string broken = "[Env]\r\nName=1\r\nbroken line\r\n";
        putBytes(path, broken);

        SettingsStore store;
        store.Set(L"Env", L"Added", L"9");
        CHECK(store.Dirty() && store.Changes().size() == 1, "the change is recorded");

        std::wstring error;
        CHECK(!SaveSettingsFileMerged(path, store, error), "merged save refuses a broken file");
        CHECK(!error.empty(), "the refusal reports a reason");
        CHECK(rawBytes(path) == broken, "the broken file is left exactly as it was");
        CHECK(store.Dirty(), "a refused save keeps the store dirty");
        CHECK(store.Changes().size() == 1, "a refused save keeps the change log");

        // 壊れた行を直せば、保留していた変更がそのまま反映される。
        putBytes(path, "[Env]\r\nName=1\r\n");
        CHECK(SaveSettingsFileMerged(path, store, error), "the pending change is saved once readable");
        CHECK(!store.Dirty(), "a successful save clears the dirty flag");
        CHECK(store.Changes().empty(), "a successful save clears the change log");

        SettingsStore reread;
        CHECK(LoadSettingsFile(path, reread, error), "reload after the repair");
        const std::wstring* added = reread.Find(L"Env", L"Added");
        CHECK(added != nullptr && *added == L"9", "the pending change reached the file");
        const std::wstring* kept = reread.Find(L"Env", L"Name");
        CHECK(kept != nullptr && *kept == L"1", "the existing key is untouched");
        ::DeleteFileW(path.c_str());
    }

    // 6) 書き込みそのものが失敗する経路（出力先を書込拒否で掴む）。置換に失敗しても
    //    元ファイルは変わらず、変更記録が残り、掴みを外せば同じ store で保存できる。
    {
        const std::wstring path = MergeTestIniPath(L"stirhex_locked_test.ini");
        std::wstring error;
        SettingsStore seed;
        seed.Set(L"Env", L"Name", L"before");
        CHECK(SaveSettingsFile(path, seed, error), "seed written for the locked case");
        const std::string original = rawBytes(path);

        SettingsStore store;
        CHECK(LoadSettingsFile(path, store, error), "snapshot for the locked case");
        store.ClearDirty();
        store.Set(L"Env", L"Name", L"after");
        store.Set(L"Env", L"Extra", L"1");
        CHECK(store.Changes().size() == 2, "two changes are recorded");

        // 読み取りのみ許可＝置換・削除を拒否するハンドル。
        HANDLE hold = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(hold != INVALID_HANDLE_VALUE, "hold the settings file against replacement");
        if (hold != INVALID_HANDLE_VALUE) {
            const bool saved = SaveSettingsFileMerged(path, store, error);
            CHECK(!saved, "the save fails while the file cannot be replaced");
            CHECK(!error.empty(), "the failed save reports a reason");
            CHECK(store.Dirty(), "the failed save keeps the store dirty");
            CHECK(store.Changes().size() == 2, "the failed save keeps every change");
            ::CloseHandle(hold);
            CHECK(rawBytes(path) == original, "the settings file is untouched after a failed save");

            // 掴みを外して再保存すると、保留していた変更がすべて反映される。
            CHECK(SaveSettingsFileMerged(path, store, error), "retrying after the lock succeeds");
            CHECK(!store.Dirty(), "the retry clears the dirty flag");
            SettingsStore reread;
            CHECK(LoadSettingsFile(path, reread, error), "reload after the retry");
            const std::wstring* name = reread.Find(L"Env", L"Name");
            const std::wstring* extra = reread.Find(L"Env", L"Extra");
            CHECK(name != nullptr && *name == L"after", "the overwritten key has the new value");
            CHECK(extra != nullptr && *extra == L"1", "the added key is present");
        }

        // 一時ファイルを残さない（失敗した保存の後始末）。
        const std::wstring temp = path + L"." + std::to_wstring(::GetCurrentProcessId()) + L".tmp";
        CHECK(::GetFileAttributesW(temp.c_str()) == INVALID_FILE_ATTRIBUTES,
              "no temporary file is left behind");
        ::DeleteFileW(path.c_str());
    }

    // 7) 同一キーの競合と、削除→追加の順序が契約どおりに適用される。
    {
        const std::wstring path = MergeTestIniPath(L"stirhex_order_test.ini");
        std::wstring error;
        SettingsStore seed;
        seed.Set(L"Env", L"Shared", L"seed");
        seed.Set(L"Env", L"Doomed", L"1");
        CHECK(SaveSettingsFile(path, seed, error), "seed written for the ordering case");

        // 別プロセス相当が先に同じキーを更新する。
        SettingsStore other;
        CHECK(LoadSettingsFile(path, other, error), "other snapshot");
        other.ClearDirty();
        other.Set(L"Env", L"Shared", L"other");
        CHECK(SaveSettingsFileMerged(path, other, error), "the other process saves first");

        // 後から保存する側も同じキーを更新する（後勝ち）。あわせて削除→再追加を行う。
        SettingsStore mine;
        CHECK(LoadSettingsFile(path, mine, error), "my snapshot");
        mine.ClearDirty();
        mine.Set(L"Env", L"Shared", L"mine");
        mine.Remove(L"Env", L"Doomed");
        mine.Set(L"Env", L"Doomed", L"2");   // 削除の後に同じキーを足し直す
        CHECK(SaveSettingsFileMerged(path, mine, error), "my save");

        SettingsStore merged;
        CHECK(LoadSettingsFile(path, merged, error), "reload after the conflicting saves");
        const std::wstring* shared = merged.Find(L"Env", L"Shared");
        CHECK(shared != nullptr && *shared == L"mine", "the later save wins the conflicting key");
        const std::wstring* doomed = merged.Find(L"Env", L"Doomed");
        CHECK(doomed != nullptr && *doomed == L"2", "remove then set applies in order");

        // 逆順（追加→削除）ではキーが消える。
        SettingsStore last;
        CHECK(LoadSettingsFile(path, last, error), "snapshot for the reverse order");
        last.ClearDirty();
        last.Set(L"Env", L"Doomed", L"3");
        last.Remove(L"Env", L"Doomed");
        CHECK(SaveSettingsFileMerged(path, last, error), "reverse order save");
        SettingsStore after;
        CHECK(LoadSettingsFile(path, after, error), "reload after the reverse order save");
        CHECK(after.Find(L"Env", L"Doomed") == nullptr, "set then remove leaves the key gone");
        ::DeleteFileW(path.c_str());
    }
}

// エクスプローラで開くフォルダの決定（Issue #133）。相対 /ini パスの未作成ファイルでも
//   保存先（カレントディレクトリ）へ辿り着けること。
static void TestFolderToReveal() {
    TestPrintf("[TestFolderToReveal]\n");
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
    TestPrintf("TestSettingsStoreBinary\n");
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
    TestPrintf("TestCp932Text\n");
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
    TestPrintf("TestCp932LeadByte\n");
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
        char reason[96];
        std::snprintf(reason, sizeof(reason), "ACP=%u, IsDBCSLeadByte comparison is JP-only",
                      static_cast<unsigned int>(::GetACP()));
        SkipTest("TestCp932LeadByte/IsDBCSLeadByte", reason);
    }
}

// 構造体編集バーの char 配列文字列化（byte 層）。Unicode 文字セットの写像は CP932 固定。
static void TestFormatStructCharArrayCp932() {
    TestPrintf("[TestFormatStructCharArrayCp932]\n");
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
    TestPrintf("[TestFormatStructCharArrayW]\n");
    using stirling::FormatStructCharArrayCp932;
    using stirling::FormatStructCharArrayW;
    using stirling::WideFromCp932;

    // --- 0..2 / 4..5 は CP932 版をワイド化したものと同じ（非退行の担保） ---
    //   charset 3 は Issue #173 でワイド直接構築へ移行したため、CP932 で表せる範囲だけ
    //   従来と一致する（表せない文字は下の charset 3 の節で個別に確かめる）。
    struct Sample { int charset; std::vector<unsigned char> bytes; const char* what; };
    const Sample samples[] = {
        { 0, { 'A', 0x00, 'B', 0x7f, 'C' },        "ASCII" },
        { 1, { 0x82, 0xa0, 'A' },                  "SJIS" },
        { 2, { 0xa4, 0xa2 },                       "EUC main plane" },
        { 2, { 0x8e, 0xb1 },                       "EUC single shift" },
        { 3, { 0x42, 0x30, 'A', 0x00 },            "UTF-16LE within CP932" },
        { 4, { 0xc1, 0xc2, 0x40 },                 "EBCDIC" },
        { 5, { 0xc1, 0xc2, 0x40 },                 "EBCIDK" },
    };
    for (const Sample& sm : samples) {
        const int n = static_cast<int>(sm.bytes.size());
        const std::string mb = FormatStructCharArrayCp932(sm.charset, sm.bytes.data(), n);
        const std::wstring expect = WideFromCp932(mb.c_str(), static_cast<int>(mb.size()));
        CHECK(FormatStructCharArrayW(sm.charset, sm.bytes.data(), n) == expect, sm.what);
    }

    // --- charset 3 (Unicode / UTF-16LE。Issue #173) ---
    {   // CP932 に無い文字がそのまま残る（この Issue の主眼）
        const unsigned char hangul[] = { 0x5c, 0xd5 };            // U+D55C "한"
        CHECK(FormatStructCharArrayW(3, hangul, 2) == std::wstring(L"한"),
              "UTF-16 keeps characters outside CP932");
        // 参考: CP932 経由だと '.' に潰れていた
        const std::string mb = FormatStructCharArrayCp932(3, hangul, 2);
        CHECK(WideFromCp932(mb.c_str(), (int)mb.size()) != std::wstring(L"한"),
              "the CP932 route cannot represent it");
    }
    {   // ユーロ記号（従来の「変換不可」サンプル）も表示できる
        const unsigned char euro[] = { 0xac, 0x20 };              // U+20AC
        CHECK(FormatStructCharArrayW(3, euro, 2) == std::wstring(L"€"),
              "UTF-16 keeps the euro sign");
    }
    {   // サロゲートペアは 1 文字（2 コード単位）として復号される
        const unsigned char emoji[] = { 0x3d, 0xd8, 0x00, 0xde }; // U+1F600
        const std::wstring w = FormatStructCharArrayW(3, emoji, 4);
        CHECK(w.size() == 2, "surrogate pair stays a pair");
        CHECK(w[0] == 0xD83D && w[1] == 0xDE00, "surrogate pair values");
    }
    {   // ペアになっていないサロゲートと端数バイトは '.'
        const unsigned char lone[] = { 0x3d, 0xd8, 'A', 0x00 };   // 上位サロゲート + "A"
        CHECK(FormatStructCharArrayW(3, lone, 4) == std::wstring(L".A"),
              "unpaired high surrogate becomes a dot");
        const unsigned char lowOnly[] = { 0x00, 0xde };           // 下位サロゲート単独
        CHECK(FormatStructCharArrayW(3, lowOnly, 2) == std::wstring(L"."),
              "unpaired low surrogate becomes a dot");
        const unsigned char odd[] = { 0x41, 0x00, 0x42 };         // 末尾に端数 1 バイト
        CHECK(FormatStructCharArrayW(3, odd, 3) == std::wstring(L"A."),
              "odd trailing byte becomes a dot");
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
        // 対比: この文字は CP932 で表現できないため、CP932 を経由する経路では
        //   復元できない（かつてここは UTF-8 バイト列を charset=3（UTF-16LE）へ
        //   渡しており、別の文字コードとして解釈した結果を見ていた。Issue #178）。
        std::string cp932;
        CHECK(!stirling::Cp932FromWide(L"한", cp932),
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
    TestPrintf("[TestStructDefParse]\n");
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

// ---- 構造体スカラの値整形（Issue #178）------------------------------------
// FormatScalarValue / FormatScalarValueW は構造体編集バーの表示値そのものだが、
//   直接のテストが無かった。幅・符号・表示基数・バイトオーダの組合せを既知バイト列
//   との対応で固定する（analysis_artifacts/docs/18_struct_edit.md §6 の実測表）。
static void TestStructScalarFormat() {
    TestPrintf("[TestStructScalarFormat]\n");
    using stirling::FieldKind;
    using stirling::FormatScalarValue;
    using stirling::FormatScalarValueW;
    using stirling::kRadixDec1;
    using stirling::kRadixFloat;
    using stirling::kRadixHex;
    using stirling::kRadixSignedDec;

    struct Case {
        FieldKind kind;
        int size;
        std::vector<unsigned char> bytes;
        bool big;
        int radix;
        const char* expect;
        const char* what;
    };
    const std::vector<Case> cases = {
        // 1 バイト: char は符号拡張、byte は符号なし。基数 1 は 1・2 バイトでは 10 進のまま。
        { FieldKind::Char, 1, {0xFF}, false, kRadixSignedDec, "-1",   "char 0xFF is -1" },
        { FieldKind::Char, 1, {0xFF}, false, kRadixDec1,      "-1",   "char keeps its sign at radix 1" },
        { FieldKind::Char, 1, {0x80}, false, kRadixSignedDec, "-128", "char lower bound" },
        { FieldKind::Char, 1, {0x7F}, false, kRadixSignedDec, "127",  "char upper bound" },
        { FieldKind::Char, 1, {0xFF}, false, kRadixHex,       "0xFF", "char in hex is unsigned" },
        { FieldKind::Byte, 1, {0xFF}, false, kRadixDec1,      "255",  "byte 0xFF is 255" },
        { FieldKind::Byte, 1, {0x00}, false, kRadixHex,       "0x00", "byte hex is zero padded" },
        { FieldKind::Byte, 1, {0x0A}, false, kRadixHex,       "0x0A", "byte hex is upper case" },

        // 2 バイト: 同じ値を LE / BE の両方の並びで確認する。
        { FieldKind::Short, 2, {0x34, 0x12}, false, kRadixSignedDec, "4660", "short little endian" },
        { FieldKind::Short, 2, {0x12, 0x34}, true,  kRadixSignedDec, "4660", "short big endian" },
        { FieldKind::Short, 2, {0xFF, 0xFF}, false, kRadixSignedDec, "65535",
          "short is read unsigned at 2 bytes" },
        { FieldKind::Word,  2, {0xFF, 0xFF}, false, kRadixHex,       "0xFFFF", "WORD hex width" },
        { FieldKind::Word,  2, {0x00, 0x01}, false, kRadixDec1,      "256",    "word radix 1" },

        // 4 バイト: 基数 0 は符号付き再解釈、基数 1 は符号なし。
        { FieldKind::Long,  4, {0xFF, 0xFF, 0xFF, 0xFF}, false, kRadixSignedDec, "-1",
          "long is signed at radix 0" },
        { FieldKind::Long,  4, {0xFF, 0xFF, 0xFF, 0xFF}, false, kRadixDec1, "4294967295",
          "radix 1 is unsigned at 4 bytes" },
        { FieldKind::Long,  4, {0x00, 0x00, 0x00, 0x80}, false, kRadixSignedDec, "-2147483648",
          "long lower bound" },
        { FieldKind::Long,  4, {0xFF, 0xFF, 0xFF, 0x7F}, false, kRadixSignedDec, "2147483647",
          "long upper bound" },
        { FieldKind::Dword, 4, {0x00, 0x00, 0x00, 0x80}, false, kRadixDec1, "2147483648",
          "dword upper half" },
        { FieldKind::Dword, 4, {0x80, 0x00, 0x00, 0x00}, true,  kRadixDec1, "2147483648",
          "dword big endian" },
        { FieldKind::Dword, 4, {0x78, 0x56, 0x34, 0x12}, false, kRadixHex, "0x12345678",
          "dword hex little endian" },
        { FieldKind::Dword, 4, {0x12, 0x34, 0x56, 0x78}, true,  kRadixHex, "0x12345678",
          "dword hex big endian" },

        // 浮動小数は基数に依らず浮動小数表示（"%g"）。
        { FieldKind::Float,  4, {0x00, 0x00, 0x80, 0x3F}, false, kRadixHex, "1",
          "float ignores the radix override" },
        { FieldKind::Float,  4, {0x3F, 0x80, 0x00, 0x00}, true,  kRadixFloat, "1",
          "float big endian" },
        { FieldKind::Float,  4, {0x00, 0x00, 0x80, 0xBF}, false, kRadixFloat, "-1",
          "negative float" },
        { FieldKind::Float,  4, {0x00, 0x00, 0x00, 0x80}, false, kRadixFloat, "-0",
          "negative zero keeps its sign" },
        { FieldKind::Double, 8, {0, 0, 0, 0, 0, 0, 0xF0, 0x3F}, false, kRadixFloat, "1",
          "double little endian" },
        { FieldKind::Double, 8, {0x3F, 0xF0, 0, 0, 0, 0, 0, 0}, true, kRadixFloat, "1",
          "double big endian" },
        { FieldKind::Double, 8, {0x9C, 0x75, 0x00, 0x88, 0x3C, 0xE4, 0x37, 0x7E}, false,
          kRadixFloat, "1e+300", "double uses exponent notation for large values" },

        // 原は double を _gcvt へ 15 桁で渡す（FUN_00411b9e。Issue #215）。既定の %g は
        //   6 桁で下位が落ちるため、桁数まで固定する。float は原も "%-g"（6 桁）。
        { FieldKind::Double, 8, {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0}, false,
          kRadixFloat, "-4.88645965504377e+235", "double keeps 15 significant digits" },
        { FieldKind::Double, 8, {0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0xD5, 0x3F}, false,
          kRadixFloat, "0.333333333333333", "double shows the digits the original shows" },
        { FieldKind::Float,  4, {0xDB, 0x0F, 0x49, 0x40}, false, kRadixFloat, "3.14159",
          "float stays at 6 significant digits" },
    };
    for (const Case& c : cases) {
        const std::string got = FormatScalarValue(c.kind, c.size, c.bytes.data(), c.big, c.radix);
        CHECK(got == c.expect, c.what);
        if (got != c.expect) {
            TestPrintf("    got \"%s\" expected \"%s\"\n", got.c_str(), c.expect);
        }
        // ワイド版は narrow 版を 1 文字ずつ広げたものと一致する（数値表記は ASCII）。
        const std::wstring wide = FormatScalarValueW(c.kind, c.size, c.bytes.data(), c.big, c.radix);
        CHECK(wide == std::wstring(got.begin(), got.end()), c.what);
    }

    // 値を持たない種別は "?"（表示だけして編集させない）。
    {
        const unsigned char zero[8] = {0};
        CHECK(FormatScalarValue(FieldKind::Unknown, 1, zero, false, kRadixSignedDec) == "?",
              "unknown kind formats as a question mark");
        CHECK(FormatScalarValue(FieldKind::Struct, 1, zero, false, kRadixSignedDec) == "?",
              "a nested struct has no scalar value");
    }
}

// ---- 構造体スカラの符号化（Issue #178）------------------------------------
// EncodeScalar は編集した値をデータへ書き戻す唯一の経路。原版互換の受理条件
//   （"0x" 前置・先頭 0 の禁止・前後空白の禁止・幅レンジ）を、一般的な数値パーサの
//   期待値で上書きしないように固定する（§6）。
static void TestStructScalarEncode() {
    TestPrintf("[TestStructScalarEncode]\n");
    using stirling::EncodeScalar;
    using stirling::FieldKind;

    struct Ok {
        FieldKind kind; int size; const char* text; bool big;
        std::vector<unsigned char> expect; const char* what;
    };
    const std::vector<Ok> accepts = {
        // 10 進（符号あり・なし）と幅の下限・上限。
        { FieldKind::Byte,  1, "0",    false, {0x00}, "zero" },
        { FieldKind::Byte,  1, "255",  false, {0xFF}, "byte upper bound" },
        { FieldKind::Char,  1, "-1",   false, {0xFF}, "negative byte" },
        { FieldKind::Char,  1, "-128", false, {0x80}, "char lower bound" },
        { FieldKind::Short, 2, "-32768", false, {0x00, 0x80}, "short lower bound little endian" },
        { FieldKind::Short, 2, "-32768", true,  {0x80, 0x00}, "short lower bound big endian" },
        { FieldKind::Word,  2, "65535", false, {0xFF, 0xFF}, "word upper bound" },
        { FieldKind::Long,  4, "2147483647",  false, {0xFF, 0xFF, 0xFF, 0x7F}, "long upper bound" },
        { FieldKind::Long,  4, "-2147483648", false, {0x00, 0x00, 0x00, 0x80}, "long lower bound" },
        { FieldKind::Dword, 4, "4294967295",  false, {0xFF, 0xFF, 0xFF, 0xFF}, "dword upper bound" },
        { FieldKind::Dword, 4, "305419896",   true,  {0x12, 0x34, 0x56, 0x78}, "dword big endian" },
        // "0x" 前置の 16 進（符号なし）。
        { FieldKind::Byte,  1, "0xFF", false, {0xFF}, "hex byte" },
        { FieldKind::Byte,  1, "0xff", false, {0xFF}, "lower case hex" },
        { FieldKind::Byte,  1, "0X0a", false, {0x0A}, "upper case prefix" },
        { FieldKind::Dword, 4, "0x12345678", false, {0x78, 0x56, 0x34, 0x12}, "hex dword" },
        // 原版互換: "-" だけの入力も通過し、値 0 になる（一般的なパーサとは異なる）。
        { FieldKind::Char,  1, "-", false, {0x00}, "a bare minus is accepted as zero (faithful)" },
        // 原版互換: 幅レンジ検証は「上位 32bit < 1」かつ「下位 32bit が負 or 幅内」なので、
        //   負の範囲外は符号ビットが立っているだけで通り、下位バイトへ切り詰められる。
        //   正の範囲外（下の rejects）とは扱いが違う。一般的な数値パーサの感覚で
        //   reject 側へ動かさないよう、現仕様として固定する。
        { FieldKind::Char,  1, "-129",    false, {0x7F},       "a byte below range wraps (faithful)" },
        { FieldKind::Short, 2, "-32769",  false, {0xFF, 0x7F}, "a short below range wraps (faithful)" },
        { FieldKind::Short, 2, "-100000", false, {0x60, 0x79}, "far below range still wraps (faithful)" },
        // 浮動小数。指数は e/E/d/D の直後に必ず符号が要る。
        { FieldKind::Float,  4, "1.0",   false, {0x00, 0x00, 0x80, 0x3F}, "float 1.0" },
        { FieldKind::Float,  4, "1.0",   true,  {0x3F, 0x80, 0x00, 0x00}, "float big endian" },
        { FieldKind::Float,  4, "+1.0",  false, {0x00, 0x00, 0x80, 0x3F}, "leading plus" },
        { FieldKind::Float,  4, "-0.0",  false, {0x00, 0x00, 0x00, 0x80}, "negative zero keeps its sign" },
        { FieldKind::Float,  4, "1e+0",  false, {0x00, 0x00, 0x80, 0x3F}, "exponent with a sign" },
        { FieldKind::Double, 8, "1.0",   false, {0, 0, 0, 0, 0, 0, 0xF0, 0x3F}, "double 1.0" },
        { FieldKind::Double, 8, "1.0",   true,  {0x3F, 0xF0, 0, 0, 0, 0, 0, 0}, "double big endian" },
    };
    for (const Ok& a : accepts) {
        std::vector<unsigned char> out;
        const bool ok = EncodeScalar(a.kind, a.size, a.text, a.big, out);
        CHECK(ok, a.what);
        CHECK(out == a.expect, a.what);
        if (ok && out != a.expect) {
            TestPrintf("    \"%s\" encoded to %zu bytes, first %02X\n", a.text, out.size(),
                        out.empty() ? 0 : out[0]);
        }
    }

    struct Ng { FieldKind kind; int size; const char* text; const char* what; };
    const std::vector<Ng> rejects = {
        { FieldKind::Byte,  1, "",       "empty text" },
        { FieldKind::Byte,  1, " 1",     "a leading space is not trimmed" },
        { FieldKind::Byte,  1, "1 ",     "a trailing space is not trimmed" },
        { FieldKind::Byte,  1, "01",     "a leading zero followed by a digit is invalid" },
        { FieldKind::Byte,  1, "0x",     "a prefix without digits is invalid" },
        { FieldKind::Byte,  1, "0xZZ",   "non hex digits" },
        { FieldKind::Byte,  1, "+1",     "a leading plus is not a valid integer" },
        { FieldKind::Byte,  1, "1.5",    "an integer field rejects a decimal point" },
        { FieldKind::Byte,  1, "256",    "byte out of range" },
        { FieldKind::Byte,  1, "0x100",  "hex byte out of range" },
        { FieldKind::Short, 2, "65536",  "short out of range" },
        { FieldKind::Dword, 4, "4294967296",  "dword out of range" },
        { FieldKind::Dword, 4, "0x100000000", "hex dword out of range" },
        { FieldKind::Float, 4, "",       "empty float text" },
        { FieldKind::Float, 4, "abc",    "not a number" },
        { FieldKind::Float, 4, "1.2.3",  "two decimal points" },
        { FieldKind::Float, 4, "1e3",    "an exponent without a sign is invalid (faithful)" },
        { FieldKind::Float, 4, "+",      "a bare sign is invalid" },
    };
    for (const Ng& r : rejects) {
        std::vector<unsigned char> out;
        CHECK(!EncodeScalar(r.kind, r.size, r.text, false, out), r.what);
    }

    // 表示 → 編集 → 表示の往復（構造体バーの実際の使われ方）。
    {
        using stirling::FormatScalarValue;
        struct Trip { FieldKind kind; int size; int radix; const char* text; };
        const std::vector<Trip> trips = {
            { FieldKind::Char,  1, stirling::kRadixSignedDec, "-128" },
            { FieldKind::Byte,  1, stirling::kRadixHex,       "0xC3" },
            { FieldKind::Short, 2, stirling::kRadixSignedDec, "4660" },
            { FieldKind::Long,  4, stirling::kRadixSignedDec, "-2147483648" },
            { FieldKind::Dword, 4, stirling::kRadixDec1,      "4294967295" },
            { FieldKind::Float, 4, stirling::kRadixFloat,     "-1.5" },
            { FieldKind::Double, 8, stirling::kRadixFloat,    "2.5" },
        };
        for (const Trip& t : trips) {
            for (int pass = 0; pass < 2; ++pass) {
                const bool big = (pass == 1);
                std::vector<unsigned char> bytes;
                CHECK(EncodeScalar(t.kind, t.size, t.text, big, bytes), "round trip encode");
                if (bytes.size() != static_cast<size_t>(t.size)) { continue; }
                const std::string shown =
                    FormatScalarValue(t.kind, t.size, bytes.data(), big, t.radix);
                // どの入力もその基数での正規形なので、表示は入力文字列そのものへ戻る。
                CHECK(shown == std::string(t.text),
                      "the displayed value survives an edit round trip");
                if (shown != t.text) {
                    TestPrintf("    round trip: \"%s\" -> \"%s\"\n", t.text, shown.c_str());
                }
            }
        }
    }
}

// ---- 構造体のサイズ計算とツリー生成（Issue #178）--------------------------
// SizeOfStruct / BuildTree は構造体編集バーの「どこを何バイト読むか」を決める中核だが、
//   直接のテストが無かった。ネスト・1/2 次元配列のオフセットとサイズ、データ不足時の
//   "----"、基数の全体上書きを小さな定義文字列から確認する。
static void TestStructTree() {
    TestPrintf("[TestStructTree]\n");
    using stirling::FieldKind;
    using stirling::StructDefSet;
    using stirling::StructNode;

    const char* const kDef =
        "struct Inner { byte a; word b; };"
        "struct Outer { char c[3]; Inner in; long l; double d[2][2]; };";

    StructDefSet defs;
    std::wstring err;
    CHECK(defs.ParseText(kDef, &err), "nested definition parses");
    CHECK(err.empty(), "nested definition leaves err empty");
    const int inner = defs.FindByName("Inner");
    const int outer = defs.FindByName("Outer");
    CHECK(inner == 0 && outer == 1, "definitions are indexed in order");
    CHECK(defs.FindByName("Missing") == -1, "an unknown name is not found");

    // サイズ: Inner = 1 + 2 = 3、Outer = 3 + 3 + 4 + 8*2*2 = 42。
    CHECK(defs.SizeOfStruct(inner) == 3, "nested struct size");
    CHECK(defs.SizeOfStruct(outer) == 42, "outer size includes nesting and both array dimensions");
    CHECK(defs.SizeOfStruct(-1) == 0, "a negative index has no size");
    CHECK(defs.SizeOfStruct(99) == 0, "an out of range index has no size");

    // 十分な長さのデータでツリーを組む。
    std::vector<unsigned char> data(64);
    for (size_t i = 0; i < data.size(); ++i) { data[i] = static_cast<unsigned char>(i); }
    data[0] = 'A'; data[1] = 'B'; data[2] = 'C';

    StructNode root;
    defs.BuildTree(outer, data, false, 0 /* ASCII */, root);
    CHECK(root.hasChildren, "the root is a container");
    CHECK(root.children.size() == 4, "four top level fields");
    if (root.children.size() != 4) { return; }

    // 1) char 配列: コンテナに文字列表現、子は 1 バイトずつの葉。
    {
        const StructNode& c = root.children[0];
        CHECK(c.type == "char" && c.name == "c[3]", "char array container naming");
        CHECK(c.hasChildren && c.children.size() == 3, "char array has one child per element");
        CHECK(c.value == L"ABC", "char array container shows the text");
        CHECK(c.children[0].name == "[0]" && c.children[2].name == "[2]", "element naming");
        for (int i = 0; i < 3; ++i) {
            CHECK(c.children[i].offset == i, "char element offset");
            CHECK(c.children[i].size == 1, "char element size");
            CHECK(c.children[i].editable, "char element is editable");
            CHECK(c.children[i].kind == FieldKind::Char, "char element kind");
        }
    }
    // 2) ネスト構造体: 型名は定義名のまま、子のオフセットは親から連続する。
    {
        const StructNode& n = root.children[1];
        CHECK(n.type == "Inner" && n.name == "in", "nested struct naming");
        CHECK(n.hasChildren && n.children.size() == 2, "nested struct children");
        CHECK(!n.editable, "a container is not editable");
        CHECK(n.children[0].name == "a" && n.children[0].offset == 3 && n.children[0].size == 1,
              "nested first field");
        CHECK(n.children[1].name == "b" && n.children[1].offset == 4 && n.children[1].size == 2,
              "nested second field");
        CHECK(n.children[1].type == "WORD", "word is displayed with the standard type name");
    }
    // 3) スカラ: ネストの直後から続く。
    {
        const StructNode& l = root.children[2];
        CHECK(l.type == "long" && l.name == "l", "scalar naming");
        CHECK(!l.hasChildren && l.editable, "a scalar leaf is editable");
        CHECK(l.offset == 6 && l.size == 4, "scalar offset follows the nested struct");
        CHECK(l.value == stirling::FormatScalarValueW(FieldKind::Long, 4, &data[6], false,
                                                      stirling::kRadixSignedDec),
              "scalar value matches the formatter");
    }
    // 4) 2 次元配列: コンテナ → 行 → 要素の 3 階層。
    {
        const StructNode& d = root.children[3];
        CHECK(d.type == "double" && d.name == "d[2][2]", "2D array naming");
        CHECK(d.hasChildren && d.children.size() == 2, "one child per first dimension");
        if (d.children.size() == 2) {
            for (int i = 0; i < 2; ++i) {
                const StructNode& row = d.children[i];
                CHECK(row.name == (i == 0 ? "[0]" : "[1]"), "row naming");
                CHECK(row.hasChildren && row.children.size() == 2, "one child per second dimension");
                for (int j = 0; j < 2; ++j) {
                    const int want = 10 + (i * 2 + j) * 8;
                    CHECK(row.children[j].offset == want, "2D element offset");
                    CHECK(row.children[j].size == 8, "2D element size");
                }
            }
        }
    }

    // データが足りない範囲は "----" で編集不可（末尾で構造体が切れている場合）。
    {
        std::vector<unsigned char> shortData(8, 0x41);
        StructNode cut;
        defs.BuildTree(outer, shortData, false, 0, cut);
        CHECK(cut.children.size() == 4, "a short buffer still yields the whole shape");
        if (cut.children.size() == 4) {
            CHECK(cut.children[2].value == L"----", "a leaf past the end shows the filler");
            CHECK(!cut.children[2].editable, "a leaf past the end is not editable");
            CHECK(cut.children[2].offset == 6, "the offset is still reported");
            CHECK(cut.children[0].children[0].editable, "leaves inside the buffer stay editable");
            CHECK(cut.children[3].children[0].children[0].value == L"----",
                  "array elements past the end show the filler too");
        }
    }

    // 基数の全体上書き: 整数葉だけが 16 進になり、float/double は影響を受けない。
    {
        StructNode hex;
        defs.BuildTree(outer, data, false, 0, hex, stirling::kRadixHex);
        CHECK(hex.children.size() == 4, "override keeps the shape");
        if (hex.children.size() == 4) {
            CHECK(hex.children[2].radix == stirling::kRadixHex, "the override reaches the leaf");
            CHECK(hex.children[2].value.rfind(L"0x", 0) == 0, "integers use hex under the override");
            const StructNode& dbl = hex.children[3].children[0].children[0];
            CHECK(dbl.value.rfind(L"0x", 0) != 0, "float and double ignore the override");
        }
    }

    // 範囲外の defIndex は空のツリー（呼び出し側が def 未読込でも落ちない）。
    {
        StructNode empty;
        defs.BuildTree(99, data, false, 0, empty);
        CHECK(empty.children.empty(), "an unknown struct yields no children");
        defs.BuildTree(-1, data, false, 0, empty);
        CHECK(empty.children.empty(), "a negative index yields no children");
    }

    // バイトオーダの指定がツリーの値まで届く。
    {
        StructNode le, be;
        defs.BuildTree(inner, data, false, 0, le);
        defs.BuildTree(inner, data, true, 0, be);
        CHECK(le.children.size() == 2 && be.children.size() == 2, "inner tree shape");
        if (le.children.size() == 2 && be.children.size() == 2) {
            CHECK(le.children[0].value == be.children[0].value, "a 1 byte field has no byte order");
            CHECK(le.children[1].value != be.children[1].value,
                  "the 2 byte field differs between little and big endian");
        }
    }
}

// 16進テキストの寛容パーサ（Issue #97。クリップボードの16進テキスト貼り付け）。
//   受理形式・拒否条件と、失敗時に部分結果を返さないことを検証する。
static void TestHexTextParse() {
    TestPrintf("[TestHexTextParse]\n");
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

// UTF-16 の復号・符号化と持ち越し判定（Issue #173。キャラクターセット Unicode で
//   CP932 外の文字を表示するための土台）。文字欄の不変条件（1 ソースバイト = 1 表示
//   セル）を保つため、サロゲートペアと不正な単独サロゲートの扱いを重点的に確認する。
static void TestUtf16Text() {
    TestPrintf("[TestUtf16Text]\n");
    using stirling::DecodeUtf16;
    using stirling::EncodeUtf16;
    using stirling::Utf16CarryBytesAt;
    using stirling::Utf16FromWide;

    // --- BMP の 1 コード単位 ---
    {
        const unsigned char le[] = { 0x42, 0x30 };   // U+3042 "あ"（リトルエンディアン）
        const stirling::Utf16Decoded d = DecodeUtf16(le, 2, false);
        CHECK(d.ok && d.codePoint == 0x3042 && d.length == 2, "decodes little endian BMP");
        const unsigned char be[] = { 0x30, 0x42 };   // ビッグエンディアン
        const stirling::Utf16Decoded b = DecodeUtf16(be, 2, true);
        CHECK(b.ok && b.codePoint == 0x3042 && b.length == 2, "decodes big endian BMP");
    }
    {   // CP932 に無い文字も素通しする（この Issue の主眼）
        const unsigned char hangul[] = { 0x5c, 0xd5 };   // U+D55C
        const stirling::Utf16Decoded d = DecodeUtf16(hangul, 2, false);
        CHECK(d.ok && d.codePoint == 0xD55C, "decodes a character outside CP932");
    }

    // --- サロゲートペア ---
    {
        const unsigned char le[] = { 0x3d, 0xd8, 0x00, 0xde };   // U+1F600
        const stirling::Utf16Decoded d = DecodeUtf16(le, 4, false);
        CHECK(d.ok && d.codePoint == 0x1F600 && d.length == 4, "decodes a surrogate pair");
        const unsigned char be[] = { 0xd8, 0x3d, 0xde, 0x00 };
        const stirling::Utf16Decoded b = DecodeUtf16(be, 4, true);
        CHECK(b.ok && b.codePoint == 0x1F600 && b.length == 4, "decodes a big endian pair");
    }
    {   // 相方が無い上位サロゲートは不正（2 バイト消費）
        const unsigned char lone[] = { 0x3d, 0xd8, 0x41, 0x00 };
        const stirling::Utf16Decoded d = DecodeUtf16(lone, 4, false);
        CHECK(!d.ok && d.length == 2, "unpaired high surrogate is invalid");
    }
    {   // 下位サロゲート単独も不正
        const unsigned char low[] = { 0x00, 0xde };
        const stirling::Utf16Decoded d = DecodeUtf16(low, 2, false);
        CHECK(!d.ok && d.length == 2, "unpaired low surrogate is invalid");
    }
    {   // 上位サロゲートの直後でバッファが尽きたら truncated
        const unsigned char cut[] = { 0x3d, 0xd8 };
        const stirling::Utf16Decoded d = DecodeUtf16(cut, 2, false);
        CHECK(!d.ok && d.truncated && d.length == 2, "high surrogate at buffer end is truncated");
    }
    {   // 端数 1 バイトは 1 セルの不正
        const unsigned char odd[] = { 0x41 };
        const stirling::Utf16Decoded d = DecodeUtf16(odd, 1, false);
        CHECK(!d.ok && d.length == 1, "a single trailing byte is invalid");
        CHECK(!DecodeUtf16(nullptr, 0, false).ok, "nullptr is invalid");
    }

    // --- 符号化 ---
    {
        std::vector<unsigned char> out;
        CHECK(EncodeUtf16(0x3042, false, out) && out.size() == 2 &&
              out[0] == 0x42 && out[1] == 0x30, "encodes BMP little endian");
        out.clear();
        CHECK(EncodeUtf16(0x3042, true, out) && out[0] == 0x30 && out[1] == 0x42,
              "encodes BMP big endian");
        out.clear();
        CHECK(EncodeUtf16(0x1F600, false, out) && out.size() == 4 &&
              out[0] == 0x3d && out[1] == 0xd8 && out[2] == 0x00 && out[3] == 0xde,
              "encodes a surrogate pair");
        out.clear();
        CHECK(!EncodeUtf16(0xD800, false, out) && out.empty(), "refuses a lone surrogate");
        CHECK(!EncodeUtf16(0x110000, false, out), "refuses beyond U+10FFFF");
    }

    // --- ワイド文字列からの変換 ---
    {
        const wchar_t w[] = L"Aあ";
        const std::vector<unsigned char> b = Utf16FromWide(w, 2, false);
        CHECK(b.size() == 4 && b[0] == 'A' && b[1] == 0x00 && b[2] == 0x42 && b[3] == 0x30,
              "converts wide to little endian bytes");
        const wchar_t pair[] = { 0xD83D, 0xDE00, 0 };
        CHECK(Utf16FromWide(pair, 2, false).size() == 4, "keeps a surrogate pair");
        const wchar_t broken[] = { 0xD83D, L'A', 0 };
        const std::vector<unsigned char> bb = Utf16FromWide(broken, 2, false);
        CHECK(bb.size() == 2 && bb[0] == 'A', "drops an unpaired surrogate");
    }

    // --- 窓の先頭がサロゲートペアの途中か ---
    {
        // 手前 2 バイトが上位サロゲート、先頭 2 バイトが下位サロゲート → 2 バイト読み飛ばす
        const unsigned char win[] = { 0x3d, 0xd8, 0x00, 0xde };
        CHECK(Utf16CarryBytesAt(win, 4, 2, false) == 2, "carries over a split surrogate pair");
        // 手前が普通の文字なら持ち越さない
        const unsigned char plain[] = { 0x41, 0x00, 0x00, 0xde };
        CHECK(Utf16CarryBytesAt(plain, 4, 2, false) == 0, "no carry when the pair is broken");
        // 先頭が下位サロゲートでなければ持ち越さない
        const unsigned char normal[] = { 0x3d, 0xd8, 0x42, 0x30 };
        CHECK(Utf16CarryBytesAt(normal, 4, 2, false) == 0, "no carry for a normal unit");
        CHECK(Utf16CarryBytesAt(win, 4, 0, false) == 0, "no carry at the very start");
    }
    // --- 境界値（Issue #178: 復号・符号化とも両端を押さえる）---
    {
        struct Bmp { unsigned int cp; const char* what; };
        const Bmp bmp[] = {
            { 0x0000,  "U+0000" },
            { 0xD7FF,  "the code unit just below the surrogate range" },
            { 0xE000,  "the code unit just above the surrogate range" },
            { 0xFFFF,  "U+FFFF (the last BMP code point)" },
        };
        for (const Bmp& b : bmp) {
            unsigned char le[2] = { static_cast<unsigned char>(b.cp & 0xFF),
                                    static_cast<unsigned char>(b.cp >> 8) };
            const stirling::Utf16Decoded d = DecodeUtf16(le, 2, false);
            CHECK(d.ok && d.codePoint == b.cp && d.length == 2, b.what);
            unsigned char be[2] = { static_cast<unsigned char>(b.cp >> 8),
                                    static_cast<unsigned char>(b.cp & 0xFF) };
            const stirling::Utf16Decoded e = DecodeUtf16(be, 2, true);
            CHECK(e.ok && e.codePoint == b.cp && e.length == 2, b.what);
        }

        // サロゲート区間の両端は単独では不正（2 バイト = 2 セルのまま）。
        const unsigned int lone[] = { 0xD800, 0xDBFF, 0xDC00, 0xDFFF };
        for (unsigned int u : lone) {
            unsigned char le[2] = { static_cast<unsigned char>(u & 0xFF),
                                    static_cast<unsigned char>(u >> 8) };
            const stirling::Utf16Decoded d = DecodeUtf16(le, 2, false);
            CHECK(!d.ok && d.length == 2, "a lone surrogate at the range edge is invalid");
        }

        // 面外の両端: U+10000（最小）と U+10FFFF（最大）。
        struct Pair { unsigned int cp; unsigned int high; unsigned int low; const char* what; };
        const Pair pairs[] = {
            { 0x10000,  0xD800, 0xDC00, "U+10000 (the first supplementary code point)" },
            { 0x10FFFF, 0xDBFF, 0xDFFF, "U+10FFFF (the last code point)" },
        };
        for (const Pair& pr : pairs) {
            const unsigned char le[4] = {
                static_cast<unsigned char>(pr.high & 0xFF), static_cast<unsigned char>(pr.high >> 8),
                static_cast<unsigned char>(pr.low & 0xFF),  static_cast<unsigned char>(pr.low >> 8),
            };
            const stirling::Utf16Decoded d = DecodeUtf16(le, 4, false);
            CHECK(d.ok && d.codePoint == pr.cp && d.length == 4, pr.what);
            const unsigned char be[4] = {
                static_cast<unsigned char>(pr.high >> 8), static_cast<unsigned char>(pr.high & 0xFF),
                static_cast<unsigned char>(pr.low >> 8),  static_cast<unsigned char>(pr.low & 0xFF),
            };
            const stirling::Utf16Decoded e = DecodeUtf16(be, 4, true);
            CHECK(e.ok && e.codePoint == pr.cp && e.length == 4, pr.what);

            // 符号化は復号と往復する（LE / BE 両方）。
            std::vector<unsigned char> enc;
            CHECK(EncodeUtf16(pr.cp, false, enc) && enc.size() == 4, pr.what);
            CHECK(enc == std::vector<unsigned char>(le, le + 4), "little endian pair encoding");
            enc.clear();
            CHECK(EncodeUtf16(pr.cp, true, enc) && enc.size() == 4, pr.what);
            CHECK(enc == std::vector<unsigned char>(be, be + 4), "big endian pair encoding");
        }

        // 3 バイトでペアが切れている（下位サロゲートの片割れだけが窓に入っている）。
        const unsigned char cut3[] = { 0x3d, 0xd8, 0x00 };
        const stirling::Utf16Decoded d3 = DecodeUtf16(cut3, 3, false);
        CHECK(!d3.ok && d3.truncated && d3.length == 2,
              "a pair cut after three bytes is truncated, not invalid");
    }

    // --- BigEndian 側の符号化・ワイド変換・持ち越し ---
    {
        std::vector<unsigned char> out;
        CHECK(EncodeUtf16(0xFFFF, true, out) && out.size() == 2 &&
              out[0] == 0xFF && out[1] == 0xFF, "encodes U+FFFF big endian");

        // 既存の内容を持つ vector へは追記する（破壊せず末尾へ足す）。
        std::vector<unsigned char> acc = { 0xAA, 0xBB };
        CHECK(EncodeUtf16(0x3042, true, acc) && acc.size() == 4, "encoding appends");
        CHECK(acc[0] == 0xAA && acc[1] == 0xBB && acc[2] == 0x30 && acc[3] == 0x42,
              "the existing content is kept in front");
        // 不正なコードポイントでは既存部分も壊さない。
        const std::vector<unsigned char> before = acc;
        CHECK(!EncodeUtf16(0xDC00, true, acc), "a lone low surrogate is refused");
        CHECK(acc == before, "a refused encoding leaves the buffer untouched");
        CHECK(!EncodeUtf16(0x110000, true, acc), "beyond U+10FFFF is refused");
        CHECK(acc == before, "a refused range leaves the buffer untouched");

        // ワイド文字列 → BigEndian バイト列。
        const wchar_t w[] = { L'A', 0x3042, 0xD83D, 0xDE00, 0 };
        const std::vector<unsigned char> be = Utf16FromWide(w, 4, true);
        const std::vector<unsigned char> want = { 0x00, 0x41, 0x30, 0x42, 0xD8, 0x3D, 0xDE, 0x00 };
        CHECK(be == want, "converts wide to big endian bytes");
        const wchar_t broken[] = { 0xDE00, L'A', 0 };   // 下位サロゲート単独 + "A"
        const std::vector<unsigned char> bb = Utf16FromWide(broken, 2, true);
        CHECK(bb.size() == 2 && bb[0] == 0x00 && bb[1] == 'A',
              "an unpaired low surrogate is dropped in big endian too");
        CHECK(Utf16FromWide(nullptr, 4, true).empty(), "nullptr yields no bytes");
        CHECK(Utf16FromWide(w, 0, true).empty(), "length 0 yields no bytes");

        // 窓の先頭がペアの途中か（BigEndian）。
        const unsigned char win[] = { 0xd8, 0x3d, 0xde, 0x00 };
        CHECK(Utf16CarryBytesAt(win, 4, 2, true) == 2, "carries over a split pair in big endian");
        const unsigned char plain[] = { 0x00, 0x41, 0xde, 0x00 };
        CHECK(Utf16CarryBytesAt(plain, 4, 2, true) == 0, "no carry when the pair is broken (BE)");
        CHECK(Utf16CarryBytesAt(win, 4, 4, true) == 0, "no carry past the end of the buffer");
        CHECK(Utf16CarryBytesAt(nullptr, 4, 2, true) == 0, "nullptr never carries");
    }
}

// UTF-8 の復号・符号化と持ち越し判定（Issue #98。キャラクターセット UTF-8 対応）。
//   文字欄の不変条件（1 ソースバイト = 1 表示セル）を保つための土台なので、
//   「不正な列は 1 バイトずつ独立して扱う」ことを重点的に確認する。
static void TestUtf8Text() {
    TestPrintf("[TestUtf8Text]\n");
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

// クリップボード転送の RAII（Issue #47）のうち、メモリだけで完結する部分。
//   グローバルメモリの所有権移譲・解放・ロックと、引数不正の検証を扱う。
//   ここで呼ぶ転送 API は、いずれもクリップボードを開く前に戻る経路だけを通るため、
//   利用者のクリップボードには触れない。実転送は TestClipboardTransferOs へ分離した
//   （Issue #181）。
static void TestClipboardUtil() {
    TestPrintf("[TestClipboardUtil]\n");
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
}

// ---- OS 統合: 実クリップボードへの転送（Issue #181） ----
// 成功した転送は利用者のクリップボードの内容を置き換える。失敗させる検証も
// PutClipboardOwned が SetClipboardData の前に EmptyClipboard を通るため、
// 同じく外に副作用が出る。コアテストは「ビルドして走らせれば通る」軽量なスイートに
// 保ちたいので、ここは環境変数 STIRLING_CORE_TEST_OS=1 のときだけ実行する。
// 実行しなかった場合もスキップとして集計へ載せ、未検証であることを出力に残す。

// 読み出し検証のためにクリップボードを開く。他プロセスのロックは一時的なので短く待つ。
//   開けなかった場合は検証をスキップせず失敗として扱う（黙って通り抜けないため）。
static bool OpenClipboardForRead() {
    for (int i = 0; i < 20; ++i) {
        if (::OpenClipboard(nullptr)) { return true; }
        ::Sleep(50);
    }
    ++g_checks;
    ++g_failures;
    TestPrintf("  FAIL: could not open the clipboard for reading\n");
    return false;
}

static void TestClipboardTransferOs() {
    TestPrintf("[TestClipboardTransferOs]\n");
    const char* env = std::getenv("STIRLING_CORE_TEST_OS");
    if (env == nullptr || std::strcmp(env, "1") != 0) {
        SkipTest("TestClipboardTransferOs",
                 "set STIRLING_CORE_TEST_OS=1 to run (it replaces the clipboard)");
        return;
    }
    using ui::GlobalLockGuard;
    using ui::GlobalMemory;

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
    TestPrintf("[TestUndoBudget]\n");
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

// 入力（pos / count / 元サイズ）だけから、削除できるはずのバイト数を求める。
//   DeleteByte は pos が現在の総長未満のときだけ成功するので、pos が範囲内なら
//   min(count, size-pos)、範囲外なら 0 になる。実装の戻り値には依存しない。
static FileOffset ExpectedDeleteCount(size_t srcSize, FileOffset pos, FileOffset count) {
    if (count <= 0 || pos < 0) { return 0; }
    const FileOffset size = static_cast<FileOffset>(srcSize);
    if (pos >= size) { return 0; }
    const FileOffset avail = size - pos;
    return (count < avail) ? count : avail;
}

// DeleteRange と「DeleteByte の反復」を同一データへ適用し、削除数・内容・ブロック構造の
// すべてが一致することを確かめる（忠実性の担保）。線形参照モデルとも突き合わせる。
//   期待削除数は入力から独立に計算し、両実装ともその値と比較する。実装同士の比較だけだと
//   両方が同じ条件で早期終了しても検出できない（Issue #178）。
static void CheckDeleteRangeEquivalence(const std::vector<unsigned char>& src,
                                        FileOffset pos, FileOffset count, const char* where) {
    BlockList bulk;
    BlockList byByte;
    FillDoc(bulk, src);
    FillDoc(byByte, src);

    const FileOffset expect = ExpectedDeleteCount(src.size(), pos, count);

    FileOffset got = 0;
    {
        BlockCursor c(&bulk);
        got = c.DeleteRange(pos, count);
    }
    FileOffset byByteCount = 0;
    {
        BlockCursor c(&byByte);
        for (FileOffset i = 0; i < count; ++i) {
            unsigned char t = 0;
            if (!c.DeleteByte(pos, &t)) { break; }
            ++byByteCount;
        }
    }
    CHECK(got == expect, where);            // DeleteRange の削除数は独立した期待値と一致
    CHECK(byByteCount == expect, where);    // DeleteByte 反復も同じ独立した期待値と一致
    CHECK(BlockShape(bulk) == BlockShape(byByte), where);   // ブロック構造
    CHECK(ReadAll(bulk) == ReadAll(byByte), where);         // 内容

    // 線形参照モデル（std::vector）との突合。erase 幅も独立した期待値を使う。
    std::vector<unsigned char> ref = src;
    if (expect > 0) {
        ref.erase(ref.begin() + static_cast<size_t>(pos),
                  ref.begin() + static_cast<size_t>(pos + expect));
    }
    CheckEqual(bulk, ref, where);
    CheckInvariants(bulk, ref.size(), where);
}

static void TestDeleteRange() {
    TestPrintf("[TestDeleteRange]\n");
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
    TestPrintf("TestAllocFailureRollback\n");

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
    TestPrintf("TestWriteAndFillRange\n");

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

    // Write: 入力の境界値。FillRange 側には同等の確認があるが Write には無かった
    //   （Issue #183）。count<=0 と負の位置は実装が明示的に弾く契約（Read と違い
    //   ガードを持つ）なので、その契約をここで固定する。
    {
        std::vector<unsigned char> src(100, 0x3C);
        const std::vector<unsigned char> before = ReadAll(list);
        BlockCursor c(&list);
        CHECK(c.Write(0, src.data(), 0) == 0, "Write of zero length writes nothing");
        CHECK(c.Write(0, src.data(), -1) == 0, "Write rejects a negative count");
        CHECK(c.Write(-1, src.data(), 10) == 0, "Write rejects a negative position");
        // EOF ちょうど（Seek は追記位置として成功し、書ける範囲が無くて 0）と、
        //   その先（Seek 自体が失敗）は別経路なので両方見る。
        CHECK(c.Write(static_cast<FileOffset>(ref.size()), src.data(), 10) == 0,
              "Write at the EOF append position writes nothing");
        CHECK(c.Write(static_cast<FileOffset>(ref.size()) + 1, src.data(), 10) == 0,
              "Write past the end writes nothing");
        CHECK(ReadAll(list) == before, "a rejected Write leaves the data untouched");
        CHECK(list.GetTotalLength() == static_cast<FileOffset>(ref.size()),
              "a rejected Write keeps the total length");
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

// ---- 前方検索の範囲指定・長いパターン（Issue #178）----
// 前方検索はこれまで end=0（全長）だけを突き合わせており、有限の [start,end) と
//   データ／範囲より長いパターンが未検証だった。範囲の境界（末尾がちょうど end に
//   届く一致と、1 バイトはみ出す一致）を区別できることを固定する。
static void TestSearchForwardRange() {
    TestPrintf("TestSearchForwardRange\n");

    auto find = [](BlockCursor& c, const std::vector<unsigned char>& pat,
                   FileOffset start, FileOffset end) -> FileOffset {
        FileOffset pos = -1;
        const bool ok = c.SearchPattern(pat.data(), static_cast<int>(pat.size()), &pos,
                                        BlockCursor::kForward, start, end);
        return ok ? pos : -1;
    };

    // 1) 単一ブロック内の範囲境界。
    {
        std::vector<unsigned char> data(100, 0x00);
        const std::vector<unsigned char> pat = {0x11, 0x22, 0x33};
        for (size_t i = 0; i < pat.size(); ++i) { data[50 + i] = pat[i]; }
        BlockList list;
        BuildDoc(list, data);
        BlockCursor c(&list);

        CHECK(find(c, pat, 0, 53) == 50, "a match ending exactly at end is found");
        CHECK(find(c, pat, 0, 52) == -1, "a match overflowing end by one byte is not found");
        CHECK(find(c, pat, 0, 0) == 50, "end=0 means the whole document");
        CHECK(find(c, pat, 50, 53) == 50, "start exactly at the match");
        CHECK(find(c, pat, 51, 53) == -1, "start past the match");
        CHECK(find(c, pat, 0, 51) == -1, "a range too short for the pattern finds nothing");
    }

    // 2) ブロック境界（16KB）を跨ぐ一致でも同じ境界判定になる。
    {
        std::vector<unsigned char> data(40000);
        for (size_t i = 0; i < data.size(); ++i) {
            data[i] = static_cast<unsigned char>((i * 91) & 0xFF);
        }
        const std::vector<unsigned char> pat = {0xDE, 0xAD, 0xBE, 0xEF, 0x11, 0x22};
        const int at = kBlockCapacity - 3;      // 3 バイトが 1 ブロック目、残りが 2 ブロック目
        for (size_t i = 0; i < pat.size(); ++i) { data[at + i] = pat[i]; }
        BlockList list;
        BuildDoc(list, data);
        CHECK(list.Count() >= 3, "multi-block doc");
        BlockCursor c(&list);

        const FileOffset endOfMatch = at + static_cast<FileOffset>(pat.size());
        CHECK(find(c, pat, 0, endOfMatch) == at, "a cross-block match ending at end is found");
        CHECK(find(c, pat, 0, endOfMatch - 1) == -1, "one byte short of the cross-block match");
        CHECK(find(c, pat, at, endOfMatch) == at, "start at the cross-block match");
    }

    // 3) データ／範囲より長いパターンは見つからない（打ち切りの確認）。
    {
        std::vector<unsigned char> data(100, 0x41);
        BlockList list;
        BuildDoc(list, data);
        BlockCursor c(&list);
        const std::vector<unsigned char> tooLong(200, 0x41);
        CHECK(find(c, tooLong, 0, 0) == -1, "a pattern longer than the document is not found");
        const std::vector<unsigned char> pat6(6, 0x41);
        CHECK(find(c, pat6, 0, 5) == -1, "a pattern longer than the range is not found");
        CHECK(find(c, pat6, 0, 6) == 0, "a pattern exactly filling the range is found");
    }

    // 4) 重なり合う一致は「次の 1 バイトから」順に返る。
    {
        const std::vector<unsigned char> data(64, 0x41);   // "AAAA..."
        const std::vector<unsigned char> pat = {0x41, 0x41, 0x41};
        BlockList list;
        BuildDoc(list, data);
        BlockCursor c(&list);
        CHECK(find(c, pat, 0, 0) == 0, "first overlapping match");
        CHECK(find(c, pat, 1, 0) == 1, "the next overlapping match starts one byte later");
        CHECK(find(c, pat, 61, 0) == 61, "the last overlapping match");
        CHECK(find(c, pat, 62, 0) == -1, "no room for a match near the end");
    }
}

// ---- 複合編集ファズ（Issue #178）----
// 既存の編集ファズは InsertByte / DeleteByte だけを使っていた。ここでは範囲操作
//   （Insert / DeleteRange / Write / FillRange）と読み取り（Read / GetByteAt / SearchPattern）を
//   同じカーソルで混ぜて使い続け、線形参照モデル（std::vector）と突き合わせる。
static void TestEditFuzzMixed() {
    TestPrintf("TestEditFuzzMixed\n");
    BlockList list;
    NewEmptyDoc(list);
    BlockCursor c(&list);          // 同じカーソルを最後まで使い回す
    std::vector<unsigned char> ref;

    std::mt19937 rng(0xBADCAFE);
    const int kOps = 6000;
    int mismatchAt = -1;
    int readMismatch = 0, byteMismatch = 0, searchMismatch = 0;
    int opCount[6] = {0, 0, 0, 0, 0, 0};

    for (int op = 0; op < kOps && mismatchAt < 0; ++op) {
        const int size = static_cast<int>(ref.size());
        // データが小さいうちは挿入へ寄せ、複数ブロックまで育ててから他の操作を混ぜる。
        int kind = static_cast<int>(rng() % 6);
        if (size < 64) { kind = static_cast<int>(rng() % 2); }

        switch (kind) {
        case 0: {   // InsertByte
            const int pos = static_cast<int>(rng() % (size + 1));
            const unsigned char v = static_cast<unsigned char>(rng() & 0xFF);
            if (!c.InsertByte(pos, v)) { mismatchAt = op; break; }
            ref.insert(ref.begin() + pos, v);
            ++opCount[0];
            break;
        }
        case 1: {   // Insert（範囲）
            const int pos = static_cast<int>(rng() % (size + 1));
            const int n = 1 + static_cast<int>(rng() % 5000);
            std::vector<unsigned char> src(n);
            for (int i = 0; i < n; ++i) { src[i] = static_cast<unsigned char>(rng() & 0xFF); }
            if (!c.Insert(pos, src.data(), n)) { mismatchAt = op; break; }
            ref.insert(ref.begin() + pos, src.begin(), src.end());
            ++opCount[1];
            break;
        }
        case 2: {   // DeleteByte
            const int pos = static_cast<int>(rng() % size);
            unsigned char got = 0;
            if (!c.DeleteByte(pos, &got)) { mismatchAt = op; break; }
            if (got != ref[pos]) { mismatchAt = op; break; }
            ref.erase(ref.begin() + pos);
            ++opCount[2];
            break;
        }
        case 3: {   // DeleteRange（期待削除数は入力から独立に求める）
            const int pos = static_cast<int>(rng() % (size + 1));
            const int n = 1 + static_cast<int>(rng() % 4000);
            const FileOffset want = ExpectedDeleteCount(ref.size(), pos, n);
            const FileOffset got = c.DeleteRange(pos, n);
            if (got != want) { mismatchAt = op; break; }
            if (want > 0) {
                ref.erase(ref.begin() + pos, ref.begin() + pos + static_cast<size_t>(want));
            }
            ++opCount[3];
            break;
        }
        case 4: {   // Write（長さ不変の上書き。末尾を越える分は打ち切られる）
            const int pos = static_cast<int>(rng() % size);
            const int n = 1 + static_cast<int>(rng() % 4000);
            std::vector<unsigned char> src(n);
            for (int i = 0; i < n; ++i) { src[i] = static_cast<unsigned char>(rng() & 0xFF); }
            const FileOffset want = (n < size - pos) ? n : (size - pos);
            const FileOffset got = c.Write(pos, src.data(), n);
            if (got != want) { mismatchAt = op; break; }
            for (FileOffset i = 0; i < want; ++i) {
                ref[static_cast<size_t>(pos) + static_cast<size_t>(i)] =
                    src[static_cast<size_t>(i)];
            }
            ++opCount[4];
            break;
        }
        default: {  // FillRange（同上）
            const int pos = static_cast<int>(rng() % size);
            const int n = 1 + static_cast<int>(rng() % 4000);
            const unsigned char v = static_cast<unsigned char>(rng() & 0xFF);
            const FileOffset want = (n < size - pos) ? n : (size - pos);
            const FileOffset got = c.FillRange(pos, n, v);
            if (got != want) { mismatchAt = op; break; }
            for (FileOffset i = 0; i < want; ++i) {
                ref[static_cast<size_t>(pos) + static_cast<size_t>(i)] = v;
            }
            ++opCount[5];
            break;
        }
        }
        if (mismatchAt >= 0) { break; }

        // 読み取り系を同じカーソルで挟む（編集で位置が壊れていないこと）。
        //   1 バイト読みは GetByteAt（絶対位置キャッシュ経由の増分アクセス）と
        //   Seek+Read（CStirlingDoc::GetByteAt と同じ経路）の両方で行い、
        //   編集を挟んでも両者が一致することを見る（Issue #182 の回帰）。
        if (!ref.empty()) {
            const size_t at = rng() % ref.size();
            unsigned char b = 0;
            if (!c.GetByteAt(static_cast<FileOffset>(at), &b) || b != ref[at]) {
                ++byteMismatch;
            }
            unsigned char b2 = 0;
            if (!c.Seek(static_cast<FileOffset>(at), BlockCursor::kBegin, nullptr) ||
                c.Read(1, &b2) != 1 || b2 != ref[at]) {
                ++byteMismatch;
            }

            const size_t from = rng() % ref.size();
            const size_t len = 1 + rng() % (ref.size() - from > 300 ? 300 : ref.size() - from);
            std::vector<unsigned char> buf(len);
            if (c.Seek(static_cast<FileOffset>(from), BlockCursor::kBegin, nullptr)) {
                const FileOffset n = c.Read(static_cast<FileOffset>(len), buf.data());
                if (n != static_cast<FileOffset>(len) ||
                    std::memcmp(buf.data(), ref.data() + from, len) != 0) {
                    ++readMismatch;
                }
            } else {
                ++readMismatch;
            }
        }
        // 実在する 3 バイト列の検索が参照モデルと一致する。
        if (ref.size() >= 8 && (op % 37) == 0) {
            const size_t src = rng() % (ref.size() - 3);
            const std::vector<unsigned char> pat(ref.begin() + src, ref.begin() + src + 3);
            const int expect = NaiveForward(ref, pat, 0, static_cast<int>(ref.size()));
            FileOffset pos = -1;
            const bool ok = c.SearchPattern(pat.data(), 3, &pos, BlockCursor::kForward, 0, 0);
            if ((ok ? pos : -1) != expect) { ++searchMismatch; }
        }

        // 全突合は高コストなので周期的に実施。
        if ((op % 500) == 499) {
            const std::vector<unsigned char> got = ReadAll(list);
            if (got != ref) { mismatchAt = op; }
        }
    }

    CHECK(mismatchAt == -1, "mixed edit fuzz stays consistent with the reference model");
    if (mismatchAt >= 0) { TestPrintf("  first divergence at op=%d\n", mismatchAt); }
    CHECK(readMismatch == 0, "Read after edits matches the reference model");
    CHECK(byteMismatch == 0, "single byte reads after edits match the reference model");
    CHECK(searchMismatch == 0, "SearchPattern after edits matches the reference model");
    CheckEqual(list, ref, "mixed edit fuzz final");
    CheckInvariants(list, ref.size(), "mixed edit fuzz final");
    CHECK(list.Count() >= 3, "mixed edit fuzz exercised multiple blocks");
    for (int i = 0; i < 6; ++i) {
        CHECK(opCount[i] > 0, "every mixed operation was exercised");
    }
    TestPrintf("  mixed fuzz: %zu bytes, %d blocks, ops ib=%d in=%d db=%d dr=%d wr=%d fi=%d\n",
                ref.size(), list.Count(), opCount[0], opCount[1], opCount[2], opCount[3],
                opCount[4], opCount[5]);
}

// ---- 編集後の GetByteAt（Issue #182 回帰）----
// GetByteAt は絶対位置キャッシュ curAbs_ からの増分移動で高速化しているが、
//   編集プリミティブが curNode_/curOffset_ だけを動かすとキャッシュと食い違い、
//   誤った値や false を返していた。Write/FillRange は逆に、カーソルを動かさないまま
//   curAbs_ だけを pos+written へ進めていた。各操作の直後に GetByteAt が
//   Seek+Read と一致することを、決定的なケースで固定する。
static void TestCursorAbsCacheAfterEdit() {
    TestPrintf("TestCursorAbsCacheAfterEdit\n");

    // 同じカーソルで全位置を GetByteAt し、参照モデルと突き合わせる。
    auto verifyAll = [](BlockCursor& c, const std::vector<unsigned char>& ref, const char* where) {
        int bad = 0;
        for (size_t i = 0; i < ref.size(); ++i) {
            unsigned char b = 0;
            if (!c.GetByteAt(static_cast<FileOffset>(i), &b) || b != ref[i]) {
                if (bad == 0) {
                    TestPrintf("    first mismatch at %zu (%s)\n", i, where);
                }
                ++bad;
            }
        }
        CHECK(bad == 0, where);
        // 末尾の 1 つ先は実データ外なので必ず false。
        unsigned char eof = 0;
        CHECK(!c.GetByteAt(static_cast<FileOffset>(ref.size()), &eof), where);
    };

    // 1) Issue に記載した最小再現: InsertByte を繰り返した直後の GetByteAt。
    {
        BlockList list;
        NewEmptyDoc(list);
        BlockCursor c(&list);
        std::vector<unsigned char> ref;
        for (int i = 0; i < 100; ++i) {
            const unsigned char v = static_cast<unsigned char>(i);
            CHECK(c.InsertByte(i, v), "seed InsertByte");
            ref.push_back(v);
        }
        unsigned char first = 0;
        CHECK(c.GetByteAt(0, &first), "GetByteAt(0) right after InsertByte succeeds");
        CHECK(first == ref[0], "GetByteAt(0) right after InsertByte reads the right byte");
        verifyAll(c, ref, "GetByteAt after repeated InsertByte");
    }

    // 2) ブロック分割を伴う InsertByte。分割は挿入位置が前半か後半かで新ブロックを
    //    手前(LinkNodeBefore)へ入れるか後ろ(LinkNodeAfter)へ入れるかが変わるので、
    //    両方の分岐を通す。
    for (int at : {8000, 12000}) {
        BlockList list;
        NewEmptyDoc(list);
        BlockCursor c(&list);
        std::vector<unsigned char> ref;
        FillFullBlock(list, c, ref);
        CHECK(c.InsertByte(at, 0xEE), "insert into a full block");
        ref.insert(ref.begin() + at, 0xEE);
        CHECK(list.Count() == 2, "the block split");
        unsigned char b = 0;
        CHECK(c.GetByteAt(at, &b) && b == 0xEE, "GetByteAt at the split point");
        CHECK(c.GetByteAt(0, &b) && b == ref[0], "GetByteAt backward across the split");
        CHECK(c.GetByteAt(16000, &b) && b == ref[16000], "GetByteAt forward across the split");
    }

    // 2b) 未シークのカーソルでも GetByteAt は Seek+Read と同じ結果になる
    //     （増分アクセスの起点が無いときは先頭から解決し直す）。
    {
        BlockList list;
        NewEmptyDoc(list);
        {
            BlockCursor seeder(&list);
            std::vector<unsigned char> data(40000);
            for (size_t i = 0; i < data.size(); ++i) {
                data[i] = static_cast<unsigned char>((i * 29 + 11) & 0xFF);
            }
            CHECK(seeder.Insert(0, data.data(), static_cast<FileOffset>(data.size())),
                  "seed for the unseeked cursor case");
        }
        const std::vector<unsigned char> ref = ReadAll(list);
        BlockCursor fresh(&list);   // 一度も Seek していないカーソル
        unsigned char b = 0;
        CHECK(fresh.GetByteAt(30000, &b) && b == ref[30000],
              "an unseeked cursor resolves the position itself");
        CHECK(fresh.GetByteAt(0, &b) && b == ref[0], "and stays usable afterwards");
    }

    // 3) 範囲挿入・DeleteByte・DeleteRange の直後。
    {
        std::vector<unsigned char> src(40000);
        for (size_t i = 0; i < src.size(); ++i) {
            src[i] = static_cast<unsigned char>((i * 31 + 7) & 0xFF);
        }
        {   // Insert（複数ブロックにまたがる範囲挿入）
            BlockList list;
            FillDoc(list, src);
            BlockCursor c(&list);
            std::vector<unsigned char> add(20000, 0xA5);
            CHECK(c.Insert(5000, add.data(), static_cast<FileOffset>(add.size())), "range insert");
            std::vector<unsigned char> ref = src;
            ref.insert(ref.begin() + 5000, add.begin(), add.end());
            unsigned char b = 0;
            CHECK(c.GetByteAt(5000, &b) && b == 0xA5, "GetByteAt at the insert point");
            CHECK(c.GetByteAt(0, &b) && b == ref[0], "GetByteAt at the head after Insert");
            CHECK(c.GetByteAt(30000, &b) && b == ref[30000], "GetByteAt far after Insert");
        }
        {   // DeleteByte
            BlockList list;
            FillDoc(list, src);
            BlockCursor c(&list);
            unsigned char removed = 0;
            CHECK(c.DeleteByte(20000, &removed), "delete one byte");
            CHECK(removed == src[20000], "the removed byte");
            std::vector<unsigned char> ref = src;
            ref.erase(ref.begin() + 20000);
            unsigned char b = 0;
            CHECK(c.GetByteAt(20000, &b) && b == ref[20000], "GetByteAt at the delete point");
            CHECK(c.GetByteAt(0, &b) && b == ref[0], "GetByteAt at the head after DeleteByte");
        }
        {   // DeleteRange（ノードごと除去される長さ）
            BlockList list;
            FillDoc(list, src);
            BlockCursor c(&list);
            CHECK(c.DeleteRange(100, 20000) == 20000, "range delete");
            std::vector<unsigned char> ref = src;
            ref.erase(ref.begin() + 100, ref.begin() + 20100);
            unsigned char b = 0;
            CHECK(c.GetByteAt(100, &b) && b == ref[100], "GetByteAt at the delete point");
            CHECK(c.GetByteAt(0, &b) && b == ref[0], "GetByteAt at the head after DeleteRange");
        }
        {   // DeleteByte でノードごとリストから外れる経路（usedLen==1 のノードを削除）。
            //   カーソルは別ノードへ張り替えられるため、キャッシュとの食い違いが起きやすい。
            BlockList list;
            const std::vector<std::vector<unsigned char>> blocks = {
                {0x10, 0x11, 0x12}, {0x20}, {0x30, 0x31},
            };
            std::vector<unsigned char> ref;
            for (const std::vector<unsigned char>& blk : blocks) {
                unsigned char* buf = new unsigned char[kBlockCapacity];
                std::memset(buf, 0, kBlockCapacity);
                std::memcpy(buf, blk.data(), blk.size());
                list.AppendBlock(buf, kBlockCapacity, static_cast<int>(blk.size()));
                ref.insert(ref.end(), blk.begin(), blk.end());
            }
            CHECK(list.Count() == 3, "three nodes before the delete");
            BlockCursor c(&list);
            unsigned char removed = 0;
            CHECK(c.DeleteByte(3, &removed) && removed == 0x20, "delete the only byte of a node");
            ref.erase(ref.begin() + 3);
            CHECK(list.Count() == 2, "the emptied node is removed from the list");
            verifyAll(c, ref, "GetByteAt after a node was unlinked");
        }
    }

    // 4) 長さを変えない書き換え（Write / FillRange / SetByteAt）の直後。
    //    以前は Write/FillRange が curNode_/curOffset_ を動かさないまま
    //    curAbs_ だけを pos+written へ進めており、直後の GetByteAt が
    //    「進めた分だけ手前」のバイトを返していた。
    {
        std::vector<unsigned char> src(40000);
        for (size_t i = 0; i < src.size(); ++i) {
            src[i] = static_cast<unsigned char>((i * 17 + 3) & 0xFF);
        }
        {   // Write
            BlockList list;
            FillDoc(list, src);
            BlockCursor c(&list);
            std::vector<unsigned char> patch(5000);
            for (size_t i = 0; i < patch.size(); ++i) {
                patch[i] = static_cast<unsigned char>(0xC0 + (i % 16));
            }
            const FileOffset n = c.Write(10000, patch.data(), static_cast<FileOffset>(patch.size()));
            CHECK(n == static_cast<FileOffset>(patch.size()), "write count");
            std::vector<unsigned char> ref = src;
            std::copy(patch.begin(), patch.end(), ref.begin() + 10000);
            unsigned char b = 0;
            // pos + written の位置（旧実装が curAbs_ を進めていた先）を最初に読む。
            CHECK(c.GetByteAt(15000, &b) && b == ref[15000], "GetByteAt at pos+written after Write");
            CHECK(c.GetByteAt(10000, &b) && b == ref[10000], "GetByteAt at the write start");
            CHECK(c.GetByteAt(0, &b) && b == ref[0], "GetByteAt at the head after Write");
        }
        {   // FillRange
            BlockList list;
            FillDoc(list, src);
            BlockCursor c(&list);
            const FileOffset n = c.FillRange(10000, 5000, 0x5A);
            CHECK(n == 5000, "fill count");
            std::vector<unsigned char> ref = src;
            std::fill(ref.begin() + 10000, ref.begin() + 15000, static_cast<unsigned char>(0x5A));
            unsigned char b = 0;
            CHECK(c.GetByteAt(15000, &b) && b == ref[15000],
                  "GetByteAt at pos+filled after FillRange");
            CHECK(c.GetByteAt(12000, &b) && b == 0x5A, "GetByteAt inside the filled range");
            CHECK(c.GetByteAt(0, &b) && b == ref[0], "GetByteAt at the head after FillRange");
        }
        {   // SetByteAt
            BlockList list;
            FillDoc(list, src);
            BlockCursor c(&list);
            CHECK(c.SetByteAt(20000, 0x99), "set one byte");
            unsigned char b = 0;
            CHECK(c.GetByteAt(20000, &b) && b == 0x99, "GetByteAt at the overwritten byte");
            CHECK(c.GetByteAt(0, &b) && b == src[0], "GetByteAt at the head after SetByteAt");
        }
    }

    // 5) 編集を挟んでも検索が正しく動く（走査は開始時にキャッシュを張り直す）。
    {
        std::vector<unsigned char> ref(30000);
        for (size_t i = 0; i < ref.size(); ++i) {
            ref[i] = static_cast<unsigned char>((i * 13 + 5) & 0xFF);
        }
        BlockList list;
        FillDoc(list, ref);
        BlockCursor c(&list);

        const std::vector<unsigned char> pat = {0xDE, 0xAD, 0xBE, 0xEF};
        CHECK(c.Insert(12345, pat.data(), static_cast<FileOffset>(pat.size())), "insert a pattern");
        ref.insert(ref.begin() + 12345, pat.begin(), pat.end());

        FileOffset pos = -1;
        CHECK(c.SearchPattern(pat.data(), static_cast<int>(pat.size()), &pos,
                              BlockCursor::kForward, 0, 0),
              "search right after an insert");
        CHECK(pos == 12345, "the search finds the inserted pattern");

        // 検索の後も GetByteAt が正しい（走査で進んだキャッシュから戻れる）。
        unsigned char b = 0;
        CHECK(c.GetByteAt(0, &b) && b == ref[0], "GetByteAt at the head after a search");
        CHECK(c.GetByteAt(29000, &b) && b == ref[29000], "GetByteAt far after a search");
    }

    // 6) 空ドキュメント・データ外は false のまま（増分アクセスの再解決で穴を開けない）。
    {
        BlockList list;
        NewEmptyDoc(list);
        BlockCursor c(&list);
        unsigned char b = 0;
        CHECK(!c.GetByteAt(0, &b), "an empty document has no byte at 0");
        CHECK(c.InsertByte(0, 0x42), "insert one byte");
        CHECK(c.GetByteAt(0, &b) && b == 0x42, "the only byte is readable");
        CHECK(!c.GetByteAt(1, &b), "one past the end is not readable");
        CHECK(!c.GetByteAt(-1, &b), "a negative position is not readable");
        unsigned char removed = 0;
        CHECK(c.DeleteByte(0, &removed) && removed == 0x42, "delete the only byte");
        CHECK(!c.GetByteAt(0, &b), "an emptied document has no byte at 0");
    }

    // 7) 相対シーク（kCurrent / kEnd）の直後。これらは原の逐語移植で絶対位置が
    //    確定しない経路があるためキャッシュを無効化しており、次の GetByteAt が
    //    自力で解決し直せることを確認する。
    {
        std::vector<unsigned char> ref(40000);
        for (size_t i = 0; i < ref.size(); ++i) {
            ref[i] = static_cast<unsigned char>((i * 23 + 9) & 0xFF);
        }
        const FileOffset total = static_cast<FileOffset>(ref.size());

        {   // kCurrent 前方・後方
            BlockList list;
            FillDoc(list, ref);
            BlockCursor c(&list);
            unsigned char b = 0;
            CHECK(c.Seek(1000, BlockCursor::kBegin, nullptr), "seek to a known position");
            CHECK(c.Seek(20000, BlockCursor::kCurrent, nullptr), "relative seek forward");
            CHECK(c.GetByteAt(30000, &b) && b == ref[30000], "GetByteAt after a forward kCurrent");
            CHECK(c.Seek(-5000, BlockCursor::kCurrent, nullptr), "relative seek backward");
            CHECK(c.GetByteAt(100, &b) && b == ref[100], "GetByteAt after a backward kCurrent");
        }
        {   // kEnd
            BlockList list;
            FillDoc(list, ref);
            BlockCursor c(&list);
            unsigned char b = 0;
            CHECK(c.Seek(-1000, BlockCursor::kEnd, nullptr), "seek from the end");
            CHECK(c.GetByteAt(total - 1, &b) && b == ref[ref.size() - 1],
                  "GetByteAt after a kEnd seek");
            CHECK(c.GetByteAt(0, &b) && b == ref[0], "GetByteAt at the head after a kEnd seek");
        }
    }
}

// ---- StreamFileWriter: 一時ファイル経由の逐次書込（Issue #155） ----
// 選択範囲の保存・ダンプ保存は、書き終えてから出力先を置換する。途中で失敗しても
// 既存ファイルが空や不完全な内容に置き換わらないことを確認する。
static void TestStreamFileWriter() {
    TestPrintf("TestStreamFileWriter\n");
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
        const std::vector<std::wstring> before = DirEntryNames(out.parent_path());
        {
            StreamFileWriter w;
            CHECK(w.Open(out.wstring().c_str()).Ok(), "open for abort");
            const std::string partial = "partial";
            CHECK(w.Write(partial.data(), partial.size()).Ok(), "write partial data");
            w.Abort();
            CHECK(!w.IsOpen(), "writer is closed after abort");
        }
        CHECK(ReadFileBytes(out) == orig, "aborting leaves the existing file untouched");
        const std::vector<std::wstring> after = DirEntryNames(out.parent_path());
        CHECK(after == before, "aborting leaves no temporary file behind");
        fs::remove(out);
    }

    // 4) デストラクタでも一時ファイルを片付ける（Commit を呼ばずに抜けた場合）。
    {
        const fs::path out = TempFile("sfw_dtor");
        const std::vector<unsigned char> orig = { 'k', 'e', 'e', 'p', '2' };
        WriteFile(out, orig);
        const std::vector<std::wstring> before = DirEntryNames(out.parent_path());
        {
            StreamFileWriter w;
            CHECK(w.Open(out.wstring().c_str()).Ok(), "open for the destructor case");
            const std::string partial = "partial";
            CHECK(w.Write(partial.data(), partial.size()).Ok(), "write partial data");
        }   // ここでデストラクタ＝Abort 相当
        CHECK(ReadFileBytes(out) == orig, "the destructor leaves the existing file untouched");
        const std::vector<std::wstring> after = DirEntryNames(out.parent_path());
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

    // 8) 明示フラッシュ（Issue #166）を挟んだ後の Commit 正常系の回帰。成功を返した直後に
    //    報告サイズ・実サイズ・内容が一致し、一時ファイルも残らないことを確認する。
    //    フラッシュ呼出の有無・失敗パスは障害注入なしでは検証できない（上の flush_save 参照）。
    {
        const fs::path out = TempFile("sfw_flush");
        const std::vector<unsigned char> orig(30000, 0xAB);
        WriteFile(out, orig);
        const std::vector<std::wstring> before = DirEntryNames(out.parent_path());
        StreamFileWriter w;
        CHECK(w.Open(out.wstring().c_str()).Ok(), "open for the flush check");
        const std::string body = "committed-after-flush";
        CHECK(w.Write(body.data(), body.size()).Ok(), "write before commit");
        const stirling::FileIoResult r = w.Commit();
        CHECK(r.Ok(), "commit reports success after the explicit flush");
        CHECK(r.fileSize == static_cast<FileOffset>(body.size()), "commit reports the written size");
        CHECK(fs::file_size(out) == body.size(), "the file on disk matches the reported size");
        CHECK(ReadFileBytes(out) == std::vector<unsigned char>(body.begin(), body.end()),
              "the flushed content replaces the original");
        const std::vector<std::wstring> after = DirEntryNames(out.parent_path());
        CHECK(after == before, "committing leaves no temporary file behind");
        fs::remove(out);
    }
}

// ---- StreamFileWriter の I/O 故障注入（Issue #180）--------------------------
// 保存経路には OS 側で自然に起こせない分岐がある（要求より短い WriteFile 成功、
//   書込途中の失敗、FlushFileBuffers 失敗、置換に失敗して出力先が消える経路）。
//   Win32FileHooks.h の差込口（STIRLING_TEST_IO_HOOK ビルドのみ）で該当 API だけを
//   肩代わりし、契約どおりの結果・後始末・呼び出し順序になることを確認する。
#ifdef STIRLING_TEST_IO_HOOK

namespace io_fault {

// フックから見える指示と記録。テストごとに Reset() してから使う。
struct Plan {
    // --- WriteFile ---
    int   writeCallsBeforeFault = -1;  // この回数だけ素通しし、次の呼び出しで細工する（-1=細工しない）
    bool  writeShort = false;          // 要求の一部だけ書けたことにする
    DWORD writeShortBytes = 0;         // writeShort のときに書けたことにするバイト数
    bool  writeFail = false;           // WriteFile 自体を失敗させる
    DWORD writeError = 0;
    bool  writeSwallow = false;        // 実際には書かず「全部書けた」ことにする（4GB 累積用）

    // --- ReadFile ---
    DWORD readCap = 0;                 // 1 回の ReadFile で返す上限（0=素通し）
    int   readFailAtCall = -1;         // この回数目の ReadFile を失敗させる（-1=しない）
    DWORD readError = 0;
    int   readEofAtCall = -1;          // この回数目の ReadFile を EOF（0 バイト成功）にする

    // --- FlushFileBuffers ---
    bool  flushFail = false;
    DWORD flushError = 0;

    // --- ReplaceFileW / MoveFileExW ---
    int   replaceFailures = 0;         // 先頭から何回失敗させるか（残りは素通し）
    DWORD replaceError = 0;
    bool  replaceDeletesTarget = false;  // 失敗と同時に出力先を消す（ERROR_UNABLE_TO_MOVE_REPLACEMENT 相当）
    int   moveFailures = 0;
    DWORD moveError = 0;

    // --- 記録 ---
    int writeCalls = 0;
    int readCalls = 0;
    int flushCalls = 0;
    int replaceCalls = 0;
    int moveCalls = 0;
    std::vector<std::string> order;    // "flush" / "replace" / "move" の呼び出し順
};

Plan g_plan;

void Reset() { g_plan = Plan(); }

bool WriteHook(HANDLE h, const void* buf, DWORD want,
               DWORD* outWrote, DWORD* outError, BOOL* outResult) {
    const int call = g_plan.writeCalls++;
    if (g_plan.writeSwallow) {          // 実 I/O を伴わずに書けたことにする
        *outWrote = want;
        *outResult = TRUE;
        return true;
    }
    if (g_plan.writeCallsBeforeFault < 0 || call != g_plan.writeCallsBeforeFault) {
        return false;                   // 素通し
    }
    if (g_plan.writeFail) {
        *outWrote = 0;
        *outError = g_plan.writeError;
        *outResult = FALSE;
        return true;
    }
    if (g_plan.writeShort) {
        // 要求の一部だけを本当に書いてから、その分だけ書けたと報告する
        //   （継ぎ足しで最終的な内容が正しくなることを見るため実データも進める）。
        DWORD wrote = 0;
        const DWORD partial = (g_plan.writeShortBytes < want) ? g_plan.writeShortBytes : want;
        if (partial > 0 && !::WriteFile(h, buf, partial, &wrote, nullptr)) {
            *outWrote = 0;
            *outError = ::GetLastError();
            *outResult = FALSE;
            return true;
        }
        *outWrote = wrote;              // 0 バイト成功もここで表現できる
        *outResult = TRUE;
        return true;
    }
    return false;
}

// ReadFile の肩代わり。readCap で 1 回に返す量を絞り、指定回で失敗や EOF を起こす。
//   絞る場合も実データは本物の ReadFile で読むため、読み継いだ結果の内容まで確認できる。
bool ReadHook(HANDLE h, void* buf, DWORD want, DWORD* outRead, DWORD* outError, BOOL* outResult) {
    const int call = g_plan.readCalls++;
    if (g_plan.readFailAtCall >= 0 && call == g_plan.readFailAtCall) {
        *outRead = 0;
        *outError = g_plan.readError;
        *outResult = FALSE;
        return true;
    }
    if (g_plan.readEofAtCall >= 0 && call >= g_plan.readEofAtCall) {
        // 0 バイト成功＝EOF（サイズ取得後に縮んだ状態）。一度 EOF に達したら以降も
        //   EOF のままにする（実ファイルなら次の読み取りでデータが戻ることはない）。
        *outRead = 0;
        *outResult = TRUE;
        return true;
    }
    if (g_plan.readCap == 0) { return false; }   // 素通し
    const DWORD ask = (want < g_plan.readCap) ? want : g_plan.readCap;
    DWORD got = 0;
    if (!::ReadFile(h, buf, ask, &got, nullptr)) {
        *outRead = 0;
        *outError = ::GetLastError();
        *outResult = FALSE;
        return true;
    }
    *outRead = got;
    *outResult = TRUE;
    return true;
}

bool FlushHook(HANDLE, DWORD* outError, BOOL* outResult) {
    ++g_plan.flushCalls;
    g_plan.order.push_back("flush");
    if (!g_plan.flushFail) { return false; }
    *outError = g_plan.flushError;
    *outResult = FALSE;
    return true;
}

bool ReplaceHook(const wchar_t* target, const wchar_t*, DWORD* outError, BOOL* outResult) {
    const int call = g_plan.replaceCalls++;
    g_plan.order.push_back("replace");
    if (call >= g_plan.replaceFailures) { return false; }   // 素通し（再試行での成功）
    if (g_plan.replaceDeletesTarget) {
        // ReplaceFileW が ERROR_UNABLE_TO_MOVE_REPLACEMENT で失敗したときと同じ状態:
        //   出力先は既に消えており、書いた内容は一時ファイルにしか無い。
        ::DeleteFileW(target);
    }
    *outError = g_plan.replaceError;
    *outResult = FALSE;
    return true;
}

// 契約違反のフック: 要求より多く書けたと報告する。ラッパが切り詰めることを確かめる用
//   （素の値をそのまま通すと、呼出側の `left -= wrote`(size_t) が巨大値へ反転する）。
bool OverreportingWriteHook(HANDLE, const void*, DWORD want,
                            DWORD* outWrote, DWORD*, BOOL* outResult) {
    ++g_plan.writeCalls;
    *outWrote = want + 1000;   // 本物の WriteFile ではあり得ない値
    *outResult = TRUE;
    return true;
}

bool MoveHook(const wchar_t*, const wchar_t*, DWORD* outError, BOOL* outResult) {
    const int call = g_plan.moveCalls++;
    g_plan.order.push_back("move");
    if (call >= g_plan.moveFailures) { return false; }
    *outError = g_plan.moveError;
    *outResult = FALSE;
    return true;
}

// テスト 1 件のあいだだけフックを張る RAII（CHECK 失敗で抜けても必ず解除する）。
struct ScopedHooks {
    ScopedHooks() {
        Reset();
        stirling::io::SetWriteHook(&WriteHook);
        stirling::io::SetReadHook(&ReadHook);
        stirling::io::SetFlushHook(&FlushHook);
        stirling::io::SetReplaceHook(&ReplaceHook);
        stirling::io::SetMoveHook(&MoveHook);
    }
    ~ScopedHooks() { stirling::io::ClearFileHooks(); Reset(); }
    ScopedHooks(const ScopedHooks&) = delete;
    ScopedHooks& operator=(const ScopedHooks&) = delete;
};

}  // namespace io_fault

static void TestStreamFileWriterFaults() {
    TestPrintf("TestStreamFileWriterFaults\n");
    using stirling::FileIoStatus;
    using stirling::StreamFileWriter;

    const std::string body = "stream-writer-fault-injection-body";
    const std::vector<unsigned char> bodyBytes(body.begin(), body.end());

    // 1) WriteFile が要求より短く成功しても、継ぎ足して全量が書かれる。
    {
        const fs::path out = TempFile("io_short_write");
        io_fault::ScopedHooks hooks;
        io_fault::g_plan.writeCallsBeforeFault = 0;   // 最初の WriteFile を短くする
        io_fault::g_plan.writeShort = true;
        io_fault::g_plan.writeShortBytes = 5;

        StreamFileWriter w;
        CHECK(w.Open(out.wstring().c_str()).Ok(), "open for the short write case");
        const stirling::FileIoResult r = w.Write(body.data(), body.size());
        CHECK(r.Ok(), "a short write is topped up, not an error");
        CHECK(w.Written() == static_cast<FileOffset>(body.size()), "every byte is accounted for");
        CHECK(io_fault::g_plan.writeCalls >= 2, "the short write forced another WriteFile");
        CHECK(w.Commit().Ok(), "commit after the short write");
        CHECK(ReadFileBytes(out) == bodyBytes, "the file holds the whole body");
        fs::remove(out);
    }

    // 2) 0 バイト成功（ディスク不足等で進まない）はエラーにする。無限ループにしない。
    {
        const fs::path out = TempFile("io_zero_write");
        io_fault::ScopedHooks hooks;
        io_fault::g_plan.writeCallsBeforeFault = 0;
        io_fault::g_plan.writeShort = true;
        io_fault::g_plan.writeShortBytes = 0;   // 「成功したが 0 バイト」

        StreamFileWriter w;
        CHECK(w.Open(out.wstring().c_str()).Ok(), "open for the zero write case");
        const stirling::FileIoResult r = w.Write(body.data(), body.size());
        CHECK(!r.Ok(), "a zero byte success is an error");
        CHECK(r.status == FileIoStatus::kWriteFailed, "zero write status");
        CHECK(r.systemError == ERROR_WRITE_FAULT, "zero write reports a stalled write");
        CHECK(w.Written() == 0, "nothing was counted as written");
        w.Abort();
        CHECK(!fs::exists(out), "the target was never created");
    }

    // 3) 書込途中の失敗は原因コードごと返り、出力先は元のまま・一時ファイルも残らない。
    {
        const fs::path out = TempFile("io_write_fail");
        const std::vector<unsigned char> orig(2048, 0x41);
        WriteFile(out, orig);
        const std::vector<std::wstring> before = DirEntryNames(out.parent_path());

        io_fault::ScopedHooks hooks;
        io_fault::g_plan.writeCallsBeforeFault = 1;   // 2 回目の WriteFile を失敗させる
        io_fault::g_plan.writeFail = true;
        io_fault::g_plan.writeError = ERROR_DISK_FULL;

        StreamFileWriter w;
        CHECK(w.Open(out.wstring().c_str()).Ok(), "open for the write failure case");
        CHECK(w.Write(body.data(), body.size()).Ok(), "the first write succeeds");
        const stirling::FileIoResult r = w.Write(body.data(), body.size());
        CHECK(!r.Ok(), "the injected write failure is reported");
        CHECK(r.status == FileIoStatus::kWriteFailed, "write failure status");
        CHECK(r.systemError == ERROR_DISK_FULL, "write failure keeps the cause code");
        CHECK(io_fault::g_plan.replaceCalls == 0 && io_fault::g_plan.moveCalls == 0,
              "a failed write never reaches the replacement");
        w.Abort();
        CHECK(ReadFileBytes(out) == orig, "the target keeps its original content");
        CHECK(DirEntryNames(out.parent_path()) == before, "no temporary file is left behind");
        fs::remove(out);
    }

    // 4) FlushFileBuffers 失敗（遅延書込エラー）も同様。置換は行わない。
    {
        const fs::path out = TempFile("io_flush_fail");
        const std::vector<unsigned char> orig(1024, 0x42);
        WriteFile(out, orig);
        const std::vector<std::wstring> before = DirEntryNames(out.parent_path());

        io_fault::ScopedHooks hooks;
        io_fault::g_plan.flushFail = true;
        io_fault::g_plan.flushError = ERROR_DISK_FULL;

        StreamFileWriter w;
        CHECK(w.Open(out.wstring().c_str()).Ok(), "open for the flush failure case");
        CHECK(w.Write(body.data(), body.size()).Ok(), "write before the flush");
        const stirling::FileIoResult r = w.Commit();
        CHECK(!r.Ok(), "the injected flush failure is reported");
        CHECK(r.status == FileIoStatus::kWriteFailed, "flush failure status");
        CHECK(r.systemError == ERROR_DISK_FULL, "flush failure keeps the cause code");
        CHECK(io_fault::g_plan.flushCalls == 1, "the flush was actually attempted");
        CHECK(io_fault::g_plan.replaceCalls == 0 && io_fault::g_plan.moveCalls == 0,
              "a failed flush never reaches the replacement");
        CHECK(!w.IsOpen(), "the writer is closed after a failed commit");
        CHECK(w.KeptTempPath().empty(), "the target survived, so no temporary file is kept");
        CHECK(ReadFileBytes(out) == orig, "the target keeps its original content");
        CHECK(DirEntryNames(out.parent_path()) == before, "no temporary file is left behind");
        fs::remove(out);
    }

    // 5) 成功する Commit の呼び出し順序は フラッシュ → 置換。
    {
        const fs::path out = TempFile("io_order");
        WriteFile(out, std::vector<unsigned char>(16, 0x43));
        io_fault::ScopedHooks hooks;

        StreamFileWriter w;
        CHECK(w.Open(out.wstring().c_str()).Ok(), "open for the ordering case");
        CHECK(w.Write(body.data(), body.size()).Ok(), "write before the ordering check");
        CHECK(io_fault::g_plan.order.empty(), "nothing is flushed or replaced before Commit");
        CHECK(w.Commit().Ok(), "commit succeeds");
        const std::vector<std::string> want = {"flush", "replace"};
        CHECK(io_fault::g_plan.order == want, "Commit flushes before it replaces");
        CHECK(ReadFileBytes(out) == bodyBytes, "the replacement content");
        fs::remove(out);
    }

    // 6) ReplaceFileW が失敗しても MoveFileExW で置換できれば成功する。
    {
        const fs::path out = TempFile("io_move_fallback");
        WriteFile(out, std::vector<unsigned char>(16, 0x44));
        io_fault::ScopedHooks hooks;
        io_fault::g_plan.replaceFailures = 5;   // 再試行を使い切らせる
        io_fault::g_plan.replaceError = ERROR_ACCESS_DENIED;

        StreamFileWriter w;
        CHECK(w.Open(out.wstring().c_str()).Ok(), "open for the move fallback case");
        CHECK(w.Write(body.data(), body.size()).Ok(), "write for the move fallback case");
        const stirling::FileIoResult r = w.Commit();
        CHECK(r.Ok(), "the move fallback completes the commit");
        CHECK(r.fileSize == static_cast<FileOffset>(body.size()), "the reported size");
        CHECK(io_fault::g_plan.moveCalls >= 1, "the move fallback was used");
        CHECK(ReadFileBytes(out) == bodyBytes, "the target holds the new content");
        CHECK(w.KeptTempPath().empty(), "a successful commit keeps no temporary file");
        fs::remove(out);
    }

    // 7) 共有違反は一時的なことがあるので数回再試行し、途中で成功したら commit も成功する。
    {
        const fs::path out = TempFile("io_retry");
        WriteFile(out, std::vector<unsigned char>(16, 0x45));
        io_fault::ScopedHooks hooks;
        io_fault::g_plan.replaceFailures = 2;   // 3 回目で素通し＝成功
        io_fault::g_plan.replaceError = ERROR_SHARING_VIOLATION;

        StreamFileWriter w;
        CHECK(w.Open(out.wstring().c_str()).Ok(), "open for the retry case");
        CHECK(w.Write(body.data(), body.size()).Ok(), "write for the retry case");
        CHECK(w.Commit().Ok(), "a transient sharing violation is retried");
        CHECK(io_fault::g_plan.replaceCalls == 3, "the replacement was retried");
        CHECK(io_fault::g_plan.moveCalls == 0, "a successful retry never falls back to the move");
        CHECK(ReadFileBytes(out) == bodyBytes, "the retried replacement content");
        fs::remove(out);
    }

    // 8) 置換の途中で出力先が消え、移動も失敗した場合だけ一時ファイルを残す（Issue #170）。
    //    この分岐はロックを掴むだけのテストでは到達できない。
    {
        const fs::path dir = TempFile("io_kept_temp");
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        const fs::path out = dir / L"target.bin";
        WriteFile(out, std::vector<unsigned char>(64, 0x46));

        std::wstring kept;
        {
            io_fault::ScopedHooks hooks;
            io_fault::g_plan.replaceFailures = 5;
            io_fault::g_plan.replaceError = ERROR_UNABLE_TO_MOVE_REPLACEMENT;
            io_fault::g_plan.replaceDeletesTarget = true;   // 出力先が消える
            io_fault::g_plan.moveFailures = 5;
            io_fault::g_plan.moveError = ERROR_ACCESS_DENIED;

            StreamFileWriter w;
            CHECK(w.Open(out.wstring().c_str()).Ok(), "open for the kept temp case");
            CHECK(w.Write(body.data(), body.size()).Ok(), "write for the kept temp case");
            const stirling::FileIoResult r = w.Commit();
            CHECK(!r.Ok(), "the commit fails when neither replace nor move works");
            CHECK(r.status == FileIoStatus::kWriteFailed, "kept temp failure status");
            CHECK(r.systemError == ERROR_UNABLE_TO_MOVE_REPLACEMENT,
                  "the first replacement error is reported");
            CHECK(!fs::exists(out), "the target really is gone in this scenario");

            kept = w.KeptTempPath();
            CHECK(!kept.empty(), "the temporary file holding the data is kept");
            CHECK(r.keptTempPath == kept,
                  "the commit result carries the same kept temporary path (Issue #186)");
            CHECK(fs::exists(fs::path(kept)), "the kept temporary file exists");
            CHECK(ReadFileBytes(fs::path(kept)) == bodyBytes,
                  "the kept temporary file holds every byte that was written");

            w.Abort();
            CHECK(fs::exists(fs::path(kept)), "Abort does not delete the kept temporary file");
        }   // デストラクタ（= Abort 相当）でも消えない
        CHECK(!kept.empty() && fs::exists(fs::path(kept)),
              "the destructor does not delete the kept temporary file either");
        fs::remove_all(dir, ec);
    }

    // 9) 4GB を超える累積でも Written / Commit の報告サイズが 32bit へ落ちない。
    //    実 I/O を伴わないフック（書いたことにするだけ）で確認するため、
    //    ディスクもメモリも消費しない。
    {
        const fs::path out = TempFile("io_4gb");
        io_fault::ScopedHooks hooks;
        io_fault::g_plan.writeSwallow = true;

        StreamFileWriter w;
        CHECK(w.Open(out.wstring().c_str()).Ok(), "open for the 4GB accumulation case");
        const size_t chunk = 8u * 1024u * 1024u;   // 内部分割の上限と同じ 8MB
        const std::vector<unsigned char> buf(chunk, 0x00);
        const FileOffset k4GB = 4LL * 1024 * 1024 * 1024;
        const FileOffset target = k4GB + static_cast<FileOffset>(chunk);
        bool writeFailed = false;
        for (FileOffset done = 0; done < target; done += static_cast<FileOffset>(chunk)) {
            if (!w.Write(buf.data(), chunk).Ok()) { writeFailed = true; break; }
        }
        CHECK(!writeFailed, "the swallowed writes all succeed");
        CHECK(w.Written() > k4GB, "the running total passes 4GB without truncating");
        const stirling::FileIoResult r = w.Commit();
        CHECK(r.Ok(), "commit after the 4GB accumulation");
        CHECK(r.fileSize == w.Written(), "the reported size matches the running total");
        CHECK(r.fileSize > k4GB, "the reported size keeps the full 64-bit total");
        fs::remove(out);
    }

    // 10) フックを外したあとは実 I/O へ戻る（他のテストへ影響を残さない）。
    {
        const fs::path out = TempFile("io_hooks_off");
        StreamFileWriter w;
        CHECK(w.Open(out.wstring().c_str()).Ok(), "open with the hooks cleared");
        CHECK(w.Write(body.data(), body.size()).Ok(), "write with the hooks cleared");
        CHECK(w.Commit().Ok(), "commit with the hooks cleared");
        CHECK(ReadFileBytes(out) == bodyBytes, "real I/O is restored");
        fs::remove(out);
    }

    // 11) 契約違反のフック（要求より多く書けたと報告）でも、ラッパが要求値へ切り詰めるので
    //     呼出側の残り長が size_t で反転せず、走査がバッファ外へ出ない。
    {
        const fs::path out = TempFile("io_overreport");
        io_fault::ScopedHooks hooks;
        stirling::io::SetWriteHook(&io_fault::OverreportingWriteHook);

        StreamFileWriter w;
        CHECK(w.Open(out.wstring().c_str()).Ok(), "open for the over-reporting hook");
        const stirling::FileIoResult r = w.Write(body.data(), body.size());
        CHECK(r.Ok(), "the over-reported write still completes");
        CHECK(w.Written() == static_cast<FileOffset>(body.size()),
              "the byte count is clamped to what was requested");
        CHECK(io_fault::g_plan.writeCalls == 1, "one call is enough (the loop does not run away)");
        w.Abort();
    }
}

// ---- BlockFileIO 読込側の I/O 故障注入（Issue #180）------------------------
// LoadFileIntoBlocks には、ReadFile の途中結果を制御しないと通れない分岐がある。
//   - ReadFile が要求より短く成功したときの読み継ぎ
//   - 途中の読込失敗（list.Clear / kReadFailed / ハンドルを渡さない）
//   - サイズ取得後にファイルが縮んだ場合（途中 EOF）の実読込サイズ
//   - 0 バイトまで縮んだ場合の空ブロック補完
// 外部プロセスとのタイミング競争ではなく、Win32FileHooks.h の差込口で再現する。
static void TestBlockFileIoReadFaults() {
    TestPrintf("TestBlockFileIoReadFaults\n");
    using stirling::FileIoResult;
    using stirling::FileIoStatus;
    using stirling::LoadFileIntoBlocks;

    // 検証用のファイル（複数ブロック・複数チャンクにまたがらない程度の大きさ）。
    const size_t kSize = 100000;   // 16KB ブロック 7 個（末尾は端数）
    std::vector<unsigned char> data(kSize);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<unsigned char>((i * 53 + 17) & 0xFF);
    }

    // 1) ReadFile が毎回わずかしか返さなくても、読み継いで全内容が組み上がる。
    {
        const fs::path in = TempFile("io_short_read");
        WriteFile(in, data);

        io_fault::ScopedHooks hooks;
        io_fault::g_plan.readCap = 1000;   // 1 回の ReadFile は最大 1000 バイト

        BlockList list;
        const FileIoResult r = LoadFileIntoBlocks(list, in.wstring().c_str());
        CHECK(r.Ok(), "short reads are stitched together");
        CHECK(r.fileSize == static_cast<FileOffset>(kSize), "the whole file was read");
        CHECK(io_fault::g_plan.readCalls > 50, "the cap really forced many ReadFile calls");
        VerifyLoadedStructure(list, kSize, "short read structure");
        CheckEqual(list, data, "short read content");
        std::error_code ec;
        fs::remove(in, ec);
    }

    // 2) 途中の読込失敗は理由付きで返り、ブロック列を残さず、ハンドルも渡さない。
    {
        const fs::path in = TempFile("io_read_fail");
        WriteFile(in, data);

        io_fault::ScopedHooks hooks;
        io_fault::g_plan.readCap = 1000;
        io_fault::g_plan.readFailAtCall = 3;   // 4 回目の ReadFile で失敗させる
        io_fault::g_plan.readError = ERROR_CRC;

        BlockList list;
        void* const kDirtySentinel = reinterpret_cast<void*>(static_cast<intptr_t>(-1));
        void* keep = kDirtySentinel;   // 上書きされることを見るため、あえて汚しておく
        const FileIoResult r = LoadFileIntoBlocks(list, in.wstring().c_str(),
                                                  stirling::FileShareMode::kDenyNone, &keep);
        CHECK(!r.Ok(), "a read failure is reported");
        CHECK(r.status == FileIoStatus::kReadFailed, "read failure status");
        CHECK(r.systemError == ERROR_CRC, "read failure keeps the cause code");
        CHECK(r.fileSize == static_cast<FileOffset>(kSize),
              "a failed read still reports the size of the target file");
        CHECK(list.IsEmpty(), "a failed load leaves no blocks behind");
        CHECK(list.Count() == 0, "a failed load leaves an empty list");
        CHECK(keep == nullptr, "a failed load does not hand over the handle");
        // 予期せず成功した場合にハンドルを掴んだままにしない（掴んだままだと後片付けが
        //   失敗し、このテストの失敗が別の場所の例外として現れて原因が見えなくなる）。
        if (keep != nullptr && keep != kDirtySentinel) {
            ::CloseHandle(static_cast<HANDLE>(keep));
        }
        std::error_code ec;
        fs::remove(in, ec);
    }

    // 2b) 既にブロックを積んだ後（2 チャンク目）で失敗した場合も、ブロック列を残さない。
    //     1 チャンク目（1.6MB）が積まれた状態で失敗させないと list.Clear() の有無を
    //     区別できない（2 の条件では失敗時点でまだ 1 個も積まれていない）。
    {
        const size_t big = static_cast<size_t>(kReadChunk) + 40000;
        std::vector<unsigned char> huge(big, 0x5A);
        const fs::path in = TempFile("io_read_fail_late");
        WriteFile(in, huge);

        io_fault::ScopedHooks hooks;
        io_fault::g_plan.readFailAtCall = 1;   // 1 チャンク目を読み切った直後に失敗
        io_fault::g_plan.readError = ERROR_DEVICE_NOT_CONNECTED;

        BlockList list;
        const FileIoResult r = LoadFileIntoBlocks(list, in.wstring().c_str());
        CHECK(!r.Ok(), "a late read failure is reported");
        CHECK(r.status == FileIoStatus::kReadFailed, "late read failure status");
        CHECK(r.systemError == ERROR_DEVICE_NOT_CONNECTED, "late read failure cause code");
        CHECK(r.fileSize == static_cast<FileOffset>(big),
              "a late failure still reports the size of the target file");
        CHECK(io_fault::g_plan.readCalls == 2, "the failure happened on the second chunk");
        CHECK(list.IsEmpty(), "the blocks read so far are discarded");
        CHECK(list.Count() == 0, "the list is empty after a late failure");
        CHECK(list.GetTotalLength() == 0, "and reports no length");
        std::error_code ec;
        fs::remove(in, ec);
    }

    // 3) サイズ取得後に他プロセスが縮めた場合（途中 EOF）。読めた分だけを返す。
    {
        const fs::path in = TempFile("io_read_eof");
        WriteFile(in, data);

        io_fault::ScopedHooks hooks;
        io_fault::g_plan.readCap = 30000;
        io_fault::g_plan.readEofAtCall = 1;   // 30000 バイト読んだ直後に EOF

        BlockList list;
        const FileIoResult r = LoadFileIntoBlocks(list, in.wstring().c_str());
        CHECK(r.Ok(), "hitting EOF early is not an error");
        CHECK(r.fileSize == 30000, "the reported size is what was actually read");
        VerifyLoadedStructure(list, 30000, "early EOF structure");
        const std::vector<unsigned char> head(data.begin(), data.begin() + 30000);
        CheckEqual(list, head, "early EOF content");
        std::error_code ec;
        fs::remove(in, ec);
    }

    // 4) 0 バイトまで縮んだ場合でも、ドキュメントの不変条件（1 個以上のブロック）を保つ。
    {
        const fs::path in = TempFile("io_read_eof0");
        WriteFile(in, data);

        io_fault::ScopedHooks hooks;
        io_fault::g_plan.readEofAtCall = 0;   // 最初の ReadFile がいきなり EOF

        BlockList list;
        void* keep = nullptr;
        const FileIoResult r = LoadFileIntoBlocks(list, in.wstring().c_str(),
                                                  stirling::FileShareMode::kDenyNone, &keep);
        CHECK(r.Ok(), "shrinking to zero is not an error");
        CHECK(r.fileSize == 0, "nothing was read");
        CHECK(list.Count() == 1, "the empty block is supplied");
        CHECK(list.GetHead() != nullptr && list.GetHead()->usedLen == 0, "and it is empty");
        CHECK(keep != nullptr, "a successful load hands over the handle");
        if (keep != nullptr) { ::CloseHandle(static_cast<HANDLE>(keep)); }
        std::error_code ec;
        fs::remove(in, ec);
    }

    // 5) チャンク境界（1.6MB）を跨ぐ読み継ぎでも内容が一致する。
    {
        const size_t big = static_cast<size_t>(kReadChunk) + 40000;   // 2 チャンク目に入る
        std::vector<unsigned char> huge(big);
        for (size_t i = 0; i < huge.size(); ++i) {
            huge[i] = static_cast<unsigned char>((i * 7 + 3) & 0xFF);
        }
        const fs::path in = TempFile("io_read_chunks");
        WriteFile(in, huge);

        io_fault::ScopedHooks hooks;
        io_fault::g_plan.readCap = 7777;   // 16KB にも 1.6MB にも揃わない半端な刻み

        BlockList list;
        const FileIoResult r = LoadFileIntoBlocks(list, in.wstring().c_str());
        CHECK(r.Ok(), "reads across the chunk boundary are stitched together");
        CHECK(r.fileSize == static_cast<FileOffset>(big), "the whole multi-chunk file was read");
        VerifyLoadedStructure(list, big, "multi-chunk short read structure");
        CheckEqual(list, huge, "multi-chunk short read content");
        std::error_code ec;
        fs::remove(in, ec);
    }

    // 6) フックを外したあとは実 I/O へ戻る。
    {
        const fs::path in = TempFile("io_read_hooks_off");
        WriteFile(in, data);
        BlockList list;
        CHECK(LoadFileIntoBlocks(list, in.wstring().c_str()).Ok(), "real read I/O is restored");
        CheckEqual(list, data, "real read content");
        std::error_code ec;
        fs::remove(in, ec);
    }
}

#endif  // STIRLING_TEST_IO_HOOK

// ---- 安全保存: temp→置換（Issue #170） ----
// SaveBlocksToFile は出力先を直接切り詰めて書いていたため、書込・フラッシュに失敗すると
// 出力先が中途状態で残った。StreamFileWriter 経由（原 CMirrorFile と同じ temp→置換）に
// なったことで、成功しない限り出力先が変わらないこと、および ReplaceFileW によって
// 出力先の同一性（ファイル ID・代替データストリーム）が保たれることを確認する。
static void TestAtomicSave() {
    TestPrintf("TestAtomicSave\n");
    using stirling::BlockList;
    using stirling::BlockCursor;
    using stirling::FileIoResult;
    using stirling::FileIoStatus;

    // 保存対象のブロック列を用意する（内容は body）。
    const std::string body = "atomic-save-body";
    auto seedList = [&body](BlockList& list) {
        NewEmptyDoc(list);
        BlockCursor c(&list);
        CHECK(c.Insert(0, body.data(), static_cast<FileOffset>(body.size())), "seed for atomic save");
    };

    // 一時ファイルの取り残しを数える検証は、%TEMP% 直下ではなく専用ディレクトリで行う
    //   （他プロセスが %TEMP% へファイルを作ると件数比較が揺れるため）。
    auto makeCaseDir = [](const char* tag) {
        const fs::path dir = TempFile(tag);
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        CHECK(fs::is_directory(dir), "create a private directory for the case");
        return dir;
    };
    auto entryNames = [](const fs::path& dir) { return DirEntryNames(dir); };

    // 1) 既存ファイルへの保存は、作成日時と代替データストリームを保つ（ReplaceFileW）。
    //    MoveFileExW で置き換えると、どちらも一時ファイル側の状態になってしまう。
    {
        const fs::path out = TempFile("atomic_ads");
        WriteFile(out, std::vector<unsigned char>(4096, 0x31));

        // 代替データストリーム（NTFS のみ）。付随情報を持つファイルの保存で失われないこと。
        //   ストリームを持てないファイルシステム（FAT32・ネットワーク）では検証を飛ばす。
        const std::wstring adsPath = out.wstring() + L":stirhex_meta";
        bool adsReady = false;
        {
            HANDLE ads = ::CreateFileW(adsPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (ads != INVALID_HANDLE_VALUE) {
                DWORD wrote = 0;
                adsReady = (::WriteFile(ads, "meta", 4, &wrote, nullptr) != FALSE) && (wrote == 4);
                ::CloseHandle(ads);
            } else {
                SkipTest("TestAtomicSave/alternate data stream",
                         "alternate data streams unsupported on this volume");
            }
        }
        // 置換前の作成日時（ReplaceFileW は置換先の作成日時を引き継ぐ。MoveFileExW は
        //   一時ファイルの作成日時になるため、ここが元の値のままなら置換方式を確認できる）。
        auto creationTimeOf = [](const fs::path& p) -> unsigned long long {
            HANDLE h = ::CreateFileW(p.wstring().c_str(), GENERIC_READ,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) { return 0; }
            FILETIME created = {};
            const BOOL ok = ::GetFileTime(h, &created, nullptr, nullptr);
            ::CloseHandle(h);
            if (ok == FALSE) { return 0; }
            return (static_cast<unsigned long long>(created.dwHighDateTime) << 32) |
                   created.dwLowDateTime;
        };
        const unsigned long long createdBefore = creationTimeOf(out);
        CHECK(createdBefore != 0, "the creation time is available before saving");

        BlockList list;
        seedList(list);
        const FileIoResult r = stirling::SaveBlocksToFile(list, out.wstring().c_str());
        CHECK(r.Ok(), "atomic save succeeds over an existing file");
        CHECK(ReadFileBytes(out) == std::vector<unsigned char>(body.begin(), body.end()),
              "the saved content replaces the original");
        CHECK(creationTimeOf(out) == createdBefore,
              "the target keeps its creation time (ReplaceFileW)");
        if (adsReady) {
            HANDLE ads = ::CreateFileW(adsPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            CHECK(ads != INVALID_HANDLE_VALUE, "the alternate data stream survives the save");
            if (ads != INVALID_HANDLE_VALUE) {
                char buf[8] = {0};
                DWORD got = 0;
                const BOOL ok = ::ReadFile(ads, buf, 4, &got, nullptr);
                ::CloseHandle(ads);
                CHECK(ok != FALSE && got == 4 && std::memcmp(buf, "meta", 4) == 0,
                      "the stream content survives the save");
            }
        }
        fs::remove(out);
    }

    // 2) 置換に失敗すると、出力先は元の内容のまま残る。
    //    Open の後に別ハンドルが出力先を書込拒否で掴むと、ReplaceFileW/MoveFileExW の
    //    どちらも共有違反で失敗する（＝書き終えた後で置換だけが失敗する経路）。
    {
        const fs::path dir = makeCaseDir("atomic_locked_commit");
        const fs::path out = dir / L"target.bin";
        const std::vector<unsigned char> orig(2048, 0x5C);
        WriteFile(out, orig);
        const std::vector<std::wstring> before = entryNames(dir);

        stirling::StreamFileWriter w;
        CHECK(w.Open(out.wstring().c_str()).Ok(), "open while the target is free");
        CHECK(w.Write(body.data(), body.size()).Ok(), "write the replacement");

        // 読み取りのみ許可＝書込・削除を拒否するハンドル。
        HANDLE hold = ::CreateFileW(out.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(hold != INVALID_HANDLE_VALUE, "hold the target against replacement");
        if (hold != INVALID_HANDLE_VALUE) {
            const FileIoResult r = w.Commit();
            ::CloseHandle(hold);

            CHECK(!r.Ok(), "commit fails when the target cannot be replaced");
            CHECK(r.status == FileIoStatus::kWriteFailed, "replacement failure status");
            CHECK(r.systemError != 0, "replacement failure reports a system error");
            CHECK(ReadFileBytes(out) == orig, "the target keeps its original content");
            CHECK(w.KeptTempPath().empty(), "the target survived, so no temporary file is kept");
            CHECK(entryNames(dir) == before,
                  "a failed replacement leaves no temporary file behind");
        } else {
            w.Abort();
        }
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    // 3) 使用中（書込拒否）のファイルへの保存は、書き始める前に kOpenFailed。
    //    以前は CREATE_ALWAYS のオープンで弾かれていた。置換方式でも、書き終えてから
    //    権限で失敗するのではなく Open の時点で理由を返す。
    {
        const fs::path out = TempFile("atomic_locked_open");
        const std::vector<unsigned char> orig(1024, 0x2D);
        WriteFile(out, orig);
        HANDLE hold = ::CreateFileW(out.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(hold != INVALID_HANDLE_VALUE, "hold the target before saving");
        if (hold != INVALID_HANDLE_VALUE) {
            BlockList list;
            seedList(list);
            const FileIoResult r = stirling::SaveBlocksToFile(list, out.wstring().c_str());
            ::CloseHandle(hold);

            CHECK(!r.Ok(), "saving to a file held against writing fails");
            CHECK(r.status == FileIoStatus::kOpenFailed, "in-use save status");
            CHECK(r.systemError != 0, "in-use save reports a system error");
            CHECK(ReadFileBytes(out) == orig,
                  "the target is untouched when it cannot be replaced");
        }
        fs::remove(out);
    }

    // 4) ディレクトリを保存先に指定しても、出力先（ディレクトリ）に触れずに失敗する。
    {
        const fs::path dir = fs::temp_directory_path();
        BlockList list;
        seedList(list);
        const FileIoResult r = stirling::SaveBlocksToFile(list, dir.wstring().c_str());
        CHECK(!r.Ok(), "saving onto a directory fails");
        CHECK(r.status == FileIoStatus::kOpenFailed, "directory save status");
        CHECK(fs::is_directory(dir), "the directory is left alone");
    }

    // 5) 空のブロック列（0 バイト）でも置換は成立し、出力先は 0 バイトになる。
    {
        const fs::path out = TempFile("atomic_empty");
        WriteFile(out, std::vector<unsigned char>(300, 0x77));
        BlockList list;
        NewEmptyDoc(list);   // 空の 16KB ブロック 1 個（usedLen == 0）
        const FileIoResult r = stirling::SaveBlocksToFile(list, out.wstring().c_str());
        CHECK(r.Ok(), "saving an empty document succeeds");
        CHECK(r.fileSize == 0, "an empty save reports zero bytes");
        CHECK(fs::file_size(out) == 0, "the target becomes empty");
        fs::remove(out);
    }

    // 6) 新規作成（出力先が無い）は MoveFileExW 経路。保存後に一時ファイルを残さない。
    {
        const fs::path dir = makeCaseDir("atomic_new");
        const fs::path out = dir / L"created.bin";
        BlockList list;
        seedList(list);
        const FileIoResult r = stirling::SaveBlocksToFile(list, out.wstring().c_str());
        CHECK(r.Ok(), "saving to a new path succeeds");
        CHECK(r.fileSize == static_cast<FileOffset>(body.size()), "new save reports the size");
        CHECK(ReadFileBytes(out) == std::vector<unsigned char>(body.begin(), body.end()),
              "new save content");
        CHECK(entryNames(dir).size() == 1, "only the saved file remains in the directory");
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    // 7) 複数ブロック（16KB 超）でも、ブロック境界をまたいで内容が連結される。
    {
        const fs::path out = TempFile("atomic_multiblock");
        WriteFile(out, std::vector<unsigned char>(10, 0x00));   // 既存ファイルを置換する経路
        std::vector<unsigned char> data(static_cast<size_t>(kBlockCapacity) * 2 + 777);
        for (size_t i = 0; i < data.size(); ++i) {
            data[i] = static_cast<unsigned char>((i * 37 + 11) & 0xFF);
        }
        BlockList list;
        NewEmptyDoc(list);
        {
            BlockCursor c(&list);
            CHECK(c.Insert(0, data.data(), static_cast<FileOffset>(data.size())),
                  "seed multiple blocks");
        }
        CHECK(list.Count() >= 3, "the document spans several blocks");
        const FileIoResult r = stirling::SaveBlocksToFile(list, out.wstring().c_str());
        CHECK(r.Ok(), "multi-block save succeeds");
        CHECK(r.fileSize == static_cast<FileOffset>(data.size()), "multi-block save size");
        CHECK(ReadFileBytes(out) == data, "multi-block save content");
        fs::remove(out);
    }

#ifdef STIRLING_TEST_IO_HOOK
    // 8) 置換に失敗して出力先が消えた場合、残した一時ファイルのパスが SaveBlocksToFile の
    //    戻り値まで伝わる（Issue #186）。この経路でしか書いた内容の在り処を利用者へ
    //    知らせられないため、writer 内部で保持するだけでなく結果へ載ることを固定する。
    {
        const fs::path dir = makeCaseDir("atomic_kept_temp_result");
        const fs::path out = dir / L"target.bin";
        WriteFile(out, std::vector<unsigned char>(32, 0x5A));

        BlockList list;
        seedList(list);

        std::wstring kept;
        {
            io_fault::ScopedHooks hooks;
            io_fault::g_plan.replaceFailures = 5;
            io_fault::g_plan.replaceError = ERROR_UNABLE_TO_MOVE_REPLACEMENT;
            io_fault::g_plan.replaceDeletesTarget = true;   // 出力先が消える
            io_fault::g_plan.moveFailures = 5;
            io_fault::g_plan.moveError = ERROR_ACCESS_DENIED;

            const FileIoResult r = stirling::SaveBlocksToFile(list, out.wstring().c_str());
            CHECK(!r.Ok(), "the save fails when neither replace nor move works");
            CHECK(r.status == FileIoStatus::kWriteFailed, "kept temp save status");
            CHECK(!fs::exists(out), "the target really is gone in this scenario");
            kept = r.keptTempPath;
            CHECK(!kept.empty(), "the result carries the kept temporary file path");
            CHECK(fs::exists(fs::path(kept)), "the kept temporary file exists");
            CHECK(ReadFileBytes(fs::path(kept)) == std::vector<unsigned char>(body.begin(), body.end()),
                  "the kept temporary file holds what the save wrote");
        }
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    // 9) 置換に成功した保存では取り残しパスを載せない（誤った案内を出さないため）。
    {
        const fs::path dir = makeCaseDir("atomic_kept_temp_absent");
        const fs::path out = dir / L"target.bin";
        WriteFile(out, std::vector<unsigned char>(32, 0x5B));
        BlockList list;
        seedList(list);
        const FileIoResult r = stirling::SaveBlocksToFile(list, out.wstring().c_str());
        CHECK(r.Ok(), "the save succeeds without injected failures");
        CHECK(r.keptTempPath.empty(), "a successful save carries no kept temporary path");
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
#endif  // STIRLING_TEST_IO_HOOK
}

// ---- BGREP 通知（Issue #156） ----
// ワーカ→UI の通知は、以前はファイルサイズ・ヒット位置を WPARAM へ直接入れていた。
// WPARAM は Win32 で 32bit のため 4GB 以上が切り詰められる（4GB の倍数は 0 に化けて
// 「アクセス拒否」と誤判定される）。LPARAM 経由で構造体を渡す形になったことで、
// 4GB 境界前後の値が欠損しないことを確認する。
static void TestBgrepNotify() {
    TestPrintf("TestBgrepNotify\n");
    using stirling::BgrepHitNotify;
    using stirling::BgrepScanNotify;

    // 通知が 64bit を保持できる型であること（WPARAM への逆戻りを型で防ぐ）。
    static_assert(sizeof(BgrepScanNotify::size) == 8, "scan size must stay 64-bit");
    static_assert(sizeof(BgrepHitNotify::pos) == 8, "hit position must stay 64-bit");

    // 型が 64bit 値を保持できること（4GB 境界前後を代表値で確認する）。
    //   かつてはここで 9 個の値を LPARAM へキャストして戻す往復と、
    //   「WPARAM は 4GB を 0 へ切り詰める」という旧方式の実演を行っていた。どちらも
    //   生産コードの送信・受信処理を通らないため、通知経路が WPARAM へ戻っても合格し得た。
    //   経路そのものの回帰検出は e2e（BGREP の結果表示）へ委ね、ここは型契約に絞る
    //   （Issue #178）。
    const FileOffset kValues[] = {
        0,                     // 「サイズ 0＝アクセス拒否」の判定に使う値
        0xFFFFFFFFll,          // 4GB - 1
        0x100000000ll,         // 4GB ちょうど（32bit 幅なら 0 に化ける）
        0x123456789Abcll,      // 任意の 4GB 超
    };
    for (const FileOffset v : kValues) {
        BgrepScanNotify scan;
        scan.size = v;
        CHECK(scan.size == v, "the scan size field holds a 64-bit value");
        // 「サイズ 0＝アクセス拒否」の判定が 4GB の倍数で誤発火しない。
        CHECK((scan.size == 0) == (v == 0), "access-denied decision uses the full 64-bit size");

        BgrepHitNotify hit;
        hit.pos = v;
        CHECK(hit.pos == v, "the hit position field holds a 64-bit value");
    }
}


static void TestChecksum() {
    using stirling::Checksum;
    using stirling::ChecksumAlgorithm;
    using stirling::ChecksumState;
    const ChecksumAlgorithm algorithms[] = { ChecksumAlgorithm::Crc32,
        ChecksumAlgorithm::Md5, ChecksumAlgorithm::Sha1, ChecksumAlgorithm::Sha256 };
    const char* inputs[] = { "", "abc", "123456789" };
    const char* expected[][4] = {
        { "00000000", "D41D8CD98F00B204E9800998ECF8427E", "DA39A3EE5E6B4B0D3255BFEF95601890AFD80709", "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855" },
        { "352441C2", "900150983CD24FB0D6963F7D28E17F72", "A9993E364706816ABA3E25717850C26C9CD0D89D", "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD" },
        { "CBF43926", "25F9E794323B453885F5181F1B624D0B", "F7C3BC1D808E04732ADF679965CCC34CA7AE3441", "15E2B0D3C33891EBB0F1EF609EC419420C20E320CE94C65FBC8C3312448EB225" }
    };
    Checksum hash;
    for (size_t row = 0; row < 3; ++row) {
        BlockList list;
        NewEmptyDoc(list);
        BlockCursor cursor(&list);
        const auto length = static_cast<FileOffset>(std::strlen(inputs[row]));
        if (length) CHECK(cursor.Insert(0, inputs[row], length), "checksum input");
        const auto before = ReadAll(list);
        for (size_t a = 0; a < 4; ++a) {
            hash.Start(list, 0, length, algorithms[a]);
            while (hash.State() == ChecksumState::Running) hash.Step(1);
            CHECK(hash.State() == ChecksumState::Complete, "checksum completed");
            CHECK(std::strcmp(hash.Hex(), expected[row][a]) == 0, "known checksum vector");
            CHECK(hash.Processed() == length, "processed length");
            CHECK(ReadAll(list) == before, "checksum did not mutate data");
        }
    }
    BlockList blocks;
    NewEmptyDoc(blocks);
    std::vector<unsigned char> data(3 * kBlockCapacity + 17);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<unsigned char>(i * 17 + 3);
    BlockCursor cursor(&blocks);
    CHECK(cursor.Insert(0, data.data(), static_cast<FileOffset>(data.size())), "multi-block input");
    // Slice starts immediately before a block boundary and crosses two boundaries.
    const FileOffset start = kBlockCapacity - 3, length = kBlockCapacity + 11;
    hash.Start(blocks, start, length, ChecksumAlgorithm::Sha256);
    CHECK(hash.Step(0) == ChecksumState::Running && hash.Processed() == 0, "zero budget");
    for (size_t i = 0; i < 1000 && hash.State() == ChecksumState::Running; ++i) hash.Step(257);
    CHECK(hash.State() == ChecksumState::Complete, "block-boundary calculation");
    CHECK(std::strcmp(hash.Hex(), "1FE455C6E440138C224DCB979C9F40D4CAACDEE2F6463DDE6D6C2D0895B50A34") == 0, "independent slice digest");
    CHECK(ReadAll(blocks) == data, "slice calculation preserved bytes");
    hash.Start(blocks, 0, blocks.GetTotalLength(), ChecksumAlgorithm::Sha256);
    hash.Step(10);
    hash.Cancel();
    CHECK(hash.State() == ChecksumState::Cancelled && !*hash.Hex(), "cancel discards digest");
    CHECK(hash.Step(100) == ChecksumState::Cancelled, "cancelled calculation stays stopped");
    hash.Start(blocks, blocks.GetTotalLength(), 0, ChecksumAlgorithm::Crc32);
    CHECK(hash.State() == ChecksumState::Complete && std::strcmp(hash.Hex(), "00000000") == 0, "empty EOF range");
    const FileOffset invalid[][2] = { {-1, 1}, {0, -1}, {1, INT64_MAX}, {INT64_MAX, 1}, {0, INT64_MAX} };
    for (const auto& range : invalid) {
        CHECK(hash.Start(blocks, range[0], range[1], ChecksumAlgorithm::Sha256) == ChecksumState::InvalidRange, "reject invalid range without overflow");
        CHECK(!*hash.Hex(), "failure clears previous digest");
    }
    CHECK(hash.Start(blocks, 0, 1, static_cast<ChecksumAlgorithm>(99)) == ChecksumState::CryptoError, "reject unsupported algorithm");
    CHECK(hash.ErrorCode() != 0 && !*hash.Hex(), "crypto error is reported");
    hash.Start(blocks, 0, 1, ChecksumAlgorithm::Crc32);
    auto* first = blocks.GetHead();
    auto* saved = first->data;
    first->data = nullptr; // Controlled read failure; restore before destruction.
    CHECK(hash.Step(1) == ChecksumState::ReadError && !*hash.Hex(), "read error is not a digest");
    first->data = saved;
    BlockList empty;
    CHECK(hash.Start(empty, 0, 0, ChecksumAlgorithm::Sha256) == ChecksumState::Complete, "empty list");
}

int wmain(int argc, wchar_t** argv) {
    std::filesystem::path reportDirectory;
    if (argc == 3 && std::wcscmp(argv[1], L"--report-dir") == 0) {
        reportDirectory = argv[2];
    } else if (argc != 1) {
        std::fprintf(stderr, "Usage: core_test.exe [--report-dir DIRECTORY]\n");
        return 2;
    }
    TestPrintf("=== Stirling core unit tests ===\n");
    g_report.Run("TestBlockListBasics", g_checks, g_failures, TestBlockListBasics);
    g_report.Run("TestMultiByteInsert", g_checks, g_failures, TestMultiByteInsert);
    g_report.Run("TestRead", g_checks, g_failures, TestRead);
    g_report.Run("TestSeek", g_checks, g_failures, TestSeek);
    g_report.Run("TestInsertByteSplit", g_checks, g_failures, TestInsertByteSplit);
    g_report.Run("TestInsertByteFullBlockLastPos", g_checks, g_failures, TestInsertByteFullBlockLastPos);
    g_report.Run("TestInsertOverflowAtLastByte", g_checks, g_failures, TestInsertOverflowAtLastByte);
    g_report.Run("TestDelete", g_checks, g_failures, TestDelete);
    g_report.Run("TestDeleteLastByteSingleBlock", g_checks, g_failures, TestDeleteLastByteSingleBlock);
    g_report.Run("TestFuzz", g_checks, g_failures, TestFuzz);
    g_report.Run("TestFileRoundTrip", g_checks, g_failures, TestFileRoundTrip);
    g_report.Run("TestLoadEditSave", g_checks, g_failures, TestLoadEditSave);
    g_report.Run("TestSearchBasic", g_checks, g_failures, TestSearchBasic);
    g_report.Run("TestSearchEofBounds", g_checks, g_failures, TestSearchEofBounds);
    g_report.Run("TestSearchMismatch", g_checks, g_failures, TestSearchMismatch);
    g_report.Run("TestSearchAcrossBlocks", g_checks, g_failures, TestSearchAcrossBlocks);
    g_report.Run("TestSearchFuzz", g_checks, g_failures, TestSearchFuzz);
    g_report.Run("TestSearchBackwardMissedMatch", g_checks, g_failures, TestSearchBackwardMissedMatch);
    g_report.Run("TestSetByteAt", g_checks, g_failures, TestSetByteAt);
    g_report.Run("TestLargeOffsetSeek", g_checks, g_failures, TestLargeOffsetSeek);
    g_report.Run("TestLargeOffsetDataOps", g_checks, g_failures, TestLargeOffsetDataOps);
    g_report.Run("TestLargeOffsetBulkOps", g_checks, g_failures, TestLargeOffsetBulkOps);
    g_report.Run("TestLargeRealData", g_checks, g_failures, TestLargeRealData);
    g_report.Run("TestFileIoStatus", g_checks, g_failures, TestFileIoStatus);
    g_report.Run("TestLargeFileRoundTrip", g_checks, g_failures, TestLargeFileRoundTrip);
    g_report.Run("TestSettingsCodec", g_checks, g_failures, TestSettingsCodec);
    g_report.Run("TestSettingsCodecWide", g_checks, g_failures, TestSettingsCodecWide);
    g_report.Run("TestSettingsMigration", g_checks, g_failures, TestSettingsMigration);
    g_report.Run("TestSettingsStoreUtf8", g_checks, g_failures, TestSettingsStoreUtf8);
    g_report.Run("TestSettingsStoreValueEscape", g_checks, g_failures, TestSettingsStoreValueEscape);
    g_report.Run("TestMarkFileRoundTrip", g_checks, g_failures, TestMarkFileRoundTrip);
    g_report.Run("TestMarkFileEmptyAndComments", g_checks, g_failures, TestMarkFileEmptyAndComments);
    g_report.Run("TestMarkFileRejects", g_checks, g_failures, TestMarkFileRejects);
    g_report.Run("TestMarkFileHugeDecimals", g_checks, g_failures, TestMarkFileHugeDecimals);
    g_report.Run("TestMarkListRoundTrip", g_checks, g_failures, TestMarkListRoundTrip);
    g_report.Run("TestMarkListLimitAndRejects", g_checks, g_failures, TestMarkListLimitAndRejects);
    g_report.Run("TestSettingsStoreIni", g_checks, g_failures, TestSettingsStoreIni);
    g_report.Run("TestSettingsStoreChangeLog", g_checks, g_failures, TestSettingsStoreChangeLog);
    g_report.Run("TestSettingsFileMergedSave", g_checks, g_failures, TestSettingsFileMergedSave);
    g_report.Run("TestSettingsFileConcurrentSave", g_checks, g_failures, TestSettingsFileConcurrentSave);
    g_report.Run("TestSettingsFileErrors", g_checks, g_failures, TestSettingsFileErrors);
    g_report.Run("TestFolderToReveal", g_checks, g_failures, TestFolderToReveal);
    g_report.Run("TestSettingsStoreBinary", g_checks, g_failures, TestSettingsStoreBinary);
    g_report.Run("TestCp932Text", g_checks, g_failures, TestCp932Text);
    g_report.Run("TestCp932LeadByte", g_checks, g_failures, TestCp932LeadByte);
    g_report.Run("TestFormatStructCharArrayCp932", g_checks, g_failures, TestFormatStructCharArrayCp932);
    g_report.Run("TestFormatStructCharArrayW", g_checks, g_failures, TestFormatStructCharArrayW);
    g_report.Run("TestStructDefParse", g_checks, g_failures, TestStructDefParse);
    g_report.Run("TestStructScalarFormat", g_checks, g_failures, TestStructScalarFormat);
    g_report.Run("TestStructScalarEncode", g_checks, g_failures, TestStructScalarEncode);
    g_report.Run("TestStructTree", g_checks, g_failures, TestStructTree);
    g_report.Run("TestHexTextParse", g_checks, g_failures, TestHexTextParse);
    g_report.Run("TestChecksum", g_checks, g_failures, TestChecksum);
    g_report.Run("TestUtf8Text", g_checks, g_failures, TestUtf8Text);
    g_report.Run("TestUtf16Text", g_checks, g_failures, TestUtf16Text);
    g_report.Run("TestClipboardUtil", g_checks, g_failures, TestClipboardUtil);
    g_report.Run("TestClipboardTransferOs", g_checks, g_failures, TestClipboardTransferOs);
    g_report.Run("TestUndoBudget", g_checks, g_failures, TestUndoBudget);
    g_report.Run("TestDeleteRange", g_checks, g_failures, TestDeleteRange);
    g_report.Run("TestWriteAndFillRange", g_checks, g_failures, TestWriteAndFillRange);
    g_report.Run("TestSearchForwardRange", g_checks, g_failures, TestSearchForwardRange);
    g_report.Run("TestEditFuzzMixed", g_checks, g_failures, TestEditFuzzMixed);
    g_report.Run("TestCursorAbsCacheAfterEdit", g_checks, g_failures, TestCursorAbsCacheAfterEdit);
    g_report.Run("TestStreamFileWriter", g_checks, g_failures, TestStreamFileWriter);
#ifdef STIRLING_TEST_IO_HOOK
    g_report.Run("TestStreamFileWriterFaults", g_checks, g_failures, TestStreamFileWriterFaults);
    g_report.Run("TestBlockFileIoReadFaults", g_checks, g_failures, TestBlockFileIoReadFaults);
#endif
    g_report.Run("TestAtomicSave", g_checks, g_failures, TestAtomicSave);
    g_report.Run("TestBgrepNotify", g_checks, g_failures, TestBgrepNotify);
#ifdef STIRLING_TEST_ALLOC_HOOK
    g_report.Run("TestAllocFailureRollback", g_checks, g_failures, TestAllocFailureRollback);
#endif

    // 実行環境とスキップの内訳を出す。ALL PASS は「実行した検証がすべて通った」意味で、
    //   スキップした項目まで検証済みという意味ではない（オプトインの 2GB 系など）。
    TestPrintf("=== arch: %d-bit, %d checks, %d failures, %zu skipped ===\n",
                static_cast<int>(sizeof(void*) * 8), g_checks, g_failures, g_skipped.size());
    for (const std::string& s : g_skipped) {
        TestPrintf("  skipped: %s\n", s.c_str());
    }

    // 実行専用の一時ディレクトリを片付ける（残っていれば取り残しとして報告する）。
    {
        std::error_code ec;
        const std::vector<std::wstring> left = DirEntryNames(TestTempRoot());
        if (!left.empty()) {
            TestPrintf("  note: %zu leftover entries in the test temp directory\n", left.size());
        }
        fs::remove_all(TestTempRoot(), ec);
    }

    if (!reportDirectory.empty()) {
        try {
            g_report.Write(reportDirectory, sizeof(void*) == 8 ? "x64" : "x86");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "Report error: %s\n", e.what());
            return 2;
        }
    }
    if (g_failures == 0) {
        TestPrintf("ALL PASS\n");
        return 0;
    }
    TestPrintf("FAILED\n");
    return 1;
}
