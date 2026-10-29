/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
 */
#define USING_LOG_PREFIX SQL_RESV
#include "share/ob_server_struct.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "sql/ai_column/ai_column.h"
#include "sql/parser/ob_parser.h"
#include "sql/resolver/dml/ob_dml_resolver.h"
#include "sql/resolver/dml/ob_select_resolver.h"

namespace oceanbase {
namespace sql {
using namespace common;
using namespace share::schema;
namespace {
std::string catalog_identifier(const ObString &s) {
  std::string out = "`";
  for (int64_t i = 0; i < s.length(); ++i) {
    out += s.ptr()[i];
    if (s.ptr()[i] == '`') {
      out += '`';
    }
  }
  return out + '`';
}
std::string catalog_literal(const std::string &s) {
  constexpr char hex[] = "0123456789abcdef";
  std::string out = "CONVERT(X'";
  for (unsigned char c : s) {
    out += hex[c >> 4];
    out += hex[c & 15];
  }
  return out + "' USING utf8mb4)";
}
std::string catalog_name(const std::string &s) {
  return catalog_identifier(ObString(s.size(), s.data()));
}
std::string catalog_rowkey(const ObTableSchema &table) {
  std::string result = "CONCAT(";
  const auto &keys = table.get_rowkey_info();
  for (int64_t i = 0; i < keys.get_size(); ++i) {
    uint64_t id = OB_INVALID_ID;
    if (keys.get_column_id(i, id) != OB_SUCCESS) {
      return "NULL";
    }
    const auto *column = table.get_column_schema(id);
    if (i) {
      result += "," + catalog_literal(" AND ") + ",";
    }
    const auto name = catalog_identifier(column->get_column_name_str());
    result += catalog_literal(name + " = ") + ",";
    if (ob_is_integer_type(column->get_data_type()) ||
        ob_is_numeric_type(column->get_data_type())) {
      result += "CAST(" + name + " AS CHAR)";
    } else {
      result += "QUOTE(" + name + ")";
    }
  }
  return result + ")";
}
} // namespace
int bind_ai_column_catalog(ObDMLResolver &resolver, bool tasks,
                           const ObString &alias, TableItem *&item) {
  int ret = OB_SUCCESS;
  ObSchemaGetterGuard guard;
  ObSEArray<const ObTableSchema *, 64> tables;
  std::string query;
  OZ(GCTX.schema_service_->get_runtime_schema_guard(guard));
  OZ(guard.get_table_schemas_in_runtime(tables));
  for (int64_t i = 0; OB_SUCC(ret) && i < tables.count(); ++i) {
    const auto *table = tables.at(i);
    if (table->is_view_table() || table->is_index_table() ||
        table->is_sys_table()) {
      continue;
    }
    const ObDatabaseSchema *database = nullptr;
    OZ(guard.get_database_schema(table->get_database_id(), database));
    if (OB_SUCC(ret) && database == nullptr) {
      ret = OB_ERR_UNEXPECTED;
    }
    for (auto c = table->column_begin();
         OB_SUCC(ret) && c != table->column_end(); ++c) {
      const auto *column = *c;
      if (!column->is_ai_column()) {
        continue;
      }
      AiColumnDefinition d;
      OZ(load_ai_column_definition(*table, *column, *resolver.allocator_, d));
      const auto *state = table->get_column_schema(
          ObString(d.state_name_.size(), d.state_name_.data()));
      const auto *request = table->get_column_schema(
          ObString(d.request_name_.size(), d.request_name_.data()));
      const auto *spec = table->get_column_schema(
          ObString(d.spec_name_.size(), d.spec_name_.data()));
      if (state == nullptr || request == nullptr || spec == nullptr) {
        ret = OB_INVALID_DATA;
      }
      if (OB_SUCC(ret)) {
        if (!query.empty()) {
          query += " UNION ALL ";
        }
        query += "SELECT /*+ OPT_PARAM('hidden_column_visible', 'true') */ " +
                 std::to_string(table->get_table_id()) + " AS table_id," +
                 std::to_string(column->get_column_id()) + " AS column_id,";
        if (!tasks) {
          query += catalog_literal(
                       std::string(column->get_column_name_str().ptr(),
                                   column->get_column_name_str().length())) +
                   " AS column_name,";
          query +=
              std::to_string(state->get_column_id()) + " AS state_column_id," +
              std::to_string(request->get_column_id()) +
              " AS request_column_id," + std::to_string(spec->get_column_id()) +
              " AS result_spec_column_id,";
          query += std::to_string(d.max_retries_) + " AS max_retries," +
                   catalog_literal(d.normalized_expr_) + " AS normalized_expr";
        } else {
          const auto req = catalog_name(d.request_name_),
                     status = catalog_name(d.state_name_);
          const auto json = [&](const char *field) {
            return "JSON_UNQUOTE(JSON_EXTRACT(" + req + ", '$." + field + "'))";
          };
          query += "CAST(" + json("id") + " AS CHAR(64)) AS request_id," +
                   catalog_rowkey(*table) + " AS rowkey_predicate,";
          query += "COALESCE(CAST(" + json("eligible") +
                   " AS DATETIME(6)),NOW(6)) AS eligible_ts,";
          query += "COALESCE(CAST(" + json("state") +
                   " AS SIGNED),0) AS state,COALESCE(CAST(" + json("retry") +
                   " AS SIGNED),0) AS retry_count";
          query += " FROM " +
                   catalog_identifier(database->get_database_name_str()) + "." +
                   catalog_identifier(table->get_table_name_str()) + " WHERE " +
                   status + "='PENDING'";
        }
      }
    }
  }
  if (OB_SUCC(ret) && query.empty()) {
    query =
        tasks
            ? "SELECT CAST(NULL AS UNSIGNED) AS table_id,CAST(NULL AS "
              "UNSIGNED) AS column_id,CAST(NULL AS CHAR(64)) AS "
              "request_id,CAST(NULL AS CHAR(512)) AS rowkey_predicate,NOW(6) "
              "AS eligible_ts,0 AS state,0 AS retry_count WHERE 0"
            : "SELECT CAST(NULL AS UNSIGNED) AS table_id,CAST(NULL AS "
              "UNSIGNED) AS column_id,CAST(NULL AS CHAR(64)) AS "
              "column_name,CAST(NULL AS UNSIGNED) AS state_column_id,CAST(NULL "
              "AS UNSIGNED) AS request_column_id,CAST(NULL AS UNSIGNED) AS "
              "result_spec_column_id,0 AS max_retries,CAST(NULL AS CHAR(512)) "
              "AS normalized_expr WHERE 0";
  }
  if (OB_SUCC(ret)) {
    auto &hint =
        resolver.get_stmt()->get_query_ctx()->get_query_hint_for_update();
    const_cast<ObGlobalHint &>(hint.get_global_hint())
        .merge_plan_cache_hint(OB_USE_PLAN_CACHE_NONE);
    ObParser parser(*resolver.allocator_,
                    resolver.session_info_->get_sql_mode(),
                    resolver.session_info_->get_charsets4parser());
    ParseResult parsed;
    OZ(parser.parse(ObString(query.size(), query.data()), parsed));
    ObResolverParams params = resolver.params_;
    ObSelectResolver binder(params);
    if (OB_SUCC(ret)) {
      OZ(binder.resolve(*parsed.result_tree_->children_[0]));
    }
    OZ(resolver.resolve_generate_table_item(binder.get_select_stmt(), alias,
                                            item));
  }
  return ret;
}
} // namespace sql
} // namespace oceanbase
