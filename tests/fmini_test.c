/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (FMini-1): the engine under test
 * Portions Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* FMINI (src/eng_fmini.c, engine 22: FMini-1's voice in Melodee).
 * 1. the engine table: index 22 selectable and shown, the 192 presets (names, E7 = their slot - 64), their categories, the
 *    part state's size.
 * 2. the patch: a preset loads its SLOT's factory patch; a turned SLOT loads once it has been still for 50 ms.
 * 3. every preset sounds: a held note is heard, never runs away (peak bounded; once both contours settle, the next four
 *    seconds, sixteen with a slow LFO, are no louder than as many before), and its voice ends after the release with silence after it.
 * 4. the corners: CUTOFF, EMPHASIS and CONTOUR at 0 and 127 with all three oscillators and the noise, a chord of four:
 *    bounded; EMPHASIS 127 with the mixer silent sings (the ladder's own sine) and stays bounded.
 * 5. determinism: a note from power-on renders the same samples again.
 * 6. voices: POLY caps at 4; LEGATO keeps one voice and glides; a chord key plays its tones on FMINI.
 * 7. cost: host instructions per sample and voice (4 held) for every preset, and stable over time (the last second
 *    within 10 % of the first); fails above FMINI_COST_MAX. */
#define main hostsim_main
#include "hostsim.c"
#undef main
#ifdef __APPLE__
#include <libproc.h>
#include <sys/resource.h>
#endif

#define FMINI_COST_MAX 350.0      /* host instructions per sample and voice (measured 279 at most: +25 %, as regress) */

static int bad;
static void check(const char *what, int ok)
{
    printf("fmini: %-86s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

static void setup(uint32_t pi)
{
    memset(trk, 0, sizeof trk);
    trk_voices_bind();
    host_tracks_init();
    host_preset(&trk[0], ENGI_FMINI, pi);
}

static int32_t blk[CTL];
static uint32_t nactive(void)
{
    uint32_t i, n = 0;
    for (i = 0; i < NVOICE; i++)
        n += trk[0].v[i].active != 0;
    return n;
}
/* nb blocks of track 0: the peak and the sum of squares (per sample, out's units) */
static void run(uint32_t nb, int64_t *peak, double *ss)
{
    uint32_t i, k;
    for (i = 0; i < nb; i++) {
        track_render(&trk[0], blk, CTL);
        for (k = 0; k < CTL; k++) {
            int64_t a = blk[k] < 0 ? -(int64_t)blk[k] : blk[k];
            if (peak && a > *peak) *peak = a;
            if (ss) *ss += (double)blk[k] * blk[k];
        }
    }
}
#define BLK_S (FS / CTL)          /* blocks a second */

/* ------------------------------------------------------------- 1. table --- */
static void table_tests(void)
{
    const engine_t *e = ENGINES[ENGI_FMINI];
    uint32_t k, names = 1, slots = 1;
    check("engine 22 is FMINI, selectable and in the shown order", e == &ENG_FMINI && eng_ok(ENGI_FMINI) &&
          eng_vis(eng_rank(ENGI_FMINI)) == ENGI_FMINI && !strcmp(e->name, "FMINI"));
    check("192 presets made on demand from the bank (no RAM table)", e->npresets == FMINI_NSLOT && !e->presets && e->preset_make);
    for (k = 0; k < FMINI_NSLOT; k++) {
        const preset_t *p = eng_preset(e, k);
        slots &= p->e[7] == (int8_t)((int32_t)k + FMINI_SLOT0);
        names &= p->name[0] && strlen(p->name) <= 12u;
    }
    check("every preset has a name (<= 12 characters) and plays its own SLOT", names && slots);
    check("the categories: one letter per preset", strlen(FMINI_CATS) == FMINI_NSLOT);
    check("the part state: in the resource arena, under 1 KiB", eng_state_size(ENGI_FMINI) == sizeof(fmini_part_t) &&
          sizeof(fmini_part_t) < 1024u);
    check("the HOME knobs: CUTOFF, EMPHASIS, CONTOUR, F.DEC", e->knob[0] == P_E0 && e->knob[1] == P_E1 &&
          e->knob[2] == P_E2 && e->knob[3] == P_E4);
    setup(0);
    midi_parameter(&trk[0], 74, 99);
    midi_parameter(&trk[0], 71, 11);
    check("CC74 / CC71 play CUTOFF / EMPHASIS", trk[0].p[P_E0] == 99 && trk[0].p[P_E1] == 11);
}

/* ------------------------------------------------------------- 2. patch --- */
static int patch_is(uint32_t tr, uint32_t s)
{
    uint8_t p[FG_SIZE];
    memcpy(p, FMINI_BANK[s].g, FG_SIZE);
    fmini_sanitize(p);
    return !memcmp(fmini_m.patch[tr], p, FG_SIZE) && fmini_m.slot[tr] == s;
}
static void patch_tests(void)
{
    uint32_t ok = 1, k;
    uint8_t g0;
    for (k = 0; k < FMINI_NSLOT; k += 7u) {
        setup(k);
        if (!patch_is(0, k)) {
            printf("fmini:   preset %03u: slot %u, E7 %d\n", k, fmini_m.slot[0], trk[0].p[P_E7]);
            ok = 0;
        }
    }
    check("a preset loads its SLOT's factory patch", ok);
    setup(1);
    g0 = fmini_pgen[0];
    fm1_ms = 1000;
    trk[0].p[P_E7] = 37 + FMINI_SLOT0;
    fmini_poll();
    fm1_ms += 20;
    fmini_poll();
    k = patch_is(0, 1) && fmini_pgen[0] == g0;
    fm1_ms += 40;
    fmini_poll();
    check("a turned SLOT waits until it has been still for 50 ms, then loads its patch", k && patch_is(0, 37));
    trk[0].p[P_E7] = 37 + FMINI_SLOT0;
    g0 = fmini_pgen[0];
    fm1_ms += 100;
    fmini_poll();
    check("an unchanged SLOT loads nothing", fmini_pgen[0] == g0);
    trk[0].p[P_E7] = 127;
    fm1_ms += 100;
    fmini_poll();
    fm1_ms += 100;
    fmini_poll();
    check("SLOT's top (127 stored) is patch 192, the last one", patch_is(0, FMINI_NSLOT - 1));
    {
        char val[8];
        const char *unit = "";
        param_format(&ENG_FMINI.edit[7], -64, val, &unit);
        k = !strcmp(val, "1");
        param_format(&ENG_FMINI.edit[7], 127, val, &unit);
        check("SLOT shows 1..192", k && !strcmp(val, "192"));
    }
    setup(15);
    check("the preset's LFO and modulation come with it", trk[0].p[P_LRATE] == FMINI_BANK[15].lfo[0] &&
          trk[0].p[P_LD_PIT] == FMINI_BANK[15].mod[0] && trk[0].p[P_REL] == FMINI_BANK[15].env[1]);
}

/* ------------------------------------------------------------ 3. presets --- */
static void preset_tests(void)
{
    uint32_t k, quiet = 0, loud = 0, grow = 0, hang = 0, tail = 0;
    int64_t worst = 0;
    for (k = 0; k < FMINI_NSLOT; k++) {
        int64_t p0 = 0, p1 = 0, p2 = 0, pt = 0;
        uint32_t note = FMINI_CATS[k] == 'B' ? 36u : 60u, nb;
        voice_t *v = &trk[0].v[0];
        setup(k);
        trk_note_on(&trk[0], note, 100);
        run(BLK_S, &p0, 0);                              /* the first second: heard */
        for (nb = 0; nb < 15u * BLK_S && (v->stage != 2u || ((uint32_t)v->s[5] >> 24) != 2u); nb++)
            run(1, &p0, 0);                              /* both contours settled (the ADSR, the filter's) */
        nb = trk[0].p[P_LRATE] < 20 ? 16u : 4u;          /* seconds: an LFO's or the oscillators' beating cycle fits */
        run(nb * BLK_S, &p1, 0);
        run(nb * BLK_S, &p2, 0);                         /* as long again: no louder */
        trk_note_off(&trk[0], note);
        for (nb = 0; nb < 20u * BLK_S && nactive(); nb++)
            run(1, 0, 0);
        run(BLK_S / 4u, &pt, 0);
        if (p0 < 300) { quiet++; printf("fmini:   quiet %03u %s: peak %lld\n", k, FMINI_BANK[k].name, (long long)p0); }
        if (p0 > 8 * VOICE_FS || p2 > 8 * VOICE_FS) loud++;
        if (p2 > p1 * 5 / 4 + 300) { grow++; printf("fmini:   grows %03u %s: %lld -> %lld\n", k, FMINI_BANK[k].name, (long long)p1, (long long)p2); }
        if (nactive()) hang++;
        if (pt) tail++;
        if (p0 > worst) worst = p0;
        if (p2 > worst) worst = p2;
    }
    printf("fmini: loudest single note %lld (%.1f dB re VOICE_FS)\n", (long long)worst, 20 * log10((double)worst / VOICE_FS));
    check("every preset sounds (peak >= 300 in its first second)", !quiet);
    check("no preset beyond 8 x VOICE_FS on one note (Prophet's range; the ladder saturates)", !loud);
    check("no preset grows once its contours settle (4 s, slow LFO 16 s, then as long: <= 1.25 x)", !grow);
    check("every voice ends after its release (within 20 s)", !hang);
    check("silence after the release", !tail);
}

/* ------------------------------------------------------------ 4. corners --- */
static void corner_tests(void)
{
    static const uint8_t V[2] = {0, 127};
    uint32_t a, b, c, i, bounded = 1;
    int64_t pk = 0, sing = 0;
    for (a = 0; a < 2; a++)
        for (b = 0; b < 2; b++)
            for (c = 0; c < 2; c++) {
                int64_t p = 0;
                setup(0);
                fmini_m.patch[0][FG_SW1] = FS_O1 | FS_O2 | FS_O3 | FS_NZ;
                fmini_m.patch[0][FG_L1] = fmini_m.patch[0][FG_L2] = fmini_m.patch[0][FG_L3] = fmini_m.patch[0][FG_LN] = 127;
                fmini_pgen[0]++;
                trk[0].p[P_E0] = V[a]; trk[0].p[P_E1] = V[b]; trk[0].p[P_E2] = V[c];
                trk[0].p[P_VOICE] = V_POLY;
                for (i = 0; i < 4u; i++)
                    trk_note_on(&trk[0], 36u + 7u * i, 127);
                int64_t q = 0;
                run(2u * BLK_S, &p, 0);
                run(2u * BLK_S, &q, 0);
                bounded &= p < 32 * VOICE_FS && q <= p * 5 / 4 + 300;
                if (p > pk) pk = p;
            }
    printf("fmini: corners: loudest four-note chord %lld (%.1f dB re VOICE_FS)\n", (long long)pk, 20 * log10((double)pk / VOICE_FS));
    check("CUTOFF / EMPHASIS / CONTOUR corners, everything on, four notes: below 4 x 8 x VOICE_FS, no growth", bounded);
    setup(0);
    fmini_m.patch[0][FG_SW1] = 0;                        /* the mixer silent */
    fmini_pgen[0]++;
    trk[0].p[P_E1] = 127;
    trk[0].p[P_E0] = 64;
    trk_note_on(&trk[0], 60, 100);
    run(BLK_S, 0, 0);
    run(BLK_S, &sing, 0);
    check("EMPHASIS 127 with the mixer silent: the ladder sings, bounded", sing > 1000 && sing < 4 * VOICE_FS);
}

/* --------------------------------------------------------- 5. determinism --- */
static uint64_t hash_note(uint32_t pi)
{
    uint64_t h = 1469598103934665603ull;
    uint32_t i, k;
    setup(pi);
    trk_note_on(&trk[0], 48, 100);
    for (i = 0; i < BLK_S; i++) {
        track_render(&trk[0], blk, CTL);
        for (k = 0; k < CTL; k++)
            h = (h ^ (uint32_t)blk[k]) * 1099511628211ull;
    }
    return h;
}

/* -------------------------------------------------------------- 6. voices --- */
static void voice_tests(void)
{
    uint32_t i, n, moves = 0, steps = 0;
    int32_t last;
    setup(2);
    trk[0].p[P_VOICE] = V_POLY;
    for (i = 0; i < 6u; i++)
        trk_note_on(&trk[0], 48u + 2u * i, 100);
    run(4, 0, 0);
    check("POLY: six notes, at most four voices", (n = nactive()) <= 4u && n >= 1u);
    check("a voice takes 3 units of the shared budget (CPU0 only)", voice_units(&trk[0]) == 3u);
    setup(1);
    trk[0].p[P_VOICE] = V_LEGATO;
    trk[0].p[P_GLIDE] = 60;
    trk_note_on(&trk[0], 36, 100);
    run(BLK_S / 4u, 0, 0);
    trk_note_on(&trk[0], 48, 100);                       /* legato: the first still held */
    for (i = 0, last = -1; i < BLK_S / 2u; i++) {
        uint32_t j;
        run(1, 0, 0);
        for (j = 0; j < NVOICE && !trk[0].v[j].active; j++)
            ;
        if (j < NVOICE) {
            int32_t pc = trk[0].v[j].pitch_cur;
            if (last >= 0 && pc != last) moves++;
            if (pc > 36 * 16 && pc < 48 * 16) steps++;
            last = pc;
        }
    }
    check("LEGATO with GLIDE: one voice, the pitch glides from C2 to C3", nactive() == 1u && moves > 4u && steps > 4u &&
          last == 48 * 16);
    {
        uint8_t notes[8];
        setup(3);
        trk[0].p[P_VOICE] = V_POLY;
        trk[0].p[P_CHRD] = 1;
        n = chord_build(&trk[0], 60, notes);
        for (i = 0; i < n; i++)
            trk_note_on(&trk[0], notes[i], 100);
        run(8, 0, 0);
        check("a chord key's tones sound on FMINI (POLY)", n >= 3u && nactive() == (n < 4u ? n : 4u));
    }
}

/* ---------------------------------------------------------------- 7. cost --- */
static uint64_t instr_now(void)
{
#ifdef __APPLE__
    struct rusage_info_v4 ri;
    if (!proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t *)&ri))
        return ri.ri_instructions;
#endif
    return 0;
}
static double held_cost(uint32_t pi, uint32_t notes, double *drift)
{
    uint32_t i;
    uint64_t i0, i1, i2;
    setup(pi);
    trk[0].p[P_VOICE] = V_POLY;
    trk[0].p[P_SUS] = 127;                               /* (held at SUS 0 a voice renders nothing: voice.c) */
    for (i = 0; i < notes; i++)
        trk_note_on(&trk[0], 48u + 5u * i, 100);
    run(64, 0, 0);
    i0 = instr_now();
    run(BLK_S, 0, 0);
    i1 = instr_now();
    run(BLK_S, 0, 0);
    run(BLK_S, 0, 0);
    i2 = instr_now();
    run(BLK_S, 0, 0);
    if (drift) {
        uint64_t i3 = instr_now();
        *drift = (double)(i3 - i2) / (double)(i1 - i0);
    }
    return (double)(i1 - i0) / ((double)BLK_S * CTL * notes);
}
static void cost_tests(void)
{
    double worst = 0, sum = 0, d, dworst = 1;
    uint32_t k, wk = 0;
    if (!instr_now()) {
        printf("fmini: cost: no instruction counter on this host (proc_pid_rusage); not measured\n");
        return;
    }
    for (k = 0; k < FMINI_NSLOT; k++) {
        double c = held_cost(k, 4, &d);
        sum += c;
        if (c > worst) { worst = c; wk = k; }
        if (fabs(d - 1) > fabs(dworst - 1)) dworst = d;
    }
    printf("fmini: cost, host instructions per sample and voice (4 held): mean %.0f, worst %.0f (%03u %s)\n",
           sum / FMINI_NSLOT, worst, wk, FMINI_BANK[wk].name);
    printf("fmini: cost over time: the fourth second / the first, worst %.3f\n", dworst);
    {
        char what[160];
        snprintf(what, sizeof what, "cost: at most %.0f instructions a sample and voice (limit %.0f)", worst, FMINI_COST_MAX);
        check(what, worst <= FMINI_COST_MAX);
    }
    check("cost stable while held (the fourth second within 10 % of the first)", fabs(dworst - 1) < 0.10);
}

int main(void)
{
    table_tests();
    patch_tests();
    preset_tests();
    corner_tests();
    check("a note from power-on renders the same samples again", hash_note(1) == hash_note(1) && hash_note(30) == hash_note(30));
    voice_tests();
    cost_tests();
    printf("fmini: %s\n", bad ? "FAILED" : "all ok");
    return bad != 0;
}
