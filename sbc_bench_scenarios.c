#include "sbc_bench_scenarios.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const ScenarioEntry g_scenario_entries[] = {
    {"baseline", "Базовый тест", "CPU + memory + storage + network + jitter"},
    {"long_soak", "Длительный тест устойчивости", "Прогрев + длинная фаза устойчивости"},
    {"server_gateway", "Периферийный сервер", "CPU/IO/сеть с акцентом на хранение и RTT"},
    {"iot", "Контроллер IoT", "Короткие bursts + стабильный uplink + low power"},
    {"embedded", "Встраиваемый сценарий", "Предсказуемая средняя нагрузка и контроль тепла"},
    {"neural_host", "Нейросетевой сценарий", "Лёгкая матричная/векторная нагрузка"},
    {"manual", "Ручной выбор тестов", "Выбор отдельных тестов пользователем"},
};

static const char *kind_name_local(WorkKind k)
{
  switch (k)
  {
  case WK_IDLE:
    return "idle";
  case WK_CPU_BURN:
    return "cpu_burn";
  case WK_STORAGE:
    return "storage";
  case WK_PING:
    return "ping";
  case WK_NN:
    return "nn_inference";
  case WK_MEMORY:
    return "memory";
  case WK_JITTER:
    return "jitter";
  default:
    return "unknown";
  }
}

Step make_step(const char *name, WorkKind kind, int duration_sec, int threads, const char *arg, const char *load_profile, const char *purpose)
{
  Step s;
  s.name = name;
  s.kind = kind;
  s.duration_sec = duration_sec;
  s.threads = threads;
  s.arg = arg;
  s.load_profile = load_profile;
  s.purpose = purpose;
  return s;
}

static void set_primary_metrics(Scenario *s, const char **metrics, int n)
{
  s->primary_metric_count = 0;
  for (int i = 0; i < n && i < (int)(sizeof(s->primary_metrics) / sizeof(s->primary_metrics[0])); ++i)
    s->primary_metrics[s->primary_metric_count++] = metrics[i];
}

void print_scenario_help(const char *prog)
{
  fprintf(stderr, "Usage: %s [scenario] [duration_scale]\n", prog);
  fprintf(stderr, "       %s --menu\n", prog);
  fprintf(stderr, "       %s --list-scenarios\n", prog);
  fprintf(stderr, "Scenarios: baseline, long_soak, server_gateway, iot, embedded, neural_host, manual\n");
}

void print_scenario_catalog(void)
{
  fprintf(stdout, "Сценарии:\n");
  for (size_t i = 0; i < sizeof(g_scenario_entries) / sizeof(g_scenario_entries[0]); ++i)
  {
    fprintf(stdout, " %zu) %s (%s)\n    %s\n",
            i + 1,
            g_scenario_entries[i].title,
            g_scenario_entries[i].key,
            g_scenario_entries[i].description);
  }
}

static Scenario build_manual_scenario_from_prompt(void)
{
  Scenario s;
  memset(&s, 0, sizeof(s));
  s.name = "manual";
  s.description = "Ручной выбор отдельных тестов";
  s.sample_sec = 1;
  s.critical_temp_c = 85.0;
  s.target_ping_p99_ms = 25.0;
  s.ref_perf_per_watt = 10000000.0;
  s.assumed_power_w = 5.0;
  s.noise_mode = NOISE_NONE;

  fprintf(stdout, "\nВыбери тесты (через пробел, Enter=все):\n");
  fprintf(stdout, "1=CPU 2=MEMORY 3=STORAGE 4=NETWORK 5=JITTER 6=NN\n> ");

  char line[256];
  if (!fgets(line, sizeof(line), stdin))
    line[0] = '\0';

  int use_cpu = (strstr(line, "1") != NULL);
  int use_mem = (strstr(line, "2") != NULL);
  int use_sto = (strstr(line, "3") != NULL);
  int use_net = (strstr(line, "4") != NULL);
  int use_jit = (strstr(line, "5") != NULL);
  int use_nn = (strstr(line, "6") != NULL);

  if (line[0] == '\n' || line[0] == '\0')
    use_cpu = use_mem = use_sto = use_net = use_jit = use_nn = 1;

  int n = 0;
  if (use_cpu)
    s.steps[n++] = make_step("cpu_manual", WK_CPU_BURN, 90, 2, NULL, "steady", "Ручной CPU тест");
  if (use_mem)
    s.steps[n++] = make_step("memory_manual", WK_MEMORY, 45, 1, "64M", "steady", "Ручной memory тест");
  if (use_sto)
    s.steps[n++] = make_step("storage_manual", WK_STORAGE, 60, 1, "4M", "steady", "Ручной storage тест");
  if (use_net)
    s.steps[n++] = make_step("network_manual", WK_PING, 45, 1, "1.1.1.1", "steady", "Ручной network RTT тест");
  if (use_jit)
    s.steps[n++] = make_step("jitter_manual", WK_JITTER, 20, 1, "1000", "steady", "Ручной jitter тест");
  if (use_nn)
    s.steps[n++] = make_step("nn_manual", WK_NN, 60, 2, "32", "steady", "Ручной NN тест");

  s.step_count = n;

  const char *metrics[] = {
      "cpu_ops_avg",
      "mem_copy_mb_s_avg",
      "storage_mb_s_avg",
      "ping_p95_ms_avg",
      "jitter_p99_us",
      "nn_inf_per_sec_avg"};
  set_primary_metrics(&s, metrics, 6);

  return s;
}

