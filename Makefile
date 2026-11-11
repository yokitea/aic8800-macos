DEVELOPER_DIR ?= /Applications/Xcode.app/Contents/Developer
SDKROOT       ?= $(DEVELOPER_DIR)/Platforms/MacOSX.platform/Developer/SDKs/MacOSX15.sdk
LINUX_FDRV    ?= /Users/rush/.gemini/antigravity/brain/3af3af06-ee65-4085-960c-f78f553e3493/scratch/aic8800d80/drivers/aic8800/aic8800_fdrv
FW_DIR        ?= /Users/rush/.gemini/antigravity/brain/3af3af06-ee65-4085-960c-f78f553e3493/scratch/aic8800d80/fw/aic8800D80

CC  := xcrun clang
CXX := xcrun clang++

BUILD_DIR := build
OBJ_DIR   := $(BUILD_DIR)/obj

KEXT_INC_SYS := -isystem $(SDKROOT)/System/Library/Frameworks/Kernel.framework/Versions/A/Headers \
                -isystem $(SDKROOT)/System/Library/Frameworks/Kernel.framework/Versions/A/Headers/bsd

KEXT_BASE_FLAGS := -arch x86_64 -mkernel -DKERNEL -DAPPLE -nostdlib -fno-builtin -fno-common -O2 \
                   -Wall -Wextra -Werror -Wno-deprecated-declarations -Icommon

WIFI_CXXFLAGS := $(KEXT_BASE_FLAGS) -std=gnu++17 -fno-exceptions -fno-rtti $(KEXT_INC_SYS)
WIFI_CFLAGS   := $(KEXT_BASE_FLAGS) -std=c11 $(KEXT_INC_SYS)
WIFI_LDFLAGS  := -arch x86_64 -mkernel -nostdlib -Xlinker -kext \
                 -L$(SDKROOT)/usr/lib -lkmod

LILU_FLAGS    := -DPRODUCT_NAME=AIC8800BTQuirk -DMODULE_VERSION=0.1.0 -DDEBUG=0 \
                 -isystem external/Lilu/Lilu -isystem external/MacKernelSDK/Headers $(KEXT_INC_SYS)
BT_CXXFLAGS   := $(KEXT_BASE_FLAGS) -std=c++14 -fno-exceptions -fno-rtti -fvisibility=hidden $(LILU_FLAGS)
BT_CFLAGS     := $(KEXT_BASE_FLAGS) -std=c11 $(LILU_FLAGS)
BT_LDFLAGS    := -arch x86_64 -mkernel -nostdlib -Xlinker -kext \
                 -Lexternal/MacKernelSDK/Library/x86_64 -lkmod

MSC_KEXT := $(BUILD_DIR)/AIC8800MSCBlocker.kext
WIFI_KEXT := $(BUILD_DIR)/AIC8800WiFi.kext
BT_KEXT   := $(BUILD_DIR)/AIC8800BTQuirk.kext
FWLOAD    := $(BUILD_DIR)/aic8800_fwload

TEST_ABI    := $(BUILD_DIR)/test_abi_parity
TEST_PHASE0 := $(BUILD_DIR)/test_phase0
TEST_PHASE1 := $(BUILD_DIR)/test_phase1
TEST_PHASE2 := $(BUILD_DIR)/test_phase2_bt
TEST_NOHEAP := $(BUILD_DIR)/test_hotpath_noheap
TEST_DAEMON := $(BUILD_DIR)/test_fwload_daemon

all: $(MSC_KEXT)/Contents/MacOS/AIC8800MSCBlocker \
     $(WIFI_KEXT)/Contents/MacOS/AIC8800WiFi \
     $(BT_KEXT)/Contents/MacOS/AIC8800BTQuirk \
     $(FWLOAD)

$(OBJ_DIR)/msc $(OBJ_DIR)/wifi $(OBJ_DIR)/bt $(BUILD_DIR):
	@mkdir -p $@

# 1. Passive Stage 0 MSC Blocker Kext (non-exclusive IOService attach)
$(OBJ_DIR)/msc/kmod_info.o: kexts/AIC8800MSCBlocker/kmod_info.c | $(OBJ_DIR)/msc
	$(CC) $(WIFI_CFLAGS) -c $< -o $@

$(OBJ_DIR)/msc/AIC8800MSCBlocker.o: kexts/AIC8800MSCBlocker/AIC8800MSCBlocker.cpp kexts/AIC8800MSCBlocker/AIC8800MSCBlocker.hpp | $(OBJ_DIR)/msc
	$(CXX) $(WIFI_CXXFLAGS) -c $< -o $@

