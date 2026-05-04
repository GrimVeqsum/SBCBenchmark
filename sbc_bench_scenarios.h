#ifndef SBC_BENCH_SCENARIOS_H
#define SBC_BENCH_SCENARIOS_H

#include "sbc_bench_types.h"

Step make_step(const char *name, WorkKind kind, int duration_sec, int threads, const char *arg, const char *load_profile, const char *purpose);

void print_scenario_help(const char *prog);
void print_scenario_catalog(void);

Scenario scenario_from_name(const char *name);
double parse_duration_scale(const char *in, double fallback);

/* Для ручного выбора отдельных тестов */
Scenario build_custom_scenario_from_prompt(void);

/*
 * return:
 *  1 - сценарий выбран
 *  0 - выход
 * -1 - ошибка ввода
 */
int show_interactive_menu(char out_scenario[64], double *out_scale, int *use_custom, int *replace_latest);

void print_execution_plan(const Scenario *sc);
int is_valid_scenario_name(const char *scenario_name);

#endif