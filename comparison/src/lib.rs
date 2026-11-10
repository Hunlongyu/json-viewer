use nsh_platform_gpui::{AppOptions, AppSource, Root};

pub fn viewer() -> (AppSource, AppOptions) {
    (
        AppSource::Rust(Root::of::<json_preview::App>()),
        AppOptions {
            title: "JSON 预览".into(),
            app_id: "json-preview".into(),
            size: (960, 680),
            ..Default::default()
        },
    )
}
