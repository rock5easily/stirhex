"""Generate the clean-room StirHex 54-image toolbar bitmap.

The output is a 4-bpp Windows BMP containing 54 cells of 16x15 pixels. Palette
index 0 is RGB(192, 192, 192), which MFC remaps to COLOR_3DFACE and treats as
transparent in the toolbar settings image list.
"""

from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path


ICON_WIDTH = 16
ICON_HEIGHT = 15
BACKGROUND = 0
NAVY = 1
CYAN = 2
DARK_CYAN = 3
WHITE = 4
LIGHT_GRAY = 5
GREEN = 6
RED = 7
AMBER = 8
PURPLE = 9
MARK1 = 10
MARK2 = 11
MARK3 = 12
DARK_GRAY = 13
BLACK = 14
ORANGE = 15
NAV_BLUE = 15

PALETTE_B_FUNCTIONAL = (
    (192, 192, 192),  # MFC transparent/background key
    (6, 24, 77),      # deep navy
    (37, 217, 242),   # bright cyan
    (11, 141, 184),   # dark cyan
    (255, 255, 255),
    (221, 230, 236),
    (0, 212, 59),     # create/save accent
    (242, 56, 56),    # edit/destructive accent
    (242, 184, 39),   # print accent
    (138, 92, 246),   # compare/structure accent
    (255, 0, 128),    # Mark1 default background
    (0, 255, 0),      # Mark2 default background
    (128, 128, 255),  # Mark3 default background
    (68, 81, 92),
    (0, 0, 0),
    (242, 117, 32),
)

PALETTE_C_SEMANTIC = (
    (192, 192, 192),  # MFC transparent/background key
    (45, 51, 57),     # charcoal outline
    (248, 248, 248),  # neutral main fill
    (138, 146, 154),  # neutral secondary detail
    (255, 255, 255),
    (221, 226, 230),
    (67, 160, 71),    # create/save accent
    (217, 74, 74),    # edit/destructive accent
    (240, 180, 41),   # print/open accent
    (143, 92, 207),   # compare/structure accent
    (255, 0, 128),    # Mark1 default background
    (0, 255, 0),      # Mark2 default background
    (128, 128, 255),  # Mark3 default background
    (79, 86, 92),
    (0, 0, 0),
    (53, 106, 195),   # navigation/search blue
)

PALETTES = {
    "b-functional": PALETTE_B_FUNCTIONAL,
    "c-semantic": PALETTE_C_SEMANTIC,
}

ICON_NAMES = (
    "new", "open", "close", "save", "save_as", "save_close", "save_all", "print",
    "exit", "data_top", "data_bottom", "goto_address", "last_modified",
    "select_to_top", "select_to_bottom", "select_all", "undo", "redo", "cut", "copy",
    "paste", "edit_lock", "overwrite_insert", "number_text", "delete_selection",
    "fill_selection", "revert", "structure", "find", "find_mismatch", "find_next",
    "find_previous", "replace", "compare", "bgrep", "new_window", "cascade",
    "tile_horizontal", "tile_vertical", "arrange_icons", "next_window", "window_list",
    "correct_window_size", "mark1", "mark_next", "mark_previous", "mark_list", "run",
    "help", "print_preview", "mark2", "mark3", "save_dump", "print_range",
)


