CC := clang
CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror -Wno-deprecated-declarations -I../../common
LDFLAGS := -framework IOKit -framework CoreFoundation -framework DiskArbitration

TARGET := aic8800_fwload

all: $(TARGET)

$(TARGET): main.c ../../common/aic8800_abi.h ../../common/aic8800_framing.h ../../common/aic8800_fwload_ctl.h
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean

