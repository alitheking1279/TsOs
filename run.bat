@echo off
echo ============================================
echo   TsOs v1.0
echo.
echo   CLICK on the QEMU window that opens
echo   then TYPE to interact with the shell.
echo   Press Ctrl+Alt to release the mouse.
echo.
echo   Serial log: serial_debug.txt
echo.
echo   If keyboard doesn't work, try:
echo     run-serial.bat
echo ============================================
D:\qemu\qemu-system-x86_64.exe -cdrom D:\TsOs\TsOs.iso -drive file=D:\TsOs\disk.img,format=raw,if=ide,index=0,media=disk -serial file:D:\TsOs\serial_debug.txt -serial null -m 512 -vga std -display sdl
