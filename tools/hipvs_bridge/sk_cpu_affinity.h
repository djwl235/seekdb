/* Copyright (c) 2026 OceanBase. Licensed under Apache-2.0.
 * Initial affinity compaction adapted from Terrified-of-Logic/seekdb-ans
 * f9fe575be66f0ce789c894590aedeaa6d3fe30de; see contest/PROVENANCE.md.
 * Resolve the calling task's effective quota and change only its build scope.
 */
#ifndef SEEKDB_SK_CPU_AFFINITY_H
#define SEEKDB_SK_CPU_AFFINITY_H
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Optional symbols: no new dependency when OpenMP is absent (host mocks). */
extern int omp_get_max_threads(void) __attribute__((weak));
extern void omp_set_num_threads(int) __attribute__((weak));
#ifdef __cplusplus
}
#endif

typedef struct {
  cpu_set_t previous;
  int changed;
  int omp_previous;
  int omp_changed;
} sk_cpu_scope;

static int sk_quota_budget(long long quota, long long period)
{
  if (quota <= 0 || period <= 0) return 0;
  long long cores = quota / period;
  return cores < 1 ? 1 : (cores > INT_MAX ? INT_MAX : (int)cores);
}
static int sk_min_budget(int a, int b)
{ return a <= 0 ? b : (b <= 0 || a < b ? a : b); }
static int sk_controller(const char *list, const char *wanted)
{
  size_t n = strlen(wanted);
  for (const char *p = list; p && *p;) {
    const char *end = strchr(p, ','); size_t len = end ? (size_t)(end-p) : strcspn(p, "\r\n");
    if (len == n && memcmp(p, wanted, n) == 0) return 1;
    p = end ? end+1 : NULL;
  }
  return 0;
}
static int sk_mount_unescape(const char *input, char *out, size_t capacity)
{
  size_t n = 0;
  for (size_t i = 0; input[i]; ++i) {
    unsigned char value = (unsigned char)input[i];
    if (value == '\\' && input[i+1] >= '0' && input[i+1] <= '7' &&
        input[i+2] && input[i+2] >= '0' && input[i+2] <= '7' &&
        input[i+3] && input[i+3] >= '0' && input[i+3] <= '7') {
      value = (unsigned char)((input[i+1]-'0')*64+(input[i+2]-'0')*8+input[i+3]-'0');
      i += 3;
    }
    if (!value || n+1 >= capacity) return 0;
    out[n++] = (char)value;
  }
  out[n] = 0; return 1;
}
static int sk_safe_member(const char *path)
{
  if (path[0] != '/') return 0;
  for (const char *p = path; *p;) {
    while (*p == '/') ++p;
    size_t n = strcspn(p, "/");
    if ((n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) return 0;
    p += n;
  }
  return 1;
}
static int sk_cpu_limit_at(const char *directory, int unified)
{
  char path[PATH_MAX], value[64]; long long quota = 0, period = 0;
  int n = snprintf(path, sizeof(path), "%s/%s", directory,
                   unified ? "cpu.max" : "cpu.cfs_quota_us");
  if (n < 0 || (size_t)n >= sizeof(path)) return 0;
  FILE *f = fopen(path, "r"); if (!f) return 0;
  if (unified) {
    int parsed = fscanf(f, "%63s %lld", value, &period); fclose(f);
    if (parsed != 2 || strcmp(value, "max") == 0) return 0;
    char *end = NULL; errno = 0; quota = strtoll(value, &end, 10);
    return !errno && end && *end == 0 ? sk_quota_budget(quota, period) : 0;
  }
  int parsed = fscanf(f, "%lld", &quota); fclose(f);
  n = snprintf(path, sizeof(path), "%s/cpu.cfs_period_us", directory);
  if (parsed != 1 || n < 0 || (size_t)n >= sizeof(path)) return 0;
  f = fopen(path, "r"); if (!f) return 0;
  parsed = fscanf(f, "%lld", &period); fclose(f);
  return parsed == 1 ? sk_quota_budget(quota, period) : 0;
}
static int sk_walk_cpu_limits(const char *mountpoint, const char *mountroot,
                              const char *member, int unified)
{
  if (!sk_safe_member(member) || !sk_safe_member(mountroot) || mountpoint[0] != '/') return 0;
  size_t rootlen = strlen(mountroot), floorlen = strlen(mountpoint);
  const char *relative = member;
  if (strcmp(mountroot, "/") != 0 && strncmp(member, mountroot, rootlen) == 0 &&
      (member[rootlen] == '/' || member[rootlen] == 0)) relative = member+rootlen;
  /* In a private cgroup namespace, membership is already relative to the mount. */
  while (*relative == '/') ++relative;
  char current[PATH_MAX];
  int n = snprintf(current, sizeof(current), "%s%s%s", mountpoint, *relative ? "/" : "", relative);
  if (n < 0 || (size_t)n >= sizeof(current)) return 0;
  int budget = 0;
  for (;;) {
    budget = sk_min_budget(budget, sk_cpu_limit_at(current, unified));
    if (strlen(current) <= floorlen) break;
    char *slash = strrchr(current, '/');
    if (!slash || (size_t)(slash-current) < floorlen) break;
    *slash = 0;
  }
  return budget;
}
static int sk_cgroup_budget_from_files(const char *membership_file, const char *mountinfo_file)
{
  char unified[PATH_MAX] = {0}, legacy[PATH_MAX] = {0};
  char line[PATH_MAX*3+512]; FILE *f = fopen(membership_file, "r");
  if (!f) return 0;
  while (fgets(line, sizeof(line), f)) {
    char *first = strchr(line, ':'), *second = first ? strchr(first+1, ':') : NULL;
    if (!second) continue;
    *second = 0; char *path = second+1; path[strcspn(path, "\r\n")] = 0;
    if (!sk_safe_member(path)) continue;
    char *dest = second == first+1 ? unified : (sk_controller(first+1, "cpu") ? legacy : NULL);
    if (dest && strlen(path) < PATH_MAX) memcpy(dest, path, strlen(path)+1);
  }
  fclose(f); f = fopen(mountinfo_file, "r"); if (!f) return 0;
  int budget = 0;
  while (fgets(line, sizeof(line), f)) {
    char *separator = strstr(line, " - "); if (!separator) continue;
    *separator = 0; char *right_save = NULL, *left_save = NULL;
    char *fs = strtok_r(separator+3, " ", &right_save);
    char *source = strtok_r(NULL, " ", &right_save);
    char *options = strtok_r(NULL, " \r\n", &right_save);
    if (!fs || !source || !options) continue;
    int v2 = strcmp(fs, "cgroup2") == 0;
    if ((!v2 && (strcmp(fs, "cgroup") != 0 || !sk_controller(options, "cpu"))) ||
        !(v2 ? unified[0] : legacy[0])) continue;
    char *rawroot = NULL, *rawmount = NULL, *token = strtok_r(line, " ", &left_save);
    for (int i = 0; token && i <= 4; ++i, token = strtok_r(NULL, " ", &left_save)) {
      if (i == 3) rawroot = token;
      if (i == 4) rawmount = token;
    }
    char root[PATH_MAX], mount[PATH_MAX];
    if (!rawroot || !rawmount || !sk_mount_unescape(rawroot, root, sizeof(root)) ||
        !sk_mount_unescape(rawmount, mount, sizeof(mount))) continue;
    size_t len = strlen(mount); while (len > 1 && mount[len-1] == '/') mount[--len] = 0;
    budget = sk_min_budget(budget, sk_walk_cpu_limits(mount, root, v2 ? unified : legacy, v2));
  }
  fclose(f); return budget;
}
static int sk_cgroup_budget(void)
{
  const char *membership = "/proc/thread-self/cgroup";
  const char *mountinfo = "/proc/thread-self/mountinfo";
  FILE *probe = fopen(membership, "r");
  if (probe) fclose(probe); else membership = "/proc/self/cgroup";
  probe = fopen(mountinfo, "r");
  if (probe) fclose(probe); else mountinfo = "/proc/self/mountinfo";
  int budget = sk_cgroup_budget_from_files(membership, mountinfo);
  if (!budget) {
    budget = sk_cpu_limit_at("/sys/fs/cgroup", 1);
    budget = sk_min_budget(budget, sk_cpu_limit_at("/sys/fs/cgroup/cpu", 0));
  }
  return budget;
}
static int sk_compact_mask(const cpu_set_t *allowed, int budget, cpu_set_t *chosen)
{
  int available = CPU_COUNT(allowed);
  if (budget <= 0 || budget >= available) return 0;
  CPU_ZERO(chosen); int added = 0;
  for (int cpu = 0; cpu < CPU_SETSIZE && added < budget; ++cpu)
    if (CPU_ISSET(cpu, allowed)) { CPU_SET(cpu, chosen); ++added; }
  return added == budget;
}
static void sk_bound_omp(sk_cpu_scope *scope, int capacity)
{
  if (capacity > 0 && omp_get_max_threads && omp_set_num_threads) {
    scope->omp_previous = omp_get_max_threads();
    if (scope->omp_previous > capacity) {
      omp_set_num_threads(capacity); scope->omp_changed = 1;
    }
  }
}
static sk_cpu_scope sk_compact_begin(void)
{
  sk_cpu_scope scope; memset(&scope, 0, sizeof(scope)); cpu_set_t chosen;
  int budget = sk_cgroup_budget(), capacity = budget;
  if (sched_getaffinity(0, sizeof(scope.previous), &scope.previous) == 0) {
    int available = CPU_COUNT(&scope.previous);
    capacity = sk_min_budget(available, budget);
    if (sk_compact_mask(&scope.previous, budget, &chosen) &&
        sched_setaffinity(0, sizeof(chosen), &chosen) == 0) scope.changed = 1;
  }
  sk_bound_omp(&scope, capacity);
  return scope;
}
static void sk_compact_end(sk_cpu_scope *scope)
{
  if (scope->omp_changed && omp_set_num_threads) omp_set_num_threads(scope->omp_previous);
  if (scope->changed && sched_setaffinity(0, sizeof(scope->previous), &scope->previous) != 0)
    fprintf(stderr, "[seekdb_cuvs] warning: failed to restore build-thread CPU affinity\n");
}
#endif
