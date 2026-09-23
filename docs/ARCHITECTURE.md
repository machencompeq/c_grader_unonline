# 架構圖 (Mermaid)

在 VS Code (內建 Markdown 預覽支援 Mermaid)、GitHub、或 https://mermaid.live 都能直接看圖。
文字版的分層說明見 [DESIGN.md](DESIGN.md)。

## 1. 模組與相依關係

```mermaid
flowchart TB
    subgraph UI["介面層 (只負責畫面與輸入)"]
        main["main.c<br/>GUI 入口"]
        gui["gui.c<br/>主視窗：資料夾、模板、設定、CE 自動修正、結果表格、背景執行緒"]
        gui_common["gui_common.c<br/>字型/DPI/模態視窗/工具提示/文字視窗"]
        gui_rules["gui_rules.c<br/>編輯評分規則"]
        gui_detail["gui_detail.c<br/>學生詳細 (Expected/Actual、AI 修正)"]
        gui_tests["gui_tests.c<br/>編輯測資總表 / AI 產生測資 / 預覽"]
        cli["cli_main.c<br/>命令列版"]
    end

    subgraph FLOW["流程層"]
        grader["grader.c<br/>找原始碼 → 編譯 (Big5/多檔/AI 修正) → 執行測資 → 比對 → 評分"]
        report["report.c<br/>老師版 / 學生版 HTML、全班總表、差異標示"]
        exporter["exporter.c<br/>CSV"]
    end

    subgraph CORE["核心層 (各自獨立、可單獨測試)"]
        compiler["compiler.c<br/>呼叫 gcc"]
        executor["executor.c<br/>Job Object、timeout、輸出上限、記憶體上限"]
        comparator["comparator.c<br/>差異數 (Levenshtein / 帶狀精確 / 近似)"]
        scorer["scorer.c<br/>差異數 → 扣分、規則檢查"]
        config["config.c<br/>grader.cfg、模板、修正扣分"]
        testcase["testcase.c<br/>.in/.out、meta.txt、總表格式"]
        roster["roster.c<br/>名單.csv"]
        aifix["ai_fix.c<br/>找本機 AI CLI、問答、取出程式碼"]
        util["util.c<br/>UTF-8/寬字元、檔案、路徑、StrBuf"]
    end

    subgraph EXT["外部程式"]
        gcc[("gcc (MinGW-w64)")]
        student[("學生程式 .exe")]
        aicli[("claude / codex / gemini CLI")]
        ps["tools\\ai_testcases.ps1<br/>tools\\ai_roster.ps1"]
    end

    main --> gui
    gui --> gui_common & gui_rules & gui_detail & gui_tests
    gui & cli --> grader & report & exporter & config
    gui_tests --> grader & testcase
    gui --> ps
    gui_tests --> ps
    grader --> compiler & executor & comparator & scorer & config & testcase & roster & aifix
    report --> comparator & config
    compiler --> executor
    aifix --> executor
    executor --> gcc & student & aicli
    ps --> aicli
    CORE --> util
```

## 2. 一次批改的流程 (含 CE 自動修正)

```mermaid
sequenceDiagram
    autonumber
    participant UI as gui.c / cli_main.c
    participant G as grader.c
    participant C as compiler.c
    participant A as ai_fix.c
    participant E as executor.c
    participant M as comparator.c + scorer.c
    participant R as report.c / exporter.c

    UI->>G: grader_open(參考答案.c, 學生資料夾, cfg)
    G->>G: 讀測資、名單.csv、建立 %TEMP%\c-grader\run-*
    UI->>G: grader_prepare_reference()
    G->>C: 編譯參考答案 (+ stdout 不暫存的小檔)
    C->>E: run_program(gcc …)
    loop 每組測資
        G->>E: run_program(reference.exe < test.in)
        E-->>G: 標準答案 (有 .out 就以 .out 為準並檢查一致)
    end
    loop 每位學生
        UI->>G: grader_grade_student(i)
        G->>G: 找 .c (含子資料夾)、非 UTF-8 逐行轉 UTF-8 副本
        G->>C: 編譯 (失敗 → 多檔改單檔 → 原始 bytes)
        alt 編譯失敗且 ai_fix = 1 且只有一個 .c
            G->>A: ai_fix_prompt(原始碼 + gcc 錯誤)
            A->>E: cmd /c claude -p … < ai_prompt.txt > ai_answer.txt (timeout)
            E-->>A: 回答
            A-->>G: ai_extract_code() → 修正後原始碼
            G->>C: 再編譯 (仍失敗就帶著新錯誤再試，最多 ai_fix_attempts 次)
            G->>M: fix_chars = compare_outputs(原始碼, 修正後)
        end
        alt 有可執行檔
            loop 每組測資
                G->>E: run_program(student.exe < test.in, 60s / 10MB / 512MB)
                E-->>G: 輸出檔、狀態 (OK / RE / TLE / OLE)
                G->>M: 差異數 → 扣分 (TLE/OLE 0 分；RE 預設仍比對)
            end
            G->>G: 成績 = max(0, Σ各題得分 − 修正扣分)
        else 無法編譯
            G->>G: Compile Error，0 分 (note 記錄 AI 失敗原因)
        end
        G-->>UI: StudentResult
        UI->>R: 老師版 + 學生版 HTML (含 AI 修正對照)
    end
    UI->>R: 全班總表 index.html、CSV
    UI->>G: grader_close() 刪暫存
```

