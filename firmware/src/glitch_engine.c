/* SPDX-License-Identifier: GPL-3.0-only
- Per-hit glitch engine (pitch-ramped stutter rolls, ADPCM slice/reverse, jitter, crush)
- and IDM step/lock mutator */

static int tl_has_locks(uint32_t k, uint32_t s);
static void lock_clear_step(uint32_t k, uint32_t s, uint32_t id);
static void tl_lock_in(uint32_t k, uint32_t s, uint32_t id, int32_t v);
static int song_lock(void);
static void ui_message(const char *s);

typedef struct {
    uint8_t rush;         /* 0 off, 1..63 pitch-down stutter, 64..127 pitch-up stutter */
    uint8_t slice;        /* 0..127 ADPCM seek-table slice jump & reverse bounce */
    uint8_t jitter;       /* 0..127 random pitch, decay & velocity scatter */
    uint8_t crush;        /* 0..127 per-hit SRR & distortion burst */
    int16_t base_srr;
    int16_t base_dist;
    int16_t base_dec;
    int8_t  rush_slope;
    uint8_t rush_active;
    uint8_t mod_active;
} glitch_trk_t;

static struct {
    glitch_trk_t tr[NTRK];
    uint32_t     seed;
    uint8_t      ready;
} glitch;

static uint32_t glitch_rng(void)
{
    uint32_t x = glitch.seed;
    if (x == 0u)
        x = 0xA5A51337u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    glitch.seed = x;
    return x;
}

static void glitch_ensure_init(void)
{
    if (glitch.ready)
        return;
    glitch.seed = 0x5EEDBEEFu ^ fm1_ticks();
    if (glitch.seed == 0u)
        glitch.seed = 0x5EEDBEEFu;
    glitch.ready = 1u;
}

static void glitch_restore_track(track_t *t, glitch_trk_t *gt)
{
    if (!gt->mod_active)
        return;
    t->p[P_SRR]  = gt->base_srr;
    t->p[P_DIST] = gt->base_dist;
    t->p[P_E1]   = gt->base_dec;
    gt->mod_active = 0u;
}

/* Called in seq.c step_fire / acid_step right after rat_n is initialized */
static void glitch_on_step(track_t *t, const step_t *s)
{
    uint32_t k = (uint32_t)(t - trk);
    glitch_trk_t *gt;
    uint32_t mag, max_rat;
    int up;
    int8_t slope;

    if (k >= NTRK)
        return;
    glitch_ensure_init();
    gt = &glitch.tr[k];
    gt->rush_active = 0u;
    gt->rush_slope  = 0;

    if (gt->rush == 0u)
        return;

    up  = (gt->rush >= 64u);
    mag = up ? (uint32_t)(gt->rush - 63u) : (uint32_t)(64u - gt->rush); /* 1..64 */
    slope = (int8_t)(1u + (mag >> 4)); /* 1..5 semitones per sub-hit */
    if (!up)
        slope = (int8_t)(-slope);

    max_rat = 3u + (mag >> 4); /* 3..7 (up to 8x with existing ratchet) */
    if (max_rat > 8u)
        max_rat = 8u;

    if (t->rat_n > 1u) {
        uint32_t boosted = (uint32_t)t->rat_n + (mag >> 4);
        t->rat_n = (uint8_t)(boosted > 8u ? 8u : boosted);
        gt->rush_active = 1u;
        gt->rush_slope  = slope;
    } else if (s->acc || (glitch_rng() & 127u) < (mag * 2u)) {
        t->rat_n = (uint8_t)max_rat;
        gt->rush_active = 1u;
        gt->rush_slope  = slope;
    }
}

/* Called in seq.c rat_due before triggering each stutter sub-hit */
static void glitch_on_rat(track_t *t, int32_t *nt, uint8_t *vel)
{
    uint32_t k = (uint32_t)(t - trk);
    const glitch_trk_t *gt;
    if (k >= NTRK)
        return;
    gt = &glitch.tr[k];
    if (!gt->rush_active)
        return;
    *nt = clamp(*nt + (int32_t)t->rat_k * (int32_t)gt->rush_slope, NT_MIN, NT_MAX);
    *vel = (uint8_t)clamp((int32_t)*vel - (int32_t)t->rat_k * 5, 48, 127);
}

