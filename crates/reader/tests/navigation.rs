use preview_reader::{Cancel, Children, Document, Format, Locator, Node, SearchMode};
use std::{fs, path::PathBuf};

fn sample(name: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../bin")
        .join(name)
}
fn fixture(data: &[u8], name: &str) -> (tempfile::TempDir, Document) {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join(name);
    fs::write(&path, data).unwrap();
    let doc = Document::open(&path, &Cancel::default()).unwrap();
    (dir, doc)
}
fn offsets(doc: &Document, query: &str, mode: SearchMode) -> Vec<usize> {
    let mut found = Vec::new();
    doc.search(query, mode, &Cancel::default(), |h| {
        found.push(h.offset);
        true
    })
    .unwrap();
    found
}

#[test]
fn sample_navigation() {
    let cancel = Cancel::default();
    for (file, count) in [("23MB.json", 3), ("30MB.json", 4)] {
        let doc = Document::open(&sample(file), &cancel).unwrap();
        let root = doc.root().unwrap();
        assert_eq!(root.start, 3);
        let mut iter = Children::new(&root);
        let mut names = Vec::new();
        while let Some(n) = iter.next(doc.bytes(), &cancel).unwrap() {
            names.push(n.label(doc.bytes()));
            if n.label(doc.bytes()) == "Runtime" {
                let mut children = Children::new(&n);
                let mut has_types = false;
                while let Some(c) = children.next(doc.bytes(), &cancel).unwrap() {
                    has_types |= c.label(doc.bytes()) == "Types";
                }
                assert!(has_types);
            }
        }
        assert_eq!(names.len(), count);
        assert_eq!(names[0], "Id");
        assert_eq!(names.last().unwrap(), "Platform");
    }
    for (file, count) in [("sdk-events_s.jsonl", 24325), ("sdk-events_x.jsonl", 50232)] {
        let doc = Document::open(&sample(file), &cancel).unwrap();
        let mut records = Vec::new();
        doc.records(&cancel, |r| {
            records.push(r);
            true
        });
        assert_eq!(records.len(), count);
        for i in [0, count / 2, count - 1] {
            let r = records[i];
            doc.node(r.start..r.end)
                .unwrap()
                .validate(doc.bytes(), &cancel)
                .unwrap();
        }
    }
}

#[test]
fn repeated_keys_and_escaped_display_values_resolve_to_distinct_nodes() {
    let data = br#"{"same":"\u4e2d\u6587","same":["x",{"\u4e2d\u6587":"ok"}]}"#;
    let (_dir, doc) = fixture(data, "duplicates.json");
    let keys = offsets(&doc, "same", SearchMode::Keys);
    assert_eq!(keys.len(), 2);
    let mut locate = Locator::new(doc.root().unwrap());
    let mut starts = Vec::new();
    for pos in keys {
        let path = locate.locate(data, pos, &Cancel::default()).unwrap();
        assert_eq!(path.len(), 2);
        starts.push(path.last().unwrap().start);
    }
    assert_ne!(starts[0], starts[1]);
    assert!(offsets(&doc, "中文", SearchMode::Raw).is_empty());
    assert_eq!(offsets(&doc, "中文", SearchMode::Keys).len(), 1);
    assert_eq!(offsets(&doc, "中文", SearchMode::Values), vec![starts[0]]);
    let end = locate
        .locate(data, data.len() - 1, &Cancel::default())
        .unwrap();
    assert_eq!(end.len(), 1);
}

