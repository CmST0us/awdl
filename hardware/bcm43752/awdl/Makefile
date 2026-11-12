ROOT := ../../..
UP ?= $(ROOT)/.build/targets/aarch64-unknown-linux-gnu/hostap
BUILD ?= $(ROOT)/.build/targets/aarch64-unknown-linux-gnu/aware-awdl
CONTROL ?= $(BUILD)/libaware_awdl.a
CPPFLAGS += -D_GNU_SOURCE -DCONFIG_NAN -I$(UP)/src -I$(UP)/src/utils
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -Wall -Wextra -Werror -Wno-unused-parameter -ffunction-sections -fdata-sections
SOURCES := main.c session.c controller.c network.c ../hostap/netlink.c ../hostap/command.c ../hostap/ndi.c ../hostap/phy.c
UTILS := common os_unix wpa_debug
OBJECTS := $(addprefix $(BUILD)/bcm-awdl/,$(notdir $(SOURCES:.c=.o))) $(addprefix $(BUILD)/bcm-awdl/,$(addsuffix .o,$(UTILS)))
VPATH := .:../hostap:$(UP)/src/utils

.PHONY: all
all: $(BUILD)/bcm43752-awdl
$(BUILD)/bcm-awdl:
	mkdir -p $@
$(BUILD)/bcm-awdl/%.o: %.c Makefile | $(BUILD)/bcm-awdl
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/bcm43752-awdl: $(OBJECTS) $(CONTROL)
	$(CC) $(LDFLAGS) -Wl,--gc-sections $(OBJECTS) $(CONTROL) -lpthread -ldl -lm -o $@
-include $(OBJECTS:.o=.d)