class Icon:
    def __init__(self) -> None:
        self.pixels = [[BACKGROUND] * ICON_WIDTH for _ in range(ICON_HEIGHT)]

    def pixel(self, x: int, y: int, color: int) -> None:
        if 0 <= x < ICON_WIDTH and 0 <= y < ICON_HEIGHT:
            self.pixels[y][x] = color

    def hline(self, x0: int, x1: int, y: int, color: int) -> None:
        for x in range(min(x0, x1), max(x0, x1) + 1):
            self.pixel(x, y, color)

    def vline(self, x: int, y0: int, y1: int, color: int) -> None:
        for y in range(min(y0, y1), max(y0, y1) + 1):
            self.pixel(x, y, color)

    def line(self, x0: int, y0: int, x1: int, y1: int, color: int) -> None:
        dx = abs(x1 - x0)
        sx = 1 if x0 < x1 else -1
        dy = -abs(y1 - y0)
        sy = 1 if y0 < y1 else -1
        error = dx + dy
        while True:
            self.pixel(x0, y0, color)
            if x0 == x1 and y0 == y1:
                break
            twice = 2 * error
            if twice >= dy:
                error += dy
                x0 += sx
            if twice <= dx:
                error += dx
                y0 += sy

    def rect(self, x0: int, y0: int, x1: int, y1: int,
             outline: int = NAVY, fill: int | None = None) -> None:
        if fill is not None:
            for y in range(y0 + 1, y1):
                self.hline(x0 + 1, x1 - 1, y, fill)
        self.hline(x0, x1, y0, outline)
        self.hline(x0, x1, y1, outline)
        self.vline(x0, y0, y1, outline)
        self.vline(x1, y0, y1, outline)

    def fill_rect(self, x0: int, y0: int, x1: int, y1: int, color: int) -> None:
        for y in range(y0, y1 + 1):
            self.hline(x0, x1, y, color)

    def polygon(self, points: tuple[tuple[int, int], ...], fill: int, outline: int = NAVY) -> None:
        min_x = min(x for x, _ in points)
        max_x = max(x for x, _ in points)
        min_y = min(y for _, y in points)
        max_y = max(y for _, y in points)
        for y in range(min_y, max_y + 1):
            for x in range(min_x, max_x + 1):
                inside = False
                previous = points[-1]
                for current in points:
                    x1, y1 = previous
                    x2, y2 = current
                    if (y1 > y) != (y2 > y):
                        cross = (x2 - x1) * (y - y1) / (y2 - y1) + x1
                        if x < cross:
                            inside = not inside
                    previous = current
                if inside:
                    self.pixel(x, y, fill)
        for first, second in zip(points, points[1:] + points[:1]):
            self.line(first[0], first[1], second[0], second[1], outline)


def plus(icon: Icon, x: int, y: int, color: int = GREEN) -> None:
    icon.hline(x - 2, x + 2, y, NAVY)
    icon.vline(x, y - 2, y + 2, NAVY)
    icon.hline(x - 1, x + 1, y, color)
    icon.vline(x, y - 1, y + 1, color)


def cross(icon: Icon, x: int, y: int, color: int = RED) -> None:
    for offset in (-1, 0, 1):
        icon.pixel(x + offset, y + offset, color)
        icon.pixel(x + offset, y - offset, color)


def document(icon: Icon, x: int = 2, y: int = 1, fill: int = CYAN) -> None:
    points = ((x, y), (x + 7, y), (x + 10, y + 3), (x + 10, y + 12), (x, y + 12))
    icon.polygon(points, fill)
    icon.line(x + 7, y, x + 7, y + 3, NAVY)
    icon.line(x + 7, y + 3, x + 10, y + 3, NAVY)


def folder(icon: Icon, x: int = 1, y: int = 3, fill: int = CYAN) -> None:
    points = ((x, y), (x + 4, y), (x + 5, y + 2), (x + 13, y + 2),
              (x + 12, y + 10), (x, y + 10))
    icon.polygon(points, fill)
    icon.line(x + 1, y + 4, x + 11, y + 4, DARK_CYAN)


def disk(icon: Icon, accent: int = GREEN, x: int = 2, y: int = 1) -> None:
    icon.rect(x, y, x + 11, y + 12, NAVY, CYAN)
    icon.rect(x + 2, y, x + 8, y + 4, NAVY, WHITE)
    icon.vline(x + 7, y + 1, y + 3, DARK_GRAY)
    icon.rect(x + 2, y + 7, x + 9, y + 11, NAVY, WHITE)
    icon.fill_rect(x + 6, y + 8, x + 8, y + 9, accent)


def printer(icon: Icon, accent: int = AMBER) -> None:
    icon.rect(4, 1, 11, 5, NAVY, WHITE)
    icon.rect(2, 5, 13, 11, NAVY, CYAN)
    icon.rect(4, 9, 11, 13, NAVY, WHITE)
    icon.fill_rect(10, 6, 11, 7, accent)


