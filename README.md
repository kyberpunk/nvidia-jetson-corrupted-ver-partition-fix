# VER Partition Bugfix — `VerFix.efi`

Fixes a silent corruption of the `A_VER` / `B_VER` QSPI partitions on Jetson
modules that store their bootloader on **QSPI NOR flash** — specifically:

| Module | SoC | QSPI bootloader |
|--------|-----|-----------------|
| **Jetson AGX Orin** | T234 | ✅ Yes |
| **Jetson Orin NX** | T234 | ✅ Yes |
| Jetson Orin Nano | T234 | ✅ Yes |
| Jetson AGX Xavier / Xavier NX | T194 | ❌ Different layout — not applicable |

The bug causes all OTA capsule updates to be permanently rejected with
`LastAttemptStatus: 6162 (LAS_ERROR_FMP_LIB_UNINITIALIZED)`.

## Motivation

Once a Jetson device is deployed to the field, the primary path for updating
the bootloader (and OS) is an **OTA capsule update** delivered over the network.
When the VER partitions are corrupted, this mechanism is completely blocked —
the UEFI FMP library refuses to accept any capsule, so the device is stuck on
its current firmware version indefinitely.

The obvious recovery path — **re-flashing the device** — is impractical in
most field deployments:

- It requires a USB connection (or a UART console + physical QSPI programmer)
  directly to the device.
- Field devices are typically installed in enclosures, vehicles, or remote
  locations where hands-on access is costly and slow.
- Coordinating a physical recall or a field engineer visit is expensive and
  introduces significant downtime.

**Repairing from the running Linux OS is not an option either.** The QSPI NOR
flash that holds the VER partitions is managed exclusively by the UEFI/TF-A
firmware stack; the Linux kernel does not expose a writable interface to it from
userspace on T234 platforms. There is no `/dev/mtd*` or equivalent device node
that a shell script could use to patch the partition in place.

`VerFix.efi` is the workaround: a small UEFI Shell application that runs
*before* the OS boots, repairs both VER partitions entirely within UEFI where
QSPI access is available, and then hands off to the normal boot path — restoring
OTA capability without any physical intervention.

---

> ⚠️ **Disclaimer**
>
> The `VerFix.efi` application in this repository was **generated with AI
> assistance** and has been tested on a limited set of hardware configurations.
> It may not cover all edge cases. Use it at your own risk.
>
> Writing to QSPI NOR flash partitions carries an inherent risk of rendering
> the device unbootable if the written content is incorrect or the process is
> interrupted. **The authors and contributors accept no liability** for any
> damage to devices, data loss, or any other consequences arising from the use
> of this software.
>
> Always back up any critical data and verify the partition content after repair
> before relying on the device in production.

---

## Contents

| File | Purpose |
|------|---------|
| `VerFix.c` | UEFI application source |
| `VerFix.inf` | EDK2 module descriptor |
| `VerFix.efi` | Pre-built AArch64 RELEASE binary (44 KB) — built for **L4T R35.4.1 / JetPack 5.1.3** (edk2-nvidia `r35.4.1+4`, commit `52cefd45`) |
| `startup.nsh` | UEFI Shell auto-run script |

---

## 1. Background

The `A_VER` and `B_VER` partitions sit on the **QSPI NOR flash** (not on eMMC
or NVMe). They are present on all T234-based Orin modules (AGX Orin, Orin NX,
Orin Nano). Each holds a small **NV4 ASCII** file that UEFI reads at every boot
to identify the firmware version and validate capsule update eligibility:

```
NV4
# R35 , REVISION: 4.1
BOARDID=3701 BOARDSKU=0004 FAB=500
20260330130359
0x230401
BYTES:85 CRC32:A10A2457
```

Line 6 carries the byte count of lines 1–5 and the CRC32 computed over them.
UEFI parses this via `VerPartitionGetVersion()` in `VerPartitionLib.c`. If the
CRC does not match, `mFmpLibInitialized` is never set to `TRUE` and every
subsequent capsule update attempt returns:

```
FmpTegraCheckImage: FMP library not initialized
SetTheImage() LastAttemptStatus: 6162
```

