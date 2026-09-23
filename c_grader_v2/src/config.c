#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "util.h"

static const RangeRule spec_ranges[] = {
    {0, 0, 0},
    {1, 2, 1},
    {3, 5, 2},
    {6, 10, 5},
    {11, 20, 10},
    {21, RANGE_MAX, 20},
};

static void set_ranges(ScoreRules *rules, const RangeRule *ranges, int count)
{
    memcpy(rules->ranges, ranges, count * sizeof(RangeRule));
    rules->range_count = count;
}

void config_default(GradeConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->full_score = 100;
    compare_options_default(&cfg->compare);

    cfg->rules.mode = SCORE_RANGE;
    cfg->rules.per_char_penalty = 1;
    cfg->rules.n_chars = 3;
    cfg->rules.per_n_penalty = 1;
    set_ranges(&cfg->rules, spec_ranges, (int)(sizeof(spec_ranges) / sizeof(spec_ranges[0])));
    cfg->rules.require_contiguous = 0;

    cfg->timeout_ms = 60000; /* 預設每題 60 秒 */
    cfg->output_limit = 10LL * 1024 * 1024;
    cfg->memory_limit = 512LL * 1024 * 1024;
    cfg->runtime_error_zero = 0;

    snprintf(cfg->gcc_path, sizeof(cfg->gcc_path), "gcc");
    snprintf(cfg->compile_flags, sizeof(cfg->compile_flags), "-lm");
    cfg->keep_temp = 0;

    cfg->ai_fix = 1;
    snprintf(cfg->ai_fix_tool, sizeof(cfg->ai_fix_tool), "auto");
    cfg->ai_fix_penalty_per_char = 1;
    cfg->ai_fix_penalty_max = 0;
    cfg->ai_fix_attempts = 2;
    cfg->ai_fix_timeout_ms = 180000;
}

double config_fix_penalty(const GradeConfig *cfg, long chars)
{
    double p;

    if (chars <= 0)
        return 0;
    p = chars * cfg->ai_fix_penalty_per_char;
    if (cfg->ai_fix_penalty_max > 0 && p > cfg->ai_fix_penalty_max)
        p = cfg->ai_fix_penalty_max;
    return p;
}

/* ---------------- 評分模板 ---------------- */

typedef struct {
    const char *name;
    const char *description;
} TemplateInfo;

static const TemplateInfo templates[] = {
    {"規格預設：逐字比對 + 區間扣分",
     "Strict 逐字比較，不做任何寬容處理。\n"
     "區間扣分 (每題)：0→0、1～2→1、3～5→2、6～10→5、11～20→10、21+→20"},
    {"建議：忽略格式小差異 + 區間扣分",
     "Strict，但忽略行尾空白/結尾換行，全形半形視為相同 (：=:、１=1)。\n"
     "適合大部分作業：只因多一個換行或用了全形冒號不會被扣分。\n"
     "區間扣分 (每題)：0→0、1～2→1、3～5→2、6～10→5、11～20→10、21+→20"},
    {"只看資料：忽略提示文字行 + 區間扣分",
     "同「建議」，另外忽略含「請輸入」「請依序」的行 (學生提示文字寫法不同不扣分)。\n"
     "可在「忽略含以下文字的行」自行修改關鍵字。\n"
     "區間扣分 (每題)：0→0、1～2→1、3～5→2、6～10→5、11～20→10、21+→20"},
    {"寬鬆：Token 比對 + 每 3 CHAR 扣 1 分",
     "Token 比對 (空白數量、換行位置不同都不算錯)，忽略大小寫、全形半形、空白行。\n"
     "每 3 個 CHAR 差異扣 1 分 (每題最低 0 分)。"},
    {"精確：每錯 1 CHAR 扣 1 分",
     "Strict，忽略行尾空白/結尾換行。每 1 CHAR 差異扣 1 分 (每題最低 0 分)。"},
    {"全對才給分：有差異該題 0 分",
     "Strict，忽略行尾空白/結尾換行。完全相同才得該題分數，有任何差異該題 0 分。"},
};

int config_template_count(void)
{
    return (int)(sizeof(templates) / sizeof(templates[0]));
}

