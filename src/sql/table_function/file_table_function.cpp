/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
 */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/table_function/file_table_function.h"
#include "common/json_type/ob_json_parse.h"
#include "common/json_type/ob_json_tree.h"
#include "common/timezone/ob_time_convert.h"
#include "file_sql_bridge.h"
#include "share/ob_server_struct.h"
#include "share/schema/ob_schema_utils.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/resolver/dml/ob_dml_resolver.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "sql/session/ob_sql_session_info.h"
#include <atomic>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace oceanbase {
namespace sql {
using namespace common;
namespace {
std::string string_value(const ObString &s) {
  return std::string(s.ptr(), s.length());
}
std::string quote_json(const std::string &value) {
  std::string out = "\"";
  for (unsigned char ch : value) {
    if (ch == '"' || ch == '\\') {
      out += '\\';
      out += ch;
    } else if (ch < 32) {
      char hex[7];
      snprintf(hex, sizeof(hex), "\\u%04x", ch);
      out += hex;
    } else {
      out += ch;
    }
  }
  return out + '"';
}
int check_enabled(ObSQLSessionInfo &session) {
  int ret = OB_SUCCESS;
  bool enabled = false;
  OZ(session.get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled));
  if (OB_SUCC(ret) && !enabled) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED,
                   "file SQL is disabled; SET SESSION enable_file_sql=ON");
  }
  return ret;
}
int authorize_path(ObSQLSessionInfo &session, ObIAllocator &allocator,
                   const std::string &path, std::string &canonical) {
  int ret = check_enabled(session);
  char real[PATH_MAX];
  ObString secure;
  if (OB_SUCC(ret) && (path.empty() || path.find('\0') != std::string::npos ||
                       path.find("://") != std::string::npos)) {
    ret = OB_INVALID_ARGUMENT;
  }
  if (OB_SUCC(ret) && realpath(path.c_str(), real) == nullptr) {
    ret = OB_FILE_NOT_EXIST;
  }
  if (OB_SUCC(ret)) {
    OZ(share::schema::ObSchemaUtils::get_runtime_varchar_variable(
        *GCTX.schema_service_, share::SYS_VAR_SECURE_FILE_PRIV, allocator,
        secure));
    OZ(ObResolverUtils::check_secure_path(secure, ObString::make_string(real)));
    if (OB_SUCC(ret)) {
      canonical = real;
    }
  }
  return ret;
}
int open_authorized(ObSQLSessionInfo &session, ObIAllocator &allocator,
                    const std::string &path, int &fd, struct stat &identity) {
  int ret = OB_SUCCESS;
  std::string canonical;
  fd = -1;
  OZ(authorize_path(session, allocator, path, canonical));
  if (OB_SUCC(ret)) {
    fd = ::open(canonical.c_str(),
                O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    struct stat named;
    if (fd < 0) {
      ret = OB_FILE_NOT_EXIST;
    } else if (fstat(fd, &identity) != 0 ||
               stat(canonical.c_str(), &named) != 0 ||
               !S_ISREG(identity.st_mode) || identity.st_dev != named.st_dev ||
               identity.st_ino != named.st_ino) {
      ret = OB_INVALID_ARGUMENT;
    } else if (identity.st_size > 4LL * 1024 * 1024 * 1024) {
      ret = OB_SIZE_OVERFLOW;
    }
  }
  if (OB_FAIL(ret) && fd >= 0) {
    ::close(fd);
    fd = -1;
  }
  return ret;
}
const char *format_for(uint64_t id) {
  return id == READ_CSV_FUNCTION_ID    ? "csv"
         : id == READ_JSON_FUNCTION_ID ? "json"
                                       : "parquet";
}
int reader_error(const file_sql::FileScan &scan) {
  const auto msg = scan.error();
  LOG_USER_ERROR(OB_INVALID_ARGUMENT,
                 std::string(msg.data(), msg.size()).c_str());
  return OB_INVALID_ARGUMENT;
}
int normalize_arguments(ObDMLResolver &resolver, const TableFunctionCall &call,
                        std::string &path, std::string &options) {
  int ret = OB_SUCCESS;
  ObString p;
  std::set<std::string> seen;
  options = "{";
  OZ(resolver.resolve_str_const(*call.arguments_->children_[0], p));
  if (OB_SUCC(ret)) {
    path = string_value(p);
  }
  for (int64_t i = 1; OB_SUCC(ret) && i < call.arguments_->num_child_; ++i) {
    const ParseNode *arg = call.arguments_->children_[i];
    if (arg->type_ != T_OP_EQ || arg->num_child_ != 2 ||
        arg->children_[0]->type_ != T_COLUMN_REF) {
      ret = OB_ERR_PARSER_SYNTAX;
    } else {
      const ParseNode *lhs = arg->children_[0];
      const ParseNode *name = lhs->children_[lhs->num_child_ - 1];
      if (name == nullptr) {
        ret = OB_ERR_PARSER_SYNTAX;
      } else {
        std::string key(name->str_value_, name->str_len_);
        for (char &c : key) {
          c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        }
        const ParseNode *value = arg->children_[1];
        std::string json;
        if (!seen.insert(key).second) {
          ret = OB_ERR_PARSER_SYNTAX;
        } else if (key == "sample_size") {
          const bool all = value->type_ == T_INT && value->value_ == -1;
          const bool negative = value->type_ == T_OP_NEG &&
                                value->num_child_ == 1 &&
                                value->children_[0]->type_ == T_INT &&
                                value->children_[0]->value_ == 1;
          if (!all && !negative) {
            ret = OB_NOT_SUPPORTED;
            LOG_USER_ERROR(OB_NOT_SUPPORTED,
                           "sample_size must be -1 (full schema inference)");
          }
        } else if (key == "header") {
          if (value->type_ != T_BOOL && value->type_ != T_INT) {
            ret = OB_ERR_PARSER_SYNTAX;
          } else {
            json = value->value_ ? "true" : "false";
          }
        } else if (key == "delimiter" || key == "sep" || key == "quote" ||
                   key == "escape" || key == "nullstr" || key == "format") {
          ObString text;
          OZ(resolver.resolve_str_const(*value, text));
          if (OB_SUCC(ret)) {
            if (key == "format") {
              const std::string f = string_value(text);
              if ((call.descriptor_->id_ == READ_JSON_FUNCTION_ID &&
                   f != "newline_delimited" && f != "ndjson" && f != "jsonl") ||
                  (call.descriptor_->id_ == READ_CSV_FUNCTION_ID &&
                   f != "csv")) {
                ret = OB_ERR_PARSER_SYNTAX;
              }
            } else {
              json = quote_json(string_value(text));
              if (key == "sep") {
                key = "delimiter";
              }
            }
          }
        } else {
          ret = OB_ERR_PARSER_SYNTAX;
        }
        if (OB_SUCC(ret) && !json.empty()) {
          if (options.size() > 1) {
            options += ',';
          }
          options += quote_json(key) + ':' + json;
        }
      }
    }
  }
  options += '}';
  return ret;
}
class FileTableScan final : public ITableFunctionScan {
public:
  FileTableScan(const TableFunctionScanContext &ctx, int fd,
                const struct stat &identity, rust::Box<file_sql::FileScan> scan)
      : context_(ctx), fd_(fd), identity_(identity), scan_(std::move(scan)) {}
  ~FileTableScan() override { close(); }
  int open() override {
    int ret = OB_SUCCESS;
    OZ(cells_.prepare_allocate(context_.column_ids_.count()));
    row_.cells_ = cells_.get_data();
    row_.count_ = cells_.count();
    return ret;
  }
  int rescan(const ObIArray<ObObj> &) override {
    int ret = check_enabled(*context_.exec_ctx_.get_my_session());
    if (OB_SUCC(ret) && scan_->rewind() != 0) {
      ret = reader_error(*scan_);
    }
    return ret;
  }
  int get_next_row(ObNewRow *&row) override {
    int ret = OB_SUCCESS;
    struct stat now;
    if (fd_ < 0 || fstat(fd_, &now) != 0 || now.st_size != identity_.st_size ||
        now.st_mtime != identity_.st_mtime) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      const int status = scan_->next();
      if (status == 1) {
        ret = OB_ITER_END;
      } else if (status != 0) {
        ret = reader_error(*scan_);
      } else {
        const auto values = scan_->row();
        const auto columns = scan_->columns();
        for (int64_t i = 0; OB_SUCC(ret) && i < cells_.count(); ++i) {
          const uint64_t offset =
              context_.column_ids_.at(i) - OB_APP_MIN_COLUMN_ID;
          if (offset >= values.size()) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            const auto &v = values[offset];
            const auto &c = columns[offset];
            ObObj &out = cells_.at(i);
            ObString text(static_cast<int32_t>(v.text.size()),
                          const_cast<char *>(v.text.data()));
            if (v.kind == 0) {
              out.set_null();
            } else if (c.kind == 1) {
              out.set_int(v.integer);
            } else if (c.kind == 2) {
              out.set_double(v.kind == 2 ? v.real
                                         : static_cast<double>(v.integer));
            } else if (c.kind == 3) {
              out.set_tinyint(v.integer);
            } else if (c.kind == 5) {
              int32_t value = 0;
              OZ(ObTimeConverter::str_to_date(text, value));
              out.set_date(value);
            } else if (c.kind == 6) {
              int64_t value = 0;
              OZ(ObTimeConverter::str_to_datetime(
                  text, ObTimeConvertCtx(nullptr, false), value));
              out.set_datetime(value);
            } else {
              out.set_varchar(text);
              out.set_collation_type(CS_TYPE_UTF8MB4_BIN);
            }
          }
        }
        if (OB_SUCC(ret)) {
          row = &row_;
        }
      }
    }
    return ret;
  }
  int close() override {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    return OB_SUCCESS;
  }

