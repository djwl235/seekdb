"""Runtime-value SQL diagnostics; prebuilt runs are not candidate acceptance."""
import argparse,json,datetime
from pathlib import Path
import pymysql
p=argparse.ArgumentParser();p.add_argument("--port",type=int,required=True);p.add_argument("--output",type=Path,required=True);p.add_argument("--reference-only",action="store_true");a=p.parse_args()
assert not a.output.exists()
if a.port in (12881,12882) and not a.reference_only:raise ValueError("prebuilt reference only")
a.output.mkdir(parents=True)
state={"scope":"reference runtime-value SQL diagnostic" if a.reference_only else "candidate runtime-value regression","checks":[]}
conn=pymysql.connect(host="127.0.0.1",port=a.port,user="root",charset="utf8mb4",autocommit=True)
cur=conn.cursor()
def scalar(sql,args=None):
    cur.execute(sql,args);return cur.fetchone()[0]
def token(sql):
    v=scalar(sql);return json.loads(v) if isinstance(v,(str,bytes)) else v
def check(name,actual,expected):
    item={"name":name,"actual":actual,"expected":expected,"passed":actual==expected};state["checks"].append(item)
    assert item["passed"],item
try:
    groups=[
        ("now",["SET timestamp=1700000000","SET timestamp=1731622400"],"CONCAT('time ',CAST(NOW() AS CHAR))"),
        ("last_insert_id",["SELECT LAST_INSERT_ID(141414)","SELECT LAST_INSERT_ID(252525)"],"CAST(LAST_INSERT_ID() AS CHAR)"),
        ("system_variable",["SET SESSION ob_query_timeout=1234567","SET SESSION ob_query_timeout=2345678"],"CAST(@@ob_query_timeout AS CHAR)"),
    ]
    for name,setters,expression in groups:
        observed=[]
        query="SELECT TOKENIZE("+expression+",'beng') AS tokens"
        for i,setter in enumerate(setters):
            cur.execute(setter)
            expected_value=scalar("SELECT "+expression)
            cur.execute("SET @runtime_expected=%s",(expected_value,))
            expected=token("SELECT TOKENIZE(@runtime_expected,'beng') AS tokens")
            actual=token(query)
            check(name+"_"+str(i),actual,expected);observed.append(actual)
        check(name+"_changes",observed[0]!=observed[1],True)
    ids=[]
    for i in range(2):
        other=pymysql.connect(host="127.0.0.1",port=a.port,user="root",charset="utf8mb4",autocommit=True)
        with other.cursor() as c:
            c.execute("SELECT CONNECTION_ID()");identity=c.fetchone()[0];ids.append(identity)
            c.execute("SET @runtime_expected=%s",(str(identity),))
            c.execute("SELECT TOKENIZE(@runtime_expected,'beng') AS tokens");expected=json.loads(c.fetchone()[0])
            c.execute("SELECT TOKENIZE(CAST(CONNECTION_ID() AS CHAR),'beng') AS tokens");actual=json.loads(c.fetchone()[0])
            check("connection_id_"+str(i),actual,expected)
        other.close()
    check("connection_ids_differ",ids[0]!=ids[1],True)
    state["status"]="passed"
except Exception as e:
    state["status"]="failed";state["error"]=repr(e);raise
finally:
    conn.close();state["finished_utc"]=datetime.datetime.now(datetime.timezone.utc).isoformat()
    (a.output/"result.json").write_text(json.dumps(state,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    print(json.dumps({"status":state["status"],"checks":len(state["checks"]),"scope":state["scope"]}))
