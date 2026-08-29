# Standalone GNU make build for the CMSIS_DAP_RA4M2 project.
#
# Mirrors the settings held in .cproject (e2 studio managed build) so the
# firmware can be built without the IDE. Flags, include paths, defines and
# the linker configuration are all taken from the Debug/Release configurations
# of the "GNU Tools for ARM Embedded Processors" toolchain entry there.
#
#   make            build Debug
#   make BUILD=Release
#                   BUILD names the output directory only - no flag varies on it.
#                   That is faithful to .cproject, where the Debug and Release
#                   configurations are equivalent: both set optimization.level.more
#                   (-O2), the same empty debug-level option, the same defines, the
#                   same -w folder overrides, and neither defines NDEBUG. So no
#                   build system here produces a smaller or assert-free image under
#                   Release. scripts/build.sh hardcodes Debug/ and scripts/flash.sh
#                   flashes out of it, so Release is never exercised and the
#                   measured 313 KiB/s figures come from the Debug tree.
#   make flash      build then program over J-Link. PREFER scripts/flash.sh: the
#                   recipe below runs a bare `erase` (Erase Chip), which re-locks a
#                   board recovered by tools/recovery/recover.sh. See the comment
#                   on the flash target.
#   make clean
#
# Prerequisites:
#   arm-none-eabi-gcc  Arm GNU toolchain. The IDE project records 10.3.1.20210824;
#                      the checked-in Debug/ tree was built with a much newer one.
#                      Both work.
#   python3            Generates $(BUILD)/memory_regions.ld from .secure_rzone; see
#                      the $(MEMREGIONS) rule below. Without it the build stops at
#                      the GEN step with "python3: command not found", before any
#                      link. scripts/build.sh checks only for arm-none-eabi-gcc, so
#                      it does not catch this.
#   JLinkExe           Programming only: `make flash`, scripts/flash.sh and
#                      tools/recovery/recover.sh.
#
# The Block X2C_AUTOMATION BSP pack (Block.X2C_AUTOMATION.6.2.0.pack, named in
# configuration.xml) is NOT needed to build from the command line, despite what
# commit fd21f64's subject line implies. Its whole payload - ra/board/x2c_automation/
# {board.h,board_init.c,board_init.h,board_leds.c,board_leds.h} - is committed, as
# are ra_gen/ and ra_cfg/, and every include path below is repo-relative. The pack
# is required only to open this project in e2 studio and re-run RA content
# generation.

PROJECT  = CMSIS_DAP_RA4M2
BUILD   ?= Debug

CROSS    = arm-none-eabi-
CC       = $(CROSS)gcc
OBJCOPY  = $(CROSS)objcopy
SIZE     = $(CROSS)size

# J-Link device name, from configuration.xml's #DeviceCommand# - NOT the orderable
# part number in #TargetName# (R7FA4M2AB3CNE), which J-Link's device database does
# not contain. Used only to emit `device $(DEVICE)` for JLinkExe. R7FA4M2AB is the
# 256 KB variant, and tools/recovery/recover.sh derives the BPS_SEL mask
# (0xFFFFC000, blocks 0-13) from this same name. Keep it in step with
# scripts/flash.sh --device.
DEVICE   = R7FA4M2AB

ARCH     = -mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16

DEFS     = -D_RA_CORE=CM33 -D_RENESAS_RA_ -D_RA_ORDINAL=1

# CMSIS_5, not CMSIS_6. ra/arm/ holds two CMSIS trees. The X2C Automation BSP pack
# added CMSIS_6 in fd21f64 and left CMSIS_5 in place. Nothing references CMSIS_6 -
# not the include path below, not .cproject, which names CMSIS_5 in all four
# toolchain configurations - and `find ra/arm -name '*.c'` is empty, so it
# contributes no objects to either build. It is inert. This is FSP 4.6.0 with
# Arm.CMSIS5.5.9.0+renesas.0.fsp.4.6.0; building against CMSIS_6 has never been
# tried. Same provenance as the bsp_linker.c exclusion further down - both arrived
# with that BSP pack.
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