Scenario scenario_from_name(const char *name)
{
  Scenario s;
  memset(&s, 0, sizeof(s));
  s.sample_sec = 1;
  s.critical_temp_c = 85.0;
  s.target_ping_p99_ms = 25.0;
  s.ref_perf_per_watt = 10000000.0;
  s.assumed_power_w = 5.0;
  s.noise_mode = NOISE_NONE;

  if (strcmp(name, "long_soak") == 0 || strcmp(name, "soak") == 0)
  {
    const char *metrics[] = {"cpu_ops_avg", "temp_c_max", "cpu_freq_mhz_avg", "psi_cpu_some_avg10_max", "stability_score_pct"};
    s.name = "long_soak";
    s.description = "Длительный тест устойчивости";
    s.critical_temp_c = 90.0;
    s.target_ping_p99_ms = 40.0;
    s.noise_mode = NOISE_CPU;
    s.steps[0] = make_step("warmup", WK_CPU_BURN, 180, 2, NULL, "burst", "Прогрев");
    s.steps[1] = make_step("steady_cpu", WK_CPU_BURN, 1200, 2, NULL, "steady", "Длинная CPU-фаза");
    s.steps[2] = make_step("steady_memory", WK_MEMORY, 120, 1, "128M", "steady", "Проверка памяти");
    s.steps[3] = make_step("steady_storage", WK_STORAGE, 240, 1, "4M", "steady", "Storage длительная фаза");
    s.steps[4] = make_step("steady_network", WK_PING, 180, 1, "1.1.1.1", "steady", "RTT фаза");
    s.steps[5] = make_step("steady_jitter", WK_JITTER, 30, 1, "1000", "steady", "Jitter фаза");
    s.step_count = 6;
    set_primary_metrics(&s, metrics, 5);
  }
  else if (strcmp(name, "server_gateway") == 0)
  {
    const char *metrics[] = {"storage_mb_s_avg", "storage_lat_p99_us", "ping_p95_ms_avg", "packet_loss_pct_avg", "psi_io_some_avg10_max"};
    s.name = "server_gateway";
    s.description = "Сценарий периферийного сервера";
    s.critical_temp_c = 90.0;
    s.target_ping_p99_ms = 50.0;
    s.noise_mode = NOISE_COMBINED;
    s.steps[0] = make_step("cpu_warmup", WK_CPU_BURN, 90, 2, NULL, "burst", "Разогрев CPU");
    s.steps[1] = make_step("storage_heavy", WK_STORAGE, 180, 1, "8M", "steady", "Тяжёлая storage нагрузка");
    s.steps[2] = make_step("network_tail", WK_PING, 90, 1, "8.8.8.8", "steady", "RTT under load");
    s.steps[3] = make_step("jitter_check", WK_JITTER, 20, 1, "1000", "steady", "Проверка джиттера");
    s.step_count = 4;
    set_primary_metrics(&s, metrics, 5);
  }
  else if (strcmp(name, "iot") == 0 || strcmp(name, "iot_controller") == 0)
  {
    const char *metrics[] = {"cpu_util_pct_avg", "mem_used_pct_avg", "ping_p95_ms_avg", "packet_loss_pct_avg", "power_w_avg"};
    s.name = "iot";
    s.description = "Сценарий контроллера интернета вещей";
    s.critical_temp_c = 80.0;
    s.target_ping_p99_ms = 20.0;
    s.assumed_power_w = 3.0;
    s.steps[0] = make_step("idle_baseline", WK_IDLE, 60, 1, NULL, "recovery", "Фон");
    s.steps[1] = make_step("cpu_burst", WK_CPU_BURN, 30, 1, NULL, "burst", "Короткий burst");
    s.steps[2] = make_step("mem_burst", WK_MEMORY, 20, 1, "32M", "burst", "Memory burst");
    s.steps[3] = make_step("network_probe", WK_PING, 60, 1, "1.1.1.1", "steady", "Uplink probe");
    s.steps[4] = make_step("sleep_window", WK_IDLE, 90, 1, NULL, "recovery", "Окно сна");
    s.step_count = 5;
    set_primary_metrics(&s, metrics, 5);
  }
  else if (strcmp(name, "embedded") == 0)
  {
    const char *metrics[] = {"cpu_ops_avg", "jitter_p99_us", "temp_c_max", "power_w_avg", "stability_score_pct"};
    s.name = "embedded";
    s.description = "Встраиваемый сценарий";
    s.critical_temp_c = 85.0;
    s.target_ping_p99_ms = 35.0;
    s.steps[0] = make_step("boot_settle", WK_IDLE, 45, 1, NULL, "recovery", "Стабилизация");
    s.steps[1] = make_step("control_cpu", WK_CPU_BURN, 90, 2, NULL, "steady", "Контрольный цикл CPU");
    s.steps[2] = make_step("control_memory", WK_MEMORY, 45, 1, "64M", "steady", "Контроль памяти");
    s.steps[3] = make_step("control_jitter", WK_JITTER, 25, 1, "1000", "steady", "Реакция таймера");
    s.step_count = 4;
    set_primary_metrics(&s, metrics, 5);
  }
  else if (strcmp(name, "neural_host") == 0 || strcmp(name, "neural") == 0)
  {
    const char *metrics[] = {"nn_inf_per_sec_avg", "perf_per_watt", "temp_c_max", "cpu_freq_mhz_avg", "mem_copy_mb_s_avg"};
    s.name = "neural_host";
    s.description = "Нейросетевой сценарий (лёгкая матричная нагрузка)";
    s.critical_temp_c = 90.0;
    s.target_ping_p99_ms = 40.0;
    s.steps[0] = make_step("nn_warmup", WK_NN, 90, 2, "32", "burst", "Прогрев матриц");
    s.steps[1] = make_step("nn_steady", WK_NN, 240, 2, "48", "steady", "Стабильный инференс");
    s.steps[2] = make_step("mem_support", WK_MEMORY, 60, 1, "128M", "steady", "Поддерживающий memory тест");
    s.step_count = 3;
    set_primary_metrics(&s, metrics, 5);
  }
  else if (strcmp(name, "manual") == 0)
  {
    return build_manual_scenario_from_prompt();
  }
  else
  {
    const char *metrics[] = {"cpu_ops_avg", "mem_copy_mb_s_avg", "storage_mb_s_avg", "ping_p95_ms_avg", "jitter_p99_us"};
    s.name = "baseline";
    s.description = "Базовый сценарий";
    s.steps[0] = make_step("cpu_base", WK_CPU_BURN, 60, 2, NULL, "steady", "CPU");
    s.steps[1] = make_step("mem_base", WK_MEMORY, 30, 1, "64M", "steady", "Memory");
    s.steps[2] = make_step("storage_base", WK_STORAGE, 60, 1, "4M", "steady", "Storage");
    s.steps[3] = make_step("network_base", WK_PING, 45, 1, "1.1.1.1", "steady", "RTT");
    s.steps[4] = make_step("jitter_base", WK_JITTER, 15, 1, "1000", "steady", "Jitter");
    s.step_count = 5;
    set_primary_metrics(&s, metrics, 5);
  }

  return s;
}

