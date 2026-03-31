/** @file
  VerFix.c - Repairs corrupted A_VER and B_VER QSPI partitions.

  VER partition content format (NVIDIA NV4 ASCII text):
    NV4\n
    # R35 , REVISION: X.Y\n
    BOARDID=NNNN BOARDSKU=NNNN FAB=NNN\n
    YYYYMMDDHHMMSS\n
    0xNNNNNN\n
    BYTES:N CRC32:XXXXXXXX\n  <- N = byte count of lines 1-5,
                                  CRC32 computed over those N bytes

  Classification of each partition:
    VALID:      CRC32 in line 6 matches the value computed over lines 1-5.
                No write needed.
    REPAIRABLE: Partition starts with NV4, BYTES:N is present and sane,
                but CRC32 is missing or wrong (e.g. the flash.sh python bug).
                CRC is recomputed from the existing lines 1-5 and a
                corrected line 6 is built.  No external data needed.
    GARBAGE:    Partition does not start with NV4 or has no parseable
                BYTES field — structure is unreadable (e.g. NVDA transport
                header from flash.sh -k --image, or blank/erased partition).

  Self-healing strategy:
    1. Read both A_VER and B_VER, classify and print each.
    2. If both VALID: nothing to write.
    3. If version.txt is present on the ESP: use it for every slot that
       needs repair (REPAIRABLE or GARBAGE), regardless of state.
       Primary path: run WITHOUT version.txt so CRC is self-computed.
       Override path: place version.txt on ESP root to force a specific
       version string (e.g. upgrade, rollback, or both-GARBAGE recovery).
    4. Otherwise self-heal:
         REPAIRABLE -> recompute CRC from existing lines 1-5.
         GARBAGE    -> copy from the other healthy/repaired slot.
         Both GARBAGE with no version.txt -> cannot repair.

  Copyright (c) 2026, Contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/PrintLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/FwPartitionDeviceLib.h>
#include <Protocol/FwPartitionProtocol.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>

/** Global log file on the ESP — written alongside every console Print. */
STATIC EFI_FILE_PROTOCOL  *gLogFile = NULL;

/** Write raw ASCII bytes to the log file (no-op when gLogFile is NULL). */
STATIC
VOID
LogFileWrite (
  IN CONST CHAR8  *Str
  )
{
  UINTN  Len;

  if ((gLogFile == NULL) || (Str == NULL)) {
    return;
  }

  Len = AsciiStrLen (Str);
  if (Len > 0) {
    gLogFile->Write (gLogFile, &Len, (VOID *)Str);
  }
}

/**
  Format and emit a prefixed line to both the UEFI console and the log file.
**/
STATIC
VOID
VerLog (
  IN CONST CHAR16  *Prefix,
  IN CONST CHAR16  *Format,
  ...
  )
{
  VA_LIST  Args;
  CHAR16   WBuf[512];
  CHAR8    ABuf[512];

  VA_START (Args, Format);
  UnicodeVSPrint (WBuf, sizeof (WBuf), Format, Args);
  VA_END (Args);

  Print (L"%s%s", Prefix, WBuf);

  if (gLogFile != NULL) {
    UnicodeStrToAsciiStrS (Prefix, ABuf, sizeof (ABuf));
    LogFileWrite (ABuf);
    UnicodeStrToAsciiStrS (WBuf, ABuf, sizeof (ABuf));
    LogFileWrite (ABuf);
  }
}

/**
  Format and emit a line (no prefix) to both the UEFI console and the log file.
  Used for raw partition content display inside InspectVerPartition.
**/
STATIC
VOID
VerPrint (
  IN CONST CHAR16  *Format,
  ...
  )
{
  VA_LIST  Args;
  CHAR16   WBuf[1024];
  CHAR8    ABuf[1024];

  VA_START (Args, Format);
  UnicodeVSPrint (WBuf, sizeof (WBuf), Format, Args);
  VA_END (Args);

  Print (L"%s", WBuf);

  if (gLogFile != NULL) {
    UnicodeStrToAsciiStrS (WBuf, ABuf, sizeof (ABuf));
    LogFileWrite (ABuf);
  }
}

