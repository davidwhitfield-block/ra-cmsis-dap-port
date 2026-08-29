# Standalone GNU make build for the CMSIS_DAP_RA4M2 project.
#
# Mirrors the settings held in .cproject (e2 studio managed build) so the
# firmware can be built without the IDE. Flags, include paths, defines and
# the linker configuration are all taken from the Debug/Release configurations
# of the "GNU Tools for ARM Embedded Processors" toolchain entry there.
#
#   make            build Debug
#   make BUILD=Release
#   make flash      build then program over J-Link
#   make clean

PROJECT  = CMSIS_DAP_RA4M2
BUILD   ?= Debug

CROSS    = arm-none-eabi-
CC       = $(CROSS)gcc
OBJCOPY  = $(CROSS)objcopy
SIZE     = $(CROSS)size

# Target device (see configuration.xml #TargetName#)
DEVICE   = R7FA4M2AB

ARCH     = -mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16

DEFS     = -D_RA_CORE=CM33 -D_RENESAS_RA_ -D_RA_ORDINAL=1

INCS = \
  -I. \
  -Isrc \
  -Ira/fsp/inc \
  -Ira/fsp/inc/api \
  -Ira/fsp/inc/instances \
  -Ira/fsp/src/rm_freertos_port \
  -Ira/arm/CMSIS_5/CMSIS/Core/Include \
  -Ira_gen \
  -Ira_cfg/fsp_cfg/bsp \
  -Ira_cfg/fsp_cfg \
  -Ira_cfg/aws \
  -Ira/fsp/src/r_usb_basic/src/driver/inc \
  -Ira/aws/FreeRTOS/FreeRTOS/Source/include

# -w matches the per-folder "Other compiler flags" override in .cproject, which
# suppresses the warning set enabled at toolchain level.
COMMON   = $(ARCH) -O2 -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -g -w
CFLAGS   = $(COMMON) -std=c99 $(DEFS) $(INCS) -MMD -MP
# -L$(BUILD) lets fsp.ld resolve its "INCLUDE memory_regions.ld" against the
# generated copy; -Lscript resolves the -T fsp.ld itself.
LDFLAGS  = $(COMMON) -T fsp.ld -Lscript -L$(BUILD) -Xlinker --gc-sections \
           -Wl,-Map,$(BUILD)/$(PROJECT).map --specs=nano.specs --specs=rdimon.specs

MEMREGIONS = $(BUILD)/memory_regions.ld

# Source roots as listed in <sourceEntries>: ra, ra_gen, src (uart_thread_entry.c excluded).
#
# bsp_linker.c is additionally excluded: it arrived with the newer X2C Automation
# BSP pack, needs a generated bsp_linker_info.h that FSP 4.6 does not produce, and
# would duplicate the .option_setting_* arrays already emitted by the FSP 4.6
# bsp_rom_registers.c in ra/fsp/src/bsp/mcu/all.
SRCS := $(shell find ra ra_gen src -type f -name '*.c' \
          ! -name 'uart_thread_entry.c' ! -name 'bsp_linker.c' | sort)
OBJS := $(addprefix $(BUILD)/,$(SRCS:.c=.o))
DEPS := $(OBJS:.o=.d)

ELF  = $(BUILD)/$(PROJECT).elf
# .srec, not .bin: the image spans the option-setting region at 0x0100A200, so a
# flat binary would be padded out to ~16MB. This matches the "Create flash image"
# (-O srec) setting in .cproject.
SREC = $(BUILD)/$(PROJECT).srec
# Code-flash-only image, for `make flash`. See the comment on the flash target:
# this deliberately drops every section that lands outside code flash, so nothing
# can program the option-setting memory at 0x0100_A1xx.
BIN  = $(BUILD)/$(PROJECT).bin
NO_CFG_SECTIONS = -R .option_setting -R .option_setting_ns -R .option_setting_s \
                  -R .data_flash

.PHONY: all clean flash size
all: $(ELF) $(SREC) $(BIN) size

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "CC   $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(MEMREGIONS): .secure_rzone script/gen_memory_regions.py
	@mkdir -p $(dir $@)
	@echo "GEN  $@"
	@python3 script/gen_memory_regions.py .secure_rzone $@

$(ELF): $(OBJS) $(MEMREGIONS) script/fsp.ld
	@echo "LD   $@"
	@$(CC) $(LDFLAGS) -o $@ $(OBJS)

$(SREC): $(ELF)
	@$(OBJCOPY) -O srec $< $@

$(BIN): $(ELF)
	@$(OBJCOPY) -O binary $(NO_CFG_SECTIONS) $< $@

size: $(ELF)
	@$(SIZE) --format=berkeley $(ELF)

# Serial number of the J-Link to program through. Required when more than one
# probe is plugged in, otherwise JLinkExe cannot pick one and aborts.
#   make flash JLINK_SN=821000843
JLINK_SN ?=
JLINK_SEL = $(if $(JLINK_SN),-SelectEmuBySN $(JLINK_SN),)

JLINK_SCRIPT = script/ra4m2_flash.JLinkScript

# Programs a code-flash-ONLY .bin. Never generate the flat binary with a plain
# `objcopy -O binary` - that is what scrapped three boards.
#
# This ELF spans 0x0 to 0x0100_A2CC, so `objcopy -O binary` with no -R emits a
# 16 MB file and fills the gaps with 0x00 (objcopy's default, not 0xFF). The
# gap at 0x0100_A138-0x0100_A1FF is the non-secure option region, which holds
# BPS (0x0100_A1C0) and PBPS (0x0100_A1E0) - the block-protect and *permanent*
# block-protect settings for code flash. So the flat binary carries 0x00000000
# at both, and `loadbin <it> 0x0` programs them.
#
# Per R01UH0892EJ section 6.2.5 / table 6.2, a block whose BPS and PBPS bits are
# both 0 is "invalid permanently": every Program/Block Erase command-locks the
# sequencer (FSTATR.ILGCOMERR, FASTAT.CMDLK) regardless of FBPROT0/FBPROT1, PBPS
# can never be restored to 1, and the boot-mode initialize command refuses to
# run while any permanently locked block exists. No recovery - not RFP, not a
# DLM transition, not RMA. The part is scrap.
#
# The released CMSIS_DAP_RA4M2.bin (16,818,892 bytes) is byte-identical to a
# plain `objcopy -O binary` of the released ELF and contains exactly those
# zeros; D-PCD-0283 tells the factory to `loadbin` it at 0x0. Three of three
# boards checked came back with BPS = PBPS = 0 and 0x00 padding above the image
# (erased flash reads 0xFF, so 0x00 proves it was programmed, not blank).
#
# For the record, `loadfile` on the .elf and the .srec are both SAFE - neither
# format carries gap padding, so neither can reach 0x0100_A1C0. Only the flat
# binary is dangerous. The -R flags below are what make it safe.
flash: $(BIN)
	@printf 'device %s\nsi SWD\nspeed 1000\nconnect\nr\nh\nerase\nloadbin %s 0x0\nr\ng\nqc\n' \
	  "$(DEVICE)" "$(BIN)" > $(BUILD)/flash.jlink
	JLinkExe -nogui 1 -ExitOnError 1 $(JLINK_SEL) \
	  -JLinkScriptFile $(JLINK_SCRIPT) -CommanderScript $(BUILD)/flash.jlink

clean:
	rm -rf $(BUILD)

-include $(DEPS)
