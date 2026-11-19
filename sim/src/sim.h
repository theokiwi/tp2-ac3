#ifndef SIM_H
#define SIM_H

#include <stdint.h>
#include <stddef.h>

/* Trace is per-block (1 MB blocks = 256 x 4KB pages), not per-page: the
 * OR of the 256 page-level aging histories inside a block collapses to a
 * single per-block history bit set if any page in the block was touched
 * in the period (see Metodologia: "o trace e por bloco..."). */
typedef struct {
    uint32_t block;
} access_t;

typedef enum {
    POLICY_OPT = 0,
    POLICY_LRU,
    POLICY_AGING,
    POLICY_CHUNKQ,
    POLICY_CLOCK,
    POLICY_COUNT
} policy_id_t;

static const char *POLICY_NAMES[POLICY_COUNT] = {
    "opt", "lru", "aging", "chunkq", "clock"
};

typedef enum {
    WL_SEQUENTIAL = 0,
    WL_ZIPF,
    WL_UNIFORM,
    WL_PHASECHANGE,
    WL_BURSTY,
    WL_COUNT
} workload_id_t;

static const char *WORKLOAD_NAMES[WL_COUNT] = {
    "sequential", "zipf", "uniform", "phasechange", "bursty"
};

#define NUM_QUEUES 256
#define AGING_BITS 8

#endif
