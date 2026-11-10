use preview_reader::{Cancel, Children, Document, Format, Node, Record, SearchMode};
use std::{
    collections::{HashMap, HashSet},
    sync::Arc,
};

#[derive(Clone, PartialEq, Eq)]
pub enum Row {
    Lines,
    Record(usize),
    Value(Node),
    More(usize),
}

/// Collapsed records form an implicit consecutive range. Only the header and
/// rows inserted by an expansion need a separate entry in the tree model.
#[derive(Default)]
pub struct VisibleRows {
    records: usize,
    explicit: Vec<(usize, usize, Row)>,
}
impl VisibleRows {
    pub fn len(&self) -> usize {
        self.records + self.explicit.len()
    }
    pub fn get(&self, index: usize) -> Option<(usize, Row)> {
        if index >= self.len() {
            return None;
        }
        match self.explicit.binary_search_by_key(&index, |entry| entry.0) {
            Ok(i) => Some((self.explicit[i].1, self.explicit[i].2.clone())),
            Err(before) => Some((1, Row::Record(index - before))),
        }
    }
    pub fn add_records(&mut self, count: usize) {
        self.records += count;
    }
    /// NSH uses these identities to retain or replace recycled row views.
    /// Walk the explicit entries once and generate contiguous record IDs in between.
    pub fn keys(&self) -> Vec<(u8, usize)> {
        let mut next = 0;
        (0..self.len())
            .map(|i| {
                if let Some((at, _, row)) = self.explicit.get(next)
                    && *at == i
                {
                    next += 1;
                    match row {
                        Row::Lines => (0, 0),
                        Row::Value(node) => (2, node.start),
                        Row::More(parent) => (3, *parent),
                        Row::Record(_) => unreachable!("records have implicit positions"),
                    }
                } else {
                    (1, i - next)
                }
            })
            .collect()
    }
    fn push(&mut self, depth: usize, row: Row) {
        self.explicit.push((self.len(), depth, row));
    }
    fn position(&self, row: &Row) -> Option<usize> {
        if let Row::Record(ordinal) = row {
            if *ordinal >= self.records {
                return None;
            }
            // at - i is the number of records preceding explicit row i.
            let mut lo = 0;
            let mut hi = self.explicit.len();
            while lo < hi {
                let mid = lo + (hi - lo) / 2;
                if self.explicit[mid].0 - mid <= *ordinal {
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }
            Some(ordinal + lo)
        } else {
            self.explicit.iter().find(|(_, _, r)| r == row).map(|e| e.0)
        }
    }
}

pub struct Branch {
    pub nodes: Vec<Node>,
    pub cursor: Option<Children>,
    pub busy: bool,
}

impl Branch {
    pub fn new(parent: &Node) -> Self {
        Self {
            nodes: Vec::new(),
            cursor: Some(Children::new(parent)),
            busy: false,
        }
    }
    pub fn add(&mut self, node: Node) {
        match self
            .nodes
            .binary_search_by_key(&node.ordinal, |n| n.ordinal)
        {
            Ok(_) => (),
            Err(at) => self.nodes.insert(at, node),
        }
    }
}

pub struct Model {
    pub doc: Option<Arc<Document>>,
    pub root: Option<Node>,
    pub file_cancel: Cancel,
    pub search_cancel: Cancel,
    pub branches: HashMap<usize, Branch>,
    pub open: HashSet<usize>,
    pub records: Vec<Record>,
    pub record_errors: HashMap<usize, String>,
    pub lines_open: bool,
    pub index_started: bool,
    pub indexed: bool,
    pub rows: VisibleRows,
    pub selected: usize,
    pub status: String,
    pub query: String,
    pub mode: SearchMode,
    pub next_match: Option<std::sync::mpsc::Sender<()>>,
    pub waiting: bool,
    pub pending_record: Option<(usize, Vec<Node>)>,
    pub detail_limit: usize,
}

impl Default for Model {
    fn default() -> Self {
        Self {
            doc: None,
            root: None,
            file_cancel: Cancel::default(),
            search_cancel: Cancel::default(),
            branches: HashMap::new(),
            open: HashSet::new(),
            records: Vec::new(),
            record_errors: HashMap::new(),
            lines_open: false,
            index_started: false,
            indexed: false,
            rows: VisibleRows::default(),
            selected: 0,
            status: String::new(),
            query: String::new(),
            mode: SearchMode::Raw,
            next_match: None,
            waiting: false,
            pending_record: None,
            detail_limit: 256,
        }
    }
}
impl Model {
    pub fn reset_search(&mut self) {
        self.search_cancel.stop();
        self.search_cancel = Cancel::default();
        self.next_match = None;
        self.pending_record = None;
        self.waiting = false;
    }
    pub fn reset_file(&mut self) {
        self.file_cancel.stop();
        self.reset_search();
        *self = Self::default();
    }
    pub fn node(&self, row: &Row) -> Option<Node> {
        match row {
            Row::Value(n) => Some(n.clone()),
            Row::Record(i) => {
                let r = self.records.get(*i)?;
                self.doc.as_ref()?.node(r.start..r.end).ok()
            }
            _ => None,
        }
    }
    pub fn rebuild(&mut self) {
        let anchor = self.rows.get(self.selected).map(|(_, row)| row);
        self.rows = VisibleRows::default();
        let Some(doc) = self.doc.as_ref() else { return };
        if doc.format == Format::Lines {
            self.rows.push(0, Row::Lines);
            if self.lines_open {
                let mut expanded: Vec<_> = self
                    .open
                    .iter()
                    .filter_map(|&offset| {
                        let i = self.records.partition_point(|r| r.end <= offset);
                        let r = self.records.get(i)?;
                        doc.node(r.start..r.end)
                            .is_ok_and(|n| n.start == offset)
                            .then_some((i, offset))
                    })
                    .collect();
                expanded.sort_unstable();
                for (i, offset) in expanded {
                    self.rows.records = i + 1;
                    self.place_branch(1, offset);
                }
                self.rows.records = self.records.len();
            }
        } else if let Some(root) = &self.root {
            self.rows.push(0, Row::Value(root.clone()));
            self.place_branch(0, root.start);
        }
        self.selected = anchor
            .and_then(|row| self.rows.position(&row))
            .unwrap_or_else(|| self.selected.min(self.rows.len().saturating_sub(1)));
    }
    fn place_branch(&mut self, depth: usize, id: usize) {
        let mut pending = Vec::new();
        let mut parent = Some((depth, id));
        loop {
            if let Some((depth, id)) = parent.take()
                && self.open.contains(&id)
                && let Some(branch) = self.branches.get(&id)
            {
                if branch.cursor.is_some() && !branch.busy {
                    pending.push((depth + 1, Row::More(id)));
                }
                pending.extend(
                    branch
                        .nodes
                        .iter()
                        .rev()
                        .map(|n| (depth + 1, Row::Value(n.clone()))),
                );
            }
            let Some((depth, row)) = pending.pop() else {
                break;
            };
            if let Row::Value(n) = &row {
                parent = Some((depth, n.start));
            }
            self.rows.push(depth, row);
        }
    }
    pub fn display(&self, index: usize) -> String {
        let Some((depth, row)) = self.rows.get(index) else {
            return String::new();
        };
        let Some(doc) = &self.doc else {
            return String::new();
        };
        let prefix = "    ".repeat(depth.min(24));
        if matches!(row, Row::Lines) {
            let mark = if self.lines_open { "▾" } else { "▸" };
            return if self.indexed {
                format!("{mark}  JSONL　{} 条记录", self.records.len())
            } else {
                format!("{mark}  JSONL")
            };
        }
        if matches!(row, Row::More(_)) {
            return format!("{prefix}    加载下一页…");
        }
        let label = match &row {
            Row::Record(i) => format!("#{}　行 {}", i + 1, self.records[*i].line),
            Row::Value(_) if depth == 0 => "$".into(),
            Row::Value(node) => node.label(doc.bytes()),
            _ => unreachable!(),
        };
        if let Row::Record(i) = &row
            && let Some(problem) = self.record_errors.get(&self.records[*i].start)
        {
            return format!("{prefix}    {label}　{problem}");
        }
        let Some(node) = self.node(&row) else {
            return format!("{prefix}    {label}　无效的 JSON");
        };
        let mark = if self.open.contains(&node.start) {
            "▾"
        } else if node.kind.container() {
            "▸"
        } else {
            " "
        };
        format!(
            "{prefix}{mark}  {label}    {}",
            node.preview(doc.bytes(), 96)
        )
    }
    pub fn detail(&self) -> String {
        let Some(doc) = &self.doc else {
            return String::new();
        };
        let Some((_, row)) = self.rows.get(self.selected) else {
            return String::new();
        };
        if let Row::Record(i) = &row {
            let r = self.records[*i];
            if self.record_errors.contains_key(&r.start) || self.node(&row).is_none() {
                let end = r.end.min(r.start.saturating_add(self.detail_limit));
                return format!(
                    "行 {}　{}",
                    r.line,
                    String::from_utf8_lossy(&doc.bytes()[r.start..end])
                );
            }
        }
        if let Some(n) = self.node(&row) {
            format!(
                "字节 {}　{}",
                n.start,
                n.preview(doc.bytes(), self.detail_limit)
            )
        } else {
            String::new()
        }
    }
    pub fn reveal(&mut self, offset: usize, path: Vec<Node>) -> Option<usize> {
        if self.doc.as_ref()?.format == Format::Lines {
            self.lines_open = true;
            if self.records.last().is_none_or(|r| r.end < offset) && !self.indexed {
                self.pending_record = Some((offset, path));
                return None;
            }
        }
        for pair in path.windows(2) {
            self.open.insert(pair[0].start);
            self.branches
                .entry(pair[0].start)
                .or_insert_with(|| Branch::new(&pair[0]))
                .add(pair[1].clone());
        }
        self.rebuild();
        let target = if self.doc.as_ref()?.format == Format::Lines && path.len() <= 1 {
            Row::Record(self.records.partition_point(|r| r.end < offset))
        } else {
            Row::Value(path.last()?.clone())
        };
        self.selected = self.rows.position(&target)?;
        Some(self.selected)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn rows_and_selection_survive_sparse_expansions() {
        let mut rows = VisibleRows::default();
        rows.push(0, Row::Lines);
        rows.add_records(4);
        rows.push(2, Row::More(20));
        rows.add_records(50_000);
        rows.push(2, Row::More(99));
        rows.add_records(2);
        for (i, row) in [
            (1, Row::Record(0)),
            (4, Row::Record(3)),
            (5, Row::More(20)),
            (6, Row::Record(4)),
            (50_005, Row::Record(50_003)),
            (50_006, Row::More(99)),
            (50_007, Row::Record(50_004)),
        ] {
            assert!(rows.get(i).unwrap().1 == row);
            assert_eq!(rows.position(&row), Some(i));
        }
        assert_eq!(rows.explicit.len(), 3);
        assert!(rows.get(rows.len()).is_none());
    }
}
