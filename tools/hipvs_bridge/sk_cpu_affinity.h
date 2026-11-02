/* Copyright (c) 2026 OceanBase. Licensed under Apache-2.0.
 * Adapted from Terrified-of-Logic/seekdb-ans tools/hipvs_build/build_helper.cpp
 * at f9fe575be66f0ce789c894590aedeaa6d3fe30de. See contest/PROVENANCE.md.
 * Unlike the standalone helper, change only this build thread and restore it.
 */
#ifndef SEEKDB_SK_CPU_AFFINITY_H
#define SEEKDB_SK_CPU_AFFINITY_H
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

typedef struct { cpu_set_t previous; int changed; } sk_cpu_scope;

static int sk_quota_budget(long long quota, long long period)
{
  if (quota <= 0 || period <= 0) return 0;
  long long cores = quota / period;
  return cores < 1 ? 1 : (cores > INT_MAX ? INT_MAX : (int)cores);
}

static int sk_cgroup_budget(void)
{
  char quota_text[64]; long long period, quota;
  FILE *f = fopen("/sys/fs/cgroup/cpu.max", "r");
  if (f) {
    int parsed = fscanf(f, "%63s %lld", quota_text, &period); fclose(f);
    if (parsed != 2 || strcmp(quota_text, "max") == 0) return 0;
    char *end = NULL; quota = strtoll(quota_text, &end, 10);
    return end && *end == '\0' ? sk_quota_budget(quota, period) : 0;
  }
  f = fopen("/sys/fs/cgroup/cpu/cpu.cfs_quota_us", "r");
  if (!f) return 0;
  int parsed = fscanf(f, "%lld", &quota); fclose(f);
  if (parsed != 1) return 0;
  f = fopen("/sys/fs/cgroup/cpu/cpu.cfs_period_us", "r");
  if (!f) return 0;
  parsed = fscanf(f, "%lld", &period); fclose(f);
  return parsed == 1 ? sk_quota_budget(quota, period) : 0;
}

static int sk_compact_mask(const cpu_set_t *allowed, int budget, cpu_set_t *chosen)
{
  int available = CPU_COUNT(allowed);
  if (budget <= 0 || budget >= available) return 0;
  CPU_ZERO(chosen);
  int added = 0;
  for (int cpu = 0; cpu < CPU_SETSIZE && added < budget; ++cpu) {
    if (CPU_ISSET(cpu, allowed)) { CPU_SET(cpu, chosen); ++added; }
  }
  return added == budget;
}

static sk_cpu_scope sk_compact_begin(void)
{
  sk_cpu_scope scope; memset(&scope, 0, sizeof(scope));
  cpu_set_t chosen;
  if (sched_getaffinity(0, sizeof(scope.previous), &scope.previous) == 0 &&
      sk_compact_mask(&scope.previous, sk_cgroup_budget(), &chosen) &&
      sched_setaffinity(0, sizeof(chosen), &chosen) == 0) scope.changed = 1;
  return scope;
}

static void sk_compact_end(sk_cpu_scope *scope)
{
  if (scope->changed && sched_setaffinity(0, sizeof(scope->previous), &scope->previous) != 0)
    fprintf(stderr, "[seekdb_cuvs] warning: failed to restore build-thread CPU affinity\n");
}
#endif
