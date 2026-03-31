@echo -off
echo [VER_FIX] Start
map -r
fs3:
VerFix.efi
echo [VER_FIX] Done
echo [VER_FIX] Launching normal boot
\EFI\BOOT\BOOTAA64.EFI
