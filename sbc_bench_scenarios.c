#include "sbc_bench_scenarios.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const ScenarioEntry g_scenario_entries[] = {
    {"baseline", "Custom (base profile)", "Базовый профиль: CPU + storage + network; подходит как отправная точка."},
    {"iot", "IoT controller", "Управление умными устройствами: короткие bursts + окно сна + стабильный uplink."},
    {"embedded", "Embedded control", "Встраиваемый режим: предсказуемая средняя нагрузка и тепловая устойчивость."},
    {"neural_host", "Neural inference", "Локальный инференс: длительная нейросетевая нагрузка + контроль энергоэффективности."},
    {"server_gateway", "Gateway / data server", "Шлюз и сервер хранения: длительный mixed workload CPU + сеть + запись."},
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
  fprintf(stderr, "Scenarios: baseline(custom), iot, server_gateway, embedded, neural_host\n");
}

void print_scenario_catalog(void)
{
  const char *baseline_metrics[] = {"cpu_ops_avg", "storage_mb_s_avg", "ping_p99_ms_avg", "thermal_headroom_pct"};
  const char *iot_metrics[] = {"cpu_util_pct_avg", "packet_loss_pct_avg", "ping_p99_ms_avg", "power_w_avg", "mem_used_pct_avg"};
  const char *server_metrics[] = {"cpu_ops_avg", "storage_mb_s_avg", "ping_p99_ms_avg", "psi_io_some_avg10_max", "temp_c_max"};
  const char *embedded_metrics[] = {"cpu_ops_avg", "temp_c_max", "power_w_avg", "psi_cpu_some_avg10_max", "mem_used_pct_avg"};
  const char *neural_metrics[] = {"nn_inf_per_sec_avg", "perf_per_watt", "temp_c_max", "cpu_freq_mhz_avg", "mem_used_pct_avg"};

  fprintf(stdout, "Available scenarios:\n\n");
  fprintf(stdout, "1) baseline — Универсальный базовый прогон для сравнения плат.\n");
  fprintf(stdout, "   Метрики: %s, %s, %s, %s\n\n", baseline_metrics[0], baseline_metrics[1], baseline_metrics[2], baseline_metrics[3]);

  fprintf(stdout, "2) iot — Контроллер/сборщик IoT с акцентом на энергоэффективность и сетевую стабильность.\n");
  fprintf(stdout, "   Метрики: %s, %s, %s, %s, %s\n\n", iot_metrics[0], iot_metrics[1], iot_metrics[2], iot_metrics[3], iot_metrics[4]);

  fprintf(stdout, "3) server_gateway — Сервер/шлюз с длительной CPU, сетью и записью в хранилище.\n");
  fprintf(stdout, "   Метрики: %s, %s, %s, %s, %s\n\n", server_metrics[0], server_metrics[1], server_metrics[2], server_metrics[3], server_metrics[4]);

  fprintf(stdout, "4) embedded — Встраиваемое устройство с устойчивым контролем и тепловыми ограничениями.\n");
  fprintf(stdout, "   Метрики: %s, %s, %s, %s, %s\n\n", embedded_metrics[0], embedded_metrics[1], embedded_metrics[2], embedded_metrics[3], embedded_metrics[4]);

  fprintf(stdout, "5) neural_host — Одноплатник с локальным размещением нейросети.\n");
  fprintf(stdout, "   Метрики: %s, %s, %s, %s, %s\n", neural_metrics[0], neural_metrics[1], neural_metrics[2], neural_metrics[3], neural_metrics[4]);
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

  if (strcmp(name, "server_gateway") == 0 || strcmp(name, "server_edge") == 0)
  {
    const char *metrics[] = {"cpu_ops_avg", "storage_mb_s_avg", "ping_p99_ms_avg", "psi_io_some_avg10_max", "temp_c_max"};
    s.name = "server_gateway";
    s.description = "Сервер/шлюз: длительная смешанная нагрузка CPU + storage + network";
    s.critical_temp_c = 90.0;
    s.target_ping_p99_ms = 40.0;
    s.steps[0] = make_step("warmup", WK_CPU_BURN, 180, 4, NULL, "burst", "Разогрев CPU и вывод частот в рабочий диапазон");
    s.steps[1] = make_step("network_latency", WK_PING, 120, 0, "8.8.8.8", "steady", "Оценка tail latency канала для ролей шлюза");
    s.steps[2] = make_step("storage_write", WK_STORAGE, 180, 0, "8M", "burst", "Проверка пропускной способности записи для кэша/логов");
    s.steps[3] = make_step("cpu_steady", WK_CPU_BURN, 900, 4, NULL, "steady", "Длительная нагрузка для оценки троттлинга и стабильности");
    s.step_count = 4;
    set_primary_metrics(&s, metrics, 5);
  }
  else if (strcmp(name, "embedded") == 0)
  {
    const char *metrics[] = {"cpu_ops_avg", "temp_c_max", "power_w_avg", "psi_cpu_some_avg10_max", "mem_used_pct_avg"};
    s.name = "embedded";
    s.description = "Встраиваемое устройство: стабильный контрольный цикл с ограниченным теплопакетом";
    s.critical_temp_c = 85.0;
    s.target_ping_p99_ms = 30.0;
    s.steps[0] = make_step("boot_settle", WK_IDLE, 60, 0, NULL, "recovery", "Стабилизация после старта, контроль фоновой температуры");
    s.steps[1] = make_step("control_compute", WK_CPU_BURN, 120, 2, NULL, "steady", "Имитация контрольного цикла встроенного устройства");
    s.steps[2] = make_step("persistent_storage", WK_STORAGE, 90, 0, "1M", "burst", "Короткие записи состояния/журнала");
    s.steps[3] = make_step("link_health", WK_PING, 60, 0, "1.1.1.1", "steady", "Проверка сетевой предсказуемости для управляющего контура");
    s.steps[4] = make_step("steady_control", WK_CPU_BURN, 240, 2, NULL, "steady", "Устойчивость вычислений на средней мощности");
    s.step_count = 5;
    set_primary_metrics(&s, metrics, 5);
  }
  else if (strcmp(name, "neural_host") == 0 || strcmp(name, "neural") == 0)
  {
    const char *metrics[] = {"nn_inf_per_sec_avg", "perf_per_watt", "temp_c_max", "cpu_freq_mhz_avg", "mem_used_pct_avg"};
    s.name = "neural_host";
    s.description = "Хост нейросети: инференс под длительной нагрузкой и контроль энергоэффективности";
    s.critical_temp_c = 90.0;
    s.target_ping_p99_ms = 35.0;
    s.ref_perf_per_watt = 15000000.0;
    s.steps[0] = make_step("idle_baseline", WK_IDLE, 60, 0, NULL, "recovery", "Базовый тепловой/энергетический фон перед инференсом");
    s.steps[1] = make_step("nn_warmup", WK_NN, 120, 2, "32", "burst", "Разогрев модели и кэшей памяти");
    s.steps[2] = make_step("storage_checkpoint", WK_STORAGE, 60, 0, "4M", "burst", "Промежуточная запись артефактов");
    s.steps[3] = make_step("network_probe", WK_PING, 45, 0, "1.1.1.1", "steady", "Контроль сетевого хвоста во время работы модели");
    s.steps[4] = make_step("nn_steady", WK_NN, 300, 2, "48", "steady", "Длительный инференс для метрик perf/watt и тепла");
    s.step_count = 5;
    set_primary_metrics(&s, metrics, 5);
  }
  else if (strcmp(name, "iot") == 0 || strcmp(name, "iot_controller") == 0)
  {
    const char *metrics[] = {"cpu_util_pct_avg", "packet_loss_pct_avg", "ping_p99_ms_avg", "power_w_avg", "mem_used_pct_avg"};
    s.name = "iot";
    s.description = "IoT сценарий: опрос сенсоров, выгрузка телеметрии и окно сна";
    s.critical_temp_c = 80.0;
    s.target_ping_p99_ms = 20.0;
    s.ref_perf_per_watt = 7000000.0;
    s.assumed_power_w = 3.0;
    s.steps[0] = make_step("idle_baseline", WK_IDLE, 120, 0, NULL, "recovery", "Энергосберегающий фон IoT-контроллера");
    s.steps[1] = make_step("sensor_batch_compute", WK_CPU_BURN, 60, 1, NULL, "burst", "Короткая обработка пачки показаний сенсоров");
    s.steps[2] = make_step("persist_batch", WK_STORAGE, 45, 0, "1M", "burst", "Сохранение телеметрии на локальный носитель");
    s.steps[3] = make_step("uplink_health", WK_PING, 60, 0, "1.1.1.1", "steady", "Проверка стабильности uplink до облака/роутера");
    s.steps[4] = make_step("sleep_window", WK_IDLE, 180, 0, NULL, "recovery", "Окно сна для оценки среднего энергопрофиля");
    s.step_count = 5;
    set_primary_metrics(&s, metrics, 5);
  }
  else
  {
    const char *metrics[] = {"cpu_ops_avg", "storage_mb_s_avg", "ping_p99_ms_avg", "thermal_headroom_pct"};
    s.name = "baseline";
    s.description = "Базовый сценарий: CPU + storage + network для первичного сравнения плат";
    s.steps[0] = make_step("boot_settle", WK_IDLE, 30, 0, NULL, "recovery", "Фиксация стартового состояния");
    s.steps[1] = make_step("cpu_warmup", WK_CPU_BURN, 120, 2, NULL, "burst", "Краткий стартовый стресс CPU");
    s.steps[2] = make_step("storage_probe", WK_STORAGE, 90, 0, "4M", "burst", "Проверка скоростей записи на файловую систему");
    s.steps[3] = make_step("network_probe", WK_PING, 45, 0, "1.1.1.1", "steady", "Оценка сетевой задержки p99");
    s.steps[4] = make_step("cpu_steady", WK_CPU_BURN, 300, 2, NULL, "steady", "Устойчивость под продолжительной нагрузкой");
    s.step_count = 5;
    set_primary_metrics(&s, metrics, 4);
  }
  return s;
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

static int custom_metric_enabled(const char *line, int idx)
{
  if (!line || !*line)
    return 1;
  char token[8];
  snprintf(token, sizeof(token), "%d", idx);
  return strstr(line, token) != NULL;
}

Scenario build_custom_scenario_from_prompt(void)
{
  Scenario s = scenario_from_name("baseline");
  s.name = "baseline";
  s.description = "Custom profile: вручную выбранные метрики на базовом наборе шагов";

  fprintf(stdout, "\n[Custom] Выбор важных метрик (через пробел, Enter = все):\n");
  fprintf(stdout, " 1=cpu_ops_avg 2=storage_mb_s_avg 3=ping_p99_ms_avg 4=thermal_headroom_pct 5=power_w_avg 6=mem_used_pct_avg\n");
  fprintf(stdout, " > ");
  char line[256];
  if (read_line_stdin(line, sizeof(line)) != 0)
    line[0] = '\0';

  const char *selected[8];
  int nsel = 0;
  if (custom_metric_enabled(line, 1))
    selected[nsel++] = "cpu_ops_avg";
  if (custom_metric_enabled(line, 2))
    selected[nsel++] = "storage_mb_s_avg";
  if (custom_metric_enabled(line, 3))
    selected[nsel++] = "ping_p99_ms_avg";
  if (custom_metric_enabled(line, 4))
    selected[nsel++] = "thermal_headroom_pct";
  if (custom_metric_enabled(line, 5))
    selected[nsel++] = "power_w_avg";
  if (custom_metric_enabled(line, 6))
    selected[nsel++] = "mem_used_pct_avg";
  if (nsel == 0)
    selected[nsel++] = "cpu_ops_avg";

  s.primary_metric_count = 0;
  for (int i = 0; i < nsel; ++i)
    s.primary_metrics[s.primary_metric_count++] = selected[i];
  return s;
}

static void print_console_header(void)
{
  fprintf(stdout, "\n============================================================\n");
  fprintf(stdout, " SBC BENCHMARK v4 - SCENARIO CONSOLE\n");
  fprintf(stdout, "============================================================\n");
  fprintf(stdout, "1) Custom (base profile)\n");
  fprintf(stdout, "2) IoT controller\n");
  fprintf(stdout, "3) Embedded control\n");
  fprintf(stdout, "4) Neural inference\n");
  fprintf(stdout, "5) Gateway / data server\n");
  fprintf(stdout, "------------------------------------------------------------\n");
}

static int prompt_report_mode(int *replace_latest)
{
  char line[32];
  fprintf(stdout, "Report mode: [1] separate timestamped run (default), [2] replace <scenario>_latest: ");
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
    fprintf(stdout, "6) List scenario details\n");
    fprintf(stdout, "7) Run all scenarios (sequential)\n");
    fprintf(stdout, "8) Analyze latest run\n");
    fprintf(stdout, "0) Exit\n\n");
    fprintf(stdout, "Select option (0-8): ");
    if (read_line_stdin(line, sizeof(line)) != 0)
      return -1;
    if (strcmp(line, "0") == 0)
      return 0;
    if (strcmp(line, "6") == 0)
    {
      print_scenario_catalog();
      continue;
    }
    if (strcmp(line, "7") == 0)
    {
      *use_custom = 0;
      *replace_latest = 0;
      snprintf(out_scenario, 64, "__all__");
      fprintf(stdout, "\nSelected: run all scenarios (baseline/iot/embedded/neural_host/server_gateway)\n");
      fprintf(stdout, "Duration scale (Enter = 0.05 for short run): ");
      if (read_line_stdin(line, sizeof(line)) != 0)
        return -1;
      *out_scale = parse_duration_scale(line, 0.05);
      if (prompt_report_mode(replace_latest) != 0)
        return -1;
      return 1;
    }
    if (strcmp(line, "8") == 0)
    {
      *use_custom = 0;
      *replace_latest = 0;
      *out_scale = 1.0;
      snprintf(out_scenario, 64, "__analyze__");
      return 1;
    }

    int choice = atoi(line);
    if (choice < 1 || choice > 5)
    {
      fprintf(stdout, "Unknown option: %s\n", line);
      continue;
    }

    snprintf(out_scenario, 64, "%s", g_scenario_entries[choice - 1].key);
    *use_custom = (choice == 1);
    fprintf(stdout, "\nSelected: %s\n%s\n", g_scenario_entries[choice - 1].title, g_scenario_entries[choice - 1].description);
    fprintf(stdout, "Duration scale (Enter = 1.0, example 0.10 for short run): ");
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
  fprintf(stdout, "\nScenario plan: %s\n", sc->name);
  fprintf(stdout, "Description: %s\n", sc->description ? sc->description : "n/a");
  fprintf(stdout, "Primary metrics: ");
  for (int i = 0; i < sc->primary_metric_count; ++i)
    fprintf(stdout, "%s%s", sc->primary_metrics[i], (i + 1 < sc->primary_metric_count) ? ", " : "\n");
  for (int i = 0; i < sc->step_count; ++i)
  {
    const Step *st = &sc->steps[i];
    fprintf(stdout, "  %d) %s | %s | %ds | load=%s\n", i + 1, st->name, kind_name_local(st->kind), st->duration_sec,
            st->load_profile ? st->load_profile : "n/a");
    if (st->purpose && st->purpose[0])
      fprintf(stdout, "     - %s\n", st->purpose);
  }
  fprintf(stdout, "\n");
}

int is_valid_scenario_name(const char *scenario_name)
{
  return strcmp(scenario_name, "baseline") == 0 ||
         strcmp(scenario_name, "iot") == 0 ||
         strcmp(scenario_name, "iot_controller") == 0 ||
         strcmp(scenario_name, "server_gateway") == 0 ||
         strcmp(scenario_name, "server_edge") == 0 ||
         strcmp(scenario_name, "embedded") == 0 ||
         strcmp(scenario_name, "neural_host") == 0 ||
         strcmp(scenario_name, "neural") == 0;
}