private:
  const TableFunctionScanContext context_;
  int fd_;
  struct stat identity_;
  rust::Box<file_sql::FileScan> scan_;
  ObSEArray<ObObj, 16> cells_;
  ObNewRow row_;
};
class GlobTableScan final : public ITableFunctionScan {
public:
  explicit GlobTableScan(const TableFunctionScanContext &ctx)
      : context_(ctx), position_(0) {}
  int open() override { return rescan(context_.arguments_); }
  int rescan(const ObIArray<ObObj> &args) override {
    int ret = check_enabled(*context_.exec_ctx_.get_my_session());
    paths_.clear();
    position_ = 0;
    if (OB_SUCC(ret) && (args.count() < 1 || args.at(0).is_null())) {
      ret = OB_INVALID_ARGUMENT;
    }
    glob_t files = {};
    if (OB_SUCC(ret)) {
      const std::string pattern = string_value(args.at(0).get_string());
      if (pattern.find('\0') != std::string::npos ||
          pattern.find("://") != std::string::npos) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        const int rc = ::glob(pattern.c_str(), GLOB_NOSORT, nullptr, &files);
        if (rc != 0 && rc != GLOB_NOMATCH) {
          ret = OB_IO_ERROR;
        }
        for (size_t i = 0; OB_SUCC(ret) && i < files.gl_pathc; ++i) {
          std::string canonical;
          struct stat st;
          OZ(authorize_path(*context_.exec_ctx_.get_my_session(),
                            context_.allocator_, files.gl_pathv[i], canonical));
          if (OB_SUCC(ret) && stat(canonical.c_str(), &st) == 0 &&
              S_ISREG(st.st_mode)) {
            paths_.emplace_back(files.gl_pathv[i]);
          }
          if (paths_.size() > 100000) {
            ret = OB_SIZE_OVERFLOW;
          }
        }
      }
    }
    globfree(&files);
    return ret;
  }
  int get_next_row(ObNewRow *&row) override {
    int ret = OB_SUCCESS;
    if (position_ >= paths_.size()) {
      ret = OB_ITER_END;
    } else {
      auto &s = paths_[position_++];
      value_.set_varchar(ObString(s.size(), s.data()));
      value_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      row_.cells_ = &value_;
      row_.count_ = context_.column_ids_.count();
      row = &row_;
    }
    return ret;
  }
  int close() override {
    paths_.clear();
    return OB_SUCCESS;
  }

