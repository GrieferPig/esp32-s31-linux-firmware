.PHONY: lp-firmware
lp-firmware: idf-check | $(STAGING_DIR)
	bash -c 'source "$(IDF_EXPORT)" >/dev/null && $(MAKE) -C "$(ROOT)/firmware/lp" O="$(LP_OUT)" IDF_PATH="$$IDF_PATH" ESP_TOOLS="$(ESP_TOOLS)" STAGE_DIR="$(STAGING_DIR)/overlay/lib/firmware/esp32s31" stage'
