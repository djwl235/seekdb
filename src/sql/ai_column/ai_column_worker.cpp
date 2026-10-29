/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
 */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/ai_column/ai_column_worker.h"
#include "common/json_type/ob_json_parse.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "lib/time/ob_time_utility.h"
#include "observer/omt/ob_ai_service.h"
#include "share/ob_server_struct.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "sql/ai_column/ai_column.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include <cerrno>
#include <cmath>
#include <map>
#include <set>
#include <unistd.h>

namespace oceanbase {
namespace sql {
using namespace common;
using namespace share::schema;
namespace {
using Rows = std::vector<std::vector<std::string>>;
std::string text(const ObString &s) { return std::string(s.ptr(), s.length()); }
ObString str(const std::string &s) { return ObString(s.size(), s.data()); }
std::string name(const ObString &s) {
  std::string out = "`";
  for (int64_t i = 0; i < s.length(); ++i) {
    out += s.ptr()[i];
    if (s.ptr()[i] == '`')
      out += '`';
  }
  return out + '`';
}
std::string name(const std::string &s) { return name(str(s)); }
std::string literal(const std::string &s) {
  constexpr char hex[] = "0123456789abcdef";
  std::string out = "CONVERT(X'";
  for (unsigned char c : s) {
    out += hex[c >> 4];
    out += hex[c & 15];
  }
  return out + "' USING utf8mb4)";
}
int read(const std::string &query, int fields, Rows &rows) {
  int ret = OB_SUCCESS;
  ObMySQLProxy::MySQLResult result;
  OZ(GCTX.sql_proxy_->read(result, query.c_str()));
  auto *cursor = result.get_result();
  if (OB_SUCC(ret) && cursor == nullptr)
    ret = OB_ERR_UNEXPECTED;
  while (OB_SUCC(ret) && (ret = cursor->next()) == OB_SUCCESS) {
    std::vector<std::string> row;
    for (int i = 0; OB_SUCC(ret) && i < fields; ++i) {
      ObString value;
      ret = cursor->get_varchar(i, value);
      if (ret == OB_ERR_NULL_VALUE) {
        ret = OB_SUCCESS;
        row.emplace_back();
      } else if (OB_SUCC(ret))
        row.push_back(text(value));
    }
    if (OB_SUCC(ret))
      rows.push_back(std::move(row));
  }
  if (ret == OB_ITER_END)
    ret = OB_SUCCESS;
  return ret;
}
int write(const std::string &query, int64_t &affected) {
  return GCTX.sql_proxy_->write(query.c_str(), affected);
}
std::string extract(const std::string &request, const char *field) {
  return "JSON_UNQUOTE(JSON_EXTRACT(" + request + ", '$." + field + "'))";
}
struct Work {
  const ObColumnSchemaV2 *column_ = nullptr;
  AiColumnDefinition definition_;
  std::string request_, state_, spec_, predicate_;
};
int run_model(ObArenaAllocator &allocator, const AiColumnDefinition &d,
              const std::vector<std::string> &input, std::string &output) {
  int ret = OB_SUCCESS;
  ObString model_id = str(input[0]), prompt = str(input[1]), result;
  ObJsonObject *config = nullptr;
  if (!input[2].empty()) {
    ObJsonNode *node = nullptr;
    OZ(ObJsonParser::get_tree(&allocator, str(input[2]), node));
    if (OB_SUCC(ret) && node->json_type() != ObJsonNodeType::J_OBJECT)
      ret = OB_INVALID_ARGUMENT;
    if (OB_SUCC(ret))
      config = static_cast<ObJsonObject *>(node);
  }
  // AI_PROMPT is a structured prompt object. A plain string is preserved
  // verbatim.
  if (OB_SUCC(ret) && !input[1].empty() && input[1][0] == '{') {
    ObJsonNode *node = nullptr;
    const int parsed = ObJsonParser::get_tree(&allocator, prompt, node);
    if (parsed == OB_SUCCESS && node->json_type() == ObJsonNodeType::J_OBJECT &&
        ObAIFuncPromptObjectUtils::is_valid_prompt_object(
            static_cast<ObJsonObject *>(node))) {
      OZ(ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
          allocator, static_cast<ObJsonObject *>(node), prompt));
    }
  }
  ObAIFuncExprInfo *info = nullptr;
  omt::ObAiServiceGuard guard;
  const share::ObAiModelEndpointInfo *endpoint = nullptr;
  OZ(ObAIFuncUtils::get_ai_func_info(allocator, model_id, info));
  OZ(guard.get_ai_endpoint_by_ai_model_name(model_id, endpoint, false));
  if (OB_SUCC(ret)) {
    ObAIFuncModel model(allocator, *info, *endpoint);
    if (d.embedding_) {
      ObJsonObject *embedding_config = nullptr;
      OZ(ObAIFuncJsonUtils::get_json_object(allocator, embedding_config));
      ObJsonInt *dimension = nullptr;
      if (OB_SUCC(ret)) {
        dimension = OB_NEWx(ObJsonInt, &allocator, d.dimension_);
        if (dimension == nullptr)
          ret = OB_ALLOCATE_MEMORY_FAILED;
      }
      OZ(embedding_config->add("dimensions", dimension));
      OZ(model.call_dense_embedding(prompt, embedding_config, result));
      if (OB_SUCC(ret)) {
        ObJsonNode *node = nullptr;
        OZ(ObJsonParser::get_tree(&allocator, result, node));
        if (OB_SUCC(ret) &&
            (node->json_type() != ObJsonNodeType::J_ARRAY ||
             static_cast<ObJsonArray *>(node)->element_count() != d.dimension_))
          ret = OB_INVALID_DATA;
      }
    } else {
      OZ(model.call_completion(prompt, config, result, str(d.instruction_)));
    }
    if (OB_SUCC(ret))
      output = text(result);
  }
  return ret;
}
} // namespace
AiColumnWorker::AiColumnWorker()
    : boot_(std::to_string(ObTimeUtility::current_time()) + "-" +
            std::to_string(getpid())) {
  set_thread_count(1);
}
void AiColumnWorker::run1() {
  lib::set_thread_name("AiColumnWorker");
  while (!has_set_stop()) {
    if (GCTX.status_ == share::SS_SERVING && GCTX.sql_proxy_ != nullptr &&
        GCTX.schema_service_ != nullptr) {
      int ret = poll();
      if (ret != OB_SUCCESS)
        LOG_TRACE("AI column worker will retry catalog scan", K(ret));
    }
    usleep(100000);
  }
}
int AiColumnWorker::poll() {
  int ret = OB_SUCCESS;
  // ADD COLUMN uses the native original default for existing rows. Initialize
  // their missing request tokens only after the schema change commits. A single
  // ordinary UPDATE also makes the backfill transactional and crash
  // recoverable.
  static std::map<uint64_t, int64_t> seeded;
  ObArenaAllocator allocator("AiColumnSeed");
  ObSchemaGetterGuard guard;
  ObSEArray<const ObTableSchema *, 64> tables;
  OZ(GCTX.schema_service_->get_runtime_schema_guard(guard));
  OZ(guard.get_table_schemas_in_runtime(tables));
  for (int64_t i = 0; OB_SUCC(ret) && i < tables.count(); ++i) {
    const auto *table = tables.at(i);
    if (table->is_index_table() || table->is_sys_table() ||
        table->is_view_table())
      continue;
    const auto version = table->get_schema_version();
    const auto known = seeded.find(table->get_table_id());
    if (known != seeded.end() && known->second == version)
      continue;
    std::string assignments, predicate, first_request;
    for (auto it = table->column_begin();
         OB_SUCC(ret) && it != table->column_end(); ++it) {
      if (!(*it)->is_ai_column())
        continue;
      AiColumnDefinition d;
      OZ(load_ai_column_definition(*table, **it, allocator, d));
      const auto request = name(d.request_name_), state = name(d.state_name_);
      const std::string missing =
          "(" + request + " IS NULL AND " + state + "='PENDING')";
      const std::string fresh =
          "CAST(JSON_OBJECT('id',UUID(),'eligible',NOW(6),'state',0,'retry',0,'"
          "owner',NULL,'boot',NULL,'lease',NULL,'done',FALSE) AS CHAR)";
      if (!assignments.empty()) {
        assignments += ",";
        predicate += " OR ";
      }
      assignments += request + "=CASE WHEN " + missing + " THEN " +
                     (first_request.empty()
                          ? fresh
                          : "COALESCE(" + first_request + "," + fresh + ")") +
                     " ELSE " + request + " END";
      predicate += missing;
      if (first_request.empty())
        first_request = request;
    }
    if (OB_SUCC(ret) && !assignments.empty()) {
      const ObDatabaseSchema *database = nullptr;
      OZ(guard.get_database_schema(table->get_database_id(), database));
      if (database == nullptr) {
        ret = OB_SCHEMA_ERROR;
        break;
      }
      int64_t affected = 0;
      OZ(write("UPDATE /*+ OPT_PARAM('hidden_column_visible','true') "
               "USE_PLAN_CACHE(NONE) */ " +
                   name(database->get_database_name_str()) + "." +
                   name(table->get_table_name_str()) + " SET " + assignments +
                   " WHERE " + predicate,
               affected));
    }
    if (OB_SUCC(ret))
      seeded[table->get_table_id()] = version;
  }
  Rows tasks;
  OZ(read("SELECT CAST(table_id AS CHAR),request_id,rowkey_predicate FROM "
          "oceanbase.__all_ai_column_task WHERE eligible_ts <= NOW(6) LIMIT 64",
          3, tasks));
  std::set<std::string> visited;
  for (const auto &task : tasks) {
    if (has_set_stop())
      break;
    if (task[1].empty() || !visited.insert(task[0] + ":" + task[1]).second)
      continue;
    uint64_t id = strtoull(task[0].c_str(), nullptr, 10);
    int tmp_ret = process(id, task[1], task[2]);
    if (tmp_ret != OB_SUCCESS)
      LOG_TRACE("AI column group superseded or retryable", K(tmp_ret), K(id));
  }
  return ret;
}
int AiColumnWorker::process(uint64_t table_id, const std::string &request,
                            const std::string &rowkey) {
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AiColumnWork");
  ObSchemaGetterGuard guard;
  const ObTableSchema *table = nullptr;
  const ObDatabaseSchema *database = nullptr;
  OZ(GCTX.schema_service_->get_runtime_schema_guard(guard));
  OZ(guard.get_table_schema(table_id, table));
  if (OB_SUCC(ret) && table == nullptr)
    return OB_SUCCESS;
  OZ(guard.get_database_schema(table->get_database_id(), database));
  if (OB_SUCC(ret) && database == nullptr)
    ret = OB_ERR_UNEXPECTED;
  if (OB_FAIL(ret))
    return ret;
  const std::string target = name(database->get_database_name_str()) + "." +
                             name(table->get_table_name_str());
  const std::string hint =
      "/*+ OPT_PARAM('hidden_column_visible','true') USE_PLAN_CACHE(NONE) */ ";
  std::vector<Work> group;
  // Every read and write checks the immutable request token and definition.
  // Native UPDATE also maintains all indexes and participates in normal row
  // locking.
  for (auto it = table->column_begin();
       OB_SUCC(ret) && it != table->column_end(); ++it) {
    if (!(*it)->is_ai_column())
      continue;
    Work work;
    work.column_ = *it;
    auto &d = work.definition_;
    OZ(load_ai_column_definition(*table, **it, allocator, d));
    work.request_ = name(d.request_name_);
    work.state_ = name(d.state_name_);
    work.spec_ = name(d.spec_name_);
    const auto *schema_spec = table->get_column_schema(str(d.spec_name_));
    if (schema_spec == nullptr) {
      ret = OB_INVALID_DATA;
      break;
    }
    work.predicate_ =
        "(" + rowkey + ") AND " + work.state_ + "='PENDING' AND " +
        extract(work.request_, "id") + "=" + literal(request) + " AND " +
        work.spec_ + "=" +
        literal(text(schema_spec->get_cur_default_value().get_string()));
    Rows status;
    OZ(read("SELECT " + hint + "CAST(COALESCE(" +
                extract(work.request_, "done") + ",'false') AS CHAR) FROM " +
                target + " WHERE " + work.predicate_,
            1, status));
    if (OB_FAIL(ret) || status.empty())
      continue;
    group.push_back(work);
    if (status[0][0] == "true" || status[0][0] == "1")
      continue;
    const auto req = work.request_;
    int64_t affected = 0;
    const std::string owner =
        boot_ + ":" + std::to_string(ObTimeUtility::current_time());
    const std::string claim =
        "UPDATE " + hint + target + " SET " + req + "=CAST(JSON_SET(" + req +
        ",'$.owner'," + literal(owner) + ",'$.boot'," + literal(boot_) +
        ",'$.lease',DATE_ADD(NOW(6),INTERVAL 120 SECOND),'$.state',1) AS CHAR) "
        "WHERE " +
        work.predicate_ + " AND CAST(" + extract(req, "eligible") +
        " AS DATETIME(6))<=NOW(6) AND (" + extract(req, "owner") +
        " IS NULL OR " + extract(req, "owner") + "='null' OR " +
        extract(req, "boot") + "<>" + literal(boot_) + " OR CAST(" +
        extract(req, "lease") + " AS DATETIME(6))<NOW(6))";
    OZ(write(claim, affected));
    if (OB_FAIL(ret) || affected == 0)
      continue;
    const std::string owned = work.predicate_ + " AND " +
                              extract(req, "owner") + "=" + literal(owner);
    Rows input;
    OZ(read("SELECT " + hint + "CAST((" + d.model_sql_ + ") AS CHAR),CAST((" +
                d.prompt_sql_ + ") AS CHAR),CAST((" + d.config_sql_ +
                ") AS CHAR),CAST(COALESCE(" + extract(req, "retry") +
                ",'0') AS CHAR) FROM " + target + " WHERE " + owned,
            4, input));
    if (OB_FAIL(ret) || input.empty())
      continue;
    std::string output;
    int outcome = run_model(allocator, d, input[0], output);
    if (outcome == OB_SUCCESS &&
        ob_is_integer_type(work.column_->get_data_type())) {
      const char *begin = output.c_str();
      char *end = nullptr;
      errno = 0;
      if (ob_is_unsigned_type(work.column_->get_data_type())) {
        if (output.find('-') != std::string::npos)
          outcome = OB_INVALID_DATA;
        (void)strtoull(begin, &end, 10);
      } else {
        (void)strtoll(begin, &end, 10);
      }
      while (end != nullptr && isspace(static_cast<unsigned char>(*end)))
        ++end;
      if (errno == ERANGE || end == begin || end == nullptr || *end != '\0')
        outcome = OB_INVALID_DATA;
    }
    if (outcome == OB_SUCCESS && output.size() > 60000)
      outcome = OB_SIZE_OVERFLOW;
    if (outcome == OB_SUCCESS) {
      // Ask the native type system to validate conversion before marking
      // success.
      std::string type;
      if (work.column_->get_data_type() == ObJsonType)
        type = "JSON";
      else if (ob_is_integer_type(work.column_->get_data_type()))
        type = "SIGNED";
      else if (ob_is_numeric_type(work.column_->get_data_type()))
        type = "DECIMAL(65,20)";
      if (!type.empty()) {
        Rows validation;
        outcome = read("SELECT CAST(CAST(" + literal(output) + " AS " + type +
                           ") AS CHAR)",
                       1, validation);
      }
    }
    const int64_t retries = strtoll(input[0][3].c_str(), nullptr, 10);
    std::string update;
    if (outcome == OB_SUCCESS) {
      update = "JSON_SET(" + req +
               ",'$.done',TRUE,'$.success',TRUE,'$.result'," + literal(output) +
               ")";
    } else if (retries < d.max_retries_) {
      update =
          "JSON_SET(" + req + ",'$.state',3,'$.retry'," +
          std::to_string(retries + 1) +
          ",'$.eligible',DATE_ADD(NOW(6),INTERVAL 1 SECOND),'$.owner',NULL)";
    } else {
      update = "JSON_SET(" + req + ",'$.done',TRUE,'$.success',FALSE)";
    }
    OZ(write("UPDATE " + hint + target + " SET " + req + "=CAST(" + update +
                 " AS CHAR) WHERE " + owned,
             affected));
  }
  if (OB_FAIL(ret) || group.empty())
    return ret;
  std::string complete, assignments;
  for (const auto &work : group) {
    const auto req = work.request_;
    if (!complete.empty())
      complete += " AND ";
    complete += "(" + work.predicate_ + " AND " + extract(req, "done") +
                " IN ('true','1'))";
    if (!assignments.empty())
      assignments += ",";
    assignments += name(work.column_->get_column_name_str()) + "=CASE WHEN " +
                   extract(req, "success") + " IN ('true','1') THEN " +
                   extract(req, "result") + " ELSE NULL END," + work.state_ +
                   "=CASE WHEN " + extract(req, "success") +
                   " IN ('true','1') THEN 'READY' ELSE 'FAILED' END";
  }
  int64_t affected = 0;
  OZ(write("UPDATE " + hint + target + " SET " + assignments + " WHERE " +
               complete,
           affected));
  return ret;
}
} // namespace sql
} // namespace oceanbase
