#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "sbc_bench_storage.h"
#include "sbc_bench_metrics.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _WIN32
#include <io.h>
#include <malloc.h>
#define fsync _commit
#define pread(fd, buf, count, offset) (_lseeki64((fd), (offset), SEEK_SET) < 0 ? -1 : read((fd), (buf), (count)))
#endif

typedef struct
{
  double *v;
  size_t n;
  size_t cap;
} SeriesLocal;

static void s_init(SeriesLocal *s, size_t cap)
{
  s->n = 0;
  s->cap = cap;
  s->v = (double *)calloc(cap ? cap : 1, sizeof(double));
}
static void s_push(SeriesLocal *s, double x)
{
  if (!s || !s->v || s->n >= s->cap)
    return;
  s->v[s->n++] = x;
}
static void s_free(SeriesLocal *s)
{
  free(s->v);
  s->v = NULL;
  s->n = s->cap = 0;
}

static double now_sec_local(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int join_path_local(char *dst, size_t dst_sz, const char *dir, const char *name)
{
  int n = snprintf(dst, dst_sz, "%s/%s", dir, name);
  return (n < 0 || (size_t)n >= dst_sz) ? -1 : 0;
}

static void detail_header(FILE *f) { fprintf(f, "op,lat_us\n"); }

double storage_run(const Step *s, const char *out_dir,
                   double *iops, double *lat_avg, double *lat_p50,
                   double *lat_p95, double *lat_p99, double *lat_p999,
                   double *lat_max, uint64_t *outliers)
{
  *iops = *lat_avg = *lat_p50 = *lat_p95 = *lat_p99 = *lat_p999 = *lat_max = -1.0;
  *outliers = 0;

  char path[PATH_MAX];
  if (join_path_local(path, sizeof(path), out_dir, "storage_test.bin") != 0)
    return -1.0;

  int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
  if (fd < 0)
    return -1.0;

  size_t block = 4 * 1024 * 1024;
  if (s->arg && strcmp(s->arg, "1M") == 0)
    block = 1024 * 1024;
  if (s->arg && strcmp(s->arg, "8M") == 0)
    block = 8 * 1024 * 1024;

  char *buf = NULL;
#ifdef _WIN32
  buf = (char *)_aligned_malloc(block, 4096);
  if (!buf)
#else
  if (posix_memalign((void **)&buf, 4096, block) != 0 || !buf)
#endif
  {
    close(fd);
    return -1.0;
  }
  memset(buf, 0x5A, block);

  char detail_csv[PATH_MAX];
  FILE *df = NULL;
  if (join_path_local(detail_csv, sizeof(detail_csv), out_dir, "storage_detail.csv") == 0)
  {
    df = fopen(detail_csv, "w");
    if (df)
      detail_header(df);
  }

  SeriesLocal lat;
  s_init(&lat, MAX_STAT_POINTS);

  uint64_t written = 0, read_bytes = 0, ops = 0;
  double t0 = now_sec_local();

  while (now_sec_local() - t0 < s->duration_sec)
  {
    double a = now_sec_local();
    ssize_t wr = write(fd, buf, block);
    if (wr < 0)
    {
      if (errno == EINTR)
        continue;
      break;
    }
    fsync(fd);
    double b = now_sec_local();

    double lat_us = (b - a) * 1e6;
    s_push(&lat, lat_us);
    if (df)
      fprintf(df, "write_fsync,%.3f\n", lat_us);
    written += (uint64_t)wr;
    ops++;

    if (lseek(fd, 0, SEEK_SET) >= 0)
    {
      double ra = now_sec_local();
      ssize_t rd = read(fd, buf, block);
      double rb = now_sec_local();
      if (rd > 0)
      {
        read_bytes += (uint64_t)rd;
        double l = (rb - ra) * 1e6;
        s_push(&lat, l);
        if (df)
          fprintf(df, "seq_read,%.3f\n", l);
        ops++;
      }
    }

    if (written > block)
    {
      off_t max_off = (off_t)(written - block);
      off_t off = (off_t)((uint64_t)rand() % (uint64_t)(max_off + 1));
      off = (off / 4096) * 4096;
      double rra = now_sec_local();
      ssize_t rrd = pread(fd, buf, block, off);
      double rrb = now_sec_local();
      if (rrd > 0)
      {
        read_bytes += (uint64_t)rrd;
        double l = (rrb - rra) * 1e6;
        s_push(&lat, l);
        if (df)
          fprintf(df, "random_read,%.3f\n", l);
        ops++;
      }
    }
  }

  for (int i = 0; i < 32; ++i)
  {
    char md[PATH_MAX];
    snprintf(md, sizeof(md), "%s/meta_%d.tmp", out_dir, i);

    double a = now_sec_local();
    int mfd = open(md, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (mfd >= 0)
    {
      ssize_t wrc = write(mfd, "x", 1);
      (void)wrc;
      close(mfd);
    }
    unlink(md);
    double b = now_sec_local();

    double l = (b - a) * 1e6;
    s_push(&lat, l);
    if (df)
      fprintf(df, "create_unlink,%.3f\n", l);
    ops++;
  }

  for (int i = 0; i < 16; ++i)
  {
    char dpath[PATH_MAX];
    snprintf(dpath, sizeof(dpath), "%s/bench_dir_%d", out_dir, i);
    double a = now_sec_local();
    int mk = mkdir(dpath
#ifndef _WIN32
                   ,
                   0755
#endif
    );
    int rm = (mk == 0) ? rmdir(dpath) : -1;
    double b = now_sec_local();
    if (mk == 0 && rm == 0)
    {
      double l = (b - a) * 1e6;
      s_push(&lat, l);
      if (df)
        fprintf(df, "mkdir_rmdir,%.3f\n", l);
      ops++;
    }
  }

  if (df)
    fclose(df);

  double elapsed = now_sec_local() - t0;

  if (lat.n > 0)
  {
    StatsSummary st = stats_from_array(lat.v, lat.n);
    *lat_avg = st.avg;
    *lat_p50 = st.median;
    *lat_p95 = st.p95;
    *lat_p99 = st.p99;
    *lat_p999 = st.p999;
    *lat_max = st.max;
    *outliers = st.outliers;
  }

  if (elapsed > 0)
    *iops = (double)ops / elapsed;

  s_free(&lat);
#ifdef _WIN32
  _aligned_free(buf);
#else
  free(buf);
#endif
  close(fd);
  unlink(path);

  if (elapsed <= 0)
    return -1.0;
  return ((double)(written + read_bytes) / (1024.0 * 1024.0)) / elapsed;
}