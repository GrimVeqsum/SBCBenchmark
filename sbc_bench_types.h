#ifndef SBC_BENCH_TYPES_H
#define SBC_BENCH_TYPES_H

#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_ROWS 864000
#define MAX_STEPS 32
#define MAX_CPUS 256
#define MAX_WARNINGS 128
#define MAX_RUN_ERRORS 128
#define MAX_SAMPLES_PER_TEST 200000

typedef enum
{
  WK_IDLE = 0,
  WK_CPU_BURN = 1,
  WK_STORAGE = 2,
  WK_PING = 3,
  WK_NN = 4,
  WK_MEMORY = 5,
  WK_JITTER = 6
} WorkKind;

typedef enum
{
  NOISE_NONE = 0,
  NOISE_CPU = 1,
  NOISE_IO = 2,
  NOISE_COMBINED = 3
} NoiseMode;

typedef struct
{
  const char *name;
  WorkKind kind;
  int duration_sec;
  int threads;
  const char *arg;
  const char *load_profile;
  const char *purpose;
} Step;

typedef struct
{
  const char *name;
  const char *description;
  int sample_sec;
  double critical_temp_c;
  double target_ping_p99_ms;
  double ref_perf_per_watt;
  double assumed_power_w;
  NoiseMode noise_mode;

  const char *primary_metrics[16];
  int primary_metric_count;

  Step steps[MAX_STEPS];
  int step_count;
} Scenario;

typedef struct
{
  const char *key;
  const char *title;
  const char *description;
} ScenarioEntry;

typedef struct
{
  double ts;
  double temp_c;
  double cpu_freq_mhz;
  double cpu_util_pct;
  double mem_used_pct;
  double power_w;
  double psi_cpu_some_avg10;
  double psi_io_some_avg10;
  double psi_mem_some_avg10;
} Row;

typedef struct
{
  const Step *step;
  double ops_per_sec;
  double throughput_mb_s;
  double ping_p99_ms;
  double packet_loss_pct;
  double nn_inf_per_sec;

  /* memory */
  double mem_read_mb_s;
  double mem_write_mb_s;
  double mem_copy_mb_s;

  /* jitter */
  double jitter_avg_us;
  double jitter_p50_us;
  double jitter_p95_us;
  double jitter_p99_us;
  double jitter_max_us;
  uint64_t jitter_over_500us;
  uint64_t jitter_over_1000us;

  /* network extended */
  double ping_min_ms;
  double ping_avg_ms;
  double ping_max_ms;
  double ping_p95_ms;
  uint64_t ping_errors;

  /* storage extended */
  double storage_iops;
  double storage_lat_avg_us;
  double storage_lat_p50_us;
  double storage_lat_p95_us;
  double storage_lat_p99_us;
  double storage_lat_p999_us;
  double storage_lat_max_us;
  uint64_t storage_outliers;
} StepResult;

typedef struct
{
  Row *rows;
  size_t cap;
  size_t nrows;
  pthread_mutex_t lock;
  int sample_sec;
  volatile sig_atomic_t stop;
  char out_dir[PATH_MAX];
} Collector;

typedef struct
{
  char warnings[MAX_WARNINGS][256];
  int warning_count;

  char errors[MAX_RUN_ERRORS][256];
  int error_count;
} RunMessages;

typedef struct
{
  double avg;
  double median;
  double p95;
  double p99;
  double p999;
  double max;
} StatsSummary;

#endif