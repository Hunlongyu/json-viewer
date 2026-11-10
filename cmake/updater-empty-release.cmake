# GitHub redirects /releases/latest to the repository's /releases page when
# there is no stable release. Handle it just like an empty REST response.
file(READ "${update_src}/au_web.c" web_source)
set(marker "    int ok = au_web_tag_from_location(location, tag, sizeof tag);")
string(FIND "${web_source}" "${marker}" offset)
if(offset LESS 0)
  message(FATAL_ERROR "Pinned updater's release redirect handling has changed")
endif()
set(empty_release [=[
    char* list = format_alloc("https://github.com/%s/%s/releases", ctx->owner, ctx->repo);
    if (!list) { free(location); return AU_ERR_NOMEM; }
    int empty = location && (!strcmp(location, list) || !strcmp(location, list + strlen("https://github.com")));
    free(list);
    if (empty) {
        free(location);
        au_logf(ctx, AU_LOG_WARN, "no usable release in channel");
        return AU_ERR_PARSE;
    }
]=])
string(REPLACE "${marker}" "${empty_release}${marker}" web_source "${web_source}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/au_web.c" CONTENT "${web_source}" @ONLY)