#[test]
fn search_across_blocks_and_stop_at_first_hit() {
    let mut data = vec![b' '; 256 * 1024 - 2];
    data.extend_from_slice(br#""abcdefabcdef""#);
    let (_dir, doc) = fixture(&data, "crossing.json");
    assert_eq!(
        offsets(&doc, "abcdef", SearchMode::Raw),
        vec![256 * 1024 - 1, 256 * 1024 + 5]
    );
    let mut count = 0;
    doc.search("abcdef", SearchMode::Raw, &Cancel::default(), |_| {
        count += 1;
        false
    })
    .unwrap();
    assert_eq!(count, 1);
}

#[test]
fn long_string_preview_never_shows_half_an_escape_or_unicode_character() {
    let data = r#""a\u4e2d\u6587\uD83D\uDE00字符long tail""#.as_bytes();
    let n = Node::at(data, 0..data.len()).unwrap();
    assert_eq!(n.preview(data, 11), "a中…");
    assert_eq!(n.preview(data, 21), "a中文…");
    assert_eq!(n.preview(data, 28), "a中文😀…");
}

#[test]
fn invalid_line_preserves_following_line_and_physical_number() {
    let (_dir, doc) = fixture(
        b"\xef\xbb\xbf\r\n{bad}\r\n\n{\"ok\":true}",
        "records.NDJSON",
    );
    assert_eq!(doc.format, Format::Lines);
    let mut lines = Vec::new();
    doc.records(&Cancel::default(), |r| {
        lines.push(r);
        true
    });
    assert_eq!(lines.iter().map(|r| r.line).collect::<Vec<_>>(), [2, 4]);
    let bad = doc.node(lines[0].start..lines[0].end).unwrap();
    assert_eq!(
        bad.validate(doc.bytes(), &Cancel::default())
            .unwrap_err()
            .offset,
        lines[0].start + 1
    );
    assert_eq!(offsets(&doc, "ok", SearchMode::Keys).len(), 1);
}

#[test]
fn positional_line_index_matches_mapped_offsets_across_blocks() {
    let mut data = b"\xef\xbb\xbf".to_vec();
    data.extend(std::iter::repeat_n(b' ', 256 * 1024 + 17));
    data.extend_from_slice(b"1\r\n  \n\"");
    data.extend(std::iter::repeat_n(b'x', 512 * 1024));
    data.extend_from_slice(b"\"\n\ntrue");
    let (_temp, doc) = fixture(&data, "index.jsonl");
    let mut expected = Vec::new();
    doc.records(&Cancel::default(), |r| {
        expected.push((r.line, r.start, r.end));
        true
    });
    let mut actual = Vec::new();
    doc.index_records(&Cancel::default(), |r| {
        actual.push((r.line, r.start, r.end));
        true
    })
    .unwrap();
    assert_eq!(actual, expected);
    assert_eq!(actual.iter().map(|r| r.0).collect::<Vec<_>>(), [1, 3, 5]);
}

#[test]
fn malformed_suffix_is_reported_after_initial_rows() {
    let data = br#"{"first":true,"big":{"x":[0,1,]}}"#;
    let root = Node::at(data, 0..data.len()).unwrap();
    let mut children = Children::new(&root);
    let cancel = Cancel::default();
    assert_eq!(
        children.next(data, &cancel).unwrap().unwrap().label(data),
        "first"
    );
    assert_eq!(
        children.next(data, &cancel).unwrap().unwrap().label(data),
        "big"
    );
    assert_eq!(
        children.next(data, &cancel).unwrap_err().offset,
        data.iter().position(|b| *b == b']').unwrap()
    );
}

#[cfg(windows)]
#[test]
fn active_writer_uses_a_stable_snapshot_and_invalidates_old_offsets() {
    use std::io::Write;
    let temp = tempfile::tempdir().unwrap();
    let path = temp.path().join("active.jsonl");
    let mut log = std::fs::File::create(&path).unwrap();
    log.write_all(b"{\"old\":1}\n").unwrap();
    log.flush().unwrap();
    let doc = Document::open(&path, &Cancel::default()).unwrap();
    assert!(!doc.changed());
    log.write_all(b"{\"new\":2}\n").unwrap();
    log.flush().unwrap();
    assert!(doc.changed());
    assert_eq!(doc.bytes(), b"{\"old\":1}\n");
}
