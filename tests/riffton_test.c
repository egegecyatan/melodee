/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 riffton contributors */
/* riffton's shell (src/ui_riff.c) through the real input and drawing code (tests/ui_test.c's harness), built without
 * RIFFTON_HOST_MELODEE: riffton's power-on sounds and screens.
 * 1. power-on: the four roles' sounds (engines.c TRK_DEF_RT), RIFF is the home, the boot screen's words.
 * 2. names: every sound TRY offers has a Title Case display name and a role; each role's list holds only its sounds.
 * 3. RIFF: ALGORITHM / K1 select the layer, K2 swaps its sound (the role's list), K4 its level; OCT stays the octave.
 * 4. TRY: PRESETS opens it; knob n swaps role n's sound once the knob rests; SELECT is A/B, OCT+ keeps B, ALGORITHM
 *    solos a role, OCT- goes back (solo off); OCT never moves the octave there.
 * 5. a swap while the riff loops leaves the other roles' sounding voices alone, and the mix keeps sounding. */
#include <stdint.h>
#define UI_TEST_NO_MAIN 1
#include "ui_test.c"
#include "riffton_demo.h"

static int rbad;
static void rcheck(const char *what, int ok)
{
    printf("riffton: %-86s %s\n", what, ok ? "ok" : "FAIL");
    rbad += !ok;
}

static void rt_power_on(void)
{
    ui_power_on();
    palette_set(UI_DEFAULT_INDEX);
    memset(&rt, 0, sizeof rt);
    rt.scr = RT_RIFF;
    track_solo = 0;
    ui.home = 1;
    rt_demo();
    song.playing = 0;
    frame();
}
static int sound_is(uint32_t k, uint32_t e, uint32_t p) { return trk[k].eng_req == e && trk[k].preset == p; }

static void test_power_on(void)
{
    uint32_t k, ok = 1;
    ui_power_on();
    for (k = 0; k < NPART; k++)
        ok &= TRK_DEF[k][0] == TRK_DEF_RT[k][0] && TRK_DEF[k][1] == TRK_DEF_RT[k][1];
    rcheck("power-on: BASS Moog Bass, LEAD Mini Lead (FMINI), PAD Soft Swell (Prophet-5), ARP Pluck (FM6)",
           ok && TRK_DEF_RT[0][0] == ENGI_FMINI && !strcmp(rt_name(TRK_DEF_RT[0][0], TRK_DEF_RT[0][1]), "Moog Bass") &&
           !strcmp(rt_name(TRK_DEF_RT[1][0], TRK_DEF_RT[1][1]), "Mini Lead") &&
           !strcmp(rt_name(TRK_DEF_RT[2][0], TRK_DEF_RT[2][1]), "Soft Swell") &&
           !strcmp(rt_name(TRK_DEF_RT[3][0], TRK_DEF_RT[3][1]), "Pluck"));
    rt_power_on();
    rcheck("RIFF is the home: riffton draws it (Melodee's Stage does not)", rt.scr == RT_RIFF && rt_shown());
    rcheck("the riff's name and key: RIFF 01, A minor, 2 bars", ({
               char b[16], key[24];
               rt_riff_name(b);
               rt_key(key, sizeof key);
               !strcmp(b, "RIFF 01") && !strcmp(key, "A minor") && rt_bars() == 2u;
           }));
}

static int title_case(const char *s)              /* every word: a capital, then no capital run (KEEP: acronyms) */
{
    const char *w = s;
    while (*w) {
        const char *e = w;
        uint32_t caps = 0, len = 0;
        while (*e && *e != ' ') { caps += *e >= 'A' && *e <= 'Z'; len++; e++; }
        if (len >= 4u && caps == len) {           /* an all-capital word of 4 or more: only the known ones */
            static const char *const OK[] = {"MKII", "CR78", "8580", "6581"};
            int known = 0;
            for (uint32_t i = 0; i < NELEM(OK); i++) known |= !strncmp(w, OK[i], len) && strlen(OK[i]) == len;
            if (!known) return 0;
        }
        w = *e ? e + 1 : e;
    }
    return 1;
}
static void test_names(void)
{
    uint32_t i, role, bad_case = 0, bad_role = 0, n[4] = {0, 0, 0, 0};
    for (i = 0; i < RT_NSOUNDS; i++)
        if (!title_case(RT_NAMES + RT_SOUNDS[i].name)) {
            if (bad_case < 5u) printf("riffton:   not Title Case: %s\n", RT_NAMES + RT_SOUNDS[i].name);
            bad_case++;
        }
    rcheck("every sound's display name is Title Case (tools/preset_names.csv)", !bad_case);
    for (role = 0; role < 4u; role++) {
        const rt_sound_t *first = rt_step(role, 0xFF, 0xFF, 0), *s = first;
        do {
            n[role]++;
            bad_role += !rt_fits(role, s);
            s = rt_step(role, s->engine, s->preset, 1);
        } while (s && s != first && n[role] < 600u);
    }
    printf("riffton: TRY lists: BASS %u, LEAD %u, PAD %u, ARP %u sounds\n", n[0], n[1], n[2], n[3]);
    rcheck("each role's list holds its own sounds only, and wraps", !bad_role && n[0] > 20u && n[1] > 20u && n[2] > 20u &&
           n[3] > 20u);
}

