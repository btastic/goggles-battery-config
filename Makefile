# battery-config: wtfOS package for the DJI FPV Goggles V1 and V2.
#
#   make NDK=/path/to/android-ndk        build libbattery_config.so
#   make NDK=/path/to/android-ndk ipk    build the .ipk
#   make clean
#
# NDK defaults to $ANDROID_NDK_HOME. Set CC/STRIP to use another toolchain.

PKG      := battery-config
MOD      := battery_config
VERSION  := 0.2.0
ARCH     := pigeon-glasses
UNIT     := battery-config
MOD_NAME := lib$(MOD).so
IPK      := $(PKG)_$(VERSION)_$(ARCH).ipk

NDK      ?= $(ANDROID_NDK_HOME)
API      ?= 23
ifeq ($(OS),Windows_NT)
HOSTTAG  ?= windows-x86_64
else
HOSTTAG  ?= $(shell uname -s | tr A-Z a-z)-x86_64
endif
TOOLBIN  := $(NDK)/toolchains/llvm/prebuilt/$(HOSTTAG)/bin
# make predefines CC, so ?= would never select the NDK compiler
ifeq ($(origin CC),default)
CC       := $(TOOLBIN)/clang --target=armv7a-linux-androideabi$(API)
endif
STRIP    ?= $(TOOLBIN)/llvm-strip

CFLAGS  := -O2 -fPIC -fvisibility=hidden -Wall -Wextra
LDFLAGS := -shared -Wl,-soname,$(MOD_NAME) -llog

BUILD := build
STAGE := $(BUILD)/stage
DATA  := $(STAGE)/data/opt/etc
TAR   := tar --owner=0 --group=0

.PHONY: all ipk clean

all: $(BUILD)/$(MOD_NAME)

$(BUILD)/$(MOD_NAME): src/battery_config.c
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)
	$(STRIP) --strip-unneeded $@

ipk: $(BUILD)/$(IPK)

$(BUILD)/$(IPK): $(BUILD)/$(MOD_NAME) pkg/* files/package-config/* files/dinit.d/$(UNIT) Makefile
	rm -rf $(STAGE)
	install -d $(DATA)/preload.d $(DATA)/package-config/$(PKG) $(DATA)/dinit.d $(STAGE)/control
	install -m0755 $(BUILD)/$(MOD_NAME) $(DATA)/preload.d/
	install -m0644 files/package-config/* $(DATA)/package-config/$(PKG)/
	install -m0644 files/dinit.d/$(UNIT) $(DATA)/dinit.d/
	sed -e 's/@VERSION@/$(VERSION)/' -e 's/@ARCH@/$(ARCH)/' pkg/control > $(STAGE)/control/control
	install -m0755 pkg/postinst pkg/prerm $(STAGE)/control/
	install -m0644 pkg/conffiles $(STAGE)/control/
	echo "2.0" > $(STAGE)/debian-binary
	cd $(STAGE)/control && $(TAR) -czf ../control.tar.gz .
	cd $(STAGE)/data && $(TAR) -czf ../data.tar.gz .
	cd $(STAGE) && $(TAR) -czf ../$(IPK) ./debian-binary ./control.tar.gz ./data.tar.gz

clean:
	rm -rf $(BUILD)
