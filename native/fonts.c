#include "fonts.h"
#include <commdlg.h>
#include <shlobj.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

enum { DEFAULT_SIZE = 13, MIN_SIZE = 8, MAX_SIZE = 48 };
static const wchar_t *const presets[] = {L"Inter", L"Segoe UI", L"Geist Mono"};

struct FontEntry {
    FontEntry *next;
    wchar_t family[LF_FACESIZE];
    int weight;
    bool italic;
    cui_font font;
    size_t byte_count;
    unsigned char bytes[];
};

static unsigned read_u32(const unsigned char *p) {
    return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3];
}

/* Extract only the selected face, including shared tables in a .ttc collection.
   The CUI loader expects a standalone sfnt or the first face of a collection. */
static FontEntry *read_face(HDC dc) {
    unsigned char header[12];
    if (GetFontData(dc, 0, 0, header, sizeof header) != sizeof header)
        return NULL;
    unsigned count = header[4] * 256u + header[5];
    if (!count || count > 4095)
        return NULL;
    unsigned directory_size = 12 + 16 * count;
    unsigned char *dir = malloc(directory_size);
    FontEntry *entry = NULL;
    if (!dir || GetFontData(dc, 0, 0, dir, directory_size) != directory_size)
        goto done;
    size_t size = directory_size;
    for (unsigned i = 0; i < count; i++)
        size += ((size_t)read_u32(dir + 12 + 16 * i + 12) + 3) & ~(size_t)3;
    if (size > UINT32_MAX)
        goto done;
    entry = calloc(1, sizeof *entry + size);
    if (!entry)
        goto done;
    entry->byte_count = size;
    memcpy(entry->bytes, dir, directory_size);
    unsigned offset = directory_size;
    for (unsigned i = 0; i < count; i++) {
        unsigned char *table = entry->bytes + 12 + 16 * i;
        DWORD tag;
        memcpy(&tag, table, sizeof tag);
        unsigned len = read_u32(table + 12);
        if (len && GetFontData(dc, tag, 0, entry->bytes + offset, len) != len) {
            free(entry);
            entry = NULL;
            goto done;
        }
        table[8] = (unsigned char)(offset >> 24);
        table[9] = (unsigned char)(offset >> 16);
        table[10] = (unsigned char)(offset >> 8);
        table[11] = (unsigned char)offset;
        offset += (len + 3) & ~3u;
    }
done:
    free(dir);
    return entry;
}

static cui_font resolve(TreeFonts *f, const wchar_t *family, int weight, bool italic) {
    for (FontEntry *e = f->loaded; e; e = e->next)
        if (weight == e->weight && italic == e->italic && !_wcsicmp(family, e->family))
            return e->font;

    LOGFONTW lf = {.lfHeight = -16,
                   .lfWeight = weight,
                   .lfItalic = italic,
                   .lfCharSet = DEFAULT_CHARSET,
                   .lfOutPrecision = OUT_TT_ONLY_PRECIS};
    wcscpy_s(lf.lfFaceName, LF_FACESIZE, family);
    HDC dc = CreateCompatibleDC(NULL);
    HFONT font = CreateFontIndirectW(&lf);
    HGDIOBJ old = dc && font ? SelectObject(dc, font) : NULL;
    wchar_t actual[LF_FACESIZE];
    FontEntry *e = NULL;
    if (old && GetTextFaceW(dc, LF_FACESIZE, actual) && !_wcsicmp(actual, family))
        e = read_face(dc);
    if (old)
        SelectObject(dc, old);
    if (font)
        DeleteObject(font);
    if (dc)
        DeleteDC(dc);
    if (!e) {
        /* Older settings may name a bundled face that is not installed in Windows. */
        if (weight == FW_NORMAL && !italic && !_wcsicmp(family, presets[0]))
            return CUI_FONT_BODY;
        if (weight == FW_NORMAL && !italic && !_wcsicmp(family, presets[2]))
            return CUI_FONT_MONO;
        return -1;
    }
    e->font = cui_font_load_memory(f->ui, e->bytes, e->byte_count, false);
    if (e->font < 0) {
        free(e);
        return -1;
    }
    if (f->fallback >= 0)
        cui_font_add_fallback(f->ui, e->font, f->fallback);
    wcscpy_s(e->family, LF_FACESIZE, family);
    e->weight = weight;
    e->italic = italic;
    e->next = f->loaded;
    f->loaded = e;
    return e->font;
}

static bool select_face(TreeFonts *f, const wchar_t *family, int weight, bool italic) {
    cui_font font = resolve(f, family, weight, italic);
    if (font < 0)
        return false;
    wcscpy_s(f->family, LF_FACESIZE, family);
    f->font = font;
    f->weight = weight;
    f->italic = italic;
    return true;
}