## 3. 差異數 (CHAR) 的計算管線

```mermaid
flowchart LR
    A["標準答案 bytes"] --> D1["解碼 UTF-8<br/>去 BOM、\\r\\n→\\n<br/>不合法 byte 各算 1 字"]
    B["學生輸出 bytes"] --> D2["解碼 UTF-8 (同左)"]
    D1 --> F1["fold：全形→半形、大小寫 (選項)"]
    D2 --> F2["fold (同左)"]
    F1 --> L1["逐行：去行尾空白、刪空白行、刪含關鍵字的行 (選項)"]
    F2 --> L2["逐行 (同左)"]
    L1 --> M1["模式：Strict 不動 / Ignore WS 刪所有空白 / Token 合併空白"]
    L2 --> M2["模式 (同左)"]
    M1 & M2 --> P["去掉相同的開頭與結尾"]
    P --> Q{"n × m ≤ 5000 萬格?"}
    Q -- 是 --> DP["完整 Levenshtein (兩列 DP)<br/>精確"]
    Q -- 否 --> BAND["帶狀 Ukkonen：帶寬 64→128→256…<br/>距離 ≤ 帶寬即精確"]
    BAND -- "工作量 > 4 億格" --> APX["近似：逐位置不同 + 長度差<br/>標記 approximate"]
    DP & BAND & APX --> S["scorer：每 CHAR / 每 N CHAR / 區間 → 扣分<br/>差異 0 一律不扣"]
```

## 4. 編譯失敗 (CE) 的處理與扣分

```mermaid
flowchart TD
    CE["gcc 編譯失敗"] --> Q1{"ai_fix = 1 ?"}
    Q1 -- 否 --> ZERO["Compile Error，0 分"]
    Q1 -- 是 --> Q2{"只有一個 .c ?"}
    Q2 -- 否 --> ZERO
    Q2 -- 是 --> Q3{"找到 claude/codex/gemini<br/>(或自訂命令) ?"}
    Q3 -- 否 --> ZERO
    Q3 -- 是 --> ASK["提示詞 = 規則 + gcc 錯誤 + 原始碼<br/>stdin → AI CLI → stdout (timeout)"]
    ASK --> EXT["取出程式碼 (去 Markdown 圍欄)"]
    EXT --> CMP["再編譯"]
    CMP -- 失敗且還有次數 --> ASK
    CMP -- 失敗且次數用完 --> ZERO2["Compile Error，0 分<br/>報告附最後一次嘗試"]
    CMP -- 成功 --> DIST["修正字元數 = 編輯距離(原始碼, 修正後)<br/>strict + 忽略行尾空白/檔尾換行"]
    DIST --> RUN["用修正後的程式執行所有測資 → 各題輸出扣分"]
    RUN --> TOTAL["成績 = max(0, Σ各題得分 − 修正扣分)<br/>修正扣分 = min(字元數 × 每 CHAR 扣分, 上限)"]
```

## 5. 檔案與資料夾

```mermaid
flowchart LR
    subgraph TOOL["批改工具資料夾"]
        exe["Grader.exe"]
        roster["名單.csv (全班共用)"]
        tools["tools\\*.ps1"]
        cli["build\\grader_cli.exe"]
    end
    subgraph HW["題目：D:\\hw\\hw1.c"]
        ref["hw1.c 參考答案"]
        data["hw1_測資\\"]
        data --> tc["testcase\\test01.in / .out / meta.txt"]
        data --> cfg["grader.cfg (含 ai_fix 設定)"]
        data --> rep["reports\\老師版\\index.html + 每人一份<br/>reports\\學生版\\每人一份"]
        data --> csv["result.csv"]
    end
    subgraph STU["學生作業資料夾 (只讀)"]
        s1["<學號>\\main.c"]
        s2["<學號>\\專案\\src\\hw.c"]
        s3["散裝 .c"]
    end
    subgraph TMP["%TEMP%\\c-grader\\run-*  (批改完刪除)"]
        t1["_reference\\reference.exe、*_reference_output.txt"]
        t2["s0000\\student.exe、out000.txt、utf8_*.c、ai_prompt.txt、ai_answer.txt、ai_fix_1.c"]
    end
    exe --> HW & STU & TMP
```
