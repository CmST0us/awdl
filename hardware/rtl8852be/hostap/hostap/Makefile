ROOT := ../../../..
UP ?= $(ROOT)/.build/hostap
BUILD ?= $(ROOT)/.build/rtl-hostap
CPPFLAGS += -D_GNU_SOURCE -DCONFIG_NAN -I$(UP)/src -I$(UP)/src/utils
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -Wall -Wextra -Werror -Wno-unused-parameter
OBJECTS := $(BUILD)/driver_rtl_nan.o $(BUILD)/rtl_schedule.o $(BUILD)/rtl_ndi.o $(BUILD)/rtl_nmi.o $(BUILD)/igtk.o $(BUILD)/phy.o $(BUILD)/backend.o

.PHONY: all
all: $(BUILD)/libwa-rtl-nan.a
$(OBJECTS): Makefile
$(BUILD):
	mkdir -p $@
$(BUILD)/driver_rtl_nan.o: driver_rtl_nan.c rtl_nmi.h rtl_ndi.h rtl_schedule.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/rtl_schedule.o: rtl_schedule.c rtl_schedule.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/rtl_ndi.o: rtl_ndi.c rtl_ndi.h ../backend.h ../../driver/include/wa_nan_uapi.h ../phy.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/rtl_nmi.o: rtl_nmi.c rtl_nmi.h ../backend.h ../igtk.h ../group_protection.h ../../driver/include/wa_nan_uapi.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/igtk.o: ../igtk.c ../igtk.h ../backend.h ../../driver/include/wa_nan_uapi.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/phy.o: ../phy.c ../phy.h ../backend.h ../../driver/include/wa_nan_uapi.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/backend.o: ../backend.c ../backend.h ../../driver/include/wa_nan_uapi.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/libwa-rtl-nan.a: $(OBJECTS)
	$(AR) rcs $@ $^
-include $(OBJECTS:.o=.d)
