#![windows_subsystem = "windows"]
fn main() -> std::process::ExitCode {
    let (source, options) = preview_host_comparison::viewer();
    nsh_platform_gpui::run_app(source, options)
}