private:
  const TableFunctionScanContext context_;
  std::vector<std::string> paths_;
  size_t position_;
  ObObj value_;
  ObNewRow row_;
};
} // namespace
int bind_file_table_function(ObDMLResolver &resolver,
                             const TableFunctionCall &call, TableItem *&item) {
  int ret = check_enabled(*resolver.session_info_);
  std::string path, options;
  ObQueryHint &hint =
      resolver.get_stmt()->get_query_ctx()->get_query_hint_for_update();
  const_cast<ObGlobalHint &>(hint.get_global_hint())
      .merge_plan_cache_hint(OB_USE_PLAN_CACHE_NONE);
  OZ(normalize_arguments(resolver, call, path, options));
  TableFunctionDescriptor descriptor = *call.descriptor_;
  std::vector<TableFunctionColumn> columns;
  int fd = -1;
  struct stat identity;
  if (OB_SUCC(ret) && descriptor.id_ == GLOB_FUNCTION_ID) {
    TableFunctionColumn c;
    c.name_ = "file";
    c.id_ = OB_APP_MIN_COLUMN_ID;
    c.type_.set_obj_type(ObVarcharType);
    c.type_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    c.type_.set_length(65535);
    columns.push_back(c);
  } else if (OB_SUCC(ret)) {
    OZ(open_authorized(*resolver.session_info_, *resolver.allocator_, path, fd,
                       identity));
    if (OB_SUCC(ret)) {
      auto scan = file_sql::open_scan(fd, format_for(descriptor.id_), options);
      if (scan->status() != 0) {
        ret = reader_error(*scan);
      } else {
        const auto inferred = scan->columns();
        for (size_t i = 0; OB_SUCC(ret) && i < inferred.size(); ++i) {
          const auto &v = inferred[i];
          TableFunctionColumn c;
          ObString name;
          OZ(ob_write_string(
              *resolver.allocator_,
              ObString(v.name.size(), const_cast<char *>(v.name.data())), name,
              true));
          if (OB_SUCC(ret)) {
            c.name_ = name.ptr();
            c.id_ = OB_APP_MIN_COLUMN_ID + i;
            switch (v.kind) {
            case 1:
              c.type_.set_int();
              break;
            case 2:
              c.type_.set_obj_type(ObDoubleType);
              break;
            case 3:
              c.type_.set_obj_type(ObTinyIntType);
              break;
            case 5:
              c.type_.set_obj_type(ObDateType);
              break;
            case 6:
              c.type_.set_obj_type(ObDateTimeType);
              c.type_.set_scale(v.scale);
              break;
            default:
              c.type_.set_obj_type(ObVarcharType);
              c.type_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
              c.type_.set_length(65535);
            }
            columns.push_back(c);
          }
        }
      }
      descriptor.columns_ = columns.data();
      descriptor.column_count_ = columns.size();
      TableFunctionCall normalized = call;
      normalized.descriptor_ = &descriptor;
      ParseNode arguments = {}, path_node = {}, options_node = {};
      ParseNode *args[] = {&path_node, &options_node};
      path_node.type_ = T_VARCHAR;
      path_node.str_value_ = path.c_str();
      path_node.str_len_ = path.size();
      options_node.type_ = T_VARCHAR;
      options_node.str_value_ = options.c_str();
      options_node.str_len_ = options.size();
      arguments.type_ = T_EXPR_LIST;
      arguments.children_ = args;
      arguments.num_child_ = 2;
      normalized.arguments_ = &arguments;
      OZ(resolver.bind_fixed_table_function(normalized, item));
      if (OB_SUCC(ret)) {
        const auto inferred = scan->columns();
        for (int64_t i = 0; i < resolver.get_stmt()->get_column_size(); ++i) {
          ColumnItem *c = resolver.get_stmt()->get_column_item(i);
          if (c->table_id_ == item->table_id_ &&
              !inferred[c->column_id_ - OB_APP_MIN_COLUMN_ID].nullable) {
            c->expr_->set_result_flag(NOT_NULL_FLAG);
          }
        }
      }
    }
  }
  if (fd >= 0) {
    ::close(fd);
  }
  if (OB_SUCC(ret) && descriptor.id_ == GLOB_FUNCTION_ID) {
    descriptor.columns_ = columns.data();
    descriptor.column_count_ = columns.size();
    TableFunctionCall bound = call;
    bound.descriptor_ = &descriptor;
    OZ(resolver.bind_fixed_table_function(bound, item));
  }
  return ret;
}
int create_file_table_scan(const TableFunctionScanContext &context,
                           ITableFunctionScan *&scan) {
  int ret = OB_SUCCESS;
  void *memory = nullptr;
  if (context.descriptor_.id_ == GLOB_FUNCTION_ID) {
    if (OB_ISNULL(memory = context.allocator_.alloc(sizeof(GlobTableScan)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      scan = new (memory) GlobTableScan(context);
    }
  } else if (context.arguments_.count() != 2 ||
             context.arguments_.at(0).is_null() ||
             context.arguments_.at(1).is_null()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    int fd = -1;
    struct stat identity;
    const std::string path =
        string_value(context.arguments_.at(0).get_string());
    const std::string options =
        string_value(context.arguments_.at(1).get_string());
    OZ(open_authorized(*context.exec_ctx_.get_my_session(), context.allocator_,
                       path, fd, identity));
    if (OB_SUCC(ret)) {
      auto reader =
          file_sql::open_scan(fd, format_for(context.descriptor_.id_), options);
      if (reader->status() != 0) {
        ret = reader_error(*reader);
      } else if (OB_ISNULL(memory = context.allocator_.alloc(
                               sizeof(FileTableScan)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        scan = new (memory)
            FileTableScan(context, fd, identity, std::move(reader));
        fd = -1;
      }
    }
    if (fd >= 0) {
      ::close(fd);
    }
  }
  return ret;
}

int resolve_file_copy_options(ObDMLResolver &resolver, const ParseNode &node,
                              ObString &out) {
  int ret = check_enabled(*resolver.session_info_);
  std::string path, options;
  TableFunctionDescriptor descriptor = {READ_CSV_FUNCTION_ID,
                                        "copy",
                                        "copy",
                                        "CSV export",
                                        1,
                                        -1,
                                        nullptr,
                                        0,
                                        nullptr,
                                        nullptr};
  TableFunctionCall call;
  call.descriptor_ = &descriptor;
  const ParseNode *list = node.children_[1];
  std::vector<ParseNode *> args;
  args.push_back(node.children_[0]);
  for (int64_t i = 0; list != nullptr && i < list->num_child_; ++i) {
    args.push_back(list->children_[i]);
  }
  ParseNode arguments = {};
  arguments.type_ = T_EXPR_LIST;
  arguments.children_ = args.data();
  arguments.num_child_ = args.size();
  call.arguments_ = &arguments;
  OZ(normalize_arguments(resolver, call, path, options));
  OZ(ob_write_string(*resolver.allocator_,
                     ObString(options.size(), options.data()), out));
  return ret;
}
int add_file_copy_names(ObIAllocator &allocator, const ObString &options,
                        const ObIArray<ObString> &names, ObString &out) {
  std::string json = string_value(options);
  json.pop_back();
  if (json.size() > 1) {
    json += ',';
  }
  json += "\"names\":[";
  for (int64_t i = 0; i < names.count(); ++i) {
    if (i) {
      json += ',';
    }
    json += quote_json(string_value(names.at(i)));
  }
  json += "]}";
  return ob_write_string(allocator, ObString(json.size(), json.data()), out);
}
namespace {
class CopyFileWriter final : public ICopyFileWriter {
public:
  explicit CopyFileWriter(ObExecContext &ctx)
      : ctx_(ctx), dir_(-1), fd_(-1), delimiter_(','), quote_('"'),
        escape_('"') {}
  ~CopyFileWriter() override {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    if (dir_ >= 0) {
      if (!temporary_.empty()) {
        unlinkat(dir_, temporary_.c_str(), 0);
      }
      ::close(dir_);
    }
  }
  int init(const ObString &destination, const ObString &options) {
    int ret = OB_SUCCESS;
    const std::string path = string_value(destination);
    const size_t slash = path.rfind('/');
    std::string parent = slash == std::string::npos ? "."
                         : slash == 0               ? "/"
                                                    : path.substr(0, slash),
                canonical;
    name_ = slash == std::string::npos ? path : path.substr(slash + 1);
    if (path.find('\0') != std::string::npos || name_.empty() || name_ == "." ||
        name_ == ".." || path.find("://") != std::string::npos) {
      ret = OB_INVALID_ARGUMENT;
    }
    OZ(authorize_path(*ctx_.get_my_session(), ctx_.get_allocator(), parent,
                      canonical));
    ObJsonNode *root = nullptr;
    OZ(ObJsonParser::get_tree(&ctx_.get_allocator(), options, root));
    if (OB_SUCC(ret) &&
        (root == nullptr || root->json_type() != ObJsonNodeType::J_OBJECT)) {
      ret = OB_INVALID_ARGUMENT;
    }
    bool header = false;
    ObJsonArray *names = nullptr;
    if (OB_SUCC(ret)) {
      auto *obj = static_cast<ObJsonObject *>(root);
      auto *h = obj->get_value("header");
      header = h != nullptr && h->get_boolean();
      auto set_char = [&](const char *key, char &target) {
        auto *v = obj->get_value(key);
        if (v != nullptr) {
          if (v->json_type() != ObJsonNodeType::J_STRING ||
              v->get_data_length() != 1) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            target = v->get_data()[0];
          }
        }
      };
      set_char("delimiter", delimiter_);
      set_char("quote", quote_);
      set_char("escape", escape_);
      auto *n = obj->get_value("nullstr");
      if (n != nullptr) {
        null_ = std::string(n->get_data(), n->get_data_length());
      }
      auto *columns = obj->get_value("names");
      if (columns == nullptr ||
          columns->json_type() != ObJsonNodeType::J_ARRAY) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        names = static_cast<ObJsonArray *>(columns);
      }
    }
    if (OB_SUCC(ret)) {
      dir_ = ::open(canonical.c_str(),
                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      struct stat existing;
      if (dir_ < 0) {
        ret = OB_IO_ERROR;
      } else if (fstatat(dir_, name_.c_str(), &existing, AT_SYMLINK_NOFOLLOW) ==
                 0) {
        ret = OB_FILE_ALREADY_EXIST;
      } else if (errno != ENOENT) {
        ret = OB_IO_ERROR;
      }
    }
    if (OB_SUCC(ret)) {
      static std::atomic<uint64_t> sequence{0};
      for (int i = 0; fd_ < 0 && i < 100; ++i) {
        temporary_ = ".seekdb-copy-" + std::to_string(getpid()) + "-" +
                     std::to_string(sequence.fetch_add(1));
        fd_ =
            openat(dir_, temporary_.c_str(),
                   O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd_ < 0 && errno != EEXIST) {
          break;
        }
      }
      if (fd_ < 0) {
        temporary_.clear();
        ret = OB_IO_ERROR;
      }
    }
    if (OB_SUCC(ret) && header) {
      std::string line;
      for (int64_t i = 0; i < names->element_count(); ++i) {
        if (i) {
          line += delimiter_;
        }
        auto *v = names->get_value(i);
        encode(std::string(v->get_data(), v->get_data_length()), line);
      }
      line += '\n';
      OZ(write_bytes(line));
    }
    return ret;
  }
  void encode(const std::string &text, std::string &out) const {
    const bool enclosed = text.empty() || text == null_ ||
                          text.find(delimiter_) != std::string::npos ||
                          text.find(quote_) != std::string::npos ||
                          text.find('\n') != std::string::npos ||
                          text.find('\r') != std::string::npos;
    if (enclosed) {
      out += quote_;
    }
    for (char c : text) {
      if (enclosed && (c == quote_ || c == escape_)) {
        out += escape_;
      }
      out += c;
    }
    if (enclosed) {
      out += quote_;
    }
  }
  int write_bytes(const std::string &text) {
    int ret = OB_SUCCESS;
    size_t pos = 0;
    while (OB_SUCC(ret) && pos < text.size()) {
      const ssize_t written =
          ::write(fd_, text.data() + pos, text.size() - pos);
      if (written < 0 && errno == EINTR) {
      } else if (written <= 0) {
        ret = OB_IO_ERROR;
      } else {
        pos += written;
      }
    }
    return ret;
  }
  int append(const ObIArray<ObObj> &values) override {
    int ret = OB_SUCCESS;
    row_allocator_.reuse();
    std::string line;
    ObObjPrintParams params;
    params.tz_info_ = ctx_.get_my_session()->get_timezone_info();
    params.use_memcpy_ = true;
    for (int64_t i = 0; OB_SUCC(ret) && i < values.count(); ++i) {
      if (i) {
        line += delimiter_;
      }
      const ObObj &v = values.at(i);
      if (v.is_null()) {
        line += null_;
      } else {
        ObString text;
        char buf[1024];
        int64_t pos = 0;
        if (v.is_string_type() || ob_is_text_tc(v.get_type())) {
          text = v.get_string();
          OZ(ObTextStringHelper::read_real_string_data(
              ctx_, &row_allocator_, v.get_type(), v.get_collation_type(),
              v.has_lob_header(), text));
        } else {
          OZ(v.print_plain_str_literal(buf, sizeof(buf), pos, params));
          text.assign_ptr(buf, pos);
        }
        if (OB_SUCC(ret)) {
          encode(string_value(text), line);
        }
      }
      if (line.size() > 16 * 1024 * 1024) {
        ret = OB_SIZE_OVERFLOW;
      }
    }
    line += '\n';
    OZ(write_bytes(line));
    return ret;
  }
  int finish() override {
    int ret = OB_SUCCESS;
    if (fd_ < 0) {
      ret = OB_NOT_INIT;
    } else if (fsync(fd_) != 0) {
      ret = OB_IO_ERROR;
    }
    if (OB_SUCC(ret)) {
      if (linkat(dir_, temporary_.c_str(), dir_, name_.c_str(), 0) != 0) {
        ret = errno == EEXIST ? OB_FILE_ALREADY_EXIST : OB_IO_ERROR;
      } else {
        unlinkat(dir_, temporary_.c_str(), 0);
        temporary_.clear();
      }
    }
    return ret;
  }

private:
  ObExecContext &ctx_;
  int dir_, fd_;
  std::string name_, temporary_, null_;
  char delimiter_, quote_, escape_;
  ObArenaAllocator row_allocator_;
};
} // namespace
int create_copy_file_writer(ObExecContext &ctx, const ObString &path,
                            const ObString &options, ICopyFileWriter *&writer) {
  int ret = OB_SUCCESS;
  void *memory = ctx.get_allocator().alloc(sizeof(CopyFileWriter));
  writer = nullptr;
  if (memory == nullptr) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    auto *instance = new (memory) CopyFileWriter(ctx);
    ret = instance->init(path, options);
    if (OB_FAIL(ret)) {
      instance->~CopyFileWriter();
    } else {
      writer = instance;
    }
  }
  return ret;
}
} // namespace sql
} // namespace oceanbase
