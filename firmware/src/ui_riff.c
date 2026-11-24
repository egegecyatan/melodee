/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 riffton contributors
 * Portions Copyright (C) 2026 Kerem Kilic (Ellic Studio): Melodee, the host this shell runs on */
/* riffton's shell: an instrument for writing riffs. The four synth parts are roles, each with one fixed colour:
 * track 1 BASS, 2 LEAD, 3 PAD, 4 ARP. These screens replace Melodee's home (Stage); Melodee's pages stay behind ENV,
 * LFO, FX, ARP, GLO and EDIT as the deep pages, and HOME comes back here.
 *   RIFF  the home: chords per bar, the four layers as mini piano rolls, K1 LAYER K2 SOUND K3 TAKE K4 LEVEL
 *   TRY   one column per role, the sound wheel: knob n swaps role n's sound while the riff loops; SELECT A/B, OCT+
 *         keeps B, ALGORITHM solos a role, OCT- back
 * The layouts are the designs' (riffton-handover/design, the .dc.html files), in device pixels; the type is gfx.c's RT_*
 * tokens (tools/gen_aa_font.py RIFFTON_TYPE), the colours the brief's tokens below. Sound names, roles and tags come
 * from tools/preset_names.csv (rt_sounds.h, the resource region). */
#include "rt_sounds.h"

/* ------------------------------------------------------------ tokens --- */
#define RT_BG RGB(0x0E, 0x0E, 0x12)
#define RT_PANEL RGB(0x17, 0x17, 0x1D)
#define RT_RAISED RGB(0x1F, 0x1F, 0x27)
#define RT_LINE RGB(0x2C, 0x2C, 0x36)
#define RT_TEXT RGB(0xF2, 0xF0, 0xEC)
#define RT_SEC RGB(0x9C, 0x99, 0xA3)
#define RT_DIM RGB(0x5C, 0x59, 0x66)
#define RT_FAINT RGB(0x3A, 0x3A, 0x46)     /* TRY's outer wheel rows */
#define RT_REC RGB(0xFF, 0x4D, 0x5E)
#define RT_HEAD_H 20                       /* the header, everywhere */
#define RT_FOOT_Y 207                      /* the footer's top line: four knob cells */
static const uint16_t RT_ROLE_C[4] = {RGB(0xFF, 0x6B, 0x4A), RGB(0xF5, 0xC0, 0x4A), RGB(0x3D, 0xC9, 0xB8),
                                      RGB(0xA7, 0x8B, 0xFA)};
static const char *const RT_ROLE[4] = {"BASS", "LEAD", "PAD", "ARP"};
/* a + (b - a) * pct / 100 per RGB565 channel: the roles' colours stay theirs in every palette (gfx.c ux_mix follows
 * the palette's MONO) */
static uint16_t rt_mix(uint16_t a, uint16_t b, int32_t pct)
{
    static const uint8_t SH[3] = {11, 5, 0}, MK[3] = {31, 63, 31};
    uint32_t out = 0;
    for (uint32_t k = 0; k < 3u; k++) {
        int32_t x = (a >> SH[k]) & MK[k], d = (((b >> SH[k]) & MK[k]) - x) * pct;
        out |= (uint32_t)(x + (d + (d >= 0 ? 50 : -50)) / 100) << SH[k];
    }
    return (uint16_t)out;
}
/* the keys' quality after the root (A minor, F# major): the common scales; any other keeps its title */
static const char *const RT_QUALITY[16] = {"chromatic", "major", "minor", "dorian", "mixolydian", "major pent.",
                                           "minor pent.", "harm. minor", "phrygian", "lydian", "locrian",
                                           "mel. minor", "blues", "whole tone", "dim. HW", "dim. WH"};
/* the engines TRY offers, in its order (FMini-1's first) */
static const uint8_t RT_ORDER[5] = {ENGI_FMINI, ENGI_PROPHET, ENGI_FM6, ENGI_CZ, 3};

enum { RT_NONE, RT_RIFF, RT_TRY };
static struct {
    uint8_t scr;                           /* the riffton screen shown while ui.home */
    uint8_t a_ok, show_a, solo;            /* TRY: A held, A sounding, the soloed role + 1 */
    uint8_t a_e[4], a_p[4], b_e[4], b_p[4];/* A: the kept combination; B: the one A replaced while A sounds */
    uint8_t pend[4], pe[4], pp[4];         /* a knob's sound waiting for the knob to rest (RT_SWAP_MS) */
    uint32_t pend_ms[4];
    uint32_t sig, msg_shown;
} rt = {RIFFTON_HOST_MELODEE ? RT_NONE : RT_RIFF};   /* (RT_NONE: Melodee's home, the host tests of it) */
#define RT_SWAP_MS 60u                     /* a sound swaps once its knob has rested this long (a fast turn skips) */

