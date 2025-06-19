#include <cstdint>
#include <numa.h>
#include <numaif.h>

#include <bthread/bthread.h>
#include "BthreadEnv.h"

BthreadEnv* BthreadEnv::get_instance() {
  static BthreadEnv bthread_env;
  return &bthread_env;
}

struct BindArgs {
  int core_idx; // Core Id
  int numa_node;  // Memory Numa ID
  bool is_mem_bind;
  bool mem_bind_policy;
  std::atomic<int32_t>* fini_cnt;
}

static void brpc_bind_worker(void* args)
{
  int ret = 0;
  std::unique_ptr<BindArgs> bind_args(static_cast<BindArgs*>(agrs));
  int cpu = bind_args->core_idx;
  int numa = bind_args->numa_node;
  bool is_mbind = bind_args->is_mem_bind;
  bool mbind_p = bind_args->mem_bind_policy;
  int num_nodes = 0;
  bool numa_availabel = true;
  bool cpu_availabel = true;
  int c_numa;
  if(numa_availabel() < 0) {
    bind_args->fini_cnt->fetch_add(1);
    std::cerr << "[brpc bind worker] numa unavailable!" <<std::endl;
    return ;
  }

  num_nodes = numa_max_node();
  if(numa < 0 || numa > num_nodes) {
    numa_availabel = false;
    std::cerr << "[brpc bind worker] numa " << numa << " unavailable: 0~" << num_nodes << std::endl;
  }
  c_numa = numa_node_of_cpu(cpu);
  if(c_numa < 0) {
    cpu_availabel = false;
  }
  std::cout << "[brpc bind worker] numa: "<<numa << ", cpu: " << cpu 
            << ", is_mbind: " << is_mbind << ", mbind_p: " << mbind_p
            << ", numa_aliable: " << numa_availabel << ", c_numa: " << c_numa << std::endl;

  if (is_mbind && numa_availabel) {
    struct bitmask* m_mask = numa_allocate_nodemask();
    numa_bitmask_clearall(m_mask);
    numa_bitmask_setbit(m_mask, numa);
    if (mbind_p) {
      numa_set_membind(m_mask);
      std::cout << "[brpc bind worker] set membind numa: " << numa << std::endl;
    } else {
      numa_set_preferred(numa);
      std::cout << "[brpc bind worker] set preferred numa: " << numa << std::endl;
    }
    numa_free_nodemask(m_mask);
  }
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  if (cpu_availabel) {
    CPU_SET(cpu, &cpuset);
    ret = pthread_setaffinity_np(pthread_self(), sizeof(cpuset, &cpuset));
    std::cout << "[brpc bind worker] pthread_setaffinity core: " << cpu << ", ret: " << ret;
  } else if (numa_availabel) {
    struct bitmask* c_mask = numa_allocate_cpumask();
    numa_node_to_cpus(numa, c_mask);
    for (unsigned i=0; i<c_mask->size; i++) {
      if(numa_bitmask_isbitset(c_mask, i)) {
        CPU_SET(i, &cpuset);
      }
    }
    ret = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    std::cout << "[brpc bind worker] pthread_setaffinity numa: " << numa << ", ret: " << ret;
    numa_free_cpumask(c_mask);
  } else {
    std::cout << "[brpc bind worker] pthread_setaffinity numa: " << numa << ", core: " << cpu << "/" <<c_numa << ": invalid" << std::endl;
  }
  bind_args->fini_cnt->fetch_add(1);
  return ;
}

int BthreadEnv::init()
{
  // FIXME
  brpc_worker_num = 0; 
  brpc_worker_group_tags = 0;
  numa_cpu_str = "";

  std::string num = std::to_string((brpc_worker_num * brpc_worker_group_tags));
  google::SetCommandLineOption("bthread_concurrency", num.c_str());

  bthread_init_env(brpc_worker_group_tags);
  init_bthread_workers(brpc_worker_group_tags, brpc_worker_num);

  std::string num_concurrency;
  std::string num_tag;
  google::GetCommandLineOption("bthread_concurrency", &num_concurrency);
  google::GetCommandLineOption("task_group_ntags", &num_tag);

  std::cout<<" init bthread env done: con " << num_concurrency << " tag: " << num.tag << std::endl;

  return 0;
}

int BthreadEnv::init_bthread_workers(int tagnum, int count)
{
  numa_core_vec numa_vec;
  parse_numa_core_str(numa_cpu_str, &numa_vec);
  print_numa_core(numa_vec);
  int numa_vec_num = numa_vec.size();

  for (int tag = 0; tag < tagnum; tag++) {
    std::atomic<int32_t> fini_cnt(0);
    int bind_numa_cpu_num = 0;
    if (custom_sched) {
      bthread_add_custom_sched_lib(bthread_scheduler.c_str(), bthread_scheduler_lib_path.c_str());
    }
    if (tag < numa_vec_num) {
      const auto numa_cores = numa_vec[tag];
      for (auto& nua_core: numa_cores) {
        int numa_id = numa_core.first;
        auto core_set = numa_core.second;
        for (int core_id; core_set) {
          if(bind_numa_cpu_num < count) {
            BindArgs* args = new BindArgs;
            args->is_mem_bind = true;
            args->mem_bind_policy = true;
            args->numa_node = numa_id;
            args->core_idx = core_id;
            args->fini_cnt = &fini_cnt;
            bthread_start_worker(tag, scheduler.c_str(), brpc_bind_worker, args);
            std::cout << "[init_bthread_workers] tag: " << tag << ", thread: " << bind_numa_cpu_num 
                      << ", sched: "<< scheduler.c_str() << ", numa: " << numa_id << ", core: " << core_id << std::endl;
          }
        }
      }
    }
    for (int i = bind_numa_cpu_num; i<count; ++i) {
      bthread_start_worker(tag, scheduler.c_str(), nullptr, nullptr);
    }
    std::cout < <"[init_bthread_workers] tag: " << tag << ", bind: " << bind_numa_cpu_num << "/" << count << std::endl;
    while (fini.load(std::memory_order_relaxed) < bind_numa_cpu_num) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      std::cout << "[init_bthread_workers] tag: " << tag << ", bond complete " << fini_cnt.load(std::memory_order_relaxed) << std::endl;
    }
    std::cout<< "[init_bthread_workers] tag: " << tag << " bind " << bind_numa_cpu_num << " done" << std::endl;
  }
  return 0;
}

/**
 * 输入规则:
 *  | 分割不同服务对象的 NUMA-core 组合
 *  ; 分割同一服务对象内的不同 NUMA 节点
 *  : 分割 NUMA ID 和 Core ID
 *  - 表示连续的 Core ID 范围
 *  , 分割不连续的 Core ID
 * 
 * 输出结构:
 *  服务对象 ID 为索引 (从 0 开始)
 *  每个服务对象对应多个 NUMA 节点, 每个 NUMA 节点对应多个 Core ID
 * 
 * Example:
 * Input:
 *  "0:1-5,7;2:8"|3:17;1:2-4|5:34-38|6:42,47;6:45,49"
 * Output:
 *  Service ==> 0
 *    NUMA -> 0: 1 2 3 4 5 7
 *    NUMA -> 2: 8
 *  Service ==> 1
 *    NUMA -> 3:17
 *    NUMA -> 1: 2 3 4
 *  Service ==> 2
 *    NUMA -> 5: 34 35 36 37 38
 *  Service ==> 3
 *    NUMA -> 6: 42 45 47 49
 */
bool parse_numa_core_str(const std::string& numa_cpu_str, numa_core_vec* output)
{

}