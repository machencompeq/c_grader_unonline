/*
 * ai_fix.h - 用本機 AI 命令列工具 (claude / codex / gemini) 修正學生程式的編譯錯誤。
 *
 * 這個模組只負責三件事，完全不知道學生、分數與批改流程：
 *   1. ai_tool_find     找出本機裝了哪一個工具 (或使用老師自訂的命令列)
 *   2. ai_ask           把提示詞交給工具 (stdin)、拿回回答 (stdout)，有時間限制
 *   3. ai_extract_code  從回答裡取出程式碼 (去掉 Markdown 圍欄與說明文字)
 * 提示詞由 ai_fix_prompt 組出：要求「只修編譯錯誤、改動字元越少越好、不可改邏輯與輸出」。
 *
 * 要不要修、修完後怎麼算修正字元數與扣分，由 grader.c 決定 (見 grader.h 的 StudentResult)。
 */
#ifndef AI_FIX_H
#define AI_FIX_H

#include <stddef.h>

#define AI_TOOL_NAME_MAX 32
#define AI_COMMAND_MAX 512

/*
 * tool = "auto" (依序找 claude → codex → gemini)、"claude" / "codex" / "gemini"，
 * 其他字串視為自訂命令列 (提示詞從 stdin 進、程式碼從 stdout 出，例如換了參數的新版工具)。
 * 找到回傳 1，並填入顯示名稱與要交給 cmd.exe 執行的命令列；找不到回傳 0。
 */
int ai_tool_find(const char *tool, char *name, size_t name_size, char *command, size_t command_size);

/*
 * 執行 command，prompt 從 stdin 送入，回傳 malloc 的回答 (UTF-8)。
 * work_dir 用來放 ai_prompt.txt / ai_answer.txt / ai_error.txt (方便老師查看)。
 * 超時、結束碼非 0、空回答都回傳 NULL 並把原因寫進 err。
 */
char *ai_ask(const char *command, const char *prompt, const char *work_dir, int timeout_ms, char *err, size_t err_size);

/*
 * 從回答取出程式碼：有 Markdown 圍欄 (```) 就取第一個圍欄裡的內容，否則整段回答。
 * 去掉 BOM、\r，結尾保證有一個換行。沒有內容回傳 NULL。回傳 malloc 的字串。
 */
char *ai_extract_code(const char *answer);

/*
 * 「修正編譯錯誤」的提示詞 (malloc)。attempt 從 1 開始；>= 2 表示上一次修正後仍無法編譯，
 * 此時 source / compile_log 是上一次修正後的程式碼與它的錯誤訊息。
 */
char *ai_fix_prompt(const char *file_name, const char *source, const char *compile_log, int attempt);

#endif