double parse_duration_scale(const char *in, double fallback)
{
  if (!in || !*in)
    return fallback;
  char *end = NULL;
  double v = strtod(in, &end);
  if (end == in || v <= 0.0)
    return fallback;
  return v;
}

Scenario build_custom_scenario_from_prompt(void)
{
  return build_manual_scenario_from_prompt();
}

static int read_line_stdin(char *buf, size_t n)
{
  if (!fgets(buf, (int)n, stdin))
    return -1;
  size_t len = strlen(buf);
  while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
    buf[--len] = '\0';
  return 0;
}

static void print_console_header(void)
{
  fprintf(stdout, "\n============================================================\n");
  fprintf(stdout, " SBC BENCHMARK v4 - SCENARIO CONSOLE\n");
  fprintf(stdout, "============================================================\n");
  fprintf(stdout, "1) Базовый тест\n");
  fprintf(stdout, "2) Длительный тест устойчивости\n");
  fprintf(stdout, "3) Периферийный сервер\n");
  fprintf(stdout, "4) Контроллер IoT\n");
  fprintf(stdout, "5) Встраиваемый сценарий\n");
  fprintf(stdout, "6) Нейросетевой сценарий\n");
  fprintf(stdout, "7) Ручной выбор тестов\n");
  fprintf(stdout, "------------------------------------------------------------\n");
}

