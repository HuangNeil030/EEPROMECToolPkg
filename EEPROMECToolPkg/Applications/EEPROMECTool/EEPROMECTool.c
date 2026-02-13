/** @file
  EEPROMECApp.c (Fixed Logic Version)

  目的
  ----
  在 UEFI Shell / 自製 UEFI App 下，透過 EC 的「EEPROM 命令介面」讀寫 EEPROM。

  你遇到的 timeout 現象
  ----------------------
  - 60/64 (Legacy/KBC 路徑) 大多不 timeout。
  - 62/66 (ACPI EC I/O Port 路徑) 可能成功也可能 timeout。
  - Index I/O (ENE:FD60h / Nuvoton:0A00h / ITE:0D00h) 常見 timeout。

  本程式的修正重點（針對 Index I/O）
  --------------------------------
  1) Wait 必須用 Indirect 方式讀 EC RAM 的 Control Byte：
     - 先寫 Index High/Low 到 Base+OffIndexHigh / Base+OffIndexLow
     - 再從 Base+OffData 讀出該 EC RAM 位址的值
     絕對不能用 IoRead8(0xF982) 這種方式直接讀（那只是「EC RAM 位址」，不是 CPU I/O Port）。

  2) 正確時序：Fill Buffers -> Set START
     - 先把 Command / Addr / Data / Bank 等寫到 EC RAM buffer
     - 然後才把 Control 設為 (Processing|Start)

  3) Timeout 時會輸出 Debug：
     - Ctl 位址、Ctl 值、Mask/Target
     - Index I/O Base 與 Offsets

  注意
  ----
  - 不同 EC/SIO/板子，Index I/O 的 Base/Offsets 或 EC RAM buffer mapping 可能完全不同。
    這份程式只把你提供的三套 mapping 做成 profile。
  - 若 profile 不符合實際平台，就會卡在 Wait(Processing/Start) 造成 EFI_TIMEOUT。

  Hotkeys
  -------
  PgUp/PgDn : Bank 切換
  TAB       : 顯示模式 BYTE/WORD/DWORD
  Arrow     : 移動游標
  ENTER     : 依顯示模式寫入 (LE)，並 read-back verify
  R         : 刷新
  I         : 切換 Access (PortIO / IndexIO-ENE / IndexIO-Nuvoton / IndexIO-ITE)
  F1        : PortIO 改 60/64
  F2        : PortIO 改 62/66
  ESC       : 離開

**/

#include <Uefi.h>

#include <Library/UefiLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <Library/IoLib.h>

// ===== EEPROM/EC command =====
#define EC_CMD_EEPROM_BANK_NUM  0x42
#define EC_CMD_EEPROM_READ      0x4E
#define EC_CMD_EEPROM_WRITE     0x4D

#define EEPROM_BANK_MAX         7

// ===== Port I/O (ACPI EC / 8042) =====
#define EC_STS_OBF              (1u << 0)   // Output Buffer Full
#define EC_STS_IBF              (1u << 1)   // Input Buffer Full

#define EC_8042_DATA_PORT       0x60
#define EC_8042_CMD_PORT        0x64
#define EC_ACPI_DATA_PORT       0x62
#define EC_ACPI_CMD_PORT        0x66

// ===== Index I/O Control Bits =====
#define CMD_CNTL_PROCESSING     (1u << 0)
#define CMD_CNTL_START          (1u << 1)

#define COLS 16
#define ROWS 16

typedef enum {
  DISP_BYTE  = 1,
  DISP_WORD  = 2,
  DISP_DWORD = 4
} DISP_MODE;

typedef enum {
  ACCESS_PORTIO = 0,
  ACCESS_INDEXIO_ENE,
  ACCESS_INDEXIO_NUVOTON,
  ACCESS_INDEXIO_ITE
} EC_ACCESS_TYPE;

typedef enum {
  PORTMODE_ACPI_62_66 = 0,
  PORTMODE_8042_60_64
} EC_PORT_MODE;

