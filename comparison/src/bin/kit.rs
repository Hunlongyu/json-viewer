#![windows_subsystem = "windows"]
fn main() {
    gpui_kit::application()
        .with_assets(gpui_kit::assets::Assets)
        .run(|cx| {
            gpui_kit::init(cx);
            cx.on_window_closed(|cx, _| {
                if cx.windows().is_empty() {
                    cx.quit()
                }
            })
            .detach();
            let (source, options) = preview_host_comparison::viewer();
            nsh_platform_gpui::open_app_window(source, options, cx).expect("open JSON viewer");
            cx.activate(true);
        });
}
