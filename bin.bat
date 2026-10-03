@REM icon.rc: 101 ICON "app.ico"
windres icon.rc -o icon.o
@REM noqrcode
@REM gcc textsync.c mongoose.c icon.o -o textsync.exe -lws2_32 -liphlpapi -lshell32 -mwindows -s -O2
@REM gcc textsync.c mongoose.c qrcodegen.c icon.o -o TextSync.exe -lws2_32 -liphlpapi -lshell32 -mwindows -s -O2
gcc textsync.c mongoose.c qrcodegen.c icon.o -o TextSync.exe -lws2_32 -liphlpapi -lshell32 -lcomctl32 -mwindows -s -O2