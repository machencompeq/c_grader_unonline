# 設計：編譯失敗 (CE) 自動最小修正與雙重扣分

日期：2026-09-23　狀態：已依使用者需求定案，進入實作 (非互動工作階段，假設項目列於文末)

## 1. 目標

學生程式編譯失敗 (CE) 時，不再直接 0 分，而是：

1. 把**學生原始碼 + gcc 錯誤訊息**交給本機 AI CLI (claude / codex / gemini，與 tools\ 腳本相同的工具)，
   要求做「能編譯即可」的**最小字元修改**，不得更動邏輯、輸出文字、排版。
2. **修正扣分** = 原始碼 → 修正後原始碼 的編輯距離 (CHAR，與輸出比對用同一套 Levenshtein) × 每 CHAR 扣分，可設上限。
3. 用修正後的程式**照常編譯、執行所有測資、比對輸出、依現有評分規則扣分** (輸出扣分)。
4. **總扣分 = 修正扣分 + 輸出扣分**；成績 = max(0, 滿分 − 總扣分)。
5. 報告、CSV、畫面、命令列都要看得到「修正了幾個字、扣了幾分、修了哪裡」。

不在範圍：修正邏輯錯誤、多檔案專案的自動修正、把 AI 用在參考答案上。

## 2. 架構位置

```
grader.c  grader_grade_student()
   找原始碼 → compile_with_fallbacks() 失敗
        │
        ▼  (cfg.ai_fix == 1 且只有一個 .c)
   try_ai_fix()  ── ai_fix.c ──►  ai_tool_find()   找 claude/codex/gemini (或自訂命令)
        │                          ai_fix_prompt()  組提示詞 (原始碼 + gcc 訊息)
        │                          ai_ask()         cmd.exe /c <tool> < prompt.txt > answer.txt (executor.c, 有 timeout)
        │                          ai_extract_code() 去掉 Markdown 圍欄 / 說明文字
        │
        ├─ 修正後編譯成功 → status = STUDENT_CE_FIXED，fix_chars = compare_outputs(原始, 修正)
        │                    → 沿用現有流程執行測資、比對、評分
        │                    → 最後 score = max(0, score − fix_penalty)
        └─ 仍失敗 (最多 ai_fix_attempts 次) / 找不到工具 / 超時 → 維持 STUDENT_COMPILE_ERROR，note 說明原因
```

新模組 `src/ai_fix.c / ai_fix.h` 只負責「找工具、問 AI、取出程式碼」，不知道學生、分數；
流程與扣分留在 grader.c，與現有分層一致 (核心層各自獨立、可單獨測試)。

## 3. 設定 (grader.cfg 新增)

```
# ---- 編譯失敗 (CE) 自動修正 ----
ai_fix = 1                      # 1 = 編譯失敗時呼叫本機 AI 做最小修正後繼續批改；0 = CE 直接 0 分
ai_fix_tool = auto              # auto / claude / codex / gemini / 自訂命令列 (提示詞從 stdin 進、程式碼從 stdout 出)
ai_fix_penalty_per_char = 1     # 每修正 1 CHAR 扣幾分
ai_fix_penalty_max = 0          # 修正扣分上限；0 = 不另設上限 (最多仍只扣到滿分)
ai_fix_attempts = 2             # 修正後仍不能編譯時，最多再讓 AI 試幾次 (含第一次)
ai_fix_timeout_ms = 180000      # 每次呼叫 AI 的時間限制
```

GradeConfig 對應新增欄位；模板 (config_apply_template / config_match_template) **不碰**這些欄位，
與「滿分、時間限制」相同歸類為「不屬於模板的設定」。

## 4. 資料結構

```c
typedef enum { STUDENT_OK, STUDENT_CE_FIXED, STUDENT_COMPILE_ERROR, STUDENT_NO_SOURCE } StudentStatus;
int student_ran_tests(StudentStatus s);   /* OK 或 CE_FIXED：有執行測資 */

StudentResult 新增：
    char  *fixed_source;   /* 修正後原始碼 (UTF-8) */
    long   fix_chars;      /* 修正字元數，-1 = 沒有修正 */
    int    fix_approximate;
    double fix_penalty;    /* 修正扣分 (已套用上限) */
    int    fix_attempts;   /* AI 試了幾次 */
    char   fix_tool[32];   /* 實際使用的工具名稱 */
    char  *fix_log;        /* 修正後程式的 gcc 訊息 (警告)；compile_log 保留原始 CE 訊息 */
```

`total_deduction` 對 CE_FIXED 學生 = 滿分 − 成績 (= 輸出扣分 + 修正扣分，封頂於滿分)。

## 5. 提示詞 (ai_fix_prompt)

繁體中文，內容固定，重點：只修編譯錯誤、不可改邏輯/輸出/排版/縮排、改得越少越好 (學生依修改字元數扣分)、
只輸出完整程式碼、不要 Markdown。第 2 次以後附上「你上次的修正仍無法編譯」+ 新的 gcc 訊息 + 上次的程式碼。
原始碼若是 Big5 先轉 UTF-8 再送出，修正距離也以 UTF-8 字元計算 (中文字 = 1 CHAR)。

## 6. 呼叫方式 (ai_ask)

沿用 tools\*.ps1 已驗證可行的做法，但不經過 PowerShell、不開視窗：

