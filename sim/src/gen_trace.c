/* Synthetic trace generator: emits a binary stream of uint32_t block ids
 * (native-endian), one per access. Deterministic given (workload, vm_blocks,
 * total_accesses, seed, params). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "sim.h"

/* xorshift64* PRNG, deterministic and seedable */
static uint64_t rng_state;
static void rng_seed(uint64_t s) { rng_state = s ? s : 0x9E3779B97F4A7C15ULL; }
static uint64_t rng_next(void) {
    uint64_t x = rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}
static double rng_uniform(void) { /* [0,1) */
    return (double)(rng_next() >> 11) * (1.0 / 9007199254740992.0);
}
static uint32_t rng_below(uint32_t n) {
    return (uint32_t)(rng_uniform() * (double)n);
}

/* Zipf sampler via inverse-CDF on precomputed cumulative weights (ranks 1..n).
 * Rank 0 (most popular) gets block id 0 after a random permutation seed
 * so "popular" isn't tied to block index ordering artifacts. */
static double *zipf_cdf = NULL;
static uint32_t zipf_n = 0;
static void zipf_init(uint32_t n, double alpha) {
    zipf_n = n;
    zipf_cdf = malloc(sizeof(double) * n);
    double sum = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        sum += 1.0 / pow((double)(i + 1), alpha);
        zipf_cdf[i] = sum;
    }
    for (uint32_t i = 0; i < n; i++) zipf_cdf[i] /= sum;
}
static uint32_t zipf_sample(void) {
    double u = rng_uniform();
    uint32_t lo = 0, hi = zipf_n - 1;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (zipf_cdf[mid] < u) lo = mid + 1; else hi = mid;
    }
    return lo;
}

int main(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr,
            "uso: %s <workload> <vm_blocks> <total_accesses> <seed> <out_path> [param]\n"
            "workloads: sequential uniform zipf:<alpha> phasechange bursty\n",
            argv[0]);
        return 1;
    }
    const char *workload = argv[1];
    uint32_t vm_blocks = (uint32_t)strtoul(argv[2], NULL, 10);
    uint64_t total_accesses = strtoull(argv[3], NULL, 10);
    uint64_t seed = strtoull(argv[4], NULL, 10);
    const char *out_path = argv[5];

    rng_seed(seed);

    FILE *out = fopen(out_path, "wb");
    if (!out) { perror("fopen"); return 1; }

    uint32_t *buf = malloc(sizeof(uint32_t) * 65536);
    uint64_t written = 0;

    if (strcmp(workload, "sequential") == 0) {
        uint32_t cur = 0;
        while (written < total_accesses) {
            uint64_t chunk = total_accesses - written;
            if (chunk > 65536) chunk = 65536;
            for (uint64_t i = 0; i < chunk; i++) {
                buf[i] = cur;
                cur = (cur + 1) % vm_blocks;
            }
            fwrite(buf, sizeof(uint32_t), chunk, out);
            written += chunk;
        }
    } else if (strcmp(workload, "uniform") == 0) {
        while (written < total_accesses) {
            uint64_t chunk = total_accesses - written;
            if (chunk > 65536) chunk = 65536;
            for (uint64_t i = 0; i < chunk; i++) buf[i] = rng_below(vm_blocks);
            fwrite(buf, sizeof(uint32_t), chunk, out);
            written += chunk;
        }
    } else if (strncmp(workload, "zipf:", 5) == 0) {
        double alpha = atof(workload + 5);
        zipf_init(vm_blocks, alpha);
        /* random permutation mapping rank -> block id, so popularity isn't
         * tied to block index order (avoids accidental correlation with
         * sequential-style policies). Fisher-Yates. */
        uint32_t *perm = malloc(sizeof(uint32_t) * vm_blocks);
        for (uint32_t i = 0; i < vm_blocks; i++) perm[i] = i;
        for (uint32_t i = vm_blocks - 1; i > 0; i--) {
            uint32_t j = rng_below(i + 1);
            uint32_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
        }
        while (written < total_accesses) {
            uint64_t chunk = total_accesses - written;
            if (chunk > 65536) chunk = 65536;
            for (uint64_t i = 0; i < chunk; i++) buf[i] = perm[zipf_sample()];
            fwrite(buf, sizeof(uint32_t), chunk, out);
            written += chunk;
        }
        free(perm);
    } else if (strcmp(workload, "phasechange") == 0) {
        /* working set shifts every phase_len accesses to a different
         * contiguous window of width phase_width (10% of VM), covering
         * the VM in disjoint successive windows (wrapping). */
        uint64_t phase_len = total_accesses / 10; /* 10 phases across the run */
        if (phase_len == 0) phase_len = 1;
        uint32_t phase_width = vm_blocks / 10;
        if (phase_width == 0) phase_width = 1;
        uint64_t idx = 0;
        while (written < total_accesses) {
            uint64_t chunk = total_accesses - written;
            if (chunk > 65536) chunk = 65536;
            for (uint64_t i = 0; i < chunk; i++) {
                uint64_t phase = idx / phase_len;
                uint32_t base = (uint32_t)((phase * phase_width) % vm_blocks);
                buf[i] = (base + rng_below(phase_width)) % vm_blocks;
                idx++;
            }
            fwrite(buf, sizeof(uint32_t), chunk, out);
            written += chunk;
        }
    } else if (strcmp(workload, "bursty") == 0) {
        /* alternates: a burst touching many distinct random blocks once
         * each, then an idle stretch re-touching a small hot set. */
        uint32_t burst_size = vm_blocks / 20; /* 5% of VM per burst */
        if (burst_size == 0) burst_size = 1;
        uint32_t hot_size = vm_blocks / 200 ? vm_blocks / 200 : 1;
        uint32_t *hotset = malloc(sizeof(uint32_t) * hot_size);
        for (uint32_t i = 0; i < hot_size; i++) hotset[i] = rng_below(vm_blocks);
        int in_burst = 1;
        uint32_t remaining = burst_size;
        while (written < total_accesses) {
            uint64_t chunk = total_accesses - written;
            if (chunk > 65536) chunk = 65536;
            for (uint64_t i = 0; i < chunk; i++) {
                if (in_burst) {
                    buf[i] = rng_below(vm_blocks);
                    if (--remaining == 0) { in_burst = 0; remaining = burst_size * 4; }
                } else {
                    buf[i] = hotset[rng_below(hot_size)];
                    if (--remaining == 0) { in_burst = 1; remaining = burst_size; }
                }
            }
            fwrite(buf, sizeof(uint32_t), chunk, out);
            written += chunk;
        }
        free(hotset);
    } else {
        fprintf(stderr, "workload desconhecida: %s\n", workload);
        return 1;
    }

    free(buf);
    fclose(out);
    fprintf(stderr, "ok: %s %u blocos %llu acessos semente %llu -> %s\n",
            workload, vm_blocks, (unsigned long long)total_accesses,
            (unsigned long long)seed, out_path);
    return 0;
}