static void config_path(TreeFonts *f) {
    wchar_t dir[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, dir)) ||
        wcslen(dir) + wcslen(L"\\JSON Viewer\\settings.ini") >= MAX_PATH)
        return;
    wcscat_s(dir, MAX_PATH, L"\\JSON Viewer");
    if (!CreateDirectoryW(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return;
    swprintf(f->ini, MAX_PATH, L"%ls\\settings.ini", dir);
}

/* Publish the family and size together. A second window can never read a
   partially written pair. UTF-16LE preserves localized Windows family names. */
static bool save(const TreeFonts *f, bool replace) {
    if (!f->ini[0])
        return false;
    wchar_t dir[MAX_PATH], tmp[MAX_PATH], data[160];
    wcscpy_s(dir, MAX_PATH, f->ini);
    *wcsrchr(dir, L'\\') = 0;
    if (!GetTempFileNameW(dir, L"jvf", 0, tmp))
        return false;
    HANDLE file =
        CreateFileW(tmp, GENERIC_WRITE, 0, NULL, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    bool ok = false;
    if (file != INVALID_HANDLE_VALUE) {
        int n = swprintf(data, _countof(data),
                         L"\xfeff[Tree]\r\nFamily=%ls\r\nSize=%d\r\nWeight=%d\r\nItalic=%d\r\n",
                         f->family, f->size, f->weight, f->italic);
        DWORD written, len = (DWORD)(n * sizeof(wchar_t));
        ok = n > 0 && WriteFile(file, data, len, &written, NULL) && written == len;
        CloseHandle(file);
    }
    if (ok && MoveFileExW(tmp, f->ini, replace ? MOVEFILE_REPLACE_EXISTING : 0))
        return true;
    DeleteFileW(tmp);
    return false;
}

void fonts_init(TreeFonts *f, cui_ctx *ui, cui_font fallback) {
    f->ui = ui;
    f->fallback = fallback;
    f->size = DEFAULT_SIZE;
    config_path(f);
    wchar_t family[LF_FACESIZE + 1] = L"Segoe UI";
    int weight = FW_NORMAL;
    bool italic = false;
    bool fresh = f->ini[0] && GetFileAttributesW(f->ini) == INVALID_FILE_ATTRIBUTES &&
                 GetLastError() == ERROR_FILE_NOT_FOUND;
    if (fresh) {
        DWORD index, bytes = sizeof index;
        if (!RegGetValueW(HKEY_CURRENT_USER, L"Software\\Hunlongyu\\JSON Viewer", L"TreeFont",
                          RRF_RT_REG_DWORD, NULL, &index, &bytes) &&
            index < _countof(presets))
            wcscpy_s(family, _countof(family), presets[index]);
    } else if (f->ini[0]) {
        GetPrivateProfileStringW(L"Tree", L"Family", L"Segoe UI", family, _countof(family), f->ini);
        wchar_t value[32], *end;
        GetPrivateProfileStringW(L"Tree", L"Size", L"", value, _countof(value), f->ini);
        long size = wcstol(value, &end, 10);
        if (!*end && size >= MIN_SIZE && size <= MAX_SIZE)
            f->size = (int)size;
        UINT saved_weight = GetPrivateProfileIntW(L"Tree", L"Weight", FW_NORMAL, f->ini);
        if (saved_weight >= 1 && saved_weight <= 1000)
            weight = (int)saved_weight;
        italic = GetPrivateProfileIntW(L"Tree", L"Italic", 0, f->ini) == 1;
    }
    if (!family[0] || wcslen(family) >= LF_FACESIZE || !select_face(f, family, weight, italic)) {
        if (!select_face(f, L"Segoe UI", FW_NORMAL, false))
            select_face(f, L"Inter", FW_NORMAL, false);
    }
    if (fresh)
        save(f, false);
}

void fonts_dispose(TreeFonts *f) {
    while (f->loaded) {
        FontEntry *entry = f->loaded;
        f->loaded = entry->next;
        free(entry);
    }
}

bool fonts_choose(TreeFonts *f, HWND owner) {
    LOGFONTW lf = {.lfHeight = -MulDiv(f->size, GetDpiForWindow(owner), 96),
                   .lfWeight = f->weight,
                   .lfItalic = f->italic,
                   .lfCharSet = DEFAULT_CHARSET};
    wcscpy_s(lf.lfFaceName, LF_FACESIZE, f->family);
    CHOOSEFONTW dialog = {.lStructSize = sizeof dialog,
                          .hwndOwner = owner,
                          .lpLogFont = &lf,
                          .Flags = CF_SCREENFONTS | CF_TTONLY | CF_NOVERTFONTS | CF_NOSIMULATIONS |
                                   CF_INITTOLOGFONTSTRUCT | CF_LIMITSIZE | CF_FORCEFONTEXIST,
                          .nSizeMin = MIN_SIZE * 72 / 96,
                          .nSizeMax = MAX_SIZE * 72 / 96};
    if (!ChooseFontW(&dialog)) {
        if (CommDlgExtendedError())
            MessageBoxW(owner, L"Windows could not open the font dialog.", L"JSON Viewer", MB_ICONERROR);
        return false;
    }
    if (!select_face(f, lf.lfFaceName, lf.lfWeight, lf.lfItalic != 0)) {
        MessageBoxW(owner, L"This font could not be loaded.", L"JSON Viewer", MB_ICONINFORMATION);
        return false;
    }
    /* iPointSize is in tenths of a point; the tree uses 96-DPI logical pixels. */
    f->size = MulDiv(dialog.iPointSize, 96, 720);
    if (!save(f, true))
        MessageBoxW(owner, L"The font was applied, but settings.ini could not be saved.",
                    L"JSON Viewer", MB_ICONWARNING);
    return true;
}
