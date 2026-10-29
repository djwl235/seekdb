/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
 */
#define USING_LOG_PREFIX SQL_ENG
#include "common/mysqlclient/ob_isql_client.h"
#include "query/session/ob_inner_sql_connection_access.h"
#include "share/ob_server_struct.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "sql/ai_column/ai_column.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/resolver/dml/ob_update_stmt.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include <set>
namespace oceanbase {
namespace sql {
using namespace common;
using namespace share::schema;
namespace {
std::string read_identifier(const ObString &s) {
  std::string out = "`";
  for (int64_t i = 0; i < s.length(); ++i) {
    out += s.ptr()[i];
    if (s.ptr()[i] == '`')
      out += '`';
  }
  return out + '`';
}
} // namespace
int collect_ai_read_columns(const ObDMLStmt &stmt, ObSchemaGetterGuard &guard,
                            ObIArray<uint64_t> &ids) {
  int ret = OB_SUCCESS;
  ObSEArray<ObRawExpr *, 32> expressions;
  if (stmt.is_select_stmt()) {
    OZ(stmt.get_relation_exprs(expressions));
  } else {
    OZ(append(expressions, stmt.get_condition_exprs()));
    if (stmt.is_update_stmt()) {
      const auto &assignments =
          static_cast<const ObUpdateStmt &>(stmt).get_update_table_info();
      for (int64_t i = 0; OB_SUCC(ret) && i < assignments.count(); ++i) {
        const auto &list = assignments.at(i)->assignments_;
        for (int64_t j = 0; OB_SUCC(ret) && j < list.count(); ++j) {
          if (!list.at(j).is_implicit_)
            OZ(expressions.push_back(list.at(j).expr_));
        }
      }
    }
  }
  ObSEArray<ObRawExpr *, 16> columns;
  OZ(ObRawExprUtils::extract_column_exprs(expressions, columns));
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    const auto *ref = static_cast<const ObColumnRefRawExpr *>(columns.at(i));
    const auto *item = stmt.get_table_item_by_id(ref->get_table_id());
    if (item == nullptr || !item->is_basic_table())
      continue;
    const ObTableSchema *table = nullptr;
    OZ(guard.get_table_schema(item->ref_id_, table));
    const auto *column = table == nullptr
                             ? nullptr
                             : table->get_column_schema(ref->get_column_id());
    if (column != nullptr && column->is_ai_column()) {
      bool found = false;
      for (int64_t j = 0; j + 1 < ids.count(); j += 2)
        found |=
            ids.at(j) == item->ref_id_ && ids.at(j + 1) == ref->get_column_id();
      if (!found) {
        OZ(ids.push_back(item->ref_id_));
        OZ(ids.push_back(ref->get_column_id()));
      }
    }
  }
  ObSEArray<ObSelectStmt *, 8> children;
  OZ(stmt.get_child_stmts(children));
  for (int64_t i = 0; OB_SUCC(ret) && i < children.count(); ++i)
    OZ(SMART_CALL(collect_ai_read_columns(*children.at(i), guard, ids)));
  return ret;
}
int check_ai_read_columns(ObExecContext &ctx, const ObIArray<uint64_t> &ids) {
  int ret = OB_SUCCESS;
  bool strict = false;
  auto *session = ctx.get_my_session();
  if (session == nullptr || !session->is_user_session() || ids.empty())
    return ret;
  OZ(session->get_sys_variable(share::SYS_VAR_AI_COLUMN_STRICT_READ, strict));
  if (OB_FAIL(ret) || !strict)
    return ret;
  ObSchemaGetterGuard guard;
  OZ(GCTX.schema_service_->get_runtime_schema_guard(guard));
  sqlclient::ObISQLConnectionGuard connection;
  // SPI borrows the caller's session and transaction, so pending writes and
  // rollback have the same visibility as the statement being guarded.
  OZ(query::ObInnerSQLConnectionAccess::
         create_spi_connection_with_external_session(session, connection));
  for (int64_t i = 0; OB_SUCC(ret) && i + 1 < ids.count(); i += 2) {
    const ObTableSchema *table = nullptr;
    const ObDatabaseSchema *db = nullptr;
    OZ(guard.get_table_schema(ids.at(i), table));
    if (table == nullptr) {
      ret = OB_SCHEMA_ERROR;
      break;
    }
    const auto *column = table->get_column_schema(ids.at(i + 1));
    if (column == nullptr || !column->is_ai_column()) {
      ret = OB_SCHEMA_ERROR;
      break;
    }
    OZ(guard.get_database_schema(table->get_database_id(), db));
    if (db == nullptr) {
      ret = OB_SCHEMA_ERROR;
      break;
    }
    const auto hidden =
        ai_column_hidden_name(column->get_column_name_str(), 's');
    const std::string query =
        "SELECT /*+ OPT_PARAM('hidden_column_visible','true') */ 1 FROM " +
        read_identifier(db->get_database_name_str()) + "." +
        read_identifier(table->get_table_name_str()) + " WHERE COALESCE(" +
        read_identifier(ObString(hidden.size(), hidden.data())) +
        ",'PENDING') <> 'READY' LIMIT 1";
    ObISQLClient::ReadResult result;
    OZ(connection.get_ptr()->execute_read(ObString(query.size(), query.data()),
                                          result));
    if (OB_SUCC(ret) && result.get_result() == nullptr)
      ret = OB_ERR_UNEXPECTED;
    if (OB_SUCC(ret)) {
      const int next = result.get_result()->next();
      if (next == OB_SUCCESS) {
        ret = OB_AI_COLUMN_NOT_READY;
        LOG_USER_ERROR(OB_AI_COLUMN_NOT_READY, ids.at(i), ids.at(i + 1));
      } else if (next != OB_ITER_END)
        ret = next;
    }
  }
  return ret;
}
} // namespace sql
} // namespace oceanbase
