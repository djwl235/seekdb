/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
 */
#pragma once
#include "share/schema/ob_table_schema.h"
#include "sql/parser/parse_node.h"
#include <string>
#include <vector>
namespace oceanbase {
namespace share {
namespace schema {
struct AlterTableSchema;
}
} // namespace share
} // namespace oceanbase
namespace oceanbase {
namespace sql {
struct ObResolverParams;
class ObDMLResolver;
class ObDMLStmt;
class ObStmt;
class ObExecContext;
struct TableItem;
struct ObTableAssignment;
class ObInsertTableInfo;
struct AiColumnDefinition {
  std::string model_sql_, prompt_sql_, config_sql_, normalized_expr_,
      instruction_;
  std::string state_name_, request_name_, spec_name_;
  std::vector<std::string> dependencies_;
  int64_t max_retries_ = 3;
  int64_t dimension_ = 0;
  bool embedding_ = false;
};
int resolve_ai_column_attribute(share::schema::ObColumnSchemaV2 &,
                                const ParseNode &, ObResolverParams &);
int prepare_ai_columns(share::schema::ObTableSchema &, common::ObIAllocator &,
                       const share::schema::ObTableSchema *original = nullptr);
int prepare_ai_alter_columns(share::schema::AlterTableSchema &,
                             const share::schema::ObTableSchema &,
                             common::ObIAllocator &);
int load_ai_column_definition(const share::schema::ObTableSchema &,
                              const share::schema::ObColumnSchemaV2 &,
                              common::ObIAllocator &, AiColumnDefinition &);
std::string ai_column_hidden_name(const common::ObString &, char kind);
int bind_ai_column_catalog(ObDMLResolver &, bool tasks,
                           const common::ObString &, TableItem *&);
int resolve_ai_retry(ObResolverParams &, const ParseNode &, ObStmt *&);
int collect_ai_read_columns(const ObDMLStmt &,
                            share::schema::ObSchemaGetterGuard &,
                            common::ObIArray<uint64_t> &);
int check_ai_read_columns(ObExecContext &, const common::ObIArray<uint64_t> &);
int apply_ai_insert_defaults(ObDMLResolver &, ObInsertTableInfo &);
int apply_ai_update_assignments(ObDMLResolver &, const TableItem &,
                                const share::schema::ObTableSchema &,
                                ObTableAssignment &);
} // namespace sql
} // namespace oceanbase