$(MSC_KEXT)/Contents/MacOS/AIC8800MSCBlocker: $(OBJ_DIR)/msc/kmod_info.o $(OBJ_DIR)/msc/AIC8800MSCBlocker.o kexts/AIC8800MSCBlocker/Info.plist
	@mkdir -p $(MSC_KEXT)/Contents/MacOS
	cp kexts/AIC8800MSCBlocker/Info.plist $(MSC_KEXT)/Contents/Info.plist
	plutil -lint $(MSC_KEXT)/Contents/Info.plist
	$(CXX) $(WIFI_LDFLAGS) $(OBJ_DIR)/msc/kmod_info.o $(OBJ_DIR)/msc/AIC8800MSCBlocker.o -o $@

# 2. Pure Public-KPI IOEthernetController Wi-Fi Kext Skeleton
$(OBJ_DIR)/wifi/kmod_info.o: kexts/AIC8800WiFi/kmod_info.c | $(OBJ_DIR)/wifi
	$(CC) $(WIFI_CFLAGS) -c $< -o $@

$(OBJ_DIR)/wifi/AIC8800WiFi.o: kexts/AIC8800WiFi/AIC8800WiFi.cpp kexts/AIC8800WiFi/AIC8800WiFi.hpp common/aic8800_abi.h common/aic8800_framing.h common/aic8800_lmac.h | $(OBJ_DIR)/wifi
	$(CXX) $(WIFI_CXXFLAGS) -c $< -o $@

$(WIFI_KEXT)/Contents/MacOS/AIC8800WiFi: $(OBJ_DIR)/wifi/kmod_info.o $(OBJ_DIR)/wifi/AIC8800WiFi.o kexts/AIC8800WiFi/Info.plist
	@mkdir -p $(WIFI_KEXT)/Contents/MacOS
	cp kexts/AIC8800WiFi/Info.plist $(WIFI_KEXT)/Contents/Info.plist
	plutil -lint $(WIFI_KEXT)/Contents/Info.plist
	$(CXX) $(WIFI_LDFLAGS) $(OBJ_DIR)/wifi/kmod_info.o $(OBJ_DIR)/wifi/AIC8800WiFi.o -o $@

# 3. Lilu 1.7.2 BT Quirk Plugin Kext Skeleton
$(OBJ_DIR)/bt/kmod_info.o: kexts/AIC8800BTQuirk/kmod_info.c | $(OBJ_DIR)/bt
	$(CC) $(BT_CFLAGS) -c $< -o $@

$(OBJ_DIR)/bt/plugin_start.o: external/Lilu/Lilu/Library/plugin_start.cpp | $(OBJ_DIR)/bt
	$(CXX) $(BT_CXXFLAGS) -Wno-unused-parameter -c $< -o $@

