windres app.rc -o app_res.o
gcc textsync.c mongoose.c qrcodegen.c app_res.o -o TextSync.exe -lws2_32 -liphlpapi -lshell32 -mwindows -s -O2