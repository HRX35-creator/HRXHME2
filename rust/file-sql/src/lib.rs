// Copyright (c) 2026 OceanBase.
// Licensed under the Apache License, Version 2.0.

//! Bounded Parquet decoding for the native SQL function-table operator.
//! The caller supplies an owned duplicate of its securely opened input FD.
//! Only a footer, a column page and a small decode batch are held in memory.

use bytes::Bytes;
use parquet::{
    basic::{ConvertedType, LogicalType, Repetition, TimeUnit, Type as Physical},
    errors::{ParquetError, Result as ParquetResult},
    file::{
        properties::ReaderProperties,
        reader::{ChunkReader, FileReader, Length},
        serialized_reader::{ReadOptionsBuilder, SerializedFileReader},
    },
    record::{reader::{ReaderIter, TreeBuilder}, Field},
    schema::types::{SchemaDescriptor, Type},
};
use std::{collections::BTreeMap, fs::File, io::{Cursor, Read, Seek, SeekFrom}, sync::{Arc, Mutex}};

const MIB: usize = 1024 * 1024;
const MAX_FOOTER: usize = 8 * MIB;
const MAX_PAGE: usize = 32 * MIB;
const MAX_GROUP: i64 = 64 * MIB as i64;
const MAX_ROWS_PER_GROUP: i64 = 1024 * 1024;
const MAX_HEADER: usize = 64 * 1024;

#[cxx::bridge(namespace = "oceanbase::file_sql")]
pub mod ffi {
    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    enum Kind { Null, Integer, Double, Boolean, Text, Date, DateTime }
    struct Column { name: String, kind: Kind, nullable: bool }
    struct Value { column: u32, kind: Kind, integer: i64, real: f64, text: String }
    extern "Rust" {
        type ParquetReader;
        // Consumes fd on every Unix success/failure path. It must be a dup.
        fn open_parquet(fd: i32) -> Box<ParquetReader>;
        fn status(self: &ParquetReader) -> i32;
        fn error(self: &ParquetReader) -> &str;
        fn columns(self: &ParquetReader) -> &[Column];
        fn project(self: &mut ParquetReader, columns: &[u32]) -> i32;
        fn rewind(self: &mut ParquetReader) -> i32;
        // 0 = row, 1 = EOF, -1 = error. Errors remain terminal until rewind.
        fn next(self: &mut ParquetReader) -> i32;
        fn values(self: &ParquetReader) -> &[Value];
    }
}

fn invalid(message: impl Into<String>) -> ParquetError { ParquetError::General(message.into()) }

// Inspect the compact-Thrift page header before the library sees its declared
// allocation sizes. Keep the captured header immutable even if the file changes
// concurrently. This is a bounded structural validator, not a second decoder.
struct Header<R> { input: R, bytes: Vec<u8> }
impl<R: Read> Header<R> {
    fn byte(&mut self) -> ParquetResult<u8> {
        if self.bytes.len() >= MAX_HEADER { return Err(invalid("Parquet page header exceeds 64 KiB")); }
        let mut b = [0]; self.input.read_exact(&mut b)?; self.bytes.push(b[0]); Ok(b[0])
    }
    fn varint(&mut self) -> ParquetResult<u64> {
        let mut value = 0u64;
        for shift in (0..70).step_by(7) {
            let b = self.byte()?;
            if shift == 63 && b > 1 { return Err(invalid("invalid Parquet header integer")); }
            value |= ((b & 127) as u64) << shift;
            if b & 128 == 0 { return Ok(value); }
        }
        Err(invalid("invalid Parquet header varint"))
    }
    fn integer(&mut self) -> ParquetResult<i64> {
        let n = self.varint()?; Ok(((n >> 1) as i64) ^ -((n & 1) as i64))
    }
    fn skip(&mut self, kind: u8, depth: usize) -> ParquetResult<()> {
        if depth > 8 { return Err(invalid("Parquet header nesting exceeds limit")); }
        match kind {
            1 | 2 => (),
            3 => { self.byte()?; },
            4..=6 => { self.integer()?; },
            7 => { for _ in 0..8 { self.byte()?; } },
            8 => {
                let n = self.varint()?;
                if n > MAX_HEADER as u64 { return Err(invalid("Parquet header string exceeds limit")); }
                for _ in 0..n { self.byte()?; }
            },
            9 | 10 => {
                let b = self.byte()?; let mut n = (b >> 4) as u64;
                if n == 15 { n = self.varint()?; }
                if n > MAX_HEADER as u64 { return Err(invalid("Parquet header list exceeds limit")); }
                for _ in 0..n {
                    if b & 15 == 1 || b & 15 == 2 { self.byte()?; }
                    else { self.skip(b & 15, depth + 1)?; }
                }
            },
            11 => {
                let n = self.varint()?;
                if n > MAX_HEADER as u64 { return Err(invalid("Parquet header map exceeds limit")); }
                if n > 0 {
                    let b = self.byte()?;
                    for _ in 0..n { self.skip(b >> 4, depth + 1)?; self.skip(b & 15, depth + 1)?; }
                }
            },
            12 => { self.structure(depth + 1)?; },
            _ => return Err(invalid("invalid Parquet header field type")),
        }
        Ok(())
    }
    fn structure(&mut self, depth: usize) -> ParquetResult<[Option<i64>; 3]> {
        if depth > 8 { return Err(invalid("Parquet header nesting exceeds limit")); }
        let mut id = 0i64; let mut sizes = [None; 3];
        loop {
            let b = self.byte()?;
            if b == 0 { break; }
            id = if b >> 4 == 0 { self.integer()? } else { id + (b >> 4) as i64 };
            if id <= 0 { return Err(invalid("invalid Parquet header field id")); }
            if depth == 0 && (1..=3).contains(&id) {
                if b & 15 != 5 || sizes[id as usize - 1].is_some() { return Err(invalid("invalid Parquet page sizes")); }
                sizes[id as usize - 1] = Some(self.integer()?);
            } else if depth == 1 && id == 1 && b & 15 == 5 {
                let count = self.integer()?;
                if !(0..=MAX_ROWS_PER_GROUP).contains(&count) { return Err(invalid("Parquet page value count exceeds limit")); }
            } else { self.skip(b & 15, depth)?; }
        }
        Ok(sizes)
    }
}

