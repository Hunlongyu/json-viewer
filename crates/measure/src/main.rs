//! Run one workload in a fresh process so peak memory belongs to one engine.
use memchr::memchr_iter;
use serde::de::{Deserializer, MapAccess, Visitor};
use serde_json::value::RawValue;
use std::borrow::Cow;
use std::error::Error;
use std::fmt;
use std::hint::black_box;
use std::io::{BufRead, BufReader};
use std::path::Path;
use std::time::Instant;

type Outcome<T> = Result<T, Box<dyn Error>>;

#[derive(Default)]
struct Stats {
    rows: usize,
    first_us: Option<u128>,
    runtime_us: Option<u128>,
    first_container_us: Option<u128>,
    validation_us: Option<u128>,
}

impl Stats {
    fn item(&mut self, key: &str, start: Instant, container: bool) {
        self.rows += 1;
        if self.rows == 1 {
            self.first_us = Some(start.elapsed().as_micros());
        }
        if key == "Runtime" {
            self.runtime_us = Some(start.elapsed().as_micros());
        }
        if container && self.first_container_us.is_none() {
            self.first_container_us = Some(start.elapsed().as_micros());
        }
    }
}

struct RootFields<'a>(&'a mut Stats, Instant);

impl<'de> Visitor<'de> for RootFields<'_> {
    type Value = ();
    fn expecting(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("root object")
    }
    fn visit_map<A: MapAccess<'de>>(self, mut map: A) -> Result<(), A::Error> {
        while let Some(key) = map.next_key::<Cow<'de, str>>()? {
            let value = map.next_value::<&'de RawValue>()?.get();
            black_box(value.len());
            self.0.item(&key, self.1, value.starts_with(['{', '[']));
        }
        Ok(())
    }
}

#[cfg(windows)]
fn peak_working_set() -> usize {
    use windows_sys::Win32::System::{
        ProcessStatus::{GetProcessMemoryInfo, PROCESS_MEMORY_COUNTERS},
        Threading::GetCurrentProcess,
    };
    let mut counters = PROCESS_MEMORY_COUNTERS::default();
    let size = std::mem::size_of_val(&counters) as u32;
    // SAFETY: counters is initialized and writable for exactly size bytes.
    unsafe { GetProcessMemoryInfo(GetCurrentProcess(), &mut counters, size) };
    counters.PeakWorkingSetSize
}

fn main() -> Outcome<()> {
    let mut args = std::env::args_os().skip(1);
    let mode = args.next().ok_or("expected a workload")?;
    let workload = mode.to_str().ok_or("invalid workload")?;
    let filename = args.next().ok_or("expected a sample path")?;
    let path = Path::new(&filename);
    let start = Instant::now();
    if let Some(scope) = workload.strip_prefix("search-") {
        let mode = match scope {
            "raw" => preview_reader::SearchMode::Raw,
            "keys" => preview_reader::SearchMode::Keys,
            "values" => preview_reader::SearchMode::Values,
            _ => return Err("expected raw, keys or values".into()),
        };
        let query = args
            .next()
            .ok_or("expected query")?
            .into_string()
            .map_err(|_| "invalid query")?;
        let cancel = preview_reader::Cancel::default();
        let doc = preview_reader::Document::open(path, &cancel)?;
        let mut first = None;
        let mut next = None;
        let mut hits = 0;
        doc.search(&query, mode, &cancel, |hit| {
            hits += 1;
            match hits {
                1 => first = Some((hit.offset, start.elapsed().as_micros())),
                2 => next = Some((hit.offset, start.elapsed().as_micros())),
                _ => (),
            }
            true
        })?;
        println!(
            "{}",
            serde_json::json!({"workload":workload,"file":path.file_name().unwrap_or_default().to_string_lossy(),
            "hits":hits,"first":first,"second":next,"total_us":start.elapsed().as_micros()})
        );
        return Ok(());
    }
    let mut options = std::fs::OpenOptions::new();
    options.read(true);
    #[cfg(windows)]
    std::os::windows::fs::OpenOptionsExt::share_mode(&mut options, 1);
    let file = options.open(path)?;
    let mut stats = Stats::default();
    if workload == "buffer-lines" {
        let mut reader = BufReader::with_capacity(256 * 1024, file);
        loop {
            let chunk = reader.fill_buf()?;
            if chunk.is_empty() {
                break;
            }
            stats.rows += memchr_iter(b'\n', chunk).count();
            let size = chunk.len();
            reader.consume(size);
        }
    } else {
        // The benchmark input is a fixed fixture, opened without write/delete sharing on Windows.
        let bytes = unsafe { memmap2::Mmap::map(&file)? };
        let json = bytes.strip_prefix(b"\xef\xbb\xbf").unwrap_or(&bytes);
        // Matched-preflight cases pay for the same complete syntax + UTF-8
        // check before either parser can emit a field, including trailing data.
        if workload.starts_with("verified-root-") {
            let root = preview_reader::Node::at(json, 0..json.len())?;
            root.validate(json, &preview_reader::Cancel::default())?;
            stats.validation_us = Some(start.elapsed().as_micros());
        }
        match workload.strip_prefix("verified-").unwrap_or(workload) {
            "root-incremental" => {
                let root =
                    preview_reader::Node::at(&bytes, (bytes.len() - json.len())..bytes.len())?;
                let mut children = preview_reader::Children::new(&root);
                let cancel = preview_reader::Cancel::default();
                while let Some(child) = children.next(&bytes, &cancel)? {
                    stats.item(&child.label(&bytes), start, child.kind.container());
                }
            }
            "map-lines" => stats.rows = memchr_iter(b'\n', json).count(),
            "root-sonic" => {
                for item in sonic_rs::to_object_iter(json) {
                    let (key, value) = item?;
                    black_box(value.as_raw_str().len());
                    stats.item(&key, start, value.as_raw_str().starts_with(['{', '[']));
                }
            }
            "root-serde" => {
                let mut parser = serde_json::Deserializer::from_slice(json);
                parser.deserialize_map(RootFields(&mut stats, start))?;
                parser.end()?;
            }
            "lines-sonic" | "lines-serde" | "lines-simd" => {
                let mut scratch = Vec::new();
                let mut buffers = simd_json::Buffers::default();
                for line in json.split(|b| *b == b'\n') {
                    if line.iter().all(u8::is_ascii_whitespace) {
                        continue;
                    }
                    match workload {
                        "lines-sonic" => {
                            black_box(sonic_rs::from_slice::<sonic_rs::Value>(line)?);
                        }
                        "lines-serde" => {
                            black_box(serde_json::from_slice::<serde_json::Value>(line)?);
                        }
                        _ => {
                            scratch.clear();
                            scratch.extend_from_slice(line);
                            black_box(simd_json::to_borrowed_value_with_buffers(
                                &mut scratch,
                                &mut buffers,
                            )?);
                        }
                    }
                    stats.item("", start, false);
                }
            }
            _ => return Err(format!("unknown workload: {workload}").into()),
        }
    }
    let total_us = start.elapsed().as_micros();
    #[cfg(windows)]
    let peak = peak_working_set();
    #[cfg(not(windows))]
    let peak = 0;
    println!(
        "{}",
        serde_json::json!({
            "workload": workload, "file": path.file_name().unwrap_or_default().to_string_lossy(),
            "items": stats.rows, "first_us": stats.first_us, "runtime_us": stats.runtime_us,
            "validation_us":stats.validation_us,"first_container_us":stats.first_container_us,
            "total_us": total_us, "peak_working_set_bytes": peak
        })
    );
    Ok(())
}
