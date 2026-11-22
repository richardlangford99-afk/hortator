/* SPDX-License-Identifier: GPL-3.0-only
- Per-track Euclidean pattern generator & seeded odd-meter randomiser */

typedef struct {
    uint8_t steps;        /* 1..64 */
    uint8_t hits;         /* 0..steps */
    uint8_t rotation;     /* 0..(steps - 1) */
    uint8_t accent_hits;  /* 0..hits */
} euclid_trk_t;

static struct {
    euclid_trk_t tr[NTRK];
    uint32_t     seed;
    uint8_t      ready;
} euclid;

static const uint8_t EUCLID_ODD_LENS[] = {
    5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31, 35, 37, 41, 47, 53, 61
};
#define EUCLID_NUM_ODD (sizeof(EUCLID_ODD_LENS) / sizeof(EUCLID_ODD_LENS[0]))

static uint32_t euclid_rng(void)
{
    uint32_t x = euclid.seed;
    if (x == 0u)
        x = 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    euclid.seed = x;
    return x;
}

static uint8_t euclid_rand_range(uint8_t min_v, uint8_t max_v)
{
    if (max_v <= min_v)
        return min_v;
    return (uint8_t)(min_v + (euclid_rng() % (uint32_t)(max_v - min_v + 1u)));
}

static void euclid_ensure_init(void)
{
    uint32_t k;
    if (euclid.ready)
        return;
    euclid.seed = 0x1337BEEFu ^ fm1_ticks();
    if (euclid.seed == 0u)
        euclid.seed = 0x1337BEEFu;
    for (k = 0; k < NTRK; k++) {
        uint8_t len = (uint8_t)clamp(trk[k].p[P_SLEN], 1, NSTEP);
        euclid.tr[k].steps       = len;
        euclid.tr[k].hits        = (k == 0u && len >= 4u) ? 4u : 0u;
        euclid.tr[k].rotation    = 0u;
        euclid.tr[k].accent_hits = (k == 0u && len >= 4u) ? 1u : 0u;
    }
    euclid.ready = 1u;
}

static void euclid_sync_len(uint32_t k)
{
    uint8_t len;
    euclid_ensure_init();
    len = (uint8_t)clamp(trk[k].p[P_SLEN], 1, NSTEP);
    if (euclid.tr[k].steps != len) {
        euclid.tr[k].steps = len;
        if (euclid.tr[k].hits > len)
            euclid.tr[k].hits = len;
        if (euclid.tr[k].rotation >= len)
            euclid.tr[k].rotation = 0u;
        if (euclid.tr[k].accent_hits > euclid.tr[k].hits)
            euclid.tr[k].accent_hits = euclid.tr[k].hits;
    }
}

/* Write the Euclidean pattern directly into track k's native 64-step sequencer */
static void euclid_apply_track(uint32_t k)
{
    euclid_trk_t *et;
    uint8_t trig[NSTEP], acc[NSTEP];
    uint32_t s, steps, hits, rot, ahits;
    int32_t bucket;

    if (k >= NTRK || song_lock())
        return;
    euclid_ensure_init();
    et = &euclid.tr[k];

    if (et->steps < 1u)
        et->steps = 1u;
    if (et->steps > NSTEP)
        et->steps = NSTEP;
    if (et->hits > et->steps)
        et->hits = et->steps;
    if (et->rotation >= et->steps)
        et->rotation = (uint8_t)(et->rotation % et->steps);
    if (et->accent_hits > et->hits)
        et->accent_hits = et->hits;

    steps = et->steps;
    hits  = et->hits;
    rot   = et->rotation;
    ahits = et->accent_hits;

    for (s = 0; s < NSTEP; s++) {
        trig[s] = 0u;
        acc[s]  = 0u;
    }

    if (hits > 0u) {
        bucket = (int32_t)(steps - hits);
        for (s = 0; s < steps; s++) {
            bucket += (int32_t)hits;
            if (bucket >= (int32_t)steps) {
                bucket -= (int32_t)steps;
                trig[(s + rot) % steps] = 1u;
            }
        }
    }

    if (ahits > 0u && hits > 0u) {
        bucket = (int32_t)(hits - ahits);
        for (s = 0; s < steps; s++) {
            uint32_t pos = (s + rot) % steps;
            if (trig[pos]) {
                bucket += (int32_t)ahits;
                if (bucket >= (int32_t)hits) {
                    bucket -= (int32_t)hits;
                    acc[pos] = 1u;
                }
            }
        }
    }

    for (s = 0; s < NSTEP; s++) {
        step_t st;
        if (tl_has_locks(k, s))
            lock_clear_step(k, s, 0);
        st.on    = (s < steps && trig[s]) ? 1u : 0u;
        st.acc   = (s < steps && acc[s]) ? 1u : 0u;
        st.slide = 0u;
        st.tie   = 0u;
        st.rat   = 0u;
        st.cond  = 0u;
        st.nt    = trk[k].key_nt;
        st.rsv   = 0u;
        trk[k].step[s] = st;
    }

    trk[k].p[P_SLEN] = (int16_t)steps;
    trk[k].p[P_SRC]  = 0;
}