#define LOG(...)  VerLog (L"[VerFix] ", __VA_ARGS__)
#define ERR(...)  VerLog (L"[VerFix] ERROR: ", __VA_ARGS__)

/** Maximum bytes to read when inspecting a VER partition. */
#define VER_READ_SIZE  512

/** Classification of a VER partition's on-flash content. */
typedef enum {
  VER_VALID,       ///< CRC32 matches — no write needed
  VER_REPAIRABLE,  ///< NV4 structure intact, CRC recomputed and corrected
  VER_GARBAGE,     ///< Unreadable — need content from elsewhere
} VER_STATE;

/**
  Find an ASCII byte pattern in a raw byte buffer.
  Returns pointer to first occurrence, or NULL.
**/
STATIC
CONST CHAR8 *
FindInBuf (
  IN CONST CHAR8  *Buf,
  IN UINTN         BufLen,
  IN CONST CHAR8  *Pattern
  )
{
  UINTN  PatLen;
  UINTN  i;

  PatLen = AsciiStrLen (Pattern);
  if ((PatLen == 0) || (PatLen > BufLen)) {
    return NULL;
  }

  for (i = 0; i + PatLen <= BufLen; i++) {
    if (CompareMem (Buf + i, Pattern, PatLen) == 0) {
      return Buf + i;
    }
  }

  return NULL;
}

/** Parse a decimal UINTN from an ASCII string. **/
STATIC
UINTN
ParseDecimal (
  IN CONST CHAR8  *S,
  IN UINTN         MaxLen
  )
{
  UINTN  Val;
  UINTN  i;

  Val = 0;
  for (i = 0; i < MaxLen && S[i] >= '0' && S[i] <= '9'; i++) {
    Val = Val * 10 + (UINTN)(S[i] - '0');
  }

  return Val;
}

/** Parse up to 8 hex digits into a UINT32. **/
STATIC
UINT32
ParseHex32 (
  IN CONST CHAR8  *S,
  IN UINTN         MaxLen
  )
{
  UINT32  Val;
  UINTN   i;
  CHAR8   C;
  UINT8   Nib;

  Val = 0;
  for (i = 0; i < MaxLen && i < 8; i++) {
    C = S[i];
    if (C >= '0' && C <= '9') {
      Nib = (UINT8)(C - '0');
    } else if (C >= 'A' && C <= 'F') {
      Nib = (UINT8)(C - 'A' + 10);
    } else if (C >= 'a' && C <= 'f') {
      Nib = (UINT8)(C - 'a' + 10);
    } else {
      break;
    }

    Val = (Val << 4) | Nib;
  }

  return Val;
}

/**
  Read and inspect a VER partition.  Prints content and diagnostic info.

  Classification:
    VER_VALID:      CRC32 matches — *OutBuf = raw read buffer, *OutLen = TextLen.
    VER_REPAIRABLE: NV4 + BYTES sane, CRC wrong/missing — *OutBuf = newly
                    allocated corrected buffer (lines 1-5 + recomputed line 6),
                    *OutLen = size of that buffer.
    VER_GARBAGE:    No parseable NV4 structure — *OutBuf = NULL, *OutLen = 0.

  Caller must FreePool(*OutBuf) when it is non-NULL.
**/
STATIC
VER_STATE
InspectVerPartition (
  IN  NVIDIA_FW_PARTITION_PROTOCOL  *Protocol,
  IN  CONST CHAR16                  *PartitionName,
  OUT VOID                          **OutBuf,
  OUT UINTN                         *OutLen
  )
{
  EFI_STATUS              Status;
  FW_PARTITION_ATTRIBUTES Attrs;
  UINT8                   *Buf;
  UINTN                   ReadSize;
  UINTN                   TextLen;
  UINTN                   i;
  CONST CHAR8             *BytesPtr;
  CONST CHAR8             *Crc32Ptr;
  UINTN                   BytesVal;
  UINT32                  StoredCrc;
  UINT32                  ComputedCrc;
  CHAR16                  *WBuf;
  CHAR8                   Line6[48];
  UINTN                   Line6Len;
  UINT8                   *NewBuf;

  *OutBuf = NULL;
  *OutLen = 0;

  Status = Protocol->GetAttributes (Protocol, &Attrs);
  if (EFI_ERROR (Status)) {
    LOG (L"  %s: GetAttributes failed: %r\n", PartitionName, Status);
    return VER_GARBAGE;
  }

  ReadSize = MIN (VER_READ_SIZE, Attrs.Bytes);
  Buf      = AllocateZeroPool (ReadSize);
  if (Buf == NULL) {
    return VER_GARBAGE;
  }

  Status = Protocol->Read (Protocol, 0, ReadSize, Buf);
  if (EFI_ERROR (Status)) {
    LOG (L"  %s: Read failed: %r\n", PartitionName, Status);
    FreePool (Buf);
    return VER_GARBAGE;
  }

  //
  // Find text length: NOR flash erases to 0xFF, so the first 0xFF byte
  // marks the end of the actual content.
  //
  TextLen = 0;
  for (i = 0; i < ReadSize; i++) {
    if (Buf[i] == 0xFF) {
      break;
    }

    TextLen = i + 1;
  }

  //
  // Print raw content as Unicode so it renders on the UEFI console.
  //
  LOG (L"--- %s (%lu bytes of text) ---\n", PartitionName, (UINT64)TextLen);
  if (TextLen == 0) {
    VerPrint (L"  <empty / erased>\r\n");
    FreePool (Buf);
    return VER_GARBAGE;
  }

  //
  // Allocate 2x space: worst case every \n becomes \r\n.
  // UEFI console requires CR+LF; bare LF advances the row but not
  // the column, producing a staircase effect.
  //
  WBuf = AllocatePool ((TextLen * 2 + 2) * sizeof (CHAR16));
  if (WBuf != NULL) {
    UINTN  WLen = 0;

    for (i = 0; i < TextLen; i++) {
      if (Buf[i] == '\n') {
        WBuf[WLen++] = L'\r';
        WBuf[WLen++] = L'\n';
      } else {
        WBuf[WLen++] = (CHAR16)Buf[i];
      }
    }

    WBuf[WLen] = L'\0';
    VerPrint (L"  %s", WBuf);
    //
    // Ensure cursor is at column 0 before the status line.
    //
    if ((WLen == 0) || (WBuf[WLen - 1] != L'\n')) {
      VerPrint (L"\r\n");
    }

    FreePool (WBuf);
  }

  //
  // The partition MUST start with "NV4\n" at offset 0, exactly as UEFI's
  // VerPartitionGetVersion() requires.  A different prefix (e.g. the
  // NVDA transport header from flash.sh -k --image) means GARBAGE.
  //
  if ((TextLen < 4) || (CompareMem (Buf, "NV4\n", 4) != 0)) {
    LOG (L"  %s: does not start with NV4 -- [GARBAGE]\n", PartitionName);
    FreePool (Buf);
    return VER_GARBAGE;
  }

  //
  // Parse BYTES:N  (byte count of lines 1-5)
  //
  BytesPtr = FindInBuf ((CONST CHAR8 *)Buf, TextLen, "BYTES:");
  if (BytesPtr == NULL) {
    LOG (L"  %s: BYTES: field not found -- [GARBAGE]\n", PartitionName);
    FreePool (Buf);
    return VER_GARBAGE;
  }

  BytesVal = ParseDecimal (BytesPtr + 6, 10);

  if ((BytesVal == 0) || (BytesVal > TextLen)) {
    LOG (L"  %s: BYTES:%lu out of range (TextLen=%lu) -- [GARBAGE]\n",
         PartitionName, (UINT64)BytesVal, (UINT64)TextLen);
    FreePool (Buf);
    return VER_GARBAGE;
  }

  //
  // Parse CRC32:XXXXXXXX for display (may be empty/absent if line 6 is corrupt).
  //
  Crc32Ptr  = FindInBuf ((CONST CHAR8 *)Buf, TextLen, "CRC32:");
  StoredCrc = (Crc32Ptr != NULL) ? ParseHex32 (Crc32Ptr + 6, 8) : 0;

  //
  // Compute CRC32 over lines 1-5 (exactly BytesVal bytes from offset 0).
  //
  ComputedCrc = 0;
  Status      = gBS->CalculateCrc32 (Buf, BytesVal, &ComputedCrc);
  if (EFI_ERROR (Status)) {
    LOG (L"  %s: CalculateCrc32 failed: %r -- [GARBAGE]\n", PartitionName, Status);
    FreePool (Buf);
    return VER_GARBAGE;
  }

  if (StoredCrc == ComputedCrc) {
    //
    // CRC matches — partition is healthy.
    //
    LOG (
      L"  %s: BYTES=%-5lu  Stored CRC32=0x%08X  Computed CRC32=0x%08X  [VALID]\n",
      PartitionName,
      (UINT64)BytesVal,
      StoredCrc,
      ComputedCrc
      );
    *OutBuf = Buf;
    *OutLen = TextLen;
    return VER_VALID;
  }

  //
  // CRC is wrong or missing but NV4 + BYTES are sane — REPAIRABLE.
  // Build a corrected buffer: lines 1-5 (BytesVal bytes) plus a new line 6
  // containing the recomputed CRC.
  //
  LOG (
    L"  %s: BYTES=%-5lu  Stored CRC32=0x%08X  Computed CRC32=0x%08X  [REPAIRABLE]\n",
    PartitionName,
    (UINT64)BytesVal,
    StoredCrc,
    ComputedCrc
    );

  Line6Len = AsciiSPrint (Line6, sizeof (Line6), "BYTES:%lu CRC32:%08X\n",
                          (UINT64)BytesVal, ComputedCrc);

  NewBuf = AllocatePool (BytesVal + Line6Len);
  if (NewBuf == NULL) {
    FreePool (Buf);
    return VER_GARBAGE;
  }

  CopyMem (NewBuf, Buf, BytesVal);
  CopyMem (NewBuf + BytesVal, Line6, Line6Len);
  FreePool (Buf);

  *OutBuf = NewBuf;
  *OutLen = BytesVal + Line6Len;
  return VER_REPAIRABLE;
}

/**
  Write a buffer to a VER partition.
  Handles write-protect bypass for the active slot (blocked by FwPartitionDeviceLib).
**/
STATIC
EFI_STATUS
WriteVerPartition (
  IN NVIDIA_FW_PARTITION_PROTOCOL  *Protocol,
  IN CONST CHAR16                  *PartitionName,
  IN VOID                          *Buffer,
  IN UINTN                         BufferSize
  )
{
  EFI_STATUS              Status;
  FW_PARTITION_ATTRIBUTES Attrs;

  Status = Protocol->GetAttributes (Protocol, &Attrs);
  if (EFI_ERROR (Status)) {
    ERR (L"GetAttributes on %s failed: %r\n", PartitionName, Status);
    return Status;
  }

  LOG (L"  %s: partition %lu bytes, BlockSize %u\n",
       PartitionName, (UINT64)Attrs.Bytes, Attrs.BlockSize);

  if (BufferSize > Attrs.Bytes) {
    ERR (L"  Content (%lu bytes) exceeds %s partition (%lu bytes)\n",
         (UINT64)BufferSize, PartitionName, (UINT64)Attrs.Bytes);
    return EFI_BAD_BUFFER_SIZE;
  }

  Status = Protocol->Write (Protocol, 0, BufferSize, Buffer);
  if (Status == EFI_WRITE_PROTECTED) {
    //
    // Active-partition writes are blocked by mOverwriteActiveFwPartition==FALSE
    // inside FwPartitionDeviceLib. Bypass the guard by calling DeviceWrite()
    // directly through the private struct (signature-checked for safety).
    //
    FW_PARTITION_PRIVATE_DATA  *Private;

    LOG (L"  %s: write-protected (active slot) -- bypassing\n", PartitionName);
    Private = BASE_CR (Protocol, FW_PARTITION_PRIVATE_DATA, Protocol);
    if (Private->Signature == FW_PARTITION_PRIVATE_DATA_SIGNATURE) {
      Status = Private->DeviceInfo->DeviceWrite (
                                     Private->DeviceInfo,
                                     Private->PartitionInfo.Offset,
                                     BufferSize,
                                     Buffer
                                     );
    } else {
      ERR (L"  %s: unexpected struct signature -- cannot bypass\n", PartitionName);
      return EFI_WRITE_PROTECTED;
    }
  }

  if (EFI_ERROR (Status)) {
    ERR (L"  Write to %s failed: %r\n", PartitionName, Status);
    return Status;
  }

  LOG (L"  %s: write OK\n", PartitionName);
  return EFI_SUCCESS;
}

