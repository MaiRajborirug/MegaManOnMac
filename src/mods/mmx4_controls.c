#include "cpu_state.h"
#include "mod_plugins.h"
#include "psx_menu_host.h"
#include "recomp_ui_psx_menu.h"

#include <stdint.h>
#include <math.h>
#include <string.h>

/* OPTION -> BUTTON CONFIG runs dispatcher 0x8002A41C, whose handler is picked
 * by the state byte below: 0 init (0x80029E1C), 1 run (0x8002A098), 2 exit
 * (0x8002A394). Exit returns to OPTION and only clears sprite slots, so the
 * Controls page replaces init + run and hands over to Capcom's exit. */
#define MMX4_CONTROLS_PLUGIN "mmx4.controls-page"
#define CONFIG_STATE 0x8013B7D0u
#define CONFIG_INIT 0x80029E1Cu
#define CONFIG_RUN 0x8002A098u
#define CONFIG_STATE_RUN 1u
#define CONFIG_STATE_EXIT 2u

/* sfx(0, id, 0): Capcom's menu sounds, as OPTION plays them. */
#define MENU_SFX 0x8001540Cu
#define SFX_CURSOR 0x0Cu
#define SFX_SELECT 0x22u

/* Capcom's row order; each action is the DualShock input X4's default action
 * table reads for it. */
static const RecompUiPsxControlAction kActions[] = {
    {"X-Buster, weapon\nZ-Saber", "square"},
    {"Jump", "cross"},
    {"Dash", "circle"},
    {"Giga attack", "r2"},
    {"X-Buster\nLearned move", "triangle"},
    {"Weapon select R", "r1"},
    {"Weapon select L", "l1"},
};

static int s_page_owns_screen;

/* ---- skin: X4's own menu art, read from VRAM while OPTION is up ----------
 * Font page: 4bpp texture page at (960,256), 16x16 cells, three sets:
 *   v 0..63    title set: 0-9 A-Z, L1 L2 R1 R2 face buttons + - at v 32,
 *              punctuation at v 48 (、 。 ・ ， … • ! ?)
 *   v 64..143  small mixed-case set (proportional)
 *   v 144..191 item set, 8 pixels per character
 * Selector diamond frames and the backdrop tiles share page (896,256).
 * CLUTs sit on rows 480/481. Atlas rows (256 wide): */
#define FONT_PAGE_X 960
#define ART_PAGE_X 896
#define PAGE_Y 256
#define AT_FONT 0       /* whole font page, white CLUT          (192 rows) */
#define AT_ORANGE 192   /* item set, selected CLUT               (48 rows) */
#define AT_TITLE 240    /* title set, title CLUT                 (64 rows) */
#define AT_BUTTON 304   /* title set row v 32, button CLUT       (16 rows) */
#define AT_ART 320      /* diamond frames u 0..47; synthesised parens (16 rows) */
#define AT_BACKDROP 336 /* backdrop tiles uv (0,208) 208x48      (48 rows) */
#define ATLAS_W 256
#define ATLAS_H 384
#define PAREN_X 64      /* ( ) white at 64/72, selected at 80/88 in the AT_ART row */

static uint32_t s_atlas[ATLAS_W * ATLAS_H];
static uint16_t s_vram[256 * 192];
static RecompUiPsxControlsSkin s_skin;

static uint32_t rgba_of(uint16_t c) {
    uint32_t r = c & 31u, g = (c >> 5) & 31u, b = (c >> 10) & 31u;
    if (!c) return 0;  /* only 0x0000 is transparent */
    r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
    return r | (g << 8) | (b << 16) | 0xFF000000u;
}

/* Decode a 4bpp region (texels u0..u0+w, v0..v0+h of a page) through one
 * CLUT into the atlas at (ax, ay). */
static int decode(int page_x, int u0, int v0, int w, int h, int clut_x, int clut_y, int ax, int ay) {
    uint16_t clut[16];
    if (!psx_mod_read_vram(clut_x, clut_y, 16, 1, clut)) return 0;
    if (!psx_mod_read_vram(page_x + u0 / 4, PAGE_Y + v0, w / 4, h, s_vram)) return 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint16_t word = s_vram[y * (w / 4) + x / 4];
            s_atlas[(ay + y) * ATLAS_W + ax + x] = rgba_of(clut[(word >> ((x & 3) * 4)) & 15u]);
        }
    return 1;
}

static void glyph(RecompUiPsxFont* f, char c, int x, int y, int w) {
    RecompUiPsxRect r = {(uint16_t)x, (uint16_t)y, (uint16_t)w, 16};
    f->glyph[(unsigned char)c - 32] = r;
}

/* 0-9 A-Z laid out 16 per row from (0, y). */
static void alnum(RecompUiPsxFont* f, int y, int w) {
    for (int i = 0; i < 36; ++i)
        glyph(f, i < 10 ? (char)('0' + i) : (char)('A' + i - 10), (i % 16) * 16, y + (i / 16) * 16, w);
}

