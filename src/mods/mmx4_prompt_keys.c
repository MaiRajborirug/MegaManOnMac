#include "cpu_state.h"
#include "mod_plugins.h"
#include "recomp_ui_psx_menu.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Capcom's button prompts ("✕ ···CHOOSE  △ ···CANCEL") are one sprite
 * assembly drawn by the object renderer 0x80024334. Object +0x3C points at the
 * sprite set, +0x47 picks the frame; the set starts with one {u16 count,
 * u16 offset_words} entry per frame, and a frame is `count` 4-byte records
 * {attr, cell, dx (s8), dy (s8)} from the object's position. attr bits 0-1
 * extend the cell; 0x04 selects the button palette.
 *
 * This plugin names the player's key after each face button: "✕ (W) ···CHOOSE".
 * The renderer runs twice inside the one call -- the frame's own records,
 * moved right to make room, then the "(KEY)" glyphs written over the same
 * records -- and the frame is restored before the call returns. The font has
 * no parentheses, so they are drawn into two empty cells of the item set from
 * its own I. */
#define PROMPT_PLUGIN "mmx4.prompt-keys"
#define OBJECT_RENDER 0x80024334u
#define OBJ_SPRITE_SET 0x3Cu
#define OBJ_FRAME 0x47u
#define MAX_RECORDS 64
#define MAX_LABEL 6

#define CELL_FACE_FIRST 0x28   /* circle, cross, triangle, square */
#define CELL_ELLIPSIS 0x34
#define CELL_DIGIT 0x90        /* item set: 0-9 A-Z - , . ! ? */
#define CELL_LETTER 0x9A
#define CELL_DASH 0xB4
#define CELL_OPEN 0xBE         /* empty item-set cells this plugin fills */
#define CELL_CLOSE 0xBF
#define ATTR_TEXT 0x00

/* Font page: 4bpp at (960,256); the item set's I is cell (2, v 160). */
#define FONT_X 960
#define FONT_Y 256
#define I_WORD_X (FONT_X + 32 / 4)
#define I_Y (FONT_Y + 160)
#define OPEN_WORD_X (FONT_X + 224 / 4)
#define CLOSE_WORD_X (FONT_X + 240 / 4)
#define PAREN_Y (FONT_Y + 176)

typedef struct { int attr, code, dx, dy; } Rec;

static const char* const kFaceButtons[4] = {"circle", "cross", "triangle", "square"};
static char s_labels[4][MAX_LABEL + 1];
static int s_label_age = -1;
static int s_busy;

static int code_of(const Rec* r) { return r->code; }
static int is_face(const Rec* r) { return code_of(r) >= CELL_FACE_FIRST && code_of(r) < CELL_FACE_FIRST + 4; }
static int width_of(const Rec* r) { return is_face(r) || code_of(r) == CELL_ELLIPSIS ? 16 : 8; }

static int glyph_code(char c) {
    if (c >= '0' && c <= '9') return CELL_DIGIT + (c - '0');
    if (c >= 'A' && c <= 'Z') return CELL_LETTER + (c - 'A');
    if (c == '-') return CELL_DASH;
    return -1;
}

/* "LEFT SHIFT" -> "LSHIFT": the prompt line has room for a few glyphs only. */
static void short_label(const char* name, char* out) {
    int n = 0;
    if (!strncmp(name, "LEFT ", 5)) { out[n++] = 'L'; name += 5; }
    else if (!strncmp(name, "RIGHT ", 6)) { out[n++] = 'R'; name += 6; }
    for (; *name && n < MAX_LABEL; ++name)
        if (glyph_code((char)toupper((unsigned char)*name)) >= 0) out[n++] = (char)toupper((unsigned char)*name);
    out[n] = '\0';
}

static void refresh_labels(void) {
    for (int i = 0; i < 4; ++i) {
        char name[64] = "";
        recomp_ui_psx_key_label(kFaceButtons[i], name, (int)sizeof name);
        short_label(name, s_labels[i]);
    }
}

