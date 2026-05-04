#ifndef SBC_BENCH_METRICS_H
#define SBC_BENCH_METRICS_H

#include <stddef.h>
#include <stdint.h>

#include "sbc_bench_types.h"

/* Базовая статистика по массиву */
StatsSummary stats_from_array(const double *arr, size_t n);

/* Удобные утилиты */
double stats_avg(const double *arr, size_t n);
double stats_percentile(const double *arr, size_t n, double p);

/* Оценка outliers по IQR */
uint64_t stats_outliers_iqr(const double *arr, size_t n);

/* Стабильность и деградация */
double calc_stability_coeff(const double *arr, size_t n);  /* [0..1] */
double calc_degradation_percent(double start, double end); /* % */

/* Признаки троттлинга по частоте и температуре */
int detect_freq_drop_with_temp_rise(
    const double *freq_mhz,
    const double *temp_c,
    size_t n,
    double min_temp_rise_c,
    double min_freq_drop_pct);

#endif