// Copyright (c) 2026 OceanBase.
// Licensed under the Apache License, Version 2.0.
// File decoding only: SQL predicates, joins and aggregates stay in seekdb.
use parquet::{file::{reader::FileReader, serialized_reader::SerializedFileReader}, record::{Field, reader::RowIter}};
use std::{collections::HashSet, fs::File, io::{BufRead, BufReader, Read, Seek, SeekFrom}, os::fd::FromRawFd};
use serde_json::Value;

#[cxx::bridge(namespace = "oceanbase::sql::file_sql")]
pub mod ffi {
    #[derive(Clone)]
    struct Column { name: String, kind: u8, nullable: bool, scale: i16 }
    #[derive(Clone)]
    struct Cell { kind: u8, integer: i64, real: f64, text: String }
    extern "Rust" {
        type FileScan;
        fn open_scan(fd: i32, format: &str, options: &str) -> Box<FileScan>;
        fn status(self: &FileScan) -> i32;
        fn error(self: &FileScan) -> &str;
        fn columns(self: &FileScan) -> &[Column];
        fn next(self: &mut FileScan) -> i32;
        fn row(self: &FileScan) -> &[Cell];
        fn rewind(self: &mut FileScan) -> i32;
    }
}
const NULL:u8=0; const INT:u8=1; const DOUBLE:u8=2; const BOOL:u8=3;
const TEXT:u8=4; const DATE:u8=5; const DATETIME:u8=6;
const MAX_ROW:usize=16*1024*1024; const MAX_COLUMNS:usize=4096;
type Result<T> = std::result::Result<T,String>;
fn cell(kind:u8, text:String) -> ffi::Cell { ffi::Cell {kind, integer:0,real:0.0,text} }
fn null() -> ffi::Cell { cell(NULL,String::new()) }
fn date_type(s:&str) -> u8 {
    let b=s.as_bytes();
    if b.len()<10 || b[4]!=b'-' || b[7]!=b'-' { return TEXT; }
    let number=|r:std::ops::Range<usize>| -> Option<u32> { s.get(r)?.parse().ok() };
    let (Some(y),Some(m),Some(d))=(number(0..4),number(5..7),number(8..10)) else{return TEXT};
    if y==0 || m==0 || m>12 {return TEXT;}
    let md=[31,if y%4==0&&(y%100!=0||y%400==0){29}else{28},31,30,31,30,31,31,30,31,30,31];
    if d==0||d>md[(m-1)as usize] {return TEXT;}
    if b.len()==10 {return DATE;}
    if b.len()<19 || !matches!(b[10],b'T'|b' ') || b[13]!=b':' || b[16]!=b':' {return TEXT;}
    let (Some(h),Some(n),Some(sec))=(number(11..13),number(14..16),number(17..19)) else{return TEXT};
    if h>23||n>59||sec>59 {return TEXT;}
    if b.len()>19 && (b[19]!=b'.'||b.len()>26||b.len()==20||!b[20..].iter().all(u8::is_ascii_digit)) {return TEXT;}
    DATETIME
}
fn scalar(s:String, infer:bool) -> ffi::Cell {
    if !infer {return cell(TEXT,s);}
    if s.eq_ignore_ascii_case("true")||s.eq_ignore_ascii_case("false") {
        let mut v=cell(BOOL,s.clone());v.integer=i64::from(s.eq_ignore_ascii_case("true"));return v;
    }
    if let Ok(n)=s.parse::<i64>() {let mut v=cell(INT,s);v.integer=n;return v;}
    // An overflowing integer must not silently lose precision through f64.
    let digits=s.trim_start_matches(['+','-']);
    if !digits.is_empty()&&digits.bytes().all(|c|c.is_ascii_digit()) {return cell(TEXT,s);}
    if let Ok(n)=s.parse::<f64>() {if n.is_finite(){let mut v=cell(DOUBLE,s);v.real=n;return v;}}
    cell(date_type(&s),s)
}
fn json_cell(v:Value)->Result<ffi::Cell>{
    Ok(match v {
        Value::Null=>null(), Value::String(s)=>scalar(s,true),
        Value::Bool(b)=>{let mut c=cell(BOOL,b.to_string());c.integer=i64::from(b);c},
        Value::Number(n)=>scalar(n.to_string(),true),
        _=>return Err("nested JSON values and arrays are not supported".into()),
    })
}
fn widen(a:u8,b:u8)->u8 {
    if a==NULL{return b;} if b==NULL||a==b{return a;}
    if matches!((a,b),(INT,BOOL)|(BOOL,INT)){return INT;}
    if matches!(a,INT|DOUBLE|BOOL)&&matches!(b,INT|DOUBLE|BOOL){return DOUBLE;}
    if matches!(a,DATE|DATETIME)&&matches!(b,DATE|DATETIME){return DATETIME;}
    TEXT
}
#[derive(Clone)]
struct Options {header:bool,delimiter:u8,quote:u8,escape:u8,nullstr:String}
impl Options {
    fn parse(s:&str)->Result<Self>{
        let v:Value=serde_json::from_str(s).map_err(|e|e.to_string())?;
        let mut o=Self{header:true,delimiter:b',',quote:b'"',escape:b'"',nullstr:String::new()};
        if let Some(h)=v.get("header"){o.header=h.as_bool().ok_or("header must be boolean")?;}
        for (key,target) in [("delimiter",&mut o.delimiter),("quote",&mut o.quote),("escape",&mut o.escape)] {
            if let Some(value)=v.get(key){let s=value.as_str().ok_or("CSV option must be a string")?;
                if s.len()!=1||!s.is_ascii()||s=="\n"||s=="\r" {return Err("CSV delimiter/quote/escape must be one ASCII character".into());}
                *target=s.as_bytes()[0];}
        }
        if let Some(n)=v.get("nullstr"){o.nullstr=n.as_str().ok_or("nullstr must be a string")?.into();}
        if o.delimiter==o.quote{return Err("delimiter and quote must differ".into());}
        Ok(o)
    }
}
struct Csv { reader:BufReader<File>, options:Options, line:u64 }
impl Csv {
    fn record(&mut self)->Result<Option<Vec<(String,bool)>>> {
        let mut fields=vec![];let mut field=vec![];let mut quoted=false;let mut in_quote=false;let mut closed=false;let mut seen=false;let mut bytes=0;
        loop {
            let buffer=self.reader.fill_buf().map_err(|e|e.to_string())?;
            if buffer.is_empty(){if in_quote{return Err(format!("unterminated CSV quote at line {}",self.line));}
                if !seen{return Ok(None);} fields.push((String::from_utf8(field).map_err(|e|e.to_string())?,quoted));break;}
            let ch=buffer[0];self.reader.consume(1);seen=true;bytes+=1;
            if bytes>MAX_ROW{return Err("CSV row exceeds resource limit".into());}
            if ch==b'\n'{self.line+=1;}
            if in_quote {
                if ch==self.options.escape && self.options.escape!=self.options.quote {
                    let b=self.reader.fill_buf().map_err(|e|e.to_string())?;
                    if b.is_empty(){return Err("truncated CSV escape".into());}field.push(b[0]);self.reader.consume(1);
                }else if ch==self.options.quote {
                    let b=self.reader.fill_buf().map_err(|e|e.to_string())?;
                    if b.first()==Some(&self.options.quote)&&self.options.escape==self.options.quote {field.push(ch);self.reader.consume(1);}
                    else{in_quote=false;closed=true;}
                }else{field.push(ch);}
            }else if ch==self.options.delimiter {
                fields.push((String::from_utf8(std::mem::take(&mut field)).map_err(|e|e.to_string())?,quoted));quoted=false;closed=false;
            }else if ch==b'\n'||ch==b'\r' {
                if ch==b'\r' {let b=self.reader.fill_buf().map_err(|e|e.to_string())?;if b.first()==Some(&b'\n'){self.reader.consume(1);self.line+=1;}}
                fields.push((String::from_utf8(field).map_err(|e|e.to_string())?,quoted));break;
            }else if ch==self.options.quote&&field.is_empty()&&!closed {quoted=true;in_quote=true;}
            else if closed {return Err(format!("unexpected bytes after CSV closing quote at line {}",self.line));}
            else{field.push(ch);}
            if field.len()>MAX_ROW||fields.len()>MAX_COLUMNS{return Err("CSV row exceeds resource limit".into());}
        }
        if fields.len()>MAX_COLUMNS{return Err("too many CSV columns".into());}
        Ok(Some(fields))
    }
}
enum Input { Csv(Csv), Json(BufReader<File>), Parquet(RowIter<'static>) }
pub struct FileScan { file:Option<File>,format:String,options:Options,input:Option<Input>,columns:Vec<ffi::Column>,row:Vec<ffi::Cell>,status:i32,error:String,line:u64 }
impl FileScan {
    fn reset(&mut self)->Result<()> {
        self.input=None;self.line=0;
        let f=self.file.as_mut().ok_or("file is not open")?;
        f.seek(SeekFrom::Start(0)).map_err(|e|e.to_string())?;
        let file=f.try_clone().map_err(|e|e.to_string())?;
        self.input=Some(match self.format.as_str(){
            "csv"=>{let mut csv=Csv{reader:BufReader::new(file),options:self.options.clone(),line:1};
                if self.options.header {csv.record()?.ok_or("CSV has no header")?;}Input::Csv(csv)},
            "json"=>Input::Json(BufReader::new(file)),
            "parquet"=>{let reader=SerializedFileReader::new(file).map_err(|e|e.to_string())?;
                Input::Parquet(RowIter::from_file_into(Box::new(reader)).with_batch_size(1024))},
            _=>return Err("unsupported file format".into()),
        });Ok(())
    }
    fn read_row(&mut self)->Result<Option<Vec<(String,ffi::Cell)>>> {
        self.line+=1;
        match self.input.as_mut().ok_or("scan is not open")? {
            Input::Csv(csv)=>{
                let Some(fields)=csv.record()? else{return Ok(None)};
                if !self.columns.is_empty()&&fields.len()!=self.columns.len(){return Err(format!("CSV column count mismatch at record {}",self.line));}
                Ok(Some(fields.into_iter().enumerate().map(|(i,(s,q))|{
                    let name=self.columns.get(i).map(|c|c.name.clone()).unwrap_or_else(||format!("column{}",i));
                    let c=if !q&&s==self.options.nullstr{null()}else{scalar(s,true)};(name,c)
                }).collect()))
            },
            Input::Json(reader)=>{
                let mut buf=String::new();
                loop {buf.clear();let n=reader.by_ref().take((MAX_ROW+1)as u64).read_line(&mut buf).map_err(|e|e.to_string())?;
                    if n==0{return Ok(None);}if n>MAX_ROW{return Err("JSONL record exceeds resource limit".into());}if !buf.trim().is_empty(){break;}}
                let v:Value=serde_json::from_str(&buf).map_err(|e|format!("JSONL record {}: {}",self.line,e))?;
                let Value::Object(obj)=v else{return Err("JSONL records must be flat objects".into())};
                if obj.len()>MAX_COLUMNS{return Err("too many JSONL columns".into());}
                obj.into_iter().map(|(n,v)|Ok((n,json_cell(v)?))).collect::<Result<Vec<_>>>().map(Some)
            },
            Input::Parquet(rows)=>{
                let Some(row)=rows.next() else{return Ok(None)};let row=row.map_err(|e|e.to_string())?;
                row.get_column_iter().map(|(n,v)|{
                    let c=match v {
                        Field::Null=>null(), Field::Bool(v)=>{let mut c=cell(BOOL,v.to_string());c.integer=i64::from(*v);c},
                        Field::Byte(v)=>scalar(v.to_string(),true), Field::Short(v)=>scalar(v.to_string(),true),Field::Int(v)=>scalar(v.to_string(),true),Field::Long(v)=>scalar(v.to_string(),true),
                        Field::UByte(v)=>scalar(v.to_string(),true),Field::UShort(v)=>scalar(v.to_string(),true),Field::UInt(v)=>scalar(v.to_string(),true),Field::ULong(v)=>scalar(v.to_string(),true),
                        Field::Float(v)=>scalar(v.to_string(),true),Field::Double(v)=>scalar(v.to_string(),true),
                        Field::Str(v)=>scalar(v.clone(),false),Field::Date(_)=>cell(DATE,v.to_string()),
                        Field::TimestampMillis(_)|Field::TimestampMicros(_)=>cell(DATETIME,v.to_string()),
                        Field::Bytes(v)=>cell(TEXT,std::str::from_utf8(v.data()).map_err(|_|"Parquet bytes are not UTF-8")?.into()),
                        _=>return Err("nested or unsupported Parquet type".into()),
                    };Ok((n.clone(),c))
                }).collect::<Result<Vec<_>>>().map(Some)
            }
        }
    }
    fn infer(&mut self)->Result<()> {
        if self.format=="csv"&&self.options.header {
            let f=self.file.as_mut().ok_or("file not open")?;f.seek(SeekFrom::Start(0)).map_err(|e|e.to_string())?;
            let mut csv=Csv{reader:BufReader::new(f.try_clone().map_err(|e|e.to_string())?),options:self.options.clone(),line:1};
            let names=csv.record()?.ok_or("CSV has no header")?;let mut used=HashSet::new();
            for (i,(name,_)) in names.into_iter().enumerate(){
                let name=if i==0{name.trim_start_matches('\u{feff}').to_string()}else{name};
                let base=if name.is_empty(){format!("column{}",i)}else{name};let mut n=base.clone();let mut suffix=2;
                while !used.insert(n.to_lowercase()){n=format!("{}__{}",base,suffix);suffix+=1;}
                self.columns.push(ffi::Column{name:n,kind:NULL,nullable:false,scale:0});
            }
        }
        if self.format=="parquet" {
            let f=self.file.as_mut().ok_or("file not open")?;f.seek(SeekFrom::Start(0)).map_err(|e|e.to_string())?;
            let r=SerializedFileReader::new(f.try_clone().map_err(|e|e.to_string())?).map_err(|e|e.to_string())?;
            let schema=r.metadata().file_metadata().schema_descr();
            for column in schema.columns(){
                if column.path().parts().len()!=1||column.max_rep_level()>0{return Err("nested Parquet schemas are not supported".into());}
                use parquet::basic::{Type,ConvertedType};
                let kind=match column.converted_type(){ConvertedType::DATE=>DATE,ConvertedType::TIMESTAMP_MILLIS|ConvertedType::TIMESTAMP_MICROS=>DATETIME,_=>match column.physical_type(){Type::BOOLEAN=>BOOL,Type::INT32|Type::INT64=>INT,Type::FLOAT|Type::DOUBLE=>DOUBLE,Type::BYTE_ARRAY=>TEXT,_=>return Err("unsupported Parquet physical type".into())}};
                self.columns.push(ffi::Column{name:column.name().into(),kind,nullable:column.max_def_level()>0,scale:0});
            }
        }
        self.reset()?;let mut count=0;
        while let Some(row)=self.read_row()? {
            for column in &mut self.columns {if !row.iter().any(|(n,_)|n==&column.name){column.nullable=true;}}
            for (name,value) in row {
                let idx=if let Some(i)=self.columns.iter().position(|c|c.name==name){i}else{
                    if self.columns.len()>=MAX_COLUMNS{return Err("too many file columns".into());}
                    self.columns.push(ffi::Column{name,kind:NULL,nullable:count>0,scale:0});self.columns.len()-1};
                let c=&mut self.columns[idx];c.kind=widen(c.kind,value.kind);c.nullable|=value.kind==NULL;
                if value.kind==DATETIME {if let Some(dot)=value.text.find('.'){c.scale=c.scale.max((value.text.len()-dot-1).min(6)as i16);}}
            }count+=1;
        }
        if self.columns.is_empty(){return Err("file has no inferable columns".into());}
        for c in &mut self.columns{if c.kind==NULL{c.kind=TEXT;c.nullable=true;}}
        self.reset()
    }
    fn fail(&mut self,e:String)->i32{self.error=e;self.status=-1;self.input=None;-1}
    pub fn status(&self)->i32{self.status}
    pub fn error(&self)->&str{&self.error}
    pub fn columns(&self)->&[ffi::Column]{&self.columns}
    pub fn row(&self)->&[ffi::Cell]{&self.row}
    pub fn rewind(&mut self)->i32{self.row.clear();match self.reset(){Ok(())=>{self.status=0;0},Err(e)=>self.fail(e)}}
    pub fn next(&mut self)->i32 {
        self.row.clear();if self.status!=0{return self.status;}
        match self.read_row(){Ok(Some(row))=>{
            for c in &self.columns {let v=row.iter().find(|(n,_)|n==&c.name).map(|(_,v)|v.clone()).unwrap_or_else(null);self.row.push(v);}0
        },Ok(None)=>{self.status=1;1},Err(e)=>self.fail(e)}
    }
}
pub fn open_scan(fd:i32,format:&str,options:&str)->Box<FileScan> {
    let mut scan=Box::new(FileScan{file:None,format:format.into(),options:Options{header:true,delimiter:b',',quote:b'"',escape:b'"',nullstr:String::new()},input:None,columns:vec![],row:vec![],status:0,error:String::new(),line:0});
    let result=(||{
        scan.options=Options::parse(options)?;
        // Own a duplicate, never close the caller's descriptor. The caller has
        // already canonicalized and authorized this exact open file.
        let owned=unsafe{libc::dup(fd)};if owned<0{return Err(std::io::Error::last_os_error().to_string());}
        scan.file=Some(unsafe{File::from_raw_fd(owned)});
        scan.infer()
    })();if let Err(e)=result{scan.fail(e);}scan
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{io::Write, os::fd::AsRawFd, sync::atomic::{AtomicU64,Ordering}};
    static SEQUENCE:AtomicU64=AtomicU64::new(0);
    fn read(bytes:&[u8],format:&str,options:&str)->Box<FileScan>{
        let path=std::env::temp_dir().join(format!("seekdb-file-sql-{}-{}",std::process::id(),SEQUENCE.fetch_add(1,Ordering::Relaxed)));
        let mut f=std::fs::OpenOptions::new().read(true).write(true).create_new(true).open(&path).unwrap();
        f.write_all(bytes).unwrap();let scan=open_scan(f.as_raw_fd(),format,options);std::fs::remove_file(path).unwrap();scan
    }
    #[test]
    fn csv_quotes_types_nulls_and_rescan(){
        let mut s=read(b"\xef\xbb\xbfid,id,note,day,active,score\r\n1,11,\"a,\r\nb\",2024-02-29,true,1.5\r\n2,12,\"say \"\"hi\"\"\",,false,2\r\n", "csv","{}");
        assert_eq!(s.status(),0,"{}",s.error());assert_eq!(s.columns()[1].name,"id__2");
        assert_eq!(s.columns().iter().map(|c|c.kind).collect::<Vec<_>>(),vec![INT,INT,TEXT,DATE,BOOL,DOUBLE]);
        assert!(s.columns()[3].nullable);assert_eq!(s.next(),0);assert_eq!(s.row()[2].text,"a,\r\nb");
        assert_eq!(s.next(),0);assert_eq!(s.row()[2].text,"say \"hi\"");assert_eq!(s.row()[3].kind,NULL);
        assert_eq!(s.next(),1);assert_eq!(s.rewind(),0);assert_eq!(s.next(),0);assert_eq!(s.row()[0].integer,1);
    }
    #[test]
    fn json_union_and_widening(){
        let mut s=read(b"{\"id\":1,\"x\":true}\n{\"id\":2,\"y\":\"ok\",\"x\":2}\n","json","{}");
        assert_eq!(s.status(),0,"{}",s.error());assert_eq!(s.columns().len(),3);assert_eq!(s.columns()[1].kind,INT);assert!(s.columns()[2].nullable);
        assert_eq!(s.next(),0);assert_eq!(s.row()[2].kind,NULL);assert_eq!(s.next(),0);assert_eq!(s.row()[2].text,"ok");
    }
    #[test]
    fn malformed_nested_and_overflow(){
        assert_ne!(read(b"a,b\n1\n","csv","{}").status(),0);
        assert_ne!(read(b"a\n\"unterminated","csv","{}").status(),0);
        assert_ne!(read(b"{\"x\":[1]}\n","json","{}").status(),0);
        let s=read(b"a,b,c\n9223372036854775808,2025-02-29,\"\"\n","csv","{}");
        assert_eq!(s.status(),0);assert!(s.columns().iter().all(|c|c.kind==TEXT));assert!(!s.columns()[2].nullable);
    }
    #[test]
    fn parquet_multiple_row_groups(){
        use parquet::{schema::parser::parse_message_type,file::writer::SerializedFileWriter,data_type::Int64Type};
        let path=std::env::temp_dir().join(format!("seekdb-parquet-{}",std::process::id()));
        let schema=std::sync::Arc::new(parse_message_type("message t { REQUIRED INT64 id; }").unwrap());
        let f=File::create(&path).unwrap();let mut writer=SerializedFileWriter::new(f,schema,Default::default()).unwrap();
        for values in [&[9_i64,3][..],&[7_i64][..]] {
            let mut group=writer.next_row_group().unwrap();let mut col=group.next_column().unwrap().unwrap();
            col.typed::<Int64Type>().write_batch(values,None,None).unwrap();col.close().unwrap();group.close().unwrap();
        }
        writer.close().unwrap();let file=File::open(&path).unwrap();let mut s=open_scan(file.as_raw_fd(),"parquet","{}");std::fs::remove_file(path).unwrap();
        assert_eq!(s.status(),0,"{}",s.error());let mut values=vec![];while s.next()==0{values.push(s.row()[0].integer);}assert_eq!(values,vec![9,3,7]);
        assert_eq!(s.rewind(),0);assert_eq!(s.next(),0);assert_eq!(s.row()[0].integer,9);
    }
}