static void euclid_adjust(uint32_t k, int32_t d_steps, int32_t d_hits, int32_t d_rot, int32_t d_acc)
{
    euclid_trk_t *et;
    if (k >= NTRK)
        return;
    euclid_sync_len(k);
    et = &euclid.tr[k];
    if (d_steps)
        et->steps = (uint8_t)clamp((int32_t)et->steps + d_steps, 1, NSTEP);
    if (d_hits)
        et->hits = (uint8_t)clamp((int32_t)et->hits + d_hits, 0, (int32_t)et->steps);
    if (d_rot) {
        int32_t st = (int32_t)et->steps;
        et->rotation = (uint8_t)(((int32_t)et->rotation + d_rot % st + st) % st);
    }
    if (d_acc)
        et->accent_hits = (uint8_t)clamp((int32_t)et->accent_hits + d_acc, 0, (int32_t)et->hits);
    euclid_apply_track(k);
}

static void euclid_fmt_summary(uint32_t k, char *b, uint32_t cap)
{
    const euclid_trk_t *et = &euclid.tr[k];
    uint32_t n;
    str_cpy(b, "EUC ", cap);
    n = str_len(b);
    fmt_int(b + n, (int32_t)et->steps);
    n = str_len(b);
    if (n + 1u < cap) { b[n] = '/'; b[n + 1u] = 0; }
    fmt_int(b + str_len(b), (int32_t)et->hits);
    str_cpy(b + str_len(b), " R", cap - str_len(b));
    fmt_int(b + str_len(b), (int32_t)et->rotation);
    str_cpy(b + str_len(b), " A", cap - str_len(b));
    fmt_int(b + str_len(b), (int32_t)et->accent_hits);
}

static void euclid_randomise_track_internal(uint32_t k)
{
    euclid_trk_t *et;
    uint8_t steps, min_pct, max_pct, pct, hits;

    euclid_ensure_init();
    et = &euclid.tr[k];

    if (k == 0u && (euclid_rng() % 100u) < 35u) {
        steps = (euclid_rng() & 1u) ? 16u : 32u;
    } else if ((euclid_rng() % 100u) < 80u) {
        steps = EUCLID_ODD_LENS[euclid_rng() % EUCLID_NUM_ODD];
    } else {
        steps = euclid_rand_range(3u, 64u);
    }

    if (k == 0u) {
        min_pct = 20u; max_pct = 42u;
    } else if (k <= 2u) {
        min_pct = 25u; max_pct = 50u;
    } else {
        min_pct = 35u; max_pct = 82u;
    }

    pct  = euclid_rand_range(min_pct, max_pct);
    hits = (uint8_t)(((uint32_t)steps * (uint32_t)pct) / 100u);
    if (hits == 0u)
        hits = 1u;
    if (hits >= steps)
        hits = (steps > 1u) ? (uint8_t)(steps - 1u) : 1u;

    et->steps       = steps;
    et->hits        = hits;
    et->rotation    = euclid_rand_range(0u, (uint8_t)(steps - 1u));
    et->accent_hits = (hits > 1u) ? euclid_rand_range(1u, (uint8_t)(hits / 2u + 1u)) : 0u;
    euclid_apply_track(k);
}

static void euclid_randomise_selected(void)
{
    char m[24];
    if (song_lock())
        return;
    euclid.seed ^= fm1_ticks();
    euclid_randomise_track_internal(song.sel);
    euclid_fmt_summary(song.sel, m, sizeof m);
    ui_message(m);
}

static void euclid_randomise_all(void)
{
    static const char HEX[] = "0123456789ABCDEF";
    char m[24];
    uint32_t k, s16;
    if (song_lock())
        return;
    euclid.seed ^= fm1_ticks();
    euclid_rng();
    s16 = euclid.seed & 0xFFFFu;
    for (k = 0; k < NTRK; k++)
        euclid_randomise_track_internal(k);
    str_cpy(m, "RND ALL #", sizeof m);
    m[9]  = HEX[(s16 >> 12) & 0xFu];
    m[10] = HEX[(s16 >> 8)  & 0xFu];
    m[11] = HEX[(s16 >> 4)  & 0xFu];
    m[12] = HEX[s16 & 0xFu];
    m[13] = 0;
    ui_message(m);
}
