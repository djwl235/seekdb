/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
 */
#define USING_LOG_PREFIX SQL_RESV
#include "sql/ai_column/ai_column.h"
#include "sql/parser/ob_parser.h"
#include "sql/resolver/ob_resolver.h"
#include "sql/resolver/ob_schema_checker.h"
#include "sql/session/ob_sql_session_info.h"
namespace oceanbase {
namespace sql {
using namespace common;
using namespace share::schema;
namespace {
std::string retry_name(const ObString &s) {
  std::string out = "`";
  for (int64_t i = 0; i < s.length(); ++i) {
    out += s.ptr()[i];
    if (s.ptr()[i] == '`')
      out += '`';
  }
  return out + '`';
}
std::string retry_literal(const ObString &s) {
  constexpr char hex[] = "0123456789abcdef";
  std::string out = "CONVERT(X'";
  for (int64_t i = 0; i < s.length(); ++i) {
    unsigned char c = s.ptr()[i];
    out += hex[c >> 4];
    out += hex[c & 15];
  }
  return out + "' USING utf8mb4)";
}
int replace_retry_status(ParseNode &node, const ObString &state,
                         ObIAllocator &allocator) {
  int ret = OB_SUCCESS;
  if (node.type_ == T_COLUMN_REF && node.num_child_ == 3 &&
      node.children_[0] == nullptr && node.children_[1] == nullptr &&
      node.children_[2] != nullptr) {
    auto *id = node.children_[2];
    if (!id->is_input_quoted_ &&
        ObString(id->str_len_, id->str_value_).case_compare("status") == 0) {
      ObString copy;
      OZ(ob_write_string(allocator, state, copy));
      if (OB_SUCC(ret)) {
        id->str_value_ = copy.ptr();
        id->str_len_ = copy.length();
      }
    }
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < node.num_child_; ++i)
      if (node.children_[i] != nullptr)
        OZ(SMART_CALL(
            replace_retry_status(*node.children_[i], state, allocator)));
  }
  return ret;
}
} // namespace
int resolve_ai_retry(ObResolverParams &params, const ParseNode &node,
                     ObStmt *&stmt) {
  int ret = OB_SUCCESS;
  if (node.num_child_ != 4 || node.children_[0] == nullptr ||
      node.children_[1] == nullptr)
    ret = OB_INVALID_ARGUMENT;
  else if (node.children_[3] != nullptr) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED,
                   "combining AI COLUMN RETRY with another ALTER action");
  }
  if (OB_FAIL(ret))
    return ret;
  const auto &relation = *node.children_[0];
  ObString database = params.session_info_->get_database_name();
  if (relation.children_[0] != nullptr)
    database.assign_ptr(relation.children_[0]->str_value_,
                        relation.children_[0]->str_len_);
  const ObString table_name(relation.children_[1]->str_len_,
                            relation.children_[1]->str_value_);
  const ObString column_name(node.children_[1]->str_len_,
                             node.children_[1]->str_value_);
  const ObTableSchema *table = nullptr;
  OZ(params.schema_checker_->get_table_schema(database, table_name, false,
                                              table));
  const auto *column =
      table == nullptr ? nullptr : table->get_column_schema(column_name);
  if (OB_SUCC(ret) && (column == nullptr || !column->is_ai_column())) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED,
                   "retrying a column that is not an AI column");
  }
  AiColumnDefinition d;
  if (OB_SUCC(ret))
    OZ(load_ai_column_definition(*table, *column, *params.allocator_, d));
  if (OB_FAIL(ret))
    return ret;
  const auto state =
      retry_name(ObString(d.state_name_.size(), d.state_name_.data()));
  const auto request =
      retry_name(ObString(d.request_name_.size(), d.request_name_.data()));
  const auto spec =
      retry_name(ObString(d.spec_name_.size(), d.spec_name_.data()));
  const auto *definition = table->get_column_schema(
      ObString(d.spec_name_.size(), d.spec_name_.data()));
  const std::string sql =
      "UPDATE /*+ OPT_PARAM('hidden_column_visible','true') "
      "USE_PLAN_CACHE(NONE) */ " +
      retry_name(database) + "." + retry_name(table_name) + " SET " +
      retry_name(column_name) + "=NULL," + state + "='PENDING'," + request +
      "=CAST(JSON_OBJECT('id',UUID(),'eligible',NOW(6),'state',0,'retry',0,'"
      "owner',NULL,'boot',NULL,'lease',NULL,'done',FALSE) AS CHAR)," +
      spec + "=" +
      retry_literal(definition->get_cur_default_value().get_string());
  ObParser parser(*params.allocator_, params.session_info_->get_sql_mode(),
                  params.session_info_->get_charsets4parser());
  ParseResult parsed;
  OZ(parser.parse(ObString(sql.size(), sql.data()), parsed));
  if (OB_SUCC(ret)) {
    ParseNode *update = parsed.result_tree_->children_[0];
    if (node.children_[2] != nullptr) {
      update->children_[3] = node.children_[2];
      OZ(replace_retry_status(
          *update->children_[3],
          ObString(d.state_name_.size(), d.state_name_.data()),
          *params.allocator_));
    }
    ObResolverParams nested = params;
    nested.is_ai_retry_ = true;
    ObResolver resolver(nested);
    OZ(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT, *update, stmt));
  }
  return ret;
}
} // namespace sql
} // namespace oceanbase
