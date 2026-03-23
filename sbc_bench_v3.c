#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pthread.h>
#include <limits.h>
#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <signal.h>
#include <stdint.h>
#include <sys/utsname.h>
#include <sys/statvfs.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_CPU 256
#define MAX_THERMAL 64
#define MAX_HWMON 256
#define MAX_MMC 32
#define MAX_NAME_LEN 256
#define LINE_BUF 1024
#define MAX_TEST_THREADS 16

static volatile sig_atomic_t g_sigint = 0;

static void signal_handler(int sig) {
    (void)sig;
    g_sigint = 1;
}

typedef struct {
    int cpu;
    int thermal;
    int psi;
    int storage;
    int network;
    int cgroup;
    int realtime;
    int hwmon;
    int duration_minutes;
    char network_host[128];
    char scenario_name[64];
    int noise_cpu;
    int noise_cpu_threads;
    int noise_io;
} Config;

typedef struct {
    atomic_int stop;
    struct timespec start_ts;
    struct timespec end_ts;
    char out_dir[PATH_MAX];
    Config cfg;
} RunContext;

typedef struct {
    char path[PATH_MAX];
} CpuFreqPath;

typedef struct {
    char dir_name[64];
    char type[128];
    char temp_path[PATH_MAX];
} ThermalZone;

typedef struct {
    char label[128];
    char path[PATH_MAX];
} HwmonSensor;

typedef struct {
    double some_avg10;
    double some_avg60;
    double some_avg300;
    long long some_total;
    double full_avg10;
    double full_avg60;
    double full_avg300;
    long long full_total;
} PSIValues;

typedef struct {
    double *data;
    size_t size;
    size_t cap;
    double min;
    double max;
    double sum;
} StatsVec;

typedef struct {
    int ran;
    int sample_count;
    double peak_throughput;
    double sustained_avg_throughput;
    double peak_avg_freq_khz;
    double sustained_avg_freq_khz;
    double drift_percent;
    double time_to_throttle_sec;
    double time_to_first_5pct_drop_sec;
    double time_to_first_10pct_drop_sec;
    int workers;
} CpuResult;

typedef struct {
    int ran;
    int zones;
    long long critical_temp_mc;
    long long max_temp_mc;
    double avg_max_temp_mc;
    double time_to_critical_sec;
    double start_temp_c;
    double end_temp_c;
    double temp_rise_c_per_min;
} ThermalResult;

typedef struct {
    int ran;
    int has_cpu;
    int has_mem;
    int has_io;
    double cpu_some_avg10_max;
    double mem_some_avg10_max;
    double io_some_avg10_max;
    long long cpu_total_delta;
    long long mem_total_delta;
    long long io_total_delta;
} PSIResult;

typedef struct {
    int ran;
    int mmc_health_present;
    StatsVec create_us;
    StatsVec write_us;
    StatsVec fsync_us;
    StatsVec close_us;
    StatsVec unlink_us;
    StatsVec mkdir_us;
    StatsVec rmdir_us;
} StorageResult;

typedef struct {
    int ran;
    int ping_available;
    StatsVec rtt_ms;
} NetworkResult;

typedef struct {
    int ran;
    int cgroup_v2_available;
    int memory_current_available;
    long long usage_usec_delta;
    long long throttled_usec_delta;
    long long nr_throttled_delta;
    long long memory_current_last;
} CgroupResult;

typedef struct {
    int ran;
    StatsVec jitter_us;
    long long over_500us_count;
    long long over_1000us_count;
} RealtimeResult;

typedef struct {
    int ran;
    int sensor_count;
} HwmonResult;

typedef struct {
    pthread_t thread;
    atomic_ullong *ops;
    atomic_int *stop;
} CpuWorkerArgs;

typedef struct {
    atomic_int *stop;
} NoiseWorkerArgs;

typedef struct {
    RunContext *ctx;
    CpuResult *res;
} CpuThreadArgs;

typedef struct {
    RunContext *ctx;
    ThermalResult *res;
} ThermalThreadArgs;

typedef struct {
    RunContext *ctx;
    PSIResult *res;
} PSIThreadArgs;

typedef struct {
    RunContext *ctx;
    StorageResult *res;
} StorageThreadArgs;

typedef struct {
    RunContext *ctx;
    NetworkResult *res;
} NetworkThreadArgs;

typedef struct {
    RunContext *ctx;
    CgroupResult *res;
} CgroupThreadArgs;

typedef struct {
    RunContext *ctx;
    RealtimeResult *res;
} RealtimeThreadArgs;

typedef struct {
    RunContext *ctx;
    HwmonResult *res;
} HwmonThreadArgs;

static int should_stop(const RunContext *ctx) {
    long long now = 0;
    struct timespec ts;
    if (g_sigint) return 1;
    if (atomic_load(&ctx->stop)) return 1;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    return now >= ((long long)ctx->end_ts.tv_sec * 1000000000LL + ctx->end_ts.tv_nsec);
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == 0) return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) {
        *end = 0;
        end--;
    }
    return s;
}

static int is_numeric_str(const char *s) {
    if (!s || !*s) return 0;
    for (size_t i = 0; s[i]; i++) {
        if (!isdigit((unsigned char)s[i])) return 0;
    }
    return 1;
}

static long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static double elapsed_sec(const RunContext *ctx) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long cur = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    long long start = (long long)ctx->start_ts.tv_sec * 1000000000LL + ctx->start_ts.tv_nsec;
    return (double)(cur - start) / 1e9;
}

static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void iso_time(char *buf, size_t sz) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, sz, "%Y-%m-%dT%H:%M:%S", &tmv);
}

static void compact_time(char *buf, size_t sz) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, sz, "%Y%m%d_%H%M%S", &tmv);
}

static long long read_ll_file(const char *path, long long fallback) {
    FILE *f = fopen(path, "r");
    if (!f) return fallback;
    long long v = fallback;
    if (fscanf(f, "%lld", &v) != 1) v = fallback;
    fclose(f);
    return v;
}

static int read_line_trim(const char *path, char *out, size_t sz) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(out, (int)sz, f)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    char *t = trim(out);
    if (t != out) memmove(out, t, strlen(t) + 1);
    return 1;
}

static int ensure_dir_recursive(const char *path) {
    if (!path || !*path) return 0;
    if (is_dir(path)) return 1;
    char buf[PATH_MAX];
    size_t len = strlen(path);
    if (len >= sizeof(buf)) return 0;
    strcpy(buf, path);
    if (len > 1 && buf[len - 1] == '/') buf[len - 1] = 0;
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (!is_dir(buf) && mkdir(buf, 0755) != 0 && errno != EEXIST) return 0;
            *p = '/';
        }
    }
    if (!is_dir(buf) && mkdir(buf, 0755) != 0 && errno != EEXIST) return 0;
    return is_dir(buf);
}

static int path_join2(char *out, size_t out_sz, const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    int need_slash = (la > 0 && a[la - 1] != '/');
    if (la + (size_t)need_slash + lb + 1 > out_sz) return 0;
    memcpy(out, a, la);
    size_t pos = la;
    if (need_slash) out[pos++] = '/';
    memcpy(out + pos, b, lb);
    out[pos + lb] = 0;
    return 1;
}

static int path_join3(char *out, size_t out_sz, const char *a, const char *b, const char *c) {
    char tmp[PATH_MAX];
    if (!path_join2(tmp, sizeof(tmp), a, b)) return 0;
    return path_join2(out, out_sz, tmp, c);
}

static int command_exists(const char *cmd) {
    const char *path_env = getenv("PATH");
    if (!path_env) return 0;
    char *dup = malloc(strlen(path_env) + 1);
    if (!dup) return 0;
    strcpy(dup, path_env);
    int found = 0;
    char *save = NULL;
    char *part = strtok_r(dup, ":", &save);
    while (part) {
        char full[PATH_MAX];
        if (path_join2(full, sizeof(full), part, cmd) && access(full, X_OK) == 0) {
            found = 1;
            break;
        }
        part = strtok_r(NULL, ":", &save);
    }
    free(dup);
    return found;
}

static int list_dir_names(const char *path, char items[][MAX_NAME_LEN], int max_items) {
    DIR *dir = opendir(path);
    if (!dir) return 0;
    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(dir)) != NULL && count < max_items) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        snprintf(items[count], MAX_NAME_LEN, "%.*s", MAX_NAME_LEN - 1, entry->d_name);
        count++;
    }
    closedir(dir);
    return count;
}

static void stats_init(StatsVec *s) {
    s->data = NULL;
    s->size = 0;
    s->cap = 0;
    s->min = 0.0;
    s->max = 0.0;
    s->sum = 0.0;
}

static void stats_free(StatsVec *s) {
    free(s->data);
    s->data = NULL;
    s->size = 0;
    s->cap = 0;
    s->min = 0.0;
    s->max = 0.0;
    s->sum = 0.0;
}

static int stats_push(StatsVec *s, double value) {
    if (s->size == s->cap) {
        size_t new_cap = s->cap ? s->cap * 2 : 256;
        double *new_data = realloc(s->data, new_cap * sizeof(double));
        if (!new_data) return 0;
        s->data = new_data;
        s->cap = new_cap;
    }
    s->data[s->size++] = value;
    if (s->size == 1) {
        s->min = value;
        s->max = value;
    } else {
        if (value < s->min) s->min = value;
        if (value > s->max) s->max = value;
    }
    s->sum += value;
    return 1;
}

