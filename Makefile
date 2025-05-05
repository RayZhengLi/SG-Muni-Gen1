#################################################
# OpenWrt Makefile for sg-nmea program
#
# Most of the variables used here are defined in
# the include directives below. We just need to
# specify a basic description of the package,
# where to build our program, where to find
# the source files, and where to install the
# compiled program on the lmu5000.
#
# Be very careful of spacing in this file!!
# Indents should be tabs, not spaces, and
# there should be no trailing whitespace in
# lines that are not commented.
#
##################################################

include $(TOPDIR)/rules.mk

PKG_NAME:=sg-muni
PKG_VERSION:=0.1.0
PKG_RELEASE:=1

PKG_SOURCE:=$(PKG_NAME)-$(PKG_VERSION).tar.xz
PKG_BUILD_DIR := $(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

define Package/sg-muni
	SECTION:=utils
	CATEGORY:=Utilities
	TITLE:=SG TCP Server Host App Test
	DEPENDS:=+libpthread +libuuid
endef

define Package/sg-muni/description
	Step Global TCP Test Host App Package
endef

# 禁用 configure，避免 OpenWrt 误调用
CONFIGURE := no

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR) \
		CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS) -std=c99 -Wall -Wextra -D_SVID_SOURCE -D_XOPEN_SOURCE=700" \
		LDFLAGS="$(TARGET_LDFLAGS) -lm  -luuid -lpthread"
endef


define Package/sg-muni/install
	$(INSTALL_DIR) $(1)/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/sg-muni $(1)/bin/
endef

define Package/sg-muni/postinst
#!/bin/sh
echo "Running post-install script for sg-muni"

cat << "EOF" > /etc/init.d/sg-muni
#!/bin/sh /etc/rc.common
START=99
STOP=10

LOGGER="/usr/bin/logger -t sg-muni"

start() {
sleep 5
`/bin/sg-muni` > /dev/null 2>&1 &
echo "sg-muni state start"
LOGGER "sg-muni state start"
}

stop() {
killall sg-muni
echo "sg-muni state stop"
LOGGER "sg-muni state stop"
}

EOF

chmod +x /etc/init.d/sg-muni
/etc/init.d/sg-muni enable

echo "sg-muni init script installed and enabled!"
endef

$(eval $(call BuildPackage,sg-muni))
