// ユーザーメニュー機能カタログ実装（UserMenuCatalog.h 参照）。
//   rawID→cmdID は原 DAT_004b51e6（cmdID = cat*0x48 + item*4 の先頭u16）から抽出した確定表。
//   名称は原の全カテゴリ・フルコマンド集合（環境設定ダイアログの kUmCatalog と同一文字列）。
#include "pch.h"

#include "frame/UserMenuCatalog.h"

#include "app/AppSettings.h"   // CAppSettings::kToolbarSep(0xFFFF) と共通のセパレータ値
#include "app/UiStrings.h"     // 機能名は文字列リソースから引く（ui::CommandNameW）

namespace {

struct Entry { UINT raw; UINT cmd; };   // 名称は文字列リソース（ui::CommandNameW）

// rawID = (カテゴリ<<8)|項目。原 DAT_004b51e6 由来の112項目に、移植で追加した機能を続ける。
const Entry kEntries[] = {
    { 0x0001, 0xE100 },
    { 0x0002, 0xE101 },
    { 0x0003, 0xE102 },
    { 0x0004, 0xE103 },
    { 0x0005, 0xE104 },
    { 0x0006, 0x8004 },
    { 0x0007, 0x8005 },
    { 0x0008, 0x8006 },
    { 0x0009, 0x8007 },
    { 0x000A, 0xE107 },
    { 0x000B, 0xE109 },
    { 0x000C, 0xE106 },
    { 0x000D, 0x8008 },
    { 0x000E, 0x8009 },
    { 0x000F, 0xE141 },
    { 0x0010, 0x8060 },
    { 0x0011, 0x8064 },
    { 0x0100, 0x800A },
    { 0x0101, 0x800B },
    { 0x0102, 0x800C },
    { 0x0103, 0x800D },
    { 0x0104, 0x800E },
    { 0x0105, 0x800F },
    { 0x0106, 0x8010 },
    { 0x0107, 0x8011 },
    { 0x0108, 0x8012 },
    { 0x0109, 0x8013 },
    { 0x010A, 0x8014 },
    { 0x010B, 0x8015 },
    { 0x010C, 0x8016 },
    { 0x010D, 0x8017 },
    { 0x010E, 0x8018 },
    { 0x010F, 0x8019 },
    { 0x0110, 0x801A },
    { 0x0111, 0x801B },
    { 0x0200, 0x801C },
    { 0x0201, 0x801D },
    { 0x0202, 0x801E },
    { 0x0203, 0x801F },
    { 0x0204, 0x8020 },
    { 0x0205, 0x8021 },
    { 0x0206, 0x8022 },
    { 0x0207, 0x8023 },
    { 0x0208, 0x8024 },
    { 0x0209, 0xE12A },
    { 0x020A, 0x805D },
    { 0x020B, 0x805E },
    { 0x020C, 0x8065 },
    { 0x0300, 0xE12B },
    { 0x0301, 0xE12C },
    { 0x0302, 0xE123 },
    { 0x0303, 0xE122 },
    { 0x0304, 0xE125 },
    { 0x0305, 0x8025 },
    { 0x0306, 0x8026 },
    { 0x0307, 0x8027 },
    { 0x0308, 0x8028 },
    { 0x0309, 0x8029 },
    { 0x030A, 0x802A },
    { 0x030B, 0x802B },
    { 0x030C, 0x802C },
    { 0x030D, 0x802D },
    { 0x030E, 0x802E },
    { 0x030F, 0x8061 },
    { 0x0400, 0xE124 },
    { 0x0401, 0x802F },
    { 0x0402, 0x8030 },
    { 0x0403, 0x8031 },
    { 0x0404, 0x8032 },
    { 0x0405, 0x8033 },
    { 0x0406, 0x8034 },
    { 0x0407, 0x8035 },
    { 0x0408, 0x8037 },
    { 0x0409, 0x8036 },
    { 0x040A, 0xE129 },
    { 0x040B, 0x8038 },
    { 0x040C, 0x8039 },
    { 0x040D, 0x805F },
    { 0x0500, 0x803A },
    { 0x0501, 0x803B },
    { 0x0502, 0x803C },
    { 0x0503, 0x803D },
    { 0x0504, 0x803E },
    { 0x0505, 0x803F },
    { 0x0506, 0x8040 },
    { 0x0507, 0x8041 },
    { 0x0508, 0x8042 },
    { 0x0509, 0x8043 },
    { 0x050A, 0x8044 },
    { 0x050B, 0x8045 },
    { 0x050C, 0x8046 },
    { 0x0600, 0xE130 },
    { 0x0601, 0xE132 },
    { 0x0602, 0xE133 },
    { 0x0603, 0xE134 },
    { 0x0604, 0xE131 },
    { 0x0605, 0x8047 },
    { 0x0606, 0x8048 },
    { 0x0607, 0x8049 },
    { 0x0700, 0x804A },
    { 0x0701, 0x804B },
    { 0x0702, 0x804C },
    { 0x0703, 0x804D },
    { 0x0704, 0x804E },
    { 0x0705, 0x804F },
    { 0x0706, 0x8050 },
    { 0x0707, 0x8051 },
    { 0x0708, 0x8052 },
    { 0x0709, 0x8057 },
    { 0x070A, 0x805A },
    { 0x070B, 0x8062 },
    { 0x070C, 0x8063 },
    // 移植で追加した機能（原の表には無い。Issue #97）
    { 0x070D, 0x80F8 },   // 16進テキスト貼り付け
    { 0x070E, 0x80FA },   // マークの書き出し...
    { 0x070F, 0x80FB },   // マークの読み込み...
};

const Entry* Find(UINT raw) {
    for (const auto& e : kEntries) {
        if (e.raw == raw) { return &e; }
    }
    return nullptr;
}

}  // namespace

UINT UserMenuRawToCmd(UINT rawId) {
    const Entry* e = Find(rawId);
    return (e != nullptr) ? e->cmd : 0;
}

CStringW UserMenuRawToName(UINT rawId) {
    return (Find(rawId) != nullptr) ? ui::CommandNameW(rawId) : CStringW();
}

bool BuildUserPopup(const std::vector<UINT>& items, CMenu& outMenu) {
    if (!outMenu.CreatePopupMenu()) { return false; }
    int added = 0;
    for (UINT item : items) {
        const UINT raw = CAppSettings::UmRaw(item);      // 下位16bit=rawID（上位=アクセラレータ）
        if (raw == CAppSettings::kUserMenuSep) {          // 0xFFFF = セパレータ
            outMenu.AppendMenu(MF_SEPARATOR);
            continue;
        }
        const Entry* e = Find(raw);
        if (e == nullptr || e->cmd == 0) { continue; }   // 未定義rawIDはスキップ（安全側）
        // アクセラレータがあれば「&X<TAB><名称>」形式（原 FUN_00465c40 の "&%c\t" ）で表示。
        const UINT accel = CAppSettings::UmAccel(item);
        const CStringW name = ui::CommandNameW(raw);   // 原 文字列表 5100+cat*100+item
        CStringW label;
        if (accel != 0) { label.Format(L"&%c\t%s", (wchar_t)accel, (LPCWSTR)name); }
        else            { label = name; }
        outMenu.AppendMenu(MF_STRING, e->cmd, label);
        ++added;
    }
    return added > 0;
}