```
cmd.exe /d /s /c "<command> 2> "<work>\ai_error.txt""     stdin = prompt.txt, stdout = answer.txt
```
- 透過 `run_program()` (executor.c)：有 timeout、輸出上限 (4 MB)、Job Object，AI 卡住也會被結束。
- 呼叫前移除環境變數 `CLAUDECODE` (在 Claude Code 終端機裡巢狀呼叫會被擋，腳本同樣處理)。
- 工具偵測：`SearchPathW` 依 PATH 找 `<tool>.exe / .cmd / .bat / .ps1`；`auto` 依序 claude → codex → gemini。
  結果在同一次批改中只找一次 (Grader 內快取)。
- `ai_fix_tool` 若不是上述名稱，就當成完整命令列 (方便換版本參數、或測試時接假工具)。

## 7. 修正字元數的定義

`compare_outputs(原始碼, 修正後, {mode = strict, ignore_trailing_space = 1})`：
- CRLF/LF 一視同仁 (AI 回傳多半是 LF)、去 BOM、行尾空白與檔尾換行不計、中文字 = 1 CHAR。
- 其餘 (縮排、空白行) 全部照算：AI 若重排版，學生會被多扣，所以提示詞明確禁止；
  這是刻意的取捨——「最小修改」由 AI 負責，工具只誠實計算距離並在報告顯示每個被改的字。

## 8. 呈現

| 位置 | 內容 |
|---|---|
| 主畫面表格 | 狀態「CE→AI 修正」(淡藍底)；備註「AI 修正 N CHAR (扣 X)」 |
| 主畫面設定 | 新群組「編譯失敗與執行異常」：☑ 啟用、工具下拉 (auto/claude/codex/gemini)、每 CHAR 扣、上限、☑ RE 直接 0 分 (自評分方式群組移過來) |
| 詳細視窗 | 摘要顯示「編譯：CE→AI 修正 N CHAR (扣 X)」；「查看編譯訊息」顯示原始 CE 訊息與修正後訊息 |
| HTML 報告 (老師版/學生版) | 摘要卡 pill「CE→AI 修正」；新區塊「AI 自動修正的編譯錯誤」：gcc 錯誤、左右對照 (原始碼紅字刪除線 = 被改掉/刪除、修正後綠字 = 新增/改成)，沿用現有 mark_differences 標示器 |
| 全班總表 | 狀態欄 pill；扣分欄含修正扣分 |
| CSV | CompileStatus = `CE Fixed`；新增欄 `FixChars`、`FixPenalty` (放在 Penalty 之後、Score 之前)；Remarks 附說明 |
| CLI | 狀態欄 `CE→修正(N)`；`--detail` 印出修正資訊；新增 `--no-ai-fix` |

## 9. 錯誤處理

| 情況 | 處理 |
|---|---|
| 找不到任何 AI 工具 | 該生維持 CE (0 分)，note「找不到本機 AI 工具 (claude/codex/gemini)」；主畫面狀態列提醒一次 |
| AI 超時 / 非 0 結束碼 / 空回答 | 維持 CE，note 附原因；ai_error.txt 前幾行寫入 compile_log 末尾方便查 |
| 修正後仍 CE (用完 attempts) | 維持 CE，note「AI 修正 N 次後仍無法編譯」；最後一次的修正碼與錯誤附在報告 (老師可判斷) |
| 學生有多個 .c | 不修正，note 說明 |
| AI 回傳非 C 程式碼 (無法編譯) | 同「仍 CE」 |
| 批改中按「停止」 | 等目前這位 (含 AI 呼叫，最多 timeout) 結束後停止，狀態列已有「等目前這位學生批改完」 |

## 10. 測試

- 單元 (tests/test_core.c)：`ai_extract_code` (圍欄/無圍欄/前後說明文字/BOM)、修正扣分計算含上限、設定檔新鍵讀寫、
  `student_ran_tests`。
- 端對端 (build\grader_cli.exe + tests\fixtures)：以 `ai_fix_tool = cmd /c type <修好的 main.c>` 假工具驗證
  004_compile_error → `CE Fixed`、fix_chars = 1、成績 = 測資得分 − 1；`--no-ai-fix` 時仍為 CE 0 分。
- 實機：本機有 claude CLI，以 004 實跑一次確認提示詞與擷取正確。

## 11. 同批處理的既有問題 (審查發現，見 docs/REVIEW-2026-09-23.md)

比對近似值改用帶狀 (Ukkonen) 精確演算法、區間模式差異 0 永不扣分、設定檔 `#` 出現在值中被當註解、
CSV 公式防護寫壞引號、上次題目路徑無法還原 (.c 檔被當資料夾檢查)、表格重繪讀到批改中的半成品、
行尾空白規則對全形空白不一致、report.c 與 comparator.c 各自解碼 UTF-8 (無效位元組算法不同)、
OLE 後整個超大輸出檔仍全部讀進記憶體、重新編譯前未刪舊 exe。

## 12. 假設 (無法即時詢問，若不符請告知即可調整)

1. `ai_fix` 預設**開啟**：這是使用者明確要的行為；老師可在畫面關閉，設定隨題目存檔。
2. 修正扣分預設每 CHAR 1 分、無另設上限 (最多扣到滿分)。
3. 只修正單一 .c 的學生；多檔案維持 CE。
4. 學生版報告也顯示 AI 修正對照 (讓學生知道少了什麼)。
5. 工具偵測順序沿用腳本：claude → codex → gemini。
