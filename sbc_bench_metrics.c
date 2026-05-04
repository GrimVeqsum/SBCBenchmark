#include "sbc_bench_metrics.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int cmp_double(const void *a, const void *b)
{
  const double da = *(const double *)a;
  const double db = *(const double *)b;
  return (da > db) - (da < db);
}

double stats_avg(const double *arr, size_t n)
{
  if (!arr || n == 0)
    return -1.0;
  double s = 0.0;
  for (size_t i = 0; i < n; ++i)
    s += arr[i];
  return s / (double)n;
}

double stats_percentile(const double *arr, size_t n, double p)
{
  if (!arr || n == 0)
    return -1.0;
  if (p < 0.0)
    p = 0.0;
  if (p > 100.0)
    p = 100.0;

  double *tmp = (double *)malloc(n * sizeof(double));
  if (!tmp)
    return -1.0;
  memcpy(tmp, arr, n * sizeof(double));
  qsort(tmp, n, sizeof(double), cmp_double);

  const double pos = ((double)(n - 1)) * p / 100.0;
  const size_t lo = (size_t)floor(pos);
  const size_t hi = (size_t)ceil(pos);

  double out;
  if (lo == hi)
  {
    out = tmp[lo];
  }
  else
  {
    const double w = pos - (double)lo;
    out = tmp[lo] * (1.0 - w) + tmp[hi] * w;
  }

  free(tmp);
  return out;
}

uint64_t stats_outliers_iqr(const double *arr, size_t n)
{
  if (!arr || n < 4)
    return 0;

  const double q1 = stats_percentile(arr, n, 25.0);
  const double q3 = stats_percentile(arr, n, 75.0);
  if (q1 < 0.0 || q3 < 0.0)
    return 0;

  const double iqr = q3 - q1;
  const double lo = q1 - 1.5 * iqr;
  const double hi = q3 + 1.5 * iqr;

  uint64_t cnt = 0;
  for (size_t i = 0; i < n; ++i)
  {
    if (arr[i] < lo || arr[i] > hi)
      cnt++;
  }
  return cnt;
}

StatsSummary stats_from_array(const double *arr, size_t n)
{
  StatsSummary s;
  s.avg = stats_avg(arr, n);
  s.median = stats_percentile(arr, n, 50.0);
  s.p95 = stats_percentile(arr, n, 95.0);
  s.p99 = stats_percentile(arr, n, 99.0);
  s.p999 = (n >= 1000) ? stats_percentile(arr, n, 99.9) : -1.0;
  s.max = stats_percentile(arr, n, 100.0);
  s.outliers = stats_outliers_iqr(arr, n);
  return s;
}

double calc_stability_coeff(const double *arr, size_t n)
{
  if (!arr || n < 2)
    return -1.0;

  size_t mid = n / 2;
  if (mid == n)
    return -1.0;

  const double first = arr[0];
  if (first <= 0.0)
    return -1.0;

  double second_avg = 0.0;
  size_t m = 0;
  for (size_t i = mid; i < n; ++i)
  {
    second_avg += arr[i];
    m++;
  }
  if (m == 0)
    return -1.0;
  second_avg /= (double)m;

  const double drift = fmax(0.0, (first - second_avg) / first);
  return fmax(0.0, fmin(1.0, 1.0 - drift));
}

double calc_degradation_percent(double start, double end)
{
  if (start <= 0.0)
    return -1.0;
  return fmax(0.0, (start - end) * 100.0 / start);
}

int detect_freq_drop_with_temp_rise(
    const double *freq_mhz,
    const double *temp_c,
    size_t n,
    double min_temp_rise_c,
    double min_freq_drop_pct)
{
  if (!freq_mhz || !temp_c || n < 2)
    return 0;

  const double f0 = freq_mhz[0];
  const double f1 = freq_mhz[n - 1];
  const double t0 = temp_c[0];
  const double t1 = temp_c[n - 1];

  if (f0 <= 0.0)
    return 0;

  const double temp_rise = t1 - t0;
  const double freq_drop_pct = (f0 - f1) * 100.0 / f0;

  return (temp_rise >= min_temp_rise_c && freq_drop_pct >= min_freq_drop_pct) ? 1 : 0;
}