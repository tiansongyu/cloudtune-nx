#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      := CloudTune
OVERLAY_ROOT := $(abspath $(dir $(firstword $(MAKEFILE_LIST))))
# Keep every generated file outside the source tree, including recursive make
# objects and the linker map. OUTPUT_DIR may be overridden for an isolated build.
OUTPUT_DIR  ?= $(abspath $(OVERLAY_ROOT)/../build/overlay)
override OUTPUT_DIR := $(abspath $(OUTPUT_DIR))
BUILD       := $(OUTPUT_DIR)/obj
SOURCES     := source ../background/client
INCLUDES    := ../include vendor/libtesla
APP_TITLE   := CloudTune NX
APP_AUTHOR  := CloudTune NX contributors
APP_VERSION := $(shell sed -n '/^project/s/.*VERSION //p' $(OVERLAY_ROOT)/../CMakeLists.txt | cut -d' ' -f1)
NO_ICON     := 1

ARCH        := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS      := -Os -g -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter -ffunction-sections -fdata-sections \
               -flto -fuse-linker-plugin -fomit-frame-pointer \
               -fno-strict-aliasing $(ARCH) $(DEFINES)
CFLAGS      += $(INCLUDE) -DNDEBUG=1 -DCLOUDTUNE_VERSION=\"$(APP_VERSION)\" \
               -DNCM_CLIENT_LOG_NAME=\"ipc-client-overlay.log\"
CXXFLAGS    := $(CFLAGS) -std=gnu++20 -fno-exceptions
ASFLAGS     := -g $(ARCH)
LDFLAGS     := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) \
               -Wl,--gc-sections,-Map,$(OUTPUT_DIR)/$(TARGET).map
LIBS        := -lnx
LIBDIRS     := $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(CURDIR))

export OUTPUT_DIR
export OUTPUT := $(OUTPUT_DIR)/$(TARGET)
export TOPDIR := $(CURDIR)
export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD := $(CXX)
export OFILES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export NROFLAGS := --nacp=$(OUTPUT).nacp

.PHONY: all clean $(BUILD)

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@rm -rf "$(BUILD)" "$(OUTPUT).ovl" "$(OUTPUT).nacp" "$(OUTPUT).elf" \
	        "$(OUTPUT).map"

else

DEPENDS := $(OFILES:.o=.d)

.PHONY: all
all: $(OUTPUT).ovl

$(OUTPUT).ovl: $(OUTPUT).elf $(OUTPUT).nacp
	@elf2nro $< $@ $(NROFLAGS)
	@echo built ... $(notdir $@)

$(OUTPUT).elf: $(OFILES)

$(OFILES): $(OVERLAY_ROOT)/../CMakeLists.txt $(OVERLAY_ROOT)/Makefile

$(OUTPUT).nacp: $(OVERLAY_ROOT)/../CMakeLists.txt

-include $(DEPENDS)

endif
