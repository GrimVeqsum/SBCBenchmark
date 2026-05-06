#include "../sbc_bench_v4.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void)
{
  double arr[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  Stats s = stats_from_array(arr, 10);
  assert(fabs(s.avg - 5.5) < 1e-9);
  assert(fabs(s.median - 5.5) < 1e-9);
  assert(s.p95 >= 9.0 && s.p95 <= 10.0);
  assert(s.max == 10.0);

  double empty_arr[] = {0};
  Stats e = stats_from_array(empty_arr, 0);
  assert(e.avg < 0.0 && e.median < 0.0 && e.p95 < 0.0 && e.p99 < 0.0 && e.p999 < 0.0);

  double sample[] = {100.0, 98.0, 95.0, 90.0, 85.0};
  double degr = calc_degradation_percent(sample, 5);
  assert(degr > 10.0 && degr < 20.0);

  double stable[] = {100, 101, 99, 100, 100};
  double unstable[] = {100, 120, 80, 140, 60};
  double cs = calc_stability_coeff(stable, 5);
  double cu = calc_stability_coeff(unstable, 5);
  assert(cs > cu);

  double temp[] = {50, 52, 55, 60, 65};
  double freq[] = {1500, 1480, 1450, 1400, 1300};
  int hint = detect_freq_drop_with_temp_rise(freq, temp, 5);
  assert(hint == 1);

  double nohint_temp[] = {60, 58, 57, 56, 55};
  double nohint_freq[] = {1300, 1350, 1380, 1400, 1420};
  assert(detect_freq_drop_with_temp_rise(nohint_freq, nohint_temp, 5) == 0);

  double big[2000];
  for (int i = 0; i < 2000; ++i)
    big[i] = (double)i;
  Stats b = stats_from_array(big, 2000);
  assert(b.p999 >= 1997.0 && b.p999 <= 1999.0);

  printf("metrics_smoke: OK\n");
  return 0;
}