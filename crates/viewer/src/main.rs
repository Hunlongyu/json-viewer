#![windows_subsystem = "windows"]
use nsh_platform_windows as platform;

fn main() -> impl std::process::Termination {
    platform::run_app(
        platform::AppSource::Rust(platform::Root::of::<json_preview::App>()),
        platform::AppOptions {
            title: "JSON 预览".into(),
            app_id: "json-preview".into(),
            size: (960, 680),
            ..Default::default()
        },
    )
}
