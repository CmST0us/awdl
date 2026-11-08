ROOT := ../../..
UP ?= $(ROOT)/.build/targets/aarch64-unknown-linux-gnu/hostap
BUILD ?= $(ROOT)/.build/bcm43752/hostap
CPPFLAGS += -D_GNU_SOURCE -DCONFIG_NAN -I$(UP)/src -I$(UP)/src/utils
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -Wall -Wextra -Werror -Wno-unused-parameter
SOURCES := netlink.c phy.c command.c events.c management.c keys.c schedule.c stations.c ndi.c startup.c driver_bcm_nan.c
OBJECTS := $(addprefix $(BUILD)/,$(SOURCES:.c=.o))

.PHONY: all
all: $(BUILD)/libwa-bcm-nan.a
$(BUILD):
	mkdir -p $@
$(BUILD)/%.o: %.c Makefile | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/libwa-bcm-nan.a: $(OBJECTS)
	$(AR) rcs $@ $^
-include $(OBJECTS:.o=.d)
