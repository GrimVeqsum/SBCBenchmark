#ifndef SBC_BENCH_NETWORK_H
#define SBC_BENCH_NETWORK_H

#include "sbc_bench_types.h"

void network_run_ping(const Step *s,
                      double *loss_pct, double *p95, double *p99,
                      double *minv, double *avgv, double *maxv,
                      uint64_t *errors, const char *out_dir);

#endif