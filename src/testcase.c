#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "testcase.h"

#define META_FILE "meta.txt"

/* 讀 meta.txt：test01|hidden|說明 */
static void load_meta(const char *dir, TestSet *set)
{
    char path[GRADER_PATH_MAX];
    size_t len = 0;
    char *text, *line;

    path_join(path, sizeof(path), dir, META_FILE);
    text = read_file(path, &len);
    if (text == NULL)
        return;
    text = text_to_utf8(text, &len);

    for (line = strtok(text, "\r\n"); line != NULL; line = strtok(NULL, "\r\n")) {
        char *bar1 = strchr(line, '|'), *bar2;
        int i;
        if (bar1 == NULL)
            continue;
        *bar1 = '\0';
        bar2 = strchr(bar1 + 1, '|');
        if (bar2 != NULL)
            *bar2 = '\0';
        for (i = 0; i < set->count; i++) {
            if (_stricmp(set->items[i].name, line) != 0)
                continue;
            set->items[i].hidden = _stricmp(bar1 + 1, "hidden") == 0;
            if (bar2 != NULL)
                snprintf(set->items[i].desc, sizeof(set->items[i].desc), "%s", bar2 + 1);
        }
    }
    free(text);
}

int testcase_scan(const char *data_dir, TestSet *set, char *err, size_t err_size)
{
    char dir[GRADER_PATH_MAX];
    StringList names = {0};
    int i;

    set->items = NULL;
    set->count = 0;

    path_join(dir, sizeof(dir), data_dir, "testcase");
    if (!dir_exists(dir)) {
        snprintf(err, err_size, "還沒有測資 (找不到 %s)", dir);
        return 0;
    }

    list_dir(dir, "*.in", 0, &names);
    set->items = calloc(names.count > 0 ? names.count : 1, sizeof(TestCase));
    if (set->items == NULL) {
        string_list_free(&names);
        snprintf(err, err_size, "記憶體不足");
        return 0;
    }

    for (i = 0; i < names.count; i++) {
        const char *file = names.items[i];
        size_t len = strlen(file);
        TestCase *tc;
        char out_name[300];

        /* Windows 的萬用字元比對有時不精確，再確認一次副檔名 */
        if (len <= 3 || _stricmp(file + len - 3, ".in") != 0)
            continue;

        tc = &set->items[set->count++];
        snprintf(tc->name, sizeof(tc->name), "%.*s", (int)(len - 3), file);
        path_join(tc->in_path, sizeof(tc->in_path), dir, file);
        snprintf(out_name, sizeof(out_name), "%s.out", tc->name);
        path_join(tc->out_path, sizeof(tc->out_path), dir, out_name);
        tc->has_out = file_exists(tc->out_path);
    }
    string_list_free(&names);

    if (set->count == 0) {
        free(set->items);
        set->items = NULL;
        snprintf(err, err_size, "還沒有任何測資 (%s 裡沒有 .in 檔)", dir);
        return 0;
    }
    load_meta(dir, set);
    return 1;
}

void testcase_free(TestSet *set)
{
    free(set->items);
    set->items = NULL;
    set->count = 0;
}

/* ---------------- 總表文字 ---------------- */

static void append_file_text(StrBuf *sb, const char *path)
{
    size_t len = 0;
    char *text = read_file(path, &len);
    char *p, *q;

    if (text == NULL)
        return;
    text = text_to_utf8(text, &len);
    /* 去掉 \r，確保最後有換行 */
    for (p = q = text; *p != '\0'; p++)
        if (*p != '\r')
            *q++ = *p;
    *q = '\0';
    sb_append(sb, text);
    if (q > text && q[-1] != '\n')
        sb_append(sb, "\n");
    free(text);
}

char *testcase_export_text(const char *data_dir)
{
    TestSet set;
    StrBuf sb = {0};
    char err[256];
    int i;

    if (!testcase_scan(data_dir, &set, err, sizeof(err))) {
        sb_append(&sb, "===== test01 | 可見 | 一般情況 =====\n"
                       "(把第 1 組輸入寫在這裡：學生程式從鍵盤讀到的內容)\n");
        return sb.data;
    }
    for (i = 0; i < set.count; i++) {
        const TestCase *tc = &set.items[i];
        sb_appendf(&sb, "===== %s | %s | %s =====\n", tc->name, tc->hidden ? "隱藏" : "可見", tc->desc);
        append_file_text(&sb, tc->in_path);
        if (tc->has_out) {
            sb_append(&sb, "----- 標準答案 -----\n");
            append_file_text(&sb, tc->out_path);
        }
    }
    testcase_free(&set);
    return sb.data;
}

typedef struct {
    int hidden;
    char desc[256];
    StrBuf input;
    StrBuf output;
    int has_output;
} Block;

/*
 * 標題行必須是「===== 」開頭 (5 個等號 + 空白) 且含有「 =====」結尾，
 * 這樣程式輸出裡常見的 "==========您輸入的資料如下==========" 分隔線不會被誤判成標題。
 */
static int is_header(const char *line)
{
    return strncmp(line, "===== ", 6) == 0 && strstr(line + 5, " =====") != NULL;
}

static int is_answer_line(const char *line)
{
    return strncmp(line, "----- ", 6) == 0 &&
           (strstr(line, "答案") != NULL || strstr(line, "expected") != NULL || strstr(line, "EXPECTED") != NULL);
}