/**
  Read version.txt from the same filesystem this EFI was loaded from.
  Used as last-resort fallback when both VER partitions are corrupt.
  Caller must FreePool(*Buffer) on success.
**/
STATIC
EFI_STATUS
ReadVersionFile (
  IN  EFI_HANDLE  ImageHandle,
  OUT VOID        **Buffer,
  OUT UINTN       *BufferSize
  )
{
  EFI_STATUS                       Status;
  EFI_LOADED_IMAGE_PROTOCOL        *LoadedImage;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;
  UINT8                            InfoBuf[sizeof (EFI_FILE_INFO) + 256];
  EFI_FILE_INFO                    *FileInfo;
  UINTN                            InfoSize;
  VOID                             *Data;
  UINTN                            DataSize;

  Status = gBS->HandleProtocol (
                  ImageHandle,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&LoadedImage
                  );
  if (EFI_ERROR (Status)) {
    ERR (L"Cannot get LoadedImageProtocol: %r\n", Status);
    return Status;
  }

  Status = gBS->HandleProtocol (
                  LoadedImage->DeviceHandle,
                  &gEfiSimpleFileSystemProtocolGuid,
                  (VOID **)&FileSystem
                  );
  if (EFI_ERROR (Status)) {
    ERR (L"No filesystem on loader device (version.txt unavailable): %r\n", Status);
    return Status;
  }

  Status = FileSystem->OpenVolume (FileSystem, &Root);
  if (EFI_ERROR (Status)) {
    ERR (L"OpenVolume failed: %r\n", Status);
    return Status;
  }

  Status = Root->Open (Root, &File, L"version.txt", EFI_FILE_MODE_READ, 0);
  Root->Close (Root);
  if (EFI_ERROR (Status)) {
    ERR (L"Cannot open version.txt: %r\n", Status);
    return Status;
  }

  InfoSize = sizeof (InfoBuf);
  FileInfo = (EFI_FILE_INFO *)InfoBuf;
  Status   = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, FileInfo);
  if (EFI_ERROR (Status)) {
    ERR (L"GetInfo failed: %r\n", Status);
    File->Close (File);
    return Status;
  }

  DataSize = (UINTN)FileInfo->FileSize;
  if (DataSize == 0) {
    ERR (L"version.txt is empty\n");
    File->Close (File);
    return EFI_INVALID_PARAMETER;
  }

  LOG (L"version.txt: %lu bytes\n", (UINT64)DataSize);

  Data = AllocatePool (DataSize);
  if (Data == NULL) {
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }

  Status = File->Read (File, &DataSize, Data);
  File->Close (File);
  if (EFI_ERROR (Status)) {
    ERR (L"Read version.txt failed: %r\n", Status);
    FreePool (Data);
    return Status;
  }

  *Buffer     = Data;
  *BufferSize = DataSize;
  return EFI_SUCCESS;
}

