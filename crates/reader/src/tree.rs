use crate::Cancel;
use std::{fmt, ops::Range};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    Object,
    Array,
    String,
    Number,
    Bool,
    Null,
}

impl Kind {
    pub fn container(self) -> bool {
        matches!(self, Self::Object | Self::Array)
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Node {
    pub start: usize,
    /// Exclusive enclosing bound. The end of a container can still be unknown.
    pub bound: usize,
    pub kind: Kind,
    pub key: Option<Range<usize>>,
    pub ordinal: usize,
}

#[derive(Debug)]
pub struct ScanError {
    pub offset: usize,
    pub message: String,
}
impl fmt::Display for ScanError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "字节 {} · {}", self.offset, self.message)
    }
}
impl std::error::Error for ScanError {}

fn error(offset: usize, message: impl Into<String>) -> ScanError {
    ScanError {
        offset,
        message: message.into(),
    }
}
fn whitespace(b: u8) -> bool {
    matches!(b, b' ' | b'\t' | b'\n' | b'\r')
}
fn trim(bytes: &[u8], mut pos: usize, bound: usize) -> usize {
    while pos < bound && whitespace(bytes[pos]) {
        pos += 1;
    }
    pos
}
fn check(cancel: &Cancel, pos: usize) -> Result<(), ScanError> {
    if cancel.stopped() {
        Err(error(pos, "已取消"))
    } else {
        Ok(())
    }
}

impl Node {
    pub fn at(bytes: &[u8], range: Range<usize>) -> Result<Self, ScanError> {
        if range.end > bytes.len() || range.start > range.end {
            return Err(error(range.start, "范围超出文件"));
        }
        let start = trim(bytes, range.start, range.end);
        let kind = match bytes.get(start).filter(|_| start < range.end) {
            Some(b'{') => Kind::Object,
            Some(b'[') => Kind::Array,
            Some(b'"') => Kind::String,
            Some(b'-' | b'0'..=b'9') => Kind::Number,
            Some(b't' | b'f') => Kind::Bool,
            Some(b'n') => Kind::Null,
            _ => return Err(error(start, "需要 JSON 值")),
        };
        Ok(Self {
            start,
            bound: range.end,
            kind,
            key: None,
            ordinal: 0,
        })
    }
    pub fn label(&self, bytes: &[u8]) -> String {
        self.key.as_ref().map_or_else(
            || format!("[{}]", self.ordinal),
            |key| preview_string(&bytes[key.clone()], 96),
        )
    }
    pub fn preview(&self, bytes: &[u8], limit: usize) -> String {
        match self.kind {
            Kind::Object => "{…}".into(),
            Kind::Array => "[…]".into(),
            Kind::String => {
                let raw = &bytes[self.start..self.bound.min(self.start.saturating_add(limit + 8))];
                let end = quoted_end(raw, 0, raw.len(), &Cancel::default()).ok();
                preview_string(&raw[..end.unwrap_or(raw.len())], limit)
            }
            _ => {
                let end = bytes[self.start..self.bound]
                    .iter()
                    .take(limit)
                    .position(|b| whitespace(*b) || matches!(b, b',' | b'}' | b']'))
                    .unwrap_or(limit.min(self.bound - self.start));
                String::from_utf8_lossy(&bytes[self.start..self.start + end]).into_owned()
            }
        }
    }
}

fn preview_string(raw: &[u8], limit: usize) -> String {
    if raw.len() <= limit
        && let Ok(s) = sonic_rs::from_slice::<String>(raw)
    {
        return s;
    }
    // Truncate between complete characters and escapes, including surrogate
    // pairs. The preview must show the same text that value search matches.
    let bound = raw.len().min(limit);
    let mut end = 1.min(raw.len());
    while end < bound {
        let step = match raw[end] {
            b'\\' if raw.get(end + 1) == Some(&b'u') => {
                let Some(hex) = raw.get(end + 2..end + 6) else {
                    break;
                };
                let high = std::str::from_utf8(hex)
                    .ok()
                    .and_then(|s| u16::from_str_radix(s, 16).ok());
                if high.is_some_and(|n| (0xd800..=0xdbff).contains(&n)) {
                    12
                } else {
                    6
                }
            }
            b'\\' => 2,
            b'"' => break,
            0..=0x7f => 1,
            0xc2..=0xdf => 2,
            0xe0..=0xef => 3,
            0xf0..=0xf4 => 4,
            _ => break,
        };
        if end + step > bound {
            break;
        }
        end += step;
    }
    let mut token = raw[..end].to_vec();
    token.push(b'"');
    let mut result = sonic_rs::from_slice::<String>(&token)
        .unwrap_or_else(|_| String::from_utf8_lossy(&raw[1.min(end)..end]).into_owned());
    result.push('…');
    result
}