static void project_cur_name(char *b);     /* project.c */
static uint32_t project_cur_slot(void);

/* ------------------------------------------------------------ sounds --- */
static const rt_sound_t *rt_sound(uint32_t e, uint32_t p)
{
    for (uint32_t i = 0; i < RT_NSOUNDS; i++)
        if (RT_SOUNDS[i].engine == e && RT_SOUNDS[i].preset == p)
            return &RT_SOUNDS[i];
    return 0;
}
/* a sound's display name (Title Case: the CSV's), else its preset name as the engine has it */
static const char *rt_name(uint32_t e, uint32_t p)
{
    const rt_sound_t *s = RES_OK() ? rt_sound(e, p) : 0;
    if (s)
        return RT_NAMES + s->name;
    e %= NENGINES;
    return ENGINES[e]->npresets ? eng_preset(ENGINES[e], p % ENGINES[e]->npresets)->name : "";
}
static void rt_tags(uint32_t e, uint32_t p, char *b, uint32_t n)
{
    const rt_sound_t *s = RES_OK() ? rt_sound(e, p) : 0;
    b[0] = 0;
    if (!s)
        return;
    str_cpy(b, RT_TAG[s->tags & 15u], n);
    if (s->tags >> 4) {
        if (b[0])
            str_cpy(b + str_len(b), " \xB7 ", n - str_len(b));
        str_cpy(b + str_len(b), RT_TAG[s->tags >> 4], n - str_len(b));
    }
}
static int rt_fits(uint32_t role, const rt_sound_t *s)
{
    return s->role == role || (role == RT_ARP_T && s->role == RT_KEYS_T);   /* ARP: plucks, bells and keys */
}
/* the role's list in TRY's order: the n-th sound from (e, p), n < 0 backwards (wraps); (e, p) not in the list: its
 * first. 0 = the role has none */
static const rt_sound_t *rt_step(uint32_t role, uint32_t e, uint32_t p, int32_t n)
{
    int32_t total = 0, at = -1, k;
    uint32_t o, i;
    if (!RES_OK())
        return 0;
    for (o = 0; o < NELEM(RT_ORDER); o++)
        for (i = 0; i < RT_NSOUNDS; i++) {
            const rt_sound_t *s = &RT_SOUNDS[i];
            if (s->engine != RT_ORDER[o] || !rt_fits(role, s) || !eng_ok(s->engine))
                continue;
            if (s->engine == e && s->preset == p)
                at = total;
            total++;
        }
    if (!total)
        return 0;
    if (at < 0) {
        at = 0;
        if (n > 0) n--;
    }
    k = ((at + n) % total + total) % total;
    for (o = 0; o < NELEM(RT_ORDER); o++)
        for (i = 0; i < RT_NSOUNDS; i++) {
            const rt_sound_t *s = &RT_SOUNDS[i];
            if (s->engine != RT_ORDER[o] || !rt_fits(role, s) || !eng_ok(s->engine))
                continue;
            if (!k--)
                return s;
        }
    return 0;
}
/* role k's sound now (a knob's waiting one first) */
static void rt_cur(uint32_t k, uint32_t *e, uint32_t *p)
{
    *e = rt.pend[k] ? rt.pe[k] : trk[k].eng_req;
    *p = rt.pend[k] ? rt.pp[k] : trk[k].preset;
}
static void rt_apply(uint32_t k, uint32_t e, uint32_t p)
{
    track_t *t = &trk[k];
    rt.pend[k] = 0;
    if (t->eng_req == e && t->preset == p && !t->user)
        return;
    if (t->eng_req == e)
        apply_preset_to(t, p);
    else
        engine_load_of(t, e, p);
}
static void rt_turn(uint32_t k, int32_t s)    /* knob k turned s detents: the next sound of role k, soon */
{
    uint32_t e, p;
    const rt_sound_t *n;
    rt_cur(k, &e, &p);
    if (!(n = rt_step(k, e, p, s)))
        return;
    rt.pend[k] = 1;
    rt.pe[k] = n->engine;
    rt.pp[k] = n->preset;
    rt.pend_ms[k] = fm1_ms;
    rt.show_a = 0;                            /* (a change is B's) */
}
static void rt_poll(void)
{
    for (uint32_t k = 0; k < 4u; k++)
        if (rt.pend[k] && fm1_ms - rt.pend_ms[k] >= RT_SWAP_MS)
            rt_apply(k, rt.pe[k], rt.pp[k]);
}
static void rt_keep(void)                     /* the combination now is A */
{
    for (uint32_t k = 0; k < 4u; k++) {
        if (rt.pend[k]) rt_apply(k, rt.pe[k], rt.pp[k]);
        rt.a_e[k] = trk[k].eng_req;
        rt.a_p[k] = trk[k].preset;
    }
    rt.a_ok = 1;
    rt.show_a = 0;
}
static int rt_changed(uint32_t k)
{
    uint32_t e, p;
    rt_cur(k, &e, &p);
    return rt.a_ok && (e != rt.a_e[k] || p != rt.a_p[k]);
}
static void rt_ab(void)                       /* SELECT: A <-> B */
{
    for (uint32_t k = 0; k < 4u; k++) {
        if (rt.pend[k]) rt_apply(k, rt.pe[k], rt.pp[k]);
        if (!rt.show_a) {
            rt.b_e[k] = trk[k].eng_req;
            rt.b_p[k] = trk[k].preset;
        }
    }
    rt.show_a ^= 1u;
    for (uint32_t k = 0; k < 4u; k++)
        rt_apply(k, rt.show_a ? rt.a_e[k] : rt.b_e[k], rt.show_a ? rt.a_p[k] : rt.b_p[k]);
}
static void rt_solo_set(uint32_t s)           /* 0 none, k + 1: role k alone (ui_layer.c layer_keys adds GLO's) */
{
    rt.solo = (uint8_t)(s % 5u);
    rt_solo_bits = rt.solo ? (uint8_t)(1u << (rt.solo - 1u)) : 0u;
    track_solo = rt_solo_bits;
}

