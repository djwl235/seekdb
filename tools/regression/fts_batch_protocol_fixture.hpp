// TEST_ONLY bounded interfaces for verbatim production-method extraction.
// Apache-2.0; not a replacement tokenizer, expression engine, DAS, or SQL test.
using int64_t = long;
constexpr int OB_SUCCESS=0, OB_ITER_END=100, OB_INVALID_ARGUMENT=-1, OB_ERR_UNEXPECTED=-2;
constexpr int OB_MALLOC_NORMAL_BLOCK_SIZE=0;
static int bad_line=0, convert_calls=0, convert_error_after=-1, lob_error=0, lob_calls=0;
#define OB_FAIL(x) ((ret=(x))!=OB_SUCCESS)
#define OB_SUCC(x) ((x)==OB_SUCCESS)
#define OB_UNLIKELY(x) (x)
#define OB_ISNULL(x) ((x)==nullptr)
#define OB_NOT_NULL(x) ((x)!=nullptr)
#define MY_SPEC spec
#define REQUIRE(x) do { if (!(x)) return __LINE__; } while (0)
static void guard(bool x,int line) { if (!x && !bad_line) bad_line=line; }
template<typename T> T min(T a,T b) { return a<b?a:b; }
enum ObExprOperatorType {T_FUN_SYS_WORD_SEGMENT=1,T_FUN_SYS_DOC_ID=2,T_FUN_SYS_WORD_COUNT=3,T_FUN_SYS_DOC_LENGTH=4,T_REF_COLUMN=5};
enum ObObjDatumMapType {OBJ_DATUM_STRING,OBJ_DATUM_8BYTE_DATA};
namespace share { struct ObFtsIndexBuilderUtil { static constexpr int64_t OB_FTS_INDEX_OR_DOC_WORD_TABLE_COL_CNT=4; }; }
namespace ObModIds { constexpr int OB_LOB_ACCESS_BUFFER=0; }
struct ObArenaAllocator { ObArenaAllocator(int,int) {} };
struct ObString { int64_t words=0; };
struct ObDatum {
  int64_t value=0; bool null=false;
  bool is_null() const { return null; }
  ObString get_string() const { return {value}; }
  int from_storage_datum(const ObDatum &other,ObObjDatumMapType) {
    if (convert_error_after>=0 && convert_calls++==convert_error_after) return -303;
    *this=other; return 0;
  }
};
namespace blocksstable { struct ObDatumRow { ObDatum storage_datums_[4]; }; }
struct Flags {
  bool bits[256]={}; int64_t capacity=256;
  void set_all(int64_t n) { guard(n<=capacity,__LINE__); for(int64_t i=0;i<n && i<256;++i) bits[i]=true; }
  void unset(int64_t i) { guard(i>=0 && i<capacity,__LINE__); if(i>=0 && i<256)bits[i]=false; }
  void reset(int64_t n) { guard(n<=capacity,__LINE__); for(int64_t i=0;i<n && i<256;++i)bits[i]=false; }
  bool at(int64_t i) const { guard(i>=0 && i<capacity,__LINE__); return i>=0 && i<256?bits[i]:false; }
};
struct ObEvalInfo { bool evaluated_=false,projected_=false; void clear_evaluated_flag(){evaluated_=false;projected_=false;} };
struct Doc { int64_t id=0,words=0,pass=0; bool skip=false; };
struct ObEvalCtx {
  int64_t idx=0,size=1; Doc source;
  struct BatchInfoScopeGuard {
    ObEvalCtx &ctx; int64_t idx,size;
    BatchInfoScopeGuard(ObEvalCtx &c):ctx(c),idx(c.idx),size(c.size){}
    ~BatchInfoScopeGuard(){ctx.idx=idx;ctx.size=size;}
    void set_batch_idx(int64_t i){ctx.idx=i;} void set_batch_size(int64_t n){ctx.size=n;}
  };
};
struct Meta { bool has_lob_header()const{return true;} };
struct ObExpr {
  ObExprOperatorType type_=T_REF_COLUMN; bool batch=true; int error=0;
  ObDatum datums[256]; ObEvalInfo info; Flags flags; Meta datum_meta_,obj_meta_;
  bool is_batch_result()const{return batch;}
  ObDatum &locate_datum_for_write(ObEvalCtx &c){guard(c.idx>=0 && c.idx<(batch?256:1),__LINE__);return datums[batch?c.idx:0];}
  ObDatum *locate_batch_datums(ObEvalCtx &){guard(batch,__LINE__);return datums;}
  ObEvalInfo &get_eval_info(ObEvalCtx &){return info;}
  Flags &get_evaluated_flags(ObEvalCtx &){guard(batch,__LINE__);return flags;}
  void clear_evaluated_flag(ObEvalCtx &c){if(batch)flags.unset(c.idx);else info.clear_evaluated_flag();}
  void set_evaluated_projected(ObEvalCtx &){info.evaluated_=true;info.projected_=true;}
  int eval(ObEvalCtx &c,ObDatum *&out){
    if(error)return error;
    out=&datums[batch?c.idx:0];
    if(info.projected_ || (info.evaluated_ && (!batch || flags.at(c.idx))))return 0;
    out->null=false;
    if(type_==T_FUN_SYS_WORD_SEGMENT){out->null=c.source.words==-1;out->value=c.source.words;}
    else if(type_==T_FUN_SYS_DOC_ID)out->value=c.source.id;
    else if(type_==T_REF_COLUMN)out->value=c.source.pass;
    else out->value=0;
    info.evaluated_=true;if(batch)flags.bits[c.idx]=true;return 0;
  }
};
struct Output { ObExpr *items[5]={}; int64_t count()const{return 5;} ObExpr *at(int64_t i)const{return items[i];} };
struct Spec { bool is_fts_index_aux_=true; int64_t max_batch_size_=256; Output output_; };
struct Batch { int64_t size_=0;bool end_=false;Flags *skip_;void reset_skip(int64_t n){skip_->reset(n);} };
struct ObTextStringHelper {
  static int read_real_string_data(ObEvalCtx &,ObArenaAllocator &,ObDatum &,Meta,bool,ObString &){++lob_calls;return lob_error;}
};
struct ObFTIndexRowCache {
  static ObObjDatumMapType FTS_INDEX_TYPES[4],FTS_DOC_WORD_TYPES[4];
  static ObExprOperatorType FTS_INDEX_EXPR_TYPE[4],FTS_DOC_WORD_EXPR_TYPE[4];
  blocksstable::ObDatumRow rows[600]; int64_t count=0,idx=0;bool aux=true;
  int segment_error=0,posting_error_after=-1,segments=0;
  void reuse(){count=0;idx=0;}
  int segment(Meta &,ObDatum &id,ObString text){
    reuse();++segments;if(segment_error)return segment_error;
    guard(text.words<=600,__LINE__);if(text.words<=0)return OB_ITER_END;
    count=text.words;
    for(int64_t i=0;i<count;++i){auto &d=rows[i].storage_datums_;d[aux?0:1]={i,false};d[aux?1:0]=id;d[2]={1,false};d[3]={count,false};}
    return 0;
  }
  int get_next_row(blocksstable::ObDatumRow *&row){
    if(idx==posting_error_after)return -305;
    if(idx>=count)return OB_ITER_END;row=&rows[idx++];return 0;
  }
};
ObObjDatumMapType ObFTIndexRowCache::FTS_INDEX_TYPES[]={OBJ_DATUM_STRING,OBJ_DATUM_STRING,OBJ_DATUM_8BYTE_DATA,OBJ_DATUM_8BYTE_DATA};
ObObjDatumMapType ObFTIndexRowCache::FTS_DOC_WORD_TYPES[]={OBJ_DATUM_STRING,OBJ_DATUM_STRING,OBJ_DATUM_8BYTE_DATA,OBJ_DATUM_8BYTE_DATA};
ObExprOperatorType ObFTIndexRowCache::FTS_INDEX_EXPR_TYPE[]={T_FUN_SYS_WORD_SEGMENT,T_FUN_SYS_DOC_ID,T_FUN_SYS_WORD_COUNT,T_FUN_SYS_DOC_LENGTH};
ObExprOperatorType ObFTIndexRowCache::FTS_DOC_WORD_EXPR_TYPE[]={T_FUN_SYS_DOC_ID,T_FUN_SYS_WORD_SEGMENT,T_FUN_SYS_WORD_COUNT,T_FUN_SYS_DOC_LENGTH};
struct FakeIter { int error=0;bool has_task(){return false;}int reuse(){return error;} };
struct FakeRand { void reset(){} };
struct ObOperator { static int inner_rescan(){return 0;} };
struct ObTableScanOp:ObOperator {
  Spec spec;ObEvalCtx eval_ctx_;ObFTIndexRowCache fts_index_;ObExpr exprs[5];Flags skip;Batch brs_;
  ObExpr *fts_output_exprs_[4]={};bool fts_output_exprs_inited_=false,iter_end_=false,in_rescan_=false,need_init_before_get_row_=false;
  FakeIter iterator,*scan_iter_=&iterator;FakeRand rand_scan_processor_;
  Doc docs[10];int64_t doc_count=0,doc_idx=0,source_calls=0;bool last_has_end=true;int status_error=0,source_error=0;
  ObTableScanOp(bool aux=true){
    spec.is_fts_index_aux_=aux;fts_index_.aux=aux;brs_.skip_=&skip;
    auto *types=aux?ObFTIndexRowCache::FTS_INDEX_EXPR_TYPE:ObFTIndexRowCache::FTS_DOC_WORD_EXPR_TYPE;
    for(int i=0;i<4;++i){exprs[i].type_=types[i];spec.output_.items[i]=&exprs[i];}
    exprs[4].type_=T_REF_COLUMN;spec.output_.items[4]=&exprs[4];
  }
  int try_check_status(){return status_error;}
  ObEvalCtx &get_exec_ctx(){return eval_ctx_;}
  int fill_storage_feedback_info(){return 0;}
  int inner_rescan_for_tsc(){doc_idx=0;iter_end_=false;return 0;}
  int inner_get_next_batch_for_tsc(int64_t capacity){
    guard(capacity==1,__LINE__);++source_calls;if(source_error)return source_error;
    for(auto &e:exprs){e.info.clear_evaluated_flag();if(e.batch)e.flags.reset(256);}
    brs_.reset_skip(1);
    if(doc_idx>=doc_count){brs_.size_=0;brs_.end_=true;iter_end_=true;return 0;}
    eval_ctx_.source=docs[doc_idx++];
    brs_.end_=last_has_end && doc_idx==doc_count;iter_end_=brs_.end_;
    brs_.size_=eval_ctx_.source.words==-2?0:1;skip.bits[0]=eval_ctx_.source.skip;
    return 0;
  }
  int inner_get_next_fts_index_batch(int64_t);
  int fetch_next_fts_index_batch_doc();int fill_fts_passthrough_cols(int64_t);
  void reset_fts_output_exprs();int init_fts_output_exprs();
  int fill_generated_fts_cols(blocksstable::ObDatumRow *);
  int get_output_fts_col_expr_by_type(const ObExprOperatorType &,ObExpr *&);
  int inner_rescan();int inner_close();
};

