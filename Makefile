# Firmware producer only; Linux, modules and rootfs are built by the consumer.
.DEFAULT_GOAL := help
.NOTPARALLEL:
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
include $(ROOT)/mk/config.mk
include $(ROOT)/mk/toolchains.mk
include $(ROOT)/mk/boot.mk
include $(ROOT)/mk/radio.mk
include $(ROOT)/mk/lp.mk
.PHONY: all help
all: radio-linux-payload lp-firmware uboot
help:
	@printf '%s\n' 'ESP32-S31 standalone firmware producer' '  make toolchain-fetch' '  make all: radio payload, LP firmware, OpenSBI and SPL/U-Boot' '  bash ci/setup.sh && bash ci/build.sh: verified input archive'
$(BUILD_DIR) $(OPENSBI_OUT) $(UBOOT_OUT) $(IMAGES_DIR) $(RADIO_OUT) $(GENERATED_DIR) $(STAGING_DIR) $(REPORTS_DIR):
	mkdir -p $@
