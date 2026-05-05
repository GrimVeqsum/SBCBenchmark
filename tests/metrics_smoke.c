#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../sbc_bench_metrics.h"

static int approx(double a, double b, double eps)
{
  return fabs(a - b) <= eps;
}

int main(void)
{
  double a[] = {1, 2, 3, 4, 5};
  StatsSummary s = stats_from_array(a, 5);
  if (!approx(s.avg, 3.0, 1e-9))
    return 1;
  if (!approx(s.median, 3.0, 1e-9))
    return 2;
  if (!approx(s.p95, 4.8, 1e-9))
    return 3;
  if (!approx(s.max, 5.0, 1e-9))
    return 4;

  double d = calc_degradation_percent(100.0, 80.0);
  if (!approx(d, 20.0, 1e-9))
    return 5;

  double f[] = {2000, 1900, 1700};
  double t[] = {50, 58, 63};
  if (detect_freq_drop_with_temp_rise(f, t, 3, 10.0, 5.0) != 1)
    return 6;

  double stable_series[] = {100, 98, 97, 96, 95, 94};
  double st = calc_stability_coeff(stable_series, sizeof(stable_series) / sizeof(stable_series[0]));
  if (!approx(st, 0.95, 1e-9))
    return 7;

  double no_drop = calc_degradation_percent(80.0, 90.0);
  if (!approx(no_drop, 0.0, 1e-9))
    return 8;

  double *big = (double *)calloc(1000, sizeof(double));
  if (!big)
    return 9;
  for (int i = 0; i < 1000; ++i)
    big[i] = (double)(i + 1);
  StatsSummary sb = stats_from_array(big, 1000);
  free(big);
  if (!(sb.p999 > 998.0 && sb.p999 <= 1000.0))
    return 10;

  printf("metrics_smoke: OK\n");
  return 0;
}