typedef struct {
  EC_ACCESS_TYPE AccessType;

  // Port I/O mode (only used when AccessType==ACCESS_PORTIO)
  EC_PORT_MODE   PortMode;

  // Index I/O profile (only used when AccessType!=ACCESS_PORTIO)
  UINT16 IndexIoBase;     // ENE:0xFD60, Nuvoton:0x0A00, ITE:0x0D00
  UINT8  OffIndexHigh;    // ENE:1, Nuvoton:0, ITE:1
  UINT8  OffIndexLow;     // ENE:2, Nuvoton:1, ITE:2
  UINT8  OffData;         // ENE:3, Nuvoton:2, ITE:3

  // EC RAM addresses (platform-provided mapping)
  UINT16 CmdBuffer;            // "command buffer" pointer/addr (some designs use high/low pointer)
  UINT16 DataOfCmdBuffer;      // place to write opcode (0x42/0x4E/0x4D)
  UINT16 CmdWriteDataBuffer;   // data/param buffer base
  UINT16 CmdCntl;              // control byte
  UINT16 CmdReturnDataBuffer;  // return data byte

  // Param buffer mapping (all are EC RAM addresses)
  UINT16 WriteAddrBuf;
  UINT16 WriteDataBuf;
  UINT16 ReadAddrBuf;
  UINT16 BankBuf;
} EC_PROFILE;

STATIC EC_PROFILE mEc;

// current UI state
STATIC UINT8     mBank     = 0;
STATIC UINT8     mDump[256];
STATIC UINT8     mCursor   = 0;
STATIC DISP_MODE mDispMode = DISP_BYTE;

// ---------- Color helpers ----------
STATIC UINTN mAttrDefault = 0;

STATIC VOID SetAttr(IN UINTN Attr) { gST->ConOut->SetAttribute(gST->ConOut, Attr); }
STATIC VOID AttrDefault(VOID) { SetAttr(mAttrDefault); }
STATIC VOID AttrGreenText(VOID) { SetAttr(EFI_TEXT_ATTR(EFI_GREEN, EFI_BLACK)); }
STATIC VOID AttrCursorBlueBg(VOID) { SetAttr(EFI_TEXT_ATTR(EFI_WHITE, EFI_BLUE)); }

STATIC VOID PrintParenGreen(IN CONST CHAR16 *Text) {
  AttrGreenText();
  Print(L"(%s)", Text);
  AttrDefault();
}

// =======================================================
//                PORT I/O backend (60/64, 62/66)
// =======================================================

STATIC
VOID
GetPortPair (
  OUT UINT16 *DataPort,
  OUT UINT16 *CmdPort
  )
{
  if (mEc.PortMode == PORTMODE_8042_60_64) {
    *DataPort = EC_8042_DATA_PORT;
    *CmdPort  = EC_8042_CMD_PORT;
  } else {
    *DataPort = EC_ACPI_DATA_PORT;
    *CmdPort  = EC_ACPI_CMD_PORT;
  }
}

STATIC
UINT8
PortReadStatus (
  VOID
  )
{
  UINT16 DataPort, CmdPort;
  GetPortPair(&DataPort, &CmdPort);
  return IoRead8(CmdPort);
}