/* ------------------------------------------------------------- music --- */
static uint32_t rt_steps(void)                /* the riff's length in steps (16 a bar), the longest layer's */
{
    uint32_t n = 16;
    for (uint32_t k = 0; k < 4u; k++)
        if ((uint32_t)trk[k].p[P_SLEN] > n) n = (uint32_t)trk[k].p[P_SLEN];
    return n > NSTEP ? NSTEP : n;
}
static uint32_t rt_bars(void) { return (rt_steps() + 15u) / 16u; }
static void rt_key(char *b, uint32_t n)       /* A minor */
{
    const track_t *t = &trk[0];
    uint32_t sc = (uint32_t)t->p[P_SCALE] % SCALE_TOTAL;
    str_cpy(b, N_NOTE[(uint32_t)t->p[P_ROOT] % 12u], n);
    str_cpy(b + str_len(b), " ", n - str_len(b));
    str_cpy(b + str_len(b), sc < 16u ? RT_QUALITY[sc] : SCALE_TITLE[sc], n - str_len(b));
}
/* bar b's chord as the layers play it: the pitch classes of its notes, the root the bass's lowest (else the
 * lowest), named as chord.c names a chord key's (Am7, Fmaj7, Gsus4). 0 = no notes */
static int rt_bar_chord(uint32_t bar, char *b)
{
    uint32_t pcs = 0, low = 128, blow = 128, k, i, s0 = bar * 16u, n = rt_steps();
    for (k = 0; k < 4u; k++) {
        const track_t *t = &trk[k];
        uint32_t len = (uint32_t)t->p[P_SLEN];
        if (ENGINES[t->engine % NENGINES] == &ENG_DRUM) continue;
        for (i = s0; i < s0 + 16u && i < n; i++) {
            const step_t *st = &t->step[i % (len ? len : 16u)];
            if (st->time != ST_NOTE) continue;
            for (uint32_t j = 0; j < st->n && j < 4u; j++) {
                uint32_t note = st->note[j];
                pcs |= 1u << (note % 12u);
                if (note < low) low = note;
                if (k == 0u && note < blow) blow = note;
            }
        }
    }
    if (!pcs)
        return 0;
    {
        uint32_t root = (blow < 128u ? blow : low) % 12u, mask = 0;
        for (i = 0; i < 12u; i++)
            if ((pcs >> ((root + i) % 12u)) & 1u) mask |= 1u << i;
        chord_name(b, root, mask);
    }
    return 1;
}
static uint32_t rt_play_step(void)            /* the playhead, in steps (0xFFFF: stopped) */
{
    return song.playing ? trk[0].seq_idx % rt_steps() : 0xFFFFu;
}

