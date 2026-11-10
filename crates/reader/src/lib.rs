//! Read-only access to JSON and JSON Lines documents.
mod document;
mod search;
mod tree;

pub use document::{Document, Format, Record};
pub use search::{Hit, SearchMode};
pub use tree::{Children, Kind, Locator, Node, ScanError};

use std::sync::{
    Arc,
    atomic::{AtomicBool, Ordering},
};

#[derive(Clone, Default)]
pub struct Cancel(Arc<AtomicBool>);

impl Cancel {
    pub fn stop(&self) {
        self.0.store(true, Ordering::Relaxed);
    }
    pub fn stopped(&self) -> bool {
        self.0.load(Ordering::Relaxed)
    }
}