# -w here applies to EVERY translation unit, which is NOT what .cproject does.
# There, "Other compiler flags = -w" is set only on the ra/aws and ra/arm
# folderInfo blocks. Every other folder - ra/fsp, ra_gen and all of src/ - inherits
# the toolchain-level warning set: -Wall -Wextra -Wunused -Wuninitialized
# -Wmissing-declarations -Wconversion -Wpointer-arith -Wshadow -Wlogical-op
# -Waggregate-return -Wfloat-equal.
#
# So this build is quieter than the IDE build, and says nothing at all about our own
# code in src/ - including src/dap_thread_entry.c and the locally modified
# src/CMSIS-DAP. That is how a -Wconversion or -Wshadow bug in the DAP path ships
# unnoticed. To restore parity, move -w out of COMMON and onto the vendor trees
# only, e.g. a `$(BUILD)/ra/%.o $(BUILD)/ra_gen/%.o: WARN := -w` target-specific
# override with the warning list in CFLAGS. Note ra/arm has no .c files, so
# silencing it is a no-op either way. This has not been tried; expect a first-run
# backlog in src/CMSIS-DAP under -Wconversion.
COMMON   = $(ARCH) -O2 -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -g -w
CFLAGS   = $(COMMON) -std=c99 $(DEFS) $(INCS) -MMD -MP
# -Lscript resolves `-T fsp.ld`; -L$(BUILD) resolves fsp.ld's first statement,
# `INCLUDE memory_regions.ld`.
#
# The order below is a latent hazard. ld resolves an INCLUDE name against the
# current directory and then each -L in command-line order, and -Lscript is first.
# e2 studio's RA content generation drops its own memory_regions.ld into script/ -
# that directory is the only "Library search path (-L)" entry in .cproject, because
# the IDE expects it there. Such a file would silently win over the copy regenerated
# below from .secure_rzone: wrong FLASH/RAM lengths, a link that still succeeds, and
# an image sized for the wrong part. Putting -L$(BUILD) first fixes it; the repo's
# .gitignore now lists ra4m2/script/memory_regions.ld so at least it cannot be
# committed by accident. (A memory_regions.ld dropped in this directory beats both
# -L paths regardless.)
#
# --specs=rdimon.specs mirrors .cproject (c.linker.other, both configurations). It
# appends -lrdimon after the objects and force-links rdimon-crt0.o as the startfile.
# librdimon is Arm semihosting: its stubs issue BKPT 0xAB, which on a probe running
# standalone with no debugger attached escalates to HardFault instead of returning,
# so any libc path that reaches a syscall would kill the board.
#
# It is inert in this build, verified on the linked ELF: FSP defines its own _sbrk
# (ra/fsp/src/bsp/mcu/all/bsp_sbrk.c), and --gc-sections discards rdimon-crt0.o and
# every librdimon_nano.a member - they appear only in the map's "Discarded input
# sections" block, and `objdump -d ... | grep -c 'bkpt.*0x00ab'` returns 0. Dropping
# --gc-sections would drag them all back in. Kept for IDE parity;
# --specs=nosys.specs is the safer equivalent if libc ever reaches a real syscall.
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

