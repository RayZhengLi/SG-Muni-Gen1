include $(TOPDIR)/rules.mk

PKG_NAME:=sg-muni
PKG_VERSION:=1.0.0
PKG_RELEASE:=1

# 源码目录：假设你的源码就在本包的 src/ 目录
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)

# 编译期依赖：使用你前面已经做好的 openssl-3.5.4-static
PKG_BUILD_DEPENDS:=openssl-3.5.4-static

include $(INCLUDE_DIR)/package.mk

define Package/sg-muni
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=sg-muni main application (Mongoose + OpenSSL 3.5.4)
  DEPENDS:=+libpthread +zlib +libgcc +libuuid
endef

define Package/sg-muni/description
sg-muni 主程序，使用 Mongoose 7.x 与 OpenSSL 3.5.4（libssl/libcrypto 静态链接，其余动态）。
endef

# -------- Build 阶段 ----------
define Build/Prepare
	# 拷贝你的 src 源到构建目录
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
	# 确保 include 子目录存在
	mkdir -p $(PKG_BUILD_DIR)/include
	# 如果你的头文件本来就在 src/include/，上面的 $(CP) 已经拷过去了
endef

# 交叉编译 flags（ARMv7 + softfp，根据你的 SDK）
TARGET_CFLAGS += -std=gnu99 \
				 -I$(STAGING_DIR)/usr/include \
                 -I$(PKG_BUILD_DIR)/include \
                 -Os -fPIC -ffunction-sections -fdata-sections \
                 -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=softfp \
                 -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -DMG_TLS=MG_TLS_OPENSSL

TARGET_LDFLAGS += -L$(STAGING_DIR)/usr/lib -Wl,--gc-sections \
                  -Wl,-rpath-link,$(STAGING_DIR)/usr/lib \
                  -Wl,-rpath-link,$(STAGING_DIR)/lib

# 仅把 OpenSSL 静态化，其余动态
TARGET_LIBS := -Wl,-Bstatic -lssl -lcrypto -Wl,-Bdynamic -ldl -pthread -lz -luuid -lm
# 某些工具链需要：
# TARGET_LIBS += -latomic
# TARGET_LIBS += -lrt

define Build/Compile
	# 调用 src 内的 Makefile 来构建（亦可直接 $(TARGET_CC) 命令行）
	$(MAKE) -C $(PKG_BUILD_DIR) \
		CC="$(TARGET_CC)" \
		DESTDIR="$(PKG_BUILD_DIR)/out" \
		CFLAGS="$(TARGET_CFLAGS)" \
		LDFLAGS="$(TARGET_LDFLAGS)" \
		LIBS="$(TARGET_LIBS)" \
		all
endef

# -------- 安装到 rootfs ----------
define Package/sg-muni/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/sg-muni $(1)/usr/bin/

	# 可选：安装一个简单的 init 脚本（如果需要随系统启动）
	#$(INSTALL_DIR) $(1)/etc/init.d
	#$(INSTALL_BIN) ./files/sg-muni.init $(1)/etc/init.d/sg-muni
endef

$(eval $(call BuildPackage,sg-muni))