def arrow(icon: Icon, direction: str, color: int = CYAN,
          x_offset: int = 0, y_offset: int = 0) -> None:
    right = ((2, 5), (8, 5), (8, 2), (13, 7), (8, 12), (8, 9), (2, 9))
    if direction == "left":
        points = tuple((15 - x, y) for x, y in right)
    elif direction == "up":
        points = tuple((y, 15 - x) for x, y in right)
    elif direction == "down":
        points = tuple((15 - y, x) for x, y in right)
    else:
        points = right
    icon.polygon(tuple((x + x_offset, y + y_offset) for x, y in points), color)


def curved_arrow(icon: Icon, direction: str) -> None:
    if direction == "left":
        points = ((2, 6), (6, 2), (6, 5), (10, 5), (12, 7), (12, 11),
                  (10, 13), (10, 9), (9, 8), (6, 8), (6, 11))
    else:
        points = tuple((15 - x, y) for x, y in
                       ((2, 6), (6, 2), (6, 5), (10, 5), (12, 7), (12, 11),
                        (10, 13), (10, 9), (9, 8), (6, 8), (6, 11)))
    icon.polygon(points, CYAN)


def magnifier(icon: Icon, x: int = 2, y: int = 1, accent: int = CYAN) -> None:
    ring = ((x + 2, y), (x + 6, y), (x + 8, y + 2), (x + 8, y + 6),
            (x + 6, y + 8), (x + 2, y + 8), (x, y + 6), (x, y + 2))
    icon.polygon(ring, WHITE)
    icon.rect(x + 2, y + 2, x + 6, y + 6, accent, BACKGROUND)
    icon.line(x + 7, y + 7, x + 12, y + 12, NAVY)
    icon.line(x + 8, y + 7, x + 12, y + 11, accent)


def flag(icon: Icon, color: int, x: int = 3, y: int = 1) -> None:
    icon.vline(x, y, y + 12, NAVY)
    icon.polygon(((x + 1, y), (x + 9, y), (x + 7, y + 4), (x + 9, y + 8), (x + 1, y + 8)), color)


def window(icon: Icon, x: int, y: int, width: int, height: int,
           fill: int = WHITE) -> None:
    icon.rect(x, y, x + width, y + height, NAVY, fill)
    icon.hline(x + 1, x + width - 1, y + 2, DARK_CYAN)


def selection(icon: Icon, x0: int = 2, y0: int = 2, x1: int = 13, y1: int = 12) -> None:
    for x in range(x0, x1 + 1, 2):
        icon.pixel(x, y0, NAVY)
        icon.pixel(x, y1, NAVY)
    for y in range(y0, y1 + 1, 2):
        icon.pixel(x0, y, NAVY)
        icon.pixel(x1, y, NAVY)


def three_mark_tabs(icon: Icon, x: int = 1, y: int = 3) -> None:
    for offset, color in ((0, MARK1), (4, MARK2), (8, MARK3)):
        icon.rect(x + offset, y, x + offset + 3, y + 7, NAVY, color)


