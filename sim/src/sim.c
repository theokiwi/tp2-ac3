/* Simulador de previsao de acesso a memoria na migracao de VMs.
 * Le um trace binario (uint32 block ids), roda tres fases (antes, migracao,
 * depois) para UMA politica, e imprime uma linha CSV com as metricas.
 *
 * As politicas aging e chunkq so recebem bits de acesso por periodo de
 * amostragem (period_size acessos). LRU exato, OPT e CLOCK operam sobre o
 * fluxo de acessos bruto (nao sao limitadas pela amostragem periodica),
 * conforme a Metodologia: "as duas politicas aproximadas recebem exatamente
 * a mesma informacao (bits de acesso por periodo)".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include "sim.h"

static uint32_t *trace = NULL;
static uint64_t trace_len = 0;

static int load_trace(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen trace"); return -1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    trace_len = (uint64_t)sz / sizeof(uint32_t);
    trace = malloc(sz);
    if (!trace) { fprintf(stderr, "malloc trace falhou\n"); return -1; }
    size_t got = fread(trace, sizeof(uint32_t), trace_len, f);
    fclose(f);
    if (got != trace_len) { fprintf(stderr, "leitura incompleta do trace\n"); return -1; }
    return 0;
}

/* ---- estado comum ---- */
static uint32_t N;            /* vm_blocks */
static uint32_t C;            /* capacidade da RAM em blocos */
static uint8_t *in_ram;       /* N */
static uint64_t *lru_time;    /* N, oraculo LRU (sempre mantido) */
static uint64_t global_clock = 0;

/* ---- aging ---- */
static uint8_t *aging_hist;   /* N */
static uint8_t *period_touched; /* N, bitset reusado a cada periodo */

/* ---- chunk queues ---- */
static uint16_t *cq_queue;    /* N, 1..256 */

/* ---- clock policy ---- */
static int32_t *clk_frame_block; /* C: qual bloco ocupa o frame k (-1 livre) */
static uint8_t *clk_ref;         /* C */
static int32_t *clk_frame_of_block; /* N: em qual frame esta o bloco, -1 se fora da RAM */
static uint32_t clk_hand = 0;

/* ---- OPT ---- */
static uint64_t *opt_next;    /* N: proxima ocorrencia do bloco a partir da posicao atual (INF se nunca mais) */
#define INF_POS UINT64_MAX

static void apply_period_update_aging(void) {
    for (uint32_t b = 0; b < N; b++) {
        uint8_t touched = period_touched[b];
        aging_hist[b] = (uint8_t)((aging_hist[b] >> 1) | (touched ? 0x80 : 0));
        period_touched[b] = 0;
    }
}

static void apply_period_update_chunkq(void) {
    /* 1) promove blocos tocados no periodo: fila i -> fila min(i+128,256) */
    for (uint32_t b = 0; b < N; b++) {
        if (period_touched[b]) {
            uint32_t q = cq_queue[b] + 128;
            if (q > NUM_QUEUES) q = NUM_QUEUES;
            cq_queue[b] = (uint16_t)q;
            period_touched[b] = 0;
        }
    }
    /* 2) envelhecimento: filas 2i-1 e 2i viram a nova fila i (compressao) */
    for (uint32_t b = 0; b < N; b++) {
        cq_queue[b] = (uint16_t)((cq_queue[b] + 1) / 2);
    }
}

/* vitima entre blocos residentes na RAM, usada por aging e chunkq */
static uint32_t victim_by_min_aging(void) {
    uint32_t best = UINT32_MAX;
    int best_val = 256;
    for (uint32_t b = 0; b < N; b++) {
        if (!in_ram[b]) continue;
        if (aging_hist[b] < best_val) { best_val = aging_hist[b]; best = b; }
    }
    return best;
}

static uint32_t victim_by_min_chunkq(void) {
    uint32_t best = UINT32_MAX;
    int best_val = NUM_QUEUES + 1;
    for (uint32_t b = 0; b < N; b++) {
        if (!in_ram[b]) continue;
        if (cq_queue[b] < best_val) { best_val = cq_queue[b]; best = b; }
    }
    return best;
}

