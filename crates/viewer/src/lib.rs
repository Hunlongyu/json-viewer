//! Shared NSH view used by the Win32 and comparison hosts.

use nsh_core::element::input;
use nsh_core::prelude::*;
use preview_reader::SearchMode;
use std::time::Duration;
mod actions;
mod model;

fn button(label: impl IntoReactive<SharedString>, action: impl Fn() + 'static) -> impl IntoElement {
    view()
        .h(px(32.))
        .px_3()
        .justify_center()
        .items_center()
        .rounded(px(5.))
        .bg(rgb(0xe9eef4))
        .hover(|s| s.bg(rgb(0xd8e4ef)))
        .cursor_pointer()
        .focusable(true)
        .on_press(action)
        .child(text(label).text_size(px(13.)).text_color(rgb(0x263a50)))
}

#[component]
pub fn App() -> impl IntoElement {
    let list = element_ref();
    let state = actions::AppState::new(list);
    let field = element_ref();
    let field_focus = field;
    on_key("ctrl+o", move |_| state.pick());
    on_key("ctrl+f", move |_| field_focus.focus());
    on_key("f3", move |_| state.search());
    on_key("up", move |_| state.select(-1));
    on_key("down", move |_| state.select(1));
    on_key("home", move |_| state.select(isize::MIN));
    on_key("end", move |_| state.select(isize::MAX));
    on_key("right", move |_| {
        if let Some(i) = state.read(|m| {
            let row = &m.rows.get(m.selected)?.1;
            let expand = match row {
                model::Row::Lines => !m.lines_open,
                model::Row::More(_) => true,
                _ => m
                    .node(row)
                    .is_some_and(|n| n.kind.container() && !m.open.contains(&n.start)),
            };
            expand.then_some(m.selected)
        }) {
            state.activate(i);
        }
    });
    on_key("left", move |_| {
        if let Some(i) = state.read(|m| {
            let row = &m.rows.get(m.selected)?.1;
            let collapse = match row {
                model::Row::Lines => m.lines_open,
                _ => m.node(row).is_some_and(|n| m.open.contains(&n.start)),
            };
            collapse.then_some(m.selected)
        }) {
            state.activate(i);
        }
    });
    if let Some(file) = std::env::args_os().nth(1) {
        state.open(file.into());
    }
    let _watch = set_interval(Duration::from_secs(2), move || {
        if state.read(|m| m.doc.as_ref().is_some_and(|doc| doc.changed())) {
            state.model.with(|m| {
                m.reset_file();
                m.status = "源文件已变更，请重新打开".into();
            });
            state.refresh_layout();
        }
    });
    let toolbar = view()
        .flex_row()
        .items_center()
        .gap_2()
        .px_4()
        .h(px(52.))
        .bg(rgb(0xf6f8fb))
        .child(button("打开文件".to_owned(), move || state.pick()))
        .child(
            text(move || {
                state.read(|m| {
                    m.doc
                        .as_ref()
                        .map(|d| d.path.to_string_lossy().into_owned())
                        .unwrap_or_default()
                })
            })
            .flex_1()
            .truncate()
            .text_size(px(13.)),
        );
    let searchbar = view()
        .flex_row()
        .items_center()
        .gap_2()
        .px_4()
        .h(px(46.))
        .child(
            input()
                .element_ref(&field)
                .flex_1()
                .h(px(32.))
                .px_2()
                .rounded(px(4.))
                .bg(rgb(0xf1f4f7))
                .placeholder("搜索文件　Ctrl+F")
                .value(move || state.read(|m| m.query.clone()))
                .on_change(move |s: &str| state.query(s))
                .on_submit(move |_: &str| state.search()),
        )
        .child(button(
            move || {
                state.read(|m| {
                    match m.mode {
                        SearchMode::Raw => "原文",
                        SearchMode::Keys => "键名",
                        SearchMode::Values => "值",
                    }
                    .to_owned()
                })
            },
            move || state.mode(),
        ))
        .child(button(
            move || {
                state.read(|m| {
                    if m.next_match.is_some() {
                        "下一处"
                    } else {
                        "查找"
                    }
                    .to_owned()
                })
            },
            move || state.search(),
        ))
        .child(button("取消搜索".to_owned(), move || state.cancel()));
    let tree = virtual_list(
        move || state.layout(|m| m.rows.len()),
        move |at| {
            view()
                .h(px(28.))
                .px_4()
                .justify_center()
                .overflow_hidden()
                .bg(move || {
                    state.read(|m| {
                        if at.index() == m.selected {
                            rgb(0xd9eaff)
                        } else {
                            rgb(0xffffff)
                        }
                    })
                })
                .cursor_pointer()
                .on_press(move || state.activate(at.index()))
                .child(
                    text(move || state.read(|m| m.display(at.index())))
                        .truncate()
                        .text_size(px(14.)),
                )
        },
    )
    .keys(move || state.layout(|m| m.rows.keys()))
    .estimated_item_size(px(28.))
    .viewport(nsh_core::ListViewport::Parent)
    .focusable(true)
    .element_ref(&list);
    let tree = nsh_core::element::scroll().flex_1().child(tree);
    let detail = view()
        .flex_row()
        .items_center()
        .gap_2()
        .px_4()
        .py_2()
        .bg(rgb(0xf6f8fb))
        .child(
            nsh_core::element::scroll()
                .flex_1()
                .h(move || state.read(|m| px(if m.detail_limit > 256 { 128. } else { 28. })))
                .child(
                    text(move || state.read(|m| m.detail()))
                        .selectable(true)
                        .text_size(px(13.)),
                ),
        )
        .child(button("更多内容".to_owned(), move || {
            state
                .model
                .with(|m| m.detail_limit = m.detail_limit.saturating_mul(2).min(1024 * 1024));
            state.refresh();
        }));
    view()
        .flex_1()
        .bg(rgb(0xffffff))
        .text_color(rgb(0x203044))
        .child(toolbar)
        .child(searchbar)
        .child(tree)
        .child(detail)
        .child(
            text(move || state.read(|m| m.status.clone()))
                .px_4()
                .py_2()
                .text_size(px(12.))
                .text_color(rgb(0x40576a)),
        )
}