/* Called in drum_core.c drum_play before model trigger */
static void glitch_pre_trigger(track_t *t, int32_t *nt, uint32_t *vel)
{
    uint32_t k = (uint32_t)(t - trk);
    glitch_trk_t *gt;

    if (k >= NTRK)
        return;
    glitch_ensure_init();
    gt = &glitch.tr[k];
    glitch_restore_track(t, gt);

    if (gt->jitter == 0u && gt->crush == 0u && gt->slice == 0u)
        return;

    gt->base_srr  = t->p[P_SRR];
    gt->base_dist = t->p[P_DIST];
    gt->base_dec  = t->p[P_E1];

    if (gt->jitter > 0u) {
        int32_t j_semi = ((int32_t)(glitch_rng() % 25u) - 12) * (int32_t)gt->jitter / 127;
        int32_t j_dec  = ((int32_t)(glitch_rng() % 65u) - 32) * (int32_t)gt->jitter / 127;
        int32_t j_vel  = ((int32_t)(glitch_rng() % 49u) - 24) * (int32_t)gt->jitter / 127;
        *nt = clamp(*nt + j_semi, NT_MIN, NT_MAX);
        t->p[P_E1] = (int16_t)clamp((int32_t)gt->base_dec + j_dec, 0, 127);
        *vel = (uint32_t)clamp((int32_t)*vel + j_vel, 24, 127);
        gt->mod_active = 1u;
    }

    if (gt->crush > 0u) {
        if (t->rat_k > 0u || (glitch_rng() & 127u) <= gt->crush) {
            int32_t srr_add  = (int32_t)(glitch_rng() % ((uint32_t)gt->crush + 1u));
            int32_t dist_add = (int32_t)(glitch_rng() % ((uint32_t)gt->crush / 2u + 1u));
            t->p[P_SRR]  = (int16_t)clamp((int32_t)gt->base_srr + srr_add, 0, 127);
            t->p[P_DIST] = (int16_t)clamp((int32_t)gt->base_dist + dist_add, 0, 127);
            gt->mod_active = 1u;
        }
    }

    if (gt->slice > 0u && (uint32_t)t->p[P_MODEL] % NMODELS != DM_SMPL) {
        if ((glitch_rng() & 127u) < gt->slice) {
            int32_t s_dec = 12 + (int32_t)(glitch_rng() % 64u);
            t->p[P_E1] = (int16_t)s_dec;
            gt->mod_active = 1u;
        }
    }
}

/* Called in dm_sample.c smpl_trigger before smp_go to scramble ADPCM seek point & reverse loop */
static void glitch_smpl_mod(const track_t *t, uint32_t *st, uint32_t *en, uint32_t *lmode)
{
    uint32_t k = (uint32_t)(t - trk);
    const glitch_trk_t *gt;
    uint32_t span, nst;

    if (k >= NTRK)
        return;
    gt = &glitch.tr[k];
    if (gt->slice == 0u)
        return;

    if ((glitch_rng() & 127u) <= gt->slice) {
        uint32_t max_jump = 16u + ((uint32_t)gt->slice * 104u) / 127u; /* up to 120 */
        nst = glitch_rng() % (max_jump + 1u);
        span = 4u + (glitch_rng() % 24u);
        *st = nst;
        *en = (nst + span > 127u) ? 127u : (nst + span);
        if ((glitch_rng() & 127u) < gt->slice)
            *lmode = 2u; /* PING-PONG reverse ADPCM grain bounce */
    }
}

static void glitch_adjust(uint32_t k, int32_t d1, int32_t d2, int32_t d3, int32_t d4)
{
    glitch_trk_t *gt;
    if (k >= NTRK)
        return;
    glitch_ensure_init();
    gt = &glitch.tr[k];
    if (d1)
        gt->rush = (uint8_t)clamp((int32_t)gt->rush + d1, 0, 127);
    if (d2)
        gt->slice = (uint8_t)clamp((int32_t)gt->slice + d2, 0, 127);
    if (d3)
        gt->jitter = (uint8_t)clamp((int32_t)gt->jitter + d3, 0, 127);
    if (d4)
        gt->crush = (uint8_t)clamp((int32_t)gt->crush + d4, 0, 127);
}

