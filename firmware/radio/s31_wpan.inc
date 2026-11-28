/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * The S31 TRM does not publish the 802.15.4 MAC register chapter.
 * Use IDF esp_ieee802154_enable(), which owns the ZB MAC, modem clocks,
 * RF calibration and coexistence setup with the other IDF frontends.
 */
static bool s31_wpan_enabled;
static volatile bool s31_wpan_tx_busy;
static uint32_t s31_wpan_tx_token;
static volatile bool s31_wpan_ed_busy;
static uint32_t s31_wpan_ed_token;
static uint8_t s31_wpan_tx[S31_WPAN_MAX_PSDU + 1] __attribute__((aligned(4)));

static void IRAM_ATTR s31_wpan_rx_done(uint8_t *frame, esp_ieee802154_frame_info_t *info)
{
	if (frame[0] >= S31_WPAN_FCS_BYTES && frame[0] <= S31_WPAN_MAX_PSDU)
		s31_radio_wifi_receive_aux(S31_RADIO_IF_WPAN_RX, frame + 1,
					   frame[0] - S31_WPAN_FCS_BYTES, info->lqi, info->rssi);
	esp_ieee802154_receive_handle_done(frame);
}

static void IRAM_ATTR s31_wpan_tx_event(uint8_t status)
{
	uint32_t token = s31_wpan_tx_token;

	s31_wpan_tx_busy = false;
	s31_radio_wifi_receive_aux(S31_RADIO_IF_WPAN_TX, (const uint8_t *)&token,
				   sizeof(token), status, 0);
}

static void IRAM_ATTR s31_wpan_tx_done(const uint8_t *frame, const uint8_t *ack,
				       esp_ieee802154_frame_info_t *info)
{
	if (ack)
		esp_ieee802154_receive_handle_done(ack);
	s31_wpan_tx_event(S31_WPAN_TX_OK);
}

static void IRAM_ATTR s31_wpan_tx_failed(const uint8_t *frame, esp_ieee802154_tx_error_t error)
{
	switch (error) {
	case ESP_IEEE802154_TX_ERR_CCA_BUSY:
		s31_wpan_tx_event(S31_WPAN_TX_CHANNEL_BUSY);
		break;
	case ESP_IEEE802154_TX_ERR_NO_ACK:
	case ESP_IEEE802154_TX_ERR_INVALID_ACK:
		s31_wpan_tx_event(S31_WPAN_TX_NO_ACK);
		break;
	default:
		s31_wpan_tx_event(S31_WPAN_TX_FAILED);
		break;
	}
}

static void IRAM_ATTR s31_wpan_ed_done(int8_t power)
{
	uint32_t token = s31_wpan_ed_token;

	s31_wpan_ed_busy = false;
	s31_radio_wifi_receive_aux(S31_RADIO_IF_WPAN_ED, (const uint8_t *)&token,
				   sizeof(token), 0, power);
}

static int s31_wpan_filter(const struct s31_wifi_control *r)
{
	int rc;

	rc = esp_ieee802154_set_panid(r->field);
	if (!rc)
		rc = esp_ieee802154_set_short_address(r->offset);
	if (!rc)
		rc = esp_ieee802154_set_extended_address(r->data);
	if (!rc)
		rc = esp_ieee802154_set_coordinator(r->total != 0);
	if (!rc)
		rc = esp_ieee802154_set_promiscuous(r->hidden != 0);
	return rc;
}

static int s31_wpan_start(const struct s31_wifi_control *r)
{
	const esp_ieee802154_event_cb_list_t callbacks = {
		.rx_done_cb = s31_wpan_rx_done,
		.tx_done_cb = s31_wpan_tx_done,
		.tx_failed_cb = s31_wpan_tx_failed,
		.ed_done_cb = s31_wpan_ed_done,
	};
	int rc;

	if (s31_wpan_enabled)
		return ESP_ERR_INVALID_STATE;
	if (r->channel < 11 || r->channel > 26)
		return ESP_ERR_INVALID_ARG;

	rc = esp_ieee802154_event_callback_list_register(callbacks);
	if (rc)
		return rc;
	rc = esp_ieee802154_enable();
	if (rc)
		goto disable;
	rc = esp_ieee802154_set_channel(r->channel);
	if (!rc)
		rc = s31_wpan_filter(r);
	if (!rc)
		rc = esp_ieee802154_set_rx_when_idle(true);
	if (!rc)
		rc = esp_ieee802154_receive();
	if (!rc) {
		s31_wpan_enabled = true;
		return 0;
	}

disable:
	esp_ieee802154_disable();
	esp_ieee802154_event_callback_list_unregister();
	return rc;
}

static int s31_wpan_stop(void)
{
	int rc;

	if (!s31_wpan_enabled)
		return 0;

	rc = esp_ieee802154_sleep();
	if (rc)
		return rc;
	rc = esp_ieee802154_disable();
	if (!rc) {
		s31_wpan_tx_busy = false;
		s31_wpan_ed_busy = false;
		s31_wpan_enabled = false;
		esp_ieee802154_event_callback_list_unregister();
	}
	return rc;
}

static int s31_radio_wpan_control_task(struct s31_wifi_control *r)
{
	int rc;

	if (r->operation == S31_RADIO_WPAN_CAP) {
		r->field = S31_RADIO_WPAN_ABI;
		return 0;
	}
	if (r->operation == S31_RADIO_WPAN_START)
		return s31_wpan_start(r);
	if (!s31_wpan_enabled)
		return ESP_ERR_INVALID_STATE;

	switch (r->operation) {
	case S31_RADIO_WPAN_STOP:
		return s31_wpan_stop();
	case S31_RADIO_WPAN_FILTER:
		return s31_wpan_filter(r);
	case S31_RADIO_WPAN_CHANNEL:
		if (r->channel < 11 || r->channel > 26)
			return ESP_ERR_INVALID_ARG;
		rc = esp_ieee802154_set_channel(r->channel);
		return rc ? rc : esp_ieee802154_receive();
	case S31_RADIO_WPAN_TX:
		if (!r->length || r->length > S31_WPAN_MAX_PSDU - S31_WPAN_FCS_BYTES)
			return ESP_ERR_INVALID_ARG;
		if (s31_wpan_tx_busy)
			return ESP_ERR_INVALID_STATE;
		s31_wpan_tx[0] = r->length + S31_WPAN_FCS_BYTES;
		memcpy(s31_wpan_tx + 1, r->data, r->length);
		s31_wpan_tx_token = r->total;
		s31_wpan_tx_busy = true;
		rc = esp_ieee802154_transmit(s31_wpan_tx, true);
		if (rc)
			s31_wpan_tx_busy = false;
		return rc;
	case S31_RADIO_WPAN_ED:
		if (s31_wpan_ed_busy)
			return ESP_ERR_INVALID_STATE;
		s31_wpan_ed_token = r->total;
		s31_wpan_ed_busy = true;
		rc = esp_ieee802154_energy_detect(8);
		if (rc)
			s31_wpan_ed_busy = false;
		return rc;
	default:
		return ESP_ERR_NOT_SUPPORTED;
	}
}