/* Lit columns of a 16x16 atlas cell, for the proportional set. */
static void trimmed(RecompUiPsxFont* f, char c, int cx, int cy) {
    int lo = 16, hi = -1;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            if (s_atlas[(cy + y) * ATLAS_W + cx + x]) { if (x < lo) lo = x; if (x > hi) hi = x; }
    if (hi >= 0) glyph(f, c, cx + lo, cy, hi - lo + 1);
}

static void small_font(RecompUiPsxFont* f) {
    static const char kPunct[] = {'+', 3, '-', 4, ',', 5, '.', 6, '!', 7, '?', 8, '"', 9, '\'', 11};
    memset(f, 0, sizeof *f);
    for (int i = 0; i < 36; ++i)
        trimmed(f, i < 10 ? (char)('0' + i) : (char)('A' + i - 10), (i % 16) * 16, 64 + (i / 16) * 16);
    for (int i = 0; i < 26; ++i) {
        const int cell = 36 + i;  /* a follows W X Y Z on the v 96 row */
        trimmed(f, (char)('a' + i), (cell % 16) * 16, 64 + (cell / 16) * 16);
    }
    trimmed(f, '/', 14 * 16, 112);
    for (size_t i = 0; i < sizeof kPunct; i += 2) trimmed(f, kPunct[i], kPunct[i + 1] * 16, 128);
    f->line_height = 16;
}

static void item_font(RecompUiPsxFont* f, int y, int paren_x) {
    static const char kPunct[] = {'-', 4, '.', 6, '!', 7, '?', 8};
    memset(f, 0, sizeof *f);
    alnum(f, y, 8);
    for (size_t i = 0; i < sizeof kPunct; i += 2) glyph(f, kPunct[i], kPunct[i + 1] * 16, y + 32, 8);
    glyph(f, ',', 5 * 16 + 8, y + 32, 8);  /* the comma sits in its cell's right half */
    glyph(f, '(', paren_x, AT_ART, 8);
    glyph(f, ')', paren_x + 8, AT_ART, 8);
    f->advance = 8;
    f->line_height = 16;
}

/* The item set has no parentheses: bend its I. Each row of the I's stroke is
 * shifted by up to 2 pixels along a half sine, so the colour ramp carries. */
static void paren_pair(int set_y, int dst_x) {
    const int ix = 2 * 16, iy = set_y + 16;  /* I: G H I on the second row */
    int top = 16, bottom = -1;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 8; ++x)
            if (s_atlas[(iy + y) * ATLAS_W + ix + x]) { if (y < top) top = y; bottom = y; }
    if (bottom <= top) return;
    for (int y = top; y <= bottom; ++y) {
        const double t = (double)(y - top) / (double)(bottom - top);
        const int bend = (int)(2.0 * (1.0 - sin(3.14159265358979 * t)) + 0.5);
        for (int x = 0; x < 8; ++x) {
            const uint32_t px = s_atlas[(iy + y) * ATLAS_W + ix + x];
            if (!px) continue;
            const int open_x = x - 2 + bend, close_x = x + 1 - bend;
            if (open_x >= 0 && open_x < 8) s_atlas[(AT_ART + y) * ATLAS_W + dst_x + open_x] = px;
            if (close_x >= 0 && close_x < 8) s_atlas[(AT_ART + y) * ATLAS_W + dst_x + 8 + close_x] = px;
        }
    }
}

static void title_font(RecompUiPsxFont* f) {
    memset(f, 0, sizeof *f);
    alnum(f, AT_TITLE, 16);
    glyph(f, '+', 12 * 16, AT_TITLE + 32, 16);
    glyph(f, '-', 13 * 16, AT_TITLE + 32, 16);
    glyph(f, ',', 3 * 16, AT_TITLE + 48, 16);
    glyph(f, '.', 1 * 16, AT_TITLE + 48, 16);
    glyph(f, '!', 6 * 16, AT_TITLE + 48, 16);
    glyph(f, '?', 7 * 16, AT_TITLE + 48, 16);
    f->advance = 16;
    f->line_height = 16;
}

