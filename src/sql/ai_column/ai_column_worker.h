/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
 */
#pragma once
#include "share/ob_thread_pool.h"
#include <string>
namespace oceanbase {
namespace sql {
// Requests and intermediate results live in ordinary MVCC business rows. The
// worker owns no volatile queue; restart reconstructs work from committed rows.
class AiColumnWorker final : public share::ObThreadPool {
public:
  AiColumnWorker();
  void run1() override;

private:
  int poll();
  int process(uint64_t table_id, const std::string &request,
              const std::string &rowkey);
  std::string boot_;
};
} // namespace sql
} // namespace oceanbase