/* --------------------------------------------------------- drawing --- */
static void rt_play_glyph(int32_t x, int32_t cy, uint16_t c)   /* the transport's triangle (the font has none) */
{
    for (int32_t r = -3; r <= 3; r++)
        cv_rect(x, cy + r, 4 - (r < 0 ? -r : r), 1, c);
}
/* free text (a name: the lint allows its ellipsis), centred on xc in at most maxw px */
static void rt_free_c(int32_t xc, int32_t y, const aafont_t *f, const char *s, uint16_t fg, uint16_t bg, int32_t maxw)
{
    char b[48];
    int cut = text_fit(b, sizeof b, s, f, maxw);
    cv_text_flags(xc - text_w(f, b) / 2, y, f, b, fg, bg, cut ? 9u : 8u);
}
static void rt_box(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t fill, uint16_t edge, uint16_t under)
{
    if (edge != fill) {
        cv_rrect(x, y, w, h, 4, edge, under);
        cv_rrect(x + 1, y + 1, w - 2, h - 2, 3, fill, edge);
    } else
        cv_rrect(x, y, w, h, 4, fill, under);
}
/* the header: title (in a colour), the context in secondary text, the transport at the right */
static void rt_head(const char *title, uint16_t tc, const char *ctx)
{
    char bpm[8];
    int32_t x = 8, cy = 10;
    if (song.rec) {
        cv_disc(x + 3, cy, 7, RT_REC, 0);
        x += 13;
    }
    x = cv_text_on(x, ty_y(cy, RT_H), RT_H, title, tc, RT_BG) + 6;
    if (ui.msg_t && ui.msg[0])
        ctx = ui.msg;                         /* (a toast: in the context's place) */
    fmt_int(bpm, song.g[G_BPM]);
    {
        int32_t bw = text_w(RT_M, bpm);
        cv_free_text(x, ty_y(cy, RT_M), RT_M, ctx, RT_SEC, RT_BG, 232 - bw - 10 - x);
        cv_text_on(232 - bw, ty_y(cy, RT_M), RT_M, bpm, RT_TEXT, RT_BG);
        rt_play_glyph(232 - bw - 7, cy, song.playing ? RT_TEXT : RT_DIM);
    }
    cv_rect(0, RT_HEAD_H - 1, 240, 1, RT_LINE);
}
/* the footer: four knob cells, K1 .. K4: a label and a value */
static void rt_foot(const char *const *label, const char *const *value, const uint16_t *vc)
{
    cv_rect(0, RT_FOOT_Y, 240, 1, RT_LINE);
    for (uint32_t k = 0; k < 4u; k++) {
        int32_t x = (int32_t)k * 60;
        if (k) cv_rect(x, RT_FOOT_Y + 1, 1, 240 - RT_FOOT_Y - 1, RT_LINE);
        cv_text_on(x + 6, RT_FOOT_Y + 5, RT_XS, label[k], RT_DIM, RT_BG);
        cv_free_text(x + 6, RT_FOOT_Y + 15, RT_V, value[k], vc[k], RT_BG, 50);   /* (K2: a sound's name) */
    }
}
static void rt_hints(int32_t y, const char *const *key, const char *const *what, uint32_t n)
{
    int32_t x = 8;
    cv_rect(0, y, 240, 1, RT_LINE);
    for (uint32_t i = 0; i < n; i++) {
        x = cv_text_on(x, ty_y(y + 12, RT_SB), RT_SB, key[i], RT_TEXT, RT_BG) + 3;
        x = cv_text_on(x, ty_y(y + 12, RT_S), RT_S, what[i], RT_SEC, RT_BG) + 10;
    }
}

