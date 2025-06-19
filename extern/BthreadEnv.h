#pragma once

#include <vector>
#include <map>
#include <set>
#include <string>

typedef std::vector<std::map<int, std::set<int>>> numa_core_vec;

class ChunkBthreadEnv {
public:
  ChunkBthreadEnv(){};
  ~ChunkBthreadEnv(){};

  static ChunkBthreadEnv* get_instance();
  int init();
  int init_bthread_worker(int tagnum, int count);

  // core, numa
  int brpc_worker_num;
  int brpc_worker_group_tags;
  std::string scheduler;
  std::string scheduler_lib_path;
  std::string numa_cpu_str;

  // scheduler
  bool custom_sched = false;
  std::string bthread_scheduler;
  std::string bthread_scheduler_lib_path;
}