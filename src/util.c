#include <windows.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "util.h"

wchar_t *utf8_to_wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w;

    if (n <= 0)
        return NULL;
    w = malloc(n * sizeof(wchar_t));
    if (w == NULL)
        return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

char *wide_to_utf8(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s;

    if (n <= 0)
        return NULL;
    s = malloc(n);
    if (s == NULL)
        return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

FILE *fopen_utf8(const char *path, const char *mode)
{
    wchar_t *wpath = utf8_to_wide(path);
    wchar_t *wmode = utf8_to_wide(mode);
    FILE *f = NULL;

    if (wpath != NULL && wmode != NULL)
        f = _wfopen(wpath, wmode);
    free(wpath);
    free(wmode);
    return f;
}

char *read_file_limited(const char *path, size_t limit, size_t *out_len, int *truncated)
{
    FILE *f = fopen_utf8(path, "rb");
    char *buf = NULL;
    size_t len = 0, cap = 0, got, want;

    if (truncated != NULL)
        *truncated = 0;
    if (f == NULL)
        return NULL;

    for (;;) {
        if (cap - len < 65536) {
            size_t new_cap = cap == 0 ? 65536 : cap * 2;
            char *p = realloc(buf, new_cap + 1);
            if (p == NULL) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = p;
            cap = new_cap;
        }
        want = cap - len;
        if (limit > 0 && len + want > limit)
            want = limit - len;
        if (want == 0) { /* 已到上限：再試讀 1 byte 看後面還有沒有內容 */
            char probe;
            if (truncated != NULL && fread(&probe, 1, 1, f) == 1)
                *truncated = 1;
            break;
        }
        got = fread(buf + len, 1, want, f);
        len += got;
        if (got == 0)
            break;
    }
    fclose(f);

    buf[len] = '\0';
    if (out_len != NULL)
        *out_len = len;
    return buf;
}

char *read_file(const char *path, size_t *out_len)
{
    return read_file_limited(path, 0, out_len, NULL);
}

int write_file(const char *path, const char *data, size_t len)
{
    FILE *f = fopen_utf8(path, "wb");
    size_t written = 0;

    if (f == NULL)
        return 0;
    if (len > 0)
        written = fwrite(data, 1, len, f);
    return fclose(f) == 0 && written == len;
}

static DWORD get_attributes(const char *path)
{
    wchar_t *w = utf8_to_wide(path);
    DWORD attr;

    if (w == NULL)
        return INVALID_FILE_ATTRIBUTES;
    attr = GetFileAttributesW(w);
    free(w);
    return attr;
}

int file_exists(const char *path)
{
    DWORD attr = get_attributes(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

int dir_exists(const char *path)
{
    DWORD attr = get_attributes(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

int make_dir(const char *path)
{
    char buf[GRADER_PATH_MAX];
    size_t i;

    snprintf(buf, sizeof(buf), "%s", path);

    /* 逐層建立：C:\a\b\c -> C:\a、C:\a\b、C:\a\b\c */
    for (i = 1; buf[i] != '\0'; i++) {
        if ((buf[i] == '\\' || buf[i] == '/') && buf[i - 1] != ':') {
            char saved = buf[i];
            buf[i] = '\0';
            if (!dir_exists(buf)) {
                wchar_t *w = utf8_to_wide(buf);
                if (w != NULL) {
                    CreateDirectoryW(w, NULL);
                    free(w);
                }
            }
            buf[i] = saved;
        }
    }
    if (!dir_exists(buf)) {
        wchar_t *w = utf8_to_wide(buf);
        if (w != NULL) {
            CreateDirectoryW(w, NULL);
            free(w);
        }
    }
    return dir_exists(buf);
}

/* 剛結束的程式有時還沒完全釋放 .exe，刪除失敗就稍等再試幾次 */
static int delete_file_retry(const wchar_t *w)
{
    int i;
    SetFileAttributesW(w, FILE_ATTRIBUTE_NORMAL);
    for (i = 0; i < 10; i++) {
        if (DeleteFileW(w))
            return 1;
        if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND)
            return 1;
        Sleep(50);
    }
    return 0;
}

int delete_file(const char *path)
{
    wchar_t *w = utf8_to_wide(path);
    int ok;

    if (w == NULL)
        return 0;
    ok = delete_file_retry(w);
    free(w);
    return ok;
}

int remove_dir_recursive(const char *path)
{
    StringList entries = {0};
    int ok = 1;
    int i;
    wchar_t *w;

    if (!dir_exists(path))
        return 1;

    list_dir(path, "*", 1, &entries);
    for (i = 0; i < entries.count; i++) {
        char child[GRADER_PATH_MAX];
        path_join(child, sizeof(child), path, entries.items[i]);
        if (!remove_dir_recursive(child))
            ok = 0;
    }
    string_list_free(&entries);

    list_dir(path, "*", 0, &entries);
    for (i = 0; i < entries.count; i++) {
        char child[GRADER_PATH_MAX];
        path_join(child, sizeof(child), path, entries.items[i]);
        w = utf8_to_wide(child);
        if (w == NULL || !delete_file_retry(w))
            ok = 0;
        free(w);
    }
    string_list_free(&entries);

    w = utf8_to_wide(path);
    if (w == NULL || !RemoveDirectoryW(w))
        ok = 0;
    free(w);
    return ok;
}

void path_join(char *out, size_t size, const char *a, const char *b)
{
    size_t n = strlen(a);

    if (n > 0 && (a[n - 1] == '\\' || a[n - 1] == '/'))
        snprintf(out, size, "%s%s", a, b);
    else
        snprintf(out, size, "%s\\%s", a, b);
}

int make_run_temp_dir(char *out, size_t size)
{
    wchar_t wtemp[MAX_PATH];
    char *temp;
    char name[64], base[GRADER_PATH_MAX];

    if (GetTempPathW(MAX_PATH, wtemp) == 0)
        return 0;
    temp = wide_to_utf8(wtemp);
    if (temp == NULL)
        return 0;
    path_join(base, sizeof(base), temp, "c-grader");
    free(temp);
    snprintf(name, sizeof(name), "run-%lu-%llu", (unsigned long)GetCurrentProcessId(),
             (unsigned long long)GetTickCount64());
    path_join(out, size, base, name);
    return make_dir(out);
}

int is_valid_utf8(const char *s, size_t len)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;

    while (i < len) {
        int extra, j;
        if (p[i] < 0x80)                extra = 0;
        else if ((p[i] & 0xE0) == 0xC0) extra = 1;
        else if ((p[i] & 0xF0) == 0xE0) extra = 2;
        else if ((p[i] & 0xF8) == 0xF0) extra = 3;
        else                            return 0;
        for (j = 1; j <= extra; j++)
            if (i + j >= len || (p[i + j] & 0xC0) != 0x80)
                return 0;
        i += 1 + extra;
    }
    return 1;
}

void app_folder(char *out, size_t size)
{
    wchar_t exe[MAX_PATH];
    char *path, *slash;

    out[0] = '\0';
    if (GetModuleFileNameW(NULL, exe, MAX_PATH) == 0)
        return;
    path = wide_to_utf8(exe);
    if (path == NULL)
        return;
    slash = strrchr(path, '\\');
    if (slash != NULL)
        *slash = '\0';
    slash = strrchr(path, '\\');
    if (slash != NULL && _stricmp(slash + 1, "build") == 0)
        *slash = '\0';
    snprintf(out, size, "%s", path);
    free(path);
}

int absolute_path(const char *path, char *out, size_t size)
{
    wchar_t *w = utf8_to_wide(path);
    wchar_t full[GRADER_PATH_MAX];
    char *s;
    DWORD n;

    if (w == NULL)
        return 0;
    n = GetFullPathNameW(w, GRADER_PATH_MAX, full, NULL);
    free(w);
    if (n == 0 || n >= GRADER_PATH_MAX)
        return 0;

    /* 去掉結尾的 \，例如 "D:\Homework\" -> "D:\Homework" */
    while (n > 3 && (full[n - 1] == L'\\' || full[n - 1] == L'/'))
        full[--n] = L'\0';

    s = wide_to_utf8(full);
    if (s == NULL)
        return 0;
    snprintf(out, size, "%s", s);
    free(s);
    return 1;
}

void string_list_add(StringList *list, const char *s)
{
    char **p = realloc(list->items, (list->count + 1) * sizeof(char *));
    if (p == NULL)
        return;
    list->items = p;
    list->items[list->count] = _strdup(s);
    list->count++;
}

void string_list_free(StringList *list)
{
    int i;
    for (i = 0; i < list->count; i++)
        free(list->items[i]);
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

static int compare_names(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int list_dir(const char *dir, const char *pattern, int want_dirs, StringList *out)
{
    char search[GRADER_PATH_MAX];
    wchar_t *wsearch;
    WIN32_FIND_DATAW fd;
    HANDLE h;

    path_join(search, sizeof(search), dir, pattern);
    wsearch = utf8_to_wide(search);
    if (wsearch == NULL)
        return 0;

    h = FindFirstFileW(wsearch, &fd);
    free(wsearch);
    if (h == INVALID_HANDLE_VALUE)
        return 0;

    do {
        int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        if (is_dir == (want_dirs != 0)) {
            char *name = wide_to_utf8(fd.cFileName);
            if (name != NULL) {
                string_list_add(out, name);
                free(name);
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (out->count > 1)
        qsort(out->items, out->count, sizeof(char *), compare_names);
    return out->count;
}

void sb_append_len(StrBuf *sb, const char *s, size_t n)
{
    if (sb->len + n + 1 > sb->cap) {
        size_t new_cap = sb->cap == 0 ? 256 : sb->cap;
        char *p;
        while (sb->len + n + 1 > new_cap)
            new_cap *= 2;
        p = realloc(sb->data, new_cap);
        if (p == NULL)
            return;
        sb->data = p;
        sb->cap = new_cap;
    }
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

void sb_append(StrBuf *sb, const char *s)
{
    sb_append_len(sb, s, strlen(s));
}

void sb_appendf(StrBuf *sb, const char *fmt, ...)
{
    char small[1024];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(small, sizeof(small), fmt, ap);
    va_end(ap);

    if (n < 0)
        return;
    if ((size_t)n < sizeof(small)) {
        sb_append_len(sb, small, (size_t)n);
    } else {
        char *big = malloc(n + 1);
        if (big == NULL)
            return;
        va_start(ap, fmt);
        vsnprintf(big, n + 1, fmt, ap);
        va_end(ap);
        sb_append_len(sb, big, (size_t)n);
        free(big);
    }
}

void sb_free(StrBuf *sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->len = sb->cap = 0;
}

void format_score(char *out, size_t size, double value)
{
    if (fabs(value - floor(value + 0.5)) < 1e-9)
        snprintf(out, size, "%.0f", value);
    else
        snprintf(out, size, "%.2f", value);
}

/* Big5 (字碼頁 950) -> UTF-8，回傳 malloc 的字串；轉不了 (這台電腦沒有 Big5 字碼頁) 回傳 NULL */
static char *big5_to_utf8(const char *s, size_t n, size_t *out_len)
{
    int wn, un;
    wchar_t *w;
    char *u;

    wn = MultiByteToWideChar(950, 0, s, (int)n, NULL, 0);
    if (wn <= 0)
        return NULL;
    w = malloc((wn + 1) * sizeof(wchar_t));
    if (w == NULL)
        return NULL;
    MultiByteToWideChar(950, 0, s, (int)n, w, wn);
    un = WideCharToMultiByte(CP_UTF8, 0, w, wn, NULL, 0, NULL, NULL);
    u = malloc(un + 1);
    if (u == NULL) {
        free(w);
        return NULL;
    }
    WideCharToMultiByte(CP_UTF8, 0, w, wn, u, un, NULL, NULL);
    u[un] = '\0';
    free(w);
    *out_len = (size_t)un;
    return u;
}

/*
 * 不是 UTF-8 的文字 (Big5) 轉成 UTF-8；會 free 原本的 text。
 * 逐行判斷：本身就是合法 UTF-8 的行原樣保留，其他行視為 Big5 轉換，
 * 所以「Big5 註解 + UTF-8 字串」這種混雜的檔案 (兩個編輯器輪流存檔) 也能正確處理。
 */
char *text_to_utf8(char *text, size_t *len)
{
    StrBuf out = {0};
    size_t pos = 0;

    if (text == NULL || *len == 0 || is_valid_utf8(text, *len))
        return text;

    while (pos < *len) {
        size_t end = pos, ulen = 0;
        char *u;

        while (end < *len && text[end] != '\n')
            end++;
        if (end < *len)
            end++; /* 連同換行一起 */
        u = is_valid_utf8(text + pos, end - pos) ? NULL : big5_to_utf8(text + pos, end - pos, &ulen);
        if (u != NULL) {
            sb_append_len(&out, u, ulen);
            free(u);
        } else {
            sb_append_len(&out, text + pos, end - pos);
        }
        pos = end;
    }
    free(text);
    if (out.data == NULL)
        sb_append(&out, "");
    *len = out.len;
    return out.data;
}