static void test_riff(void)
{
    uint32_t e, p;
    int8_t oct;
    rt_power_on();
    song.sel = 1;
    turn(EN_ALGO, 1);
    rcheck("RIFF: ALGORITHM selects the next layer (LEAD -> PAD)", song.sel == 2u);
    turn(EN_K1, -1);
    rcheck("RIFF: K1 LAYER selects the previous one (PAD -> LEAD)", song.sel == 1u);
    e = trk[1].eng_req;
    p = trk[1].preset;
    turn(EN_K2, 1);
    frame();
    {
        const rt_sound_t *want = rt_step(1, e, p, 1);
        rcheck("RIFF: K2 SOUND swaps LEAD to the next lead sound", want && sound_is(1, want->engine, want->preset));
    }
    {
        int16_t lv = trk[1].p[P_LEVEL];
        turn(EN_K4, -3);
        rcheck("RIFF: K4 LEVEL turns the layer's level", trk[1].p[P_LEVEL] == lv - 3);
    }
    oct = song.octave;
    press(B_OCTUP);
    rcheck("RIFF: OCT+ is still the keyboard's octave (no screen to enter)", song.octave == oct + 1);
    press(B_OCTDN);
}

static void test_try(void)
{
    uint32_t e[4], p[4], k;
    int8_t oct;
    rt_power_on();
    for (k = 0; k < 4u; k++) { e[k] = trk[k].eng_req; p[k] = trk[k].preset; }
    turn(EN_PRESET, 1);
    rcheck("PRESETS opens TRY; A is the combination playing", rt.scr == RT_TRY && rt.a_ok && !rt_changed(0) &&
           !rt_changed(3));
    turn(EN_K2, 1);
    rcheck("TRY: a turned knob's sound waits for the knob to rest", rt.pend[1] || !sound_is(1, e[1], p[1]));
    frame(); frame(); frame(); frame(); frame();
    {
        const rt_sound_t *want = rt_step(1, e[1], p[1], 1);
        rcheck("TRY: knob 2 swaps LEAD's sound, LEAD is marked changed", want && sound_is(1, want->engine, want->preset) &&
               rt_changed(1) && !rt_changed(0));
    }
    turn(EN_K4, 2);
    frame(); frame(); frame(); frame(); frame();
    rcheck("TRY: knob 4 swaps ARP's", rt_changed(3) && !sound_is(3, e[3], p[3]));
    {
        uint32_t be1 = trk[1].eng_req, bp1 = trk[1].preset, be3 = trk[3].eng_req, bp3 = trk[3].preset;
        turn(EN_SELECT, 1);
        rcheck("TRY: SELECT plays A (the kept sounds back)", rt.show_a && sound_is(1, e[1], p[1]) && sound_is(3, e[3], p[3]));
        turn(EN_SELECT, 1);
        rcheck("TRY: SELECT again plays B (the tried sounds back)", !rt.show_a && sound_is(1, be1, bp1) &&
               sound_is(3, be3, bp3));
        oct = song.octave;
        press(B_OCTUP);
        rcheck("TRY: OCT+ keeps B as the new A (nothing changed now; the octave stays)", !rt_changed(1) && !rt_changed(3) &&
               rt.a_e[1] == be1 && rt.a_p[1] == bp1 && song.octave == oct);
    }
    turn(EN_ALGO, 1);
    rcheck("TRY: ALGORITHM solos BASS", rt.solo == 1u && track_solo == 1u);
    turn(EN_ALGO, 1);
    turn(EN_ALGO, 1);
    rcheck("TRY: ALGORITHM steps the solo on (PAD)", rt.solo == 3u && track_solo == 4u);
    oct = song.octave;
    press(B_OCTDN);
    rcheck("TRY: OCT- goes back to RIFF, the solo off, the octave unmoved", rt.scr == RT_RIFF && !rt.solo && !track_solo &&
           song.octave == oct);
}

static void test_swap_playing(void)
{
    static int32_t o[2 * CTL];
    uint32_t i, k, pad_voice = 0, quiet = 0, run = 0;
    rt_power_on();
    trk[2].p[P_SUS] = 127;
    trk_note_on(&trk[2], 57, 100);                /* PAD holds a note */
    for (i = 0; i < 64u; i++) mix_block(o, CTL);
    for (k = 0; k < NVOICE; k++) pad_voice |= trk[2].v[k].active && trk[2].v[k].gate;
    turn(EN_PRESET, 1);
    for (i = 0; i < 6u; i++) {                    /* six swaps of BASS and LEAD, the mix rendered between them */
        turn(i & 1u ? EN_K2 : EN_K1, 1);
        frame(); frame(); frame(); frame(); frame();
        for (uint32_t b = 0; b < 200u; b++) {
            int32_t pk = 0;
            mix_block(o, CTL);
            for (uint32_t j = 0; j < 2u * CTL; j++) pk = o[j] > pk ? o[j] : -o[j] > pk ? -o[j] : pk;
            run = pk < 8 ? run + 1u : 0u;
            if (run > quiet) quiet = run;
        }
    }
    {
        uint32_t still = 0;
        for (k = 0; k < NVOICE; k++) still |= trk[2].v[k].active && trk[2].v[k].gate && trk[2].v[k].note == 57;
        rcheck("TRY: swapping BASS and LEAD leaves PAD's held note sounding", pad_voice && still);
    }
    rcheck("TRY: the mix never falls silent while the swaps happen", quiet < 4u);
    trk_note_off(&trk[2], 57);
}

int main(void)
{
    test_power_on();
    test_names();
    test_riff();
    test_try();
    test_swap_playing();
    printf("riffton: %s\n", rbad ? "FAILED" : "all ok");
    return rbad != 0;
}
