/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
 */
#pragma once
#include "sql/table_function/table_function.h"
namespace oceanbase {
namespace sql {
struct ObSelectIntoItem;
constexpr uint64_t READ_CSV_FUNCTION_ID = 5;
constexpr uint64_t READ_JSON_FUNCTION_ID = 6;
constexpr uint64_t READ_PARQUET_FUNCTION_ID = 7;
constexpr uint64_t GLOB_FUNCTION_ID = 8;
int bind_file_table_function(ObDMLResolver &, const TableFunctionCall &,
                             TableItem *&);
int create_file_table_scan(const TableFunctionScanContext &,
                           ITableFunctionScan *&);
int resolve_file_copy_options(ObDMLResolver &, const _ParseNode &,
                              common::ObString &);
int add_file_copy_names(common::ObIAllocator &, const common::ObString &,
                        const common::ObIArray<common::ObString> &,
                        common::ObString &);
class ICopyFileWriter {
public:
  virtual ~ICopyFileWriter() = default;
  virtual int append(const common::ObIArray<common::ObObj> &) = 0;
  virtual int finish() = 0;
};
int create_copy_file_writer(ObExecContext &, const common::ObString &,
                            const common::ObString &, ICopyFileWriter *&);
} // namespace sql
} // namespace oceanbase
