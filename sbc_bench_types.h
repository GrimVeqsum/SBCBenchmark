#ifndef SBC_BENCH_TYPES_H
#define SBC_BENCH_TYPES_H

#include <pthread.h>
#include <signal.h>
#include <stddef.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_ROWS 864000
#define MAX_STEPS 16
#define MAX_CPUS 256

typedef enum
{
  WK_IDLE,
  WK_CPU_BURN,
  WK_STORAGE,
  WK_PING,
  WK_NN
} WorkKind;

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
  const char *primary_metrics[8];
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

#endif