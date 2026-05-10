I compile in PowerShell with:

```powershell
& "C:\Users\User\AppData\Local\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe" compile --fqbn espressif:esp32:XIAO_ESP32C3 "c:\projects\smart_board\Arduino\Clock\Clock.ino"
```

I upload with:

```powershell
& "C:\Users\User\AppData\Local\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe" upload --fqbn espressif:esp32:XIAO_ESP32C3 --port COM3 "c:\projects\smart_board\Arduino\Clock\Clock.ino"
```

Please don’t compile or upload anything yourself. Just tell me when compilation is needed.