def draw_icon(index: int) -> Icon:
    icon = Icon()

    if index == 0:  # new
        document(icon)
        plus(icon, 12, 10)
    elif index == 1:  # open
        folder(icon)
        icon.polygon(((3, 6), (14, 6), (12, 13), (1, 13)), CYAN)
    elif index == 2:  # close
        document(icon, fill=WHITE)
        cross(icon, 11, 10)
    elif index == 3:  # save
        disk(icon)
    elif index == 4:  # save as
        disk(icon)
        icon.polygon(((9, 9), (13, 9), (13, 7), (15, 11), (13, 14), (13, 12), (9, 12)), GREEN)
    elif index == 5:  # save and close
        disk(icon)
        cross(icon, 12, 11)
    elif index == 6:  # save all
        disk(icon, x=0, y=2)
        icon.rect(6, 1, 14, 11, NAVY, DARK_CYAN)
        icon.rect(8, 6, 13, 10, NAVY, WHITE)
        icon.fill_rect(10, 7, 12, 8, GREEN)
    elif index == 7:
        printer(icon)
    elif index == 8:  # exit
        icon.rect(2, 1, 9, 13, NAVY, WHITE)
        icon.vline(5, 3, 11, CYAN)
        icon.polygon(((7, 5), (11, 5), (11, 3), (15, 7), (11, 11), (11, 9), (7, 9)), RED)
    elif index == 9:  # data top
        arrow(icon, "up")
        icon.hline(2, 13, 1, NAVY)
    elif index == 10:  # data bottom
        arrow(icon, "down")
        icon.hline(2, 13, 13, NAVY)
    elif index == 11:  # go to address
        icon.rect(1, 3, 8, 11, NAVY, WHITE)
        icon.hline(3, 6, 7, DARK_CYAN)
        icon.vline(5, 5, 9, DARK_CYAN)
        icon.polygon(((8, 5), (12, 5), (12, 3), (15, 7), (12, 11), (12, 9), (8, 9)), CYAN)
    elif index == 12:  # last modified
        document(icon, fill=WHITE)
        icon.fill_rect(5, 5, 8, 8, MARK1)
        icon.line(10, 11, 13, 8, CYAN)
        icon.line(10, 11, 13, 12, CYAN)
    elif index == 13:  # select to top
        selection(icon, 4, 3, 13, 12)
        icon.polygon(((0, 6), (3, 2), (6, 6), (4, 6), (4, 10), (2, 10), (2, 6)), CYAN)
    elif index == 14:  # select to bottom
        selection(icon, 4, 2, 13, 11)
        icon.polygon(((0, 8), (2, 8), (2, 4), (4, 4), (4, 8), (6, 8), (3, 12)), CYAN)
    elif index == 15:
        selection(icon)
        icon.fill_rect(5, 5, 10, 9, DARK_CYAN)
    elif index == 16:
        curved_arrow(icon, "left")
    elif index == 17:
        curved_arrow(icon, "right")
    elif index == 18:  # cut
        icon.line(3, 2, 11, 12, NAVY)
        icon.line(11, 2, 3, 12, NAVY)
        icon.line(4, 2, 11, 11, CYAN)
        icon.line(10, 2, 3, 11, CYAN)
        icon.rect(1, 10, 5, 14, NAVY, WHITE)
        icon.rect(9, 10, 13, 14, NAVY, WHITE)
        icon.fill_rect(3, 11, 4, 12, RED)
        icon.fill_rect(10, 11, 11, 12, RED)
    elif index == 19:  # copy
        document(icon, 4, 1, WHITE)
        icon.rect(1, 4, 8, 13, NAVY, CYAN)
    elif index == 20:  # paste
        icon.rect(3, 3, 12, 13, NAVY, WHITE)
        icon.rect(5, 1, 10, 4, NAVY, CYAN)
        icon.hline(5, 10, 7, DARK_CYAN)
        icon.hline(5, 10, 9, DARK_CYAN)
        icon.hline(5, 9, 11, DARK_CYAN)
    elif index == 21:  # edit lock
        icon.rect(3, 6, 12, 13, NAVY, CYAN)
        icon.rect(5, 1, 10, 8, NAVY, BACKGROUND)
        icon.fill_rect(7, 9, 8, 11, RED)
    elif index == 22:  # overwrite/insert
        icon.rect(1, 3, 8, 10, NAVY, WHITE)
        icon.rect(7, 5, 14, 12, NAVY, CYAN)
        icon.line(3, 12, 12, 2, GREEN)
        icon.pixel(11, 2, GREEN)
        icon.pixel(12, 3, GREEN)
    elif index == 23:  # number/text toggle
        icon.rect(1, 2, 7, 12, NAVY, WHITE)
        icon.rect(8, 2, 14, 12, NAVY, CYAN)
        icon.fill_rect(3, 4, 5, 5, DARK_CYAN)
        icon.fill_rect(3, 8, 5, 9, DARK_CYAN)
        icon.line(10, 10, 11, 4, WHITE)
        icon.line(11, 4, 13, 10, WHITE)
        icon.hline(10, 13, 8, WHITE)
    elif index == 24:  # delete selection
        selection(icon)
        icon.line(5, 5, 10, 10, RED)
        icon.line(10, 5, 5, 10, RED)
    elif index == 25:  # fill selection
        selection(icon)
        icon.fill_rect(4, 7, 11, 10, CYAN)
        icon.polygon(((5, 3), (9, 3), (11, 6), (8, 8), (4, 6)), WHITE)
    elif index == 26:  # revert
        document(icon, fill=WHITE)
        curved_arrow(icon, "left")
    elif index == 27:  # structure
        icon.rect(2, 1, 6, 4, NAVY, PURPLE)
        icon.rect(1, 10, 5, 13, NAVY, PURPLE)
        icon.rect(6, 10, 10, 13, NAVY, CYAN)
        icon.rect(11, 10, 15, 13, NAVY, PURPLE)
        icon.vline(4, 4, 7, NAVY)
        icon.hline(3, 13, 7, NAVY)
        icon.vline(3, 7, 10, NAVY)
        icon.vline(8, 7, 10, NAVY)
        icon.vline(13, 7, 10, NAVY)
    elif index == 28:
        magnifier(icon)
    elif index == 29:  # mismatch find
        magnifier(icon)
        icon.line(3, 7, 8, 2, RED)
        icon.pixel(6, 6, RED)
    elif index == 30:  # find next
        magnifier(icon, 0, 1)
        icon.polygon(((9, 8), (13, 8), (13, 6), (15, 10), (13, 14), (13, 12), (9, 12)), CYAN)
    elif index == 31:
        magnifier(icon, 5, 1)
        icon.polygon(((6, 8), (2, 8), (2, 6), (0, 10), (2, 14), (2, 12), (6, 12)), CYAN)
    elif index == 32:  # replace
        icon.polygon(((2, 3), (10, 3), (10, 1), (14, 5), (10, 9), (10, 7), (2, 7)), RED)
        icon.polygon(((14, 8), (6, 8), (6, 6), (2, 10), (6, 14), (6, 12), (14, 12)), CYAN)
    elif index == 33:  # compare
        document(icon, 0, 1, CYAN)
        document(icon, 5, 1, WHITE)
        icon.fill_rect(5, 6, 9, 8, PURPLE)
    elif index == 34:  # bgrep
        folder(icon, 0, 3)
        magnifier(icon, 7, 5, accent=WHITE)
    elif index == 35:  # new window
        window(icon, 1, 4, 9, 9, WHITE)
        window(icon, 5, 1, 9, 9, CYAN)
        plus(icon, 12, 11)
    elif index == 36:  # cascade
        window(icon, 1, 5, 9, 8, LIGHT_GRAY)
        window(icon, 3, 3, 9, 8, WHITE)
        window(icon, 5, 1, 9, 8, CYAN)
    elif index == 37:  # tile horizontal
        window(icon, 1, 1, 13, 5, CYAN)
        window(icon, 1, 8, 13, 5, WHITE)
    elif index == 38:  # tile vertical
        window(icon, 1, 2, 6, 11, CYAN)
        window(icon, 8, 2, 6, 11, WHITE)
    elif index == 39:  # arrange icons
        icon.hline(1, 14, 12, NAVY)
        for x in (1, 5, 9, 13):
            icon.rect(x, 8, min(x + 2, 15), 11, NAVY, CYAN)
        icon.polygon(((2, 5), (11, 5), (11, 3), (14, 6), (11, 9), (11, 7), (2, 7)), DARK_CYAN)
    elif index == 40:  # next window
        window(icon, 1, 3, 9, 9, WHITE)
        window(icon, 4, 1, 9, 9, CYAN)
        icon.polygon(((9, 8), (13, 8), (13, 6), (15, 10), (13, 14), (13, 12), (9, 12)), GREEN)
    elif index == 41:  # window list
        window(icon, 1, 1, 13, 12, WHITE)
        for y in (5, 8, 11):
            icon.fill_rect(3, y, 4, y + 1, CYAN)
            icon.hline(6, 12, y, NAVY)
    elif index == 42:  # correct window size
        window(icon, 3, 3, 9, 8, WHITE)
        icon.line(3, 3, 0, 0, CYAN)
        icon.line(12, 3, 15, 0, CYAN)
        icon.line(3, 11, 0, 14, CYAN)
        icon.line(12, 11, 15, 14, CYAN)
    elif index == 43:
        flag(icon, MARK1)
    elif index == 44:  # next mark
        three_mark_tabs(icon, 0, 4)
        icon.polygon(((10, 5), (13, 5), (13, 3), (15, 7), (13, 11), (13, 9), (10, 9)), CYAN)
    elif index == 45:
        three_mark_tabs(icon, 4, 4)
        icon.polygon(((5, 5), (2, 5), (2, 3), (0, 7), (2, 11), (2, 9), (5, 9)), CYAN)
    elif index == 46:  # mark list
        for y, color in ((2, MARK1), (6, MARK2), (10, MARK3)):
            icon.fill_rect(1, y, 3, y + 2, color)
            icon.rect(1, y, 3, y + 2, NAVY, color)
            icon.hline(6, 14, y + 1, NAVY)
    elif index == 47:  # run
        window(icon, 1, 2, 13, 11, WHITE)
        icon.polygon(((4, 5), (4, 11), (10, 8)), GREEN)
        icon.hline(10, 13, 11, DARK_CYAN)
    elif index == 48:  # help
        icon.polygon(((5, 1), (10, 1), (13, 4), (13, 10),
                      (10, 13), (5, 13), (2, 10), (2, 4)), CYAN)
        icon.hline(6, 9, 3, WHITE)
        icon.pixel(5, 4, WHITE)
        icon.vline(10, 4, 6, WHITE)
        icon.pixel(9, 7, WHITE)
        icon.pixel(8, 8, WHITE)
        icon.pixel(8, 10, WHITE)
        icon.pixel(8, 12, WHITE)
    elif index == 49:  # print preview
        document(icon, 0, 1, WHITE)
        magnifier(icon, 6, 5, accent=AMBER)
    elif index == 50:
        flag(icon, MARK2)
    elif index == 51:
        flag(icon, MARK3)
    elif index == 52:  # save dump
        disk(icon)
        icon.rect(8, 6, 14, 13, NAVY, WHITE)
        for y in (8, 10, 12):
            icon.hline(9, 13, y, DARK_CYAN)
    elif index == 53:  # print range
        printer(icon)
        selection(icon, 8, 7, 15, 14)
        icon.fill_rect(10, 9, 13, 11, AMBER)
    else:
        raise ValueError(f"invalid toolbar index: {index}")

    return icon


