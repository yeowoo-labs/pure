/* Linear probing's probes per insert on benign key sets, at loads 1/2 (a key set's table) and 4/5 (a HASHED list's),
 * 200,000 keys: the slot before the secret (one round of MurmurHash3's finalizer, low bits: "old") against keyed
 * candidates, each the worst of five random odd multipliers K: ms, multiply-shift of the bits (the top bits of
 * bits * K); xs+ms, multiply-shift after the round's first xorshift; rot+ms after a rotation by 32; r25,50 after
 * xoring two rotations (a cheaper linear bijection); mum, the low bits of the 128-bit product's halves xored; fmix+ms,
 * multiply-shift after the whole round (what core/module-list.h's pure_key_slot is: the only one that is as random
 * hashing on every set). Each column is mean/most probes. cc -O2 probes.c -o probes && ./probes
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
static uint64_t K;
static size_t old_slot(uint64_t b, size_t mask) { b ^= b >> 33; b *= UINT64_C(0xff51afd7ed558ccd); b ^= b >> 33; return b & mask; }
static size_t ms_slot(uint64_t b, size_t mask) { return (size_t)((b * K) >> __builtin_clzll(mask | 1)) & mask; }
static size_t c_slot(uint64_t b, size_t mask) { b ^= b >> 33; return (size_t)((b * K) >> __builtin_clzll(mask | 1)) & mask; }
static size_t b_slot(uint64_t b, size_t mask) { b ^= b >> 33; b *= UINT64_C(0xff51afd7ed558ccd); b ^= b >> 33; return (size_t)((b * K) >> __builtin_clzll(mask | 1)) & mask; }
static size_t rot_slot(uint64_t b, size_t mask) { b = b << 32 | b >> 32; return (size_t)((b * K) >> __builtin_clzll(mask | 1)) & mask; }
static size_t r2_slot(uint64_t b, size_t mask) { b ^= (b << 25 | b >> 39) ^ (b << 50 | b >> 14); return (size_t)((b * K) >> __builtin_clzll(mask | 1)) & mask; }
static size_t mum_slot(uint64_t b, size_t mask) { unsigned __int128 p = (unsigned __int128)b * K; return (size_t)((uint64_t)(p >> 64) ^ (uint64_t)p) & mask; }
static uint64_t dbits(double d) { uint64_t b; memcpy(&b, &d, 8); return b; }
static void run(const char *name, const uint64_t *keys, size_t n, double load, size_t (*slot)(uint64_t, size_t), double *avg, size_t *max) {
    size_t cap = 16; while (cap * load < n) cap *= 2;
    uint64_t *t = malloc(cap * 8); for (size_t i = 0; i < cap; ++i) t[i] = UINT64_MAX;
    size_t total = 0, most = 0, mask = cap - 1;
    for (size_t i = 0; i < n; ++i) {
        size_t at = slot(keys[i], mask), p = 1;
        while (t[at] != UINT64_MAX) at = (at + 1) & mask, ++p;
        t[at] = keys[i]; total += p; if (p > most) most = p;
    }
    free(t); *avg = (double)total / n; *max = most; (void)name;
}
int main(void) {
    enum { N = 200000 };
    static uint64_t sets[6][N];
    const char *names[6] = {"seq ints as doubles", "stride 2^20 ints as doubles", "MINSTD ids as doubles", "raw i64 0..n", "raw i64 stride 2^32", "random doubles"};
    uint64_t x = 1;
    for (size_t i = 0; i < N; ++i) {
        sets[0][i] = dbits((double)i);
        sets[1][i] = dbits((double)i * 1048576.0);
        x = x * 48271 % 2147483647; sets[2][i] = dbits((double)x);
        sets[3][i] = i;
        sets[4][i] = (uint64_t)i << 32;
        uint64_t r; getrandom(&r, 8, 0); sets[5][i] = dbits((double)(r >> 11) / 9007199254740992.0 * 1e6);
    }
    for (int s = 0; s < 6; ++s) for (int l = 0; l < 2; ++l) {
        double load = l ? 0.8 : 0.5, a; size_t m;
        run(names[s], sets[s], N, load, old_slot, &a, &m);
        printf("%-28s load %.1f  old %5.2f/%-4zu", names[s], load, a, m);
        size_t (*fs[6])(uint64_t, size_t) = {ms_slot, c_slot, rot_slot, r2_slot, mum_slot, b_slot};
        const char *fn[6] = {"ms", "xs+ms", "rot+ms", "r25,50", "mum", "fmix+ms"};
        for (int f = 0; f < 6; ++f) {
            double wa = 0; size_t wm = 0;
            for (int k = 0; k < 5; ++k) { getrandom(&K, 8, 0); K |= 1; run(names[s], sets[s], N, load, fs[f], &a, &m); if (a > wa) wa = a; if (m > wm) wm = m; }
            printf("  %s %5.2f/%-4zu", fn[f], wa, wm);
        }
        printf("\n");
    }
}
