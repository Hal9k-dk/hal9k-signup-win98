CC = i686-w64-mingw32-gcc
WINDRES = i686-w64-mingw32-windres
CFLAGS = -Os -Wall -Wextra -march=i386
LDFLAGS = -mwindows -nostdlib -Wl,--disable-auto-import -Wl,-e,_WinMainCRTStartup@0 -Wl,--disable-dynamicbase -Wl,--disable-nxcompat

.PHONY: all clean
all: hal9k.exe

hal9k-res.o: hal9k.rc hal9k.ico
	$(WINDRES) -i hal9k.rc -o $@

hal9k.exe: hal9k.c hal9k-res.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ -luser32 -lgdi32 -lwininet -lkernel32

hal9k.iso: hal9k.exe autorun.inf
	xorriso -as mkisofs -iso-level 1 -V HAL9K -o $@ -graft-points HAL9K.EXE=hal9k.exe AUTORUN.INF=autorun.inf

clean:
	rm -f hal9k.exe hal9k-res.o hal9k.iso
