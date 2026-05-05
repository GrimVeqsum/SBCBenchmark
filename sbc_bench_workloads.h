#ifndef SBC_BENCH_WORKLOADS_H
#define SBC_BENCH_WORKLOADS_H

#include "sbc_bench_types.h"

void workload_run_cpu_burn(const Step *s, volatile sig_atomic_t *g_stop,
                           double *ops_per_sec, double *window_start_ops, double *window_end_ops, double *degradation_pct);

double workload_run_nn_inference(const Step *s, volatile sig_atomic_t *g_stop);

size_t workload_parse_mem_bytes(const char *arg, int *was_clamped);

void workload_run_memory_test(const Step *s,
                              double *read_mb_s,
                              double *write_mb_s,
                              double *copy_mb_s,
                              volatile sig_atomic_t *g_stop,
                              int *was_clamped);

void workload_run_jitter_test(const Step *s,
                              double *avg_us,
                              double *p50_us,
                              double *p95_us,
                              double *p99_us,
                              double *max_us,
                              uint64_t *over_500us,
                              uint64_t *over_1000us,
                              volatile sig_atomic_t *g_stop);

#endif