pub(crate) fn quoted_end(
    bytes: &[u8],
    start: usize,
    bound: usize,
    cancel: &Cancel,
) -> Result<usize, ScanError> {
    let mut pos = start + 1;
    let mut segment = pos;
    while pos < bound {
        check(cancel, pos)?;
        let block_end = bound.min(pos.saturating_add(128 * 1024));
        let Some(step) = memchr::memchr2(b'"', b'\\', &bytes[pos..block_end]) else {
            pos = block_end;
            continue;
        };
        pos += step;
        let plain = &bytes[segment..pos];
        if let Some(i) = plain.iter().position(|b| *b < 0x20) {
            return Err(error(segment + i, "字符串含控制字符"));
        }
        if let Err(e) = std::str::from_utf8(plain) {
            return Err(error(segment + e.valid_up_to(), "无效的 UTF-8"));
        }
        if bytes[pos] == b'"' {
            return Ok(pos + 1);
        }
        pos += 1;
        match bytes.get(pos).filter(|_| pos < bound) {
            Some(b'"' | b'\\' | b'/' | b'b' | b'f' | b'n' | b'r' | b't') => pos += 1,
            Some(b'u')
                if pos + 5 <= bound
                    && bytes[pos + 1..pos + 5].iter().all(u8::is_ascii_hexdigit) =>
            {
                pos += 5
            }
            _ => return Err(error(pos, "无效的字符串转义")),
        }
        segment = pos;
    }
    Err(error(bound, "字符串未结束"))
}

fn scalar_end(bytes: &[u8], node: &Node, cancel: &Cancel) -> Result<usize, ScanError> {
    let (start, bound) = (node.start, node.bound);
    if node.kind == Kind::String {
        return quoted_end(bytes, start, bound, cancel);
    }
    if matches!(node.kind, Kind::Bool | Kind::Null) {
        let word: &[u8] = match bytes[start] {
            b't' => b"true",
            b'f' => b"false",
            _ => b"null",
        };
        return bytes[start..bound]
            .starts_with(word)
            .then_some(start + word.len())
            .ok_or_else(|| error(start, "无效的字面值"));
    }
    let mut p = start;
    let data = &bytes[..bound];
    if data[p] == b'-' {
        p += 1;
    }
    let digits = |p: &mut usize| {
        let before = *p;
        while data.get(*p).is_some_and(u8::is_ascii_digit) {
            *p += 1;
        }
        *p > before
    };
    if data.get(p) == Some(&b'0') {
        p += 1;
    } else if !digits(&mut p) {
        return Err(error(p, "需要数字"));
    }
    if data.get(p) == Some(&b'.') {
        p += 1;
        if !digits(&mut p) {
            return Err(error(p, "需要小数位"));
        }
    }
    if matches!(data.get(p), Some(b'e' | b'E')) {
        p += 1;
        if matches!(data.get(p), Some(b'+' | b'-')) {
            p += 1;
        }
        if !digits(&mut p) {
            return Err(error(p, "需要指数"));
        }
    }
    Ok(p)
}

#[derive(Clone, Debug)]
struct Frame {
    kind: Kind,
    count: usize,
}

fn next_child(
    bytes: &[u8],
    pos: &mut usize,
    bound: usize,
    frame: &mut Frame,
    cancel: &Cancel,
) -> Result<Option<Node>, ScanError> {
    check(cancel, *pos)?;
    *pos = trim(bytes, *pos, bound);
    let close = if frame.kind == Kind::Array {
        b']'
    } else {
        b'}'
    };
    if *pos < bound && bytes[*pos] == close {
        *pos += 1;
        return Ok(None);
    }
    if frame.count > 0 {
        if *pos >= bound || bytes[*pos] != b',' {
            return Err(error(*pos, "需要逗号或容器结束符"));
        }
        *pos = trim(bytes, *pos + 1, bound);
    }
    let key = if frame.kind == Kind::Object {
        if *pos >= bound || bytes[*pos] != b'"' {
            return Err(error(*pos, "需要对象键名"));
        }
        let end = quoted_end(bytes, *pos, bound, cancel)?;
        let key = *pos..end;
        *pos = trim(bytes, end, bound);
        if *pos >= bound || bytes[*pos] != b':' {
            return Err(error(*pos, "键名后需要冒号"));
        }
        *pos += 1;
        Some(key)
    } else {
        None
    };
    let mut node = Node::at(bytes, *pos..bound)?;
    node.key = key;
    node.ordinal = frame.count;
    frame.count += 1;
    Ok(Some(node))
}

