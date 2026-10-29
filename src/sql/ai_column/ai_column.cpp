/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
 */
#define USING_LOG_PREFIX SQL_RESV
#include "sql/ai_column/ai_column.h"
#include "common/json_type/ob_json_parse.h"
#include "common/json_type/ob_json_tree.h"
#include "query/vector/ob_vector_index_util.h"
#include "share/schema/ob_schema_service.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "sql/resolver/ob_resolver_define.h"
#include <set>

namespace oceanbase {
namespace sql {
using namespace common;
using namespace share::schema;
namespace {
std::string ai_text(const ObString &s) {
  return std::string(s.ptr(), s.length());
}
std::string ai_node_text(const ParseNode &n) {
  return std::string(n.str_value_, n.str_len_);
}
std::string ai_lower(std::string s) {
  for (char &c : s) {
    c = tolower(static_cast<unsigned char>(c));
  }
  return s;
}
std::string ai_json_string(const std::string &s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c < 32) {
      char b[7];
      snprintf(b, sizeof(b), "\\u%04x", c);
      out += b;
    } else {
      out += c;
    }
  }
  return out + '"';
}
int gather_ai_dependencies(const ParseNode &node,
                           std::vector<std::string> &names) {
  int ret = OB_SUCCESS;
  if (node.type_ == T_SELECT || node.type_ == T_REF_QUERY ||
      node.type_ == T_QUESTIONMARK ||
      node.type_ == T_USER_VARIABLE_IDENTIFIER) {
    ret = OB_NOT_SUPPORTED;
  } else if (node.type_ == T_COLUMN_REF) {
    if (node.num_child_ != 3 || node.children_[0] != nullptr ||
        node.children_[1] != nullptr || node.children_[2] == nullptr) {
      ret = OB_NOT_SUPPORTED;
    } else {
      const auto name = ai_node_text(*node.children_[2]);
      bool found = false;
      for (const auto &v : names) {
        found |= ai_lower(v) == ai_lower(name);
      }
      if (!found) {
        names.push_back(name);
      }
    }
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < node.num_child_; ++i) {
      if (node.children_[i] != nullptr) {
        OZ(gather_ai_dependencies(*node.children_[i], names));
      }
    }
  }
  return ret;
}
std::string encode_ai_definition(const AiColumnDefinition &d) {
  std::string s =
      "{\"version\":1,\"model_sql\":" + ai_json_string(d.model_sql_) +
      ",\"prompt_sql\":" + ai_json_string(d.prompt_sql_) +
      ",\"config_sql\":" + ai_json_string(d.config_sql_);
  s += ",\"normalized_expr\":" + ai_json_string(d.normalized_expr_) +
       ",\"instruction\":" + ai_json_string(d.instruction_) +
       ",\"max_retries\":" + std::to_string(d.max_retries_);
  s += ",\"dimension\":" + std::to_string(d.dimension_) +
       ",\"embedding\":" + (d.embedding_ ? "true" : "false") +
       ",\"dependencies\":[";
  for (size_t i = 0; i < d.dependencies_.size(); ++i) {
    if (i) {
      s += ',';
    }
    s += ai_json_string(d.dependencies_[i]);
  }
  return s + "]}";
}
int decode_ai_definition(ObIAllocator &allocator, const ObString &text,
                         AiColumnDefinition &d) {
  int ret = OB_SUCCESS;
  ObJsonNode *root = nullptr;
  OZ(ObJsonParser::get_tree(&allocator, text, root));
  if (OB_SUCC(ret) &&
      (root == nullptr || root->json_type() != ObJsonNodeType::J_OBJECT)) {
    ret = OB_INVALID_DATA;
  }
  if (OB_SUCC(ret)) {
    auto *obj = static_cast<ObJsonObject *>(root);
    auto read = [&](const char *name, std::string &target) {
      auto *v = obj->get_value(name);
      if (v == nullptr || v->json_type() != ObJsonNodeType::J_STRING) {
        ret = OB_INVALID_DATA;
      } else {
        target.assign(v->get_data(), v->get_data_length());
      }
    };
    read("model_sql", d.model_sql_);
    read("prompt_sql", d.prompt_sql_);
    read("config_sql", d.config_sql_);
    read("normalized_expr", d.normalized_expr_);
    read("instruction", d.instruction_);
    auto *retry = obj->get_value("max_retries"),
         *dim = obj->get_value("dimension"),
         *embed = obj->get_value("embedding"),
         *deps = obj->get_value("dependencies");
    if (retry == nullptr || dim == nullptr || embed == nullptr ||
        deps == nullptr || deps->json_type() != ObJsonNodeType::J_ARRAY) {
      ret = OB_INVALID_DATA;
    } else {
      d.max_retries_ = retry->get_int();
      d.dimension_ = dim->get_int();
      d.embedding_ = embed->get_boolean();
      d.dependencies_.clear();
      auto *a = static_cast<ObJsonArray *>(deps);
      for (int64_t i = 0; OB_SUCC(ret) && i < a->element_count(); ++i) {
        auto *v = a->get_value(i);
        if (v->json_type() != ObJsonNodeType::J_STRING) {
          ret = OB_INVALID_DATA;
        } else {
          d.dependencies_.emplace_back(v->get_data(), v->get_data_length());
        }
      }
    }
  }
  return ret;
}
} // namespace
std::string ai_column_hidden_name(const ObString &name, char kind) {
  return "__ai_" + std::string(1, kind) + "_" + std::to_string(name.hash());
}
int resolve_ai_column_attribute(ObColumnSchemaV2 &column, const ParseNode &node,
                                ObResolverParams &params) {
  int ret = OB_SUCCESS;
  AiColumnDefinition d;
  if (column.is_ai_column() || node.num_child_ != 3 ||
      node.children_[0] == nullptr || node.children_[1] == nullptr) {
    ret = OB_NOT_SUPPORTED;
  } else {
    const std::string function = ai_lower(ai_node_text(*node.children_[0]));
    d.embedding_ = function == "ai_embed";
    const ParseNode &args = *node.children_[1];
    if ((function != "ai_embed" && function != "ai_complete") ||
        args.num_child_ < 2 || args.num_child_ > 3) {
      ret = OB_NOT_SUPPORTED;
    } else {
      const ParseNode &model = *args.children_[0]->children_[0];
      if (model.type_ != T_VARCHAR) {
        ret = OB_NOT_SUPPORTED;
      } else {
        ObAIFuncExprInfo *info = nullptr;
        OZ(ObAIFuncUtils::get_ai_func_info(
            *params.allocator_, ObString(model.str_len_, model.str_value_),
            info));
        if (OB_SUCC(ret) &&
            (info == nullptr ||
             (d.embedding_ && !ObAIFuncUtils::is_dense_embedding_type(info)) ||
             (!d.embedding_ && !ObAIFuncUtils::is_completion_type(info)))) {
          ret = OB_NOT_SUPPORTED;
        }
      }
      d.model_sql_ = ai_node_text(*args.children_[0]);
      d.prompt_sql_ = ai_node_text(*args.children_[1]);
      d.config_sql_ = "NULL";
      OZ(gather_ai_dependencies(*args.children_[1]->children_[0],
                                d.dependencies_));
      if (d.embedding_) {
        if (!column.is_collection()) {
          ret = OB_NOT_SUPPORTED;
        } else {
          OZ(share::ObVectorIndexUtil::get_vector_dim_from_extend_type_info(
              column.get_extended_type_info(), d.dimension_));
        }
        if (OB_SUCC(ret) && args.num_child_ == 3) {
          const auto &dim = *args.children_[2]->children_[0];
          if (dim.type_ != T_INT || dim.value_ != d.dimension_) {
            ret = OB_ERR_INVALID_VECTOR_DIM;
            LOG_USER_ERROR(OB_ERR_INVALID_VECTOR_DIM,
                           static_cast<int>(d.dimension_),
                           static_cast<int>(dim.value_));
          }
        }
      } else {
        if (args.num_child_ == 3) {
          d.config_sql_ = ai_node_text(*args.children_[2]);
        }
        if (column.is_json()) {
          d.instruction_ = "Return exactly one valid JSON value. Do not "
                           "include markdown or explanatory text.";
        } else if (ob_is_integer_type(column.get_data_type())) {
          d.instruction_ = "Return exactly one base-10 integer, with no "
                           "markdown or explanatory text.";
        } else if (ob_is_numeric_type(column.get_data_type())) {
          d.instruction_ = "Return exactly one finite decimal number, with no "
                           "markdown or explanatory text.";
        } else if (!column.is_string_type() &&
                   !ob_is_text_tc(column.get_data_type())) {
          ret = OB_NOT_SUPPORTED;
        }
      }
      d.normalized_expr_ = d.embedding_ ? "AI_EMBED(" : "AI_COMPLETE(";
      d.normalized_expr_ += d.model_sql_ + ", " + d.prompt_sql_;
      if (d.embedding_) {
        d.normalized_expr_ += ", " + std::to_string(d.dimension_);
      } else if (args.num_child_ == 3) {
        d.normalized_expr_ += ", " + d.config_sql_;
      }
      d.normalized_expr_ += ")";
      if (!d.instruction_.empty()) {
        d.normalized_expr_ += " /* system: " + d.instruction_ + " */";
      }
    }
    const ParseNode *options = node.children_[2];
    std::set<std::string> seen;
    for (int64_t i = 0;
         OB_SUCC(ret) && options != nullptr && i < options->num_child_; ++i) {
      const ParseNode *item = options->children_[i];
      if (item == nullptr || item->type_ != T_OP_EQ || item->num_child_ != 2 ||
          item->children_[0]->type_ != T_COLUMN_REF) {
        ret = OB_NOT_SUPPORTED;
      } else {
        auto *keynode = item->children_[0]->children_[2];
        auto *value = item->children_[1];
        const auto key = ai_lower(ai_node_text(*keynode));
        if (!seen.insert(key).second) {
          ret = OB_INVALID_ARGUMENT;
        } else if (key == "sync_mode") {
          if (value->type_ != T_VARCHAR ||
              ai_lower(ai_node_text(*value)) != "async") {
            ret = OB_NOT_SUPPORTED;
          }
        } else if (key == "max_retries") {
          if (value->type_ != T_INT || value->value_ < 0 ||
              value->value_ > 32) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            d.max_retries_ = value->value_;
          }
        } else {
          ret = OB_NOT_SUPPORTED;
        }
      }
    }
    if (OB_SUCC(ret)) {
      const auto metadata = encode_ai_definition(d);
      ObObj value;
      value.set_varchar(ObString(metadata.size(), metadata.data()));
      value.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      OZ(column.set_orig_default_value(value));
      column.add_column_flag(AI_COLUMN_FLAG);
      column.set_nullable(true);
    }
  }
  if (ret == OB_NOT_SUPPORTED) {
    LOG_USER_ERROR(OB_NOT_SUPPORTED,
                   "AI COLUMN requires an async AI_COMPLETE/AI_EMBED "
                   "definition with ordinary-column dependencies");
  }
  return ret;
}
int prepare_ai_columns(ObTableSchema &table, ObIAllocator &allocator,
                       const ObTableSchema *original) {
  int ret = OB_SUCCESS;
  std::vector<uint64_t> ids;
  for (auto it = table.column_begin(); it != table.column_end(); ++it) {
    if ((*it)->is_ai_column()) {
      ids.push_back((*it)->get_column_id());
    }
  }
  uint64_t next = table.get_max_used_column_id();
  for (auto it = table.column_begin(); it != table.column_end(); ++it) {
    next = std::max(next, (*it)->get_column_id());
  }
  for (uint64_t id : ids) {
    if (OB_FAIL(ret)) {
      break;
    }
    auto *column = table.get_column_schema(id);
    const std::string spec =
        ai_column_hidden_name(column->get_column_name_str(), 'p');
    if (table.get_column_schema(ObString(spec.size(), spec.data())) !=
        nullptr) {
      continue;
    }
    AiColumnDefinition d;
    const ObObj metadata = column->get_orig_default_value();
    if (metadata.is_null() || !metadata.is_string_type()) {
      ret = OB_INVALID_DATA;
    } else {
      OZ(decode_ai_definition(allocator, metadata.get_string(), d));
    }
    for (const auto &dep : d.dependencies_) {
      if (OB_FAIL(ret)) {
        break;
      }
      const ObString name(dep.size(), dep.data());
      const auto *source = table.get_column_schema(name);
      if (source == nullptr && original != nullptr) {
        source = original->get_column_schema(name);
      }
      if (source == nullptr || source->is_ai_column() || source->is_hidden()) {
        ret = OB_NOT_SUPPORTED;
      }
    }
    for (char kind : {'s', 'r', 'p'}) {
      if (OB_FAIL(ret)) {
        break;
      }
      ObColumnSchemaV2 hidden;
      hidden.set_table_id(table.get_table_id());
      hidden.set_column_id(++next);
      const std::string name =
          ai_column_hidden_name(column->get_column_name_str(), kind);
      OZ(hidden.set_column_name(ObString(name.size(), name.data())));
      hidden.set_data_type(ObVarcharType);
      hidden.set_data_length(kind == 's' ? 16 : 65535);
      hidden.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      hidden.set_charset_type(CHARSET_UTF8MB4);
      hidden.set_is_hidden(true);
      hidden.set_nullable(true);
      ObObj value;
      if (kind == 's') {
        value.set_varchar("PENDING");
      } else if (kind == 'p') {
        value = metadata;
      } else {
        value.set_null();
      }
      if (!value.is_null()) {
        value.set_collation_type(CS_TYPE_UTF8MB4_BIN);
        value.set_collation_level(CS_LEVEL_IMPLICIT);
      }
      OZ(hidden.set_cur_default_value(value, false));
      OZ(hidden.set_orig_default_value(value));
      OZ(table.add_column(hidden));
    }
    ObObj absent;
    absent.set_null();
    if (OB_SUCC(ret)) {
      OZ(column->set_orig_default_value(absent));
    }
  }
  table.set_max_used_column_id(next);
  return ret;
}
int load_ai_column_definition(const ObTableSchema &table,
                              const ObColumnSchemaV2 &column,
                              ObIAllocator &allocator, AiColumnDefinition &d) {
  int ret = OB_SUCCESS;
  d.state_name_ = ai_column_hidden_name(column.get_column_name_str(), 's');
  d.request_name_ = ai_column_hidden_name(column.get_column_name_str(), 'r');
  d.spec_name_ = ai_column_hidden_name(column.get_column_name_str(), 'p');
  const auto *spec = table.get_column_schema(
      ObString(d.spec_name_.size(), d.spec_name_.data()));
  if (spec == nullptr || spec->get_cur_default_value().is_null()) {
    ret = OB_INVALID_DATA;
  } else {
    OZ(decode_ai_definition(allocator,
                            spec->get_cur_default_value().get_string(), d));
  }
  return ret;
}
int prepare_ai_alter_columns(AlterTableSchema &changes,
                             const ObTableSchema &original,
                             ObIAllocator &allocator) {
  int ret = OB_SUCCESS;
  ObTableSchema combined(&allocator);
  std::vector<AlterColumnSchema *> added;
  OZ(combined.assign(original));
  uint64_t next = original.get_max_used_column_id();
  for (auto it = changes.column_begin();
       OB_SUCC(ret) && it != changes.column_end(); ++it) {
    auto *column = static_cast<AlterColumnSchema *>(*it);
    if (column->is_ai_column() && column->alter_type_ == OB_DDL_ADD_COLUMN) {
      ObColumnSchemaV2 copy;
      OZ(copy.assign(*column));
      copy.set_column_id(++next);
      OZ(combined.add_column(copy));
      added.push_back(column);
    }
  }
  combined.set_max_used_column_id(next);
  OZ(prepare_ai_columns(combined, allocator, &original));
  for (auto *column : added) {
    if (OB_FAIL(ret)) {
      break;
    }
    for (char kind : {'s', 'r', 'p'}) {
      if (OB_FAIL(ret)) {
        break;
      }
      const auto name =
          ai_column_hidden_name(column->get_column_name_str(), kind);
      const auto *hidden =
          combined.get_column_schema(ObString(name.size(), name.data()));
      if (hidden == nullptr) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        AlterColumnSchema addition;
        OZ(addition.assign(*hidden));
        addition.alter_type_ = OB_DDL_ADD_COLUMN;
        addition.is_set_default_ = true;
        addition.is_set_nullable_ = true;
        addition.set_column_id(OB_INVALID_ID);
        OZ(changes.add_alter_column(addition, true));
      }
    }
    ObObj absent;
    absent.set_null();
    OZ(column->set_orig_default_value(absent));
  }
  return ret;
}
} // namespace sql
} // namespace oceanbase