static int prompt_report_mode(int *replace_latest)
{
  char line[32];
  fprintf(stdout, "Режим отчёта: [1] новый run (по умолчанию), [2] заменить <scenario>_latest: ");
  if (read_line_stdin(line, sizeof(line)) != 0)
    return -1;
  *replace_latest = (strcmp(line, "2") == 0);
  return 0;
}

int show_interactive_menu(char out_scenario[64], double *out_scale, int *use_custom, int *replace_latest)
{
  char line[256];
  while (1)
  {
    print_console_header();
    fprintf(stdout, "8) Показать описание сценариев\n");
    fprintf(stdout, "9) Запустить все сценарии\n");
    fprintf(stdout, "a) Анализ последнего прогона\n");
    fprintf(stdout, "q) Выход\n\n");
    fprintf(stdout, "Выбор (1-9, a, q): ");

    if (read_line_stdin(line, sizeof(line)) != 0)
      return -1;

    if (strcmp(line, "q") == 0 || strcmp(line, "Q") == 0)
      return 0;

    if (strcmp(line, "8") == 0)
    {
      print_scenario_catalog();
      continue;
    }

    if (strcmp(line, "9") == 0)
    {
      *use_custom = 0;
      *replace_latest = 0;
      snprintf(out_scenario, 64, "__all__");
      fprintf(stdout, "\nБудут запущены все сценарии. ");
      fprintf(stdout, "Это может занять длительное время.\n");
      fprintf(stdout, "Масштаб длительности (Enter=0.05): ");
      if (read_line_stdin(line, sizeof(line)) != 0)
        return -1;
      *out_scale = parse_duration_scale(line, 0.05);
      if (prompt_report_mode(replace_latest) != 0)
        return -1;
      return 1;
    }

    if (strcmp(line, "a") == 0 || strcmp(line, "A") == 0)
    {
      *use_custom = 0;
      *replace_latest = 0;
      *out_scale = 1.0;
      snprintf(out_scenario, 64, "__analyze__");
      return 1;
    }

    int choice = atoi(line);
    if (choice < 1 || choice > 7)
    {
      fprintf(stdout, "Неизвестная опция: %s\n", line);
      continue;
    }

    snprintf(out_scenario, 64, "%s", g_scenario_entries[choice - 1].key);
    *use_custom = (choice == 7);

    fprintf(stdout, "\nВыбран сценарий: %s\n", g_scenario_entries[choice - 1].title);
    fprintf(stdout, "%s\n", g_scenario_entries[choice - 1].description);
    fprintf(stdout, "Масштаб длительности (Enter=1.0): ");
    if (read_line_stdin(line, sizeof(line)) != 0)
      return -1;
    *out_scale = parse_duration_scale(line, 1.0);

    if (prompt_report_mode(replace_latest) != 0)
      return -1;

    return 1;
  }
}

void print_execution_plan(const Scenario *sc)
{
  fprintf(stdout, "\nПлан сценария: %s\n", sc->name);
  fprintf(stdout, "Описание: %s\n", sc->description ? sc->description : "n/a");
  fprintf(stdout, "Основные метрики: ");
  for (int i = 0; i < sc->primary_metric_count; ++i)
    fprintf(stdout, "%s%s", sc->primary_metrics[i], (i + 1 < sc->primary_metric_count) ? ", " : "\n");

  fprintf(stdout, "Noise mode: %d\n", (int)sc->noise_mode);

  for (int i = 0; i < sc->step_count; ++i)
  {
    const Step *st = &sc->steps[i];
    fprintf(stdout, "  %d) %s | %s | %d sec | threads=%d | load=%s\n",
            i + 1,
            st->name,
            kind_name_local(st->kind),
            st->duration_sec,
            st->threads,
            st->load_profile ? st->load_profile : "n/a");
    if (st->purpose && st->purpose[0])
      fprintf(stdout, "     - %s\n", st->purpose);
  }
  fprintf(stdout, "\n");
}

int is_valid_scenario_name(const char *scenario_name)
{
  return strcmp(scenario_name, "baseline") == 0 ||
         strcmp(scenario_name, "long_soak") == 0 ||
         strcmp(scenario_name, "soak") == 0 ||
         strcmp(scenario_name, "server_gateway") == 0 ||
         strcmp(scenario_name, "iot") == 0 ||
         strcmp(scenario_name, "iot_controller") == 0 ||
         strcmp(scenario_name, "embedded") == 0 ||
         strcmp(scenario_name, "neural_host") == 0 ||
         strcmp(scenario_name, "neural") == 0 ||
         strcmp(scenario_name, "manual") == 0;
}