const char *config_template_name(int index)
{
    return index >= 0 && index < config_template_count() ? templates[index].name : "";
}

const char *config_template_description(int index)
{
    return index >= 0 && index < config_template_count() ? templates[index].description : "";
}

void config_apply_template(GradeConfig *cfg, int index)
{
    static const RangeRule all_or_nothing[] = {
        {0, 0, 0},
        {1, RANGE_MAX, SCORE_PENALTY_ALL},
    };
    CompareOptions *c = &cfg->compare;
    ScoreRules *r = &cfg->rules;

    compare_options_default(c);
    r->require_contiguous = 0;

    switch (index) {
    case 0: /* 規格預設 */
        r->mode = SCORE_RANGE;
        set_ranges(r, spec_ranges, 6);
        break;
    case 1: /* 建議 */
        c->ignore_trailing_space = 1;
        c->fullwidth_as_halfwidth = 1;
        r->mode = SCORE_RANGE;
        set_ranges(r, spec_ranges, 6);
        break;
    case 2: /* 只看資料 */
        c->ignore_trailing_space = 1;
        c->fullwidth_as_halfwidth = 1;
        snprintf(c->ignore_lines_with, sizeof(c->ignore_lines_with), "請輸入|請依序");
        r->mode = SCORE_RANGE;
        set_ranges(r, spec_ranges, 6);
        break;
    case 3: /* 寬鬆 */
        c->mode = COMPARE_TOKEN;
        c->ignore_trailing_space = 1;
        c->fullwidth_as_halfwidth = 1;
        c->ignore_case = 1;
        c->ignore_blank_lines = 1;
        r->mode = SCORE_PER_N_CHAR;
        r->n_chars = 3;
        r->per_n_penalty = 1;
        break;
    case 4: /* 精確 */
        c->ignore_trailing_space = 1;
        r->mode = SCORE_PER_CHAR;
        r->per_char_penalty = 1;
        break;
    case 5: /* 全對才給分 */
        c->ignore_trailing_space = 1;
        r->mode = SCORE_RANGE;
        set_ranges(r, all_or_nothing, 2);
        break;
    }
}

/* ---------------- 讀寫設定檔 ---------------- */

void format_penalty(char *out, size_t size, double penalty)
{
    if (penalty >= SCORE_PENALTY_ALL)
        snprintf(out, size, "ALL");
    else
        snprintf(out, size, "%g", penalty);
}

int parse_penalty(const char *text, double *penalty)
{
    char *end;

    while (isspace((unsigned char)*text))
        text++;
    if (_stricmp(text, "ALL") == 0 || strcmp(text, "扣光") == 0) {
        *penalty = SCORE_PENALTY_ALL;
        return 1;
    }
    *penalty = strtod(text, &end);
    while (isspace((unsigned char)*end))
        end++;
    return end != text && *end == '\0';
}

