# Build the S10e (BCM4375) radio-layer helper tools for aarch64 Android.
#
# These are research helpers for bringing up the mosey/AirDrop radio path
# on the Samsung BCM4375 (Galaxy S10 family, Exynos 9820). They are NOT
# needed on devices with native BCM wondertap (Pixel 9+, S24 Exynos).
#
# Build on-device with Termux clang, or cross-compile with the Android NDK:
#   make                       # on-device (Termux)
#   make CC=aarch64-linux-android31-clang   # NDK cross

CC      ?= clang
CFLAGS  ?= -O2 -Wall -Wextra
BINS     = wlmon wlaf

all: $(BINS)

wlmon: wlmon.c
	$(CC) $(CFLAGS) -o $@ $<

wlaf: wlaf.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(BINS)

.PHONY: all clean