---

## 2. Root Cause — `flash.sh` CRC Bug and UEFI Failure Chain

| Property | Value |
|----------|---------|
| File | `Linux_for_Tegra/flash.sh` |
| Line | 3076 |
| Effect | CRC32 field is silently written as empty; UEFI reads `0` instead of the real CRC |

### How the partition gets corrupted

The VER partition is written by `flash.sh` during the full device flash. A
combination of environment issues on the flash host (such as a missing `python`
interpreter or a `zlib` import failure) can silently cause the CRC32 computation
to return an empty string with **no error and no non-zero exit code**. The
partition is then written to QSPI with a blank CRC field:

```
BYTES:85 CRC32:
```

The key symptom is the **empty value after `CRC32:`**. Both the A and B slots
are typically corrupted at the same time because `flash.sh` writes them both
in the same pass.

### Why OTA (capsule update) fails

At boot, before any capsule update is attempted, the UEFI Firmware Management
Protocol (FMP) library reads the VER partition to establish the current firmware
version. The failure propagates through three layers:

**Layer 1 — CRC parse (`VerPartitionLib.c :: VerPartitionGetVersion`)**

`AsciiStrHexToUintn("")` silently returns `0` when given an empty string.
The expected CRC is therefore stored as `0`, while the actual CRC computed over
the partition content is a non-zero value (e.g., `0xA10A2457`). The mismatch
causes the function to return `EFI_VOLUME_CORRUPTED` and abort.

**Layer 2 — Version initialisation (`TegraFmp.c :: GetVersionInfo`)**

`VerPartitionGetVersion()` failed, so `GetVersionInfo` jumps to its `Done`
label without setting `mFmpLibInitialized = TRUE`. The FMP library remains
in an uninitialised state for the entire boot.

**Layer 3 — Capsule gate (`TegraFmp.c :: FmpTegraCheckImage`)**

Every capsule update is pre-validated by `FmpTegraCheckImage`. Its very first
check is:

```c
if (!mFmpLibInitialized) {
    *LastAttemptStatus = LAS_ERROR_FMP_LIB_UNINITIALIZED;  // 6162
    return EFI_NOT_READY;
}
```

Because `mFmpLibInitialized` is still `FALSE`, the capsule is rejected before
any image validation even begins.

**Annotated call trace**

```
VerPartitionLib.c :: VerPartitionGetVersion()
  AsciiStrHexToUintn("") → 0      ← empty string parsed as 0
  CrcExpected = 0
  CrcReceived = 0xA10A2457        ← actual CRC over lines 1-5
  0 ≠ 0xA10A2457  →  EFI_VOLUME_CORRUPTED

TegraFmp.c :: GetVersionInfo()
  VerPartitionGetVersion() failed  →  goto Done
  mFmpLibInitialized stays FALSE

TegraFmp.c :: FmpTegraCheckImage()
  if (!mFmpLibInitialized)
      *LastAttemptStatus = 6162   ← LAS_ERROR_FMP_LIB_UNINITIALIZED
      return EFI_NOT_READY
```

> ⚠️ **Brick risk:** The UEFI update flow writes the new OS image to the
> inactive slot *before* attempting to apply the bootloader capsule. If the
> capsule is then rejected (as above), the device is left with a new OS image
> but an old, incompatible bootloader — a mismatch that can prevent the device
> from booting at all.

Existing corrupted devices need `VerFix.efi` (see Section 5).

---

## 3. Diagnosing a Device

### From running Linux

```bash
# 6162 = LAS_ERROR_FMP_LIB_UNINITIALIZED
cat /sys/firmware/efi/esrt/entries/entry0/last_attempt_status
```

### Read the raw partition content

```bash
VER_DEV=$(blkid -L A_VER 2>/dev/null)
sudo dd if="${VER_DEV}" bs=128 count=1 2>/dev/null | strings
```

| Last line | Meaning |
|-----------|---------|
| `BYTES:85 CRC32:A10A2457` | Healthy — 8 hex digits present |
| `BYTES:85 CRC32:` | **Corrupted** — CRC field empty |
| Does not start with `NV4` | **GARBAGE** — e.g. NVDA transport header from `flash.sh -k --image` |