/* -------------------------------------------------------------- RIFF --- */
static void rt_riff_name(char *b)
{
    uint32_t n = project_cur_slot() < PROJ_SLOTS ? project_cur_slot() + 1u : 1u;
    project_cur_name(b);
    if (!b[0]) {                              /* an unnamed riff: RIFF 07 */
        str_cpy(b, "RIFF ", 13);
        b[5] = (char)('0' + n / 10u % 10u);
        b[6] = (char)('0' + n % 10u);
        b[7] = 0;
    }
}
/* layer k's notes into its roll (x0, y0, w, h): each note a 3 px bar, its pitch within the layer's own range */
static void rt_roll(uint32_t k, int32_t x0, int32_t y0, int32_t w, int32_t h, uint16_t c)
{
    const track_t *t = &trk[k];
    uint32_t n = rt_steps(), len = (uint32_t)t->p[P_SLEN], i, lo = 127, hi = 0;
    if (!len) len = 16;
    for (i = 0; i < len && i < NSTEP; i++)
        if (t->step[i].time == ST_NOTE)
            for (uint32_t j = 0; j < t->step[i].n && j < 4u; j++) {
                if (t->step[i].note[j] < lo) lo = t->step[i].note[j];
                if (t->step[i].note[j] > hi) hi = t->step[i].note[j];
            }
    if (lo > hi)
        return;
    if (hi - lo < 12u) { lo = lo > 6u ? lo - 6u + (hi - lo) / 2u : 0u; hi = lo + 12u; }
    for (i = 0; i < n; i++) {
        const step_t *st = &t->step[i % len];
        uint32_t d = 1;
        if (st->time != ST_NOTE) continue;
        while (i + d < n && t->step[(i + d) % len].time == ST_TIE) d++;
        for (uint32_t j = 0; j < st->n && j < 4u; j++) {
            int32_t x = x0 + (int32_t)(i * (uint32_t)w / n), x1 = x0 + (int32_t)((i + d) * (uint32_t)w / n) - 1;
            int32_t y = y0 + (int32_t)((hi - st->note[j]) * (uint32_t)(h - 3) / (hi - lo));
            cv_rrect(x, y, x1 - x > 2 ? x1 - x : 2, 3, 1, c, cv_bg);
        }
    }
}
static void rt_draw_riff(void)
{
    char name[16], ctx[40], ch[12];
    uint32_t bars = rt_bars(), k, sel = song.sel % 4u, ph = rt_play_step(), cur = ph == 0xFFFFu ? 0xFFu : ph / 16u;
    int32_t y, cw;
    rt_riff_name(name);
    rt_key(ctx, sizeof ctx);
    str_cpy(ctx + str_len(ctx), " \xB7 ", sizeof ctx - str_len(ctx));
    fmt_int(ctx + str_len(ctx), (int32_t)bars);
    str_cpy(ctx + str_len(ctx), bars == 1u ? " bar" : " bars", sizeof ctx - str_len(ctx));
    rt_head(name, RT_TEXT, ctx);
    /* the chords: a cell per bar */
    cw = (int32_t)((186u - 2u * (bars - 1u)) / bars);
    for (k = 0; k < bars; k++) {
        int32_t x = 48 + (int32_t)k * (cw + 2);
        int on = k == cur;
        cv_rrect(x, 24, cw, 15, 3, on ? RT_LINE : RT_PANEL, RT_BG);
        if (rt_bar_chord(k, ch))
            cv_text_c(x + cw / 2, ty_y(31, RT_M), RT_M, ch, on ? RT_TEXT : RT_SEC, on ? RT_LINE : RT_PANEL);
    }
    /* the layers, the selected one taller and outlined in its colour */
    for (k = 0, y = 43; k < 4u; k++) {
        int s = k == sel;
        int32_t h = s ? 56 : 33;
        uint16_t c = RT_ROLE_C[k], fill = s ? RT_RAISED : RT_PANEL;
        uint32_t e, p;
        rt_box(6, y, 228, h, fill, s ? c : fill, RT_BG);
        cv_text_on(10, ty_y(y + h / 2 - 5, RT_MB), RT_MB, RT_ROLE[k], c, fill);
        rt_cur(k, &e, &p);
        cv_free_text(10, ty_y(y + h / 2 + 5, RT_XS), RT_XS, rt_name(e, p), RT_SEC, fill, 36);
        cv_bg = fill;
        rt_roll(k, 50, y + 3, 180, h - 6, s ? c : rt_mix(fill, c, 75));
        cv_bg = RT_BG;
        y += h + 3;
    }
    if (ph != 0xFFFFu) {                      /* the playhead, across all four */
        int32_t x = 50 + (int32_t)(ph * 180u / rt_steps());
        cv_rect(x, 45, 1, y - 48, rt_mix(RT_BG, RT_TEXT, 60));
    }
    {
        static const char *const L[4] = {"K1 LAYER", "K2 SOUND", "K3 TAKE", "K4 LEVEL"};
        char lvl[8];
        const char *v[4];
        uint16_t vc[4] = {RT_ROLE_C[sel], RT_TEXT, RT_TEXT, RT_TEXT};
        uint32_t e, p;
        rt_cur(sel, &e, &p);
        fmt_int(lvl, trk[sel].p[P_LEVEL] * 100 / 127);
        v[0] = RT_ROLE[sel];
        v[1] = rt_name(e, p);
        v[2] = "A";                            /* (takes: the WRITE phase) */
        v[3] = lvl;
        rt_foot(L, v, vc);
    }
}

