// BlockCursor 実装。原 Stirling 1.31 の編集プリミティブを逆コンパイルから忠実に移植。
// メモ: 原 FUN_00467a70=memmove/memcpy, FUN_0047c3b5=malloc(0x4000), FUN_0047c3de=free。
//       本移植では memmove（右シフトの重なり対応）と new[]/delete[] を用いる。
//
// x64 化(Issue #19): 絶対位置・ファイル長は FileOffset(64bit) へ置換した。
// analysis_artifacts/docs/05_BlockCursor_edit_ops.md は原の 32bit 実装を記述しており、
// 位置・長さの型については本ファイルが意図的に乖離している（アルゴリズムは同一）。
// ノード内オフセット・ブロック長は 16KB 上限のため int を維持し、
// 64bit→32bit の変換は「16KB 未満であることが確定した箇所」でのみ明示的に行う。
#include "BlockCursor.h"

#include <algorithm>
#include <cstring>

namespace stirling {

BlockCursor::BlockCursor(BlockList* list)
    : list_(list), curNode_(nullptr), curOffset_(0), curAbs_(0) {}

bool BlockCursor::Seek(FileOffset pos, int origin, FileOffset* outAbs) {
    if (list_->IsEmpty()) {
        return false;
    }

    FileOffset total = 0;  // 原 local_10: 現ノードまでの累積長 / 最終的な絶対位置
    int off = 0;           // 原 local_14: ノード内オフセット（<=capacity のため int）
    BlockNode* node = nullptr;  // 原 local_c

    if (origin == kBegin) {
        // ---- 先頭からの絶対シーク（編集系の主経路）----
        if (pos < 0) return false;
        node = list_->GetHead();
        if (node == nullptr) return false;
        if (pos == 0) {
            off = 0;
        } else {
            int used;
            for (total = 0; (used = node->usedLen), (total + used) <= pos; total += used) {
                BlockNode* nx = list_->GetNext(node);
                if (nx == nullptr) {
                    // 末尾に到達。ぴったり末尾(=EOF追記位置)なら off=usedLen を許容。
                    if (total + used != pos) return false;
                    break;
                }
                node = nx;
            }
            // ループ脱出時 (pos - total) <= usedLen <= capacity のため int に収まる。
            off = static_cast<int>(pos - total);
            total = total + off;  // == pos
        }
    } else if (origin == kCurrent) {
        // ---- 現在位置からの相対シーク（逐語移植）----
        if (curNode_ == nullptr) return false;
        if (pos == 0) {
            // カーソルは動かさない。要求があれば絶対位置のみ算出。
            if (outAbs != nullptr) {
                node = list_->GetHead();
                if (node == nullptr) return false;
                total = 0;
                while (node != curNode_) {
                    total += node->usedLen;
                    node = list_->GetNext(node);
                    if (node == nullptr) return false;
                }
                *outAbs = total + curOffset_;
            }
            return true;
        }
        if (pos < 1) {
            // 後方へ delta バイト
            FileOffset delta = -pos;      // 原 iVar3
            node = curNode_;              // 原 local_c
            int step = curOffset_ + 1;    // 原 local_8（ノード内の歩数のため int）
            off = curOffset_;             // 原 local_14
            total = 0;                    // 原 local_10
            while (total + step <= delta) {
                BlockNode* nx = list_->GetNext(node);  // 原 iVar2
                if (nx == nullptr) {
                    if (total + step != delta) return false;
                    nx = node;
                    if (outAbs != nullptr) {
                        BlockNode* p = node;
                        // ここでは delta - total == step（直前の等値判定）のため int 幅で安全。
                        total = (step - static_cast<int>(delta - total)) - 1;
                        while ((p = list_->GetPrev(p)) != nullptr) {
                            step = p->usedLen;
                            total += step;
                        }
                    }
                }
                node = nx;
                total += step;
                step = node->usedLen;
                off = step - 1;
            }
            // ループ脱出時 delta - total < step <= capacity のため int に収まる。
            off = off - static_cast<int>(delta - total);
            if (outAbs != nullptr) {
                BlockNode* p = node;
                total = off;
                while ((p = list_->GetPrev(p)) != nullptr) {
                    total += p->usedLen;
                }
            }
        } else {
            // 前方へ pos バイト
            node = curNode_;
            int step = node->usedLen - (curOffset_ + 1);  // 現ブロックのカーソル後方バイト数
            off = curOffset_;
            total = 0;
            bool landed = false;
            while (total + step <= pos) {
                BlockNode* nx = list_->GetNext(node);
                if (nx == nullptr) {
                    if (total + step != pos) return false;
                    off = static_cast<int>(pos - total);  // == step（<=capacity）
                    total = total + off;
                    landed = true;
                    break;
                }
                total += step;
                step = nx->usedLen;
                off = 0;
                node = nx;
            }
            if (!landed) {
                // ループ脱出時 pos - total < step <= capacity のため int に収まる。
                off = off + static_cast<int>(pos - total);
                if (outAbs != nullptr) {
                    BlockNode* p = node;
                    total = off;
                    while ((p = list_->GetPrev(p)) != nullptr) {
                        total += p->usedLen;
                    }
                }
            }
        }
    } else if (origin == kEnd) {
        // ---- 末尾からの相対シーク（逐語移植, delta=-pos で末尾から遡上）----
        if (pos > 0) return false;
        FileOffset delta = -pos;        // 原 uVar4
        node = list_->GetTail();        // 原 FUN_0041d9d0
        if (node == nullptr) return false;
        off = 0;  // 原コードは local_14 未初期化（delta==0 の縮退時）。UB回避のため 0 で初期化。
        total = 0;
        while (total < delta) {
            int step = node->usedLen;              // 原 local_8: 現ノードの used
            if (delta < total + step) {            // 目標が現ブロック内
                // 0 < delta - total < step <= capacity のため int に収まる。
                off = (step - static_cast<int>(delta - total)) - 1;
                if (outAbs != nullptr) {
                    BlockNode* p = node;
                    total = off;
                    while ((p = list_->GetPrev(p)) != nullptr) total += p->usedLen;
                }
                break;
            }
            BlockNode* pv = list_->GetPrev(node);  // 原 iVar3
            if (pv == nullptr) {                   // 先頭に到達
                if (total + step != delta) return false;
                // delta - total == step（直前の等値判定）のため int 幅で安全。
                off = (step - static_cast<int>(delta - total)) - 1;
                pv = node;                         // 原: 現ノードに留まる(=self)
                if (outAbs != nullptr) {
                    BlockNode* p = node;
                    total = off;
                    while ((p = list_->GetPrev(p)) != nullptr) total += p->usedLen;
                }
                // 原コードは break せず継続（total を上書き後に step 加算する縮退挙動を再現）。
            }
            node = pv;
            total += step;
        }
    } else {
        return false;
    }

    // ---- 共通格納（原 LAB_0041b7d0）----
    curNode_ = node;
    curOffset_ = off;
    if (outAbs != nullptr) {
        *outAbs = total;
    }
    return true;
}

// 注: count>=0 は呼出側の契約（詳細は BlockCursor.h の Read 宣言コメント）。
//   原 BlockCursor_Read(0x0041b7fb) 同様、負値のガードは意図的に置いていない。
//   全呼出側が非負を確定させてから呼ぶため、負値が渡る経路は存在しない。
FileOffset BlockCursor::Read(FileOffset count, void* dstv) {
    unsigned char* dst = static_cast<unsigned char*>(dstv);
    if (curNode_ == nullptr) {
        return 0;
    }
    unsigned char* data = curNode_->data;
    int used = curNode_->usedLen;
    if (curOffset_ < 0 || used <= curOffset_) {
        return 0;
    }
    FileOffset readTotal;
    if (used - curOffset_ < count) {
        // 現ブロックの残り全部を読み、後続ブロックへ跨ぐ。
        const int first = used - curOffset_;
        readTotal = first;
        std::memmove(dst, data + curOffset_, static_cast<size_t>(first));
        count -= first;
        dst += first;
        for (BlockNode* n = list_->GetNext(curNode_); n != nullptr; n = list_->GetNext(n)) {
            unsigned char* d2 = n->data;
            int u2 = n->usedLen;
            // count < u2 のときのみ count 側を採るため int に収まる。
            int chunk = (count < u2) ? static_cast<int>(count) : u2;
            std::memmove(dst, d2, static_cast<size_t>(chunk));
            count -= chunk;
            readTotal += chunk;
            dst += chunk;
            if (count == 0) {
                return readTotal;
            }
        }
    } else {
        // count <= used - curOffset_ <= capacity のため int に収まる。
        const int n = static_cast<int>(count);
        std::memmove(dst, data + curOffset_, static_cast<size_t>(n));
        readTotal = n;
    }
    return readTotal;
}

bool BlockCursor::Insert(FileOffset pos, const void* srcv, FileOffset count) {
    if (!Seek(pos, kBegin, nullptr)) return false;
    if (curNode_ == nullptr) return false;
    unsigned char* data = curNode_->data;
    int capacity = curNode_->capacity;
    int used = curNode_->usedLen;
    if (curOffset_ < 0 || used < curOffset_) return false;  // curOffset==used(EOF追記)は許容
    InsertWorker(used, capacity, data, count, static_cast<const unsigned char*>(srcv));
    return true;
}

void BlockCursor::InsertWorker(int curUsedLen, int capacity, unsigned char* data,
                               FileOffset insertCount, const unsigned char* src) {
    if (capacity < curUsedLen + insertCount) {
        // 現ブロックに収まらない。
        if (curOffset_ < curUsedLen) {
            // カーソルがブロック途中: 後半を新ブロックへ退避(分割)。
            // 原 BlockCursor_InsertWorker(0x0041cd40) はここが `curOffset_ < curUsedLen - 1` で、
            // 「ブロック最終データバイト上(curOffset_ == curUsedLen-1)」がどの分岐にも入らず
            // 末尾バイトを右へずらさないまま後続ブロックへ追記していた（Issue #93）。
            // 挿入バイトと既存の最終バイトが入れ替わるデータ破壊のため、原版から意図的に逸脱する。
            int tailLen = curUsedLen - curOffset_;
            unsigned char* nb = new unsigned char[kBlockCapacity];
            std::memmove(nb, data + curOffset_, static_cast<size_t>(tailLen));
            list_->InsertNodeAfter(curNode_, nb, kBlockCapacity, tailLen);
            // 先頭側の空きに入る分だけ src を書込む（上限 capacity - curOffset_ のため int）。
            const int fill =
                static_cast<int>(std::min<FileOffset>(insertCount, capacity - curOffset_));
            std::memmove(data + curOffset_, src, static_cast<size_t>(fill));
            curNode_->usedLen = curOffset_ + fill;
            insertCount -= fill;
            src += fill;
        } else if (curUsedLen == 0) {
            // 空ブロック: 容量いっぱいまで書込む。
            std::memmove(data, src, static_cast<size_t>(capacity));
            curNode_->usedLen = capacity;
            insertCount -= capacity;
            src += capacity;
        }
        // 残りを 16KB ブロック単位で現ノードの後ろへ順次追加（カーソルは末尾側へ前進）。
        while (insertCount != 0) {
            const int chunk = (insertCount < kBlockCapacity) ? static_cast<int>(insertCount)
                                                             : kBlockCapacity;
            unsigned char* nb = new unsigned char[kBlockCapacity];
            std::memmove(nb, src, static_cast<size_t>(chunk));
            BlockNode* nn = list_->InsertNodeAfter(curNode_, nb, kBlockCapacity, chunk);
            curNode_ = nn;
            src += chunk;
            insertCount -= chunk;
        }
    } else {
        // 空きに収まる: buf[off..] を右シフトして src を挿入。
        // ここでは curUsedLen + insertCount <= capacity のため int に収まる。
        const int n = static_cast<int>(insertCount);
        std::memmove(data + curOffset_ + n, data + curOffset_,
                     static_cast<size_t>(curUsedLen - curOffset_));
        std::memmove(data + curOffset_, src, static_cast<size_t>(n));
        curNode_->usedLen = curUsedLen + n;
    }
}

bool BlockCursor::InsertByte(FileOffset pos, unsigned char b) {
    if (!Seek(pos, kBegin, nullptr)) return false;
    if (curNode_ == nullptr) return false;
    unsigned char* data = curNode_->data;
    int capacity = curNode_->capacity;
    int used = curNode_->usedLen;
    if (curOffset_ < 0 || capacity < curOffset_) return false;

    if (used == capacity) {
        // 満杯ブロックを半分に分割してから挿入。
        int half = capacity / 2;
        unsigned char* nb = new unsigned char[kBlockCapacity];
        if (curOffset_ < half) {
            // 前半側へ挿入 → 新ブロックを手前(Before)へ。
            if (curOffset_ == 0) {
                nb[0] = b;
                std::memmove(nb + 1, data, static_cast<size_t>(half));
            } else {
                std::memmove(nb, data, static_cast<size_t>(curOffset_));
                nb[curOffset_] = b;
                std::memmove(nb + curOffset_ + 1, data + curOffset_,
                             static_cast<size_t>(half - curOffset_));
            }
            // 現ブロックは後半を先頭へ寄せる
            std::memmove(data, data + half, static_cast<size_t>(half));
            curNode_->usedLen = half;
            BlockNode* newNode = list_->InsertNodeBefore(curNode_, nb, kBlockCapacity, half + 1);
            curNode_ = newNode;
            curOffset_ = curOffset_ + 1;
        } else {
            // 後半側へ挿入 → 新ブロックを後ろ(After)へ。
            // 原 BlockCursor_InsertByte(0x0041c238) は curOffset_ == used-1 のとき
            // 「後半全部 + b」と組み立てる特殊分岐を持ち、挿入バイトが既存の最終バイトの
            // 後ろへ回り込んでいた（さらに caret local_1c を未初期化のまま格納していた）。
            // 通常式に同じ入力を通せば正しく並ぶため、特殊分岐ごと廃した（Issue #93）。
            const int newOff = curOffset_ - half;
            std::memmove(nb, data + half, static_cast<size_t>(newOff));
            nb[newOff] = b;
            std::memmove(nb + newOff + 1, data + curOffset_,
                         static_cast<size_t>(used - curOffset_));
            curNode_->usedLen = half;
            BlockNode* newNode = list_->InsertNodeAfter(curNode_, nb, kBlockCapacity, half + 1);
            curNode_ = newNode;
            curOffset_ = newOff;
        }
    } else {
        // 空きあり: 1バイト右シフトして挿入。
        int i = used;
        for (;;) {
            int j = i - 1;
            if (curOffset_ <= j) {
                data[i] = data[j];
                i = j;
            } else {
                break;
            }
        }
        data[curOffset_] = b;
        curNode_->usedLen = used + 1;
        curOffset_ = curOffset_ + 1;
    }
    return true;
}

bool BlockCursor::DeleteByte(FileOffset pos, unsigned char* outByte) {
    if (!Seek(pos, kBegin, nullptr)) return false;
    if (curNode_ == nullptr) return false;
    unsigned char* data = curNode_->data;
    int used = curNode_->usedLen;
    if (curOffset_ < 0 || used <= curOffset_) return false;

    *outByte = data[curOffset_];
    if (used == 1) {
        // ブロックが1バイトのみ: ノードごと除去し、次(なければ前)ノードへ。
        int newOff = 0;
        bool relink = true;
        BlockNode* target = list_->GetNext(curNode_);
        if (target == nullptr) {
            target = list_->GetPrev(curNode_);
            if (target == nullptr) {
                relink = false;  // 唯一のノード → 空ブロックとして残す
            } else {
                newOff = target->usedLen - 1;
            }
        }
        if (relink) {
            unsigned char* old = list_->RemoveNode(curNode_);
            delete[] old;
            curNode_ = target;
            curOffset_ = newOff;
        } else {
            curNode_->usedLen = 0;
        }
    } else {
        // ブロック内左シフト、末尾を0クリア、usedLen--。
        int i = curOffset_;
        while (++i < used) {
            data[i - 1] = data[i];
        }
        data[used - 1] = 0;
        curNode_->usedLen = used - 1;
    }
    return true;
}

// 範囲削除（移植独自。Issue #62）。DeleteByte の反復と同じ結果を、ブロック単位でまとめて作る。
FileOffset BlockCursor::DeleteRange(FileOffset pos, FileOffset count) {
    if (count <= 0) {
        return 0;
    }
    if (!Seek(pos, kBegin, nullptr)) {
        return 0;
    }
    if (curNode_ == nullptr || curOffset_ < 0) {
        return 0;
    }

    FileOffset deleted = 0;
    BlockNode* node = curNode_;
    int off = curOffset_;                    // 最初のノードだけ途中から削る
    while (node != nullptr && count > 0) {
        const int used = node->usedLen;
        BlockNode* next = list_->GetNext(node);   // 除去前に次を控える
        // usedLen==0 のノードは飛ばす。空ノードはリスト唯一のノードのときだけ生じる
        //   （空ファイル読込・全削除後）ため通常は末尾にしか現れないが、仮に途中に
        //   あっても Read（同じく空ノードを読み飛ばす）と整合する側へ倒す。
        if (off < used) {
            const int avail = used - off;
            const int del = (count < static_cast<FileOffset>(avail))
                                ? static_cast<int>(count)
                                : avail;
            if (off == 0 && del == used && list_->Count() > 1) {
                // ノード全体が消える: ノードごと除去（唯一のノードなら空にして残す）。
                unsigned char* old = list_->RemoveNode(node);
                delete[] old;
            } else {
                unsigned char* data = node->data;
                const int tail = used - off - del;   // 削除範囲より後ろに残るバイト数
                if (tail > 0) {
                    std::memmove(data + off, data + off + del, static_cast<size_t>(tail));
                }
                std::memset(data + (used - del), 0, static_cast<size_t>(del));   // 原の末尾0クリア
                node->usedLen = used - del;
            }
            deleted += del;
            count -= del;
        }
        node = next;
        off = 0;
    }

    // カーソルを削除位置へ再解決する（全削除で末尾を越える場合は新しい総長へ寄せる）。
    const FileOffset total = list_->GetTotalLength();
    const FileOffset newPos = (pos > total) ? total : pos;
    if (Seek(newPos, kBegin, nullptr)) {
        curAbs_ = newPos;
    } else {
        curNode_ = list_->GetHead();
        curOffset_ = 0;
        curAbs_ = 0;
    }
    return deleted;
}

bool BlockCursor::GetByteAt(FileOffset pos, unsigned char* out) {
    if (curNode_ == nullptr) {
        return false;
    }
    int used = curNode_->usedLen;       // 原 local_c
    FileOffset delta = pos - curAbs_;   // 原 iVar1（符号付き。64bit 化で 2GB 超の差分も表現可）
    if (curAbs_ < pos) {
        // 前進
        if (delta < used - curOffset_) {
            // 0 < delta < used - curOffset_ <= capacity のため int に収まる。
            curOffset_ += static_cast<int>(delta);
        } else {
            if (!Seek(pos, kBegin, &curAbs_)) return false;
        }
    } else if (pos < curAbs_) {
        // 後退
        if (curOffset_ + delta < 1) {
            if (!Seek(pos, kBegin, &curAbs_)) return false;
        } else {
            // 1 <= curOffset_ + delta かつ delta < 0 のため int に収まる。
            curOffset_ += static_cast<int>(delta);
        }
    }

    // Seek は追記位置として EOF (curOffset_ == usedLen) を許容するが、読取りは実データ内のみ。
    // 検索の内側ループから呼ばれるため、総長の線形走査ではなくノード内境界で判定する。
    if (curOffset_ < 0 || curOffset_ >= curNode_->usedLen) return false;
    *out = curNode_->data[curOffset_];
    curAbs_ = pos;
    return true;
}

bool BlockCursor::SetByteAt(FileOffset pos, unsigned char b) {
    // pos の属するノード/オフセットへ解決して直接代入（上書き編集）。
    if (!Seek(pos, kBegin, nullptr)) return false;
    if (curNode_ == nullptr) return false;
    if (curOffset_ < 0 || curOffset_ >= curNode_->usedLen) return false;  // 実データ位置のみ
    curNode_->data[curOffset_] = b;
    curAbs_ = pos;
    return true;
}

bool BlockCursor::SearchPattern(const unsigned char* pattern, int patternLen, FileOffset* outPos,
                                int direction, FileOffset start, FileOffset end) {
    if (patternLen <= 0) {
        return false;  // 無効入力（原は patternLen>=1 前提）
    }
    const FileOffset total = list_->GetTotalLength();
    if (start < 0 || start >= total) {
        return false;  // GetByteAt が参照できる実データ位置のみを開始位置として受け付ける
    }
    // 終端未指定(0)かつ前方は全長。
    if (direction == kForward && end == 0) {
        end = total;
    }
    // 開始位置へシーク。絶対位置を curAbs_ へ格納（原は out へ this+0xc を渡す）。
    if (!Seek(start, kBegin, &curAbs_)) return false;

    // bad-character スキップ表（値域は 1..patternLen のため int）。
    int skip[256];
    for (int i = 0; i < 256; ++i) skip[i] = patternLen;
    const int last = patternLen - 1;

    if (direction == kForward) {
        for (int i = 0; i < last; ++i) {
            skip[pattern[i]] = (patternLen - i) - 1;
        }
        unsigned char b = 0;
        // start/pos とも 64bit のため (start-1)+patternLen は 2GB 超でも桁溢れしない。
        for (FileOffset pos = (start - 1) + patternLen; pos < end; ) {
            int j = patternLen;
            for (;;) {
                --j;
                if (!GetByteAt(pos, &b)) return false;
                if (b != pattern[j]) break;
                if (j == 0) { *outPos = pos; return true; }
                --pos;
            }
            int shift;
            if ((patternLen - j) < skip[b]) shift = skip[b];
            else shift = patternLen - j;
            pos += shift;
        }
    } else {
        // 後方検索は patternLen-1 <= start のときのみ走査（原挙動）。
        if (last <= start) {
            // 後方走査の bad-character 表（reverse Horspool）。ウィンドウ先頭のバイトを
            //   pattern[i] へ合わせて左へずらすため、i は「パターン内で最も左の出現位置」で
            //   なければならない。原実装は i を昇順に代入して最右の出現位置を採り、さらに
            //   シフト量を max(k+1, skip[b]) としていたため、間にある一致を飛び越えていた
            //   （Issue #71）。降順に代入して最小の i を残す。出現しないバイトは patternLen。
            for (int i = last; i >= 1; --i) {
                skip[pattern[i]] = i;
            }
            unsigned char b = 0;
            for (FileOffset windowStart = start - last; end <= windowStart; ) {
                FileOffset pos = windowStart;
                int k = 0;
                for (;;) {
                    if (!GetByteAt(pos, &b)) return false;
                    if (b != pattern[k]) break;
                    if (k == last) { *outPos = windowStart; return true; }
                    ++pos;
                    ++k;
                }
                // ウィンドウ先頭のバイト。k==0 なら今読んだ b がそれ自身、k>0 なら先頭は
                //   pattern[0] と一致済みなので、読み直さずに決まる。
                const unsigned char head = (k == 0) ? b : pattern[0];
                // skip[] の値域は 1..patternLen なので、ウィンドウは必ず左へ前進する
                //   （原実装にあった「進まない場合の打ち切り」は不要になった）。
                const FileOffset shift = static_cast<FileOffset>(skip[head]);
                if (windowStart - shift < end) return false;   // 最左候補まで調べ終えた
                windowStart -= shift;
            }
        }
    }
    return false;
}

bool BlockCursor::SearchMismatch(unsigned char value, FileOffset* outPos,
                                 int direction, FileOffset start, FileOffset end) {
    // 前方かつ終端未指定(0)は全長（原 param_3==0 && param_5==0 → 全長）。
    if (direction == kForward && end == 0) {
        end = list_->GetTotalLength();
    }
    // 開始位置へシーク（GetByteAt の増分アクセスを始動。原は out へ this+0xc）。
    Seek(start, kBegin, &curAbs_);

    if (direction == kForward) {
        for (FileOffset pos = start; pos < end; ++pos) {
            unsigned char b = 0;
            if (!GetByteAt(pos, &b)) return false;
            if (b != value) { *outPos = pos; return true; }
        }
    } else if (start != 0) {   // 原 else if (param_4 != 0): start==0 の後方は非走査
        for (FileOffset pos = start; end <= pos; --pos) {
            unsigned char b = 0;
            if (!GetByteAt(pos, &b)) return false;
            if (b != value) { *outPos = pos; return true; }
        }
    }
    return false;
}

}  // namespace stirling