/**
  Open (or create) VerFix.log on the same ESP as the running EFI image.
  Each run truncates the previous log so output is always from the latest run.
  Silently skips on any error — log output remains console-only.
**/
STATIC
VOID
OpenLogFile (
  IN EFI_HANDLE  ImageHandle
  )
{
  EFI_STATUS                       Status;
  EFI_LOADED_IMAGE_PROTOCOL        *LoadedImage;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *OldLog;

  Status = gBS->HandleProtocol (
                  ImageHandle,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&LoadedImage
                  );
  if (EFI_ERROR (Status)) {
    return;
  }

  Status = gBS->HandleProtocol (
                  LoadedImage->DeviceHandle,
                  &gEfiSimpleFileSystemProtocolGuid,
                  (VOID **)&FileSystem
                  );
  if (EFI_ERROR (Status)) {
    return;
  }

  Status = FileSystem->OpenVolume (FileSystem, &Root);
  if (EFI_ERROR (Status)) {
    return;
  }

  //
  // Delete any previous log so each run starts with a clean file.
  //
  Status = Root->Open (Root, &OldLog, L"VerFix.log",
                       EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
  if (!EFI_ERROR (Status)) {
    OldLog->Delete (OldLog);
  }

  Status = Root->Open (
                   Root,
                   &gLogFile,
                   L"VerFix.log",
                   EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                   0
                   );
  Root->Close (Root);

  if (EFI_ERROR (Status)) {
    gLogFile = NULL;
  }
}

/**
  Application entry point.
**/
EFI_STATUS
EFIAPI
VerFixEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                    Status;
  EFI_HANDLE                    *Handles    = NULL;
  UINTN                         HandleCount = 0;
  UINTN                         Index;
  NVIDIA_FW_PARTITION_PROTOCOL  *FwPartition;
  CONST CHAR16                  *PartName;
  NVIDIA_FW_PARTITION_PROTOCOL  *ProtoA     = NULL;
  NVIDIA_FW_PARTITION_PROTOCOL  *ProtoB     = NULL;
  CONST CHAR16                  *NameA      = NULL;
  CONST CHAR16                  *NameB      = NULL;
  VER_STATE                     StateA;
  VER_STATE                     StateB;
  VOID                          *BufA       = NULL;  // VALID: raw; REPAIRABLE: corrected
  VOID                          *BufB       = NULL;
  UINTN                         LenA        = 0;
  UINTN                         LenB        = 0;
  VOID                          *VersionBuf = NULL;
  UINTN                         VersionLen  = 0;
  VOID                          *WriteA     = NULL;
  VOID                          *WriteB     = NULL;
  UINTN                         WLenA       = 0;
  UINTN                         WLenB       = 0;
  BOOLEAN                       NeedA;
  BOOLEAN                       NeedB;
  BOOLEAN                       Repaired    = FALSE;

  //
  // Open VerFix.log on the ESP for this run (best-effort; failures are silent).
  //
  OpenLogFile (ImageHandle);

  LOG (L"==============================================\n");
  LOG (L"VER Partition Repair Utility\n");
  LOG (L"==============================================\n");

  //
  // Step 1: Enumerate all FwPartition handles, locate A_VER and B_VER.
  //
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gNVIDIAFwPartitionProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    ERR (L"No FwPartition handles found (%r) -- FwPartitionDxe not running?\n", Status);
    return Status;
  }

  LOG (L"Found %lu FwPartition handles\n", (UINT64)HandleCount);

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gNVIDIAFwPartitionProtocolGuid,
                    (VOID **)&FwPartition
                    );
    if (EFI_ERROR (Status) || (FwPartition == NULL)) {
      continue;
    }

    PartName = FwPartition->PartitionName;

    if ((StrCmp (PartName, L"A_VER") == 0) || (StrCmp (PartName, L"VER") == 0)) {
      ProtoA = FwPartition;
      NameA  = PartName;
    } else if ((StrCmp (PartName, L"B_VER") == 0) || (StrCmp (PartName, L"VER_b") == 0)) {
      ProtoB = FwPartition;
      NameB  = PartName;
    }
  }

  FreePool (Handles);

  if ((ProtoA == NULL) || (ProtoB == NULL)) {
    ERR (
      L"Could not find both VER partitions (A=%s B=%s)\n",
      (NameA != NULL) ? NameA : L"<missing>",
      (NameB != NULL) ? NameB : L"<missing>"
      );
    return EFI_NOT_FOUND;
  }

  //
  // Step 2: Read, classify, and (if REPAIRABLE) produce corrected buffers.
  //
  LOG (L"\n");
  StateA = InspectVerPartition (ProtoA, NameA, &BufA, &LenA);
  LOG (L"\n");
  StateB = InspectVerPartition (ProtoB, NameB, &BufB, &LenB);
  LOG (L"\n");

  //
  // Step 3: Nothing to write if both are already valid.
  //
  if ((StateA == VER_VALID) && (StateB == VER_VALID)) {
    LOG (L"Both VER partitions are VALID -- no repair needed.\n");
    FreePool (BufA);
    FreePool (BufB);
    return EFI_SUCCESS;
  }

  //
  // Step 4: Determine write content for each slot.
  //
  NeedA = (StateA != VER_VALID);
  NeedB = (StateB != VER_VALID);

  //
  // Optional override: try to read version.txt from the ESP immediately.
  // If present, it is used as the write content for every slot that needs
  // repair, regardless of state (REPAIRABLE or GARBAGE).  This lets an
  // operator force a specific version string (e.g. after an upgrade or
  // rollback) or recover both-GARBAGE situations without a full reflash.
  //
  // Primary / recommended path: run WITHOUT version.txt so that VerFix
  // self-heals by recomputing the CRC from the on-flash NV4 data.
  //
  Status = ReadVersionFile (ImageHandle, &VersionBuf, &VersionLen);
  if (!EFI_ERROR (Status)) {
    LOG (L"version.txt found (%lu bytes) -- using it for all slots needing repair\n",
         (UINT64)VersionLen);
    WriteA = NeedA ? VersionBuf : NULL;
    WLenA  = NeedA ? VersionLen : 0;
    WriteB = NeedB ? VersionBuf : NULL;
    WLenB  = NeedB ? VersionLen : 0;
  } else {
    //
    // No version.txt -- self-heal:
    //   REPAIRABLE: BufA/BufB already holds the corrected buffer.
    //   GARBAGE:    copy from the other healthy/repaired slot.
    //   Both GARBAGE with no version.txt: cannot repair.
    //
    WriteA = BufA;   // NULL when GARBAGE
    WriteB = BufB;
    WLenA  = LenA;
    WLenB  = LenB;

    if (NeedA && (WriteA == NULL) && (BufB != NULL)) {
      WriteA = BufB;
      WLenA  = LenB;
    }

    if (NeedB && (WriteB == NULL) && (BufA != NULL)) {
      WriteB = BufA;
      WLenB  = LenA;
    }

    if ((NeedA && (WriteA == NULL)) || (NeedB && (WriteB == NULL))) {
      ERR (L"Both partitions GARBAGE and no version.txt -- no repair possible\n");
      ERR (L"Place a valid version.txt on the ESP root and re-run VerFix\n");
      Status = EFI_NOT_FOUND;
      goto Cleanup;
    }
  }

  //
  // Step 5: Perform writes.
  //   REPAIRABLE slots receive their own recomputed content.
  //   GARBAGE slots receive content from the donor.
  //   VALID slots are not written.
  //
  if (NeedA) {
    LOG (L"%s %s%s\n",
         (StateA == VER_REPAIRABLE) ? L"Repairing" : L"Restoring",
         NameA,
         (VersionBuf != NULL) ? L" (from version.txt)" : L"");
    Status = WriteVerPartition (ProtoA, NameA, WriteA, WLenA);
    if (EFI_ERROR (Status)) {
      goto Cleanup;
    }

    Repaired = TRUE;
  }

  if (NeedB) {
    LOG (L"%s %s%s\n",
         (StateB == VER_REPAIRABLE) ? L"Repairing" : L"Restoring",
         NameB,
         (VersionBuf != NULL) ? L" (from version.txt)" : L"");
    Status = WriteVerPartition (ProtoB, NameB, WriteB, WLenB);
    if (EFI_ERROR (Status)) {
      goto Cleanup;
    }

    Repaired = TRUE;
  }

  if (Repaired) {
    LOG (L"==============================================\n");
    LOG (L"VER repair completed successfully!\n");
    LOG (L"System will boot normally on next start.\n");
    LOG (L"==============================================\n");
  }

Cleanup:
  if (BufA != NULL) {
    FreePool (BufA);
  }

  if (BufB != NULL) {
    FreePool (BufB);
  }

  if (VersionBuf != NULL) {
    FreePool (VersionBuf);
  }

  //
  // Flush and close the log file.
  //
  if (gLogFile != NULL) {
    gLogFile->Close (gLogFile);
    gLogFile = NULL;
  }

  return Status;
}