#[derive(Default)]
struct PageBudget {
    ranges: Vec<(u64, u64)>,
    pages: BTreeMap<u64, Vec<u8>>,
    decoded: usize,
    headers: usize,
}
#[derive(Clone)]
struct GuardedFile { file: Arc<File>, length: u64, footer_start: u64, footer: [u8; 8], budget: Arc<Mutex<PageBudget>> }
impl GuardedFile {
    fn new(mut file: File) -> ParquetResult<Self> {
        let length = file.metadata()?.len();
        if length < 12 { return Err(invalid("truncated Parquet file")); }
        let mut magic = [0; 4]; file.seek(SeekFrom::Start(0))?; file.read_exact(&mut magic)?;
        let mut footer = [0; 8]; file.seek(SeekFrom::End(-8))?; file.read_exact(&mut footer)?;
        let size = u32::from_le_bytes(footer[..4].try_into().map_err(|_| invalid("invalid footer"))?) as usize;
        if magic != *b"PAR1" || footer[4..] != *b"PAR1" || size > MAX_FOOTER || size as u64 > length - 12 {
            return Err(invalid("invalid or oversized Parquet footer"));
        }
        Ok(Self { file: Arc::new(file), length, footer_start: length - 8 - size as u64, footer,
            budget: Arc::new(Mutex::new(PageBudget::default())) })
    }
}
impl Length for GuardedFile { fn len(&self) -> u64 { self.length } }
impl ChunkReader for GuardedFile {
    type T = Cursor<Vec<u8>>;
    fn get_read(&self, start: u64) -> ParquetResult<Self::T> {
        if start == self.length - 8 { return Ok(Cursor::new(self.footer.to_vec())); }
        if start >= self.footer_start { return Err(invalid("Parquet page offset exceeds data section")); }
        let mut budget = self.budget.lock().map_err(|_| invalid("Parquet page budget is poisoned"))?;
        if let Some(header) = budget.pages.get(&start) { return Ok(Cursor::new(header.clone())); }
        let end = budget.ranges.iter().find_map(|&(a, b)| (a <= start && start < b).then_some(b))
            .ok_or_else(|| invalid("Parquet page is outside projected column chunks"))?;
        let mut file = self.file.try_clone()?; file.seek(SeekFrom::Start(start))?;
        let mut header = Header { input: file.take(MAX_HEADER as u64), bytes: Vec::new() };
        let fields = header.structure(0)?;
        let (_kind, uncompressed, compressed) = match fields {
            [Some(k), Some(u), Some(c)] => (k, u, c),
            _ => return Err(invalid("incomplete Parquet page header")),
        };
        if !(0..=MAX_PAGE as i64).contains(&uncompressed) || !(0..=MAX_PAGE as i64).contains(&compressed)
            || start.checked_add(header.bytes.len() as u64).and_then(|n| n.checked_add(compressed as u64)).is_none_or(|n| n > end) {
            return Err(invalid("Parquet page exceeds resource or file bounds"));
        }
        budget.decoded += uncompressed as usize;
        budget.headers += header.bytes.len();
        if budget.decoded > MAX_GROUP as usize || budget.headers > MAX_FOOTER || budget.pages.len() >= 65536 {
            return Err(invalid("Parquet group page allocations exceed decode budget"));
        }
        budget.pages.insert(start, header.bytes.clone());
        Ok(Cursor::new(header.bytes))
    }
    fn get_bytes(&self, start: u64, length: usize) -> ParquetResult<Bytes> {
        if length > MAX_PAGE || start.checked_add(length as u64).is_none_or(|n| n > self.length) {
            return Err(invalid("Parquet read exceeds resource or file bounds"));
        }
        let mut reader = self.file.try_clone()?; reader.seek(SeekFrom::Start(start))?;
        let mut bytes = vec![0; length]; reader.read_exact(&mut bytes)?; Ok(bytes.into())
    }
}