static int double_cmp(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

static double stats_avg(const StatsVec *s) {
    if (!s || s->size == 0) return 0.0;
    return s->sum / (double)s->size;
}

static double stats_percentile(const StatsVec *s, double p) {
    if (!s || s->size == 0) return 0.0;
    if (p <= 0.0) return s->min;
    if (p >= 100.0) return s->max;
    double *copy = malloc(s->size * sizeof(double));
    if (!copy) return 0.0;
    memcpy(copy, s->data, s->size * sizeof(double));
    qsort(copy, s->size, sizeof(double), double_cmp);
    double rank = (p / 100.0) * (double)(s->size - 1);
    size_t lo = (size_t)rank;
    size_t hi = lo + 1;
    double frac = rank - (double)lo;
    double result;
    if (hi >= s->size) result = copy[lo];
    else result = copy[lo] * (1.0 - frac) + copy[hi] * frac;
    free(copy);
    return result;
}

static double stats_avg_range(const StatsVec *s, size_t start, size_t end) {
    if (!s || s->size == 0) return 0.0;
    if (start >= s->size) return 0.0;
    if (end > s->size) end = s->size;
    if (start >= end) return 0.0;
    double sum = 0.0;
    size_t count = 0;
    for (size_t i = start; i < end; i++) {
        sum += s->data[i];
        count++;
    }
    return count ? sum / (double)count : 0.0;
}

static double stats_percentile_range(const StatsVec *s, double p, size_t start, size_t end) {
    if (!s || s->size == 0) return 0.0;
    if (start >= s->size) return 0.0;
    if (end > s->size) end = s->size;
    if (start >= end) return 0.0;
    size_t n = end - start;
    double *copy = malloc(n * sizeof(double));
    if (!copy) return 0.0;
    memcpy(copy, s->data + start, n * sizeof(double));
    qsort(copy, n, sizeof(double), double_cmp);
    if (p <= 0.0) {
        double result = copy[0];
        free(copy);
        return result;
    }
    if (p >= 100.0) {
        double result = copy[n - 1];
        free(copy);
        return result;
    }
    double rank = (p / 100.0) * (double)(n - 1);
    size_t lo = (size_t)rank;
    size_t hi = lo + 1;
    double frac = rank - (double)lo;
    double result;
    if (hi >= n) result = copy[lo];
    else result = copy[lo] * (1.0 - frac) + copy[hi] * frac;
    free(copy);
    return result;
}

static double calc_percent_change(double baseline, double current) {
    if (baseline == 0.0) return 0.0;
    return ((current - baseline) / baseline) * 100.0;
}


static void psi_values_init(PSIValues *v) {
    v->some_avg10 = -1.0;
    v->some_avg60 = -1.0;
    v->some_avg300 = -1.0;
    v->some_total = -1;
    v->full_avg10 = -1.0;
    v->full_avg60 = -1.0;
    v->full_avg300 = -1.0;
    v->full_total = -1;
}

static void parse_psi_line(const char *line, const char *kind, PSIValues *v) {
    if (strncmp(line, kind, strlen(kind)) != 0) return;
    char buf[LINE_BUF];
    snprintf(buf, sizeof(buf), "%s", line);
    char *save = NULL;
    char *tok = strtok_r(buf, " ", &save);
    while ((tok = strtok_r(NULL, " ", &save)) != NULL) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        const char *key = tok;
        const char *val = eq + 1;
        if (strcmp(kind, "some") == 0) {
            if (strcmp(key, "avg10") == 0) v->some_avg10 = atof(val);
            else if (strcmp(key, "avg60") == 0) v->some_avg60 = atof(val);
            else if (strcmp(key, "avg300") == 0) v->some_avg300 = atof(val);
            else if (strcmp(key, "total") == 0) v->some_total = atoll(val);
        } else {
            if (strcmp(key, "avg10") == 0) v->full_avg10 = atof(val);
            else if (strcmp(key, "avg60") == 0) v->full_avg60 = atof(val);
            else if (strcmp(key, "avg300") == 0) v->full_avg300 = atof(val);
            else if (strcmp(key, "total") == 0) v->full_total = atoll(val);
        }
    }
}

static PSIValues read_psi_file(const char *path) {
    PSIValues v;
    psi_values_init(&v);
    FILE *f = fopen(path, "r");
    if (!f) return v;
    char line[LINE_BUF];
    while (fgets(line, sizeof(line), f)) {
        parse_psi_line(line, "some", &v);
        parse_psi_line(line, "full", &v);
    }
    fclose(f);
    return v;
}

static int get_cpu_freq_paths(CpuFreqPath paths[], int max_paths) {
    char names[512][MAX_NAME_LEN];
    int n = list_dir_names("/sys/devices/system/cpu", names, 512);
    int count = 0;
    for (int i = 0; i < n && count < max_paths; i++) {
        if (strncmp(names[i], "cpu", 3) != 0) continue;
        if (!is_numeric_str(names[i] + 3)) continue;
        char path[PATH_MAX];
        if (!path_join3(path, sizeof(path), "/sys/devices/system/cpu", names[i], "cpufreq/scaling_cur_freq")) continue;
        if (file_exists(path)) {
            snprintf(paths[count].path, sizeof(paths[count].path), "%s", path);
            count++;
        }
    }
    return count;
}

static int read_cpu_freqs(CpuFreqPath paths[], int count, long long out[]) {
    for (int i = 0; i < count; i++) out[i] = read_ll_file(paths[i].path, -1);
    return count;
}

static int get_thermal_zones(ThermalZone zones[], int max_zones) {
    char names[256][MAX_NAME_LEN];
    int n = list_dir_names("/sys/class/thermal", names, 256);
    int count = 0;
    for (int i = 0; i < n && count < max_zones; i++) {
        if (strncmp(names[i], "thermal_zone", 12) != 0) continue;
        char base[PATH_MAX];
        if (!path_join2(base, sizeof(base), "/sys/class/thermal", names[i])) continue;
        char temp_path[PATH_MAX];
        if (!path_join2(temp_path, sizeof(temp_path), base, "temp")) continue;
        if (!file_exists(temp_path)) continue;
        snprintf(zones[count].dir_name, sizeof(zones[count].dir_name), "%s", names[i]);
        snprintf(zones[count].temp_path, sizeof(zones[count].temp_path), "%s", temp_path);
        char type_path[PATH_MAX];
        if (path_join2(type_path, sizeof(type_path), base, "type") && read_line_trim(type_path, zones[count].type, sizeof(zones[count].type))) {
        } else {
            snprintf(zones[count].type, sizeof(zones[count].type), "unknown");
        }
        count++;
    }
    return count;
}

static long long detect_critical_temp_mc(ThermalZone zones[], int zone_count) {
    long long best = -1;
    for (int z = 0; z < zone_count; z++) {
        char base[PATH_MAX];
        if (!path_join2(base, sizeof(base), "/sys/class/thermal", zones[z].dir_name)) continue;
        for (int i = 0; i < 16; i++) {
            char name[64];
            char tp_type[PATH_MAX];
            char tp_temp[PATH_MAX];
            snprintf(name, sizeof(name), "trip_point_%d_type", i);
            if (!path_join2(tp_type, sizeof(tp_type), base, name)) continue;
            snprintf(name, sizeof(name), "trip_point_%d_temp", i);
            if (!path_join2(tp_temp, sizeof(tp_temp), base, name)) continue;
            if (!file_exists(tp_type) || !file_exists(tp_temp)) continue;
            char type_buf[64];
            if (!read_line_trim(tp_type, type_buf, sizeof(type_buf))) continue;
            for (size_t k = 0; type_buf[k]; k++) type_buf[k] = (char)tolower((unsigned char)type_buf[k]);
            if (strstr(type_buf, "crit") || strstr(type_buf, "hot")) {
                long long temp = read_ll_file(tp_temp, -1);
                if (temp >= 0 && (best < 0 || temp < best)) best = temp;
            }
        }
    }
    return best;
}

static int get_hwmon_sensors(HwmonSensor sensors[], int max_sensors) {
    char hwmons[256][MAX_NAME_LEN];
    int hw_count = list_dir_names("/sys/class/hwmon", hwmons, 256);
    int count = 0;
    for (int h = 0; h < hw_count && count < max_sensors; h++) {
        char base[PATH_MAX];
        if (!path_join2(base, sizeof(base), "/sys/class/hwmon", hwmons[h])) continue;
        char chip_name[64];
        char name_path[PATH_MAX];
        if (!path_join2(name_path, sizeof(name_path), base, "name") || !read_line_trim(name_path, chip_name, sizeof(chip_name))) {
            snprintf(chip_name, sizeof(chip_name), "%s", hwmons[h]);
        }
        char files[512][MAX_NAME_LEN];
        int file_count = list_dir_names(base, files, 512);
        for (int i = 0; i < file_count && count < max_sensors; i++) {
            const char *file = files[i];
            if (!strstr(file, "_input")) continue;
            if (strncmp(file, "temp", 4) != 0 && strncmp(file, "in", 2) != 0 && strncmp(file, "curr", 4) != 0 && strncmp(file, "power", 5) != 0 && strncmp(file, "energy", 6) != 0) {
                continue;
            }
            char path[PATH_MAX];
            if (!path_join2(path, sizeof(path), base, file)) continue;
            if (!file_exists(path)) continue;
            char label[64];
            size_t len = strlen(file);
            if (len > 6) {
                char prefix[128];
                snprintf(prefix, sizeof(prefix), "%.*s", (int)(len - 6), file);
                char label_path[PATH_MAX];
                char label_name[140];
                snprintf(label_name, sizeof(label_name), "%s_label", prefix);
                if (path_join2(label_path, sizeof(label_path), base, label_name) && read_line_trim(label_path, label, sizeof(label))) {
                } else {
                    snprintf(label, sizeof(label), "%s", file);
                }
            } else {
                snprintf(label, sizeof(label), "%s", file);
            }
            snprintf(sensors[count].label, sizeof(sensors[count].label), "%s_%s", chip_name, label);
            snprintf(sensors[count].path, sizeof(sensors[count].path), "%s", path);
            count++;
        }
    }
    return count;
}

static int get_mmc_health_paths(char out[][PATH_MAX], int max_paths) {
    char names[256][MAX_NAME_LEN];
    int n = list_dir_names("/sys/block", names, 256);
    int count = 0;
    for (int i = 0; i < n && count < max_paths; i++) {
        if (strncmp(names[i], "mmcblk", 6) != 0) continue;
        char path[PATH_MAX];
        if (!path_join3(path, sizeof(path), "/sys/block", names[i], "device/life_time_est")) continue;
        if (file_exists(path)) {
            snprintf(out[count], PATH_MAX, "%s", path);
            count++;
        }
    }
    return count;
}

static long long read_memtotal_kb(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char key[64];
    long long value = -1;
    char unit[32];
    while (fscanf(f, "%63s %lld %31s", key, &value, unit) == 3) {
        if (strcmp(key, "MemTotal:") == 0) {
            fclose(f);
            return value;
        }
    }
    fclose(f);
    return -1;
}

