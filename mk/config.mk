include $(ROOT)/configs/build-versions.mk
-include $(ROOT)/local.mk
OUT_ROOT ?= $(ROOT)/out
BUILD_DIR := $(abspath $(OUT_ROOT))
CACHE_DIR ?= $(ROOT)/cache
JOBS ?= 4
TOOLCHAIN_DIR ?= $(CACHE_DIR)/toolchains
TOOLCHAIN_PREFIX ?= $(TOOLCHAIN_DIR)/riscv32-esp-linux-musl
CROSSTOOL_NG_DIR ?= $(abspath $(ROOT)/../crosstool-NG)
TOOLCHAIN_RELEASE_ASSET := riscv32-esp-linux-musl.tar.xz
TOOLCHAIN_RELEASE_REPOSITORY ?= GrieferPig/crosstool-NG-s31
TOOLCHAIN_RELEASE_API ?= https://api.github.com/repos/$(TOOLCHAIN_RELEASE_REPOSITORY)/releases/latest
TOOLCHAIN_RELEASE_DOWNLOAD_BASE ?= https://github.com/$(TOOLCHAIN_RELEASE_REPOSITORY)/releases/download
TOOLCHAIN_ARCHIVE := $(CACHE_DIR)/downloads/$(TOOLCHAIN_RELEASE_ASSET)
CROSS_COMPILE := $(TOOLCHAIN_PREFIX)/bin/riscv32-esp-linux-musl-
CC := $(CROSS_COMPILE)gcc
S31_SAFE_ISA := rv32imabc_zicsr_zifencei_zaamo_zalrsc_zba_zbb_zbc_zbs
OPENSBI_DIR := $(ROOT)/opensbi-esp32-s31
UBOOT_DIR := $(ROOT)/u-boot-esp32-s31
RADIO_IDF_DEPS_DIR := $(ROOT)/firmware/radio/idf_deps
OPENSBI_OUT := $(BUILD_DIR)/opensbi
UBOOT_OUT := $(BUILD_DIR)/u-boot
IMAGES_DIR := $(BUILD_DIR)/images
RADIO_OUT := $(BUILD_DIR)/radio
LP_OUT := $(BUILD_DIR)/lp
RADIO_IDF_BUILD := $(BUILD_DIR)/idf-radio
GENERATED_DIR := $(BUILD_DIR)/generated
STAGING_DIR := $(BUILD_DIR)/staging
REPORTS_DIR := $(BUILD_DIR)/reports
S31_LAYOUT_CFG := $(ROOT)/configs/esp32s31-layout.cfg
UBOOT_ITB := $(IMAGES_DIR)/u-boot.itb
UBOOT_SPL_DTB := $(IMAGES_DIR)/u-boot-spl-dtb.bin
SPL_APP_BIN := $(IMAGES_DIR)/spl_app.bin
IDF_ROOT ?= $(HOME)/.espressif
IDF_EXPORT ?= $(IDF_PATH)/export.sh
IDF_TOOLS_PATH ?= $(IDF_ROOT)
export IDF_TOOLS_PATH
ESP_TOOLS ?= $(IDF_TOOLS_PATH)/tools/riscv32-esp-elf/$(ESP_ELF_RELEASE)/riscv32-esp-elf
HOST_PYTHON ?= $(shell command -v python3)
UBOOT_PYTHONPATH ?=
SOURCE_DATE_EPOCH ?= $(shell git -C $(ROOT) log -1 --format=%ct)
export SOURCE_DATE_EPOCH