def apply_c_semantic_theme(icons: list[Icon]) -> None:
    # The drawing recipes use CYAN/DARK_CYAN as semantic base/detail slots. The C
    # theme converts those slots per command category while preserving the same
    # clean-room silhouettes and fixed image indexes.
    blue_icons = {
        3, 4, 5, 6, 9, 10, 11, 12, 13, 14, 16, 17, 22, 25, 26,
        28, 29, 30, 31, 32, 35, 36, 37, 38, 39, 40, 42, 44, 45,
        48, 52,
    }
    amber_icons = {1, 34}
    gray_icons = {7, 49, 53}
    for index, icon in enumerate(icons):
        if index in blue_icons:
            primary = NAV_BLUE
        elif index in amber_icons:
            primary = AMBER
        elif index in gray_icons:
            primary = LIGHT_GRAY
        else:
            primary = WHITE
        for y in range(ICON_HEIGHT):
            for x in range(ICON_WIDTH):
                color = icon.pixels[y][x]
                if color == CYAN:
                    icon.pixels[y][x] = primary
                elif color == DARK_CYAN:
                    icon.pixels[y][x] = DARK_GRAY


def generate_icons(theme: str) -> list[Icon]:
    icons = [draw_icon(index) for index in range(len(ICON_NAMES))]
    if theme == "c-semantic":
        apply_c_semantic_theme(icons)
    if len(icons) != 54:
        raise RuntimeError(f"expected 54 icons, got {len(icons)}")
    signatures = {bytes(color for row in icon.pixels for color in row) for icon in icons}
    if len(signatures) != len(icons):
        raise RuntimeError("toolbar contains duplicate icon pixel data")
    for index, icon in enumerate(icons):
        if all(color == BACKGROUND for row in icon.pixels for color in row):
            raise RuntimeError(f"icon {index} ({ICON_NAMES[index]}) is empty")
    return icons