$(OBJ_DIR)/bt/kern_start.o: kexts/AIC8800BTQuirk/kern_start.cpp kexts/AIC8800BTQuirk/AIC8800BTQuirk.hpp | $(OBJ_DIR)/bt
	$(CXX) $(BT_CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/bt/AIC8800BTQuirk.o: kexts/AIC8800BTQuirk/AIC8800BTQuirk.cpp kexts/AIC8800BTQuirk/AIC8800BTQuirk.hpp common/aic8800_abi.h | $(OBJ_DIR)/bt
	$(CXX) $(BT_CXXFLAGS) -c $< -o $@

$(BT_KEXT)/Contents/MacOS/AIC8800BTQuirk: $(OBJ_DIR)/bt/kmod_info.o $(OBJ_DIR)/bt/plugin_start.o $(OBJ_DIR)/bt/kern_start.o $(OBJ_DIR)/bt/AIC8800BTQuirk.o kexts/AIC8800BTQuirk/Info.plist
	@mkdir -p $(BT_KEXT)/Contents/MacOS
	cp kexts/AIC8800BTQuirk/Info.plist $(BT_KEXT)/Contents/Info.plist
	plutil -lint $(BT_KEXT)/Contents/Info.plist
	$(CXX) $(BT_LDFLAGS) $(OBJ_DIR)/bt/kmod_info.o $(OBJ_DIR)/bt/plugin_start.o $(OBJ_DIR)/bt/kern_start.o $(OBJ_DIR)/bt/AIC8800BTQuirk.o -o $@

# 4. Stage 0/1 User-Space Bootloader, IOKit Watch Daemon & 5 GHz Patch Tool
$(FWLOAD): daemon/aic8800_fwload/main.c common/aic8800_abi.h common/aic8800_framing.h common/aic8800_fwload_ctl.h | $(BUILD_DIR)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -Wno-deprecated-declarations -Icommon $< -o $@ -framework IOKit -framework CoreFoundation -framework DiskArbitration

# 5. Phase 0 Verification Tests
$(TEST_ABI): tests/test_abi_parity.c common/aic8800_abi.h | $(BUILD_DIR)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror \
		-DCONFIG_RWNX_FULLMAC=1 -DCONFIG_USER_MAX=1 -DNX_TXQ_CNT=5 -DCONFIG_RWNX_BCMC=1 \
		-DNX_REMOTE_STA_MAX=32 -DNX_REMOTE_STA_MAX_FOR_OLD_IC=8 -DAICWF_USB_SUPPORT=1 \
		-Itests/linux_shim -Icommon -isystem $(LINUX_FDRV) \
		$< -o $@

$(TEST_PHASE0): tests/test_phase0.cpp common/aic8800_abi.h common/aic8800_framing.h | $(BUILD_DIR)
	$(CXX) -std=c++17 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all \
		-Wall -Wextra -Werror -Icommon \
		$< -o $@

$(TEST_PHASE1): tests/test_phase1.cpp common/aic8800_abi.h common/aic8800_framing.h common/aic8800_lmac.h | $(BUILD_DIR)
	$(CXX) -std=c++17 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all \
		-Wall -Wextra -Werror -Icommon \
		$< -o $@

$(TEST_PHASE2): tests/test_phase2_bt.cpp common/aic8800_abi.h | $(BUILD_DIR)
	$(CXX) -std=c++17 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all \
		-Wall -Wextra -Werror -Icommon \
		$< -o $@

$(TEST_NOHEAP): tests/test_hotpath_noheap.c common/aic8800_abi.h common/aic8800_framing.h common/aic8800_lmac.h | $(BUILD_DIR)
	$(CC) -std=c11 -O2 -fno-builtin -Wall -Wextra -Werror -Icommon $< -o $@

$(TEST_DAEMON): tests/test_fwload_daemon.c common/aic8800_abi.h common/aic8800_fwload_ctl.h | $(BUILD_DIR)
	$(CC) -std=c11 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all \
		-Wall -Wextra -Werror -Icommon $< -o $@

test: all $(TEST_ABI) $(TEST_PHASE0) $(TEST_PHASE1) $(TEST_PHASE2) $(TEST_NOHEAP) $(TEST_DAEMON)
	@echo "--- Running ABI compile-time parity check ---"
	./$(TEST_ABI)
	@echo "--- Running ASan + UBSan 10k+ Phase 0 vector suite ---"
	./$(TEST_PHASE0)
	@echo "--- Running ASan + UBSan Phase 1 LMAC & TX/RX flow suite ---"
	./$(TEST_PHASE1)
	@echo "--- Running ASan + UBSan Phase 2 Bluetooth quirk suite ---"
	./$(TEST_PHASE2)
	@echo "--- Running zero-heap C interpose hot-path trap ---"
	./$(TEST_NOHEAP)
	@echo "--- Running ASan + UBSan fwload daemon control-plane suite ---"
	./$(TEST_DAEMON)
	@echo "--- Validating LaunchDaemon plist ---"
	plutil -lint daemon/com.rush.aic8800_fwload.plist
	@echo "--- Running aic8800_fwload --help & --dry-run (Stage 1 5 GHz patch validation) ---"
	./$(FWLOAD) --help >/dev/null
	./$(FWLOAD) --fw-dir "$(FW_DIR)" --dry-run
	@echo "--- Inspecting Mach-O types & kext KPI purity ---"
	file $(MSC_KEXT)/Contents/MacOS/AIC8800MSCBlocker $(WIFI_KEXT)/Contents/MacOS/AIC8800WiFi $(BT_KEXT)/Contents/MacOS/AIC8800BTQuirk $(FWLOAD)
	@echo "Checking AIC8800MSCBlocker & AIC8800WiFi undefined symbols for private/Lilu leakage:"
	@if nm -u $(MSC_KEXT)/Contents/MacOS/AIC8800MSCBlocker $(WIFI_KEXT)/Contents/MacOS/AIC8800WiFi | grep -E "Lilu|lilu|KernelPatcher|Apple80211|IOMallocPage"; then exit 1; else echo "OK: AIC8800MSCBlocker & AIC8800WiFi pure public KPI"; fi

install-daemon: $(FWLOAD)
	sudo FW_DIR="$(FW_DIR)" ./scripts/install_daemon.sh

uninstall-daemon:
	sudo ./scripts/uninstall_daemon.sh

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all test clean install-daemon uninstall-daemon
