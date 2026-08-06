@echo off
echo ============================================
echo   TsOs v1.0 - Production Mode (no tests)
echo.
echo   CLICK on the QEMU window that opens
echo   then TYPE to interact with the shell.
echo   Press Ctrl+Alt to release the mouse.
echo.
echo   Serial log: serial_debug.txt
echo ============================================
wsl -d Ubuntu -- bash -c "cd '/mnt/d/TsOs' && make clean && make SKIP_TESTS=1 && make disk.img 2>&1 | tail -5"
if %ERRORLEVEL% NEQ 0 (
    echo Build failed! Check WSL is running.
    pause
    exit /b 1
)
D:\qemu\qemu-system-x86_64.exe -cdrom D:\TsOs\TsOs.iso -drive file=D:\TsOs\disk.img,format=raw,if=ide,index=0,media=disk -serial file:D:\TsOs\serial_debug.txt -serial null -m 512