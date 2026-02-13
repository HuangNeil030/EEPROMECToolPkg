# EEPROMECToolPkg
# EEPROMECTool 開發筆記與使用說明 (README)

## 1. 專案與架構簡介

這是一個專為 UEFI Shell 環境開發的互動式 EC (Embedded Controller) EEPROM 讀寫工具。本工具的設計目的在於提供一個統一的介面，以存取系統中由 EC 管理的 EEPROM 資料 。在現代筆記型電腦與嵌入式系統中，EC 負責管理多項關鍵資訊，例如系統序號 (Serial Number)、MAC 位址以及 UUID 等 。

本工具完整支援兩種截然不同的硬體存取拓撲：

* 
**Port I/O 模式**：包含傳統的 `60/64` (Legacy KBC) 以及 `62/66` (ACPI) 介面 。


* 
**Index I/O 模式**：支援特定廠商的記憶體映射 (Memory Mapping) 存取，包含 ENE 晶片 (Base: `0xFD60`) 與 Nuvoton 晶片 (Base: `0x0A00`) 。



---

## 2. 核心存取協定與硬體暫存器定義

### A. Port I/O 介面 (60/64 & 62/66)

Port I/O 依賴直接讀寫特定的 CPU I/O Port。

* 
**Data Port**：`0x60` (8042) 或 `0x62` (ACPI) 。


* 
**Command Port**：`0x64` (8042) 或 `0x66` (ACPI) 。


* **狀態暫存器 (Status Register)**：
* 
**IBF (Input Buffer Full)**: 位於 Bit 1 。表示主機 (Host) 已寫入資料，EC 尚未讀取。主機在寫入前必須確保此位元為清空狀態 。


* 
**OBF (Output Buffer Full)**: 位於 Bit 0 。表示 EC 已將資料準備好，主機可以讀取。主機在讀取資料前必須確認此位元已被設定 。





### B. Index I/O 介面 (ENE & Nuvoton)

Index I/O 是一種透過 "Index" 與 "Data" 暫存器間接存取 EC 內部 RAM 的機制。

* **ENE 晶片 (`0xFD60`)**：
* High Byte Index: Base + `0x01` 。


* Low Byte Index: Base + `0x02` 。


* Data Port: Base + `0x03` 。




* **Nuvoton 晶片 (`0x0A00`)**：
* High Byte Index: Base + `0x00` 。


* Low Byte Index: Base + `0x01` 。


* Data Port: Base + `0x02` 。




* **通訊交握控制暫存器 (CMD_CNTL)**：
* 
**Bit 0 (Processing)**: 標示 EC 正在處理命令或資料 。


* 
**Bit 1 (Start)**: 觸發 EC 開始執行命令的旗標 。





---

## 3. 開發者 API 指南 (Developer Functions)

此區段詳細解說 `EEPROMECTool.c` 中各階層函式的實作細節，這對於後續維護或移植到其他專案至關重要。

### 底層等待機制 (Low-Level Wait Helpers)

為了防止系統因 EC 無回應而死結，所有的 I/O 操作都必須包裝在具備 Timeout 機制的迴圈中。

* **`PortWait(UINT16 Port, UINT8 Mask, UINT8 TargetVal)`**
* **功能**：處理實體 Port I/O 的等待邏輯。它會連續執行 `IoRead8(Port)` 並進行 Mask 遮罩比對，直到符合 `TargetVal` 或超時為止。
* 
**設計考量**：直接用於輪詢 IBF 與 OBF 的硬體狀態 。




* **`IdxWaitCtl(UINT8 Mask, UINT8 TargetVal)`**
* **功能**：專門用於 Index I/O 的控制暫存器等待。
* **關鍵機制**：與 Port I/O 不同，它**絕對不能**直接使用 `IoRead8` 讀取暫存器位址。它必須呼叫 `IdxRead8`，透過 High/Low Index 間接讀取 EC RAM 中的狀態值。
* **除錯能力**：發生 Timeout 時，會將當前的 Control Register 位址、讀取值、Mask 遮罩與目標值列印出來，協助判別是映射位址錯誤還是 EC 當機。



### Index I/O 間接存取層 (Indirect Access Layer)

* **`IdxRead8(UINT16 EcRamAddr)` / `IdxWrite8(UINT16 EcRamAddr, UINT8 Val)**`
* 
**功能**：將傳入的 16-bit `EcRamAddr` 拆分為 High Byte 與 Low Byte，依序寫入對應的 Index Port，最後從 Data Port 進行讀取或寫入 。


* 
**架構相容性**：函式內部會根據全域變數 `mAccType` 自動切換 ENE 與 Nuvoton 的暫存器偏移量 (Offsets) 。





### 協定執行層 (Protocol Handlers)

