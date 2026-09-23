#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "comparator.h"

/* 完整動態規劃要算 n x m 格。超過這個數量就改用帶狀演算法。 */
#define EDIT_DISTANCE_CELL_LIMIT 50000000.0
/* 帶狀演算法的總工作量預算 (格)；超過才退回近似值 */
#define EDIT_DISTANCE_BAND_BUDGET 400000000.0
#define EDIT_DISTANCE_FIRST_BAND 64
#define MAX_KEYWORDS 16

void compare_options_default(CompareOptions *options)
{
    memset(options, 0, sizeof(*options));
    options->mode = COMPARE_STRICT;
}

const char *compare_mode_name(CompareMode mode)
{
    switch (mode) {
    case COMPARE_STRICT:            return "strict";
    case COMPARE_IGNORE_WHITESPACE: return "ignore_whitespace";
    case COMPARE_TOKEN:             return "token";
    }
    return "strict";
}

int compare_mode_from_name(const char *name, CompareMode *mode)
{
    if (strcmp(name, "strict") == 0)
        *mode = COMPARE_STRICT;
    else if (strcmp(name, "ignore_whitespace") == 0)
        *mode = COMPARE_IGNORE_WHITESPACE;
    else if (strcmp(name, "token") == 0)
        *mode = COMPARE_TOKEN;
    else
        return 0;
    return 1;
}

static int is_space_char(uint32_t c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f' ||
           c == 0x00A0 || c == 0x3000; /* 不斷行空白、全形空白 */
}