/* --------------------------------------------------------------- TRY --- */
static void rt_draw_try(void)
{
    char ctx[32], tags[32];
    rt_riff_name(ctx);
    str_cpy(ctx + str_len(ctx), song.playing ? " looping" : " stopped", sizeof ctx - str_len(ctx));
    if (rt.solo) {
        str_cpy(ctx, RT_ROLE[rt.solo - 1u], sizeof ctx);
        str_cpy(ctx + str_len(ctx), " solo", sizeof ctx - str_len(ctx));
    }
    {   /* the header: TRY, the riff, the A/B chip */
        int32_t x = cv_text_on(8, ty_y(10, RT_H), RT_H, "TRY", RT_TEXT, RT_BG) + 6;
        const char *ab = rt.show_a ? "A" : "B", *kept = rt.show_a ? "B = new" : "A = kept";
        int32_t kw = text_w(RT_S, kept), cw = text_w(RT_S, ab) + 8;
        cv_free_text(x, ty_y(10, RT_M), RT_M, ui.msg_t && ui.msg[0] ? ui.msg : ctx, RT_SEC, RT_BG, 232 - kw - cw - 8 - x);
        cv_rrect(232 - kw - 6 - cw, 4, cw, 12, 3, RT_TEXT, RT_BG);
        cv_text_on(232 - kw - 6 - cw + 4, ty_y(10, RT_S), RT_S, ab, RT_BG, RT_TEXT);
        cv_text_on(232 - kw, ty_y(10, RT_S), RT_S, kept, RT_DIM, RT_BG);
        cv_rect(0, RT_HEAD_H - 1, 240, 1, RT_LINE);
    }
    for (uint32_t k = 0; k < 4u; k++) {      /* the columns: the sound wheel, the level */
        int32_t x = 4 + (int32_t)k * 58, w = 55, cx = x + w / 2;
        uint16_t c = RT_ROLE_C[k];
        int dim = rt.solo && rt.solo - 1u != k, chg = rt_changed(k);
        uint16_t fill = chg ? rt_mix(RT_BG, RT_TEXT, 6) : RT_PANEL;
        uint32_t e, p, i;
        char kn[4] = {'K', (char)('1' + k), 0, 0};
        const rt_sound_t *n;
        rt_cur(k, &e, &p);
        cv_text_on(x + 2, ty_y(31, RT_MB), RT_MB, RT_ROLE[k], dim ? RT_DIM : c, RT_BG);
        cv_text_r(x + w - 2, ty_y(31, RT_XS), RT_XS, kn, RT_DIM, RT_BG);
        for (i = 0; i < 4u; i++) {            /* the neighbours: two above, two below */
            static const int8_t OFF[4] = {-2, -1, 1, 2};
            static const uint8_t CY[4] = {46, 62, 140, 156};
            if ((n = rt_step(k, e, p, OFF[i])) != 0) {
                rt_free_c(cx, ty_y(CY[i], RT_S), RT_S, RT_NAMES + n->name, (i == 0u || i == 3u) ? RT_FAINT : RT_DIM, RT_BG,
                          w - 2);
            }
        }
        rt_box(x, 72, w, 58, fill, dim ? RT_LINE : c, RT_BG);
        {   /* the sound now: its name (two lines when long), its tags, kept / CHANGED */
            const char *nm = rt_name(e, p);
            char l1[24], l2[24];
            uint32_t j = 0, cut = 0;
            str_cpy(l1, nm, sizeof l1);
            if (text_w(RT_VB, l1) > w - 6) {  /* (the last space that fits) */
                for (j = 0; l1[j]; j++) {
                    if (l1[j] != ' ') continue;
                    l1[j] = 0;
                    if (text_w(RT_VB, l1) <= w - 6) cut = j;
                    l1[j] = ' ';
                }
            }
            l2[0] = 0;
            if (cut) {
                str_cpy(l2, l1 + cut + 1u, sizeof l2);
                l1[cut] = 0;
            }
            if (l2[0]) {
                rt_free_c(cx, ty_y(88, RT_VB), RT_VB, l1, dim ? RT_SEC : RT_TEXT, fill, w - 6);
                rt_free_c(cx, ty_y(99, RT_VB), RT_VB, l2, dim ? RT_SEC : RT_TEXT, fill, w - 6);
            } else
                rt_free_c(cx, ty_y(93, RT_VB), RT_VB, l1, dim ? RT_SEC : RT_TEXT, fill, w - 6);
            rt_tags(e, p, tags, sizeof tags);
            if (tags[0])
                rt_free_c(cx, ty_y(110, RT_XS), RT_XS, tags, RT_SEC, fill, w - 4);
            cv_text_c(cx, ty_y(121, RT_XS), RT_XS, chg ? "CHANGED" : "kept", chg ? RT_TEXT : RT_DIM, fill);
        }
        cv_rrect(x + 4, 167, w - 8, 3, 1, RT_LINE, RT_BG);
        cv_rrect(x + 4, 167, (w - 8) * trk[k].p[P_LEVEL] / 127 > 2 ? (w - 8) * trk[k].p[P_LEVEL] / 127 : 2, 3, 1,
                 dim ? RT_DIM : c, RT_LINE);
    }
    {
        static const char *const K[3] = {"OCT+", "SELECT", "ALGO"}, *const W[3] = {"keep B", "A/B", "solo"};
        rt_hints(218, K, W, 3);
    }
}

