@echo off
echo Starting QEMU...
echo.
echo Type these commands in QEMU when it boots:
echo   write fruits/orange I am Orange
echo   cat fruits/orange
echo.
echo Close the QEMU window when done.
echo Press any key to start QEMU...
pause > nul
"D:\qemu\qemu-system-x86_64.exe" -cdrom D:\TsOs\TsOs.iso -drive file=D:\TsOs\disk.img,format=raw,if=ide,index=0,media=disk -serial file:D:\TsOs\serial_debug.txt -serial null -m 512
echo QEMU exited.
pause