/// Iterative traversal. A caller can stop at a value's opening byte, before scanning its descendants.
pub(crate) fn walk(
    bytes: &[u8],
    root: &Node,
    cancel: &Cancel,
    mut visit: impl FnMut(&Node, usize) -> Result<bool, ScanError>,
) -> Result<Option<usize>, ScanError> {
    let mut stack = Vec::new();
    let mut current = Some(root.clone());
    let mut pos = root.start;
    loop {
        if let Some(node) = current.take() {
            check(cancel, node.start)?;
            if !visit(&node, stack.len())? {
                return Ok(None);
            }
            if node.kind.container() {
                pos = node.start + 1;
                stack.push(Frame {
                    kind: node.kind,
                    count: 0,
                });
            } else {
                pos = scalar_end(bytes, &node, cancel)?;
            }
        }
        let Some(frame) = stack.last_mut() else {
            return Ok(Some(pos));
        };
        current = next_child(bytes, &mut pos, root.bound, frame, cancel)?;
        if current.is_none() {
            stack.pop();
        }
    }
}

/// Resumable enumeration of immediate children. Retain it to load the next page.
#[derive(Clone, Debug)]
pub struct Children {
    frame: Frame,
    pos: usize,
    bound: usize,
    previous: Option<Node>,
    done: bool,
}

impl Children {
    pub fn new(parent: &Node) -> Self {
        Self {
            frame: Frame {
                kind: parent.kind,
                count: 0,
            },
            pos: parent.start + 1,
            bound: parent.bound,
            previous: None,
            done: !parent.kind.container(),
        }
    }
    pub fn next(&mut self, bytes: &[u8], cancel: &Cancel) -> Result<Option<Node>, ScanError> {
        if self.done {
            return Ok(None);
        }
        if let Some(previous) = self.previous.take() {
            self.pos =
                walk(bytes, &previous, cancel, |_, _| Ok(true))?.expect("uninterrupted traversal");
        }
        let item = next_child(bytes, &mut self.pos, self.bound, &mut self.frame, cancel)?;
        self.previous = item.clone();
        self.done = item.is_none();
        Ok(item)
    }
    pub fn end(&self) -> Option<usize> {
        self.done.then_some(self.pos)
    }
}

pub(crate) fn valid_end(bytes: &[u8], end: usize, bound: usize) -> Result<(), ScanError> {
    let pos = trim(bytes, end, bound);
    if pos != bound {
        Err(error(pos, "JSON 值之后还有其他内容"))
    } else {
        Ok(())
    }
}

/// Reuse the structural cursor for increasing raw-byte hits in the same document.
/// No per-file map of all paths is required.
pub struct Locator {
    parents: Vec<(Node, Frame)>,
    current: Option<Node>,
    pos: usize,
    root: Node,
    last: usize,
}

impl Locator {
    pub fn new(root: Node) -> Self {
        Self {
            parents: Vec::new(),
            current: Some(root.clone()),
            pos: root.start,
            last: 0,
            root,
        }
    }
    fn path(&self, leaf: Option<&Node>) -> Vec<Node> {
        self.parents
            .iter()
            .map(|(n, _)| n.clone())
            .chain(leaf.cloned())
            .collect()
    }
    pub fn locate(
        &mut self,
        bytes: &[u8],
        offset: usize,
        cancel: &Cancel,
    ) -> Result<Vec<Node>, ScanError> {
        if offset < self.last {
            *self = Self::new(self.root.clone());
        }
        self.last = offset;
        loop {
            check(cancel, self.pos)?;
            if let Some(n) = &self.current {
                let first = n.key.as_ref().map_or(n.start, |k| k.start);
                if offset < first {
                    return Ok(self.path(None));
                }
                if offset <= n.start {
                    return Ok(self.path(Some(n)));
                }
                if n.kind.container() {
                    let n = self.current.take().unwrap();
                    self.pos = n.start + 1;
                    self.parents.push((
                        n.clone(),
                        Frame {
                            kind: n.kind,
                            count: 0,
                        },
                    ));
                } else {
                    if self.pos <= n.start {
                        self.pos = scalar_end(bytes, n, cancel)?;
                    }
                    if offset < self.pos {
                        return Ok(self.path(Some(n)));
                    }
                    self.current = None;
                }
            }
            if self.parents.is_empty() {
                return Ok(vec![self.root.clone()]);
            }
            let mut pos = self.pos;
            let last = self.parents.last_mut().unwrap();
            let mut frame = last.1.clone();
            self.current = next_child(bytes, &mut pos, self.root.bound, &mut frame, cancel)?;
            if let Some(node) = &self.current {
                last.1 = frame;
                self.pos = node.start;
            } else if offset < pos {
                return Ok(self.path(None));
            } else {
                self.parents.pop();
                self.pos = pos;
            }
        }
    }
}

impl Node {
    pub fn validate(&self, bytes: &[u8], cancel: &Cancel) -> Result<(), ScanError> {
        let end = walk(bytes, self, cancel, |_, _| Ok(true))?.unwrap();
        valid_end(bytes, end, self.bound)
    }
}