/* Parentheses bent from the item set's I, 2 pixels at the tips. */
static void ensure_parens(void) {
    uint16_t src[2 * 16], open_px[2 * 16], close_px[2 * 16], have[2 * 16];
    if (!psx_mod_read_vram(I_WORD_X, I_Y, 2, 16, src)) return;
    int top = 16, bottom = -1;
    for (int y = 0; y < 16; ++y)
        if (src[y * 2] | src[y * 2 + 1]) { if (y < top) top = y; bottom = y; }
    if (bottom <= top) return;
    memset(open_px, 0, sizeof open_px);
    memset(close_px, 0, sizeof close_px);
    for (int y = top; y <= bottom; ++y) {
        const double t = (double)(y - top) / (double)(bottom - top);
        const double arc = t < 0.5 ? 1.0 - 2.0 * t : 2.0 * t - 1.0;  /* 1 at the tips */
        const int bend = (int)(2.0 * arc * arc + 0.5);
        for (int x = 0; x < 8; ++x) {
            const unsigned ix = (src[y * 2 + x / 4] >> ((x & 3) * 4)) & 15u;
            const int to_open = x - 2 + bend, to_close = x + 1 - bend;
            if (!ix) continue;
            if (to_open >= 0 && to_open < 8)
                open_px[y * 2 + to_open / 4] |= (uint16_t)(ix << ((to_open & 3) * 4));
            if (to_close >= 0 && to_close < 8)
                close_px[y * 2 + to_close / 4] |= (uint16_t)(ix << ((to_close & 3) * 4));
        }
    }
    if (!psx_mod_read_vram(OPEN_WORD_X, PAREN_Y, 2, 16, have) || memcmp(have, open_px, sizeof have))
        psx_mod_write_vram(OPEN_WORD_X, PAREN_Y, 2, 16, open_px);
    if (!psx_mod_read_vram(CLOSE_WORD_X, PAREN_Y, 2, 16, have) || memcmp(have, close_px, sizeof have))
        psx_mod_write_vram(CLOSE_WORD_X, PAREN_Y, 2, 16, close_px);
}

static int has_prompt(const Rec* r, int n) {
    for (int i = 0; i + 1 < n; ++i)
        if (is_face(&r[i]) && code_of(&r[i + 1]) == CELL_ELLIPSIS && r[i + 1].dy == r[i].dy) return 1;
    return 0;
}

/* Moves rec right by the room each earlier label on its row takes, and
 * appends the label glyphs to extra. Returns the extra count. */
static int insert_labels(Rec* rec, int n, Rec* extra) {
    int shift[256] = {0};
    int m = 0;
    for (int i = 0; i < n; ++i) {
        Rec* r = &rec[i];
        r->dx += shift[r->dy & 255];
        if (!is_face(r) || i + 1 >= n || code_of(&rec[i + 1]) != CELL_ELLIPSIS) continue;
        const char* label = s_labels[code_of(r) - CELL_FACE_FIRST];
        if (!label[0] || m + MAX_LABEL + 2 > MAX_RECORDS * 2) continue;
        int x = r->dx + 18;
        extra[m++] = (Rec){ATTR_TEXT, CELL_OPEN, x, r->dy};
        for (const char* c = label; *c; ++c) extra[m++] = (Rec){ATTR_TEXT, glyph_code(*c), x += 8, r->dy};
        extra[m++] = (Rec){ATTR_TEXT, CELL_CLOSE, x += 8, r->dy};
        shift[r->dy & 255] += x + 8 + 2 - (r->dx + 16);
    }
    return m;
}

typedef struct { int lo, hi; } Span;

static Span row_span(const Rec* a, int n, const Rec* b, int m, int dy) {
    Span s = {1 << 20, -(1 << 20)};
    for (int k = 0; k < n + m; ++k) {
        const Rec* r = k < n ? &a[k] : &b[k - n];
        if (r->dy != dy) continue;
        if (r->dx < s.lo) s.lo = r->dx;
        if (r->dx + width_of(r) > s.hi) s.hi = r->dx + width_of(r);
    }
    return s;
}

/* A widened row closes gaps wider than 16 pixels (Capcom spaces its two
 * prompts far apart), then centres on where it stood. 0 if it cannot fit the
 * signed-byte offsets. */
