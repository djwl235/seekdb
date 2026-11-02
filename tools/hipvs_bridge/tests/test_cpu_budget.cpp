#include "../sk_cpu_affinity.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
static thread_local int fake_omp = 128;
extern "C" int omp_get_max_threads(void) { return fake_omp; }
extern "C" void omp_set_num_threads(int n) { assert(n > 0); fake_omp = n; }
namespace fs = std::filesystem;
static void write(const fs::path &p, const std::string &s) {
  fs::create_directories(p.parent_path()); std::ofstream f(p); f << s; assert(f.good());
}
static std::string escape(const fs::path &p) {
  std::string out; for (char c : p.string()) out += c == ' ' ? "\\040" : std::string(1,c); return out;
}
static int budget(const fs::path &root, const std::string &membership,
                  const std::string &mountroot, bool unified) {
  write(root/"self", membership);
  std::string line = "1 0 0:1 " + mountroot + " " + escape(root/"cg space") +
      " rw - " + (unified ? "cgroup2 cgroup rw\n" : "cgroup cgroup rw,cpu,cpuacct\n");
  write(root/"mountinfo", line);
  return sk_cgroup_budget_from_files((root/"self").c_str(), (root/"mountinfo").c_str());
}
int main() {
  assert(sk_quota_budget(2400000,100000)==24);
  assert(sk_quota_budget(50000,100000)==1);
  assert(sk_quota_budget(-1,100000)==0 && sk_quota_budget(1,0)==0);
  assert(sk_controller("cpu,cpuacct","cpu") && !sk_controller("cpuacct,cpuset","cpu"));
  char pattern[]="/var/tmp/seekdb-cpu-budget-XXXXXX"; char *made=mkdtemp(pattern); assert(made);
  fs::path root(made), cg=root/"cg space";
  write(cg/"cpu.max","max 100000\n");
  write(cg/"service/cpu.max","2400000 100000\n");
  write(cg/"service/job/cpu.max","max 100000\n");
  assert(budget(root,"0::/service/job\n","/",true)==24);
  write(cg/"cpu.max","1600000 100000\n");
  assert(budget(root,"0::/service/job\n","/",true)==16);
  write(cg/"service/job/cpu.max","800000 100000\n");
  assert(budget(root,"0::/service/job\n","/",true)==8);
  write(cg/"cpu.max","max 100000\n");
  assert(budget(root,"0::/pods/contest/service/job\n","/pods/contest",true)==8);
  write(cg/"cpu.max","600000 100000\n");
  assert(budget(root,"0::/\n","/pods/contest",true)==6);
  write(cg/"cpu.max","max 100000\n");
  write(cg/"service/job/cpu.max","oops 100000\n");
  write(cg/"service/cpu.max","max 100000\n");
  assert(budget(root,"0::/service/job\n","/",true)==0);
  assert(budget(root,"0::/../escape\n","/",true)==0);
  write(cg/"cpu.cfs_quota_us","1600000\n");write(cg/"cpu.cfs_period_us","100000\n");
  write(cg/"service/job/cpu.cfs_quota_us","2400000\n");write(cg/"service/job/cpu.cfs_period_us","100000\n");
  assert(budget(root,"5:cpu,cpuacct:/service/job\n","/",false)==16);
  write(cg/"cpu.cfs_quota_us","-1\n");
  assert(budget(root,"5:cpu,cpuacct:/service/job\n","/",false)==24);
  sk_cpu_scope scope{}; fake_omp=128; sk_bound_omp(&scope,16);
  assert(fake_omp==16 && scope.omp_previous==128 && scope.omp_changed);
  sk_compact_end(&scope);assert(fake_omp==128);
  scope={}; fake_omp=8;sk_bound_omp(&scope,16);assert(fake_omp==8 && !scope.omp_changed);
  cpu_set_t before,after; assert(sched_getaffinity(0,sizeof(before),&before)==0);
  fake_omp=128;scope=sk_compact_begin();assert(fake_omp<=CPU_COUNT(&before));
  sk_compact_end(&scope);assert(fake_omp==128);
  assert(sched_getaffinity(0,sizeof(after),&after)==0 && CPU_EQUAL(&before,&after));
  assert(root.string().find("/var/tmp/seekdb-cpu-budget-")==0);fs::remove_all(root);
  puts("CPU budget: v2 ancestors/namespaces, combined v1, malformed limits, OpenMP ICV and affinity restore PASS");
}