/* -------------------------------------------------------------- BOOT --- */
/* the boot screen: the wordmark, a small riff in the roles' colours, the line, the credit */
static void rt_draw_boot(void)
{
    static const uint8_t ROWS[4][12] = {{3, 1, 2, 1, 4, 1, 2}, {1, 1, 1, 3, 1, 1, 2, 1, 1}, {6, 1, 5},
                                        {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}};
    static const uint8_t ROLE_OF_ROW[4] = {1, 0, 2, 3};       /* LEAD, BASS, PAD, ARP */
    for (uint32_t band = 0; band < 240u; band += CV_MAX / 240u) {
        cv_begin_band(240, 240, RT_BG, band);
        cv_text_c(120, 72, RT_W, "riffton", RT_TEXT, RT_BG);
        for (uint32_t r = 0; r < 4u; r++) {
            uint32_t sum = 0, n = 0, i;
            int32_t x = 54;
            while (n < 12u && ROWS[r][n]) sum += ROWS[r][n++];
            for (i = 0; i < n; i++) {
                int32_t w = (int32_t)((132u - 2u * (n - 1u)) * ROWS[r][i] / sum);
                if (!(i & 1u)) cv_rrect(x, 124 + (int32_t)r * 6, w, 3, 1, RT_ROLE_C[ROLE_OF_ROW[r]], RT_BG);
                x += w + 2;
            }
        }
        cv_text_c(120, ty_y(157, RT_M), RT_M, "write the riff", RT_SEC, RT_BG);
        cv_text_c(120, ty_y(216, RT_XS), RT_XS, "riffton " RIFFTON_VERSION, RT_DIM, RT_BG);   /* (#86: the version) */
        cv_text_c(120, ty_y(228, RT_XS), RT_XS, "built on Melodee and FMini-1 \xB7 GPL-3.0", RT_DIM, RT_BG);
        cv_blit_band(0, 0);
    }
}

