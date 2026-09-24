/*
 * feedback.c - 給學生看的白話說明 (說明見 feedback.h)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "comparator.h"
#include "feedback.h"

#define LINE_DIFF_CELL_LIMIT 4000000.0 /* 逐行比對的表格上限 (行數 x 行數) */
#define SHOW_CHARS 60                  /* 一行最多顯示幾個字 */

/* ---------------- 錯誤類型提示 ---------------- */

static long diff_with(const char *e, size_t el, const char *a, size_t al, const CompareOptions *o)
{
    int approx;
    return compare_outputs(e, el, a, al, o, &approx);
}

/* 數字 (連續的 0-9) 全部換成一個 0：用來判斷「格式對、數字錯」 */
static char *mask_digits(const char *s, size_t len, size_t *out_len)
{
    char *m = malloc(len + 1);
    size_t i, k = 0;

    if (m == NULL)
        return NULL;
    for (i = 0; i < len; i++) {
        if (s[i] >= '0' && s[i] <= '9') {
            if (i == 0 || !(s[i - 1] >= '0' && s[i - 1] <= '9')) /* 一串數字只留一個 0 */
                m[k++] = '0';
        } else {
            m[k++] = s[i];
        }
    }
    m[k] = '\0';
    *out_len = k;
    return m;
}

static int is_blank_text(const char *s, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++)
        if (s[i] != ' ' && s[i] != '\t' && s[i] != '\r' && s[i] != '\n')
            return 0;
    return 1;
}

/* 回傳 1 = 找到一個明確的錯誤類型 (之後就不必再猜別的類型) */
static int add_hints(const char *e, size_t el, const char *a, size_t al, StrBuf *out)
{
    CompareOptions o;
    char *me, *ma;
    size_t mel = 0, mal = 0;
    int found = 0;

    if (is_blank_text(a, al)) {
        sb_append(out, "程式沒有印出任何東西 (是否忘了 printf、或讀取輸入的地方卡住了？)\n");
        return 1;
    }

    compare_options_default(&o);
    o.ignore_trailing_space = 1;
    if (diff_with(e, el, a, al, &o) == 0) {
        sb_append(out, "文字都對，只差在行尾的空白、或輸出最後多了 / 少了換行\n");
        return 1;
    }
    o.fullwidth_as_halfwidth = 1;
    if (diff_with(e, el, a, al, &o) == 0) {
        sb_append(out, "用了全形符號 (例如 ：、，、１、（ )，程式輸出要用半形\n");
        return 1;
    }
    o.fullwidth_as_halfwidth = 0;
    o.ignore_case = 1;
    if (diff_with(e, el, a, al, &o) == 0) {
        sb_append(out, "英文大小寫不同\n");
        return 1;
    }
    compare_options_default(&o);
    o.mode = COMPARE_IGNORE_WHITESPACE;
    if (diff_with(e, el, a, al, &o) == 0) {
        sb_append(out, "文字內容都對，但空白或換行的數量、位置不同\n");
        return 1;
    }

    me = mask_digits(e, el, &mel);
    ma = mask_digits(a, al, &mal);
    if (me != NULL && ma != NULL) {
        compare_options_default(&o);
        o.ignore_trailing_space = 1;
        if (diff_with(me, mel, ma, mal, &o) == 0) {
            sb_append(out, "輸出格式正確，但數字不對：請檢查計算過程 (整數除法、變數型態、四捨五入、%.2f 的小數位數)\n");
            found = 1;
        }
    }
    free(me);
    free(ma);
    return found;
}

/* ---------------- 逐行比對 ---------------- */

typedef struct {
    const char *p;
    size_t len;
} Line;

