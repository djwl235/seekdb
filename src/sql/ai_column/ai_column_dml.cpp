/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
 */
#define USING_LOG_PREFIX SQL_RESV
#include "sql/ai_column/ai_column.h"
#include "sql/parser/ob_parser.h"
#include "sql/resolver/dml/ob_del_upd_resolver.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"

namespace oceanbase {
namespace sql {
using namespace common;
using namespace share::schema;
namespace {
int make_ai_request_expr(ObDMLResolver &resolver, ObRawExpr *&expr) {
  int ret = OB_SUCCESS;
  const ObString sql(
      "SELECT "
      "CAST(JSON_OBJECT('id',UUID(),'eligible',NOW(6),'state',0,'retry',0,'"
      "owner',NULL,'boot',NULL,'lease',NULL,'done',FALSE) AS CHAR)");
  ObParser parser(*resolver.allocator_, resolver.session_info_->get_sql_mode(),
                  resolver.session_info_->get_charsets4parser());
  ParseResult parsed;
  OZ(parser.parse(sql, parsed));
  if (OB_SUCC(ret)) {
    ParseNode *select = parsed.result_tree_->children_[0];
    ParseNode *projection =
        select->children_[PARSE_SELECT_SELECT]->children_[0];
    OZ(resolver.resolve_sql_expr(*projection->children_[0], expr));
    OZ(expr->deduce_type(resolver.session_info_));
    OZ(expr->extract_info());
  }
  return ret;
}
int ai_string_expr(ObDMLResolver &resolver, const ObString &text,
                   ObRawExpr *&expr) {
  int ret = OB_SUCCESS;
  ObString copy;
  ObConstRawExpr *constant = nullptr;
  OZ(ob_write_string(*resolver.allocator_, text, copy));
  OZ(ObRawExprUtils::build_const_string_expr(*resolver.params_.expr_factory_,
                                             ObVarcharType, copy,
                                             CS_TYPE_UTF8MB4_BIN, constant));
  expr = constant;
  return ret;
}
int ai_changed_expr(ObDMLResolver &resolver, const ObAssignment &assignment,
                    ObRawExpr *&expr) {
  int ret = OB_SUCCESS;
  ObOpRawExpr *equal = nullptr, *inverse = nullptr;
  OZ(resolver.params_.expr_factory_->create_raw_expr(T_OP_NSEQ, equal));
  OZ(equal->set_param_exprs(assignment.column_expr_, assignment.expr_));
  OZ(resolver.params_.expr_factory_->create_raw_expr(T_OP_NOT, inverse));
  OZ(inverse->init_param_exprs(1));
  OZ(inverse->add_param_expr(equal));
  expr = inverse;
  return ret;
}
int ai_append_conditional(ObDMLResolver &resolver,
                          ObTableAssignment &assignments,
                          const ObTableSchema &schema, uint64_t column_id,
                          ObRawExpr *condition, ObRawExpr *value) {
  int ret = OB_SUCCESS;
  ObColumnRefRawExpr *column = resolver.get_stmt()->get_column_expr_by_id(
      assignments.table_id_, column_id);
  if (column == nullptr) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ObAssignment assignment;
    assignment.column_expr_ = column;
    assignment.is_implicit_ = true;
    OZ(ObRawExprUtils::build_case_when_expr(*resolver.params_.expr_factory_,
                                            condition, value, column,
                                            assignment.expr_));
    OZ(assignments.assignments_.push_back(assignment));
  }
  UNUSED(schema);
  return ret;
}
} // namespace
int apply_ai_insert_defaults(ObDMLResolver &resolver,
                             ObInsertTableInfo &table_info) {
  int ret = OB_SUCCESS;
  const ObTableSchema *schema = nullptr;
  ObRawExpr *request = nullptr;
  if (!resolver.session_info_->is_user_session()) {
    return ret;
  }
  OZ(resolver.schema_checker_->get_table_schema(table_info.ref_table_id_,
                                                schema));
  for (int64_t i = 0; OB_SUCC(ret) && i < table_info.values_desc_.count();
       ++i) {
    const auto *column = schema->get_column_schema(
        table_info.values_desc_.at(i)->get_column_id());
    if (column != nullptr && column->is_ai_column()) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED,
                     "explicit writes to an AI result column");
    }
  }
  for (auto it = schema->column_begin();
       OB_SUCC(ret) && it != schema->column_end(); ++it) {
    const auto *column = *it;
    if (!column->is_ai_column()) {
      continue;
    }
    AiColumnDefinition d;
    OZ(load_ai_column_definition(*schema, *column, *resolver.allocator_, d));
    const auto *hidden = schema->get_column_schema(
        ObString(d.request_name_.size(), d.request_name_.data()));
    if (request == nullptr) {
      OZ(make_ai_request_expr(resolver, request));
    }
    if (OB_SUCC(ret) && hidden == nullptr) {
      ret = OB_INVALID_DATA;
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < table_info.column_exprs_.count();
         ++i) {
      if (table_info.column_exprs_.at(i)->get_column_id() ==
          hidden->get_column_id()) {
        table_info.column_conv_exprs_.at(i) = request;
      }
    }
  }
  return ret;
}
int apply_ai_update_assignments(ObDMLResolver &resolver, const TableItem &table,
                                const ObTableSchema &schema,
                                ObTableAssignment &assignments) {
  int ret = OB_SUCCESS;
  bool has_ai = false;
  ObRawExpr *request = nullptr;
  if (!resolver.session_info_->is_user_session()) {
    return ret;
  }
  const int64_t original_count = assignments.assignments_.count();
  for (auto it = schema.column_begin(); it != schema.column_end(); ++it) {
    has_ai |= (*it)->is_ai_column();
  }
  if (!has_ai) {
    return ret;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < original_count; ++i) {
    const auto &assignment = assignments.assignments_.at(i);
    const auto *column =
        schema.get_column_schema(assignment.column_expr_->get_column_id());
    if (column != nullptr && column->is_ai_column() && !assignment.is_implicit_) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED,
                     "explicit writes to an AI result column");
    }
  }
  OZ(resolver.resolve_all_basic_table_columns(table, true, nullptr));
  for (auto it = schema.column_begin();
       OB_SUCC(ret) && it != schema.column_end(); ++it) {
    const auto *column = *it;
    if (!column->is_ai_column()) {
      continue;
    }
    AiColumnDefinition d;
    OZ(load_ai_column_definition(schema, *column, *resolver.allocator_, d));
    ObSEArray<ObRawExpr *, 8> changed;
    for (int64_t i = 0; OB_SUCC(ret) && i < original_count; ++i) {
      const auto &assignment = assignments.assignments_.at(i);
      const auto *source =
          schema.get_column_schema(assignment.column_expr_->get_column_id());
      bool dependent = source != nullptr && source->is_rowkey_column();
      for (const auto &name : d.dependencies_) {
        dependent |=
            source != nullptr && source->get_column_name_str().case_compare(
                                     ObString(name.size(), name.data())) == 0;
      }
      if (dependent) {
        ObRawExpr *difference = nullptr;
        OZ(ai_changed_expr(resolver, assignment, difference));
        OZ(changed.push_back(difference));
      }
    }
    if (OB_SUCC(ret) && changed.count() > 0) {
      ObRawExpr *condition = nullptr, *absent = nullptr, *pending = nullptr,
                *spec_value = nullptr;
      OZ(ObRawExprUtils::build_or_exprs(*resolver.params_.expr_factory_,
                                        changed, condition));
      if (request == nullptr) {
        OZ(make_ai_request_expr(resolver, request));
      }
      OZ(ObRawExprUtils::build_null_expr(*resolver.params_.expr_factory_,
                                         absent));
      OZ(ai_string_expr(resolver, ObString("PENDING"), pending));
      const auto *state = schema.get_column_schema(
          ObString(d.state_name_.size(), d.state_name_.data()));
      const auto *req = schema.get_column_schema(
          ObString(d.request_name_.size(), d.request_name_.data()));
      const auto *spec = schema.get_column_schema(
          ObString(d.spec_name_.size(), d.spec_name_.data()));
      if (state == nullptr || req == nullptr || spec == nullptr) {
        ret = OB_INVALID_DATA;
      } else {
        OZ(ai_string_expr(resolver, spec->get_cur_default_value().get_string(),
                          spec_value));
        OZ(ai_append_conditional(resolver, assignments, schema,
                                 column->get_column_id(), condition, absent));
        OZ(ai_append_conditional(resolver, assignments, schema,
                                 state->get_column_id(), condition, pending));
        OZ(ai_append_conditional(resolver, assignments, schema,
                                 req->get_column_id(), condition, request));
        OZ(ai_append_conditional(resolver, assignments, schema,
                                 spec->get_column_id(), condition, spec_value));
      }
    }
  }
  return ret;
}
} // namespace sql
} // namespace oceanbase