static int read_cpu_model(char *out, size_t out_sz) {
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return 0;
    char line[LINE_BUF];
    while (fgets(line, sizeof(line), f)) {
        char *sep = strchr(line, ':');
        if (!sep) continue;
        *sep = 0;
        char *key = trim(line);
        char *val = trim(sep + 1);
        if (strcmp(key, "model name") == 0 || strcmp(key, "Hardware") == 0 || strcmp(key, "Processor") == 0) {
            snprintf(out, out_sz, "%.*s", (int)out_sz - 1, val);
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

static void read_root_mount(char *source, size_t source_sz, char *fstype, size_t fstype_sz) {
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) {
        snprintf(source, source_sz, "unknown");
        snprintf(fstype, fstype_sz, "unknown");
        return;
    }
    char dev[256], mountp[256], type[64], opts[256];
    int dumpv, passv;
    while (fscanf(f, "%255s %255s %63s %255s %d %d", dev, mountp, type, opts, &dumpv, &passv) == 6) {
        if (strcmp(mountp, "/") == 0) {
            snprintf(source, source_sz, "%.*s", (int)source_sz - 1, dev);
            snprintf(fstype, fstype_sz, "%.*s", (int)fstype_sz - 1, type);
            fclose(f);
            return;
        }
    }
    fclose(f);
    snprintf(source, source_sz, "unknown");
    snprintf(fstype, fstype_sz, "unknown");
}

static void write_system_info_json(const RunContext *ctx) {
    char path[PATH_MAX];
    if (!path_join2(path, sizeof(path), ctx->out_dir, "system_info.json")) return;
    FILE *f = fopen(path, "w");
    if (!f) return;

    struct utsname uts;
    memset(&uts, 0, sizeof(uts));
    uname(&uts);

    char cpu_model[256];
    if (!read_cpu_model(cpu_model, sizeof(cpu_model))) snprintf(cpu_model, sizeof(cpu_model), "unknown");

    char governor[64];
    if (!read_line_trim("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", governor, sizeof(governor))) snprintf(governor, sizeof(governor), "unknown");
    long long freq_min = read_ll_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq", -1);
    long long freq_max = read_ll_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq", -1);
    long long memtotal_kb = read_memtotal_kb();
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    char root_source[256], root_fstype[64];
    read_root_mount(root_source, sizeof(root_source), root_fstype, sizeof(root_fstype));
    struct statvfs vfs;
    unsigned long long root_bytes_total = 0ULL;
    if (statvfs("/", &vfs) == 0) {
        root_bytes_total = (unsigned long long)vfs.f_blocks * (unsigned long long)vfs.f_frsize;
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"sysname\": \"%s\",\n", uts.sysname);
    fprintf(f, "  \"nodename\": \"%s\",\n", uts.nodename);
    fprintf(f, "  \"release\": \"%s\",\n", uts.release);
    fprintf(f, "  \"version\": \"%s\",\n", uts.version);
    fprintf(f, "  \"machine\": \"%s\",\n", uts.machine);
    fprintf(f, "  \"cpu_model\": \"%s\",\n", cpu_model);
    fprintf(f, "  \"online_cores\": %ld,\n", cores);
    fprintf(f, "  \"mem_total_kb\": %lld,\n", memtotal_kb);
    fprintf(f, "  \"cpu0_governor\": \"%s\",\n", governor);
    fprintf(f, "  \"cpu0_scaling_min_freq_khz\": %lld,\n", freq_min);
    fprintf(f, "  \"cpu0_scaling_max_freq_khz\": %lld,\n", freq_max);
    fprintf(f, "  \"root_source\": \"%s\",\n", root_source);
    fprintf(f, "  \"root_fstype\": \"%s\",\n", root_fstype);
    fprintf(f, "  \"root_bytes_total\": %llu\n", root_bytes_total);
    fprintf(f, "}\n");
    fclose(f);
}

static double ping_once_ms(const char *host) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "ping -n -c 1 -W 1 %s 2>/dev/null", host);
    FILE *pipe = popen(cmd, "r");
    if (!pipe) return -1.0;
    char line[LINE_BUF];
    double result = -1.0;
    while (fgets(line, sizeof(line), pipe)) {
        char *p = strstr(line, "time=");
        int less = 0;
        if (!p) {
            p = strstr(line, "time<");
            if (p) less = 1;
        }
        if (p) {
            p += 5;
            result = atof(p);
            if (less && result == 0.0) result = 0.5;
            break;
        }
    }
    pclose(pipe);
    return result;
}

static void write_run_info_json(RunContext *ctx) {
    char path[PATH_MAX];
    if (!path_join2(path, sizeof(path), ctx->out_dir, "run_info.json")) return;
    FILE *f = fopen(path, "w");
    if (!f) return;

    CpuFreqPath cpu_paths[MAX_CPU];
    ThermalZone zones[MAX_THERMAL];
    HwmonSensor sensors[MAX_HWMON];
    char mmc[MAX_MMC][PATH_MAX];

    int cpu_count = get_cpu_freq_paths(cpu_paths, MAX_CPU);
    int zone_count = get_thermal_zones(zones, MAX_THERMAL);
    int sensor_count = get_hwmon_sensors(sensors, MAX_HWMON);
    int mmc_count = get_mmc_health_paths(mmc, MAX_MMC);

    char ts[64];
    iso_time(ts, sizeof(ts));
    fprintf(f, "{\n");
    fprintf(f, "  \"timestamp\": \"%s\",\n", ts);
    fprintf(f, "  \"output_dir\": \"%s\",\n", ctx->out_dir);
    fprintf(f, "  \"duration_minutes\": %d,\n", ctx->cfg.duration_minutes);
    fprintf(f, "  \"selected_tests\": {\n");
    fprintf(f, "    \"cpu\": %s,\n", ctx->cfg.cpu ? "true" : "false");
    fprintf(f, "    \"thermal\": %s,\n", ctx->cfg.thermal ? "true" : "false");
    fprintf(f, "    \"psi\": %s,\n", ctx->cfg.psi ? "true" : "false");
    fprintf(f, "    \"storage\": %s,\n", ctx->cfg.storage ? "true" : "false");
    fprintf(f, "    \"network\": %s,\n", ctx->cfg.network ? "true" : "false");
    fprintf(f, "    \"cgroup\": %s,\n", ctx->cfg.cgroup ? "true" : "false");
    fprintf(f, "    \"realtime\": %s,\n", ctx->cfg.realtime ? "true" : "false");
    fprintf(f, "    \"hwmon\": %s,\n", ctx->cfg.hwmon ? "true" : "false");
    fprintf(f, "    \"noise_cpu\": %s,\n", ctx->cfg.noise_cpu ? "true" : "false");
    fprintf(f, "    \"noise_io\": %s\n", ctx->cfg.noise_io ? "true" : "false");
    fprintf(f, "  },\n");
    fprintf(f, "  \"detected\": {\n");
    fprintf(f, "    \"cpu_freq_files\": %d,\n", cpu_count);
    fprintf(f, "    \"thermal_zones\": %d,\n", zone_count);
    fprintf(f, "    \"psi_cpu\": %s,\n", file_exists("/proc/pressure/cpu") ? "true" : "false");
    fprintf(f, "    \"psi_memory\": %s,\n", file_exists("/proc/pressure/memory") ? "true" : "false");
    fprintf(f, "    \"psi_io\": %s,\n", file_exists("/proc/pressure/io") ? "true" : "false");
    fprintf(f, "    \"hwmon_inputs\": %d,\n", sensor_count);
    fprintf(f, "    \"ping_available\": %s,\n", command_exists("ping") ? "true" : "false");
    fprintf(f, "    \"cgroup_v2\": %s,\n", file_exists("/sys/fs/cgroup/cgroup.controllers") ? "true" : "false");
    fprintf(f, "    \"mmc_life_time_est_count\": %d\n", mmc_count);
    fprintf(f, "  }\n");
    fprintf(f, "}\n");
    fclose(f);
}

static void write_stats_json(FILE *f, const char *name, const StatsVec *s, int indent, int trailing_comma) {
    int i;
    for (i = 0; i < indent; i++) fputc(' ', f);
    if (!s || s->size == 0) {
        fprintf(f, "\"%s\": null%s\n", name, trailing_comma ? "," : "");
        return;
    }
    fprintf(f, "\"%s\": {\"count\": %zu, \"min\": %.3f, \"avg\": %.3f, \"max\": %.3f, \"p50\": %.3f, \"p95\": %.3f, \"p99\": %.3f}%s\n",
            name,
            s->size,
            s->min,
            stats_avg(s),
            s->max,
            stats_percentile(s, 50.0),
            stats_percentile(s, 95.0),
            stats_percentile(s, 99.0),
            trailing_comma ? "," : "");
}

