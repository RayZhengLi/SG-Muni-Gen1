
# # build sg-tcp executable when user executes "make"
# INCLUDES = -I./include
# LIBS = -pthread -luuid

# TARGET = sg-tcp

# # Add custom flags for this project
# MY_CFLAGS = -std=c99

# $(TARGET): $(TARGET).o sg-tcp-server.o sg-gpio.o sg-com
# 	$(CC) $(LDFLAGS) $(MY_CFLAGS) $(LIBS) $(INCLUDES) -o $(TARGET) $(TARGET).o sg-tcp_server.o sg-gpio.o
# $(TARGET).o: $(TARGET).c
# 	$(CC) $(CFLAGS) $(MY_CFLAGS) $(LIBS) $(INCLUDES) -c $(TARGET).c

# sg-tcp_server.o: sg-tcp_server.c
# 	$(CC) $(CFLAGS) $(MY_CFLAGS) $(LIBS) $(INCLUDES) -c sg-tcp_server.c

# sg-gpio.o: sg-gpio.c
# 	$(CC) $(CFLAGS) $(MY_CFLAGS) $(INCLUDES) -c sg-gpio.c

# # remove object files and executable when user executes "make clean"
# clean:
# 	rm *.o $(TARGET)

# OpenWrt cross-compiler variables
CC      = $(TARGET_CC)
CFLAGS  = -Wall -Wextra -std=c99 -g -D_SVID_SOURCE -D_XOPEN_SOURCE=700
LDFLAGS = -luuid -lpthread -lm

# Paths
SRC_DIR = .
INCLUDE_DIR = $(SRC_DIR)/include
#BUILD_DIR = $(PKG_BUILD_DIR)
BUILD_DIR ?= build


# Find all source files
SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(SRCS))

# Target executable
TARGET = sg-muni

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(INCLUDE_DIR) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(TARGET)
	rm *.o $(TARGET)

