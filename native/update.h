#pragma once
#include <windows.h>
#include <stdbool.h>
#include <cui/cui.h>
#include <win_autoupdate.h>

#define RELEASES_URL L"https://github.com/Hunlongyu/json-viewer/releases"
typedef struct {
    HWND window;
    HANDLE thread, install_lock;
    au_ctx_t *ctx;
    au_release_t *release;
    const au_asset_t *asset;
    au_err_t error;
    enum { UPDATE_IDLE, CHECKING, AVAILABLE, DOWNLOADING, DOWNLOADED, UPDATE_MESSAGE } state;
    bool open, empty, background;
    volatile LONG progress;
    char message[256], path[32768];
} ViewerUpdate;
/* Background checks only open the dialog when an update is available. */
void update_check(ViewerUpdate *u, HWND window, bool background);
void update_draw(ViewerUpdate *u, cui_ctx *ui, bool exporting);
void update_dispose(ViewerUpdate *u);
void update_after_restart(int argc, wchar_t **argv);
