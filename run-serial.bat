@echo off
echo ============================================
echo   TsOs - Serial Console Mode (Fallback)
echo.
echo   Type in THIS window to send input
echo   to the shell via serial port.
echo.
echo   Watch the QEMU window for output.
echo   Press Ctrl+Alt to release mouse.
echo.
echo   Serial log: serial_debug.txt
echo ============================================
D:\qemu\qemu-system-x86_64.exe -cdrom D:\TsOs\TsOs.iso -drive file=D:\TsOs\disk.img,format=raw,if=ide,index=0,media=disk -serial mon:stdio -serial null -m 512