* **`PortOp(UINT8 Cmd, UINT8 *Val, BOOLEAN IsWrite)`**
* **功能**：執行標準的 Port I/O 命令序列。
* **執行流程**：
1. 檢查 IBF 清空 。


2. 寫入指令到 Command Port 。


3. 檢查 IBF 清空 。


4. 寫入位址或 Bank 編號到 Data Port 。


5. 如果是讀取：檢查 OBF 設置 ，然後從 Data Port 讀取資料 。


6. 如果是寫入：寫入實際資料到 Data Port 。






* **`IdxExec(UINT8 Cmd, UINT8 Addr, UINT8 Data)`**
* **功能**：處理高度規範化的 Mailbox Handshake (交握) 協定。
* **嚴格執行順序**：
1. 等待 `CMD_CNTL` 的 Bit 0 (Processing) 為 0 。


2. 設定 `CMD_CNTL` 的 Bit 0 為 1 以鎖定緩衝區 。


3. 
**寫入資料到緩衝區**：將 Command、Address 及 Data 填入對應的記憶體映射區 。


4. 設定 `CMD_CNTL` 的 Bit 1 (Start) 為 1，通知 EC 開始處理 。


5. 等待 EC 執行完畢，即 `CMD_CNTL` 的 Bit 1 清空為 0 。


6. 清除 `CMD_CNTL` 的 Bit 0 (Processing) 。







### 高階操作介面 (High-Level API)

* **`EcSetBank(UINT8 B)`**
* 
**功能**：切換 EEPROM 的 Bank 頁面。EEPROM 空間通常被劃分為 Bank 0 到 Bank 7 。


* 
**對應指令**：發送 `0x42` (EC_CMD_EEPROM_BANK_NUM) 。




* **`EcRW(UINT8 A, UINT8 *D, BOOLEAN W)`**
* **功能**：根據 `W` 參數決定執行讀取或寫入。
* 
**對應指令**：讀取發送 `0x4E` (EC_CMD_EEPROM_READ) ，寫入發送 `0x4D` (EC_CMD_EEPROM_WRITE) 。

---

## 4. 終端使用者操作手冊 (User Interface Guide)

工具啟動後，將呈現一個互動式的 Hex Editor 介面。EEPROM 的定址方式為 Bank (0~7) 配合 Offset (00~FF) 。

### 快捷鍵與功能列表

| 按鍵指令 | 功能描述與細節 |
| --- | --- |
| **F1** | 強制切換為 **Port I/O 60/64** 模式。通常用於較舊的 Legacy 架構。 |
| **F2** | 強制切換為 **Port I/O 62/66** 模式。此為 ACPI 標準介面，相容性與穩定度最高。 |
| **I (Access)** | 循環切換硬體存取協定。順序為：`PortIO`  `IndexIO-ENE`  `IndexIO-Nuvoton`。 |
| **PgUp / PgDn** | 切換 EEPROM Bank。支援 Bank 0 到 Bank 7 的快速切換 。

 |
| **TAB (Mode)** | 切換游標修改的資料寬度。支援 `BYTE` (8-bit)、`WORD` (16-bit) 與 `DWORD` (32-bit)。 |
| **方向鍵** | 在 Hex 視窗中移動藍色反白游標，精準定位目標 Address。 |
| **ENTER (Write)** | 在當前游標位置**寫入新資料**。程式會彈出輸入提示，依據當前的 Mode 要求輸入對應長度的 Hex 字串。 |
| **R (Refresh)** | 重新讀取當前 Bank 的所有資料 256 bytes，並更新畫面顯示。 |
| **ESC (Exit)** | 安全退出工具並返回 UEFI Shell。 |

### 畫面佈局說明

1. **狀態列 (頂部)**：顯示當前的工具狀態，包含已啟用的存取協定 (`PortIO` / `Index-ENE` 等)、正在監聽的 Port 位址、當前的 Bank 編號以及編輯模式 (`BYTE`/`WORD`/`DWORD`)。
2. **Hex 檢視區 (左側)**：以 16x16 網格顯示 256 Bytes 的 EEPROM 資料。藍色游標標示目前準備寫入或讀取的位置。
3. 
**ASCII 檢視區 (右側)**：將左側的 Hex 數值即時轉換為 ASCII 字元。若數值介於 `0x20` 與 `0x7E` 之間，則顯示對應的英數字元；否則以 `.` (點) 取代。這對於快速尋找系統序號或 MAC 位址非常有效 。

---

cd /d D:\BIOS\MyWorkSpace\edk2

edksetup.bat Rebuild

chcp 65001

set PYTHONUTF8=1

set PYTHONIOENCODING=utf-8

rmdir /s /q Build\EEPROMECToolPkg

build -p EEPROMECToolPkg\EEPROMECToolPkg.dsc -a X64 -t VS2019 -b DEBUG