# e2 studio emits memory_regions.ld as part of RA content generation, so it is not
# checked in. Regenerate it from the same source of truth the configurator uses: the
# <memories> block of .secure_rzone, which carries base and size for the configured
# part (FLASH 0x00000000/0x40000, RAM 0x20000000/0x20000, OPTION_SETTING
# 0x0100A100/0x100, OPTION_SETTING_S 0x0100A200/0x100 for R7FA4M2AB3CNE). Generated
# into $(BUILD), not script/ - see the -L note above.
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
# `objcopy -O binary` - that is what block-protected three boards.
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
# run while any permanently locked block exists.
#
# THAT IS TRUE ONLY OF THE *APPLIED* REGISTER PAIR, AND THE PART IS RECOVERABLE.
# BPS_SEL (0x0100_A2C0) selects, per block, whether the applied settings come from
# the poisoned non-secure pair or the pristine secure pair (BPS_SEC 0x0100_A240 /
# PBPS_SEC 0x0100_A260). Writing it redirects to blank settings; Table 44.18 lists
# BPS_SEL as plain Writable with no "cannot be restored to 1" note, and Table
# 44.22's write freeze covers only BPS[n] and PBPS[n]. BPS/PBPS stay 0x00000000
# forever and are simply never consulted again, so a recovered board reads
# BPS = PBPS = 0 permanently - a healthy steady state, not a fault. See
# tools/recovery/README.md and tools/recovery/recover.sh. FBPROT cancellation,
# boot-mode Initialize, RMA and DLM transitions all do NOT work. The only
# unrecoverable case is BPS_SEC/PBPS_SEC not reading FFFFFFFF, which means
# replacing the IC.
#
# The released CMSIS_DAP_RA4M2.bin (16,818,892 bytes) is byte-identical to a
# plain `objcopy -O binary` of the released ELF and contains exactly those
# zeros; D-PCD-0283 tells the factory to `loadbin` it at 0x0. Three of three
# boards checked came back with BPS = PBPS = 0 and 0x00 padding above the image
# (erased flash reads 0xFF, so 0x00 proves it was programmed, not blank).
#
# `loadfile` on the .elf and the .srec cannot reach BPS or PBPS: .option_setting is
# only 0x38 bytes (0x0100_A100-0x0100_A137) and .option_setting_ns is zero length,
# and neither format carries gap padding. That is the whole of the guarantee - and
# it does NOT extend to a *recovered* board. fsp.ld places .option_setting_bps_sel0
# at 0x0100_A2C0 = BPS_SEL, inside the loadable .option_setting_s section, and FSP
# emits a variable there: bsp_rom_registers.c defines g_bsp_rom_bps_sel0 =
# BSP_CFG_ROM_REG_BPS_SEL0 = 0xFFFFFFFF. Both artefacts carry it -
#   arm-none-eabi-nm ...elf | grep bps_sel0  ->  0100a2c0 r g_bsp_rom_bps_sel0
#   grep 0100A2C0 ...srec                    ->  S3110100A2C0FFFFFFFF...
# and 0xFFFFFFFF is precisely what re-selects the poisoned non-secure pair. Read
# from the linker script, nm and objdump on Debug/*.elf and Debug/*.srec;
# deliberately NOT tested on hardware, since there is only one flashable board.
#
# The -R'd .bin above is the only safe *image*. Note the recipe below still runs a
# bare `erase`, which re-locks a recovered board whatever image follows it.
#
# SECOND, SEPARATE HAZARD - this recipe's `erase` is J-Link's Erase Chip, which also
# erases the config area, blanking BPS_SEL back to 0xFFFFFFFF. On a board recovered
# through the BPS_SEL redirect that re-locks it on the spot: BPS/PBPS are still
# 0x00000000 and become the applied pair again at the next reset, which the `r` two
# commands later provides. Unlike the flat-binary trap above, this one IS
# recoverable - re-run tools/recovery/recover.sh, which is idempotent. `loadbin`
# erases only the sectors it is about to write, so the chip erase buys nothing here;
# on a still-poisoned part it just fails with "Failed to erase sectors". PREFER
# scripts/flash.sh, which uses loadbin and re-reads BPS_SEL afterwards. See
# tools/recovery/README.md ("Traps").
flash: $(BIN)
	@printf 'device %s\nsi SWD\nspeed 1000\nconnect\nr\nh\nerase\nloadbin %s 0x0\nr\ng\nqc\n' \
	  "$(DEVICE)" "$(BIN)" > $(BUILD)/flash.jlink
	JLinkExe -nogui 1 -ExitOnError 1 $(JLINK_SEL) \
	  -JLinkScriptFile $(JLINK_SCRIPT) -CommanderScript $(BUILD)/flash.jlink

clean:
	rm -rf $(BUILD)

-include $(DEPS)
