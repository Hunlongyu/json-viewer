#include "update.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); exit(1); \
} } while (0)

/* Exercise real worker completion and dialog policy without network or downloads. */
struct au_ctx { volatile LONG cancelled; };
static HANDLE gate;
static au_err_t fetch_error;
static int newer, dialogs, creates, cancels;
static bool has_asset, fail_create;
static au_asset_t asset;

au_ctx_t *au_create(const au_config_t *config) {
    CHECK(!strcmp(config->current_version, VIEWER_VERSION));
    CHECK(config->channel == AU_CHANNEL_STABLE);
    ++creates;
    return fail_create ? NULL : calloc(1, sizeof(au_ctx_t));
}
void au_destroy(au_ctx_t *ctx) { free(ctx); }
void au_cancel(au_ctx_t *ctx) {
    ++cancels;
    InterlockedExchange(&ctx->cancelled, 1);
    SetEvent(gate);
}
au_err_t au_fetch_latest(au_ctx_t *ctx, au_release_t **out) {
    CHECK(WaitForSingleObject(gate, 5000) == WAIT_OBJECT_0);
    if (InterlockedCompareExchange(&ctx->cancelled, 0, 0))
        return AU_ERR_CANCELED;
    if (fetch_error != AU_OK)
        return fetch_error;
    *out = calloc(1, sizeof **out);
    CHECK(*out);
    strcpy((*out)->ver.raw, "v99.0.0");
    return AU_OK;
}
void au_release_free(au_release_t *release) { free(release); }
int au_compare_current(au_ctx_t *ctx, const au_release_t *release) { return newer; }
const au_asset_t *au_pick_asset(const au_release_t *release, const char *pattern) {
    return has_asset ? &asset : NULL;
}
const char *au_strerror(au_err_t error) { return "test network error"; }
au_err_t au_download(au_ctx_t *ctx, const au_asset_t *a, char *path, size_t cap) {
    CHECK(false); /* Checking must never download automatically. */
    return AU_ERR_STATE;
}
au_err_t au_apply(au_ctx_t *ctx, const char *path) {
    CHECK(false);
    return AU_ERR_STATE;
}
void au_wait_for_predecessor_w(int argc, wchar_t **argv, int timeout_ms) {}

bool cui_dialog_begin(cui_ctx *ui, const char *title, bool *open, const cui_dialog_opts *opts) {
    CHECK(*open);
    ++dialogs;
    return true;
}
void cui_dialog_end(cui_ctx *ui) {}
void cui_text_ex(cui_ctx *ui, const char *text, const cui_text_opts *opts) {}
bool cui_button_ex(cui_ctx *ui, const char *label, const cui_button_opts *opts) { return false; }
void cui_progress_ex(cui_ctx *ui, float value, const cui_progress_opts *opts) {}
void cui_request_wakeup(cui_ctx *ui, float seconds) {}

static void reset(void) {
    ResetEvent(gate);
    fetch_error = AU_OK;
    newer = 1;
    has_asset = true;
    fail_create = false;
    dialogs = creates = cancels = 0;
}
static void finish(ViewerUpdate *u) {
    SetEvent(gate);
    CHECK(WaitForSingleObject(u->thread, 5000) == WAIT_OBJECT_0);
    update_draw(u, NULL, false);
    CHECK(!u->thread);
}
static void quiet_result(au_err_t error, int comparison, bool matching_asset) {
    reset();
    fetch_error = error;
    newer = comparison;
    has_asset = matching_asset;
    ViewerUpdate u = {0};
    update_check(&u, NULL, true);
    CHECK(u.thread && u.background && !u.open);
    update_draw(&u, NULL, false);
    CHECK(!dialogs && !cancels); /* Hidden checks must not be cancelled by drawing. */
    finish(&u);
    CHECK(u.state == UPDATE_IDLE && !u.open && !u.ctx && !u.release && !dialogs);
    update_dispose(&u);
}
int main(void) {
    gate = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK(gate);
    quiet_result(AU_OK, 0, true);
    quiet_result(AU_OK, -1, true);
    quiet_result(AU_ERR_NET, 1, true);
    quiet_result(AU_ERR_PARSE, 1, true);
    quiet_result(AU_OK, 1, false);

    reset();
    ViewerUpdate u = {0};
    update_check(&u, NULL, true);
    finish(&u);
    CHECK(u.state == AVAILABLE && u.open && !u.background && dialogs == 1);
    u.open = false;
    update_draw(&u, NULL, false);
    CHECK(u.state == UPDATE_IDLE && !u.ctx);

    reset();
    fetch_error = AU_ERR_NET;
    update_check(&u, NULL, true);
    HANDLE worker = u.thread;
    update_check(&u, NULL, false);
    CHECK(u.open && !u.background && u.thread == worker && creates == 1);
    finish(&u);
    CHECK(u.state == UPDATE_MESSAGE && u.open && dialogs == 1);
    CHECK(strstr(u.message, "Update failed"));
    update_dispose(&u);

    reset();
    newer = 0;
    update_check(&u, NULL, false);
    update_draw(&u, NULL, false);
    CHECK(dialogs == 1 && !cancels);
    finish(&u);
    CHECK(u.open && u.state == UPDATE_MESSAGE && strstr(u.message, "latest version"));
    update_dispose(&u);

    reset();
    update_check(&u, NULL, false);
    u.open = false;
    update_draw(&u, NULL, false);
    CHECK(cancels == 1);
    finish(&u);
    CHECK(u.state == UPDATE_IDLE && !u.ctx && !dialogs);

    reset();
    update_check(&u, NULL, true);
    update_dispose(&u);
    CHECK(cancels == 1 && !u.thread && !u.ctx);

    reset();
    fail_create = true;
    update_check(&u, NULL, true);
    CHECK(u.state == UPDATE_IDLE && !u.open && !u.background);
    update_check(&u, NULL, false);
    CHECK(u.state == UPDATE_MESSAGE && u.open);
    update_dispose(&u);
    CloseHandle(gate);
    puts("Passed startup update notifications, quiet failures, manual checks and cancellation.");
    return 0;
}
