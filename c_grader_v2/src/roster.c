#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "roster.h"

/* 讀一個 CSV 欄位 (支援 "..." 引號)，回傳下一個欄位的開頭，行尾回傳 NULL */
static const char *read_field(const char *p, char *out, size_t size)
{
    size_t n = 0;

    if (*p == '"') {
        p++;
        while (*p != '\0') {
            if (*p == '"' && p[1] == '"') {
                if (n + 1 < size)
                    out[n++] = '"';
                p += 2;
            } else if (*p == '"') {
                p++;
                break;
            } else {
                if (n + 1 < size)
                    out[n++] = *p;
                p++;
            }
        }
    }
    while (*p != '\0' && *p != ',' && *p != '\r' && *p != '\n') {
        if (n + 1 < size)
            out[n++] = *p;
        p++;
    }
    /* 去掉前後空白 */
    out[n] = '\0';
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t'))
        out[--n] = '\0';
    while (out[0] == ' ' || out[0] == '\t')
        memmove(out, out + 1, strlen(out));
    return *p == ',' ? p + 1 : NULL;
}

int roster_load(const char *path, Roster *roster)
{
    size_t len = 0;
    char *text = read_file(path, &len);
    const char *line;
    int line_no = 0;

    roster->items = NULL;
    roster->count = 0;
    if (text == NULL)
        return 0;
    text = text_to_utf8(text, &len); /* Excel 另存的 Big5 CSV 也能讀 */

    line = text;
    if (len >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
        line += 3;

    while (*line != '\0') {
        const char *next_line = strchr(line, '\n');
        RosterEntry e;
        const char *p = line;

        memset(&e, 0, sizeof(e));
        line_no++;
        p = read_field(p, e.folder, sizeof(e.folder));
        if (p != NULL)
            p = read_field(p, e.name, sizeof(e.name));
        if (p != NULL)
            read_field(p, e.sid, sizeof(e.sid));

        /* 第一行是標題就略過 */
        if (e.folder[0] != '\0' && !(line_no == 1 && strcmp(e.folder, "資料夾名稱") == 0)) {
            RosterEntry *items = realloc(roster->items, (roster->count + 1) * sizeof(RosterEntry));
            if (items != NULL) {
                roster->items = items;
                roster->items[roster->count++] = e;
            }
        }
        if (next_line == NULL)
            break;
        line = next_line + 1;
    }
    free(text);
    return 1;
}

void roster_free(Roster *roster)
{
    free(roster->items);
    roster->items = NULL;
    roster->count = 0;
}

const RosterEntry *roster_find(const Roster *roster, const char *folder)
{
    int i;
    for (i = 0; i < roster->count; i++)
        if (_stricmp(roster->items[i].folder, folder) == 0)
            return &roster->items[i];
    return NULL;
}

void roster_guess_sid(const char *folder, char *out, size_t size)
{
    const char *p = folder;
    size_t n = 0, digits = 0;

    out[0] = '\0';
    /* 可以有一個英文字母開頭 (例如研究生 d11230735)，接著至少 6 位數字 */
    if (isalpha((unsigned char)*p) && isdigit((unsigned char)p[1]))
        p++;
    while (isdigit((unsigned char)p[digits]))
        digits++;
    if (digits < 6)
        return;
    n = (size_t)(p - folder) + digits;
    if (n >= size)
        n = size - 1;
    memcpy(out, folder, n);
    out[n] = '\0';
}

static void write_field(FILE *f, const char *s)
{
    if (strpbrk(s, ",\"\r\n") == NULL) {
        fputs(s, f);
        return;
    }
    fputc('"', f);
    for (; *s != '\0'; s++) {
        if (*s == '"')
            fputc('"', f);
        fputc(*s, f);
    }
    fputc('"', f);
}

int roster_sync(const char *path, const StringList *folders)
{
    Roster old;
    FILE *f;
    int i, added = 0;

    roster_load(path, &old);
    f = fopen_utf8(path, "wb");
    if (f == NULL) {
        roster_free(&old);
        return -1;
    }
    fputs("\xEF\xBB\xBF", f); /* UTF-8 BOM：Excel 直接開中文不會亂碼 */
    fputs("資料夾名稱,姓名,學號\r\n", f);

    for (i = 0; i < folders->count; i++) {
        const RosterEntry *e = roster_find(&old, folders->items[i]);
        char sid[64];

        write_field(f, folders->items[i]);
        fputc(',', f);
        if (e != NULL) {
            write_field(f, e->name);
            fputc(',', f);
            write_field(f, e->sid);
        } else {
            roster_guess_sid(folders->items[i], sid, sizeof(sid));
            fputc(',', f);
            write_field(f, sid);
            added++;
        }
        fputs("\r\n", f);
    }
    /* 名單裡有、但這次資料夾沒有的學生 (例如沒交作業) 也保留 */
    for (i = 0; i < old.count; i++) {
        int j, found = 0;
        for (j = 0; j < folders->count && !found; j++)
            found = _stricmp(old.items[i].folder, folders->items[j]) == 0;
        if (!found) {
            write_field(f, old.items[i].folder);
            fputc(',', f);
            write_field(f, old.items[i].name);
            fputc(',', f);
            write_field(f, old.items[i].sid);
            fputs("\r\n", f);
        }
    }
    fclose(f);
    roster_free(&old);
    return added;
}

void roster_display_name(const Roster *roster, const char *folder, char *out, size_t size)
{
    const RosterEntry *e = roster != NULL ? roster_find(roster, folder) : NULL;

    if (e != NULL && e->name[0] != '\0' && e->sid[0] != '\0')
        snprintf(out, size, "%s (%s)", e->name, e->sid);
    else if (e != NULL && e->name[0] != '\0')
        snprintf(out, size, "%s", e->name);
    else
        snprintf(out, size, "%s", folder);
}

void roster_default_path(char *out, size_t size)
{
    char folder[GRADER_PATH_MAX];

    app_folder(folder, sizeof(folder));
    path_join(out, size, folder, ROSTER_FILE_NAME);
}
