#include "update.h"
#include <process.h>
#include <stdio.h>
#include <string.h>

#define UPDATE_ASSET "json-viewer.exe"
#define INSTALL_LOCK L"Local\\Hunlongyu.JsonViewer.Update"
/* EXE mode uses two sibling files. A ZIP staging directory is never ours. */
static bool update_file(wchar_t path[MAX_PATH], const wchar_t *suffix) {
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (!n || n >= MAX_PATH || wcslen(suffix) >= MAX_PATH - n)
        return false;
    wcscpy(path + n, suffix);
    DWORD attrs = GetFileAttributesW(path);
    if (attrs == INVALID_FILE_ATTRIBUTES)
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    return !(attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}
static void note(ViewerUpdate *u, const char *message) {
    snprintf(u->message, sizeof u->message, "%s", message);
    u->state = UPDATE_MESSAGE;
}
static int progress(uint64_t total, uint64_t done, void *arg) {
    ViewerUpdate *u = arg;
    if (total)
        InterlockedExchange(&u->progress, (LONG)(100.0 * done / total));
    return 0;
}
static void update_log(au_log_level_t level, const char *message, void *arg) {
    (void)level;
    if (!strcmp(message, "no usable release in channel"))
        ((ViewerUpdate *)arg)->empty = true;
}
static unsigned __stdcall check_worker(void *arg) {
    ViewerUpdate *u = arg;
    u->error = au_fetch_latest(u->ctx, &u->release);
    PostMessageW(u->window, WM_NULL, 0, 0);
    return 0;
}
static unsigned __stdcall download_worker(void *arg) {
    ViewerUpdate *u = arg;
    u->error = au_download(u->ctx, u->asset, u->path, sizeof u->path);
    PostMessageW(u->window, WM_NULL, 0, 0);
    return 0;
}
void update_dispose(ViewerUpdate *u) {
    if (u->thread) {
        au_cancel(u->ctx);
        WaitForSingleObject(u->thread, INFINITE);
        CloseHandle(u->thread);
    }
    if (u->install_lock && u->path[0]) {
        wchar_t path[MAX_PATH];
        if (update_file(path, L".autmp"))
            DeleteFileW(path);
    }
    if (u->install_lock)
        CloseHandle(u->install_lock);
    au_release_free(u->release);
    if (u->ctx)
        au_destroy(u->ctx);
    memset(u, 0, sizeof *u);
}
void update_check(ViewerUpdate *u, HWND window) {
    if (u->state != UPDATE_IDLE) {
        u->open = true;
        return;
    }
    u->window = window;
    u->open = true;
    au_config_t config = {.owner = "Hunlongyu",
                          .repo = "json-viewer",
                          .current_version = VIEWER_VERSION,
                          .asset_pattern = UPDATE_ASSET,
                          .mode = AU_MODE_EXE,
                          .timeout_ms = 8000,
                          .retry_max = 1,
                          .on_progress = progress,
                          .on_log = update_log,
                          .userdata = u};
    u->ctx = au_create(&config);
    u->state = CHECKING;
    if (!u->ctx || !(u->thread = (HANDLE)_beginthreadex(NULL, 0, check_worker, u, 0, NULL)))
        note(u, "Could not start the update check.");
}
static void finished(ViewerUpdate *u) {
    CloseHandle(u->thread);
    u->thread = NULL;
    if (u->error != AU_OK) {
        if (u->empty)
            note(u, "No stable release has been published yet.");
        else {
            snprintf(u->message, sizeof u->message, "Update failed: %s", au_strerror(u->error));
            u->state = UPDATE_MESSAGE;
        }
    } else if (u->state == DOWNLOADING) {
        u->state = DOWNLOADED;
    } else if (au_compare_current(u->ctx, u->release) <= 0) {
        note(u, "You are using the latest version.");
    } else if (!(u->asset = au_pick_asset(u->release, UPDATE_ASSET))) {
        note(u, "The new release has no matching Windows executable.");
    } else {
        u->state = AVAILABLE;
    }
}
static void download(ViewerUpdate *u) {
    u->install_lock = CreateMutexW(NULL, FALSE, INSTALL_LOCK);
    if (!u->install_lock || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (u->install_lock)
            CloseHandle(u->install_lock);
        u->install_lock = NULL;
        note(u, "Another update may be in progress. Please try again later.");
        return;
    }
    wchar_t path[MAX_PATH];
    if (!update_file(path, L".autmp") || !update_file(path, L".old")) {
        note(u, "Cannot create the update files next to this executable.");
        return;
    }
    u->state = DOWNLOADING;
    u->thread = (HANDLE)_beginthreadex(NULL, 0, download_worker, u, 0, NULL);
    if (!u->thread)
        note(u, "Could not start the download.");
}
void update_draw(ViewerUpdate *u, cui_ctx *ui, bool exporting) {
    if (u->thread && WaitForSingleObject(u->thread, 0) == WAIT_OBJECT_0)
        finished(u);
    if (!u->open) {
        if (u->thread)
            au_cancel(u->ctx);
        else if (u->state != UPDATE_IDLE)
            update_dispose(u);
    } else if (cui_dialog_begin(ui, "Check For Update", &u->open,
                                &(cui_dialog_opts){.width = 350})) {
        if (u->state == CHECKING)
            cui_text(ui, "Checking GitHub Releases…");
        else if (u->state == UPDATE_MESSAGE)
            cui_text(ui, u->message, .width = CUI_FILL);
        else {
            char text[160];
            snprintf(text, sizeof text, "Current: %s     Update: %s", VIEWER_VERSION,
                     u->release->ver.raw);
            cui_text(ui, text);
            if (u->state == AVAILABLE && cui_button(ui, "Download Update"))
                download(u);
            if (u->state == DOWNLOADING) {
                LONG percent = InterlockedCompareExchange(&u->progress, 0, 0);
                cui_progress(ui, percent / 100.f, .show_value = true, .width = CUI_FILL);
            }
            if (u->state == DOWNLOADED) {
                if (exporting)
                    cui_text(ui, "Waiting for the current transfer to finish.");
                else if (cui_button(ui, "Restart and Install")) {
                    au_err_t err = au_apply(u->ctx, u->path);
                    snprintf(u->message, sizeof u->message, "Installation failed: %s",
                             au_strerror(err));
                    u->state = UPDATE_MESSAGE;
                }
            }
        }
        cui_dialog_end(ui);
    }
    if (u->thread)
        cui_request_wakeup(ui, 0.08f);
}
void update_after_restart(int argc, wchar_t **argv) {
    au_wait_for_predecessor_w(argc, argv, 10000);
    HANDLE lock = CreateMutexW(NULL, FALSE, INSTALL_LOCK);
    if (lock) {
        if (GetLastError() != ERROR_ALREADY_EXISTS) {
            wchar_t path[MAX_PATH];
            if (update_file(path, L".old"))
                DeleteFileW(path);
        }
        CloseHandle(lock);
    }
}