/* PRODUCTION_METHODS */

static int drain(ObTableScanOp &op,int64_t cap,int64_t first_id,int64_t first_words,int64_t first_pass,int64_t second_words=0){
  int64_t total=0;bool ended=false;
  for(int n=0;n<1000 && !ended;++n){
    REQUIRE(op.inner_get_next_fts_index_batch(cap)==0);
    REQUIRE(op.brs_.size_<=cap && op.brs_.size_<=op.spec.max_batch_size_);
    if(op.brs_.end_){REQUIRE(op.brs_.size_==0);ended=true;break;}
    REQUIRE(op.brs_.size_>0);
    int64_t batch_id=-1;
    for(int64_t i=0;i<op.brs_.size_;++i){
      auto at=[&](int col)->ObDatum&{return op.exprs[col].datums[op.exprs[col].batch?i:0];};
      const int wi=op.spec.is_fts_index_aux_?0:1,di=op.spec.is_fts_index_aux_?1:0;
      int64_t id=at(di).value;if(batch_id<0)batch_id=id;REQUIRE(batch_id==id);
      bool second=total>=first_words;REQUIRE(id==(second?first_id+1:first_id));
      REQUIRE(at(wi).value==(second?total-first_words:total));
      REQUIRE(at(2).value==1 && at(3).value==(second?second_words:first_words));
      REQUIRE(at(4).value==(second?first_pass+1:first_pass));
      REQUIRE(!op.brs_.skip_->at(i));
      for(int col=0;col<5;++col){REQUIRE(op.exprs[col].info.projected_);if(op.exprs[col].batch)REQUIRE(op.exprs[col].flags.at(i));}
      ++total;
    }
  }
  REQUIRE(ended && total==first_words+second_words);return 0;
}
static int run_impl(int c){
  bad_line=0;convert_calls=0;convert_error_after=-1;lob_error=0;lob_calls=0;
  ObTableScanOp op(c<3 || c>=6);
  if(c<9){
    op.docs[0]={1,7,1,true};op.docs[1]={2,0,2,false};op.docs[2]={3,-1,3,false};op.docs[3]={4,-2,4,false};
    op.docs[4]={11,515,110,false};op.docs[5]={12,3,111,false};op.doc_count=6;
    if(c==6)op.exprs[2].batch=false;
    if(c==7)op.exprs[4].batch=false;
    if(c==8)op.last_has_end=false;
    int64_t cap=c<6?(c%3==0?1:c%3==1?2:256):256;
    int result=drain(op,cap,11,515,110,3);if(result)return result;
    REQUIRE(op.source_calls==(op.last_has_end?6:7));
    REQUIRE(op.fts_index_.segments==3);REQUIRE(lob_calls==3);
  }else{
    op.docs[0]={31,5,310,false};op.doc_count=1;
    if(c==9 || c==10){
      REQUIRE(op.inner_get_next_fts_index_batch(2)==0 && op.brs_.size_==2);
      if(c==9){op.docs[0]={77,3,900,false};REQUIRE(op.inner_rescan()==0);REQUIRE(!op.fts_output_exprs_inited_ && op.fts_index_.count==0);int r=drain(op,2,77,3,900);if(r)return r;}
      else {op.iterator.error=-307;REQUIRE(op.inner_close()==-307);REQUIRE(!op.fts_output_exprs_inited_ && op.fts_index_.count==0);}
    }else if(c==18){
      auto saved=op.spec.output_.items[2];op.spec.output_.items[2]=nullptr;
      REQUIRE(op.inner_get_next_fts_index_batch(2)==OB_ERR_UNEXPECTED);REQUIRE(op.brs_.size_==0 && !op.fts_output_exprs_inited_);
      for(auto *p:op.fts_output_exprs_)REQUIRE(p==nullptr);
      op.spec.output_.items[2]=saved;int r=drain(op,2,31,5,310);if(r)return r;
    }else if(c==19){REQUIRE(op.inner_get_next_fts_index_batch(0)==OB_INVALID_ARGUMENT);REQUIRE(op.brs_.size_==0 && op.source_calls==0);}
    else {
      if(c==11)op.status_error=-301;
      if(c==12)op.fts_index_.segment_error=-302;
      if(c==13)op.fts_index_.posting_error_after=2;
      if(c==14)convert_error_after=5;
      if(c==15)lob_error=-304;
      if(c==16)op.exprs[op.spec.is_fts_index_aux_?0:1].error=-306;
      if(c==17)op.exprs[4].error=-308;
      REQUIRE(op.inner_get_next_fts_index_batch(256)<0);REQUIRE(op.brs_.size_==0 && !op.brs_.end_);
    }
  }
  return bad_line;
}
extern "C" int run_case(int c){return run_impl(c);}