pub struct ParquetReader {
    source: Option<SerializedFileReader<GuardedFile>>,
    guarded: Option<GuardedFile>,
    columns: Vec<ffi::Column>,
    selected: Vec<u32>,
    projection: Option<Type>,
    iter: Option<ReaderIter>,
    group: usize,
    remaining: i64,
    row: Vec<ffi::Value>,
    message: String,
}

pub fn open_parquet(fd: i32) -> Box<ParquetReader> {
    let mut reader = Box::new(ParquetReader {
        source: None, guarded: None, columns: vec![], selected: vec![], projection: None,
        iter: None, group: 0, remaining: 0, row: vec![], message: String::new(),
    });
    #[cfg(unix)]
    let result = {
        use std::os::fd::FromRawFd;
        if fd < 0 { Err(invalid("invalid Parquet descriptor")) }
        else {
            // The bridge contract transfers exactly one owned duplicate.
            let file = unsafe { File::from_raw_fd(fd) };
            reader.initialize(file)
        }
    };
    #[cfg(not(unix))]
    let result: ParquetResult<()> = { let _ = fd; Err(invalid("Parquet FD bridge is not supported on this platform")) };
    if let Err(error) = result { reader.message = error.to_string(); }
    reader
}

impl ParquetReader {
    fn initialize(&mut self, file: File) -> ParquetResult<()> {
        let props = ReaderProperties::builder().set_read_bloom_filter(false).set_read_page_statistics(false).build();
        let guarded = GuardedFile::new(file)?;
        let footer_start = guarded.footer_start;
        self.guarded = Some(guarded.clone());
        let source = SerializedFileReader::new_with_options(guarded,
            ReadOptionsBuilder::new().with_reader_properties(props).build())?;
        let metadata = source.metadata();
        let schema = metadata.file_metadata().schema_descr();
        if schema.num_columns() == 0 || schema.num_columns() > 1024 || source.num_row_groups() > 32768 {
            return Err(invalid("Parquet schema or row-group count exceeds limit"));
        }
        for column in schema.columns() {
            if column.path().parts().len() != 1 || column.max_rep_level() != 0 || column.name().len() > 1024 {
                return Err(invalid("only flat Parquet columns are supported"));
            }
            let converted = column.converted_type();
            let kind = match (column.physical_type(), converted) {
                (Physical::BOOLEAN, ConvertedType::NONE) => ffi::Kind::Boolean,
                (Physical::INT32, ConvertedType::DATE) => ffi::Kind::Date,
                (Physical::INT64, ConvertedType::TIMESTAMP_MICROS | ConvertedType::TIMESTAMP_MILLIS) => ffi::Kind::DateTime,
                (Physical::INT64, _) if matches!(column.logical_type_ref(), Some(LogicalType::Timestamp(_))) => ffi::Kind::DateTime,
                (_, _) if matches!(column.logical_type_ref(), Some(LogicalType::Unknown)) => ffi::Kind::Null,
                (Physical::INT32, ConvertedType::NONE | ConvertedType::INT_8 | ConvertedType::INT_16 | ConvertedType::INT_32 | ConvertedType::UINT_8 | ConvertedType::UINT_16 | ConvertedType::UINT_32)
                | (Physical::INT64, ConvertedType::NONE | ConvertedType::INT_64) => ffi::Kind::Integer,
                (Physical::INT64, ConvertedType::UINT_64) => ffi::Kind::Text,
                (Physical::FLOAT | Physical::DOUBLE, ConvertedType::NONE) => ffi::Kind::Double,
                (Physical::BYTE_ARRAY, ConvertedType::NONE | ConvertedType::UTF8 | ConvertedType::ENUM) => ffi::Kind::Text,
                _ => return Err(invalid(format!("unsupported Parquet type in column {}", column.name()))),
            };
            self.columns.push(ffi::Column { name: column.name().into(), kind,
                nullable: column.self_type().get_basic_info().repetition() != Repetition::REQUIRED });
        }
        let mut total = 0i64;
        for group in metadata.row_groups() {
            if !(0..=MAX_ROWS_PER_GROUP).contains(&group.num_rows()) { return Err(invalid("Parquet row group exceeds row limit")); }
            total = total.checked_add(group.num_rows()).ok_or_else(|| invalid("Parquet row count overflow"))?;
            let mut bytes = 0i64;
            for column in group.columns() {
                // Validate before byte_range(), which asserts a nonnegative start.
                if column.data_page_offset() < 4 || column.dictionary_page_offset().is_some_and(|n| n < 4)
                    || column.compressed_size() < 0 || column.uncompressed_size() < 0 || column.num_values() != group.num_rows() {
                    return Err(invalid("invalid Parquet column metadata"));
                }
                let (start, length) = column.byte_range();
                if start.checked_add(length).is_none_or(|n| n > footer_start) { return Err(invalid("Parquet column exceeds file bounds")); }
                bytes = bytes.checked_add(column.uncompressed_size()).ok_or_else(|| invalid("Parquet group size overflow"))?;
            }
            if bytes > MAX_GROUP { return Err(invalid("Parquet row group exceeds 64 MiB decode budget")); }
        }
        if total != metadata.file_metadata().num_rows() { return Err(invalid("inconsistent Parquet row counts")); }
        self.source = Some(source);
        let selected: Vec<u32> = (0..self.columns.len() as u32).collect();
        self.configure(&selected)?;
        Ok(())
    }
    fn configure(&mut self, columns: &[u32]) -> ParquetResult<()> {
        if columns.iter().any(|&n| n as usize >= self.columns.len()) { return Err(invalid("invalid Parquet projection")); }
        self.selected = columns.to_vec(); self.selected.sort_unstable(); self.selected.dedup();
        let source = self.source.as_ref().ok_or_else(|| invalid("Parquet reader is not open"))?;
        let root = source.metadata().file_metadata().schema_descr().root_schema();
        let fields = self.selected.iter().map(|&n| root.get_fields()[n as usize].clone()).collect();
        self.projection = Some(Type::group_type_builder(root.name()).with_fields(fields).build()?);
        self.iter = None; self.group = 0; self.row.clear(); self.message.clear();
        self.remaining = source.metadata().file_metadata().num_rows();
        Ok(())
    }
    fn advance(&mut self) -> ParquetResult<bool> {
        self.row.clear();
        if self.remaining == 0 { return Ok(false); }
        if self.selected.is_empty() { self.remaining -= 1; return Ok(true); }
        loop {
            if let Some(iter) = self.iter.as_mut() {
                if let Some(row) = iter.next() {
                    let row = row?;
                    if row.len() != self.selected.len() { return Err(invalid("inconsistent Parquet projected row")); }
                    for (index, (_, field)) in row.get_column_iter().enumerate() {
                        self.row.push(self.convert(self.selected[index], field)?);
                    }
                    self.remaining -= 1; return Ok(true);
                }
            }
            let source = self.source.as_ref().ok_or_else(|| invalid("Parquet reader is not open"))?;
            if self.group >= source.num_row_groups() { return Err(invalid("truncated Parquet rows")); }
            let group = source.get_row_group(self.group)?;
            let guarded = self.guarded.as_ref().ok_or_else(|| invalid("missing Parquet page guard"))?;
            let mut budget = guarded.budget.lock().map_err(|_| invalid("Parquet page budget is poisoned"))?;
            *budget = PageBudget::default();
            for &index in &self.selected {
                let (start, length) = group.metadata().column(index as usize).byte_range();
                budget.ranges.push((start, start + length));
            }
            drop(budget);
            // Build one owned group decoder at a time, propagating its errors.
            let projection = self.projection.clone().ok_or_else(|| invalid("missing Parquet projection"))?;
            self.iter = Some(TreeBuilder::new().with_batch_size(64)
                .as_iter(Arc::new(SchemaDescriptor::new(Arc::new(projection))), group.as_ref())?);
            self.group += 1;
        }
    }
    fn convert(&self, column: u32, field: &Field) -> ParquetResult<ffi::Value> {
        let mut value = ffi::Value { column, kind: self.columns[column as usize].kind, integer: 0, real: 0.0, text: String::new() };
        match field {
            Field::Null => value.kind = ffi::Kind::Null,
            Field::Bool(n) => value.integer = i64::from(*n),
            Field::Byte(n) => value.integer = i64::from(*n),
            Field::Short(n) => value.integer = i64::from(*n),
            Field::Int(n) => value.integer = i64::from(*n),
            Field::Long(n) => {
                value.integer = *n;
                if value.kind == ffi::Kind::DateTime {
                    let source = self.source.as_ref().ok_or_else(|| invalid("Parquet reader is not open"))?;
                    let schema = source.metadata().file_metadata().schema_descr();
                    match schema.column(column as usize).logical_type_ref() {
                        Some(LogicalType::Timestamp(timestamp)) if timestamp.unit == TimeUnit::NANOS => value.integer = n.div_euclid(1000),
                        _ => return Err(invalid("unsupported Parquet timestamp encoding")),
                    }
                }
            },
            Field::UByte(n) => value.integer = i64::from(*n),
            Field::UShort(n) => value.integer = i64::from(*n),
            Field::UInt(n) => value.integer = i64::from(*n),
            Field::ULong(n) => value.text = n.to_string(),
            Field::Float(n) => value.real = f64::from(*n),
            Field::Double(n) => value.real = *n,
            Field::Date(n) => value.integer = i64::from(*n),
            Field::TimestampMillis(n) => value.integer = n.checked_mul(1000).ok_or_else(|| invalid("Parquet timestamp overflow"))?,
            Field::TimestampMicros(n) => value.integer = *n,
            Field::Str(s) => value.text = s.clone(),
            Field::Bytes(b) => value.text = std::str::from_utf8(b.data()).map_err(|_| invalid("Parquet string is not UTF-8"))?.into(),
            _ => return Err(invalid("unsupported Parquet value")),
        }
        if value.text.len() > 4 * MIB || !value.real.is_finite()
            || (value.kind == ffi::Kind::Date && !(-719162..=2932896).contains(&value.integer))
            || (value.kind == ffi::Kind::DateTime && !(-62135596800000000..=253402300799999999).contains(&value.integer)) {
            return Err(invalid("Parquet value exceeds SQL type or resource bounds"));
        }
        Ok(value)
    }
    pub fn status(&self) -> i32 { if self.message.is_empty() { 0 } else { -1 } }
    pub fn error(&self) -> &str { &self.message }
    pub fn columns(&self) -> &[ffi::Column] { &self.columns }
    pub fn values(&self) -> &[ffi::Value] { &self.row }
    pub fn project(&mut self, columns: &[u32]) -> i32 {
        match self.configure(columns) { Ok(()) => 0, Err(e) => { self.message = e.to_string(); -1 } }
    }
    pub fn rewind(&mut self) -> i32 { let selected = self.selected.clone(); self.project(&selected) }
    pub fn next(&mut self) -> i32 {
        if self.status() != 0 { return -1; }
        match self.advance() { Ok(true) => 0, Ok(false) => 1, Err(e) => { self.message = e.to_string(); -1 } }
    }
}