static void write_summary_json(
        RunContext *ctx,
        CpuResult *cpu,
        ThermalResult *thermal,
        PSIResult *psi,
        StorageResult *storage,
        NetworkResult *network,
        CgroupResult *cgroup,
        RealtimeResult *rt,
        HwmonResult *hwmon) {
    char path[PATH_MAX];
    if (!path_join2(path, sizeof(path), ctx->out_dir, "summary.json")) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    char ts[64];
    iso_time(ts, sizeof(ts));

    double fsync_first_half_p99 = 0.0, fsync_second_half_p99 = 0.0, fsync_growth_pct = 0.0;
    double metadata_p99_us = 0.0;
    if (storage->ran && storage->fsync_us.size > 1) {
        size_t mid = storage->fsync_us.size / 2;
        fsync_first_half_p99 = stats_percentile_range(&storage->fsync_us, 99.0, 0, mid == 0 ? storage->fsync_us.size : mid);
        fsync_second_half_p99 = stats_percentile_range(&storage->fsync_us, 99.0, mid, storage->fsync_us.size);
        fsync_growth_pct = calc_percent_change(fsync_first_half_p99, fsync_second_half_p99);
        metadata_p99_us = fmax(fmax(stats_percentile(&storage->unlink_us, 99.0), stats_percentile(&storage->mkdir_us, 99.0)), stats_percentile(&storage->rmdir_us, 99.0));
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"finished_at\": \"%s\",\n", ts);
    fprintf(f, "  \"scenario\": \"%s\",\n", ctx->cfg.scenario_name);
    fprintf(f, "  \"interrupted\": %s,\n", g_sigint ? "true" : "false");

    fprintf(f, "  \"cpu\": ");
    if (cpu->ran) {
        fprintf(f, "{\"samples\": %d, \"workers\": %d, \"peak_throughput\": %.3f, \"sustained_avg_throughput\": %.3f, \"peak_avg_freq_khz\": %.3f, \"sustained_avg_freq_khz\": %.3f, \"drift_percent\": %.3f, \"time_to_throttle_sec\": %.3f, \"time_to_first_5pct_drop_sec\": %.3f, \"time_to_first_10pct_drop_sec\": %.3f},\n",
                cpu->sample_count, cpu->workers, cpu->peak_throughput, cpu->sustained_avg_throughput, cpu->peak_avg_freq_khz, cpu->sustained_avg_freq_khz, cpu->drift_percent, cpu->time_to_throttle_sec, cpu->time_to_first_5pct_drop_sec, cpu->time_to_first_10pct_drop_sec);
    } else {
        fprintf(f, "null,\n");
    }

    fprintf(f, "  \"thermal\": ");
    if (thermal->ran) {
        fprintf(f, "{\"zones\": %d, \"critical_temp_mc\": %lld, \"max_temp_mc\": %lld, \"avg_max_temp_mc\": %.3f, \"time_to_critical_sec\": %.3f, \"start_temp_c\": %.3f, \"end_temp_c\": %.3f, \"temp_rise_c_per_min\": %.3f},\n",
                thermal->zones, thermal->critical_temp_mc, thermal->max_temp_mc, thermal->avg_max_temp_mc, thermal->time_to_critical_sec, thermal->start_temp_c, thermal->end_temp_c, thermal->temp_rise_c_per_min);
    } else {
        fprintf(f, "null,\n");
    }

    fprintf(f, "  \"psi\": ");
    if (psi->ran) {
        fprintf(f, "{\"has_cpu\": %s, \"has_mem\": %s, \"has_io\": %s, \"cpu_some_avg10_max\": %.3f, \"mem_some_avg10_max\": %.3f, \"io_some_avg10_max\": %.3f, \"cpu_total_delta\": %lld, \"mem_total_delta\": %lld, \"io_total_delta\": %lld},\n",
                psi->has_cpu ? "true" : "false",
                psi->has_mem ? "true" : "false",
                psi->has_io ? "true" : "false",
                psi->cpu_some_avg10_max,
                psi->mem_some_avg10_max,
                psi->io_some_avg10_max,
                psi->cpu_total_delta,
                psi->mem_total_delta,
                psi->io_total_delta);
    } else {
        fprintf(f, "null,\n");
    }

    fprintf(f, "  \"storage\": ");
    if (storage->ran) {
        fprintf(f, "{\n");
        write_stats_json(f, "create_us", &storage->create_us, 4, 1);
        write_stats_json(f, "write_us", &storage->write_us, 4, 1);
        write_stats_json(f, "fsync_us", &storage->fsync_us, 4, 1);
        write_stats_json(f, "close_us", &storage->close_us, 4, 1);
        write_stats_json(f, "unlink_us", &storage->unlink_us, 4, 1);
        write_stats_json(f, "mkdir_us", &storage->mkdir_us, 4, 1);
        write_stats_json(f, "rmdir_us", &storage->rmdir_us, 4, 1);
        fprintf(f, "    \"fsync_p99_first_half_us\": %.3f,\n", fsync_first_half_p99);
        fprintf(f, "    \"fsync_p99_second_half_us\": %.3f,\n", fsync_second_half_p99);
        fprintf(f, "    \"fsync_p99_growth_percent\": %.3f,\n", fsync_growth_pct);
        fprintf(f, "    \"metadata_p99_us\": %.3f,\n", metadata_p99_us);
        fprintf(f, "    \"mmc_health_present\": %s\n", storage->mmc_health_present ? "true" : "false");
        fprintf(f, "  },\n");
    } else {
        fprintf(f, "null,\n");
    }

    fprintf(f, "  \"network\": ");
    if (network->ran) {
        fprintf(f, "{\n");
        write_stats_json(f, "rtt_ms", &network->rtt_ms, 4, 1);
        fprintf(f, "    \"ping_available\": %s\n", network->ping_available ? "true" : "false");
        fprintf(f, "  },\n");
    } else {
        fprintf(f, "null,\n");
    }

    fprintf(f, "  \"cgroup\": ");
    if (cgroup->ran) {
        fprintf(f, "{\"cgroup_v2_available\": %s, \"memory_current_available\": %s, \"usage_usec_delta\": %lld, \"throttled_usec_delta\": %lld, \"nr_throttled_delta\": %lld, \"memory_current_last\": %lld},\n",
                cgroup->cgroup_v2_available ? "true" : "false",
                cgroup->memory_current_available ? "true" : "false",
                cgroup->usage_usec_delta,
                cgroup->throttled_usec_delta,
                cgroup->nr_throttled_delta,
                cgroup->memory_current_last);
    } else {
        fprintf(f, "null,\n");
    }

    fprintf(f, "  \"realtime\": ");
    if (rt->ran) {
        fprintf(f, "{\n");
        write_stats_json(f, "jitter_us", &rt->jitter_us, 4, 1);
        fprintf(f, "    \"over_500us_count\": %lld,\n", rt->over_500us_count);
        fprintf(f, "    \"over_1000us_count\": %lld\n", rt->over_1000us_count);
        fprintf(f, "  },\n");
    } else {
        fprintf(f, "null,\n");
    }

    fprintf(f, "  \"hwmon\": ");
    if (hwmon->ran) {
        fprintf(f, "{\"sensor_count\": %d},\n", hwmon->sensor_count);
    } else {
        fprintf(f, "null,\n");
    }

    fprintf(f, "  \"derived\": {\n");
    fprintf(f, "    \"compute_stability_hint\": \"lower cpu drift and later throttle are better\",\n");
    fprintf(f, "    \"storage_tail_hint\": \"lower fsync p99 and lower second-half growth are better\",\n");
    fprintf(f, "    \"realtime_hint\": \"lower jitter p99/max and fewer over-threshold events are better\"\n");
    fprintf(f, "  }\n");
    fprintf(f, "}\n");
    fclose(f);
}

static void *cpu_worker(void *arg) {
    CpuWorkerArgs *a = (CpuWorkerArgs *)arg;
    volatile double x = 1.0;
    while (!atomic_load(a->stop)) {
        for (int i = 0; i < 20000; i++) {
            x = x * 1.0000001 + sqrt(x + 1.0);
            if (x > 1000000.0) x = 1.0;
        }
        atomic_fetch_add(a->ops, 20000ULL);
    }
    return NULL;
}

static void *noise_cpu_worker(void *arg) {
    NoiseWorkerArgs *a = (NoiseWorkerArgs *)arg;
    volatile double x = 1.0;
    while (!atomic_load(a->stop) && !g_sigint) {
        for (int i = 0; i < 30000; i++) {
            x += sin(x) + cos(x);
            if (x > 1000000.0) x = 1.0;
        }
    }
    return NULL;
}

static void *noise_io_worker(void *arg) {
    NoiseWorkerArgs *a = (NoiseWorkerArgs *)arg;
    char path[] = "/tmp/sbc_noise_io.bin";
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) return NULL;
    unsigned char buf[4096];
    memset(buf, 0xA5, sizeof(buf));
    while (!atomic_load(a->stop) && !g_sigint) {
        lseek(fd, 0, SEEK_SET);
        ssize_t wr = write(fd, buf, sizeof(buf));
        (void)wr;
        fsync(fd);
        sleep_ms(10);
    }
    close(fd);
    unlink(path);
    return NULL;
}

static void *run_cpu_test(void *arg) {
    CpuThreadArgs *args = (CpuThreadArgs *)arg;
    RunContext *ctx = args->ctx;
    CpuResult *res = args->res;

    CpuFreqPath paths[MAX_CPU];
    long long freqs[MAX_CPU];
    int cpu_count = get_cpu_freq_paths(paths, MAX_CPU);

    char csv_path[PATH_MAX];
    if (!path_join2(csv_path, sizeof(csv_path), ctx->out_dir, "cpu.csv")) return NULL;
    FILE *csv = fopen(csv_path, "w");
    if (!csv) return NULL;
    fprintf(csv, "timestamp,elapsed_s,throughput_units_per_sec,avg_freq_khz,throttled");
    for (int i = 0; i < cpu_count; i++) fprintf(csv, ",cpu%d_freq_khz", i);
    fprintf(csv, "\n");

    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores < 1) cores = 1;
    atomic_ullong ops;
    atomic_init(&ops, 0ULL);
    atomic_int stop_workers;
    atomic_init(&stop_workers, 0);

    pthread_t *threads = calloc((size_t)cores, sizeof(pthread_t));
    CpuWorkerArgs *worker_args = calloc((size_t)cores, sizeof(CpuWorkerArgs));
    StatsVec throughput_stats;
    StatsVec freq_stats;
    stats_init(&throughput_stats);
    stats_init(&freq_stats);
    if (!threads || !worker_args) {
        fclose(csv);
        free(threads);
        free(worker_args);
        return NULL;
    }

    for (long i = 0; i < cores; i++) {
        worker_args[i].ops = &ops;
        worker_args[i].stop = &stop_workers;
        pthread_create(&threads[i], NULL, cpu_worker, &worker_args[i]);
    }

    unsigned long long last_ops = 0ULL;
    double peak_thr = 0.0;
    double peak_avg_freq = 0.0;
    double time_to_throttle = -1.0;
    double time_to_drop_5 = -1.0;
    double time_to_drop_10 = -1.0;
    int samples = 0;

    while (!should_stop(ctx)) {
        sleep_ms(1000);
        unsigned long long cur_ops = atomic_load(&ops);
        double throughput = (double)(cur_ops - last_ops);
        last_ops = cur_ops;
        read_cpu_freqs(paths, cpu_count, freqs);

        double avg_freq = 0.0;
        int valid = 0;
        for (int i = 0; i < cpu_count; i++) {
            if (freqs[i] >= 0) {
                avg_freq += (double)freqs[i];
                valid++;
            }
        }
        if (valid > 0) avg_freq /= valid;
        if (throughput > peak_thr) peak_thr = throughput;
        if (avg_freq > peak_avg_freq) peak_avg_freq = avg_freq;

        stats_push(&throughput_stats, throughput);
        if (valid > 0) stats_push(&freq_stats, avg_freq);

        int throttled = 0;
        double e = elapsed_sec(ctx);
        if (e > 5.0 && peak_thr > 0.0) {
            if (time_to_drop_5 < 0.0 && throughput < peak_thr * 0.95) time_to_drop_5 = e;
            if (time_to_drop_10 < 0.0 && throughput < peak_thr * 0.90) time_to_drop_10 = e;
            if (peak_avg_freq > 0.0 && throughput < peak_thr * 0.90 && avg_freq < peak_avg_freq * 0.95) {
                throttled = 1;
                if (time_to_throttle < 0.0) time_to_throttle = e;
            }
        }

        char ts[64];
        iso_time(ts, sizeof(ts));
        fprintf(csv, "%s,%.3f,%.3f,%.3f,%d", ts, e, throughput, avg_freq, throttled);
        for (int i = 0; i < cpu_count; i++) fprintf(csv, ",%lld", freqs[i]);
        fprintf(csv, "\n");
        fflush(csv);
        samples++;
    }

    atomic_store(&stop_workers, 1);
    for (long i = 0; i < cores; i++) pthread_join(threads[i], NULL);
    fclose(csv);

    res->ran = 1;
    res->sample_count = samples;
    res->peak_throughput = peak_thr;
    res->sustained_avg_throughput = throughput_stats.size > 1 ? stats_avg_range(&throughput_stats, throughput_stats.size / 2, throughput_stats.size) : stats_avg(&throughput_stats);
    res->peak_avg_freq_khz = peak_avg_freq;
    res->sustained_avg_freq_khz = freq_stats.size > 1 ? stats_avg_range(&freq_stats, freq_stats.size / 2, freq_stats.size) : stats_avg(&freq_stats);
    if (peak_thr > 0.0) res->drift_percent = ((peak_thr - res->sustained_avg_throughput) / peak_thr) * 100.0;
    else res->drift_percent = 0.0;
    res->time_to_throttle_sec = time_to_throttle;
    res->time_to_first_5pct_drop_sec = time_to_drop_5;
    res->time_to_first_10pct_drop_sec = time_to_drop_10;
    res->workers = (int)cores;

    char summary_path[PATH_MAX];
    if (path_join2(summary_path, sizeof(summary_path), ctx->out_dir, "cpu_summary.txt")) {
        FILE *sum = fopen(summary_path, "w");
        if (sum) {
            fprintf(sum, "peak_throughput_units_per_sec=%.3f\n", res->peak_throughput);
            fprintf(sum, "sustained_avg_throughput_units_per_sec=%.3f\n", res->sustained_avg_throughput);
            fprintf(sum, "peak_avg_freq_khz=%.3f\n", res->peak_avg_freq_khz);
            fprintf(sum, "sustained_avg_freq_khz=%.3f\n", res->sustained_avg_freq_khz);
            fprintf(sum, "drift_percent=%.3f\n", res->drift_percent);
            fprintf(sum, "time_to_throttle_sec=%.3f\n", res->time_to_throttle_sec);
            fprintf(sum, "time_to_first_5pct_drop_sec=%.3f\n", res->time_to_first_5pct_drop_sec);
            fprintf(sum, "time_to_first_10pct_drop_sec=%.3f\n", res->time_to_first_10pct_drop_sec);
            fprintf(sum, "workers=%d\n", res->workers);
            fclose(sum);
        }
    }

    stats_free(&throughput_stats);
    stats_free(&freq_stats);
    free(threads);
    free(worker_args);
    return NULL;
}

