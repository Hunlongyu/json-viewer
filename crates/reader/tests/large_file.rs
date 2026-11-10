use preview_reader::{Cancel, Children, Document, SearchMode};
use std::{
    fs::File,
    io::{BufWriter, Write},
};

#[test]
#[ignore = "creates a temporary 1 GiB JSON file"]
fn gibibyte_wide_array() {
    let temp = tempfile::tempdir().unwrap();
    let path = temp.path().join("wide.json");
    let mut row = vec![b'x'; 1024];
    row[0] = b'"';
    row[1022] = b'"';
    row[1023] = b',';
    let mut out = BufWriter::with_capacity(1024 * 1024, File::create(&path).unwrap());
    out.write_all(br#"{"wide":["#).unwrap();
    for _ in 0..(1024 * 1024) {
        out.write_all(&row).unwrap();
    }
    out.write_all(br#"null],"tail":"unique-end-of-gibibyte"}"#)
        .unwrap();
    out.flush().unwrap();
    drop(out);
    let cancel = Cancel::default();
    let doc = Document::open(&path, &cancel).unwrap();
    let root = doc.root().unwrap();
    let mut fields = Children::new(&root);
    let wide = fields.next(doc.bytes(), &cancel).unwrap().unwrap();
    assert_eq!(wide.label(doc.bytes()), "wide");
    let mut page = Children::new(&wide);
    for ordinal in 0..512 {
        assert_eq!(
            page.next(doc.bytes(), &cancel).unwrap().unwrap().ordinal,
            ordinal
        );
    }
    let mut found = Vec::new();
    doc.search("unique-end-of-gibibyte", SearchMode::Raw, &cancel, |hit| {
        found.push(hit.offset);
        true
    })
    .unwrap();
    assert_eq!(found.len(), 1);
    assert!(found[0] > 1024 * 1024 * 1024);
    assert_eq!(
        fields
            .next(doc.bytes(), &cancel)
            .unwrap()
            .unwrap()
            .label(doc.bytes()),
        "tail"
    );
    assert!(fields.next(doc.bytes(), &cancel).unwrap().is_none());
}