static RecompUiPsxRect rect(int x, int y, int w, int h) {
    RecompUiPsxRect r = {(uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h};
    return r;
}

/* The OPTION backdrop: two tiles in a brick pattern, rows 48 pixels tall. */
static void backdrop(RecompUiPsxControlsSkin* k) {
    static const int16_t kEven[][3] = {{0, 0, 112}, {112, 112, 96}, {208, 0, 112}};
    static const int16_t kOdd[][3] = {{0, 112, 96}, {96, 0, 112}, {208, 112, 96}, {304, 0, 16}};
    k->backdrop_count = 0;
    for (int row = 0; row < 5; ++row) {
        const int odd = row & 1, n = odd ? 4 : 3;
        for (int i = 0; i < n; ++i) {
            const int16_t* b = odd ? kOdd[i] : kEven[i];
            RecompUiPsxBlit* out = &k->backdrop[k->backdrop_count++];
            out->src = rect(b[1], AT_BACKDROP, b[2], 48);
            out->x = b[0];
            out->y = (int16_t)(row * 48);
        }
    }
}

static int build_skin(void) {
    memset(s_atlas, 0, sizeof s_atlas);
    if (!decode(FONT_PAGE_X, 0, 0, 256, 192, 0, 480, 0, AT_FONT) ||
        !decode(FONT_PAGE_X, 0, 144, 256, 48, 48, 480, 0, AT_ORANGE) ||
        !decode(FONT_PAGE_X, 0, 0, 256, 64, 32, 480, 0, AT_TITLE) ||
        !decode(FONT_PAGE_X, 0, 32, 256, 16, 16, 480, 0, AT_BUTTON) ||
        !decode(ART_PAGE_X, 0, 192, 48, 16, 96, 480, 0, AT_ART) ||
        !decode(ART_PAGE_X, 0, 208, 208, 48, 192, 481, 0, AT_BACKDROP))
        return 0;
    paren_pair(AT_FONT + 144, PAREN_X);
    paren_pair(AT_ORANGE, PAREN_X + 16);
    RecompUiPsxControlsSkin* k = &s_skin;
    memset(k, 0, sizeof *k);
    k->rgba = s_atlas;
    k->width = ATLAS_W;
    k->height = ATLAS_H;
    title_font(&k->title);
    item_font(&k->item, AT_FONT + 144, PAREN_X);
    item_font(&k->item_selected, AT_ORANGE, PAREN_X + 16);
    small_font(&k->small);
    for (int i = 0; i < 4; ++i) {
        k->face[i] = rect(128 + 16 * i, AT_BUTTON, 16, 16);    /* circle cross triangle square */
        k->shoulder[i] = rect(64 + 16 * i, AT_BUTTON, 16, 16); /* L1 L2 R1 R2 */
    }
    {   /* recomp-ui wants cross, circle, triangle, square */
        const RecompUiPsxRect circle = k->face[0];
        k->face[0] = k->face[1];
        k->face[1] = circle;
    }
    k->ellipsis = rect(4 * 16, AT_FONT + 48, 16, 16);
    k->cursor[0] = rect(16, AT_ART, 16, 16);  /* OPTION's frame order */
    k->cursor[1] = rect(32, AT_ART, 16, 16);
    k->cursor[2] = rect(0, AT_ART, 16, 16);
    k->cursor_dx[0] = 1;
    k->cursor_frames = 3;
    k->cursor_vblanks = 7;
    backdrop(k);
    return 1;
}

static void play_cues(struct CPUState* cpu) {
    int cue;
    while ((cue = recomp_ui_psx_controls_take_cue()) != RECOMP_UI_PSX_CUE_NONE) {
        const uint32_t id = cue == RECOMP_UI_PSX_CUE_MOVE ? SFX_CURSOR : SFX_SELECT;
        (void)psx_mod_call_guest(cpu, MENU_SFX, cpu->gpr[31], 0, id, 0, 0);
    }
}

static int on_config_init(struct CPUState *cpu, uint32_t address) {
    (void)cpu; (void)address;
    recomp_ui_psx_controls_set_skin(build_skin() ? &s_skin : NULL);
    recomp_ui_psx_controls_set_actions(kActions, (int)(sizeof kActions / sizeof kActions[0]));
    s_page_owns_screen = recomp_ui_psx_controls_open();
    if (!s_page_owns_screen) return 0;  /* no menu: Capcom's screen runs */
    psx_mod_write_byte(CONFIG_STATE, CONFIG_STATE_RUN);
    return 1;
}

static int on_config_run(struct CPUState *cpu, uint32_t address) {
    (void)address;
    if (!s_page_owns_screen) return 0;
    play_cues(cpu);
    if (recomp_ui_psx_controls_is_open()) return 1;
    s_page_owns_screen = 0;
    psx_mod_write_byte(CONFIG_STATE, CONFIG_STATE_EXIT);
    return 1;
}

static void on_session_reset(void) { s_page_owns_screen = 0; }

/* X4 is single-disc, so recomp-ui's game menu would decline a default
 * selection. Choosing it explicitly gives X4 the in-game menu (F10 / Ctrl+L,
 * Select + L3) that hosts the Controls page. */
PSX_MOD_CONSTRUCTOR(mmx4_controls_register) {
    (void)psx_menu_set_provider(recomp_ui_psx_menu_provider());
    psx_mod_register_session_reset_plugin(MMX4_CONTROLS_PLUGIN, on_session_reset);
    psx_mod_register_function_filter_plugin(MMX4_CONTROLS_PLUGIN, CONFIG_INIT, on_config_init);
    psx_mod_register_function_filter_plugin(MMX4_CONTROLS_PLUGIN, CONFIG_RUN, on_config_run);
}