def write_bmp(path: Path, icons: list[Icon], palette: tuple[tuple[int, int, int], ...]) -> None:
    width = ICON_WIDTH * len(icons)
    row_bytes = ((width + 1) // 2 + 3) & ~3
    image_size = row_bytes * ICON_HEIGHT
    pixel_offset = 14 + 40 + len(palette) * 4
    file_size = pixel_offset + image_size

    output = bytearray()
    output.extend(struct.pack("<2sIHHI", b"BM", file_size, 0, 0, pixel_offset))
    output.extend(struct.pack("<IiiHHIIiiII", 40, width, ICON_HEIGHT, 1, 4,
                              0, image_size, 0, 0, len(palette), len(palette)))
    for red, green, blue in palette:
        output.extend(struct.pack("<BBBB", blue, green, red, 0))

    rows = []
    for y in range(ICON_HEIGHT):
        colors = [color for icon in icons for color in icon.pixels[y]]
        row = bytearray((colors[i] << 4) | colors[i + 1] for i in range(0, width, 2))
        row.extend(b"\0" * (row_bytes - len(row)))
        rows.append(row)
    for row in reversed(rows):
        output.extend(row)

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(output)


def write_preview(path: Path, icons: list[Icon], palette: tuple[tuple[int, int, int], ...],
                  scale: int = 6) -> None:
    columns = 9
    rows = 6
    padding = 4
    cell_width = ICON_WIDTH * scale + padding * 2
    cell_height = ICON_HEIGHT * scale + padding * 2
    width = columns * cell_width
    height = rows * cell_height
    pixels = bytearray([240, 240, 240, 255] * (width * height))

    for index, icon in enumerate(icons):
        cell_x = (index % columns) * cell_width + padding
        cell_y = (index // columns) * cell_height + padding
        for y in range(ICON_HEIGHT):
            for x in range(ICON_WIDTH):
                red, green, blue = palette[icon.pixels[y][x]]
                for sy in range(scale):
                    for sx in range(scale):
                        px = cell_x + x * scale + sx
                        py = cell_y + y * scale + sy
                        offset = (py * width + px) * 4
                        pixels[offset:offset + 4] = bytes((red, green, blue, 255))

    def chunk(kind: bytes, payload: bytes) -> bytes:
        data = kind + payload
        return struct.pack(">I", len(payload)) + data + struct.pack(">I", zlib.crc32(data))

    scanlines = b"".join(
        b"\0" + pixels[y * width * 4:(y + 1) * width * 4] for y in range(height)
    )
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
           chunk(b"IDAT", zlib.compress(scanlines, 9)) + chunk(b"IEND", b""))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


def main() -> None:
    project_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path,
                        default=project_root / "StirHex" / "res" / "bmp_128.bmp")
    parser.add_argument("--preview", type=Path)
    parser.add_argument("--theme", choices=tuple(PALETTES), default="c-semantic")
    args = parser.parse_args()

    icons = generate_icons(args.theme)
    palette = PALETTES[args.theme]
    write_bmp(args.output, icons, palette)
    if args.preview is not None:
        write_preview(args.preview, icons, palette)
    print(f"generated {args.output}: {len(icons)} icons, "
          f"{ICON_WIDTH * len(icons)}x{ICON_HEIGHT}, 4-bpp, theme={args.theme}")
    if args.preview is not None:
        print(f"generated preview {args.preview}")


if __name__ == "__main__":
    main()
