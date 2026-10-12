@ECHO OFF
PROMPT $P$G

REM Initialize Serial Port (COM1)
MODE COM1:9600,N,8,1 > NUL 2> NUL

ECHO ====================================================== > COM1
ECHO 86Box Automated Dynarec Benchmark Suite               > COM1
ECHO System: Intel Pentium II on 440BX                     > COM1
ECHO ====================================================== > COM1

REM --------------------------------------------------------
REM 1. Doom Timedemo (32-bit Integer ALU & Register Pressure)
REM --------------------------------------------------------
IF NOT EXIST C:\BENCH\DOOM\DOOM.EXE GOTO SKIP_DOOM
ECHO [START] DOOM demo3                                     > COM1
CD C:\BENCH\DOOM
DOOM.EXE -timedemo demo3 -nosound -nodraw                   > COM1
ECHO [FINISH] DOOM demo3                                    > COM1
CD \
:SKIP_DOOM

REM --------------------------------------------------------
REM 2. Quake Timedemo (32-bit FPU & Software Texture Raster)
REM --------------------------------------------------------
IF NOT EXIST C:\BENCH\QUAKE\QUAKE.EXE GOTO SKIP_QUAKE
ECHO [START] QUAKE demo1                                    > COM1
CD C:\BENCH\QUAKE
QUAKE.EXE -timedemo demo1 -nosound -width 320 -height 200   > COM1
ECHO [FINISH] QUAKE demo1                                   > COM1
CD \
:SKIP_QUAKE

REM --------------------------------------------------------
REM 3. PCPBench (PC Player Benchmark - Mode 13h / VESA 3D)
REM --------------------------------------------------------
IF NOT EXIST C:\BENCH\PCPBENCH\PCPBENCH.EXE GOTO SKIP_PCP
ECHO [START] PCPBENCH vga                                   > COM1
CD C:\BENCH\PCPBENCH
PCPBENCH.EXE /vga                                           > COM1
ECHO [FINISH] PCPBENCH vga                                  > COM1
CD \
:SKIP_PCP

REM --------------------------------------------------------
REM 4. Dhrystone 2.1 (Pure 32-bit DPMI Integer Compute)
REM --------------------------------------------------------
IF NOT EXIST C:\BENCH\DHRY\DHRY.EXE GOTO SKIP_DHRY
ECHO [START] DHRYSTONE                                      > COM1
CD C:\BENCH\DHRY
DHRY.EXE 50000000                                           > COM1
ECHO [FINISH] DHRYSTONE                                     > COM1
CD \
:SKIP_DHRY

REM --------------------------------------------------------
REM Completion Marker & Clean Poweroff
REM --------------------------------------------------------
ECHO ====================================================== > COM1
ECHO === BENCHMARK SUITE COMPLETE ===                       > COM1
ECHO ====================================================== > COM1

REM Delay to ensure virtual COM1 buffer flushes to host
IF EXIST C:\FREEDOS\BIN\SLEEP.EXE C:\FREEDOS\BIN\SLEEP.EXE 2
IF EXIST C:\FDOS\BIN\SLEEP.EXE C:\FDOS\BIN\SLEEP.EXE 2
CHOICE /C:YN /N /T:Y,2 > NUL 2> NUL

REM Clean APM Shutdown (FreeDOS native FDAPM, then fallback POWEROFF.COM)
IF EXIST C:\FREEDOS\BIN\FDAPM.EXE C:\FREEDOS\BIN\FDAPM.EXE POWEROFF
IF EXIST C:\FDOS\BIN\FDAPM.EXE C:\FDOS\BIN\FDAPM.EXE POWEROFF
FDAPM POWEROFF > NUL 2> NUL
IF EXIST C:\UTILS\POWEROFF.COM C:\UTILS\POWEROFF.COM
IF EXIST C:\POWEROFF.COM C:\POWEROFF.COM