### UEFI serial log

```
FmpTegraCheckImage: FMP library not initialized
FmpDxe(NVIDIA System Firmware): SetTheImage() LastAttemptStatus: 6162.
```

---

## 4. Reproducing the Bug

Use these steps to verify that a flash host is affected before deploying to
additional devices.

1. **Confirm `python` is absent** on the host:

   ```bash
   python --version   # expected: command not found
   which python3      # python3 must be present
   ```

2. **Remove any cached VER file** (optional, ensures a fresh write):

   ```bash
   rm -f Linux_for_Tegra/bootloader/qspi_bootblob_ver.txt
   ```

3. **Run the full flash**:

   ```bash
   sudo ./flash.sh <board-config> internal
   # Completes with exit 0; no error is printed — the bug is silent
   ```

4. **Read `A_VER` on the device**:

   ```bash
   VER_DEV=$(blkid -L A_VER 2>/dev/null)
   sudo dd if="${VER_DEV}" bs=128 count=1 2>/dev/null | strings
   # Last line: BYTES:85 CRC32:   ← blank = corrupted
   ```

5. **Confirm OTA is blocked**:

   ```bash
   cat /sys/firmware/efi/esrt/entries/entry0/last_attempt_status
   # 6162
   ```

---

## 5. Fix — VerFix.efi

`VerFix.efi` is a UEFI Shell application that classifies and repairs both VER
partitions automatically, then chainloads the normal Linux boot — no USB
recovery and no full reflash needed.

### How it classifies each partition

| State | Condition | Action |
|-------|-----------|--------|
| **VALID** | CRC32 matches computed value | No write |
| **REPAIRABLE** | NV4 structure intact; CRC wrong or missing | Recompute CRC from lines 1–5; write corrected line 6 |
| **GARBAGE** | Does not start with `NV4` at offset 0 | Copy content from the other healthy/repaired slot |

If **both** slots are GARBAGE and no `version.txt` is present on the ESP, the
tool reports an error. Provide a `version.txt` (see Section 5.2) to recover.

### 5.1 Install on the device

On the **flash host**, copy the files to the device:

```bash
scp VerFix.efi startup.nsh orin@<DEVICE_IP>:/home/orin/
```

On the **target device** (via SSH):

```bash
sudo mkdir -p /esp_mount
sudo mount /dev/mmcblk0p10 /esp_mount
sudo cp /home/orin/VerFix.efi /esp_mount/VerFix.efi
sudo cp /home/orin/startup.nsh /esp_mount/startup.nsh
sudo umount /esp_mount
```

> **Choosing the correct filesystem handle in `startup.nsh`**
>
> The UEFI Shell assigns `FS0:`, `FS1:`, `FS2:` … to each storage device it
> enumerates. The ESP (EFI System Partition) handle varies by module:
>
> | Module | Typical ESP handle |
> |--------|-----------------|
> | Jetson AGX Orin (eMMC) | `FS3:` |
> | Jetson Orin NX / Orin Nano (eMMC) | `FS2:` or `FS3:` |
> | Any module (NVMe or USB boot) | may differ |
>
> The pre-supplied `startup.nsh` uses `FS3:`. If `VerFix.efi` is not found
> when you boot into the UEFI Shell, determine the correct handle manually:
>
> ```
> Shell> map -r
> Shell> fs2:
> FS2:\> ls
> # look for the EFI\ directory — that handle is the ESP
> ```
>
> Then edit `startup.nsh` on the ESP to replace `fs3:` with the correct handle
> before rebooting into the shell again:
>
> ```bash
> sudo mount /dev/mmcblk0p10 /esp_mount
> sudo sed -i 's/^fs3:/fs2:/' /esp_mount/startup.nsh
> sudo umount /esp_mount
> ```

### 5.2 version.txt (optional override)

By default `VerFix.efi` self-heals without any external file. If `version.txt`
is present on the ESP root, it is read immediately and used as the write content
for **all slots that need repair** — regardless of their state on QSPI. Remove
it after use:

```bash
# Generate version.txt programmatically (adjust board values):
python3 - <<'EOF'
import zlib, datetime
BOARDID, BOARDSKU, FAB = '3701', '0004', '500'
BSP_BRANCH, BSP_MAJOR, BSP_MINOR = '35', '4', '1'
branch, major, minor = int(BSP_BRANCH), int(BSP_MAJOR), int(BSP_MINOR)
ver32 = '0x%x' % ((branch << 16) | (major << 8) | minor)
ts = datetime.datetime.now().strftime('%Y%m%d%H%M%S')
content = (f'NV4\n# R{BSP_BRANCH} , REVISION: {BSP_MAJOR}.{BSP_MINOR}\n'
           f'BOARDID={BOARDID} BOARDSKU={BOARDSKU} FAB={FAB}\n{ts}\n{ver32}\n')
data = content.encode('ascii')
crc32 = zlib.crc32(data) & 0xFFFFFFFF
open('version.txt', 'w').write(content + f'BYTES:{len(data)} CRC32:{crc32:X}\n')
print('Done')
EOF
scp version.txt orin@<DEVICE_IP>:/home/orin/
ssh orin@<DEVICE_IP> 'sudo mount /dev/mmcblk0p10 /esp_mount && \
    sudo cp /home/orin/version.txt /esp_mount/version.txt && \
    sudo umount /esp_mount'
```

> ⚠️ **Always remove `version.txt` from the ESP after repair.** Leaving it in
> place means every subsequent VerFix run will use it instead of self-healing,
> even when both partitions are already correct.

### 5.3 Boot into the UEFI Shell

```bash
sudo apt install efibootmgr          # install once if not already present
efibootmgr | grep -i shell           # find the UEFI Shell boot entry
sudo efibootmgr --bootnext 0003      # set one-time next boot (adjust number)
sudo reboot
```

`startup.nsh` runs automatically. `VerFix.efi` repairs the partition, then the
normal Linux boot resumes.

### 5.4 Expected output (REPAIRABLE case — no version.txt)

```
[VerFix] ==============================================
[VerFix] VER Partition Repair Utility
[VerFix] ==============================================
[VerFix] Found 50 FwPartition handles

[VerFix] --- A_VER (110 bytes of text) ---
  NV4
  # R35 , REVISION: 4.1
  BOARDID=3701 BOARDSKU=0004 FAB=500
  20260330130359
  0x230401
  BYTES:85 CRC32:
[VerFix]   A_VER: BYTES=85   Stored CRC32=0x00000000  Computed CRC32=0xA10A2457  [REPAIRABLE]

[VerFix] --- B_VER (110 bytes of text) ---
  ...
[VerFix]   B_VER: BYTES=85   Stored CRC32=0xA10A2457  Computed CRC32=0xA10A2457  [VALID]

[VerFix] Repairing A_VER
[VerFix]   A_VER: write OK
[VerFix] ==============================================
[VerFix] VER repair completed successfully!
[VerFix] ==============================================
```

If both partitions were already valid on a subsequent run:

```
[VerFix] Both VER partitions are VALID -- no repair needed.
```

### 5.5 Retrieve the run log

`VerFix.efi` writes a copy of every console line to `VerFix.log` on the ESP
root. Retrieve it from the booted Linux system:

```bash
sudo mount /dev/mmcblk0p10 /esp_mount
cat /esp_mount/VerFix.log
sudo umount /esp_mount
```

The log is recreated fresh on each run. Useful when the UEFI serial console is
unavailable.

### 5.6 Validate and re-run OTA

```bash
VER_DEV=$(blkid -L A_VER 2>/dev/null)
sudo dd if="${VER_DEV}" bs=128 count=1 2>/dev/null | strings
# Last line must be:  BYTES:85 CRC32:<8 hex digits>

sudo nv_ota_start.sh /path/to/ota_payload_package.tar.gz
```

### 5.7 Cleanup

```bash
sudo mount /dev/mmcblk0p10 /esp_mount
sudo rm -f /esp_mount/VerFix.efi /esp_mount/startup.nsh \
           /esp_mount/version.txt /esp_mount/VerFix.log
sudo umount /esp_mount
```