static uint32_t victim_by_min_lru(void) {
    uint32_t best = UINT32_MAX;
    uint64_t best_val = UINT64_MAX;
    for (uint32_t b = 0; b < N; b++) {
        if (!in_ram[b]) continue;
        if (lru_time[b] < best_val) { best_val = lru_time[b]; best = b; }
    }
    return best;
}

static uint32_t victim_by_max_opt_dist(uint32_t exclude) {
    uint32_t best = UINT32_MAX;
    uint64_t best_val = 0; /* queremos o MAIOR next_occurrence (mais distante) */
    int best_is_inf = 0;
    for (uint32_t b = 0; b < N; b++) {
        if (!in_ram[b] || b == exclude) continue;
        if (opt_next[b] == INF_POS) { return b; } /* nunca mais usado: vitima imediata */
        if (!best_is_inf && opt_next[b] >= best_val) { best_val = opt_next[b]; best = b; }
    }
    return best;
}

/* ---- comparador para ranking inicial (split da migracao) ---- */
typedef struct { uint32_t block; double score; } rank_t;
static int cmp_rank_desc(const void *a, const void *b) {
    double da = ((const rank_t *)a)->score, db = ((const rank_t *)b)->score;
    if (da < db) return 1;
    if (da > db) return -1;
    return (int)(((const rank_t *)a)->block - ((const rank_t *)b)->block);
}

