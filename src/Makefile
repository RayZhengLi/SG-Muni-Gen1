# ====== 基本设置 ======
APP      := sg-muni
SRC_DIR  := .
INC_DIR  := ./include

# 收集所有 .c（如果你想精确控制，直接写死 SRC := xxx.c yyy.c）
SRC := $(wildcard $(SRC_DIR)/*.c)
OBJ := $(SRC:.c=.o)

# ====== 交叉/本机编译选择（被包调用时会传 CC/CFLAGS/LDFLAGS/LIBS，以下仅作默认） ======
USE_CROSS      ?= 1
TARGET_TRIPLET ?= arm-openwrt-linux-uclibcgnueabi
SDK            ?= $(HOME)/lmu5541_sdk
SYSROOT        ?= $(SDK)/staging_dir/lmu5541_target-arm_cortex-a9+vfpv3_uClibc-0.9.33.2_eabi

ifeq ($(origin CC), default) # 只有在外部没传 CC 才生效
  ifeq ($(USE_CROSS),1)
    CC := $(TARGET_TRIPLET)-gcc
  else
    CC := gcc
  endif
endif

# ====== 默认 CFLAGS/LDFLAGS/LIBS（外部传入则会叠加/覆盖） ======
COMMON_CFLAGS  := -I$(INC_DIR) -Os -fPIC -ffunction-sections -fdata-sections \
                  -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -DMG_TLS=MG_TLS_OPENSSL
COMMON_LDFLAGS := -Wl,--gc-sections
COMMON_LIBS    := -ldl -pthread -lz

ifeq ($(USE_CROSS),1)
  COMMON_CFLAGS  += --sysroot=$(SYSROOT) -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=softfp
  COMMON_LDFLAGS += --sysroot=$(SYSROOT) 
endif

# 注意：这里不直接加 -I$(STAGING_DIR)/usr/include/-L$(STAGING_DIR)/usr/lib，
# 由上层包（package Makefile）传进来，或你手动传入
CFLAGS  ?= $(COMMON_CFLAGS)
LDFLAGS ?= $(COMMON_LDFLAGS)
LIBS    ?= -Wl,-Bstatic -lssl -lcrypto -Wl,-Bdynamic $(COMMON_LIBS)
# 如需：LIBS += -latomic -lrt

# ====== 规则 ======
.PHONY: all clean
all: $(APP)

$(APP): $(OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LIBS)

%.o: %.c $(INC_DIR)/*
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(APP) $(OBJ)
