use crate::{
    Cancel, Document, Format, Kind, Node, ScanError,
    tree::{quoted_end, walk},
};
use memchr::memmem::Finder;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SearchMode {
    Raw,
    Keys,
    Values,
}

#[derive(Clone, Debug)]
pub struct Hit {
    pub offset: usize,
}

impl Document {
    /// Deliver matches incrementally. Returning false stops without scanning the suffix.
    pub fn search(
        &self,
        query: &str,
        mode: SearchMode,
        cancel: &Cancel,
        mut emit: impl FnMut(Hit) -> bool,
    ) -> Result<(), ScanError> {
        if query.is_empty() {
            return Ok(());
        }
        let finder = Finder::new(query.as_bytes());
        let bytes = self.bytes();
        if mode == SearchMode::Raw {
            let mut pos = self.content_start();
            while pos < bytes.len() && !cancel.stopped() {
                let stop = bytes.len().min(pos.saturating_add(256 * 1024));
                let overlap = bytes.len().min(stop.saturating_add(query.len() - 1));
                let mut next = stop;
                for i in finder.find_iter(&bytes[pos..overlap]) {
                    let offset = pos + i;
                    if offset >= stop {
                        break;
                    }
                    if cancel.stopped() || !emit(Hit { offset }) {
                        return Ok(());
                    }
                    next = next.max(offset + query.len());
                }
                pos = next;
            }
            return Ok(());
        }
        let mut scan = |root: &Node| -> Result<bool, ScanError> {
            let end = walk(bytes, root, cancel, |node, _| {
                let (offset, raw) = match mode {
                    SearchMode::Keys => {
                        let Some(key) = &node.key else {
                            return Ok(true);
                        };
                        (key.start, &bytes[key.clone()])
                    }
                    _ if node.kind.container() => return Ok(true),
                    _ if node.kind == Kind::String => {
                        let end = quoted_end(bytes, node.start, node.bound, cancel)?;
                        (node.start, &bytes[node.start..end])
                    }
                    _ => {
                        let end = bytes[node.start..node.bound]
                            .iter()
                            .position(|b| {
                                b.is_ascii_whitespace() || matches!(b, b',' | b'}' | b']')
                            })
                            .map_or(node.bound, |i| node.start + i);
                        (node.start, &bytes[node.start..end])
                    }
                };
                let matches = if raw.first() == Some(&b'"') {
                    if memchr::memchr(b'\\', raw).is_some() {
                        let decoded =
                            sonic_rs::from_slice::<String>(raw).map_err(|e| ScanError {
                                offset,
                                message: e.to_string(),
                            })?;
                        finder.find(decoded.as_bytes()).is_some()
                    } else {
                        finder.find(&raw[1..raw.len() - 1]).is_some()
                    }
                } else {
                    finder.find(raw).is_some()
                };
                Ok(!matches || emit(Hit { offset }))
            })?;
            Ok(end.is_some())
        };
        match self.format {
            Format::Json => {
                scan(&self.root()?)?;
            }
            Format::Lines => self.records(cancel, |line| {
                if let Ok(node) = self.node(line.start..line.end) {
                    scan(&node).unwrap_or(true)
                } else {
                    true
                }
            }),
        }
        Ok(())
    }
}
