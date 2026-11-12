CC = i686-w64-mingw32-gcc
WINDRES = i686-w64-mingw32-windres
CFLAGS = -Os -Wall -Wextra -march=i386
LDFLAGS = -mwindows -nostdlib -Wl,--disable-auto-import -Wl,-e,_WinMainCRTStartup@0 -Wl,--disable-dynamicbase -Wl,--disable-nxcompat

.PHONY: all clean
all: hal9k.exe

hal9k-res.o: hal9k.rc hal9k.ico
	$(WINDRES) -i hal9k.rc -o $@

hal9k.exe: hal9k.c hal9k-res.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ -luser32 -lgdi32 -lwininet -lwinmm -lkernel32

# HAL9K.INI is git-ignored and holds the signup address; a Slack webhook URL
# in it is a credential. When the file is present it is folded into the image,
# so a built image sends signups without further steps - and therefore carries
# whatever this folder is configured for. Keep such an image private. Without
# the file the image still builds, and the form stays offline.
CONFIG = $(firstword $(wildcard HAL9K.INI hal9k.ini))
ISO_FILES = HAL9K.EXE=hal9k.exe AUTORUN.INF=autorun.inf
ifneq ($(CONFIG),)
ISO_FILES += HAL9K.INI=$(CONFIG)
endif

# The image is rebuilt on every request: whether the config exists and how old
# it is say nothing about what an earlier image was built from, so a copied-in
# HAL9K.INI with an older timestamp would otherwise be left out silently.
.PHONY: force
force:

hal9k.iso: force hal9k.exe autorun.inf $(CONFIG)
	xorriso -as mkisofs -iso-level 1 -V HAL9K -o $@ -graft-points $(ISO_FILES)
	@if [ -n "$(CONFIG)" ]; then \
		echo "hal9k.iso includes $(CONFIG)"; \
	else \
		echo "hal9k.iso built without HAL9K.INI: the form stays offline"; \
	fi

clean:
	rm -f hal9k.exe hal9k-res.o hal9k.iso