static void glitch_fmt_summary(uint32_t k, char *b, uint32_t cap)
{
    const glitch_trk_t *gt = &glitch.tr[k];
    str_cpy(b, "GLT R", cap);
    fmt_int(b + str_len(b), (int32_t)gt->rush);
    str_cpy(b + str_len(b), " S", cap - str_len(b));
    fmt_int(b + str_len(b), (int32_t)gt->slice);
    str_cpy(b + str_len(b), " J", cap - str_len(b));
    fmt_int(b + str_len(b), (int32_t)gt->jitter);
    str_cpy(b + str_len(b), " C", cap - str_len(b));
    fmt_int(b + str_len(b), (int32_t)gt->crush);
}

static void glitch_mutate_track_internal(uint32_t k)
{
    glitch_trk_t *gt;
    uint32_t len, s;

    if (k >= NTRK)
        return;
    glitch_ensure_init();
    gt  = &glitch.tr[k];
    len = (uint32_t)clamp(trk[k].p[P_SLEN], 1, NSTEP);

    for (s = 0; s < len; s++) {
        step_t *st = &trk[k].step[s];
        if (!st->on)
            continue;
        if (tl_has_locks(k, s))
            lock_clear_step(k, s, 0);

        /* Inject ratchets (2x..4x) */
        if ((glitch_rng() % 100u) < 35u)
            st->rat = (uint8_t)(1u + (glitch_rng() % 3u));
        else
            st->rat = 0u;

        /* Inject microtiming rush/drag (-35..+35) */
        if ((glitch_rng() % 100u) < 40u)
            st->micro = (int8_t)((int32_t)(glitch_rng() % 71u) - 35);
        else
            st->micro = 0;

        /* Inject pitch scatter (-12..+12 semitones) */
        if ((glitch_rng() % 100u) < 30u)
            st->nt = (int8_t)clamp((int32_t)trk[k].key_nt + (int32_t)(glitch_rng() % 25u) - 12, NT_MIN, NT_MAX);
        else
            st->nt = trk[k].key_nt;

        /* Lock per-step SRR and distortion glitches */
        if ((glitch_rng() % 100u) < 35u) {
            tl_lock_in(k, s, P_SRR, (int32_t)(30u + (glitch_rng() % 85u)));
            if (glitch_rng() & 1u)
                tl_lock_in(k, s, P_DIST, (int32_t)(40u + (glitch_rng() % 80u)));
        }
    }

    if (gt->rush == 0u && gt->slice == 0u && gt->jitter == 0u && gt->crush == 0u) {
        gt->rush   = (uint8_t)(24u + (glitch_rng() % 80u));
        gt->slice  = (uint8_t)(30u + (glitch_rng() % 70u));
        gt->jitter = (uint8_t)(16u + (glitch_rng() % 48u));
        gt->crush  = (uint8_t)(25u + (glitch_rng() % 65u));
    }
}

static void glitch_mutate_selected(void)
{
    char m[24];
    if (song_lock())
        return;
    glitch.seed ^= fm1_ticks();
    glitch_mutate_track_internal(song.sel);
    glitch_fmt_summary(song.sel, m, sizeof m);
    ui_message(m);
}

static void glitch_mutate_all(void)
{
    uint32_t k;
    if (song_lock())
        return;
    glitch.seed ^= fm1_ticks();
    for (k = 0; k < NTRK; k++)
        glitch_mutate_track_internal(k);
    ui_message("GLITCH MUTATE ALL");
}

static void glitch_clear_selected(void)
{
    uint32_t k = song.sel, len, s;
    if (song_lock())
        return;
    glitch_ensure_init();
    glitch_restore_track(&trk[k], &glitch.tr[k]);
    memset(&glitch.tr[k], 0, sizeof glitch.tr[k]);
    len = (uint32_t)clamp(trk[k].p[P_SLEN], 1, NSTEP);
    for (s = 0; s < len; s++) {
        trk[k].step[s].rat   = 0u;
        trk[k].step[s].micro = 0;
        trk[k].step[s].nt    = trk[k].key_nt;
        if (tl_has_locks(k, s))
            lock_clear_step(k, s, 0);
    }
    ui_message("GLITCH CLEARED");
}