/* ------------------------------------------------------- the frame --- */
static int rt_shown(void)                     /* a riffton screen is the page now (Melodee's home otherwise) */
{
    return rt.scr != RT_NONE && ui.home && !pop.on && !ui.ly && !ui.layer && !name_on() && !new_on();
}
static uint32_t rt_signature(void)            /* what the screen shows: a change redraws it */
{
    uint32_t h = 2166136261u, k;
#define RT_MIX(v) (h = (h ^ (uint32_t)(v)) * 16777619u)
    RT_MIX(rt.scr); RT_MIX(song.sel); RT_MIX(rt_play_step()); RT_MIX(song.playing); RT_MIX(song.rec);
    RT_MIX(song.g[G_BPM]); RT_MIX(rt.show_a); RT_MIX(rt.solo); RT_MIX(rt.a_ok); RT_MIX(ty_pixel());
    RT_MIX(ui.msg_t ? 1u : 0u); RT_MIX(ui.msg[0]); RT_MIX(project_cur_slot()); RT_MIX(pattern_epoch);
    for (k = 0; k < 4u; k++) {
        uint32_t e, p;
        rt_cur(k, &e, &p);
        RT_MIX(e); RT_MIX(p); RT_MIX(trk[k].p[P_LEVEL]); RT_MIX(trk[k].p[P_SLEN]);
        RT_MIX(steps_sig(&trk[k]));
    }
    RT_MIX(trk[0].p[P_ROOT]); RT_MIX(trk[0].p[P_SCALE]);
#undef RT_MIX
    return h;
}
static void rt_draw(void)
{
    uint32_t sig;
    if (ui.msg_t) ui.msg_t--;
    sig = rt_signature();
    if (sig == rt.sig && !ui.force)
        return;
    rt.sig = sig;
    ui.force = 0;
    for (uint32_t band = 0; band < 240u; band += CV_MAX / 240u) {
        cv_begin_band(240, 240, RT_BG, band);
        if (rt.scr == RT_TRY) rt_draw_try();
        else rt_draw_riff();
        cv_blit_band(0, 0);
    }
}

/* ------------------------------------------------------------ input --- */
/* riffton's controls while one of its screens shows (ui.home): what it takes it clears from pressed and from the
 * encoders; the rest (the keys, PLAY, REC, ENV, LFO, FX, ARP, GLO, EDIT, SEQ, SCL, SAVE) stays Melodee's. 1 = the
 * frame's input is done */
static int rt_input(uint32_t *pressed)
{
    uint32_t bit_home = 1u << panel.btn[B_HOME], up = 1u << panel.btn[B_OCTUP], dn = 1u << panel.btn[B_OCTDN];
    int32_t s;
    rt_poll();
    if (!rt_shown() || ui.menu || ui.confirm)
        return 0;
    if (*pressed & bit_home) {                /* HOME: RIFF */
        *pressed &= ~bit_home;
        if (rt.scr == RT_TRY) rt_solo_set(0);
        rt.scr = RT_RIFF;
        ui.force = 1;
    }
    if ((s = panel_enc(EN_PRESET)) != 0 && rt.scr != RT_TRY) {   /* PRESETS: TRY */
        rt.scr = RT_TRY;
        if (!rt.a_ok) rt_keep();
        ui.force = 1;
        s = 0;
    }
    if (rt.scr == RT_TRY) {
        for (uint32_t k = 0; k < 4u; k++)
            if ((s = panel_enc(EN_K1 + k)) != 0) rt_turn(k, s);
        if (panel_enc(EN_SELECT) != 0) rt_ab();
        if ((s = panel_enc(EN_ALGO)) != 0) rt_solo_set((uint32_t)((int32_t)rt.solo + (s > 0 ? 1 : 4)));
        if (*pressed & up) {                  /* OCT+: keep B */
            *pressed &= ~up;
            oct_eat |= 2u;
            rt_keep();
            ui_message("kept");
        }
        if (*pressed & dn) {                  /* OCT-: back */
            *pressed &= ~dn;
            oct_eat |= 1u;
            rt_solo_set(0);
            rt.scr = RT_RIFF;
            ui.force = 1;
        }
        return 0;
    }
    /* RIFF */
    if ((s = panel_enc(EN_K1)) != 0 || (s = panel_enc(EN_ALGO)) != 0) {
        song.sel = (uint8_t)(((int32_t)(song.sel % 4u) + (s > 0 ? 1 : 3)) % 4);
        ui.force = 1;
    }
    if ((s = panel_enc(EN_K2)) != 0) rt_turn(song.sel % 4u, s);
    (void)panel_enc(EN_K3);                   /* (TAKE: the WRITE phase) */
    if ((s = panel_enc(EN_K4)) != 0) {
        track_t *t = &trk[song.sel % 4u];
        t->p[P_LEVEL] = (int16_t)clamp(t->p[P_LEVEL] + s, TP[P_LEVEL].min, TP[P_LEVEL].max);
    }
    (void)panel_enc(EN_SELECT);
    return 0;
}
