use crate::{Cancel, Node, ScanError};
use memmap2::Mmap;
use std::{
    fs::{File, OpenOptions},
    io::{self, BufReader, Read, Write},
    ops::Range,
    path::{Path, PathBuf},
    time::SystemTime,
};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Format {
    Json,
    Lines,
}

#[derive(Clone, Copy, Debug)]
pub struct Record {
    pub line: u64,
    pub start: usize,
    pub end: usize,
}

pub struct Document {
    pub path: PathBuf,
    pub format: Format,
    mapping: Option<Mmap>,
    handle: File,
    original: Option<(u64, SystemTime)>,
}

impl Document {
    pub fn open(path: &Path, cancel: &Cancel) -> io::Result<Self> {
        let format = match path
            .extension()
            .and_then(|s| s.to_str())
            .unwrap_or("")
            .to_ascii_lowercase()
            .as_str()
        {
            "jsonl" | "ndjson" => Format::Lines,
            _ => Format::Json,
        };
        let mut options = OpenOptions::new();
        options.read(true);
        #[cfg(windows)]
        {
            use std::os::windows::fs::OpenOptionsExt;
            // Keep this handle alive: deny writers and deletion while the map exists.
            match options.share_mode(1).open(path) {
                Ok(file) => return Self::map(path, format, file, None),
                Err(e) if e.raw_os_error() == Some(32) => (),
                Err(e) => return Err(e),
            }
        }
        // An active writer cannot be safely mapped. Capture a private file in bounded chunks.
        let file = File::open(path)?;
        let meta = file.metadata()?;
        let stamp = (meta.len(), meta.modified()?);
        let mut input = BufReader::new(file);
        let mut copy = tempfile::tempfile()?;
        let mut block = vec![0u8; 256 * 1024];
        let mut copied = 0;
        while copied < stamp.0 {
            if cancel.stopped() {
                return Err(io::ErrorKind::Interrupted.into());
            }
            let n = input.read(&mut block)?;
            if n == 0 {
                break;
            }
            copy.write_all(&block[..n])?;
            copied += n as u64;
        }
        let updated = input.get_ref().metadata()?;
        if copied != stamp.0 || (updated.len(), updated.modified()?) != stamp {
            return Err(io::Error::other("文件正在变化，请重新打开"));
        }
        Self::map(path, format, copy, Some(stamp))
    }

    fn map(
        path: &Path,
        format: Format,
        file: File,
        original: Option<(u64, SystemTime)>,
    ) -> io::Result<Self> {
        let mapping = if file.metadata()?.len() == 0 {
            None
        } else {
            // SAFETY: file is either held without write/delete sharing on Windows or is our private snapshot.
            Some(unsafe { Mmap::map(&file)? })
        };
        Ok(Self {
            path: path.to_owned(),
            format,
            mapping,
            handle: file,
            original,
        })
    }
    pub fn changed(&self) -> bool {
        self.original.is_some_and(|stamp| {
            std::fs::metadata(&self.path)
                .and_then(|m| Ok((m.len(), m.modified()?)))
                .ok()
                != Some(stamp)
        })
    }
    pub fn bytes(&self) -> &[u8] {
        self.mapping.as_deref().unwrap_or_default()
    }
    pub fn content_start(&self) -> usize {
        if self.bytes().starts_with(b"\xef\xbb\xbf") {
            3
        } else {
            0
        }
    }
    pub fn root(&self) -> Result<Node, ScanError> {
        self.node(self.content_start()..self.bytes().len())
    }
    pub fn node(&self, range: Range<usize>) -> Result<Node, ScanError> {
        Node::at(self.bytes(), range)
    }

    /// Produces the first records before reading later pages, including a final line without LF.
    pub fn records(&self, cancel: &Cancel, mut visit: impl FnMut(Record) -> bool) {
        let bytes = self.bytes();
        let mut start = self.content_start();
        let mut line = 1;
        for (i, block) in bytes[start..].chunks(256 * 1024).enumerate() {
            if cancel.stopped() {
                return;
            }
            let base = self.content_start() + i * 256 * 1024;
            for lf in memchr::memchr_iter(b'\n', block) {
                let end = base + lf;
                if bytes[start..end].iter().any(|b| !b.is_ascii_whitespace())
                    && !visit(Record { line, start, end })
                {
                    return;
                }
                start = end + 1;
                line += 1;
            }
        }
        if bytes[start..].iter().any(|b| !b.is_ascii_whitespace()) {
            visit(Record {
                line,
                start,
                end: bytes.len(),
            });
        }
    }

    /// Build the line index with sequential I/O. Reading each mapped page on
    /// Windows creates page faults and brings the whole file into our working set.
    pub fn index_records(
        &self,
        cancel: &Cancel,
        mut visit: impl FnMut(Record) -> bool,
    ) -> io::Result<()> {
        let mut block = vec![0u8; 256 * 1024];
        let mut pos = self.content_start();
        let mut start = pos;
        let mut line = 1;
        let mut has_content = false;
        while pos < self.bytes().len() && !cancel.stopped() {
            #[cfg(windows)]
            let n = std::os::windows::fs::FileExt::seek_read(&self.handle, &mut block, pos as u64)?;
            #[cfg(unix)]
            let n = std::os::unix::fs::FileExt::read_at(&self.handle, &mut block, pos as u64)?;
            if n == 0 {
                return Err(io::ErrorKind::UnexpectedEof.into());
            }
            let mut previous = 0;
            for lf in memchr::memchr_iter(b'\n', &block[..n]) {
                if (has_content || block[previous..lf].iter().any(|b| !b.is_ascii_whitespace()))
                    && !visit(Record {
                        line,
                        start,
                        end: pos + lf,
                    })
                {
                    return Ok(());
                }
                previous = lf + 1;
                start = pos + previous;
                line += 1;
                has_content = false;
            }
            has_content |= block[previous..n].iter().any(|b| !b.is_ascii_whitespace());
            pos += n;
        }
        if !cancel.stopped() && has_content {
            visit(Record {
                line,
                start,
                end: pos,
            });
        }
        Ok(())
    }
}