size_t compare_decode_utf8(const unsigned char *s, size_t n, uint32_t *out)
{
    size_t i = 0, count = 0;

    if (n >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF)
        i = 3;

    while (i < n) {
        unsigned char c = s[i];
        uint32_t cp;
        int extra, j, valid = 1;

        if (c == '\r' && i + 1 < n && s[i + 1] == '\n') {
            i++;
            continue;
        }

        if (c < 0x80)                { cp = c;        extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else                         { cp = 0;        extra = 0; valid = 0; }

        for (j = 1; valid && j <= extra; j++) {
            if (i + j >= n || (s[i + j] & 0xC0) != 0x80)
                valid = 0;
            else
                cp = (cp << 6) | (s[i + j] & 0x3F);
        }

        if (valid) {
            out[count++] = cp;
            i += 1 + extra;
        } else {
            out[count++] = COMPARE_INVALID_BYTE + c;
            i++;
        }
    }
    return count;
}

/* 步驟 2：全形轉半形、忽略大小寫 */
static uint32_t fold_char(uint32_t c, const CompareOptions *o)
{
    if (o->fullwidth_as_halfwidth) {
        if (c >= 0xFF01 && c <= 0xFF5E) /* ！～ -> !~ */
            c -= 0xFEE0;
        else if (c == 0x3000)           /* 全形空白 */
            c = ' ';
    }
    if (o->ignore_case && c >= 'A' && c <= 'Z')
        c += 'a' - 'A';
    return c;
}

typedef struct {
    uint32_t ch[IGNORE_LINES_MAX];
    size_t len;
} Keyword;

/* 把 "請輸入|請依序" 拆成關鍵字 (關鍵字也做同樣的全形/大小寫處理) */
static int parse_keywords(const CompareOptions *o, Keyword *keywords)
{
    const char *p = o->ignore_lines_with;
    int count = 0;

    while (*p != '\0' && count < MAX_KEYWORDS) {
        const char *end = strchr(p, '|');
        size_t len = end != NULL ? (size_t)(end - p) : strlen(p);
        Keyword *k = &keywords[count];
        size_t i;

        k->len = compare_decode_utf8((const unsigned char *)p, len, k->ch);
        for (i = 0; i < k->len; i++)
            k->ch[i] = fold_char(k->ch[i], o);
        /* 去掉關鍵字前後的空白 */
        while (k->len > 0 && k->ch[k->len - 1] == ' ')
            k->len--;
        i = 0;
        while (i < k->len && k->ch[i] == ' ')
            i++;
        if (i > 0) {
            memmove(k->ch, k->ch + i, (k->len - i) * sizeof(uint32_t));
            k->len -= i;
        }
        if (k->len > 0)
            count++;

        if (end == NULL)
            break;
        p = end + 1;
    }
    return count;
}

static int line_contains(const uint32_t *line, size_t n, const Keyword *k)
{
    size_t i;
    if (k->len > n)
        return 0;
    for (i = 0; i + k->len <= n; i++)
        if (memcmp(line + i, k->ch, k->len * sizeof(uint32_t)) == 0)
            return 1;
    return 0;
}

/* 步驟 3：逐行處理，直接在原陣列上修改，回傳新長度 */
static size_t filter_lines(uint32_t *s, size_t n, const CompareOptions *o, const Keyword *keywords, int keyword_count)
{
    size_t out = 0, start = 0;

    if (!o->ignore_trailing_space && !o->ignore_blank_lines && keyword_count == 0)
        return n;

    while (start < n) {
        size_t end = start, e, i;
        int has_newline, skip = 0, k;

        while (end < n && s[end] != '\n')
            end++;
        has_newline = end < n;

        e = end;
        if (o->ignore_trailing_space) /* 行尾所有種類的空白 (含 Tab、全形空白、不斷行空白) 都不計 */
            while (e > start && is_space_char(s[e - 1]))
                e--;

        if (o->ignore_blank_lines) {
            skip = 1;
            for (i = start; i < e; i++)
                if (!is_space_char(s[i])) {
                    skip = 0;
                    break;
                }
        }
        for (k = 0; !skip && k < keyword_count; k++)
            if (line_contains(s + start, e - start, &keywords[k]))
                skip = 1;

        if (!skip) {
            memmove(s + out, s + start, (e - start) * sizeof(uint32_t));
            out += e - start;
            if (has_newline)
                s[out++] = '\n';
        }
        start = end + 1;
    }

    /* 輸出最後多或少的換行、空白都不計 */
    if (o->ignore_trailing_space)
        while (out > 0 && is_space_char(s[out - 1]))
            out--;
    return out;
}

/* 步驟 4：比對模式 */
static size_t apply_mode(uint32_t *s, size_t n, CompareMode mode)
{
    size_t i, k = 0;
    int pending_space = 0;

    switch (mode) {
    case COMPARE_STRICT:
        return n;

    case COMPARE_IGNORE_WHITESPACE:
        for (i = 0; i < n; i++)
            if (!is_space_char(s[i]))
                s[k++] = s[i];
        return k;

    case COMPARE_TOKEN:
        for (i = 0; i < n; i++) {
            if (is_space_char(s[i])) {
                if (k > 0)
                    pending_space = 1;
            } else {
                if (pending_space)
                    s[k++] = ' ';
                pending_space = 0;
                s[k++] = s[i];
            }
        }
        return k;
    }
    return n;
}

static size_t normalize(uint32_t *s, size_t n, const CompareOptions *o, const Keyword *keywords, int keyword_count)
{
    size_t i;

    if (o->fullwidth_as_halfwidth || o->ignore_case)
        for (i = 0; i < n; i++)
            s[i] = fold_char(s[i], o);
    n = filter_lines(s, n, o, keywords, keyword_count);
    return apply_mode(s, n, o->mode);
}

static long min3(long a, long b, long c)
{
    long m = a < b ? a : b;
    return m < c ? m : c;
}

/* 完整 Levenshtein 動態規劃，只用兩列記憶體 (呼叫前保證 m <= n)。回傳 -1 = 記憶體不足 */
static long full_distance(const uint32_t *a, size_t n, const uint32_t *b, size_t m)
{
    long *row = malloc((m + 1) * sizeof(long));
    size_t i, j;
    long result;

    if (row == NULL)
        return -1;
    for (j = 0; j <= m; j++)
        row[j] = (long)j;

    for (i = 1; i <= n; i++) {
        long diag = row[0]; /* 左上角 */
        row[0] = (long)i;
        for (j = 1; j <= m; j++) {
            long up = row[j];
            long cost = a[i - 1] == b[j - 1] ? 0 : 1;
            row[j] = min3(up + 1, row[j - 1] + 1, diag + cost);
            diag = up;
        }
    }
    result = row[m];
    free(row);
    return result;
}

/*
 * 帶狀編輯距離 (Ukkonen)：只算 |i - j| <= band 的格子，成本 O(n * band)。
 * 任何成本 <= band 的編輯路徑一定落在帶內，所以真正的距離 <= band 時回傳的就是精確值；
 * 距離 > band 回傳 -1；記憶體不足回傳 -2。
 *
 * 第 i 列只存 j 在 [i - band, i + band] 的格子，索引 k = j - i + band：
 *   D(i-1, j-1) = prev[k]、D(i-1, j) = prev[k + 1]、D(i, j-1) = cur[k - 1]
 */
static long banded_distance(const uint32_t *a, size_t n, const uint32_t *b, size_t m, long band)
{
    const long INF = LONG_MAX / 4;
    size_t width = 2 * (size_t)band + 1, i, k;
    long *prev, *cur, result = -1;
    long len_diff = (long)n - (long)m;

    if (len_diff > band || -len_diff > band)
        return -1;
    prev = malloc(width * sizeof(long));
    cur = malloc(width * sizeof(long));
    if (prev == NULL || cur == NULL) {
        free(prev);
        free(cur);
        return -2;
    }

    for (k = 0; k < width; k++) { /* 第 0 列：D(0, j) = j */
        long j = (long)k - band;
        prev[k] = j >= 0 && (size_t)j <= m ? j : INF;
    }
    for (i = 1; i <= n; i++) {
        long *t;
        for (k = 0; k < width; k++) {
            long j = (long)i + (long)k - band, best;
            if (j < 0 || (size_t)j > m) {
                cur[k] = INF;
                continue;
            }
            if (j == 0) {
                cur[k] = (long)i;
                continue;
            }
            best = prev[k] + (a[i - 1] == b[j - 1] ? 0 : 1);
            if (k + 1 < width && prev[k + 1] + 1 < best)
                best = prev[k + 1] + 1;
            if (k > 0 && cur[k - 1] + 1 < best)
                best = cur[k - 1] + 1;
            cur[k] = best;
        }
        t = prev;
        prev = cur;
        cur = t;
    }
    k = (size_t)((long)m - (long)n + band); /* D(n, m) */
    if (k < width && prev[k] <= band)
        result = prev[k];
    free(prev);
    free(cur);
    return result;
}

/*
 * Myers (1999) 位元平行 Levenshtein，Hyyrö 的多字組 (block) 版本 (與 edlib 相同的公式)：
 * 把較短的一邊 (pattern，長 m) 切成每 64 列一個 64 位元字組，對較長的一邊 (text) 每個字元
 * 只做幾個位元運算就更新一整個字組，成本 O(n × ⌈m/64⌉)，結果與完整動態規劃完全相同。
 *   Pv / Mv：垂直差分 (+1 / −1) 的位元向量；Ph / Mh：水平差分；Eq：pattern 各列是否等於目前字元。
 * 回傳距離；記憶體不足或超出預算回傳 -1 (改走其他方法)。
 */
#define MYERS_WORD 64
#define MYERS_MAX_PATTERN (1u << 21)           /* pattern 超過 200 萬字就不用 (雜湊表太大) */
#define MYERS_PEQ_MEMORY (128.0 * 1024 * 1024) /* Peq 表上限 */
#define MYERS_WORK_BUDGET 2.0e9                /* n × 字組數 的上限 (約 2～4 秒) */

typedef struct {
    uint32_t key;
    int32_t idx; /* -1 = 空 */
} PeqSlot;

/* 字元 -> Peq 列編號；insert = 0 時找不到回傳 -1 */
static int peq_index(PeqSlot *table, size_t mask, uint32_t c, int insert, int *count)
{
    size_t h = (size_t)(c * 2654435761u) & mask;
    for (;;) {
        if (table[h].idx < 0) {
            if (!insert)
                return -1;
            table[h].key = c;
            table[h].idx = (*count)++;
            return table[h].idx;
        }
        if (table[h].key == c)
            return table[h].idx;
        h = (h + 1) & mask;
    }
}

static long myers_distance(const uint32_t *text, size_t n, const uint32_t *pat, size_t m)
{
    size_t blocks = (m + MYERS_WORD - 1) / MYERS_WORD, table_size = 16, mask, i, j, b;
    PeqSlot *table;
    uint64_t *peq, *pv, *mv, last_bit;
    int distinct = 0;
    long score = (long)m;

    if (m == 0 || m > MYERS_MAX_PATTERN || (double)n * (double)blocks > MYERS_WORK_BUDGET)
        return -1;
    while (table_size < 2 * m + 2)
        table_size <<= 1;
    mask = table_size - 1;
    table = malloc(table_size * sizeof(PeqSlot));
    if (table == NULL)
        return -1;
    for (i = 0; i < table_size; i++)
        table[i].idx = -1;
    for (i = 0; i < m; i++)
        peq_index(table, mask, pat[i], 1, &distinct);
    if ((double)distinct * (double)blocks * sizeof(uint64_t) > MYERS_PEQ_MEMORY) {
        free(table);
        return -1;
    }
    peq = calloc((size_t)distinct * blocks, sizeof(uint64_t));
    pv = malloc(blocks * sizeof(uint64_t));
    mv = calloc(blocks, sizeof(uint64_t));
    if (peq == NULL || pv == NULL || mv == NULL) {
        free(table); free(peq); free(pv); free(mv);
        return -1;
    }
    for (i = 0; i < m; i++)
        peq[(size_t)peq_index(table, mask, pat[i], 0, &distinct) * blocks + i / MYERS_WORD] |=
            (uint64_t)1 << (i % MYERS_WORD);
    for (b = 0; b < blocks; b++)
        pv[b] = ~(uint64_t)0;
    last_bit = (uint64_t)1 << ((m - 1) % MYERS_WORD); /* pattern 最後一列 (最後一個字組裡多出來的列不影響它) */

    for (j = 0; j < n; j++) {
        int idx = peq_index(table, mask, text[j], 0, &distinct);
        const uint64_t *row = idx >= 0 ? peq + (size_t)idx * blocks : NULL;
        int hin = 1; /* 第 0 列的水平差分是 +1 (D[0][j] = j) */
        for (b = 0; b < blocks; b++) {
            uint64_t eq = row != NULL ? row[b] : 0, xv, xh, ph, mh;
            uint64_t hin_neg = hin < 0 ? 1 : 0;
            int hout;
            xv = eq | mv[b];
            eq |= hin_neg;
            xh = (((eq & pv[b]) + pv[b]) ^ pv[b]) | eq;
            ph = mv[b] | ~(xh | pv[b]);
            mh = pv[b] & xh;
            if (b == blocks - 1) {
                if (ph & last_bit)
                    score++;
                else if (mh & last_bit)
                    score--;
            }
            hout = (int)(ph >> (MYERS_WORD - 1)) - (int)(mh >> (MYERS_WORD - 1));
            ph <<= 1;
            mh <<= 1;
            if (hin_neg)
                mh |= 1;
            else if (hin > 0)
                ph |= 1;
            pv[b] = mh | ~(xv | ph);
            mv[b] = ph & xv;
            hin = hout;
        }
    }
    free(table); free(peq); free(pv); free(mv);
    return score;
}

static long edit_distance(const uint32_t *a, size_t n, const uint32_t *b, size_t m, int *approximate)
{
    size_t i, shorter, longer;
    long diff, band;
    double work = 0;

    /* 相同的開頭和結尾不影響距離，先去掉 (大部分情況會快很多) */
    while (n > 0 && m > 0 && a[0] == b[0]) {
        a++; b++; n--; m--;
    }
    while (n > 0 && m > 0 && a[n - 1] == b[m - 1]) {
        n--; m--;
    }
    if (n == 0)
        return (long)m;
    if (m == 0)
        return (long)n;

    /* 讓 b 是比較短的那一個，減少記憶體 */
    if (m > n) {
        const uint32_t *t = a; size_t tn = n;
        a = b; n = m;
        b = t; m = tn;
    }
    shorter = m;
    longer = n;

    /* 很短的就用最簡單的動態規劃；其他先用 Myers 位元平行 (精確、快幾十倍) */
    if ((double)n * (double)m <= 4096.0) {
        diff = full_distance(a, n, b, m);
        if (diff >= 0)
            return diff;
    }
    diff = myers_distance(a, n, b, m);
    if (diff >= 0)
        return diff;

    if ((double)n * (double)m <= EDIT_DISTANCE_CELL_LIMIT) {
        diff = full_distance(a, n, b, m);
        if (diff >= 0)
            return diff;
    } else {
        /* 帶寬加倍搜尋：差異不多的長輸出很快就能得到精確值 */
        for (band = EDIT_DISTANCE_FIRST_BAND;; band *= 2) {
            double cost = (double)(n + 1) * (2.0 * band + 1);
            if (work + cost > EDIT_DISTANCE_BAND_BUDGET)
                break;
            diff = banded_distance(a, n, b, m, band);
            work += cost;
            if (diff >= 0)
                return diff;
            if (diff == -2 || (size_t)band >= longer)
                break;
        }
    }

    /* 近似：同位置不同的字元數 + 長度差 (只在輸出極長且差異極多時才會走到這裡) */
    diff = (long)(longer - shorter);
    for (i = 0; i < shorter; i++)
        if (a[i] != b[i])
            diff++;
    *approximate = 1;
    return diff;
}

int compare_first_difference(const char *expected, size_t expected_len,
                             const char *actual, size_t actual_len, long *line, long *column)
{
    uint32_t *e = malloc((expected_len + 1) * sizeof(uint32_t));
    uint32_t *a = malloc((actual_len + 1) * sizeof(uint32_t));
    size_t en, an, i;
    int found = 0;

    *line = 1;
    *column = 1;
    if (e == NULL || a == NULL) {
        free(e);
        free(a);
        return 0;
    }
    en = compare_decode_utf8((const unsigned char *)expected, expected_len, e);
    an = compare_decode_utf8((const unsigned char *)actual, actual_len, a);

    for (i = 0; i < en || i < an; i++) {
        if (i >= en || i >= an || e[i] != a[i]) {
            found = 1;
            break;
        }
        if (e[i] == '\n') {
            (*line)++;
            *column = 1;
        } else {
            (*column)++;
        }
    }
    free(e);
    free(a);
    return found;
}

long compare_outputs(const char *expected, size_t expected_len,
                     const char *actual, size_t actual_len,
                     const CompareOptions *options, int *approximate)
{
    uint32_t *e = malloc((expected_len + 1) * sizeof(uint32_t));
    uint32_t *a = malloc((actual_len + 1) * sizeof(uint32_t));
    Keyword keywords[MAX_KEYWORDS];
    int keyword_count;
    size_t en, an;
    long diff;

    *approximate = 0;
    if (e == NULL || a == NULL) {
        free(e);
        free(a);
        *approximate = 1;
        return (long)(expected_len > actual_len ? expected_len : actual_len);
    }

    keyword_count = parse_keywords(options, keywords);
    en = normalize(e, compare_decode_utf8((const unsigned char *)expected, expected_len, e), options, keywords,
                   keyword_count);
    an = normalize(a, compare_decode_utf8((const unsigned char *)actual, actual_len, a), options, keywords,
                   keyword_count);
    diff = edit_distance(e, en, a, an, approximate);

    free(e);
    free(a);
    return diff;
}
