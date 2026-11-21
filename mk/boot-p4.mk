# ESP32-P4 rev1.3. The consumer packs U-Boot proper into its Linux FIT.
P4_UBOOT_OUT := $(BUILD_DIR)/u-boot-p4
P4_CROSS_COMPILE ?= $(ESP_TOOLS)/bin/riscv32-esp-elf-
P4_ESPTOOL ?= esptool
P4_UBOOT_PROPER := $(P4_UBOOT_OUT)/u-boot.bin
P4_SPL_PAYLOAD := $(P4_UBOOT_OUT)/spl/u-boot-spl-dtb.bin
P4_SPL_IMAGE := $(P4_UBOOT_OUT)/spl-p4-200.bin
# TRM ch.9 System and Memory: both entries are in HP SRAM.
P4_SPL_ENTRY := 0x4ff2b000
P4_UBOOT_ENTRY := 0x4ff00000

.PHONY: p4-uboot
p4-uboot: | $(IMAGES_DIR)
	@echo "--- ESP32-P4 rev1.3 SPL + U-Boot proper ---"
	@test -f "$(UBOOT_DIR)/configs/espressif_esp32p4_defconfig" || { \
		echo 'ERROR: run git submodule update --init --depth 1 u-boot-esp32-s31' >&2; exit 1; }
	command -v "$(P4_CROSS_COMPILE)gcc"
	command -v "$(P4_ESPTOOL)"
	python3 "$(ROOT)/tools/build/configure.py" native --kind uboot \
		--source "$(UBOOT_DIR)" --output "$(P4_UBOOT_OUT)" \
		--defconfig espressif_esp32p4_defconfig \
		--fragment "$(UBOOT_DIR)/board/espressif/esp32p4/psram200.config" \
		--compiler "$$(command -v "$(P4_CROSS_COMPILE)gcc")" --cross "$(P4_CROSS_COMPILE)"
	grep -Fx 'CONFIG_ESP32P4_SPL_PSRAM_200MHZ=y' "$(P4_UBOOT_OUT)/.config"
	PYTHONPATH="$(UBOOT_PYTHONPATH)$${PYTHONPATH:+:$$PYTHONPATH}" \
		python3 "$(ROOT)/tools/build/host_python.py" --python "$(HOST_PYTHON)" \
		--shim "$(P4_UBOOT_OUT)/.host-python" -- \
		$(MAKE) -C "$(UBOOT_DIR)" O="$(P4_UBOOT_OUT)" ARCH=riscv \
		CROSS_COMPILE="$(P4_CROSS_COMPILE)" PYTHON="$(HOST_PYTHON)" \
		PYTHON3="$(HOST_PYTHON)" -j$(JOBS)
	@set -eu; \
	for entry in "$(P4_UBOOT_ENTRY):u-boot" "$(P4_SPL_ENTRY):spl/u-boot-spl"; do \
		address=$${entry%%:*}; elf="$(P4_UBOOT_OUT)/$${entry#*:}"; \
		actual=$$("$(P4_CROSS_COMPILE)nm" "$$elf" | awk '$$3 == "_start" {print "0x" $$1}'); \
		if [ "$$actual" != "$$address" ]; then \
			echo "ERROR: $$elf entry $$actual differs from $$address" >&2; exit 1; \
		fi; \
	done
	"$(P4_CROSS_COMPILE)objcopy" -I binary -O elf32-littleriscv -B riscv \
		"$(P4_SPL_PAYLOAD)" "$(P4_UBOOT_OUT)/p4-spl-payload.elf"
	"$(P4_CROSS_COMPILE)objcopy" --change-section-address ".data=$(P4_SPL_ENTRY)" \
		--rename-section .data=.text,alloc,load,readonly,code,contents \
		--set-start "$(P4_SPL_ENTRY)" "$(P4_UBOOT_OUT)/p4-spl-payload.elf" \
		"$(P4_UBOOT_OUT)/p4-spl-rom.elf"
	"$(P4_ESPTOOL)" --chip esp32p4 elf2image --flash-mode dio --flash-freq 40m \
		--flash-size 16MB --min-rev-full 103 --max-rev-full 103 \
		--output "$(P4_SPL_IMAGE)" "$(P4_UBOOT_OUT)/p4-spl-rom.elf"
	python3 "$(ROOT)/tools/checks/p4_boot.py" "$(P4_SPL_IMAGE)" "$(P4_UBOOT_PROPER)"
	cp "$(P4_SPL_IMAGE)" "$(IMAGES_DIR)/spl-p4-200.bin"
	cp "$(P4_UBOOT_PROPER)" "$(IMAGES_DIR)/uboot-p4.bin"
