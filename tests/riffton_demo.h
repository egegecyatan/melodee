/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 riffton contributors */
/* A two-bar riff in A minor for the riffton tests and renders (tests/ui_render.c, tests/riffton_test.c): BASS, LEAD,
 * PAD (three-note chords) and ARP on tracks 1..4, the roles' power-on sounds (engines.c TRK_DEF), LEAD selected,
 * playing at step 12 of 32, 124 BPM. */
static void rt_demo_layer(uint32_t k, const uint8_t *n, const uint8_t *len, uint32_t cnt, uint32_t third)
{
    track_t *t = &trk[k];
    uint32_t i, at, d;
    for (i = 0; i < NSTEP; i++) {
        memset(&t->step[i], 0, sizeof t->step[i]);
        t->step[i].time = ST_REST;
    }
    for (i = 0, at = 0; i < cnt && at < NSTEP; i++) {
        step_t *s = &t->step[at];
        s->time = ST_NOTE;
        s->n = third ? 3 : 1;
        s->note[0] = n[i];
        s->note[1] = (uint8_t)(n[i] + third);
        s->note[2] = (uint8_t)(n[i] + 7u);
        s->vel = 96;
        for (d = 1; d < len[i] && at + d < NSTEP; d++)
            t->step[at + d].time = ST_TIE;
        at += len[i];
    }
    t->p[P_SLEN] = 32;
}
static void rt_demo(void)
{
    static const uint8_t BN[] = {33, 33, 36, 33, 29, 29, 31, 28, 33, 36}, BLEN[] = {2, 2, 2, 2, 4, 4, 4, 4, 4, 4};
    static const uint8_t LN[] = {69, 72, 76, 74, 72, 77, 76, 72, 74, 71}, LLEN[] = {3, 1, 4, 2, 2, 4, 4, 4, 4, 4};
    static const uint8_t PN[] = {57, 53, 52, 55}, PLEN[] = {8, 8, 8, 8};
    static const uint8_t AN[] = {69, 72, 76, 72, 65, 69, 72, 69, 64, 67, 72, 67, 67, 71, 74, 71};
    static const uint8_t ALEN[] = {2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};
    uint32_t k;
    for (k = 0; k < NPART; k++) {
        host_preset(&trk[k], TRK_DEF_RT[k][0], TRK_DEF_RT[k][1]);
        trk[k].p[P_ROOT] = 9;                       /* A minor */
        trk[k].p[P_SCALE] = 2;
    }
    rt_demo_layer(0, BN, BLEN, NELEM(BN), 0);
    rt_demo_layer(1, LN, LLEN, NELEM(LN), 0);
    rt_demo_layer(2, PN, PLEN, NELEM(PN), 3);
    rt_demo_layer(3, AN, ALEN, NELEM(AN), 0);
    song.g[G_BPM] = 124;
    song.sel = 1;
    song.playing = 1;
    trk[0].seq_idx = 12;
}