static void *run_thermal_test(void *arg) {
    ThermalThreadArgs *args = (ThermalThreadArgs *)arg;
    RunContext *ctx = args->ctx;
    ThermalResult *res = args->res;

    ThermalZone zones[MAX_THERMAL];
    int zone_count = get_thermal_zones(zones, MAX_THERMAL);
    char csv_path[PATH_MAX];
    if (!path_join2(csv_path, sizeof(csv_path), ctx->out_dir, "thermal.csv")) return NULL;
    FILE *csv = fopen(csv_path, "w");
    if (!csv) return NULL;
    fprintf(csv, "timestamp,elapsed_s,max_temp_mc,critical_temp_mc,reached_critical");
    for (int i = 0; i < zone_count; i++) fprintf(csv, ",%s_temp_mc", zones[i].dir_name);
    fprintf(csv, "\n");

    long long critical = detect_critical_temp_mc(zones, zone_count);
    double time_to_critical = -1.0;
    long long max_seen = -1;
    double first_temp_c = -1.0;
    double last_temp_c = -1.0;
    StatsVec max_temp_stats;
    stats_init(&max_temp_stats);

    while (!should_stop(ctx)) {
        long long max_temp = -1;
        long long temps[MAX_THERMAL];
        for (int i = 0; i < zone_count; i++) {
            temps[i] = read_ll_file(zones[i].temp_path, -1);
            if (temps[i] > max_temp) max_temp = temps[i];
        }
        if (max_temp > max_seen) max_seen = max_temp;
        if (max_temp >= 0) {
            double temp_c = max_temp / 1000.0;
            if (first_temp_c < 0.0) first_temp_c = temp_c;
            last_temp_c = temp_c;
            stats_push(&max_temp_stats, (double)max_temp);
        }
        int reached = (critical > 0 && max_temp >= critical) ? 1 : 0;
        double e = elapsed_sec(ctx);
        if (reached && time_to_critical < 0.0) time_to_critical = e;
        char ts[64];
        iso_time(ts, sizeof(ts));
        fprintf(csv, "%s,%.3f,%lld,%lld,%d", ts, e, max_temp, critical, reached);
        for (int i = 0; i < zone_count; i++) fprintf(csv, ",%lld", temps[i]);
        fprintf(csv, "\n");
        fflush(csv);
        sleep_ms(1000);
    }
    fclose(csv);

    res->ran = 1;
    res->zones = zone_count;
    res->critical_temp_mc = critical;
    res->max_temp_mc = max_seen;
    res->avg_max_temp_mc = stats_avg(&max_temp_stats);
    res->time_to_critical_sec = time_to_critical;
    res->start_temp_c = first_temp_c;
    res->end_temp_c = last_temp_c;
    if (first_temp_c >= 0.0 && last_temp_c >= 0.0) res->temp_rise_c_per_min = ((last_temp_c - first_temp_c) / (ctx->cfg.duration_minutes > 0 ? (double)ctx->cfg.duration_minutes : 1.0));
    else res->temp_rise_c_per_min = 0.0;

    char summary_path[PATH_MAX];
    if (path_join2(summary_path, sizeof(summary_path), ctx->out_dir, "thermal_summary.txt")) {
        FILE *sum = fopen(summary_path, "w");
        if (sum) {
            fprintf(sum, "critical_temp_mc=%lld\n", res->critical_temp_mc);
            fprintf(sum, "max_temp_mc=%lld\n", res->max_temp_mc);
            fprintf(sum, "avg_max_temp_mc=%.3f\n", res->avg_max_temp_mc);
            fprintf(sum, "time_to_critical_sec=%.3f\n", res->time_to_critical_sec);
            fprintf(sum, "start_temp_c=%.3f\n", res->start_temp_c);
            fprintf(sum, "end_temp_c=%.3f\n", res->end_temp_c);
            fprintf(sum, "temp_rise_c_per_min=%.3f\n", res->temp_rise_c_per_min);
            fprintf(sum, "zones=%d\n", res->zones);
            fclose(sum);
        }
    }
    stats_free(&max_temp_stats);
    return NULL;
}

static void *run_psi_test(void *arg) {
    PSIThreadArgs *args = (PSIThreadArgs *)arg;
    RunContext *ctx = args->ctx;
    PSIResult *res = args->res;

    int has_cpu = file_exists("/proc/pressure/cpu");
    int has_mem = file_exists("/proc/pressure/memory");
    int has_io = file_exists("/proc/pressure/io");
    if (!has_cpu && !has_mem && !has_io) return NULL;

    char csv_path[PATH_MAX];
    if (!path_join2(csv_path, sizeof(csv_path), ctx->out_dir, "psi.csv")) return NULL;
    FILE *csv = fopen(csv_path, "w");
    if (!csv) return NULL;
    fprintf(csv, "timestamp,elapsed_s,");
    fprintf(csv, "cpu_some_avg10,cpu_some_avg60,cpu_some_avg300,cpu_some_total,cpu_full_avg10,cpu_full_avg60,cpu_full_avg300,cpu_full_total,");
    fprintf(csv, "mem_some_avg10,mem_some_avg60,mem_some_avg300,mem_some_total,mem_full_avg10,mem_full_avg60,mem_full_avg300,mem_full_total,");
    fprintf(csv, "io_some_avg10,io_some_avg60,io_some_avg300,io_some_total,io_full_avg10,io_full_avg60,io_full_avg300,io_full_total\n");

    PSIValues cpu0, mem0, io0;
    psi_values_init(&cpu0);
    psi_values_init(&mem0);
    psi_values_init(&io0);
    if (has_cpu) cpu0 = read_psi_file("/proc/pressure/cpu");
    if (has_mem) mem0 = read_psi_file("/proc/pressure/memory");
    if (has_io) io0 = read_psi_file("/proc/pressure/io");

    double cpu_some_avg10_max = 0.0, mem_some_avg10_max = 0.0, io_some_avg10_max = 0.0;
    long long cpu_last_total = cpu0.some_total;
    long long mem_last_total = mem0.some_total;
    long long io_last_total = io0.some_total;

    while (!should_stop(ctx)) {
        PSIValues cpu, mem, io;
        psi_values_init(&cpu);
        psi_values_init(&mem);
        psi_values_init(&io);
        if (has_cpu) cpu = read_psi_file("/proc/pressure/cpu");
        if (has_mem) mem = read_psi_file("/proc/pressure/memory");
        if (has_io) io = read_psi_file("/proc/pressure/io");
        if (cpu.some_avg10 > cpu_some_avg10_max) cpu_some_avg10_max = cpu.some_avg10;
        if (mem.some_avg10 > mem_some_avg10_max) mem_some_avg10_max = mem.some_avg10;
        if (io.some_avg10 > io_some_avg10_max) io_some_avg10_max = io.some_avg10;
        if (cpu.some_total >= 0) cpu_last_total = cpu.some_total;
        if (mem.some_total >= 0) mem_last_total = mem.some_total;
        if (io.some_total >= 0) io_last_total = io.some_total;
        char ts[64];
        iso_time(ts, sizeof(ts));
        fprintf(csv,
                "%s,%.3f,%.3f,%.3f,%.3f,%lld,%.3f,%.3f,%.3f,%lld,%.3f,%.3f,%.3f,%lld,%.3f,%.3f,%.3f,%lld,%.3f,%.3f,%.3f,%lld,%.3f,%.3f,%.3f,%lld\n",
                ts, elapsed_sec(ctx),
                cpu.some_avg10, cpu.some_avg60, cpu.some_avg300, cpu.some_total, cpu.full_avg10, cpu.full_avg60, cpu.full_avg300, cpu.full_total,
                mem.some_avg10, mem.some_avg60, mem.some_avg300, mem.some_total, mem.full_avg10, mem.full_avg60, mem.full_avg300, mem.full_total,
                io.some_avg10, io.some_avg60, io.some_avg300, io.some_total, io.full_avg10, io.full_avg60, io.full_avg300, io.full_total);
        fflush(csv);
        sleep_ms(1000);
    }
    fclose(csv);

    res->ran = 1;
    res->has_cpu = has_cpu;
    res->has_mem = has_mem;
    res->has_io = has_io;
    res->cpu_some_avg10_max = cpu_some_avg10_max;
    res->mem_some_avg10_max = mem_some_avg10_max;
    res->io_some_avg10_max = io_some_avg10_max;
    res->cpu_total_delta = (cpu0.some_total >= 0 && cpu_last_total >= 0) ? (cpu_last_total - cpu0.some_total) : -1;
    res->mem_total_delta = (mem0.some_total >= 0 && mem_last_total >= 0) ? (mem_last_total - mem0.some_total) : -1;
    res->io_total_delta = (io0.some_total >= 0 && io_last_total >= 0) ? (io_last_total - io0.some_total) : -1;
    return NULL;
}

