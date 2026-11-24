/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (FMini-1, https://github.com/ruppert/fmini-1): the FMINI engine, its filter and patches
 * Portions Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments: the Felucca engine interface */
/* FMINI (FMini-1): a monosynth in the style of the classic three-oscillator Moog instruments, ported from FMini-1
 * (main c39e186, v2.6.x) as Melodee engine 22.
 *
 * Signal path (per voice): three oscillators (TRI, SHARK, SAW, SQR, wide and narrow PULSE, band-limited by polyBLEP,
 * ranges LO 32' 16' 8' 4' 2') and a noise source (white or pink) -> the mixer (a level each, the sum saturates softly:
 * the "overload" of the original) -> a 24 dB/oct transistor-ladder low-pass modelled on the Minimoog's (see below)
 * with its own contour (ATTACK DECAY SUSTAIN; the release takes the DECAY time) and keyboard tracking at 1 V/oct. The
 * loudness contour is the host's ADSR (P_ATK .. P_REL). Modulation: the track's LFO (or noise, MOD MIX) to the pitch
 * (MOD OSC) and the filter (MOD FLT), its depth E6 MOD (the matrix can drive it); the mod wheel (CC1) is a vibrato on
 * the pitch, +-1 semitone.
 *
 * The sound is the track's patch (fmini_patch, FG_SIZE bytes): the factory patch of E7 SLOT (1..192 shown, stored as
 * slot - 64: a stored parameter is one byte, -64..127; FMINI_BANK in the resource region). Projects and user presets keep the eight EDIT values, SLOT with them; the patch is loaded from
 * SLOT again (fmini_poll), so no store holds the patch itself. The eight EDIT values are the ones to play with (E0 CUT,
 * E1 EMPH, E2 CNTR the contour's amount, E3 F.ATK, E4 F.DEC, E5 F.SUS, E6 MOD, E7 SLOT).
 * FMini-1's last eight factory patches (FX) are not here: SLOT's byte reaches 192.
 *
 * Changes from FMini-1: voice_t has six state slots here, so s[0..3] are the ladder, s[4] the feedback's lag pole (all
 * Q8) and s[5] the contour (stage << 24 | level, Q23; FMini-1's s[7]); the oscillators' drift and the part's audio
 * state live in fmini_part_t (the resource arena, engines.c eng_state_size). Per-track arrays are NPART wide (the MIDI
 * tracks never sound). No OWN patch, no patch tempo; the patch's level is a trim (fmini_lvl), not the track's fader.
 * eng_fmini_filter.h is FMini-1's, unchanged (its tools/fmini_filter.py generates it). */
#define FMINI_POLY 4
#define FG_SIZE 32u
#define FMINI_NSLOT 192u          /* the factory patches SLOT reaches (eng_fmini_bank.h): FMini-1's first 192 of 200 */
#define FMINI_SLOT0 (-64)         /* SLOT (E7) as stored: slot - 64, so -64..127 fits a project's and a preset's byte */
#define fmini_slot_of(v) ((uint32_t)clamp((int32_t)(v) - FMINI_SLOT0, 0, FMINI_NSLOT - 1))
#define FMINI_LEVEL0 104          /* the bank's levels are trims against this P_LEVEL (the default fader) */

enum {                           /* the patch's bytes */
    FG_R1, FG_R2, FG_R3,         /* range: 0 LO, 1 32', 2 16', 3 8', 4 4', 5 2' */
    FG_W1, FG_W2, FG_W3,         /* wave: 0 TRI, 1 SHARK, 2 SAW, 3 SQR, 4 PLS W, 5 PLS N, 6 SIN */
    FG_F2, FG_F3,                /* OSC 2 / 3 frequency, 64 = 0, +-7 semitones */
    FG_L1, FG_L2, FG_L3, FG_LN,  /* mixer levels 0..127 (a squared curve) */
    FG_SW1,                      /* FS_ bits */
    FG_SW2,                      /* FT_ bits */
    FG_TUNE,                     /* 64 = A 440, +-100 cents */
    FG_MMIX,                     /* modulation source: 0 LFO .. 127 noise */
    FG_KBD,                      /* keyboard tracking of the cutoff, 0..127 = 0..1 */
    FG_GLID,                     /* glide time kept while the GLIDE switch is off (FMini-1's panel; unused here) */
    FG_DC2L, FG_DC2H,            /* DET 2 in cents: a signed 16-bit (low, high byte), -700 .. +700 */
    FG_DC3L, FG_DC3H,            /* DET 3 in cents, the same */
    FG_BPML, FG_BPMH,            /* FMini-1's patch tempo (unused here: the song's tempo stays) */
    FG_NUM
};
enum { FS_O1 = 1, FS_O2 = 2, FS_O3 = 4, FS_NZ = 8, FS_PINK = 16, FS_MOSC = 32, FS_MFLT = 64 };   /* FG_SW1 */
enum { FT_GLIDE = 1, FT_SQR = 2, FT_HP = 4 };                                                      /* FG_SW2 (FT_HP: the high-pass output) */

/* DET 2 / 3, in cents: the signed 16-bit at bytes lo, lo + 1 (FG_DC2L, FG_DC3L) */
static int32_t fmini_det(const uint8_t *g, uint32_t lo)
{
    int32_t m = g[lo] | (int32_t)(g[lo + 1] & 7) << 7;
    return g[lo + 1] & 8 ? -m : m;
}

/* a factory patch and its preset (eng_fmini_bank.h, tools/gen_fmini_bank.py) */
typedef struct {
    uint8_t g[FG_SIZE];
    char name[13];
    int8_t e[8];                 /* P_E0..P_E7 */
    uint8_t env[4];              /* ATK DEC SUS REL */
    int8_t fenv;
    uint8_t mono;
    uint8_t lfo[4];              /* P_LRATE P_LWAVE P_LFADE P_LSYNC */
    uint8_t level;               /* FMini-1's P_LEVEL for the patch: here a trim against FMINI_LEVEL0 */
    int8_t mod[3];               /* P_LD_PIT P_LD_AMP P_ED_PIT */
} fmini_rom_t;
#include "eng_fmini_bank.h"            /* FMINI_BANK (the resource region) and FMINI_CATS: tools/gen_fmini_bank.py */
#include "eng_fmini_filter.h"          /* the ladder's laws: FMINI_CUT FMINI_K FMINI_TRIM FMINI_G FMINI_GX (FMini-1) */

/* a part's audio state (RES_ENGINE0 + part while FMINI plays it: engines.c eng_state_size; zeroed when made) */
typedef struct {
    uint8_t p[FG_SIZE];          /* the patch as the audio ISR sees it (taken at a block's start) */
    uint8_t gen, ok, sm_ok, rsv;
    int32_t lvl12;               /* the patch's level trim, Q12 */
    int32_t pk[3];               /* the pink filter */
    int32_t nbuf[CTL];           /* the part's noise for this block, +-32768: one source, all its voices */
    int32_t sm[3];               /* the smoothed CUTOFF, EMPH, CNTR knobs (Q16): the encoder's steps become ramps */
    int32_t mn;                  /* the modulation noise of this block, +-16384 */
    int32_t drift[NVOICE][2];    /* the VCOs' drift per voice, cents * 256 (FMini-1's s[5], s[6]) */
} fmini_part_t;
static fmini_part_t *fmini_part(uint32_t part);   /* engines.c */

/* the main loop's side (it writes a patch, then fmini_pgen: the ISR copies it at its next block) */
static struct {
    uint8_t patch[NPART][FG_SIZE];
    int32_t lvl12[NPART];
    uint16_t slot[NPART];        /* the SLOT value last loaded; 0xFFFF = none */
    uint16_t want[NPART];        /* the slot the knob points to, not loaded yet (0xFFFF: none) */
    uint32_t want_ms[NPART];     /* when want last changed */
    int32_t nst[NPART];          /* a part's noise generator (note-on, the audio ISR) */
} fmini_m __attribute__((section(".pool")));
static volatile uint8_t fmini_pgen[NPART];
#define FMINI_LOAD_MS 50u        /* a SLOT that keeps turning loads when it stops for this long */

/* the highest value of each byte */
static uint32_t fmini_max(uint32_t i)
{
    static const uint8_t MAXV[FG_NUM] = {5, 5, 5, 6, 6, 6, 127, 127, 127, 127, 127, 127, 127, 7, 127, 127, 127, 127, 127, 15, 127, 15, 127, 127};
    return i < FG_NUM ? MAXV[i] : 0u;
}

static void fmini_sanitize(uint8_t *v)
{
    uint32_t i;
    for (i = 0; i < FG_SIZE; i++)
        v[i] = i < FG_NUM ? (uint8_t)(v[i] > fmini_max(i) ? fmini_max(i) : v[i]) : 0;
}

/* the INIT patch: a sawtooth on oscillator 1 alone (the resource region missing) */
static void fmini_init_patch(uint8_t *p)
{
    memset(p, 0, FG_SIZE);
    p[FG_R1] = p[FG_R2] = p[FG_R3] = 3;          /* 8' */
    p[FG_W1] = p[FG_W2] = p[FG_W3] = 2;          /* SAW */
    p[FG_F2] = p[FG_F3] = 64;
    p[FG_L1] = 127;
    p[FG_SW1] = FS_O1;
    p[FG_TUNE] = 64;
}

/* --------------------------------------------------------------- the patch --- */
/* part tr's patch = slot s's (main loop; the ISR takes it at its next block) */
static void fmini_load_slot(uint32_t tr, uint32_t s)
{
    uint8_t p[FG_SIZE];
    uint32_t lv = FMINI_LEVEL0;
    tr %= NPART;
    s %= FMINI_NSLOT;
    if (RES_OK()) {
        memcpy(p, FMINI_BANK[s].g, FG_SIZE);
        lv = FMINI_BANK[s].level & 127u;
    } else
        fmini_init_patch(p);                     /* (res.c: the factory patches not installed) */
    fmini_sanitize(p);
    memcpy(fmini_m.patch[tr], p, FG_SIZE);
    fmini_m.lvl12[tr] = (int32_t)(((uint32_t)LEVEL_Q12[lv] << 12) / (uint32_t)LEVEL_Q12[FMINI_LEVEL0]);
    RING_PUBLISH();
    fmini_pgen[tr]++;
    fmini_m.slot[tr] = (uint16_t)s;
}

/* a factory sound load (apply_preset_to: a preset, an engine change): its patch now, and what FMini-1's preset sets
 * beyond preset_t (the LFO, the common modulation). The arpeggiator, the effects and the octave stay the track's and
 * the project's; the level is the patch's trim (fmini_load_slot), the fader stays */
static void fmini_track_loaded(track_t *t)
{
    uint32_t tr = (uint32_t)(t - trk);
    if (tr >= NPART || t->eng_req != ENGI_FMINI)
        return;
    fmini_load_slot(tr, fmini_slot_of(t->p[P_E7]));
    fmini_m.want[tr] = 0xFFFFu;
    if (t->preset < FMINI_NSLOT && RES_OK()) {
        const fmini_rom_t *r = &FMINI_BANK[t->preset];
        t->p[P_LRATE] = r->lfo[0];
        t->p[P_LWAVE] = r->lfo[1];
        t->p[P_LFADE] = r->lfo[2];
        t->p[P_LSYNC] = r->lfo[3];
        t->p[P_LD_PIT] = r->mod[0];
        t->p[P_LD_AMP] = r->mod[1];
        t->p[P_ED_PIT] = r->mod[2];
    }
}

static void fmini_init(void)                     /* power-on: every part the first patch, the noise seeded */
{
    static const int32_t SEED[4] = {0x2F6E2B1, 0x1A2B3C5, 0x3C4D5E7, 0x0F1E2D3};
    uint32_t tr;
    for (tr = 0; tr < NPART; tr++) {
        fmini_m.nst[tr] = SEED[tr % 4u];
        fmini_load_slot(tr, 0);
        fmini_m.slot[tr] = 0xFFFFu;              /* (the first poll loads what the track's SLOT says) */
        fmini_m.want[tr] = 0xFFFFu;
    }
}

/* main loop: SLOT changed (a knob, the editor, MIDI, motion, a project or user preset load) -> that factory patch,
 * once the knob has been still for FMINI_LOAD_MS (a load the moment SLOT stops, not on every click) */
static void fmini_poll(void)
{
    uint32_t tr;
    for (tr = 0; tr < NPART; tr++) {
        uint32_t s = fmini_slot_of(trk[tr].p[P_E7]);
        if (trk[tr].eng_req != ENGI_FMINI || s == fmini_m.slot[tr]) {
            fmini_m.want[tr] = 0xFFFFu;
            continue;
        }
        if (fmini_m.slot[tr] != 0xFFFFu) {       /* (the first load after power-on or an engine change: at once) */
            if (fmini_m.want[tr] != (uint16_t)s) {
                fmini_m.want[tr] = (uint16_t)s;
                fmini_m.want_ms[tr] = fm1_ms;
                continue;
            }
            if (fm1_ms - fmini_m.want_ms[tr] < FMINI_LOAD_MS)
                continue;
        }
        fmini_load_slot(tr, s);
        fmini_m.want[tr] = 0xFFFFu;
    }
}

/* preset k: slot k's row (FMini-1's factory releases: the loudness's RELEASE is its DECAY, as they always sounded) */
static void fmini_preset_make(uint32_t k, preset_t *p, char *name)
{
    uint32_t i;
    k %= FMINI_NSLOT;
    if (!RES_OK()) {                             /* (the factory patches not installed: INIT on every slot) */
        memcpy(name, "INIT", 5);
        p->e[0] = 127;
        p->e[7] = (int8_t)((int32_t)k + FMINI_SLOT0);
        p->env[2] = 127;
        return;
    }
    const fmini_rom_t *r = &FMINI_BANK[k];
    for (i = 0; i < 12u && r->name[i]; i++)
        name[i] = r->name[i];
    name[i] = 0;
    for (i = 0; i < 8u; i++)
        p->e[i] = r->e[i];
    for (i = 0; i < 4u; i++)
        p->env[i] = r->env[i];
    p->env[3] = r->env[1];
    p->fenv = r->fenv;
    p->mono = r->mono;
}

/* once a block and part, before its voices: the ISR's copy of the patch, the noise (one source for all the part's
 * voices, white or pink: Kellet's filter) and the modulation noise */
static void fmini_block(track_t *t)
{
    uint32_t tr = (uint32_t)(t - trk), i;
    fmini_part_t *fp;
    const uint8_t *g;
    if (tr >= NPART)
        return;
    fp = fmini_part(tr);
    if (!fp->ok || fp->gen != fmini_pgen[tr]) {
        fp->gen = fmini_pgen[tr];
        memcpy(fp->p, fmini_m.patch[tr], FG_SIZE);
        fp->lvl12 = fmini_m.lvl12[tr];
        fp->ok = 1;
    }
    for (i = 0; i < NVOICE && !t->v[i].active; i++)
        ;
    {   /* the knobs' steps (one encoder click = 1/127) glide: a one-pole per block, time constant 200 ms (275 blocks of
         * 0.73 ms: 1/275 = 238/65536 of the distance a block), exact landing; silent = snap */
        static const uint8_t KN[3] = {P_E0, P_E1, P_E2};
        uint32_t j;
        for (j = 0; j < 3; j++) {
            int32_t want = (int32_t)clamp(t->p[KN[j]], 0, 127) << 16, d = want - fp->sm[j], st;
            if (i == NVOICE || !fp->sm_ok) {
                fp->sm[j] = want;
                continue;
            }
            st = d >= 0 ? (int32_t)(((int64_t)d * 238) >> 16) : -(int32_t)(((int64_t)-d * 238) >> 16);
            if (!st)
                st = d > 0 ? 1 : d < 0 ? -1 : 0;
            fp->sm[j] += st;
        }
        fp->sm_ok = (uint8_t)(i != NVOICE);
    }
    if (i == NVOICE)
        return;                                              /* nothing sounds: no noise to make */
    g = fp->p;
    fp->mn = (int32_t)(noise32(&fmini_m.nst[tr]) >> 17) - 16384;
    if (g[FG_SW1] & FS_NZ) {
        int32_t b0 = fp->pk[0], b1 = fp->pk[1], b2 = fp->pk[2];
        for (i = 0; i < CTL; i++) {
            int32_t w = (int32_t)(noise32(&fmini_m.nst[tr]) >> 16) - 32768;   /* white, +-32768 */
            if (g[FG_SW1] & FS_PINK) {
                b0 += ((w * 3245) >> 15) - ((b0 * 77) >> 15);              /* 0.0990 in, 0.99765 kept */
                b1 += ((w * 9716) >> 15) - ((b1 * 1212) >> 15);            /* 0.2965 in, 0.963 kept */
                b2 += ((w * 34495) >> 15) - ((b2 * 14090) >> 15);          /* 1.0527 in, 0.57 kept */
                w = (b0 + b1 + b2 + ((w * 6056) >> 15)) >> 1;
            }
            fp->nbuf[i] = w;
        }
        fp->pk[0] = b0;
        fp->pk[1] = b1;
        fp->pk[2] = b2;
    }
}

/* ------------------------------------------------------------------ voice --- */
static inline int32_t fmini_shark(uint32_t ph)            /* a triangle that rises for 3/4 of the cycle */
{
    uint32_t x = ph >> 16;
    return x < 49152u ? (int32_t)((x * 43691u) >> 15) - 32768 : 32767 - (int32_t)((x - 49152u) * 4u);
}

/* the ladder's input is the mixer's sum 12 dB below full scale and its output raised by the same: the CA-72's ladder
 * runs well under its pairs' knee, so a sawtooth or triangle comes out as drawn while the singing sits at the CA-72's
 * level against the waveform */
#define FMINI_LADDER_IN 2
/* the differential pair: tanh with its bias, the bias's own offset taken away (the CA-72's pairs sit on a bias; its 2nd
 * harmonic at full level is -37 dB, the triangle's odd harmonics unchanged). pair_off = softclip(FMINI_PAIR_BIAS), once a block */
#define FMINI_PAIR_BIAS 1500
static inline int32_t pairb(int32_t x, int32_t pair_off)
{
    return softclip(x + FMINI_PAIR_BIAS) - pair_off;
}

/* one oscillator of the mix: mix[i] += its sample at level g (Q15); a loop per wave. keep = -1 adds to what the mix
 * holds; keep = 0 writes it (the first oscillator: no zeroing pass, 0 + x = x exactly).
 * The polyBLEP residual is zero except within one step of a wrap: its bounds (lo, hi) and the step d are
 * worked out once per call, so each sample tests two constants (the same branches as dsp.c's blep, same bits). */
static inline int32_t fm_blep(uint32_t ph, uint32_t d, uint32_t lo, uint32_t hi)
{
    int32_t x;
    if (ph < lo) {
        x = (int32_t)(ph / d);
        return x + x - ((x * x) >> 15) - 32768;
    }
    if (ph > hi) {
        x = -(int32_t)((0xFFFFFFFFu - ph) / d);
        return ((x * x) >> 15) + x + x + 32768;
    }
    return 0;
}

static void fmini_wave(uint32_t w, uint32_t *php, uint32_t inc, int32_t g, int32_t *mix, uint32_t n, int32_t keep)
{
    uint32_t ph = *php, i, d = inc >> 15;
    uint32_t lo = d ? inc : 0u, hi = d ? 0xFFFFFFFFu - inc : 0xFFFFFFFFu;   /* d == 0: no residual at all (dsp.c blep) */
    switch (w) {
    case 0:
        for (i = 0; i < n; i++) {
            mix[i] = (mix[i] & keep) + mulq15(osc_tri(ph), g);
            ph += inc;
        }
        break;
    case 1:
        for (i = 0; i < n; i++) {
            mix[i] = (mix[i] & keep) + mulq15(fmini_shark(ph), g);
            ph += inc;
        }
        break;
    case 2:
        for (i = 0; i < n; i++) {
            mix[i] = (mix[i] & keep) + mulq15(((int32_t)(ph >> 16) - 32768) - fm_blep(ph, d, lo, hi), g);
            ph += inc;
        }
        break;
    case 3:                                             /* PULSE 50 %: the saw minus the saw half a turn on */
        for (i = 0; i < n; i++) {
            uint32_t q = ph + 0x80000000u;
            mix[i] = (mix[i] & keep) + mulq15((((int32_t)(ph >> 16) - 32768) - fm_blep(ph, d, lo, hi)
                            - (((int32_t)(q >> 16) - 32768) - fm_blep(q, d, lo, hi))) >> 1, g);
            ph += inc;
        }
        break;
    case 4:
        for (i = 0; i < n; i++) {
            uint32_t q = ph + 0x55555555u;
            mix[i] = (mix[i] & keep) + mulq15((((int32_t)(ph >> 16) - 32768) - fm_blep(ph, d, lo, hi)
                            - (((int32_t)(q >> 16) - 32768) - fm_blep(q, d, lo, hi))) >> 1, g);
            ph += inc;
        }
        break;
    case 6:                                             /* SIN: the sine table (dsp.c), no harmonics to fold back */
        for (i = 0; i < n; i++) {
            mix[i] = (mix[i] & keep) + mulq15(osc_sine(ph), g);
            ph += inc;
        }
        break;
    default:
        for (i = 0; i < n; i++) {
            uint32_t q = ph + 0x1C71C71Cu;
            mix[i] = (mix[i] & keep) + mulq15((((int32_t)(ph >> 16) - 32768) - fm_blep(ph, d, lo, hi)
                            - (((int32_t)(q >> 16) - 32768) - fm_blep(q, d, lo, hi))) >> 1, g);
            ph += inc;
        }
        break;
    }
    *php = ph;
}

/* range of an oscillator in 1/16 semitones (LO: very low) */
static int32_t fmini_range16(uint32_t r)
{
    static const int16_t R[6] = {-48 * 16, -24 * 16, -12 * 16, 0, 12 * 16, 24 * 16};
    return R[r % 6u];
}

static void fmini_note_on(track_t *t, voice_t *v)
{
    uint32_t tr = (uint32_t)(t - trk) % NPART;
    if (!v->env && !v->env_out) {                          /* from silence: the ladder and the contour from 0 */
        int32_t *dr = fmini_part(tr)->drift[(uint32_t)(v - t->v) % NVOICE];
        v->s[0] = v->s[1] = v->s[2] = v->s[3] = 0;
        v->s[4] = 0;
        v->s[5] = 0;
        if (t->p[P_E1] >= 96)                                         /* it will sing: a nudge to start from (the hardware has its own noise) */
            v->s[0] = (noise32(&fmini_m.nst[tr]) & 0x80000000u) ? 700 * 256 : -700 * 256;
        dr[0] = (int32_t)(noise32(&fmini_m.nst[tr]) >> 20) - 2048;   /* the VCOs start a little apart: drift, cents * 256 */
        dr[1] = (int32_t)(noise32(&fmini_m.nst[tr]) >> 20) - 2048;
    }
    v->s[5] = (int32_t)(1u << 24) | (v->s[5] & 0xFFFFFF);  /* the contour attacks from where it is */
}

/* the contour, once a block: stage 1 attack, 2 decay to the sustain, 3 release (the DECAY time, kr); level Q23 */
static int32_t fmini_contour(const int16_t *p, voice_t *v, int32_t kr)
{
    uint32_t st = (uint32_t)v->s[5] >> 24;
    int32_t e = v->s[5] & 0xFFFFFF, sus = (int32_t)p[P_E5] << 16, k = (int32_t)ENV_EXP[p[P_E4] & 127];
    if (!v->gate && st && st != 3u)
        st = 3;
    switch (st) {
    case 1:
        {   /* the capacitor charges toward a level above full: a concave rise that ends in a corner, as in the Minimoog */
            int32_t step = (int32_t)(ENV_LIN[p[P_E3] & 127] >> 1), d = (int32_t)(((int64_t)((5 << 21) - e) * step * 824) >> 32);
            e += d < 1 ? 1 : d;
        }
        if (e >= (1 << 23)) {
            e = 1 << 23;
            st = 2;
        }
        break;
    case 2:
        e += mulq16(sus - e, (uint32_t)k);                  /* the DECAY time is the drop from the peak to the SUSTAIN level, whatever it is */
        break;
    case 3:
        e -= mulq16(e, (uint32_t)kr);                       /* the RELEASE */
        break;
    default:
        break;
    }
    v->s[5] = (int32_t)(st << 24) | (e & 0xFFFFFF);
    return e >> 8;                                          /* Q15 */
}

/* a * k / 65536 rounded to the nearest (mulq16 floors: four stages of it leave a bias that a quiet, low ladder cannot sing against) */
static inline int32_t fmini_mulr(int32_t a, uint32_t k)
{
    return (int32_t)(((a >> 16) * (int32_t)k) + (int32_t)(((uint32_t)(a & 0xFFFF) * k + 32768u) >> 16));
}

/* The filter, after the Minimoog's (FMini-1's tools/fmini_filter.py has the laws and the reasoning, the CA-72 project's
 * calibration the measurements): four equal one-pole stages, each a differential pair (tanh in front), the last stage's
 * output fed back to the first through a lag pole (the emphasis network), solved without a delay in the loop (zero-delay
 * feedback: each stage is a trapezoidal integrator, G = g / (1 + g), and the feedback is found from the stages' states
 * before the sample is processed). The lag pole puts the loop's 180-degree point at 0.686 of the stage pole: the filter
 * sings at 1.578 times its -3 dB corner, and it sings when the feedback k reaches 2.92. The knobs and the pitch work in
 * 1/16 semitones of the singing pitch (MIDI x 16): CUTOFF is the dial -5 .. +5 (FMINI_CUT), the keyboard adds 1 V/oct
 * from F (MIDI 41) at KBD 127, the contour up to 6.3 octaves through the CA-72's AMOUNT taper (FMINI_CNTR); EMPHASIS is
 * the dial 0 .. 10 (FMINI_K) with the hardware's passband loss (FMINI_TRIM). With the mixer silent and EMPHASIS past 8
 * the ladder sings a sine at the singing pitch, 1 V/oct over the keyboard.
 * Q formats: signals +-32768 (tanh(1)); the states Q8; G Q16; the prediction's coefficients c Q14; k Q12. */
static void fmini_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t tr = (uint32_t)(t - trk) % NPART, i;
    fmini_part_t *fp = fmini_part(tr);
    const uint8_t *g = fp->p;
    int32_t mix[CTL];
    int32_t env15, mod, lfo, vib, lc, p16, fine, k12, G16, gx16, c0, c1, c2, c3, inv14, kinv12, trim12, pair_off;
    int32_t l1 = 0, l2 = 0, l3 = 0, ln = 0, dr1, dr2, dr3;
    uint32_t hp = (uint32_t)(g[FG_SW2] & FT_HP) ? 1u : 0u;      /* the output: LP (the last stage) or HP ((1 - LP)^4 of the same stages) */
    uint32_t sw = g[FG_SW1];
    uint32_t inc1, inc2, inc3;
    int32_t s0 = v->s[0], s1 = v->s[1], s2 = v->s[2], s3 = v->s[3], sx = v->s[4];
    uint32_t ph0 = v->ph[0], ph1 = v->ph[1], ph2 = v->ph[2];
    if (n > CTL)
        n = CTL;
    env15 = fmini_contour(p, v, (int32_t)ENV_EXP[p[P_E4] & 127]);

    /* modulation: the LFO (a square when FT_SQR) or noise held for the block, MOD deep */
    lfo = mulq15(t->lfo_val, t->lfo_fade);
    if (g[FG_SW2] & FT_SQR)
        lfo = lfo >= 0 ? 32767 : -32767;
    mod = mulq15(lfo, 32767 - (int32_t)g[FG_MMIX] * 258) + mulq15(fp->mn << 1, (int32_t)g[FG_MMIX] * 258);
    mod = mulq15(mod, (int32_t)p[P_E6] * 258);                          /* the patch's MOD: Q15 of its depth */
    /* the mod wheel (CC1, t->mw) is the vibrato: the LFO on the pitch, +-1 semitone at full wheel, whatever MOD OSC is */
    vib = (mulq15(lfo, (int32_t)t->mw * 258) * 16) >> 15;

    /* oscillator pitches (the global tuning: cents_inc's tuned_pitch_inc and m->fine) */
    p16 = m->pitch16 + vib + (sw & FS_MOSC ? (mod * 16) >> 14 : 0);    /* full depth: +-2 semitones */
    fine = m->fine + ((int32_t)g[FG_TUNE] - 64) * 237 / 64;
    {   /* the VCOs wander a few cents (temperature, the integrators): a slow random walk per oscillator, about +-2 cents */
        int32_t *dr = fp->drift[(uint32_t)(v - t->v) % NVOICE];
        int32_t d1 = dr[0], d2 = dr[1];
        d1 += (((int32_t)(noise32(&fmini_m.nst[tr]) >> 20) - 2048) >> 6) - (d1 >> 11);
        d2 += (((int32_t)(noise32(&fmini_m.nst[tr]) >> 20) - 2048) >> 6) - (d2 >> 11);
        dr[0] = d1;
        dr[1] = d2;
        dr1 = d1 >> 8;
        dr2 = d2 >> 8;
        dr3 = (d1 - d2) >> 9;
    }
    inc1 = cents_inc(clamp(p16 + fmini_range16(g[FG_R1]), 0, 2047), dr1, fine);
    inc2 = cents_inc(clamp(p16 + fmini_range16(g[FG_R2]), 0, 2047), fmini_det(g, FG_DC2L) + dr2, fine);
    inc3 = cents_inc(clamp(p16 + fmini_range16(g[FG_R3]), 0, 2047), fmini_det(g, FG_DC3L) + dr3, fine);

    /* mixer levels: a squared curve */
    if (sw & FS_O1) l1 = (int32_t)(g[FG_L1] * g[FG_L1] * 2);
    if (sw & FS_O2) l2 = (int32_t)(g[FG_L2] * g[FG_L2] * 2);
    if (sw & FS_O3) l3 = (int32_t)(g[FG_L3] * g[FG_L3] * 2);
    if (sw & FS_NZ) ln = (int32_t)(g[FG_LN] * g[FG_LN] * 2);
    {   /* the first oscillator that runs writes the mix (keep 0); the others add (keep -1) */
        int32_t keep = 0;
        if (l1) {
            fmini_wave(g[FG_W1], &ph0, inc1, l1, mix, n, keep);
            keep = -1;
        } else
            ph0 += inc1 * n;
        if (l2) {
            fmini_wave(g[FG_W2], &ph1, inc2, l2, mix, n, keep);
            keep = -1;
        } else
            ph1 += inc2 * n;
        if (l3) {
            fmini_wave(g[FG_W3], &ph2, inc3, l3, mix, n, keep);
            keep = -1;
        } else
            ph2 += inc3 * n;
        if (!keep)                                      /* no oscillator: the mix starts at zero */
            for (i = 0; i < n; i++)
                mix[i] = 0;
    }
    if (ln)
        for (i = 0; i < n; i++)
            mix[i] += mulq15(fp->nbuf[i], ln);

    /* the singing pitch (MIDI x 16): the CUTOFF knob's dial, the contour, the keyboard, the modulation, the host's (LFO / matrix / ENV routings) */
    {
        int32_t kn = fp->sm[0], i0 = kn >> 16, fr = (kn >> 8) & 255;
        lc = FMINI_CUT[i0];
        if (i0 < 127)
            lc += ((FMINI_CUT[i0 + 1] - lc) * fr) >> 8;
    }
    lc += (m->cutoff * 55) >> 10;                                                                           /* the host's steps (1/14 octave) in 1/16 semitones */
    {                                                                                                           /* the contour: the CA-72's taper, up to 6.3 octaves */
        uint32_t ci = (uint32_t)fp->sm[2] >> 16, cf = ((uint32_t)fp->sm[2] & 0xFFFFu) >> 1;
        int32_t ctap;
        if (ci > 126u) { ci = 126u; cf = 32768u; }
        ctap = (int32_t)FMINI_CNTR[ci] + (int32_t)((((int32_t)FMINI_CNTR[ci + 1u] - (int32_t)FMINI_CNTR[ci]) * (int32_t)cf) >> 15);
        lc += (int32_t)(((int64_t)env15 * ctap * FMINI_CNTR_FULL16) >> 30);
    }
    lc += ((v->pitch_cur - FMINI_NOTE0 * 16) * (int32_t)g[FG_KBD] * 516) >> 16;                              /* the keyboard: 1 V/oct at 127, 0 V on F */
    if (sw & FS_MFLT)
        lc += (mod * 547) >> 15;                                                                            /* +-2.85 octaves at full depth */
    lc = clamp(lc, FMINI_GM0 * 16, FMINI_FOSC_MAX16);
    {   /* the stage pole G and the lag pole's from the tables (one entry a semitone, linear between), the feedback solved ahead */
        uint32_t ix = (uint32_t)((lc >> 4) - FMINI_GM0), fr = (uint32_t)lc & 15u, gq, gxq, g2, g3, g4, om;
        gq = FMINI_G[ix] + (uint32_t)(((int32_t)(FMINI_G[ix + 1] - FMINI_G[ix]) * (int32_t)fr) >> 4);
        gxq = FMINI_GX[ix] + (uint32_t)(((int32_t)(FMINI_GX[ix + 1] - FMINI_GX[ix]) * (int32_t)fr) >> 4);
        G16 = (int32_t)((gq + 128u) >> 8);
        gx16 = (int32_t)((gxq + 128u) >> 8);
        om = 65536u - (uint32_t)G16;
        g2 = ((uint32_t)G16 * (uint32_t)G16) >> 16;
        g3 = (g2 * (uint32_t)G16) >> 16;
        g4 = (g3 * (uint32_t)G16) >> 16;
        c3 = (int32_t)(om >> 2);                                   /* y4 = G^4 u + c0 s0 + c1 s1 + c2 s2 + c3 s3 */
        c2 = (int32_t)(((uint32_t)G16 * om) >> 18);
        c1 = (int32_t)((g2 * om) >> 18);
        c0 = (int32_t)((g3 * om) >> 18);
        {   /* EMPHASIS: the feedback gain k and the passband's loss, from the knob (Q16) */
            int32_t kn = fp->sm[1], i0 = kn >> 16, fk = (kn >> 8) & 255, den;
            k12 = FMINI_K[i0];
            trim12 = FMINI_TRIM[i0];
            if (i0 < 127) {
                k12 += ((FMINI_K[i0 + 1] - k12) * fk) >> 8;
                trim12 += ((FMINI_TRIM[i0 + 1] - trim12) * fk) >> 8;
            }
            den = 65536 + ((k12 * (int32_t)((((uint32_t)gxq >> 8) * g4) >> 16)) >> 12);        /* 1 + k Gx G^4: the loop around the whole sample */
            inv14 = (int32_t)((1u << 30) / (uint32_t)den);
            kinv12 = (k12 * inv14) >> 14;
        }
    }
    trim12 = (trim12 * fp->lvl12) >> 12;                           /* the patch's level (FMini-1's P_LEVEL), Q12 */

    const int32_t amp0 = m->amp0;                      /* the voice's ramp (voice_amp), as a running sum below */
    uint32_t xa = 0, da = (uint32_t)(m->amp1 - m->amp0);
    pair_off = softclip(FMINI_PAIR_BIAS);
    for (i = 0; i < n; i++) {
        int32_t x = (mix[i] > (32767 << FMINI_LADDER_IN) ? (32767 << FMINI_LADDER_IN) : mix[i] < -(32767 << FMINI_LADDER_IN) ? -(32767 << FMINI_LADDER_IN) : mix[i]) >> FMINI_LADDER_IN, S, fbc, u, xin, v1, v2, v3, v4, y1, y2, y3, y4, w, o;   /* the mixer is linear (the CA-72's is) up to 12 dB over full scale, then clips hard: the shift is its headroom */
        S = (c0 * ((s0 + 128) >> 8) + c1 * ((s1 + 128) >> 8) + c2 * ((s2 + 128) >> 8) + c3 * ((s3 + 128) >> 8)) >> 14;   /* what the stages already hold */
        fbc = sx + fmini_mulr((S << 8) - sx, (uint32_t)gx16);              /* through the lag pole (Q8) */
        u = ((x * inv14) >> 14) - ((kinv12 * ((fbc + 128) >> 8)) >> 12);  /* the input less the feedback, the loop solved */
        xin = (pairb(u, pair_off)) << 8;                                   /* the first stage's input (Q8) */
        v1 = fmini_mulr(xin - s0, (uint32_t)G16);                          /* each stage is a differential pair: tanh in front of it */
        y1 = v1 + s0;
        s0 = y1 + v1;
        v2 = fmini_mulr((pairb((y1 + 128) >> 8, pair_off) << 8) - s1, (uint32_t)G16);
        y2 = v2 + s1;
        s1 = y2 + v2;
        v3 = fmini_mulr((pairb((y2 + 128) >> 8, pair_off) << 8) - s2, (uint32_t)G16);
        y3 = v3 + s2;
        s2 = y3 + v3;
        v4 = fmini_mulr((pairb((y3 + 128) >> 8, pair_off) << 8) - s3, (uint32_t)G16);
        y4 = v4 + s3;
        s3 = y4 + v4;
        w = fmini_mulr(y4 - sx, (uint32_t)gx16);                           /* the lag pole follows the output */
        sx += w + w;
        /* HP: the stages are one-pole lows H; (1 - H)^4 = 1 - 4H + 6H^2 - 4H^3 + H^4 on the stages' outputs (linear, the states as they are) */
        o = hp ? xin - 4 * y1 + 6 * y2 - 4 * y3 + y4 : y4;
        {   /* voice_amp with amp_at's ramp by its running sum (x = (amp1 - amp0) * i, one add a sample, mod 2^32 as the product) */
            int32_t xr = (int32_t)xa, amp = amp0 + ((xr + ((xr >> 31) & (CTL - 1))) >> CTL_LOG2);
            xa += da;
            out[i] += mulq15(mulq15((((o + 128) >> 8) * trim12) >> 12, amp), VOICE_FS) << (1 + FMINI_LADDER_IN);
        }
    }
    v->ph[0] = ph0;
    v->ph[1] = ph1;
    v->ph[2] = ph2;
    v->s[0] = s0;
    v->s[1] = s1;
    v->s[2] = s2;
    v->s[3] = s3;
    v->s[4] = sx;
}