/* 切行：\r\n 視為 \n；結尾的換行不會多出一個空行 */
static Line *split_lines(const char *s, size_t len, size_t *count)
{
    size_t i, start = 0, n = 0, cap = 16;
    Line *lines = malloc(cap * sizeof(Line));

    if (lines == NULL) {
        *count = 0;
        return NULL;
    }
    if (len >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
        start = 3;
    for (i = start; i <= len; i++) {
        if (i == len || s[i] == '\n') {
            size_t end = i;
            if (i == len && start == len)
                break;
            if (end > start && s[end - 1] == '\r')
                end--;
            if (n == cap) {
                Line *p = realloc(lines, cap * 2 * sizeof(Line));
                if (p == NULL)
                    break;
                lines = p;
                cap *= 2;
            }
            lines[n].p = s + start;
            lines[n].len = end - start;
            n++;
            start = i + 1;
        }
    }
    *count = n;
    return lines;
}

static int same_line(const Line *a, const Line *b)
{
    return a->len == b->len && memcmp(a->p, b->p, a->len) == 0;
}

/* 一行文字 (最多 SHOW_CHARS 個字，UTF-8 不切半個字)；空白行顯示成 (空白行) */
static void append_line(StrBuf *out, const Line *l)
{
    size_t i = 0, chars = 0;

    if (l->len == 0) {
        sb_append(out, "(空白行)");
        return;
    }
    sb_append(out, "「");
    while (i < l->len && chars < SHOW_CHARS) {
        size_t step = 1;
        unsigned char c = (unsigned char)l->p[i];
        if (c >= 0xF0) step = 4;
        else if (c >= 0xE0) step = 3;
        else if (c >= 0xC0) step = 2;
        if (i + step > l->len)
            step = l->len - i;
        sb_append_len(out, l->p + i, step);
        i += step;
        chars++;
    }
    if (i < l->len)
        sb_append(out, "…");
    sb_append(out, "」");
}

/* 兩行從第幾個字開始不同 (以字元計，從 1 開始) */
static long first_diff_column(const Line *e, const Line *a)
{
    long line, col;
    if (!compare_first_difference(e->p, e->len, a->p, a->len, &line, &col))
        return 0;
    return col;
}

typedef struct {
    int op; /* 0 相同、-1 只在標準答案、+1 只在學生輸出 */
    size_t ei, ai; /* 行號 (從 0 開始) */
} LineOp;

static void list_line_differences(const char *e, size_t el, const char *a, size_t al, StrBuf *out)
{
    size_t ne = 0, na = 0, i, j, count = 0, shown = 0, total = 0;
    Line *le = split_lines(e, el, &ne), *la = split_lines(a, al, &na);
    int *d = NULL;
    LineOp *ops = NULL;

    if (le == NULL || la == NULL || (double)(ne + 1) * (double)(na + 1) > LINE_DIFF_CELL_LIMIT)
        goto done;
    d = malloc((ne + 1) * (na + 1) * sizeof(int));
    ops = malloc((ne + na + 1) * sizeof(LineOp));
    if (d == NULL || ops == NULL)
        goto done;

    /* 以「行」為單位的編輯距離，回溯出哪些行相同、哪些行只在一邊 */
#define D(x, y) d[(x) * (na + 1) + (y)]
    for (i = 0; i <= ne; i++)
        D(i, 0) = (int)i;
    for (j = 0; j <= na; j++)
        D(0, j) = (int)j;
    for (i = 1; i <= ne; i++)
        for (j = 1; j <= na; j++) {
            int best = D(i - 1, j - 1) + (same_line(&le[i - 1], &la[j - 1]) ? 0 : 2); /* 換一行 = 刪 + 插 */
            if (D(i - 1, j) + 1 < best)
                best = D(i - 1, j) + 1;
            if (D(i, j - 1) + 1 < best)
                best = D(i, j - 1) + 1;
            D(i, j) = best;
        }
    i = ne;
    j = na;
    while (i > 0 || j > 0) {
        if (i > 0 && j > 0 && same_line(&le[i - 1], &la[j - 1]) && D(i, j) == D(i - 1, j - 1)) {
            i--, j--;
            ops[count].op = 0;
        } else if (i > 0 && (j == 0 || D(i, j) == D(i - 1, j) + 1)) {
            i--;
            ops[count].op = -1;
        } else {
            j--;
            ops[count].op = 1;
        }
        ops[count].ei = i;
        ops[count].ai = j;
        count++;
    }
#undef D

    /* 正向走過；一段連續的「少了」與「多了」配對成「這一行寫錯」 */
    i = count;
    while (i > 0) {
        size_t k, dels = 0, inss = 0, del_at[64], ins_at[64], pairs;
        if (ops[i - 1].op == 0) {
            i--;
            continue;
        }
        while (i > 0 && ops[i - 1].op != 0) {
            i--;
            if (ops[i].op < 0 && dels < 64)
                del_at[dels++] = ops[i].ei;
            else if (ops[i].op > 0 && inss < 64)
                ins_at[inss++] = ops[i].ai;
        }
        pairs = dels < inss ? dels : inss;
        for (k = 0; k < pairs; k++, total++) {
            if (shown < FEEDBACK_MAX_ITEMS) {
                long col = first_diff_column(&le[del_at[k]], &la[ins_at[k]]);
                sb_appendf(out, "第 %zu 行：應該是", del_at[k] + 1);
                append_line(out, &le[del_at[k]]);
                sb_append(out, "，你的輸出是");
                append_line(out, &la[ins_at[k]]);
                if (col > 0)
                    sb_appendf(out, " (從第 %ld 個字開始不同)", col);
                sb_append(out, "\n");
                shown++;
            }
        }
        for (k = pairs; k < dels; k++, total++) {
            if (shown < FEEDBACK_MAX_ITEMS) {
                sb_appendf(out, "少了第 %zu 行：", del_at[k] + 1);
                append_line(out, &le[del_at[k]]);
                sb_append(out, "\n");
                shown++;
            }
        }
        for (k = pairs; k < inss; k++, total++) {
            if (shown < FEEDBACK_MAX_ITEMS) {
                sb_appendf(out, "你的第 %zu 行是多出來的：", ins_at[k] + 1);
                append_line(out, &la[ins_at[k]]);
                sb_append(out, "\n");
                shown++;
            }
        }
    }
    if (total > shown)
        sb_appendf(out, "…還有 %zu 處不同，完整差異請看下方的比對\n", total - shown);
    if (ne != na)
        sb_appendf(out, "輸出行數不同：標準答案 %zu 行，你的輸出 %zu 行\n", ne, na);

done:
    free(le);
    free(la);
    free(d);
    free(ops);
}

void feedback_output(const char *expected, size_t expected_len, const char *actual, size_t actual_len, int hidden,
                     StrBuf *out)
{
    CompareOptions o;
    int approx, typed;

    if (expected == NULL)
        expected = "";
    if (actual == NULL)
        actual = "";
    compare_options_default(&o);
    if (compare_outputs(expected, expected_len, actual, actual_len, &o, &approx) == 0)
        return;

    typed = add_hints(expected, expected_len, actual, actual_len, out);
    if (hidden) {
        if (!typed)
            sb_append(out, "輸出內容與標準答案不同 (隱藏測資不公開內容)\n");
        return;
    }
    if (!is_blank_text(actual, actual_len))
        list_line_differences(expected, expected_len, actual, actual_len, out);
}

/* ---------------- gcc 錯誤訊息翻成白話 ---------------- */

/* 取出訊息裡第一個被引號包住的名稱 (gcc 用 ‘x’ 或 'x') */
static void quoted_name(const char *msg, char *out, size_t size)
{
    const char *open = strstr(msg, "\xE2\x80\x98"), *close; /* ‘ */
    size_t skip = 3;

    out[0] = '\0';
    if (open == NULL) {
        open = strchr(msg, '\'');
        skip = 1;
    }
    if (open == NULL)
        return;
    open += skip;
    close = skip == 3 ? strstr(open, "\xE2\x80\x99") : strchr(open, '\''); /* ’ */
    if (close == NULL || close == open)
        return;
    snprintf(out, size, "%.*s", (int)(close - open), open);
}

static void explain(const char *msg, StrBuf *out)
{
    char name[128];

    quoted_name(msg, name, sizeof(name));
    if (strstr(msg, "stray") != NULL)
        sb_append(out, "程式裡有不合法的字元：常見原因是全形空白、全形引號「」、中文標點，請改成半形");
    else if (strstr(msg, "expected") != NULL &&
             (strstr(msg, "';'") != NULL || strstr(msg, "\xE2\x80\x98;\xE2\x80\x99") != NULL)) /* 含 "expected ',' or ';'" */
        sb_append(out, "少了分號 ; (通常在這一行或上一行的結尾)");
    else if (strstr(msg, "implicit declaration of function") != NULL)
        sb_appendf(out, "函式 %s 沒有宣告：名稱可能拼錯 (例如 prinft)，或少了 #include", name[0] ? name : "");
    else if (strstr(msg, "undeclared") != NULL)
        sb_appendf(out, "%s 沒有宣告：變數名稱可能拼錯、大小寫不同，或忘了宣告", name[0] ? name : "名稱");
    else if (strstr(msg, "missing terminating") != NULL)
        sb_append(out, "字串或字元少了結尾的引號 \" 或 '");
    else if (strstr(msg, "No such file or directory") != NULL)
        sb_appendf(out, "找不到 #include 的標頭檔 %s：檔名是否拼錯？", name);
    else if (strstr(msg, "expected declaration or statement at end of input") != NULL ||
             strstr(msg, "expected '}'") != NULL || strstr(msg, "expected \xE2\x80\x98}\xE2\x80\x99") != NULL)
        sb_append(out, "大括號 { } 沒有成對，少了 }");
    else if (strstr(msg, "expected ')'") != NULL || strstr(msg, "expected \xE2\x80\x98)\xE2\x80\x99") != NULL)
        sb_append(out, "小括號 ( ) 沒有成對，少了 )");
    else if (strstr(msg, "expected ']'") != NULL || strstr(msg, "expected \xE2\x80\x98]\xE2\x80\x99") != NULL)
        sb_append(out, "中括號 [ ] 沒有成對，少了 ]");
    else if (strstr(msg, "redefinition") != NULL || strstr(msg, "conflicting types") != NULL ||
             strstr(msg, "redeclaration") != NULL)
        sb_appendf(out, "%s 重複定義了 (同一個名稱宣告了兩次)", name[0] ? name : "名稱");
    else if (strstr(msg, "too few arguments") != NULL)
        sb_append(out, "呼叫函式時參數太少");
    else if (strstr(msg, "too many arguments") != NULL)
        sb_append(out, "呼叫函式時參數太多");
    else if (strstr(msg, "lvalue required") != NULL)
        sb_append(out, "等號左邊必須是變數 (是否把 == 寫成 = 或寫反了？)");
    else if (strstr(msg, "incompatible") != NULL)
        sb_append(out, "型別不相容：例如把字串指定給整數、或指標與數值混用");
    else if (strstr(msg, "expected expression") != NULL)
        sb_append(out, "這裡應該要有運算式或數值 (是否多打或少打了符號？)");
    else if (strstr(msg, "expected") != NULL)
        sb_appendf(out, "語法錯誤 (%s)", msg);
    else
        sb_appendf(out, "%s", msg);
}

void feedback_compile_errors(const char *log, StrBuf *out)
{
    const char *line = log;
    int shown = 0, total = 0;

    if (log == NULL)
        return;
    while (*line != '\0') {
        const char *end = strchr(line, '\n');
        size_t len = end != NULL ? (size_t)(end - line) : strlen(line);
        char buf[1024], *mark;

        snprintf(buf, sizeof(buf), "%.*s", (int)(len < sizeof(buf) - 1 ? len : sizeof(buf) - 1), line);
        if (len > 0 && buf[strlen(buf) - 1] == '\r')
            buf[strlen(buf) - 1] = '\0';
        mark = strstr(buf, ": error: ");
        if (mark == NULL)
            mark = strstr(buf, ": fatal error: ");
        if (mark != NULL) {
            const char *msg = strstr(mark, "error: ") + 7;
            long line_no = 0;
            char *p = mark, *q;
            /* 往前找 ":行:欄" (檔名可能含 C:\ 的冒號，所以從後面找) */
            *p = '\0';
            q = strrchr(buf, ':');
            if (q != NULL) {
                *q = '\0';
                q = strrchr(buf, ':');
                if (q != NULL)
                    line_no = strtol(q + 1, NULL, 10);
            }
            total++;
            if (shown < FEEDBACK_MAX_ITEMS) {
                if (line_no > 0)
                    sb_appendf(out, "第 %ld 行：", line_no);
                explain(msg, out);
                sb_append(out, "\n");
                shown++;
            }
        }
        if (end == NULL)
            break;
        line = end + 1;
    }
    if (total > shown)
        sb_appendf(out, "…還有 %d 個編譯錯誤 (常常是第一個錯誤連帶造成的，先修第一個)\n", total - shown);
}