int main(int argc, char **argv) {
    if (argc < 8) {
        fprintf(stderr,
            "uso: %s <trace> <vm_blocks> <ram_fraction> <policy> <period_size> <pre_periods> <post_accesses> [meta_prefix]\n",
            argv[0]);
        return 1;
    }
    const char *trace_path = argv[1];
    N = (uint32_t)strtoul(argv[2], NULL, 10);
    double ram_fraction = atof(argv[3]);
    const char *policy_name = argv[4];
    uint64_t period_size = strtoull(argv[5], NULL, 10);
    uint64_t pre_periods = strtoull(argv[6], NULL, 10);
    uint64_t post_accesses = strtoull(argv[7], NULL, 10);
    const char *meta_prefix = (argc > 8) ? argv[8] : "";

    policy_id_t policy;
    if (!strcmp(policy_name, "opt")) policy = POLICY_OPT;
    else if (!strcmp(policy_name, "lru")) policy = POLICY_LRU;
    else if (!strcmp(policy_name, "aging")) policy = POLICY_AGING;
    else if (!strcmp(policy_name, "chunkq")) policy = POLICY_CHUNKQ;
    else if (!strcmp(policy_name, "clock")) policy = POLICY_CLOCK;
    else { fprintf(stderr, "politica desconhecida: %s\n", policy_name); return 1; }

    if (load_trace(trace_path) != 0) return 1;

    uint64_t pre_accesses = pre_periods * period_size;
    if (pre_accesses + post_accesses > trace_len) {
        fprintf(stderr, "trace curto demais: precisa %llu, tem %llu\n",
                (unsigned long long)(pre_accesses + post_accesses),
                (unsigned long long)trace_len);
        return 1;
    }

    C = (uint32_t)(ram_fraction * N + 0.5);
    if (C < 1) C = 1;
    if (C >= N) C = N - 1;

    in_ram = calloc(N, 1);
    lru_time = calloc(N, sizeof(uint64_t));
    aging_hist = calloc(N, 1);
    period_touched = calloc(N, 1);
    cq_queue = malloc(N * sizeof(uint16_t));
    for (uint32_t b = 0; b < N; b++) cq_queue[b] = 1;
    opt_next = malloc(N * sizeof(uint64_t));

    clk_frame_block = malloc(C * sizeof(int32_t));
    clk_ref = calloc(C, 1);
    clk_frame_of_block = malloc(N * sizeof(int32_t));
    for (uint32_t b = 0; b < N; b++) clk_frame_of_block[b] = -1;
    for (uint32_t k = 0; k < C; k++) clk_frame_block[k] = -1;

    uint64_t *lru_oracle = calloc(N, sizeof(uint64_t)); /* independente da politica, sempre atualizado */

    /* ===== FASE 1: antes da migracao ===== */
    uint64_t pos = 0;
    if (policy == POLICY_AGING || policy == POLICY_CHUNKQ) {
        for (uint64_t p = 0; p < pre_periods; p++) {
            for (uint64_t i = 0; i < period_size; i++, pos++) {
                uint32_t b = trace[pos];
                period_touched[b] = 1;
                global_clock++;
                lru_oracle[b] = global_clock;
                lru_time[b] = global_clock;
            }
            if (policy == POLICY_AGING) apply_period_update_aging();
            else apply_period_update_chunkq();
        }
    } else {
        /* LRU, OPT, CLOCK: usam o fluxo bruto (recencia continua) */
        for (uint64_t i = 0; i < pre_accesses; i++, pos++) {
            uint32_t b = trace[pos];
            global_clock++;
            lru_oracle[b] = global_clock;
            lru_time[b] = global_clock;
        }
    }

    /* precomputa, para OPT: (a) opt_next[b] = primeira ocorrencia de b a
     * partir de 'pos' (usada no ranking da migracao), e (b) next_same[k] =
     * proxima ocorrencia do bloco em trace[pos+k], apos a posicao k, usada
     * para manter opt_next em dia em O(1) por acesso durante a fase 3
     * (evita re-varrer o trace a cada page-in). */
    uint64_t *next_same = NULL;
    uint64_t post_start = pos;
    if (policy == POLICY_OPT) {
        for (uint32_t b = 0; b < N; b++) opt_next[b] = INF_POS;
        next_same = malloc(post_accesses * sizeof(uint64_t));
        uint64_t *last_seen = malloc(N * sizeof(uint64_t));
        for (uint32_t b = 0; b < N; b++) last_seen[b] = INF_POS;
        for (uint64_t k = post_accesses; k > 0; k--) {
            uint64_t idx = k - 1;
            uint32_t b = trace[post_start + idx];
            next_same[idx] = last_seen[b];
            last_seen[b] = idx;
        }
        for (uint32_t b = 0; b < N; b++) opt_next[b] = last_seen[b];
        free(last_seen);
    }

    /* ===== rank para a migracao (quais blocos vao para a RAM) ===== */
    rank_t *rank = malloc(N * sizeof(rank_t));
    for (uint32_t b = 0; b < N; b++) {
        rank[b].block = b;
        switch (policy) {
            case POLICY_OPT:
                rank[b].score = (opt_next[b] == INF_POS) ? -1e18 : -(double)opt_next[b];
                break;
            case POLICY_LRU:
            case POLICY_CLOCK: /* CLOCK usa recencia (LRU) para o split inicial: e uma aproximacao de LRU */
                rank[b].score = (double)lru_time[b];
                break;
            case POLICY_AGING:
                rank[b].score = (double)aging_hist[b];
                break;
            case POLICY_CHUNKQ:
                rank[b].score = (double)cq_queue[b];
                break;
            default: rank[b].score = 0;
        }
    }
    qsort(rank, N, sizeof(rank_t), cmp_rank_desc);
    for (uint32_t i = 0; i < C; i++) in_ram[rank[i].block] = 1;
    free(rank);

    if (policy == POLICY_CLOCK) {
        uint32_t k = 0;
        for (uint32_t b = 0; b < N && k < C; b++) {
            if (in_ram[b]) {
                clk_frame_block[k] = (int32_t)b;
                clk_frame_of_block[b] = (int32_t)k;
                clk_ref[k] = 0;
                k++;
            }
        }
    }

    /* precisao da divisao: fraction dos primeiros K blocos distintos
     * acessados pos-migracao que ja estavam na RAM */
    uint32_t K = 1000;
    uint8_t *seen = calloc(N, 1);
    uint32_t distinct_seen = 0, distinct_hit = 0;
    for (uint64_t i = 0; i < post_accesses && distinct_seen < K; i++) {
        uint32_t b = trace[pos + i];
        if (!seen[b]) {
            seen[b] = 1;
            distinct_seen++;
            if (in_ram[b]) distinct_hit++;
        }
    }
    free(seen);
    double split_precision = distinct_seen ? (double)distinct_hit / distinct_seen : 0.0;

    /* ===== FASE 3: depois da migracao ===== */
    uint64_t page_ins = 0;
    uint64_t eviction_count = 0, lru_agree_count = 0;
    uint64_t resident_count = C; /* RAM comeca cheia apos a migracao */
    uint64_t period_accum = 0;
    clock_t decision_clock_total = 0;

    for (uint64_t i = 0; i < post_accesses; i++, pos++) {
        uint32_t b = trace[pos];
        global_clock++;
        lru_oracle[b] = global_clock;

        if (policy == POLICY_OPT) {
            /* avanca o ponteiro de proxima ocorrencia de b em O(1) */
            opt_next[b] = next_same[i];
        }

        if (policy == POLICY_AGING || policy == POLICY_CHUNKQ) {
            period_touched[b] = 1;
            period_accum++;
            if (period_accum == period_size) {
                if (policy == POLICY_AGING) apply_period_update_aging();
                else apply_period_update_chunkq();
                period_accum = 0;
            }
        } else if (policy != POLICY_OPT) {
            lru_time[b] = global_clock;
        }

        if (in_ram[b]) {
            if (policy == POLICY_CLOCK) {
                int32_t fr = clk_frame_of_block[b];
                if (fr >= 0) clk_ref[fr] = 1;
            }
            continue; /* hit, sem page-in */
        }

        /* miss: page-in */
        page_ins++;

        uint32_t victim = UINT32_MAX;
        clock_t t0 = clock();
        if (resident_count < C) {
            /* nunca deveria acontecer apos a migracao encher a RAM, mas
             * por seguranca: sem vitima necessaria */
            resident_count++;
        } else {
            switch (policy) {
                case POLICY_OPT:
                    victim = victim_by_max_opt_dist(b);
                    break;
                case POLICY_LRU:
                    victim = victim_by_min_lru();
                    break;
                case POLICY_AGING:
                    victim = victim_by_min_aging();
                    break;
                case POLICY_CHUNKQ:
                    victim = victim_by_min_chunkq();
                    break;
                case POLICY_CLOCK: {
                    while (1) {
                        int32_t occ = clk_frame_block[clk_hand];
                        if (clk_ref[clk_hand]) {
                            clk_ref[clk_hand] = 0;
                            clk_hand = (clk_hand + 1) % C;
                            continue;
                        }
                        victim = (uint32_t)occ;
                        break;
                    }
                    break;
                }
                default: break;
            }
        }
        clock_t t1 = clock();
        decision_clock_total += (t1 - t0);

        if (victim != UINT32_MAX) {
            /* concordancia com o oraculo LRU: quem o LRU exato teria escolhido
             * agora, dentre os residentes (usando lru_oracle, independente da
             * politica sob teste) */
            uint32_t oracle_victim = UINT32_MAX;
            uint64_t oracle_best = UINT64_MAX;
            for (uint32_t bb = 0; bb < N; bb++) {
                if (!in_ram[bb]) continue;
                if (lru_oracle[bb] < oracle_best) { oracle_best = lru_oracle[bb]; oracle_victim = bb; }
            }
            eviction_count++;
            if (oracle_victim == victim) lru_agree_count++;

            in_ram[victim] = 0;
            if (policy == POLICY_CLOCK) {
                int32_t fr = clk_frame_of_block[victim];
                clk_frame_of_block[victim] = -1;
                clk_frame_block[clk_hand] = (int32_t)b;
                clk_frame_of_block[b] = (int32_t)clk_hand;
                clk_ref[clk_hand] = 1;
                clk_hand = (clk_hand + 1) % C;
                (void)fr;
            }
        }
        in_ram[b] = 1;
        if (policy != POLICY_CLOCK) {
            /* clock ja posicionou o frame acima */
        }
        if (policy != POLICY_AGING && policy != POLICY_CHUNKQ && policy != POLICY_OPT) {
            lru_time[b] = global_clock;
        }

    }

    double decision_time_ms = 1000.0 * (double)decision_clock_total / CLOCKS_PER_SEC;
    double lru_agreement = eviction_count ? (double)lru_agree_count / eviction_count : -1.0;

    printf("%s%s,%" PRIu64 ",%.6f,%.6f,%.3f,%" PRIu64 ",%" PRIu64 "\n",
           meta_prefix, policy_name,
           (uint64_t)page_ins, split_precision, lru_agreement, decision_time_ms,
           (uint64_t)post_accesses, (uint64_t)eviction_count);

    return 0;
}