/* the voice budget (voice.c voice_units): FMINI renders on CPU0 only (no paired CPU1 job), and a voice costs about
 * 0.8 of a Prophet voice on the host (tests/fmini_test.c, the TRACKS bench): it is charged as Prophet is on one core.
 * Provisional until a device measurement (tools/usb_audio_stats.py --parts) */
static uint32_t fmini_units(const track_t *t)
{
    (void)t;
    return 3u;
}

/* --------------------------------------------------------------- the engine --- */
static const engine_t ENG_FMINI = {
    .name = "FMINI",
    .page_title = {"FILTER", "PATCH"},
    .edit = {
        {"CUT", F_INT, 0, 127, 64, 0, 0},
        {"EMPH", F_INT, 0, 127, 40, 0, 0},
        {"CNTR", F_PCT, 0, 127, 64, 0, 0},
        {"F.ATK", F_TIME, 0, 127, 0, 0, 0},
        {"F.DEC", F_TIME, 0, 127, 64, 0, 0},
        {"F.SUS", F_PCT, 0, 127, 40, 0, 0},
        {"MOD", F_PCT, 0, 127, 0, 0, 0},
        {"SLOT", F_PATCH, FMINI_SLOT0, FMINI_SLOT0 + FMINI_NSLOT - 1, FMINI_SLOT0, 0, 0},
    },
    .presets = 0,
    .npresets = FMINI_NSLOT,
    .preset_make = fmini_preset_make,
    .note_on = fmini_note_on,
    .render = fmini_render,
    .knob = {P_E0, P_E1, P_E2, P_E4},
    .poly = FMINI_POLY,
    .keep = 0x13,
    .block = fmini_block,
    .units = fmini_units,
};