> `VerFix.efi` can be left in place permanently — when both partitions are
> healthy it performs only a read-only CRC check (exits in ~1 s) and acts as a
> health check on every boot. `VerFix.log` is recreated fresh each run.

---

## 6. Building VerFix.efi

### 6.0 Pre-built binary provenance

The `VerFix.efi` checked into this repository was compiled from the source in
this repo against the following codebase:

| Property | Value |
|----------|-------|
| **L4T / BSP** | R35.4.1 (JetPack 5.1.3) |
| **UEFI release tag** | `uefi-202210.3` / `r35.4.1` |
| **edk2-nvidia commit** | `52cefd45` (4 commits ahead of `r35.4.1`) |
| **Build target** | `RELEASE`, `AARCH64`, `GCC5` |
| **Build date** | 2026-03-30 |
| **BUILDID\_STRING** | `202210.3-52cefd45-dirty` |

The binary **should work on any R35.x or R36.x BSP** because `VerFix.efi`
only calls standard EDK2 boot services and the stable `NVIDIA_FW_PARTITION_PROTOCOL`
interface, neither of which changed between those releases. If you are on a
substantially different BSP revision, rebuild from source (see §6.1–6.3).

### 6.1 Full UEFI bootloader build (Docker)

The recommended build environment is the official NVIDIA Docker image. Follow
the complete setup and build instructions on the NVIDIA wiki:

> **[Build with Docker — edk2-nvidia wiki](https://github.com/NVIDIA/edk2-nvidia/wiki/Build-with-docker)**

This produces the full `BOOTAA64.efi` firmware plus all UEFI applications
including `VerFix.efi`.

### 6.2 Integrating VerFix into the edk2-nvidia tree

Before building, copy the source files into the edk2-nvidia repository:

```bash
# From the root of your edk2-nvidia workspace:
VERFIX_DST=edk2-nvidia/Silicon/NVIDIA/Application/VerFix
mkdir -p "${VERFIX_DST}"
cp /path/to/ver-partition-bugfix/VerFix.c   "${VERFIX_DST}/"
cp /path/to/ver-partition-bugfix/VerFix.inf "${VERFIX_DST}/"
```

Then register `VerFix` in the platform DSC so it is compiled as part of the
firmware build. Add the following line to the `[Components.AARCH64]` section of
`edk2-nvidia/Platform/NVIDIA/Jetson/Jetson.dsc` (or the equivalent platform
DSC for your board):

```ini
Silicon/NVIDIA/Application/VerFix/VerFix.inf
```

And add the corresponding FDF rule to the UEFI application FV in
`edk2-nvidia/Platform/NVIDIA/Jetson/Jetson.fdf`:

```ini
INF Silicon/NVIDIA/Application/VerFix/VerFix.inf
```

### 6.3 Run the Docker build

```bash
cd <edk2-nvidia-workspace>
docker run --rm \
  -w "$(pwd)" \
  -v "$(pwd):$(pwd)" \
  -e UEFI_RELEASE_ONLY=yes \
  "ghcr.io/tianocore/containers/ubuntu-22-dev:latest" \
  bash -c "edk2-nvidia/Platform/NVIDIA/Jetson/build.sh"
```

The output binary is at:

```
Build/Jetson/RELEASE_GCC5/AARCH64/VerFix.efi
```

### 6.4 Dependencies declared in VerFix.inf

`VerFix.efi` depends only on standard EDK2 and NVIDIA packages:

| Package | Provides |
|---------|---------|
| `MdePkg` | `UefiLib`, `BaseLib`, `MemoryAllocationLib`, `PrintLib`, `DebugLib` |
| `MdeModulePkg` | (indirect, via NVIDIA platform) |
| `Silicon/NVIDIA/NVIDIA.dec` | `FwPartitionDeviceLib`, `NVIDIA_FW_PARTITION_PROTOCOL` |

No external or out-of-tree dependencies.

---

---

## License

BSD-2-Clause-Patent — see `VerFix.c` header.

Platform: Jetson Orin AGX T234 · BSP R35.x / R36.x · Updated 2026-03-31