#[cfg(all(test, unix))]
mod tests {
    use super::*;
    use parquet::{
        basic::Compression, data_type::{BoolType, ByteArray, ByteArrayType, Int64Type},
        file::{properties::WriterProperties, writer::SerializedFileWriter},
        schema::parser::parse_message_type,
    };
    use std::{os::fd::IntoRawFd, sync::atomic::{AtomicU64, Ordering}};

    fn fixture() -> (std::path::PathBuf, Vec<u8>) {
        static SERIAL: AtomicU64 = AtomicU64::new(0);
        let path = std::env::temp_dir().join(format!("seekdb-parquet-{}-{}.parquet", std::process::id(), SERIAL.fetch_add(1, Ordering::Relaxed)));
        let schema = Arc::new(parse_message_type("message s { REQUIRED INT64 id; OPTIONAL BINARY name (UTF8); OPTIONAL BOOLEAN active; }").unwrap());
        let props = Arc::new(WriterProperties::builder().set_compression(Compression::SNAPPY).build());
        let mut writer = SerializedFileWriter::new(File::create(&path).unwrap(), schema, props).unwrap();
        for start in [0i64, 2i64, 4i64] {
            let mut group = writer.next_row_group().unwrap();
            let mut id = group.next_column().unwrap().unwrap();
            id.typed::<Int64Type>().write_batch(&[start, start + 1], None, None).unwrap(); id.close().unwrap();
            let mut name = group.next_column().unwrap().unwrap();
            name.typed::<ByteArrayType>().write_batch(&[ByteArray::from("世界")], Some(&[1, 0]), None).unwrap(); name.close().unwrap();
            let mut active = group.next_column().unwrap().unwrap();
            active.typed::<BoolType>().write_batch(&[true, false], Some(&[1, 1]), None).unwrap(); active.close().unwrap();
            group.close().unwrap();
        }
        writer.close().unwrap();
        let bytes = std::fs::read(&path).unwrap();
        (path, bytes)
    }
    #[test]
    fn snappy_multiple_groups_nulls_projection_and_rescan() {
        let (path, _) = fixture();
        let mut reader = open_parquet(File::open(&path).unwrap().into_raw_fd());
        std::fs::remove_file(path).unwrap();
        assert_eq!(0, reader.status(), "{}", reader.error());
        assert_eq!(3, reader.columns().len());
        assert!(!reader.columns()[0].nullable);
        assert!(reader.columns()[1].nullable);
        assert_eq!(0, reader.project(&[1, 0]));
        for id in 0..6 {
            assert_eq!(0, reader.next(), "{}", reader.error());
            assert_eq!(id, reader.values()[0].integer);
            assert_eq!(if id % 2 == 0 { ffi::Kind::Text } else { ffi::Kind::Null }, reader.values()[1].kind);
            if id % 2 == 0 { assert_eq!("世界", reader.values()[1].text); }
        }
        assert_eq!(1, reader.next());
        assert_eq!(0, reader.rewind());
        assert_eq!(0, reader.next());
        assert_eq!(0, reader.values()[0].integer);
        assert_eq!(0, reader.project(&[]));
        assert_eq!(6, (0..6).filter(|_| reader.next() == 0).count());
        assert_eq!(1, reader.next());
    }
    #[test]
    fn oversized_page_rejected_before_decompression_but_omitted_column_not_read() {
        let (path, bytes) = fixture();
        // Corrupt the first id page's size, keeping the footer and other columns valid.
        let guarded = GuardedFile::new(File::open(&path).unwrap()).unwrap();
        let source = SerializedFileReader::new(guarded).unwrap();
        let offset = source.metadata().row_group(0).column(0).byte_range().0 as usize;
        let mut altered = bytes;
        // Compact-Thrift PageHeader: type DATA_PAGE then a >32 MiB uncompressed size.
        let replacement = [0x15, 0x00, 0x15, 0x80, 0x80, 0x80, 0x40];
        altered[offset..offset + replacement.len()].copy_from_slice(&replacement);
        std::fs::write(&path, altered).unwrap();
        let mut reader = open_parquet(File::open(&path).unwrap().into_raw_fd());
        std::fs::remove_file(path).unwrap();
        assert_eq!(0, reader.status(), "{}", reader.error());
        assert_eq!(0, reader.project(&[1]));
        assert_eq!(0, reader.next(), "{}", reader.error());
        assert_eq!("世界", reader.values()[0].text);
        assert_eq!(0, reader.project(&[0]));
        assert_eq!(-1, reader.next());
        assert!(!reader.error().is_empty());
        assert_eq!(-1, reader.next());
    }
    #[test]
    fn malformed_and_oversized_footer_are_errors() {
        let (path, bytes) = fixture();
        for length in [0usize, 4, 8, 11] {
            std::fs::write(&path, &bytes[..length]).unwrap();
            let reader = open_parquet(File::open(&path).unwrap().into_raw_fd());
            assert_eq!(-1, reader.status());
        }
        let mut bad = bytes;
        let offset = bad.len() - 8;
        bad[offset..offset + 4].copy_from_slice(&((MAX_FOOTER + 1) as u32).to_le_bytes());
        std::fs::write(&path, bad).unwrap();
        assert_eq!(-1, open_parquet(File::open(&path).unwrap().into_raw_fd()).status());
        std::fs::remove_file(path).unwrap();
    }
}