static int fit_row(Rec* a, int n, Rec* b, int m, int dy, Span before) {
    for (;;) {
        int gap_at = 1 << 20, gap = 0;
        for (int k = 0; k < n + m; ++k) {
            const Rec* r = k < n ? &a[k] : &b[k - n];
            if (r->dy != dy) continue;
            int prev_end = -(1 << 20);
            for (int j = 0; j < n + m; ++j) {
                const Rec* q = j < n ? &a[j] : &b[j - n];
                if (q->dy == dy && q->dx < r->dx && q->dx + width_of(q) > prev_end) prev_end = q->dx + width_of(q);
            }
            if (prev_end > -(1 << 20) && r->dx - prev_end > 16 && r->dx < gap_at) { gap_at = r->dx; gap = r->dx - prev_end - 16; }
        }
        const Span now = row_span(a, n, b, m, dy);
        if (now.lo >= -128 && now.hi <= 127 + 8) break;
        if (!gap) return 0;
        for (int k = 0; k < n + m; ++k) {
            Rec* r = k < n ? &a[k] : &b[k - n];
            if (r->dy == dy && r->dx >= gap_at) r->dx -= gap;
        }
    }
    const Span now = row_span(a, n, b, m, dy);
    const int move = (before.lo + before.hi) / 2 - (now.lo + now.hi) / 2;
    for (int k = 0; k < n + m; ++k) {
        Rec* r = k < n ? &a[k] : &b[k - n];
        if (r->dy == dy) r->dx += move;
    }
    const Span placed = row_span(a, n, b, m, dy);
    return placed.lo >= -128 && placed.hi <= 127 + 8;
}

static void write_recs(uint32_t at, const Rec* r, int n) {
    for (int i = 0; i < n; ++i) {
        psx_mod_write_byte(at + 4u * i, (uint8_t)(r[i].attr | ((r[i].code >> 8) & 3)));
        psx_mod_write_byte(at + 4u * i + 1, (uint8_t)r[i].code);
        psx_mod_write_byte(at + 4u * i + 2, (uint8_t)(int8_t)r[i].dx);
        psx_mod_write_byte(at + 4u * i + 3, (uint8_t)(int8_t)r[i].dy);
    }
}

static uint32_t render(struct CPUState* cpu) {
    return psx_mod_call_guest(cpu, OBJECT_RENDER, cpu->gpr[31], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
}

static int on_object_render(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (s_busy) return 0;
    const uint32_t obj = cpu->gpr[4];
    const uint32_t set = psx_mod_read_word(obj + OBJ_SPRITE_SET);
    if ((set & 0xFFE00000u) != 0x80000000u) return 0;
    const uint32_t entry = set + 4u * psx_mod_read_byte(obj + OBJ_FRAME);
    const int n = psx_mod_read_half(entry);
    const uint32_t at = set + 4u * psx_mod_read_half(entry + 2);
    if (n < 2 || n > MAX_RECORDS) return 0;

    Rec orig[MAX_RECORDS], moved[MAX_RECORDS], extra[MAX_RECORDS * 2];
    for (int i = 0; i < n; ++i) {
        const uint8_t attr = psx_mod_read_byte(at + 4u * i);
        orig[i] = (Rec){attr & ~3, ((attr & 3) << 8) | psx_mod_read_byte(at + 4u * i + 1),
                        (int8_t)psx_mod_read_byte(at + 4u * i + 2), (int8_t)psx_mod_read_byte(at + 4u * i + 3)};
    }
    if (!has_prompt(orig, n)) return 0;
    if (s_label_age < 0 || ++s_label_age >= 120) { refresh_labels(); s_label_age = 0; }
    memcpy(moved, orig, sizeof(Rec) * n);
    const int m = insert_labels(moved, n, extra);
    if (!m) return 0;
    for (int i = 0; i < n; ++i) {
        int seen = 0;
        for (int j = 0; j < i; ++j) seen |= orig[j].dy == orig[i].dy;
        if (!seen && !fit_row(moved, n, extra, m, orig[i].dy, row_span(orig, n, NULL, 0, orig[i].dy))) return 0;
    }
    ensure_parens();

    s_busy = 1;
    write_recs(at, moved, n);
    uint32_t v0 = render(cpu);
    for (int k = 0; k < m; k += n) {
        const int chunk = m - k < n ? m - k : n;
        write_recs(at, extra + k, chunk);
        psx_mod_write_half(entry, (uint16_t)chunk);
        v0 = render(cpu);
    }
    psx_mod_write_half(entry, (uint16_t)n);
    write_recs(at, orig, n);
    s_busy = 0;
    cpu->gpr[2] = v0;
    return 1;
}

static void on_session_reset(void) { s_label_age = -1; s_busy = 0; }

PSX_MOD_CONSTRUCTOR(mmx4_prompt_keys_register) {
    psx_mod_register_session_reset_plugin(PROMPT_PLUGIN, on_session_reset);
    psx_mod_register_function_filter_plugin(PROMPT_PLUGIN, OBJECT_RENDER, on_object_render);
}