static void *run_storage_test(void *arg) {
    StorageThreadArgs *args = (StorageThreadArgs *)arg;
    RunContext *ctx = args->ctx;
    StorageResult *res = args->res;

    char work_dir[PATH_MAX];
    if (!path_join2(work_dir, sizeof(work_dir), ctx->out_dir, "storage_work")) return NULL;
    if (!ensure_dir_recursive(work_dir)) return NULL;

    char csv_path[PATH_MAX];
    if (!path_join2(csv_path, sizeof(csv_path), ctx->out_dir, "storage.csv")) return NULL;
    FILE *csv = fopen(csv_path, "w");
    if (!csv) return NULL;
    fprintf(csv, "timestamp,elapsed_s,create_file_us,write_us,fsync_us,close_us,unlink_us,mkdir_us,rmdir_us\n");

    stats_init(&res->create_us);
    stats_init(&res->write_us);
    stats_init(&res->fsync_us);
    stats_init(&res->close_us);
    stats_init(&res->unlink_us);
    stats_init(&res->mkdir_us);
    stats_init(&res->rmdir_us);

    unsigned char buf[4096];
    memset(buf, 0, sizeof(buf));
    unsigned long long counter = 0;

    while (!should_stop(ctx)) {
        char file_name[64];
        char dir_name[64];
        char file_path[PATH_MAX];
        char dir_path[PATH_MAX];
        snprintf(file_name, sizeof(file_name), "f_%llu.bin", counter);
        snprintf(dir_name, sizeof(dir_name), "d_%llu", counter);
        if (!path_join2(file_path, sizeof(file_path), work_dir, file_name)) break;
        if (!path_join2(dir_path, sizeof(dir_path), work_dir, dir_name)) break;
        counter++;

        long long t0 = now_ns();
        int fd = open(file_path, O_CREAT | O_TRUNC | O_RDWR, 0644);
        long long t1 = now_ns();
        long long t2 = t1, t3 = t1, t4 = t1, t5 = t1, t6 = t1, t7 = t1;
        if (fd >= 0) {
            ssize_t wr = write(fd, buf, sizeof(buf));
            (void)wr;
            t2 = now_ns();
            fsync(fd);
            t3 = now_ns();
            close(fd);
            t4 = now_ns();
            unlink(file_path);
            t5 = now_ns();
        }
        mkdir(dir_path, 0755);
        t6 = now_ns();
        rmdir(dir_path);
        t7 = now_ns();

        double create_us = (t1 - t0) / 1000.0;
        double write_us = (t2 - t1) / 1000.0;
        double fsync_us = (t3 - t2) / 1000.0;
        double close_us = (t4 - t3) / 1000.0;
        double unlink_us = (t5 - t4) / 1000.0;
        double mkdir_us = (t6 - t5) / 1000.0;
        double rmdir_us = (t7 - t6) / 1000.0;

        stats_push(&res->create_us, create_us);
        stats_push(&res->write_us, write_us);
        stats_push(&res->fsync_us, fsync_us);
        stats_push(&res->close_us, close_us);
        stats_push(&res->unlink_us, unlink_us);
        stats_push(&res->mkdir_us, mkdir_us);
        stats_push(&res->rmdir_us, rmdir_us);

        char ts[64];
        iso_time(ts, sizeof(ts));
        fprintf(csv, "%s,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                ts, elapsed_sec(ctx), create_us, write_us, fsync_us, close_us, unlink_us, mkdir_us, rmdir_us);
        fflush(csv);
        sleep_ms(20);
    }
    fclose(csv);

    char mmc_paths[MAX_MMC][PATH_MAX];
    int mmc_count = get_mmc_health_paths(mmc_paths, MAX_MMC);
    res->mmc_health_present = (mmc_count > 0);
    if (mmc_count > 0) {
        char out_path[PATH_MAX];
        if (path_join2(out_path, sizeof(out_path), ctx->out_dir, "mmc_health.txt")) {
            FILE *f = fopen(out_path, "w");
            if (f) {
                for (int i = 0; i < mmc_count; i++) {
                    char val[128];
                    if (!read_line_trim(mmc_paths[i], val, sizeof(val))) snprintf(val, sizeof(val), "unreadable");
                    fprintf(f, "%s=%s\n", mmc_paths[i], val);
                }
                fclose(f);
            }
        }
    }
    res->ran = 1;
    return NULL;
}

static void *run_network_test(void *arg) {
    NetworkThreadArgs *args = (NetworkThreadArgs *)arg;
    RunContext *ctx = args->ctx;
    NetworkResult *res = args->res;
    res->ping_available = command_exists("ping");
    if (!res->ping_available) return NULL;

    char csv_path[PATH_MAX];
    if (!path_join2(csv_path, sizeof(csv_path), ctx->out_dir, "network.csv")) return NULL;
    FILE *csv = fopen(csv_path, "w");
    if (!csv) return NULL;
    stats_init(&res->rtt_ms);
    fprintf(csv, "timestamp,elapsed_s,host,rtt_ms\n");
    while (!should_stop(ctx)) {
        double rtt = ping_once_ms(ctx->cfg.network_host);
        if (rtt >= 0.0) stats_push(&res->rtt_ms, rtt);
        char ts[64];
        iso_time(ts, sizeof(ts));
        fprintf(csv, "%s,%.3f,%s,%.3f\n", ts, elapsed_sec(ctx), ctx->cfg.network_host, rtt);
        fflush(csv);
        sleep_ms(1000);
    }
    fclose(csv);
    res->ran = 1;
    return NULL;
}

static void *run_cgroup_test(void *arg) {
    CgroupThreadArgs *args = (CgroupThreadArgs *)arg;
    RunContext *ctx = args->ctx;
    CgroupResult *res = args->res;
    res->cgroup_v2_available = file_exists("/sys/fs/cgroup/cgroup.controllers");
    if (!res->cgroup_v2_available) return NULL;

    char csv_path[PATH_MAX];
    if (!path_join2(csv_path, sizeof(csv_path), ctx->out_dir, "cgroup.csv")) return NULL;
    FILE *csv = fopen(csv_path, "w");
    if (!csv) return NULL;
    fprintf(csv, "timestamp,elapsed_s,usage_usec,user_usec,system_usec,nr_periods,nr_throttled,throttled_usec,memory_current\n");

    long long usage0 = -1, throttled0 = -1, nr_throttled0 = -1;
    long long last_usage = -1, last_throttled = -1, last_nr_throttled = -1, last_mem = -1;

    while (!should_stop(ctx)) {
        long long usage = -1, user = -1, system = -1, nr_periods = -1, nr_throttled = -1, throttled = -1;
        FILE *f = fopen("/sys/fs/cgroup/cpu.stat", "r");
        if (f) {
            char key[64];
            long long value;
            while (fscanf(f, "%63s %lld", key, &value) == 2) {
                if (strcmp(key, "usage_usec") == 0) usage = value;
                else if (strcmp(key, "user_usec") == 0) user = value;
                else if (strcmp(key, "system_usec") == 0) system = value;
                else if (strcmp(key, "nr_periods") == 0) nr_periods = value;
                else if (strcmp(key, "nr_throttled") == 0) nr_throttled = value;
                else if (strcmp(key, "throttled_usec") == 0) throttled = value;
            }
            fclose(f);
        }
        long long mem_current = read_ll_file("/sys/fs/cgroup/memory.current", -1);
        if (usage0 < 0) usage0 = usage;
        if (throttled0 < 0) throttled0 = throttled;
        if (nr_throttled0 < 0) nr_throttled0 = nr_throttled;
        last_usage = usage;
        last_throttled = throttled;
        last_nr_throttled = nr_throttled;
        last_mem = mem_current;
        char ts[64];
        iso_time(ts, sizeof(ts));
        fprintf(csv, "%s,%.3f,%lld,%lld,%lld,%lld,%lld,%lld,%lld\n", ts, elapsed_sec(ctx), usage, user, system, nr_periods, nr_throttled, throttled, mem_current);
        fflush(csv);
        sleep_ms(1000);
    }
    fclose(csv);

    res->ran = 1;
    res->memory_current_available = (last_mem >= 0);
    res->usage_usec_delta = (usage0 >= 0 && last_usage >= 0) ? (last_usage - usage0) : -1;
    res->throttled_usec_delta = (throttled0 >= 0 && last_throttled >= 0) ? (last_throttled - throttled0) : -1;
    res->nr_throttled_delta = (nr_throttled0 >= 0 && last_nr_throttled >= 0) ? (last_nr_throttled - nr_throttled0) : -1;
    res->memory_current_last = last_mem;
    return NULL;
}

static void *run_realtime_test(void *arg) {
    RealtimeThreadArgs *args = (RealtimeThreadArgs *)arg;
    RunContext *ctx = args->ctx;
    RealtimeResult *res = args->res;
    char csv_path[PATH_MAX];
    if (!path_join2(csv_path, sizeof(csv_path), ctx->out_dir, "realtime.csv")) return NULL;
    FILE *csv = fopen(csv_path, "w");
    if (!csv) return NULL;
    stats_init(&res->jitter_us);
    res->over_500us_count = 0;
    res->over_1000us_count = 0;
    fprintf(csv, "timestamp,elapsed_s,period_us,jitter_us\n");

    const long long period_ns = 10LL * 1000LL * 1000LL;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    while (!should_stop(ctx)) {
        long long target = (long long)next.tv_sec * 1000000000LL + next.tv_nsec + period_ns;
        next.tv_sec = target / 1000000000LL;
        next.tv_nsec = target % 1000000000LL;
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        long long actual = now_ns();
        double jitter_us = (actual - target) / 1000.0;
        stats_push(&res->jitter_us, jitter_us);
        if (jitter_us > 500.0) res->over_500us_count++;
        if (jitter_us > 1000.0) res->over_1000us_count++;
        char ts[64];
        iso_time(ts, sizeof(ts));
        fprintf(csv, "%s,%.3f,%.3f,%.3f\n", ts, elapsed_sec(ctx), period_ns / 1000.0, jitter_us);
        if ((res->jitter_us.size & 63U) == 0) fflush(csv);
    }
    fclose(csv);
    res->ran = 1;
    return NULL;
}

static void *run_hwmon_test(void *arg) {
    HwmonThreadArgs *args = (HwmonThreadArgs *)arg;
    RunContext *ctx = args->ctx;
    HwmonResult *res = args->res;
    HwmonSensor sensors[MAX_HWMON];
    int sensor_count = get_hwmon_sensors(sensors, MAX_HWMON);
    if (sensor_count == 0) return NULL;
    char csv_path[PATH_MAX];
    if (!path_join2(csv_path, sizeof(csv_path), ctx->out_dir, "hwmon.csv")) return NULL;
    FILE *csv = fopen(csv_path, "w");
    if (!csv) return NULL;
    fprintf(csv, "timestamp,elapsed_s");
    for (int i = 0; i < sensor_count; i++) fprintf(csv, ",%s", sensors[i].label);
    fprintf(csv, "\n");
    while (!should_stop(ctx)) {
        char ts[64];
        iso_time(ts, sizeof(ts));
        fprintf(csv, "%s,%.3f", ts, elapsed_sec(ctx));
        for (int i = 0; i < sensor_count; i++) fprintf(csv, ",%lld", read_ll_file(sensors[i].path, -1));
        fprintf(csv, "\n");
        fflush(csv);
        sleep_ms(2000);
    }
    fclose(csv);
    res->ran = 1;
    res->sensor_count = sensor_count;
    return NULL;
}

static void apply_preset(Config *cfg, int preset) {
    cfg->cpu = cfg->thermal = cfg->psi = cfg->storage = cfg->network = cfg->cgroup = cfg->realtime = cfg->hwmon = 0;
    cfg->noise_cpu = 0;
    cfg->noise_io = 0;
    snprintf(cfg->scenario_name, sizeof(cfg->scenario_name), "custom");
    if (preset == 1) {
        cfg->cpu = 1; cfg->thermal = 1; cfg->psi = 1; cfg->hwmon = 1;
        snprintf(cfg->scenario_name, sizeof(cfg->scenario_name), "soak");
    } else if (preset == 2) {
        cfg->storage = 1; cfg->thermal = 1; cfg->psi = 1; cfg->hwmon = 1;
        snprintf(cfg->scenario_name, sizeof(cfg->scenario_name), "storage-node");
    } else if (preset == 3) {
        cfg->realtime = 1; cfg->thermal = 1; cfg->hwmon = 1;
        snprintf(cfg->scenario_name, sizeof(cfg->scenario_name), "control-loop");
    } else if (preset == 4) {
        cfg->cpu = cfg->thermal = cfg->psi = cfg->storage = cfg->network = cfg->cgroup = cfg->realtime = cfg->hwmon = 1;
        snprintf(cfg->scenario_name, sizeof(cfg->scenario_name), "full-stack");
    } else if (preset == 5) {
        cfg->cpu = 1; cfg->thermal = 1; cfg->psi = 1; cfg->realtime = 1; cfg->hwmon = 1;
        cfg->noise_cpu = 1;
        snprintf(cfg->scenario_name, sizeof(cfg->scenario_name), "noisy-compute");
    } else if (preset == 6) {
        cfg->storage = 1; cfg->psi = 1; cfg->network = 1; cfg->thermal = 1;
        cfg->noise_io = 1;
        snprintf(cfg->scenario_name, sizeof(cfg->scenario_name), "edge-under-io-noise");
    }
}

static void print_menu(const Config *cfg) {
    printf("\n===== SBC Benchmark Menu =====\n");
    printf("1. CPU                  [%c]\n", cfg->cpu ? 'x' : ' ');
    printf("2. Thermal              [%c]\n", cfg->thermal ? 'x' : ' ');
    printf("3. PSI                  [%c]\n", cfg->psi ? 'x' : ' ');
    printf("4. Storage              [%c]\n", cfg->storage ? 'x' : ' ');
    printf("5. Network              [%c]\n", cfg->network ? 'x' : ' ');
    printf("6. cgroup v2            [%c]\n", cfg->cgroup ? 'x' : ' ');
    printf("7. Real-time latency    [%c]\n", cfg->realtime ? 'x' : ' ');
    printf("8. hwmon / power        [%c]\n", cfg->hwmon ? 'x' : ' ');
    printf("\nScenario: %s\n", cfg->scenario_name);
    printf("Duration: %d min\n", cfg->duration_minutes);
    printf("Network host: %s\n", cfg->network_host);
    printf("Noise CPU: %s (threads=%d)\n", cfg->noise_cpu ? "on" : "off", cfg->noise_cpu_threads);
    printf("Noise IO : %s\n", cfg->noise_io ? "on" : "off");
    printf("\nCommands:\n");
    printf("  1 3 5      - toggle tests\n");
    printf("  a          - enable all\n");
    printf("  n          - disable all\n");
    printf("  p1         - preset soak   (CPU + Thermal + PSI + hwmon)\n");
    printf("  p2         - preset IO     (Storage + Thermal + PSI + hwmon)\n");
    printf("  p3         - preset RT     (Real-time + Thermal + hwmon)\n");
    printf("  p4         - preset full   (all tests)\n");
    printf("  p5         - preset noisy  (CPU + PSI + Thermal + RT + CPU noise)\n");
    printf("  p6         - preset edge   (Storage + Network + PSI + Thermal + IO noise)\n");
    printf("  d <min>    - set duration\n");
    printf("  h <host>   - set ping host\n");
    printf("  z          - toggle CPU noisy-neighbor\n");
    printf("  zi         - toggle IO noisy-neighbor\n");
    printf("  nt <N>     - set CPU noise threads\n");
    printf("  s          - start benchmark and return to menu after finish\n");
    printf("  q          - quit\n");
    printf("> ");
    fflush(stdout);
}

static void toggle_by_index(Config *cfg, int idx) {
    if (idx == 1) cfg->cpu = !cfg->cpu;
    else if (idx == 2) cfg->thermal = !cfg->thermal;
    else if (idx == 3) cfg->psi = !cfg->psi;
    else if (idx == 4) cfg->storage = !cfg->storage;
    else if (idx == 5) cfg->network = !cfg->network;
    else if (idx == 6) cfg->cgroup = !cfg->cgroup;
    else if (idx == 7) cfg->realtime = !cfg->realtime;
    else if (idx == 8) cfg->hwmon = !cfg->hwmon;
}

typedef enum {
    MENU_START = 1,
    MENU_QUIT = 0
} MenuAction;

static MenuAction interactive_configure(Config *cfg) {
    char line[256];
    while (1) {
        print_menu(cfg);
        if (!fgets(line, sizeof(line), stdin)) return MENU_QUIT;
        char *cmd = trim(line);
        if (strcmp(cmd, "q") == 0) return MENU_QUIT;
        if (strcmp(cmd, "s") == 0) return MENU_START;
        if (strcmp(cmd, "a") == 0) {
            cfg->cpu = cfg->thermal = cfg->psi = cfg->storage = cfg->network = cfg->cgroup = cfg->realtime = cfg->hwmon = 1;
            snprintf(cfg->scenario_name, sizeof(cfg->scenario_name), "custom");
            continue;
        }
        if (strcmp(cmd, "n") == 0) {
            cfg->cpu = cfg->thermal = cfg->psi = cfg->storage = cfg->network = cfg->cgroup = cfg->realtime = cfg->hwmon = 0;
            snprintf(cfg->scenario_name, sizeof(cfg->scenario_name), "custom");
            continue;
        }
        if (strcmp(cmd, "z") == 0) {
            cfg->noise_cpu = !cfg->noise_cpu;
            snprintf(cfg->scenario_name, sizeof(cfg->scenario_name), "custom");
            continue;
        }
        if (strcmp(cmd, "zi") == 0) {
            cfg->noise_io = !cfg->noise_io;
            snprintf(cfg->scenario_name, sizeof(cfg->scenario_name), "custom");
            continue;
        }
        if (strcmp(cmd, "p1") == 0 || strcmp(cmd, "p2") == 0 || strcmp(cmd, "p3") == 0 || strcmp(cmd, "p4") == 0 || strcmp(cmd, "p5") == 0 || strcmp(cmd, "p6") == 0) {
            apply_preset(cfg, cmd[1] - '0');
            continue;
        }
        if (strncmp(cmd, "d ", 2) == 0) {
            int v = atoi(trim(cmd + 2));
            if (v > 0) cfg->duration_minutes = v;
            snprintf(cfg->scenario_name, sizeof(cfg->scenario_name), "custom");
            continue;
        }
        if (strncmp(cmd, "h ", 2) == 0) {
            char *host = trim(cmd + 2);
            if (*host) snprintf(cfg->network_host, sizeof(cfg->network_host), "%s", host);
            snprintf(cfg->scenario_name, sizeof(cfg->scenario_name), "custom");
            continue;
        }
        if (strncmp(cmd, "nt ", 3) == 0) {
            int v = atoi(trim(cmd + 3));
            if (v > 0 && v <= 256) cfg->noise_cpu_threads = v;
            snprintf(cfg->scenario_name, sizeof(cfg->scenario_name), "custom");
            continue;
        }
        char *save = NULL;
        char *tok = strtok_r(cmd, " \t", &save);
        int changed = 0;
        while (tok) {
            if (is_numeric_str(tok)) {
                int idx = atoi(tok);
                if (idx >= 1 && idx <= 8) {
                    toggle_by_index(cfg, idx);
                    changed = 1;
                }
            }
            tok = strtok_r(NULL, " \t", &save);
        }
        if (changed) {
            snprintf(cfg->scenario_name, sizeof(cfg->scenario_name), "custom");
            continue;
        }
        printf("Unknown command\n");
    }
}

static void print_run_summary(
        const RunContext *ctx,
        const CpuResult *cpu,
        const ThermalResult *thermal,
        const PSIResult *psi,
        const StorageResult *storage,
        const NetworkResult *network,
        const CgroupResult *cgroup,
        const RealtimeResult *rt,
        const HwmonResult *hwmon) {
    printf("\n===== Run summary =====\n");
    printf("Results: %s\n", ctx->out_dir);
    if (cpu->ran) {
        printf("CPU: peak %.2f, sustained %.2f, drift %.2f%%, drop5 %.2fs, drop10 %.2fs, throttle %.2fs\n",
               cpu->peak_throughput,
               cpu->sustained_avg_throughput,
               cpu->drift_percent,
               cpu->time_to_first_5pct_drop_sec,
               cpu->time_to_first_10pct_drop_sec,
               cpu->time_to_throttle_sec);
    }
    if (thermal->ran) {
        printf("Thermal: start %.2f C, end %.2f C, max %.2f C, rise %.2f C/min, critical %.2f C, time-to-critical %.2fs\n",
               thermal->start_temp_c,
               thermal->end_temp_c,
               thermal->max_temp_mc / 1000.0,
               thermal->temp_rise_c_per_min,
               thermal->critical_temp_mc / 1000.0,
               thermal->time_to_critical_sec);
    }
    if (psi->ran) {
        printf("PSI: cpu_max_avg10 %.3f, mem_max_avg10 %.3f, io_max_avg10 %.3f\n",
               psi->cpu_some_avg10_max, psi->mem_some_avg10_max, psi->io_some_avg10_max);
    }
    if (storage->ran) {
        printf("Storage fsync_us: avg %.2f, p95 %.2f, p99 %.2f\n",
               stats_avg(&storage->fsync_us),
               stats_percentile(&storage->fsync_us, 95.0),
               stats_percentile(&storage->fsync_us, 99.0));
    }
    if (network->ran && network->rtt_ms.size > 0) {
        printf("Network rtt_ms: avg %.2f, p95 %.2f, p99 %.2f\n",
               stats_avg(&network->rtt_ms),
               stats_percentile(&network->rtt_ms, 95.0),
               stats_percentile(&network->rtt_ms, 99.0));
    }
    if (rt->ran && rt->jitter_us.size > 0) {
        printf("Realtime jitter_us: avg %.2f, p95 %.2f, p99 %.2f, max %.2f, >500us %lld, >1000us %lld\n",
               stats_avg(&rt->jitter_us),
               stats_percentile(&rt->jitter_us, 95.0),
               stats_percentile(&rt->jitter_us, 99.0),
               rt->jitter_us.max,
               rt->over_500us_count,
               rt->over_1000us_count);
    }
    if (cgroup->ran) {
        printf("cgroup: usage delta %lld usec, throttled delta %lld usec\n",
               cgroup->usage_usec_delta,
               cgroup->throttled_usec_delta);
    }
    if (hwmon->ran) {
        printf("hwmon: %d sensor(s) logged\n", hwmon->sensor_count);
    }
    if (g_sigint) {
        printf("Run stopped by Ctrl+C. Returning to menu.\n");
    }
}

static void free_results(StorageResult *storage, NetworkResult *network, RealtimeResult *rt) {
    stats_free(&storage->create_us);
    stats_free(&storage->write_us);
    stats_free(&storage->fsync_us);
    stats_free(&storage->close_us);
    stats_free(&storage->unlink_us);
    stats_free(&storage->mkdir_us);
    stats_free(&storage->rmdir_us);
    stats_free(&network->rtt_ms);
    stats_free(&rt->jitter_us);
}

static void run_benchmark(const Config *cfg) {
    RunContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    atomic_init(&ctx.stop, 0);
    ctx.cfg = *cfg;
    clock_gettime(CLOCK_MONOTONIC, &ctx.start_ts);
    long long end_ns = (long long)ctx.start_ts.tv_sec * 1000000000LL + ctx.start_ts.tv_nsec + (long long)cfg->duration_minutes * 60LL * 1000000000LL;
    ctx.end_ts.tv_sec = end_ns / 1000000000LL;
    ctx.end_ts.tv_nsec = end_ns % 1000000000LL;

    char tbuf[64];
    compact_time(tbuf, sizeof(tbuf));
    snprintf(ctx.out_dir, sizeof(ctx.out_dir), "bench_results_%s", tbuf);
    if (!ensure_dir_recursive(ctx.out_dir)) {
        fprintf(stderr, "Cannot create output directory: %s\n", ctx.out_dir);
        return;
    }

    write_run_info_json(&ctx);
    write_system_info_json(&ctx);

    printf("\nScenario: %s\n", cfg->scenario_name);
    printf("Output directory: %s\n", ctx.out_dir);
    printf("Running for %d minute(s)... Press Ctrl+C to stop current run and return to menu.\n", cfg->duration_minutes);
    if (cfg->network && !command_exists("ping")) printf("Network test requested, but ping not found. It will be skipped.\n");
    if (cfg->psi && !file_exists("/proc/pressure/cpu") && !file_exists("/proc/pressure/memory") && !file_exists("/proc/pressure/io")) printf("PSI requested, but unavailable. It will be skipped.\n");
    if (cfg->cgroup && !file_exists("/sys/fs/cgroup/cgroup.controllers")) printf("cgroup v2 requested, but unavailable. It will be skipped.\n");

    CpuResult cpu = {0};
    ThermalResult thermal = {0};
    PSIResult psi = {0};
    StorageResult storage = {0};
    NetworkResult network = {0};
    CgroupResult cgroup = {0};
    RealtimeResult rt = {0};
    HwmonResult hwmon = {0};

    pthread_t threads[MAX_TEST_THREADS];
    int tcount = 0;

    CpuThreadArgs cpu_args = { &ctx, &cpu };
    ThermalThreadArgs thermal_args = { &ctx, &thermal };
    PSIThreadArgs psi_args = { &ctx, &psi };
    StorageThreadArgs storage_args = { &ctx, &storage };
    NetworkThreadArgs network_args = { &ctx, &network };
    CgroupThreadArgs cgroup_args = { &ctx, &cgroup };
    RealtimeThreadArgs rt_args = { &ctx, &rt };
    HwmonThreadArgs hwmon_args = { &ctx, &hwmon };

    atomic_int noise_stop;
    atomic_init(&noise_stop, 0);
    pthread_t noise_threads[260];
    int noise_count = 0;
    NoiseWorkerArgs noise_args = { &noise_stop };

    g_sigint = 0;

    if (cfg->noise_cpu) {
        int n = cfg->noise_cpu_threads > 0 ? cfg->noise_cpu_threads : 1;
        for (int i = 0; i < n && noise_count < 260; i++) {
            pthread_create(&noise_threads[noise_count++], NULL, noise_cpu_worker, &noise_args);
        }
    }
    if (cfg->noise_io && noise_count < 260) {
        pthread_create(&noise_threads[noise_count++], NULL, noise_io_worker, &noise_args);
    }

    if (cfg->cpu) pthread_create(&threads[tcount++], NULL, run_cpu_test, &cpu_args);
    if (cfg->thermal) pthread_create(&threads[tcount++], NULL, run_thermal_test, &thermal_args);
    if (cfg->psi) pthread_create(&threads[tcount++], NULL, run_psi_test, &psi_args);
    if (cfg->storage) pthread_create(&threads[tcount++], NULL, run_storage_test, &storage_args);
    if (cfg->network) pthread_create(&threads[tcount++], NULL, run_network_test, &network_args);
    if (cfg->cgroup) pthread_create(&threads[tcount++], NULL, run_cgroup_test, &cgroup_args);
    if (cfg->realtime) pthread_create(&threads[tcount++], NULL, run_realtime_test, &rt_args);
    if (cfg->hwmon) pthread_create(&threads[tcount++], NULL, run_hwmon_test, &hwmon_args);

    for (int i = 0; i < tcount; i++) pthread_join(threads[i], NULL);
    atomic_store(&ctx.stop, 1);
    atomic_store(&noise_stop, 1);
    for (int i = 0; i < noise_count; i++) pthread_join(noise_threads[i], NULL);

    write_summary_json(&ctx, &cpu, &thermal, &psi, &storage, &network, &cgroup, &rt, &hwmon);

    char done_path[PATH_MAX];
    if (path_join2(done_path, sizeof(done_path), ctx.out_dir, "DONE.txt")) {
        FILE *done = fopen(done_path, "w");
        if (done) {
            char ts[64];
            iso_time(ts, sizeof(ts));
            fprintf(done, "finished_at=%s\n", ts);
            fprintf(done, "output_dir=%s\n", ctx.out_dir);
            fprintf(done, "interrupted=%d\n", g_sigint ? 1 : 0);
            fclose(done);
        }
    }

    print_run_summary(&ctx, &cpu, &thermal, &psi, &storage, &network, &cgroup, &rt, &hwmon);
    free_results(&storage, &network, &rt);
}

int main(void) {
    signal(SIGINT, signal_handler);

    Config cfg;
    cfg.cpu = 1;
    cfg.thermal = 1;
    cfg.psi = 1;
    cfg.storage = 0;
    cfg.network = 0;
    cfg.cgroup = 0;
    cfg.realtime = 0;
    cfg.hwmon = 1;
    cfg.duration_minutes = 5;
    cfg.noise_cpu = 0;
    cfg.noise_cpu_threads = 2;
    cfg.noise_io = 0;
    snprintf(cfg.network_host, sizeof(cfg.network_host), "1.1.1.1");
    snprintf(cfg.scenario_name, sizeof(cfg.scenario_name), "soak");

    while (1) {
        MenuAction act = interactive_configure(&cfg);
        if (act == MENU_QUIT) {
            printf("Exit.\n");
            break;
        }
        run_benchmark(&cfg);
    }
    return 0;
}
