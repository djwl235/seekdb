"""Differential TOKENIZE regression: real SQL execution, independent of optimizer implementation."""
import argparse, datetime, hashlib, json
from pathlib import Path
import pymysql
p=argparse.ArgumentParser()
p.add_argument("--port",type=int,required=True)
p.add_argument("--output",type=Path,required=True)
p.add_argument("--source-sha",required=True)
p.add_argument("--reference-only",action="store_true")
a=p.parse_args()
assert not a.output.exists(), "preserve previous evidence"
if a.port in (12881,12882) and not a.reference_only:
    raise ValueError("shared/prebuilt server is diagnostic only")
a.output.mkdir(parents=True)
state={"scope":"reference SQL/harness diagnostic only" if a.reference_only else "candidate differential SQL regression",
       "source_sha":a.source_sha,"port":a.port,"started_utc":datetime.datetime.now(datetime.timezone.utc).isoformat(),"checks":[]}
conn=pymysql.connect(host="127.0.0.1",port=a.port,user="root",charset="utf8mb4",autocommit=True,connect_timeout=5)
cur=conn.cursor()
def scalar(sql,args=None):
    cur.execute(sql,args)
    result=cur.fetchone()[0]
    return json.loads(result) if isinstance(result,(str,bytes)) else result
def outcome(sql,args=None):
    try:return {"value":scalar(sql,args)}
    except pymysql.MySQLError as e:return {"error_code":e.args[0]}
def check(name,lhs,rhs):
    record={"name":name,"actual":lhs,"expected":rhs,"passed":lhs==rhs}
    state["checks"].append(record)
    assert record["passed"],record
try:
    cur.execute("SELECT version(),@@version_comment,@@collation_connection")
    state["server"]=cur.fetchone()
    cases=[
      ("Alpha Beta","beng","[]"),("Gamma Delta","beng","[]"),
      ("alpha alphabet","space",'[{"output":"all"}]'),
      ("alpha alphabet","space",'[{"additional_args":[{"min_token_size":6}]}]'),
      ("中国数据库","ik","[]"),("中国数据库","ngram","[]"),
      ("","space","[]"),(None,"space","[]"),
      ("Echo café","beng","[]"),("Echo café","space","[]"),
      ("final value","not_a_parser","[]"),("final value","space",'[{"output":"bad"}]'),
      ("final value","space","{bad"),("final value","space",'[{"stopwords":"x"}]')
    ]
    # Repeated text protocol SQL has identical shape but changing literal values.
    # Oracle reads changing user variables, which must be evaluated each execution.
    for collation in ("utf8mb4_bin","utf8mb4_general_ci"):
        cur.execute("SET NAMES utf8mb4 COLLATE "+collation)
        for turn,(text,parser,props) in enumerate(cases+list(reversed(cases))):
            cur.execute("SET @token_text=%s,@token_parser=%s,@token_props=%s",(text,parser,props))
            dynamic=outcome("SELECT TOKENIZE(@token_text,@token_parser,@token_props) AS tokens")
            literal=outcome("SELECT TOKENIZE(%s,%s,%s) AS tokens",(text,parser,props))
            check(f"literal_vs_variable_{collation}_{turn}",literal,dynamic)
        for first,last in (("Alpha ","Beta"),("Gamma ","Delta"),("Deep ","Blue")):
            cur.execute("SET @token_text=%s",(first+last,))
            check("nested_literal_"+collation+first,
                  outcome("SELECT TOKENIZE(CONCAT(%s,%s),'beng') AS tokens",(first,last)),
                  outcome("SELECT TOKENIZE(@token_text,'beng') AS tokens"))
        cur.execute("PREPARE tokenize_regression FROM 'SELECT TOKENIZE(?,?,?) AS tokens'")
        for turn,(text,parser,props) in enumerate(cases[:10]):
            cur.execute("SET @token_text=%s,@token_parser=%s,@token_props=%s",(text,parser,props))
            check(f"prepared_{collation}_{turn}",
                  outcome("EXECUTE tokenize_regression USING @token_text,@token_parser,@token_props"),
                  outcome("SELECT TOKENIZE(@token_text,@token_parser,@token_props) AS tokens"))
        cur.execute("DEALLOCATE PREPARE tokenize_regression")
    state["status"]="passed"
except Exception as e:
    state["status"]="failed";state["error"]=repr(e);raise
finally:
    conn.close()
    state["finished_utc"]=datetime.datetime.now(datetime.timezone.utc).isoformat()
    (a.output/"result.json").write_text(json.dumps(state,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    print(json.dumps({"status":state["status"],"checks":len(state["checks"]),"scope":state["scope"]}))