static char *trim(char *s)
{
    char *end;
    while (isspace((unsigned char)*s))
        s++;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

/* 去掉註解：# 在行首、或前面是空白才算註解 (值裡的 C#、a#b 不會被切掉) */
static void strip_comment(char *s)
{
    char *p;
    for (p = s; *p != '\0'; p++) {
        if (*p == '#' && (p == s || p[-1] == ' ' || p[-1] == '\t')) {
            *p = '\0';
            return;
        }
    }
}

static int parse_bool(const char *value)
{
    return atoi(value) != 0 || _stricmp(value, "true") == 0 || _stricmp(value, "yes") == 0;
}

int config_load(const char *path, GradeConfig *cfg, char *err, size_t err_size)
{
    FILE *f = fopen_utf8(path, "r");
    char line[2048];
    int line_no = 0;
    int ranges_reset = 0;

#define FAIL(...)                                  \
    do {                                           \
        snprintf(err, err_size, __VA_ARGS__);      \
        fclose(f);                                 \
        return 0;                                  \
    } while (0)

    if (f == NULL) {
        snprintf(err, err_size, "無法開啟設定檔：%s", path);
        return 0;
    }

    while (fgets(line, sizeof(line), f) != NULL) {
        char *key, *value, *eq;
        line_no++;

        /* 去掉 UTF-8 BOM 與註解 */
        key = line;
        if (line_no == 1 && (unsigned char)key[0] == 0xEF && (unsigned char)key[1] == 0xBB &&
            (unsigned char)key[2] == 0xBF)
            key += 3;
        strip_comment(key);
        key = trim(key);
        if (*key == '\0')
            continue;

        eq = strchr(key, '=');
        if (eq == NULL)
            FAIL("設定檔第 %d 行格式錯誤 (缺少 '=')", line_no);
        *eq = '\0';
        value = trim(eq + 1);
        key = trim(key);

        if (strcmp(key, "full_score") == 0) {
            cfg->full_score = atof(value);
        } else if (strcmp(key, "compare") == 0) {
            if (!compare_mode_from_name(value, &cfg->compare.mode))
                FAIL("第 %d 行：未知的比對模式 '%s'", line_no, value);
        } else if (strcmp(key, "ignore_trailing_space") == 0) {
            cfg->compare.ignore_trailing_space = parse_bool(value);
        } else if (strcmp(key, "fullwidth_as_halfwidth") == 0) {
            cfg->compare.fullwidth_as_halfwidth = parse_bool(value);
        } else if (strcmp(key, "ignore_case") == 0) {
            cfg->compare.ignore_case = parse_bool(value);
        } else if (strcmp(key, "ignore_blank_lines") == 0) {
            cfg->compare.ignore_blank_lines = parse_bool(value);
        } else if (strcmp(key, "ignore_lines_with") == 0) {
            snprintf(cfg->compare.ignore_lines_with, sizeof(cfg->compare.ignore_lines_with), "%s", value);
        } else if (strcmp(key, "score_mode") == 0) {
            if (!score_mode_from_name(value, &cfg->rules.mode))
                FAIL("第 %d 行：未知的評分方式 '%s'", line_no, value);
        } else if (strcmp(key, "per_char_penalty") == 0) {
            cfg->rules.per_char_penalty = atof(value);
        } else if (strcmp(key, "n_chars") == 0) {
            cfg->rules.n_chars = atol(value);
        } else if (strcmp(key, "per_n_penalty") == 0) {
            cfg->rules.per_n_penalty = atof(value);
        } else if (strcmp(key, "range") == 0) {
            char max_text[32], penalty_text[32];
            RangeRule r;
            if (!ranges_reset) {
                cfg->rules.range_count = 0; /* 檔案裡有 range 就整組取代預設值 */
                ranges_reset = 1;
            }
            if (sscanf(value, "%ld %31s %31s", &r.min, max_text, penalty_text) != 3 ||
                !parse_penalty(penalty_text, &r.penalty))
                FAIL("第 %d 行：range 格式應為「最小 最大 扣分」，例如 range = 21 MAX 20", line_no);
            r.max = (_stricmp(max_text, "MAX") == 0) ? RANGE_MAX : atol(max_text);
            if (cfg->rules.range_count >= MAX_RANGE_RULES)
                FAIL("區間規則最多 %d 條", MAX_RANGE_RULES);
            cfg->rules.ranges[cfg->rules.range_count++] = r;
        } else if (strcmp(key, "require_contiguous") == 0) {
            cfg->rules.require_contiguous = parse_bool(value);
        } else if (strcmp(key, "timeout_ms") == 0) {
            cfg->timeout_ms = atoi(value);
        } else if (strcmp(key, "output_limit_mb") == 0) {
            cfg->output_limit = (long long)(atof(value) * 1024 * 1024);
        } else if (strcmp(key, "memory_limit_mb") == 0) {
            cfg->memory_limit = (long long)(atof(value) * 1024 * 1024);
        } else if (strcmp(key, "runtime_error_zero") == 0) {
            cfg->runtime_error_zero = parse_bool(value);
        } else if (strcmp(key, "gcc") == 0) {
            snprintf(cfg->gcc_path, sizeof(cfg->gcc_path), "%s", value);
        } else if (strcmp(key, "compile_flags") == 0) {
            snprintf(cfg->compile_flags, sizeof(cfg->compile_flags), "%s", value);
        } else if (strcmp(key, "keep_temp") == 0) {
            cfg->keep_temp = parse_bool(value);
        } else if (strcmp(key, "ai_fix") == 0) {
            cfg->ai_fix = parse_bool(value);
        } else if (strcmp(key, "ai_fix_tool") == 0) {
            snprintf(cfg->ai_fix_tool, sizeof(cfg->ai_fix_tool), "%s", value[0] != '\0' ? value : "auto");
        } else if (strcmp(key, "ai_fix_penalty_per_char") == 0) {
            cfg->ai_fix_penalty_per_char = atof(value);
        } else if (strcmp(key, "ai_fix_penalty_max") == 0) {
            cfg->ai_fix_penalty_max = atof(value);
        } else if (strcmp(key, "ai_fix_attempts") == 0) {
            cfg->ai_fix_attempts = atoi(value);
        } else if (strcmp(key, "ai_fix_timeout_ms") == 0) {
            cfg->ai_fix_timeout_ms = atoi(value);
        } else {
            FAIL("第 %d 行：未知的設定項目 '%s'", line_no, key);
        }
    }
    fclose(f);

    /* 基本檢查：這些值不合理會讓分數算錯或程式永遠跑不完 */
    if (cfg->full_score <= 0) {
        snprintf(err, err_size, "full_score 必須大於 0");
        return 0;
    }
    if (cfg->timeout_ms <= 0) {
        snprintf(err, err_size, "timeout_ms 必須大於 0");
        return 0;
    }
    if (cfg->output_limit <= 0) {
        snprintf(err, err_size, "output_limit_mb 必須大於 0");
        return 0;
    }
    if (cfg->ai_fix_penalty_per_char < 0 || cfg->ai_fix_penalty_max < 0) {
        snprintf(err, err_size, "ai_fix_penalty_per_char / ai_fix_penalty_max 不能是負數");
        return 0;
    }
    if (cfg->ai_fix_attempts < 1)
        cfg->ai_fix_attempts = 1;
    if (cfg->ai_fix_timeout_ms <= 0)
        cfg->ai_fix_timeout_ms = 180000;
#undef FAIL
    return 1;
}

int config_save(const char *path, const GradeConfig *cfg)
{
    FILE *f = fopen_utf8(path, "w");
    char num[64];
    int i;

    if (f == NULL)
        return 0;

    fprintf(f, "# C 作業批改工具設定檔 (可用記事本修改；1 = 開啟，0 = 關閉)\n\n");
    format_score(num, sizeof(num), cfg->full_score);
    fprintf(f, "full_score = %s\n\n", num);

    fprintf(f, "# ---- 比對方式 ----\n");
    fprintf(f, "compare = %s            # strict / ignore_whitespace / token\n",
            compare_mode_name(cfg->compare.mode));
    fprintf(f, "ignore_trailing_space = %d     # 忽略行尾空白、結尾多或少的換行\n",
            cfg->compare.ignore_trailing_space);
    fprintf(f, "fullwidth_as_halfwidth = %d    # 全形英數符號視為半形 (：= :)\n",
            cfg->compare.fullwidth_as_halfwidth);
    fprintf(f, "ignore_case = %d               # 忽略英文大小寫\n", cfg->compare.ignore_case);
    fprintf(f, "ignore_blank_lines = %d        # 忽略空白行\n", cfg->compare.ignore_blank_lines);
    fprintf(f, "ignore_lines_with = %s\n\n", cfg->compare.ignore_lines_with);

    fprintf(f, "# ---- 評分方式 (每個 testcase 各自扣分，每題最低 0 分；差異 0 一律不扣) ----\n");
    fprintf(f, "score_mode = %s          # per_char / per_n_char / range\n\n",
            score_mode_name(cfg->rules.mode));
    fprintf(f, "# per_char：每 1 CHAR 扣幾分\n");
    fprintf(f, "per_char_penalty = %g\n\n", cfg->rules.per_char_penalty);
    fprintf(f, "# per_n_char：每 N CHAR 扣幾分\n");
    fprintf(f, "n_chars = %ld\n", cfg->rules.n_chars);
    fprintf(f, "per_n_penalty = %g\n\n", cfg->rules.per_n_penalty);
    fprintf(f, "# range：range = 最小CHAR 最大CHAR(或 MAX) 扣分(或 ALL = 扣光該題)\n");
    for (i = 0; i < cfg->rules.range_count; i++) {
        const RangeRule *r = &cfg->rules.ranges[i];
        format_penalty(num, sizeof(num), r->penalty);
        if (r->max == RANGE_MAX)
            fprintf(f, "range = %ld MAX %s\n", r->min, num);
        else
            fprintf(f, "range = %ld %ld %s\n", r->min, r->max, num);
    }
    fprintf(f, "require_contiguous = %d\n\n", cfg->rules.require_contiguous);

    fprintf(f, "# ---- 執行限制 ----\n");
    fprintf(f, "timeout_ms = %d\n", cfg->timeout_ms);
    fprintf(f, "output_limit_mb = %g\n", cfg->output_limit / (1024.0 * 1024.0));
    fprintf(f, "memory_limit_mb = %g\n", cfg->memory_limit / (1024.0 * 1024.0));
    fprintf(f, "runtime_error_zero = %d        # 1 = 程式當掉的題目直接 0 分\n\n", cfg->runtime_error_zero);

    fprintf(f, "# ---- 編譯 ----\n");
    fprintf(f, "gcc = %s\n", cfg->gcc_path);
    fprintf(f, "compile_flags = %s\n", cfg->compile_flags);
    fprintf(f, "keep_temp = %d\n\n", cfg->keep_temp);

    fprintf(f, "# ---- 編譯失敗 (CE) 自動修正 ----\n");
    fprintf(f, "ai_fix = %d                     # 1 = 呼叫本機 AI 做最小修正後繼續批改；0 = CE 直接 0 分\n",
            cfg->ai_fix);
    fprintf(f, "ai_fix_tool = %s             # auto / claude / codex / gemini / 自訂命令列\n", cfg->ai_fix_tool);
    fprintf(f, "ai_fix_penalty_per_char = %g    # 每修正 1 CHAR 扣幾分\n", cfg->ai_fix_penalty_per_char);
    fprintf(f, "ai_fix_penalty_max = %g         # 修正扣分上限 (0 = 不另設上限)\n", cfg->ai_fix_penalty_max);
    fprintf(f, "ai_fix_attempts = %d\n", cfg->ai_fix_attempts);
    fprintf(f, "ai_fix_timeout_ms = %d\n", cfg->ai_fix_timeout_ms);

    fclose(f);
    return 1;
}

static int same_compare(const CompareOptions *a, const CompareOptions *b)
{
    return a->mode == b->mode && !a->ignore_trailing_space == !b->ignore_trailing_space &&
           !a->fullwidth_as_halfwidth == !b->fullwidth_as_halfwidth && !a->ignore_case == !b->ignore_case &&
           !a->ignore_blank_lines == !b->ignore_blank_lines &&
           strcmp(a->ignore_lines_with, b->ignore_lines_with) == 0;
}

/* 只比較「目前選用的評分方式」用到的數字 */
static int same_rules(const ScoreRules *a, const ScoreRules *b)
{
    int i;

    if (a->mode != b->mode)
        return 0;
    switch (a->mode) {
    case SCORE_PER_CHAR:
        return a->per_char_penalty == b->per_char_penalty;
    case SCORE_PER_N_CHAR:
        return a->n_chars == b->n_chars && a->per_n_penalty == b->per_n_penalty;
    case SCORE_RANGE:
        if (a->range_count != b->range_count || !a->require_contiguous != !b->require_contiguous)
            return 0;
        for (i = 0; i < a->range_count; i++)
            if (a->ranges[i].min != b->ranges[i].min || a->ranges[i].max != b->ranges[i].max ||
                a->ranges[i].penalty != b->ranges[i].penalty)
                return 0;
        return 1;
    }
    return 0;
}

int config_match_template(const GradeConfig *cfg)
{
    int i;

    for (i = 0; i < config_template_count(); i++) {
        GradeConfig t = *cfg;
        config_apply_template(&t, i);
        if (same_compare(&t.compare, &cfg->compare) && same_rules(&t.rules, &cfg->rules))
            return i;
    }
    return -1;
}