static void trim_in_place(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '='))
        s[--n] = '\0';
    while (*s == ' ' || *s == '\t' || *s == '=')
        memmove(s, s + 1, strlen(s));
}

/* "===== test01 | 隱藏 | 說明 =====" -> hidden、desc (名稱不重要，匯入時會重新編號) */
static void parse_header(const char *line, Block *b)
{
    char buf[512], *parts[3] = {NULL, NULL, NULL}, *p;
    int n = 0;

    snprintf(buf, sizeof(buf), "%s", line);
    trim_in_place(buf);
    p = buf;
    parts[n++] = p;
    while (n < 3 && (p = strchr(p, '|')) != NULL) {
        *p++ = '\0';
        parts[n++] = p;
    }
    for (n = 0; n < 3; n++)
        if (parts[n] != NULL)
            trim_in_place(parts[n]);

    b->hidden = 0;
    b->desc[0] = '\0';
    if (parts[1] != NULL)
        b->hidden = strstr(parts[1], "隱藏") != NULL || _stricmp(parts[1], "hidden") == 0;
    if (parts[2] != NULL)
        snprintf(b->desc, sizeof(b->desc), "%s", parts[2]);
    /* 說明裡不能有 | 與換行 (meta.txt 的分隔字元) */
    for (p = b->desc; *p != '\0'; p++)
        if (*p == '|')
            *p = '/';
}

/* 去掉結尾的空白行；內容非空就確保以一個換行結尾 */
static void finish_text(StrBuf *sb)
{
    if (sb->data == NULL)
        return;
    while (sb->len > 0 && (sb->data[sb->len - 1] == '\n' || sb->data[sb->len - 1] == ' ' ||
                           sb->data[sb->len - 1] == '\t'))
        sb->data[--sb->len] = '\0';
    if (sb->len > 0)
        sb_append(sb, "\n");
}

int testcase_import_text(const char *data_dir, const char *text, char *err, size_t err_size)
{
    char dir[GRADER_PATH_MAX], path[GRADER_PATH_MAX + 64], name[32];
    Block *blocks = NULL;
    int count = 0, i, in_answer = 0;
    const char *line = text;
    StringList old = {0};
    StrBuf meta = {0};

    /* 1. 解析 */
    while (line != NULL && *line != '\0') {
        const char *end = strchr(line, '\n');
        size_t len = end != NULL ? (size_t)(end - line) : strlen(line);
        char *copy = malloc(len + 1);

        memcpy(copy, line, len);
        copy[len] = '\0';
        if (len > 0 && copy[len - 1] == '\r')
            copy[len - 1] = '\0';

        if (is_header(copy)) {
            Block *nb = realloc(blocks, (count + 1) * sizeof(Block));
            if (nb == NULL) {
                free(copy);
                break;
            }
            blocks = nb;
            memset(&blocks[count], 0, sizeof(Block));
            parse_header(copy, &blocks[count]);
            count++;
            in_answer = 0;
        } else if (count > 0 && is_answer_line(copy)) {
            in_answer = 1;
            blocks[count - 1].has_output = 1;
        } else if (count > 0) {
            StrBuf *target = in_answer ? &blocks[count - 1].output : &blocks[count - 1].input;
            sb_append(target, copy);
            sb_append(target, "\n");
        } else if (copy[0] != '\0') {
            snprintf(err, err_size, "第一組測資前面缺少標題行，例如：===== test01 | 可見 | 說明 =====");
            free(copy);
            for (i = 0; i < count; i++) {
                sb_free(&blocks[i].input);
                sb_free(&blocks[i].output);
            }
            free(blocks);
            return -1;
        }
        free(copy);
        line = end != NULL ? end + 1 : NULL;
    }

    if (count == 0) {
        snprintf(err, err_size, "沒有任何測資。每組測資以一行 ===== 名稱 | 可見 | 說明 ===== 開頭。");
        free(blocks);
        return -1;
    }

    /* 2. 刪掉舊檔，寫入新檔 */
    path_join(dir, sizeof(dir), data_dir, "testcase");
    make_dir(dir);
    list_dir(dir, "*.in", 0, &old);
    list_dir(dir, "*.out", 0, &old);
    for (i = 0; i < old.count; i++) {
        path_join(path, sizeof(path), dir, old.items[i]);
        delete_file(path);
    }
    string_list_free(&old);

    for (i = 0; i < count; i++) {
        Block *b = &blocks[i];
        snprintf(name, sizeof(name), "test%02d", i + 1);

        finish_text(&b->input);
        snprintf(path, sizeof(path), "%s\\%s.in", dir, name);
        write_file(path, b->input.data != NULL ? b->input.data : "", b->input.len);

        finish_text(&b->output);
        if (b->has_output && b->output.len > 0) {
            snprintf(path, sizeof(path), "%s\\%s.out", dir, name);
            write_file(path, b->output.data, b->output.len);
        }
        sb_appendf(&meta, "%s|%s|%s\n", name, b->hidden ? "hidden" : "visible", b->desc);

        sb_free(&b->input);
        sb_free(&b->output);
    }
    free(blocks);

    path_join(path, sizeof(path), dir, META_FILE);
    write_file(path, meta.data, meta.len);
    sb_free(&meta);
    return count;
}