STATIC
EFI_STATUS
PortWaitIbfClear (
  IN UINTN TimeoutUs
  )
{
  while (TimeoutUs > 0) {
    if ((PortReadStatus() & EC_STS_IBF) == 0) return EFI_SUCCESS;
    gBS->Stall(50);
    TimeoutUs = (TimeoutUs > 50) ? (TimeoutUs - 50) : 0;
  }
  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
PortWaitObfSet (
  IN UINTN TimeoutUs
  )
{
  while (TimeoutUs > 0) {
    if ((PortReadStatus() & EC_STS_OBF) != 0) return EFI_SUCCESS;
    gBS->Stall(50);
    TimeoutUs = (TimeoutUs > 50) ? (TimeoutUs - 50) : 0;
  }
  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
PortWaitObfClear (
  IN UINTN TimeoutUs
  )
{
  while (TimeoutUs > 0) {
    if ((PortReadStatus() & EC_STS_OBF) == 0) return EFI_SUCCESS;
    gBS->Stall(50);
    TimeoutUs = (TimeoutUs > 50) ? (TimeoutUs - 50) : 0;
  }
  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
PortWriteCmd (
  IN UINT8 Cmd
  )
{
  EFI_STATUS Status;
  UINT16 DataPort, CmdPort;
  GetPortPair(&DataPort, &CmdPort);

  Status = PortWaitIbfClear(200000);
  if (EFI_ERROR(Status)) return Status;

  IoWrite8(CmdPort, Cmd);

  return PortWaitIbfClear(200000);
}

STATIC
EFI_STATUS
PortWriteData (
  IN UINT8 Data
  )
{
  EFI_STATUS Status;
  UINT16 DataPort, CmdPort;
  GetPortPair(&DataPort, &CmdPort);

  Status = PortWaitIbfClear(200000);
  if (EFI_ERROR(Status)) return Status;

  IoWrite8(DataPort, Data);

  return PortWaitIbfClear(200000);
}

STATIC
EFI_STATUS
PortReadData (
  OUT UINT8 *Data
  )
{
  EFI_STATUS Status;
  UINT16 DataPort, CmdPort;
  GetPortPair(&DataPort, &CmdPort);

  if (!Data) return EFI_INVALID_PARAMETER;

  Status = PortWaitObfSet(200000);
  if (EFI_ERROR(Status)) return Status;

  *Data = IoRead8(DataPort);

  return PortWaitObfClear(200000);
}

// =======================================================
//                INDEX I/O backend (ENE/Nuvoton/ITE)
// =======================================================

STATIC
VOID
IndexIoSetAddr (
  IN UINT16 EcRamAddr
  )
{
  IoWrite8(mEc.IndexIoBase + mEc.OffIndexHigh, (UINT8)(EcRamAddr >> 8));
  IoWrite8(mEc.IndexIoBase + mEc.OffIndexLow,  (UINT8)(EcRamAddr & 0xFF));
}

STATIC
VOID
IndexIoWrite8 (
  IN UINT16 EcRamAddr,
  IN UINT8  Val
  )
{
  IndexIoSetAddr(EcRamAddr);
  IoWrite8(mEc.IndexIoBase + mEc.OffData, Val);
}

STATIC
UINT8
IndexIoRead8 (
  IN UINT16 EcRamAddr
  )
{
  IndexIoSetAddr(EcRamAddr);
  return IoRead8(mEc.IndexIoBase + mEc.OffData);
}

// Indirect wait on EC RAM control byte
STATIC
EFI_STATUS
IndexWaitCtl (
  IN UINT8 Mask,
  IN UINT8 Target,
  IN UINTN TimeoutUs
  )
{
  UINT8 Cur = 0;

  while (TimeoutUs > 0) {
    Cur = IndexIoRead8(mEc.CmdCntl); // IMPORTANT: indirect read!

    if ((Cur & Mask) == Target) return EFI_SUCCESS;

    gBS->Stall(50);
    TimeoutUs = (TimeoutUs > 50) ? (TimeoutUs - 50) : 0;
  }

  // Debug on timeout
  Print(L"\n[IndexWait Timeout] Base=0x%04x Off(H/L/D)=(0x%02x/0x%02x/0x%02x)\n",
        mEc.IndexIoBase, mEc.OffIndexHigh, mEc.OffIndexLow, mEc.OffData);
  Print(L"  CtlAddr=0x%04x Cur=0x%02x Mask=0x%02x Target=0x%02x\n",
        mEc.CmdCntl, Cur, Mask, Target);

  return EFI_TIMEOUT;
}

// Fixed sequence: Fill buffers -> Set Start -> Wait Done -> Clear Processing
STATIC
EFI_STATUS
IndexExecEepromCmd (
  IN UINT8 Cmd,
  IN UINT8 AddrOrBank,
  IN UINT8 WriteData,
  IN BOOLEAN IsWrite
  )
{
  EFI_STATUS Status;

  // 1) Wait idle: Processing bit must be 0
  Status = IndexWaitCtl(CMD_CNTL_PROCESSING, 0, 200000);
  if (EFI_ERROR(Status)) return Status;

  // 2) Lock: set Processing
  IndexIoWrite8(mEc.CmdCntl, CMD_CNTL_PROCESSING);

  // 3) Fill buffers FIRST
  IndexIoWrite8(mEc.DataOfCmdBuffer, Cmd);

  if (Cmd == EC_CMD_EEPROM_BANK_NUM) {
    IndexIoWrite8(mEc.BankBuf, AddrOrBank);
  } else {
    // Read/Write address
    IndexIoWrite8(mEc.ReadAddrBuf, AddrOrBank);

    if (IsWrite) {
      IndexIoWrite8(mEc.WriteDataBuf, WriteData);
    }
  }

  // 4) Trigger: set Start|Processing
  IndexIoWrite8(mEc.CmdCntl, (UINT8)(CMD_CNTL_PROCESSING | CMD_CNTL_START));

  // 5) Wait done: Start bit becomes 0
  Status = IndexWaitCtl(CMD_CNTL_START, 0, 500000);
  if (EFI_ERROR(Status)) return Status;

  // 6) Unlock: clear Processing
  IndexIoWrite8(mEc.CmdCntl, 0);

  return EFI_SUCCESS;
}

// =======================================================
//             Unified EEPROM operations (bank/read/write)
// =======================================================

STATIC
EFI_STATUS
EcSetBank (
  IN UINT8 Bank
  )
{
  if (Bank > EEPROM_BANK_MAX) return EFI_INVALID_PARAMETER;

  if (mEc.AccessType == ACCESS_PORTIO) {
    EFI_STATUS Status;
    Status = PortWriteCmd(EC_CMD_EEPROM_BANK_NUM);
    if (EFI_ERROR(Status)) return Status;
    return PortWriteData(Bank);
  }

  return IndexExecEepromCmd(EC_CMD_EEPROM_BANK_NUM, Bank, 0, FALSE);
}

STATIC
EFI_STATUS
EcReadEeprom8 (
  IN  UINT8  Addr,
  OUT UINT8 *Val
  )
{
  if (!Val) return EFI_INVALID_PARAMETER;

  if (mEc.AccessType == ACCESS_PORTIO) {
    EFI_STATUS Status;
    Status = PortWriteCmd(EC_CMD_EEPROM_READ);
    if (EFI_ERROR(Status)) return Status;
    Status = PortWriteData(Addr);
    if (EFI_ERROR(Status)) return Status;
    return PortReadData(Val);
  }

  {
    EFI_STATUS Status = IndexExecEepromCmd(EC_CMD_EEPROM_READ, Addr, 0, FALSE);
    if (EFI_ERROR(Status)) return Status;
  }

  *Val = IndexIoRead8(mEc.CmdReturnDataBuffer);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EcWriteEeprom8 (
  IN UINT8 Addr,
  IN UINT8 Data
  )
{
  if (mEc.AccessType == ACCESS_PORTIO) {
    EFI_STATUS Status;
    Status = PortWriteCmd(EC_CMD_EEPROM_WRITE);
    if (EFI_ERROR(Status)) return Status;
    Status = PortWriteData(Addr);
    if (EFI_ERROR(Status)) return Status;
    return PortWriteData(Data);
  }

  return IndexExecEepromCmd(EC_CMD_EEPROM_WRITE, Addr, Data, TRUE);
}

// =======================================================
//                       UI helpers
// =======================================================

STATIC
BOOLEAN
IsPrintableAscii (
  IN UINT8 b
  )
{
  return (BOOLEAN)(b >= 0x20 && b <= 0x7E);
}

STATIC
UINT16
ReadU16LE (
  IN UINTN Off
  )
{
  return (UINT16)(mDump[Off] | ((UINT16)mDump[Off + 1] << 8));
}

STATIC
UINT32
ReadU32LE (
  IN UINTN Off
  )
{
  return (UINT32)(mDump[Off] |
                 ((UINT32)mDump[Off + 1] << 8) |
                 ((UINT32)mDump[Off + 2] << 16) |
                 ((UINT32)mDump[Off + 3] << 24));
}

STATIC
VOID
AlignCursorToMode (
  VOID
  )
{
  UINT8 col = (UINT8)(mCursor % COLS);
  UINT8 row = (UINT8)(mCursor / COLS);

  if (mDispMode == DISP_WORD) {
    col = (UINT8)(col & ~1u);
  } else if (mDispMode == DISP_DWORD) {
    col = (UINT8)(col & ~3u);
  }
  mCursor = (UINT8)(row * COLS + col);
}

STATIC
VOID
CycleDispMode (
  VOID
  )
{
  if (mDispMode == DISP_BYTE) mDispMode = DISP_WORD;
  else if (mDispMode == DISP_WORD) mDispMode = DISP_DWORD;
  else mDispMode = DISP_BYTE;

  AlignCursorToMode();
}

STATIC
VOID
MoveCursor (
  IN INTN dRow,
  IN INTN dCol
  )
{
  INTN row = (INTN)(mCursor / COLS);
  INTN col = (INTN)(mCursor % COLS);

  row += dRow;

  if (dCol != 0) {
    INTN step = (INTN)mDispMode; // 1/2/4
    col += dCol * step;

    if (mDispMode == DISP_WORD) col &= ~1;
    else if (mDispMode == DISP_DWORD) col &= ~3;
  }

  if (row < 0) row = 0;
  if (row >= (INTN)ROWS) row = (INTN)ROWS - 1;
  if (col < 0) col = 0;
  if (col >= (INTN)COLS) col = (INTN)COLS - 1;

  mCursor = (UINT8)(row * COLS + col);
}

STATIC
CONST CHAR16*
AccessName (
  VOID
  )
{
  switch (mEc.AccessType) {
  case ACCESS_PORTIO:          return L"PortIO";
  case ACCESS_INDEXIO_ENE:     return L"IndexIO-ENE";
  case ACCESS_INDEXIO_NUVOTON: return L"IndexIO-Nuvoton";
  case ACCESS_INDEXIO_ITE:     return L"IndexIO-ITE";
  default:                     return L"Unknown";
  }
}

STATIC
VOID
GetPortPairText (
  OUT CONST CHAR16 **Text
  )
{
  if (mEc.PortMode == PORTMODE_8042_60_64) *Text = L"60/64";
  else *Text = L"62/66";
}

STATIC
VOID
PrintHeader (
  VOID
  )
{
  CONST CHAR16 *ModeStr =
    (mDispMode == DISP_BYTE) ? L"BYTE" :
    (mDispMode == DISP_WORD) ? L"WORD" : L"DWORD";

  CONST CHAR16 *PortTxt = L"--";
  GetPortPairText(&PortTxt);

  PrintParenGreen(L"EEPROM/EC Tool");
  Print(L" ");
  PrintParenGreen(L"PortIO 60/64,62/66 + IndexIO ENE/Nuvoton/ITE");
  Print(L"\n");

  PrintParenGreen(L"Access:");
  Print(L"%s  ", AccessName());

  PrintParenGreen(L"Port:");
  if (mEc.AccessType == ACCESS_PORTIO) Print(L"%s  ", PortTxt);
  else Print(L"--  ");

  PrintParenGreen(L"Bank:");
  Print(L"%u  ", mBank);

  Print(L"Mode:%s\n", ModeStr);

  Print(L"      ");
  for (UINTN i = 0; i < COLS; i++) Print(L"%02x ", (UINTN)i);
  Print(L"   ASCII\n");
}

STATIC
VOID
PrintRow (
  IN UINTN Row
  )
{
  UINTN base = Row * COLS;

  Print(L"%02x | ", (UINTN)base);

  if (mDispMode == DISP_BYTE) {
    for (UINTN i = 0; i < COLS; i++) {
      UINTN idx = base + i;
      if (idx == mCursor) {
        AttrCursorBlueBg();
        Print(L" %02x ", (UINTN)mDump[idx]);
        AttrDefault();
      } else {
        Print(L" %02x ", (UINTN)mDump[idx]);
      }
    }
  } else if (mDispMode == DISP_WORD) {
    for (UINTN i = 0; i < COLS; i += 2) {
      UINTN idx = base + i;
      UINT16 v  = ReadU16LE(idx);
      if (mCursor == idx || mCursor == idx + 1) {
        AttrCursorBlueBg();
        Print(L" %04x  ", (UINTN)v);
        AttrDefault();
      } else {
        Print(L" %04x  ", (UINTN)v);
      }
    }
  } else {
    for (UINTN i = 0; i < COLS; i += 4) {
      UINTN idx = base + i;
      UINT32 v  = ReadU32LE(idx);
      if (mCursor >= idx && mCursor <= idx + 3) {
        AttrCursorBlueBg();
        Print(L" %08x  ", (UINTN)v);
        AttrDefault();
      } else {
        Print(L" %08x  ", (UINTN)v);
      }
    }
  }

  Print(L"  ");
  for (UINTN i = 0; i < COLS; i++) {
    UINT8 b = mDump[base + i];
    Print(L"%c", IsPrintableAscii(b) ? (CHAR16)b : L'.');
  }
  Print(L"\n");
}

STATIC
VOID
Render (
  VOID
  )
{
  gST->ConOut->ClearScreen(gST->ConOut);

  PrintHeader();
  for (UINTN r = 0; r < ROWS; r++) PrintRow(r);

  Print(L"\nKeys: ");
  PrintParenGreen(L"PgUp/PgDn"); Print(L"=Bank  ");
  PrintParenGreen(L"TAB");       PrintParenGreen(L"=Mode(BYTE/WORD/DWORD)  ");
  PrintParenGreen(L"Arrows");    Print(L"=Move  ");
  PrintParenGreen(L"ENTER");     PrintParenGreen(L"=Write(BYTE/WORD/DWORD)  ");
  PrintParenGreen(L"R");         Print(L"=Refresh  ");
  PrintParenGreen(L"I");         Print(L"=Access  ");
  PrintParenGreen(L"F1");        Print(L"=Port 60/64  ");
  PrintParenGreen(L"F2");        Print(L"=Port 62/66  ");
  PrintParenGreen(L"ESC");       Print(L"=Exit\n");
}

// ---------------- Dump / refresh ----------------
STATIC
EFI_STATUS
RefreshDump (
  VOID
  )
{
  EFI_STATUS Status;

  Status = EcSetBank(mBank);
  if (EFI_ERROR(Status)) return Status;

  for (UINTN i = 0; i < 256; i++) {
    Status = EcReadEeprom8((UINT8)i, &mDump[i]);
    if (EFI_ERROR(Status)) return Status;
  }
  return EFI_SUCCESS;
}

// ---------------- Input hex ----------------
STATIC
BOOLEAN
HexCharToNibble (
  IN  CHAR16 c,
  OUT UINT8  *n
  )
{
  if (!n) return FALSE;
  if (c >= L'0' && c <= L'9') { *n = (UINT8)(c - L'0'); return TRUE; }
  if (c >= L'a' && c <= L'f') { *n = (UINT8)(c - L'a' + 10); return TRUE; }
  if (c >= L'A' && c <= L'F') { *n = (UINT8)(c - L'A' + 10); return TRUE; }
  return FALSE;
}

STATIC
EFI_STATUS
ReadHexValueNFromKeyboard (
  IN  UINTN  HexDigits,     // 2/4/8
  OUT UINT32 *OutValue
  )
{
  EFI_INPUT_KEY Key;
  UINT32 v = 0;
  UINT8  nib;

  if (!OutValue) return EFI_INVALID_PARAMETER;
  if (HexDigits != 2 && HexDigits != 4 && HexDigits != 8) return EFI_INVALID_PARAMETER;

  Print(L"\nInput %u hex digits, ", (UINTN)HexDigits);
  PrintParenGreen(L"ESC/ENTER");
  Print(L" to cancel: ");

  for (UINTN i = 0; i < HexDigits; ) {
    if (EFI_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn, &Key))) continue;

    if (Key.ScanCode == SCAN_ESC || Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
      Print(L"\nCanceled.\n");
      return EFI_ABORTED;
    }

    if (HexCharToNibble(Key.UnicodeChar, &nib)) {
      v = (v << 4) | (UINT32)nib;
      Print(L"%c", Key.UnicodeChar);
      i++;
    }
  }

  Print(L"\n");
  *OutValue = v;
  return EFI_SUCCESS;
}

// ENTER: write by display mode (1/2/4 bytes), LE, readback verify
STATIC
EFI_STATUS
WriteByModeAtCursor (
  VOID
  )
{
  EFI_STATUS Status;
  UINTN      size   = (UINTN)mDispMode;   // 1/2/4
  UINTN      digits = size * 2;           // 2/4/8
  UINT32     inputVal;
  UINT8      addr   = mCursor;

  if ((UINTN)addr + size - 1 > 0xFF) {
    Print(L"\nWrite overflow: addr=0x%02x size=%u\n", addr, size);
    return EFI_INVALID_PARAMETER;
  }

  Status = ReadHexValueNFromKeyboard(digits, &inputVal);
  if (Status == EFI_ABORTED) return EFI_SUCCESS;
  if (EFI_ERROR(Status)) return Status;

  Status = EcSetBank(mBank);
  if (EFI_ERROR(Status)) return Status;

  // write LE bytes
  for (UINTN i = 0; i < size; i++) {
    UINT8 b = (UINT8)((inputVal >> (8 * i)) & 0xFF);
    Status = EcWriteEeprom8((UINT8)(addr + i), b);
    if (EFI_ERROR(Status)) return Status;
  }

  // readback verify + update dump
  for (UINTN i = 0; i < size; i++) {
    UINT8 rb = 0;
    UINT8 expect = (UINT8)((inputVal >> (8 * i)) & 0xFF);

    Status = EcReadEeprom8((UINT8)(addr + i), &rb);
    if (EFI_ERROR(Status)) return Status;

    mDump[addr + i] = rb;

    if (rb != expect) {
      Print(L"\nVerify fail @Bank%u Addr 0x%02x: expect 0x%02x read 0x%02x\n",
            mBank, (UINT8)(addr + i), expect, rb);
      return EFI_DEVICE_ERROR;
    }
  }

  return EFI_SUCCESS;
}

// ---------------- Access toggles ----------------
STATIC
VOID
ApplyProfileForAccess (
  VOID
  )
{
  if (mEc.AccessType == ACCESS_INDEXIO_ENE) {
    // ENE
    mEc.IndexIoBase = 0xFD60;
    mEc.OffIndexHigh= 0x01;
    mEc.OffIndexLow = 0x02;
    mEc.OffData     = 0x03;

    mEc.CmdBuffer             = 0xF98B;
    mEc.DataOfCmdBuffer       = 0xF98C;
    mEc.CmdWriteDataBuffer    = 0xF98D;
    mEc.CmdCntl               = 0xF982;
    mEc.CmdReturnDataBuffer   = 0xF983;

    // Your doc: WriteAddr / ReadAddr / Bank share buffer in this mapping
    mEc.BankBuf               = mEc.CmdWriteDataBuffer;
    mEc.ReadAddrBuf           = mEc.CmdWriteDataBuffer;
    mEc.WriteAddrBuf          = mEc.CmdWriteDataBuffer;
    mEc.WriteDataBuf          = (UINT16)(mEc.CmdWriteDataBuffer + 1);

  } else if (mEc.AccessType == ACCESS_INDEXIO_NUVOTON) {
    // Nuvoton
    mEc.IndexIoBase = 0x0A00;
    mEc.OffIndexHigh= 0x00;
    mEc.OffIndexLow = 0x01;
    mEc.OffData     = 0x02;

    mEc.CmdBuffer             = 0x128B;
    mEc.DataOfCmdBuffer       = 0x128C;
    mEc.CmdWriteDataBuffer    = 0x128D;
    mEc.CmdCntl               = 0x1282;
    mEc.CmdReturnDataBuffer   = 0x1283;

    mEc.BankBuf               = mEc.CmdWriteDataBuffer;
    mEc.ReadAddrBuf           = mEc.CmdWriteDataBuffer;
    mEc.WriteAddrBuf          = mEc.CmdWriteDataBuffer;
    mEc.WriteDataBuf          = (UINT16)(mEc.CmdWriteDataBuffer + 1);

  } else if (mEc.AccessType == ACCESS_INDEXIO_ITE) {
    // ITE (你提供的 Base/EC RAM mapping)
    mEc.IndexIoBase = 0x0D00;
    mEc.OffIndexHigh= 0x01;
    mEc.OffIndexLow = 0x02;
    mEc.OffData     = 0x03;

    mEc.CmdBuffer             = 0xC62B;
    mEc.DataOfCmdBuffer       = 0xC62C;
    mEc.CmdWriteDataBuffer    = 0xC62D;
    mEc.CmdCntl               = 0xC622;
    mEc.CmdReturnDataBuffer   = 0xC623;

    mEc.BankBuf               = mEc.CmdWriteDataBuffer;
    mEc.ReadAddrBuf           = mEc.CmdWriteDataBuffer;
    mEc.WriteAddrBuf          = mEc.CmdWriteDataBuffer;
    mEc.WriteDataBuf          = (UINT16)(mEc.CmdWriteDataBuffer + 1);
  }
}

STATIC
VOID
CycleAccess (
  VOID
  )
{
  if (mEc.AccessType == ACCESS_PORTIO) mEc.AccessType = ACCESS_INDEXIO_ENE;
  else if (mEc.AccessType == ACCESS_INDEXIO_ENE) mEc.AccessType = ACCESS_INDEXIO_NUVOTON;
  else if (mEc.AccessType == ACCESS_INDEXIO_NUVOTON) mEc.AccessType = ACCESS_INDEXIO_ITE;
  else mEc.AccessType = ACCESS_PORTIO;

  ApplyProfileForAccess();
}

// =======================================================
//                       Entry
// =======================================================
EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS    Status;
  EFI_INPUT_KEY Key;

  mAttrDefault = gST->ConOut->Mode->Attribute;

  // Default: PortIO 62/66
  SetMem(&mEc, sizeof(mEc), 0);
  mEc.AccessType = ACCESS_PORTIO;
  mEc.PortMode   = PORTMODE_ACPI_62_66;
  ApplyProfileForAccess();

  SetMem(mDump, sizeof(mDump), 0xFF);

  Status = RefreshDump();
  if (EFI_ERROR(Status)) {
    Print(L"Initial refresh failed: %r\n", Status);
    Print(L"Hint: try ");
    PrintParenGreen(L"F1");
    Print(L"/");
    PrintParenGreen(L"F2");
    Print(L" to switch Port (PortIO), or ");
    PrintParenGreen(L"I");
    Print(L" to switch Access.\n");
    return Status;
  }

  AlignCursorToMode();
  Render();

  while (TRUE) {
    if (EFI_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn, &Key))) continue;

    // ESC
    if (Key.ScanCode == SCAN_ESC) break;

    // Bank switch PgUp/PgDn
    if (Key.ScanCode == SCAN_PAGE_UP) {
      mBank = (mBank == 0) ? EEPROM_BANK_MAX : (UINT8)(mBank - 1);
      Status = RefreshDump();
      AlignCursorToMode();
      Render();
      if (EFI_ERROR(Status)) Print(L"\nSwitch bank failed: %r\n", Status);
      continue;
    }

    if (Key.ScanCode == SCAN_PAGE_DOWN) {
      mBank = (mBank >= EEPROM_BANK_MAX) ? 0 : (UINT8)(mBank + 1);
      Status = RefreshDump();
      AlignCursorToMode();
      Render();
      if (EFI_ERROR(Status)) Print(L"\nSwitch bank failed: %r\n", Status);
      continue;
    }

    // Move
    if (Key.ScanCode == SCAN_UP)    { MoveCursor(-1, 0); Render(); continue; }
    if (Key.ScanCode == SCAN_DOWN)  { MoveCursor(+1, 0); Render(); continue; }
    if (Key.ScanCode == SCAN_LEFT)  { MoveCursor(0, -1); Render(); continue; }
    if (Key.ScanCode == SCAN_RIGHT) { MoveCursor(0, +1); Render(); continue; }

    // TAB: mode
    if (Key.UnicodeChar == CHAR_TAB) {
      CycleDispMode();
      Render();
      continue;
    }

    // F1: PortIO -> 60/64
    if (Key.ScanCode == SCAN_F1) {
      if (mEc.AccessType == ACCESS_PORTIO) {
        mEc.PortMode = PORTMODE_8042_60_64;
        Status = RefreshDump();
        AlignCursorToMode();
        Render();
        if (EFI_ERROR(Status)) Print(L"\nSwitch to 60/64 failed: %r\n", Status);
      } else {
        Render();
        Print(L"\nF1 only works in PortIO mode.\n");
      }
      continue;
    }

    // F2: PortIO -> 62/66
    if (Key.ScanCode == SCAN_F2) {
      if (mEc.AccessType == ACCESS_PORTIO) {
        mEc.PortMode = PORTMODE_ACPI_62_66;
        Status = RefreshDump();
        AlignCursorToMode();
        Render();
        if (EFI_ERROR(Status)) Print(L"\nSwitch to 62/66 failed: %r\n", Status);
      } else {
        Render();
        Print(L"\nF2 only works in PortIO mode.\n");
      }
      continue;
    }

    // I: cycle access backend
    if (Key.UnicodeChar == L'I' || Key.UnicodeChar == L'i') {
      CycleAccess();
      Status = RefreshDump();
      AlignCursorToMode();
      Render();
      if (EFI_ERROR(Status)) Print(L"\nAccess switch refresh failed: %r\n", Status);
      continue;
    }

    // R: refresh
    if (Key.UnicodeChar == L'R' || Key.UnicodeChar == L'r') {
      Status = RefreshDump();
      AlignCursorToMode();
      Render();
      if (EFI_ERROR(Status)) Print(L"\nRefresh failed: %r\n", Status);
      continue;
    }

    // ENTER: write by mode
    if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
      Status = WriteByModeAtCursor();
      Render();
      if (EFI_ERROR(Status)) {
        Print(L"\nWrite failed @Bank%u Addr 0x%02x (size=%u): %r\n",
              mBank, mCursor, (UINTN)mDispMode, Status);
      } else {
        Print(L"\nWrite OK @Bank%u Addr 0x%02x (size=%u)\n",
              mBank, mCursor, (UINTN)mDispMode);
      }
      continue;
    }
  }

  AttrDefault();
  gST->ConOut->ClearScreen(gST->ConOut);
  Print(L"Exit EEPROMECApp.\n");
  return EFI_SUCCESS;
}
