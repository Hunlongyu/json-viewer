use super::model::{Branch, Model, Row};
use async_channel::Sender;
use nsh_core::{
    file_dialog::{FileFilter, OpenDialog},
    prelude::*,
};
use preview_reader::{
    Cancel, Children as ChildCursor, Document, Format, Locator, Node, Record, SearchMode,
};
use std::{
    path::PathBuf,
    sync::{Arc, mpsc},
    thread,
    time::{Duration, Instant},
};

enum Update {
    Open(Arc<Document>, Option<Node>),
    Index(Vec<Record>, bool),
    Children(usize, Vec<Node>, Option<ChildCursor>, bool),
    Hit(usize, usize, Vec<Node>),
    End(String),
    Error(String),
    RecordError(Record, String),
}
type Message = (Cancel, Update);

#[derive(Clone, Copy)]
pub struct AppState {
    pub model: Stored<Model>,
    version: Signal<u64>,
    layout_version: Signal<u64>,
    tx: Stored<Sender<Message>>,
    list: Stored<ElementRef>,
}

impl AppState {
    pub fn new(list: ElementRef) -> Self {
        let (tx, rx) = async_channel::bounded(16);
        let state = Self {
            model: stored(Model::default()),
            version: signal(0),
            layout_version: signal(0),
            tx: stored(tx),
            list: stored(list),
        };
        let _task = spawn(async move {
            while let Ok((token, event)) = rx.recv().await {
                if !token.stopped() {
                    state.receive(event);
                }
            }
        });
        cleanup(move || state.model.with(|m| m.reset_file()));
        state
    }
    pub fn read<R>(&self, f: impl FnOnce(&Model) -> R) -> R {
        self.version.get();
        self.model.with(|m| f(m))
    }
    pub fn refresh(&self) {
        self.version.update(|v| *v = v.wrapping_add(1));
    }
    pub fn layout<R>(&self, f: impl FnOnce(&Model) -> R) -> R {
        self.layout_version.get();
        self.model.with(|m| f(m))
    }
    pub fn refresh_layout(&self) {
        self.layout_version.update(|v| *v = v.wrapping_add(1));
        self.refresh();
    }
    fn sender(&self) -> Sender<Message> {
        self.tx.with(|s| s.clone())
    }
    fn receive(&self, event: Update) {
        let changes_layout = matches!(
            &event,
            Update::Open(..) | Update::Index(..) | Update::Children(..) | Update::Hit(..)
        );
        let opened = matches!(&event, Update::Open(..));
        let mut jump = None;
        let mut need_index = false;
        self.model.with(|m| {
            match event {
                Update::Open(doc, root) => {
                    m.doc = Some(doc);
                    m.root = root;
                    m.status = "就绪".into();
                    m.rebuild();
                }
                Update::Index(mut records, finished) => {
                    let old = m.records.len();
                    m.records.append(&mut records);
                    m.indexed = finished;
                    if m.lines_open {
                        // Records always append after the last visible record and its children.
                        m.rows.add_records(m.records.len() - old);
                    }
                    if let Some((offset, path)) = m.pending_record.take() {
                        jump = m.reveal(offset, path);
                    }
                }
                Update::Children(id, items, cursor, finished) => {
                    if let Some(branch) = m.branches.get_mut(&id) {
                        for node in items {
                            branch.add(node);
                        }
                        if finished {
                            branch.busy = false;
                            branch.cursor = cursor;
                        }
                    }
                    m.rebuild();
                }
                Update::Hit(count, offset, path) => {
                    m.status = format!("第 {count} 处匹配　·　字节 {offset}");
                    m.waiting = false;
                    jump = m.reveal(offset, path);
                    need_index = m.pending_record.is_some();
                }
                Update::End(msg) => {
                    m.status = msg;
                    m.waiting = false;
                    m.next_match = None;
                }
                Update::Error(msg) => m.status = msg,
                Update::RecordError(record, msg) => {
                    m.status = format!("行 {}　{msg}", record.line);
                    m.record_errors.insert(record.start, msg);
                }
            }
        });
        if changes_layout {
            self.refresh_layout();
        } else {
            self.refresh();
        }
        if opened {
            self.list.with(|r| r.focus());
        }
        if need_index {
            self.index();
        }
        if let Some(index) = jump {
            self.scroll(index);
        }
    }
    pub fn scroll(&self, index: usize) {
        self.list
            .with(|r| r.scroll_to_index(index as u32, Percentage(0.4), px(0.), false));
    }
    pub fn pick(self) {
        let _task = spawn(async move {
            let dialog = OpenDialog::files()
                .title("打开 JSON 或 JSONL")
                .filter(FileFilter::new("JSON / JSONL", ["json", "jsonl", "ndjson"]));
            match pick_paths(dialog).await {
                Ok(Some(paths)) => {
                    if let Some(path) = paths.into_iter().next() {
                        self.open(path);
                    }
                }
                Err(e) => self.receive(Update::Error(e)),
                _ => (),
            }
        });
    }
    pub fn open(self, path: PathBuf) {
        let token = self.model.with(|m| {
            m.reset_file();
            m.status = format!("正在打开 {}", path.display());
            m.file_cancel.clone()
        });
        self.refresh_layout();
        let tx = self.sender();
        thread::spawn(move || {
            let load = (|| {
                let doc = Arc::new(
                    Document::open(&path, &token)
                        .map_err(|e| format!("{}　{e}", path.display()))?,
                );
                if doc.bytes().starts_with(&[0xff, 0xfe])
                    || doc.bytes().starts_with(&[0xfe, 0xff])
                    || doc.bytes().starts_with(&[0, 0, 0xfe, 0xff])
                {
                    return Err("此文件使用 UTF-16 或 UTF-32 编码。预览需要 UTF-8。".to_owned());
                }
                let root = if doc.format == Format::Json {
                    Some(doc.root().map_err(|e| e.to_string())?)
                } else {
                    None
                };
                Ok((doc, root))
            })();
            match load {
                Ok((doc, root)) => {
                    let scalar = root.as_ref().filter(|n| !n.kind.container()).cloned();
                    if tx
                        .send_blocking((token.clone(), Update::Open(doc.clone(), root)))
                        .is_ok()
                        && let Some(n) = scalar
                        && let Err(e) = n.validate(doc.bytes(), &token)
                    {
                        let _ = tx.send_blocking((
                            token,
                            Update::Error(format!("{e}　已检查字节 {}..{}", n.start, e.offset)),
                        ));
                    }
                }
                Err(e) => {
                    let _ = tx.send_blocking((token, Update::Error(e)));
                }
            }
        });
    }
    fn index(self) {
        let job = self.model.with(|m| {
            if m.index_started || m.doc.as_ref()?.format != Format::Lines {
                return None;
            }
            m.index_started = true;
            Some((m.doc.clone()?, m.file_cancel.clone()))
        });
        let Some((doc, token)) = job else { return };
        let tx = self.sender();
        thread::spawn(move || {
            let mut batch = Vec::new();
            let mut first = true;
            let mut progress = Instant::now();
            let result = doc.index_records(&token, |record| {
                batch.push(record);
                if (first && batch.len() >= 48)
                    || (batch.len().is_multiple_of(4096)
                        && progress.elapsed() >= Duration::from_millis(100))
                {
                    first = false;
                    progress = Instant::now();
                    return tx
                        .send_blocking((
                            token.clone(),
                            Update::Index(std::mem::take(&mut batch), false),
                        ))
                        .is_ok();
                }
                true
            });
            let _ = tx.send_blocking((token.clone(), Update::Index(batch, result.is_ok())));
            if let Err(e) = result {
                let _ = tx.send_blocking((
                    token,
                    Update::Error(format!("行索引未完成，请重新打开文件。{e}")),
                ));
            }
        });
    }
    fn page(self, id: usize) {
        let job = self.model.with(|m| {
            let doc = m.doc.clone()?;
            let branch = m.branches.get_mut(&id)?;
            if branch.busy {
                return None;
            }
            let cursor = branch.cursor.take()?;
            branch.busy = true;
            let whole = m.root.as_ref().is_some_and(|n| n.start == id);
            let record = if doc.format == Format::Lines {
                let i = m.records.partition_point(|r| r.end <= id);
                m.records
                    .get(i)
                    .filter(|r| doc.node(r.start..r.end).is_ok_and(|n| n.start == id))
                    .copied()
            } else {
                None
            };
            Some((doc, m.file_cancel.clone(), cursor, whole, record))
        });
        let Some((doc, token, mut cursor, whole, record)) = job else {
            return;
        };
        let tx = self.sender();
        thread::spawn(move || {
            let mut batch = Vec::new();
            let mut last = Instant::now();
            let mut error = None;
            let mut more = true;
            for i in 0..256 {
                match cursor.next(doc.bytes(), &token) {
                    Ok(Some(n)) => batch.push(n),
                    Ok(None) => {
                        more = false;
                        break;
                    }
                    Err(e) => {
                        more = false;
                        error = Some(format!("{e}　已检查字节 {id}..{}", e.offset));
                        break;
                    }
                }
                // Deliver the initial viewport before skipping each subtree; batch later rows.
                if i < 24 || last.elapsed() >= Duration::from_millis(12) {
                    if tx
                        .send_blocking((
                            token.clone(),
                            Update::Children(id, std::mem::take(&mut batch), None, false),
                        ))
                        .is_err()
                    {
                        return;
                    }
                    last = Instant::now();
                }
            }
            if let Some(end) = cursor.end()
                && (whole || record.is_some())
            {
                let bound = record.map_or(doc.bytes().len(), |r| r.end);
                if let Some(extra) = doc.bytes()[end..bound]
                    .iter()
                    .position(|b| !b.is_ascii_whitespace())
                {
                    error = Some(format!("字节 {} · JSON 值之后还有其他内容", end + extra));
                }
            }
            let _ = tx.send_blocking((
                token.clone(),
                Update::Children(id, batch, more.then_some(cursor), true),
            ));
            if let Some(e) = error {
                let event = match record {
                    Some(r) => Update::RecordError(r, e),
                    None => Update::Error(e),
                };
                let _ = tx.send_blocking((token, event));
            }
        });
    }
    pub fn activate(self, i: usize) {
        let mut need_index = false;
        let mut scalar_record = None;
        let id = self.model.with(|m| {
            let row = m.rows.get(i)?.1;
            m.selected = i;
            m.detail_limit = 256;
            match row {
                Row::Lines => {
                    m.lines_open = !m.lines_open;
                    need_index = m.lines_open;
                }
                Row::More(id) => return Some(id),
                _ => {
                    if let Row::Record(i) = &row
                        && let Some(doc) = m.doc.clone()
                        && m.node(&row).is_none_or(|n| !n.kind.container())
                    {
                        scalar_record = Some((doc, m.records[*i], m.file_cancel.clone()));
                    }
                    if let Some(n) = m.node(&row)
                        && n.kind.container()
                        && !m.open.remove(&n.start)
                    {
                        m.open.insert(n.start);
                        m.branches.entry(n.start).or_insert_with(|| Branch::new(&n));
                        m.rebuild();
                        if m.branches[&n.start].nodes.is_empty() {
                            return Some(n.start);
                        }
                    }
                }
            }
            m.rebuild();
            None
        });
        self.refresh_layout();
        if let Some((doc, r, token)) = scalar_record {
            let tx = self.sender();
            thread::spawn(move || {
                if let Err(e) = doc
                    .node(r.start..r.end)
                    .and_then(|n| n.validate(doc.bytes(), &token))
                {
                    let _ = tx.send_blocking((token, Update::RecordError(r, e.to_string())));
                }
            });
        }
        if need_index {
            self.index();
        }
        if let Some(id) = id {
            self.page(id);
        }
    }
    pub fn select(self, delta: isize) {
        let index = self.model.with(|m| {
            m.selected = m
                .selected
                .saturating_add_signed(delta)
                .min(m.rows.len().saturating_sub(1));
            m.detail_limit = 256;
            m.selected
        });
        self.refresh();
        self.scroll(index);
    }
    pub fn query(self, q: &str) {
        self.model.with(|m| {
            if m.query != q {
                m.reset_search();
                m.query = q.into();
                m.status.clear();
            }
        });
        self.refresh();
    }
    pub fn mode(self) {
        self.model.with(|m| {
            m.reset_search();
            m.mode = match m.mode {
                SearchMode::Raw => SearchMode::Keys,
                SearchMode::Keys => SearchMode::Values,
                SearchMode::Values => SearchMode::Raw,
            };
            m.status.clear();
        });
        self.refresh();
    }
    pub fn cancel(self) {
        self.model.with(|m| {
            m.reset_search();
            m.status = "搜索已取消".into()
        });
        self.refresh();
    }
    pub fn search(self) {
        let job = self.model.with(|m| {
            if m.waiting || m.query.is_empty() {
                return None;
            }
            let doc = m.doc.clone()?;
            m.waiting = true;
            m.status = "正在搜索…".into();
            if let Some(next) = &m.next_match {
                let _ = next.send(());
                return None;
            }
            let (send, recv) = mpsc::channel();
            m.next_match = Some(send);
            Some((doc, m.query.clone(), m.mode, m.search_cancel.clone(), recv))
        });
        self.refresh();
        let Some((doc, q, mode, token, next)) = job else {
            return;
        };
        let tx = self.sender();
        thread::spawn(move || {
            let mut count = 0;
            let mut locator: Option<(usize, Locator)> = None;
            let result = doc.search(&q, mode, &token, |hit| {
                count += 1;
                let bytes = doc.bytes();
                let root = if doc.format == Format::Lines {
                    let start = memchr::memrchr(b'\n', &bytes[..hit.offset])
                        .map_or(doc.content_start(), |p| p + 1);
                    let end = memchr::memchr(b'\n', &bytes[hit.offset..])
                        .map_or(bytes.len(), |p| hit.offset + p);
                    doc.node(start..end)
                } else {
                    doc.root()
                };
                let path = root
                    .ok()
                    .and_then(|r| {
                        if locator.as_ref().is_none_or(|(start, _)| *start != r.start) {
                            locator = Some((r.start, Locator::new(r)));
                        }
                        locator.as_mut()?.1.locate(bytes, hit.offset, &token).ok()
                    })
                    .unwrap_or_default();
                if tx
                    .send_blocking((token.clone(), Update::Hit(count, hit.offset, path)))
                    .is_err()
                {
                    return false;
                }
                // Backpressure keeps "next" on this traversal with constant hit storage.
                next.recv().is_ok() && !token.stopped()
            });
            let msg = match result {
                Err(e) => e.to_string(),
                Ok(()) if count == 0 => "未找到匹配".into(),
                Ok(()) => format!("已到末尾　·　共 {count} 处匹配"),
            };
            let _ = tx.send_blocking((token, Update::End(msg)));
        });
    }
}
