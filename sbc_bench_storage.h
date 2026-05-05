#ifndef SBC_BENCH_STORAGE_H
#define SBC_BENCH_STORAGE_H

#include "sbc_bench_types.h"

double storage_run(const Step *s, const char *out_dir,
                   double *iops, double *lat_avg, double *lat_p50,
                   double *lat_p95, double *lat_p99, double *lat_p999,
                   double *lat_max, uint64_t *outliers);

#endif