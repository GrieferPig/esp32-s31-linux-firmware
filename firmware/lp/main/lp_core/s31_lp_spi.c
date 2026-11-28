/* SPDX-License-Identifier: MIT */
#include <stdbool.h>
#include <stdint.h>

#include "hal/clk_tree_ll.h"
#include "hal/lp_spi_ll.h"
#include "hal/rtc_io_ll.h"
#include "soc/lp_gpio_sig_map.h"
#include "ulp_lp_core_lp_timer_shared.h"
#include "s31_lp_spi.h"

#define SPI_TIMEOUT_US 20000U
#define SPI_MODE_MASK (S31_LP_SPI_CPHA | S31_LP_SPI_CPOL | S31_LP_SPI_CS_HIGH | \
		       S31_LP_SPI_LSB_FIRST)

static lp_spi_ll_dev_t * const bus = LP_SPI_LL_GET_HW();
static uint32_t pin_mask;
static uint32_t pins;
static uint32_t cs_pin;
static uint32_t applied_mode;
static bool selected;
static bool initialized;

_Static_assert(S31_LP_SPI_CHUNK_BYTES <= LP_SPI_LL_MAX_BUFFER_SIZE, "LP SPI buffer size");

uint32_t s31_lp_spi_pins(void)
{
	return pin_mask;
}

static uint32_t clock_source(void)
{
	/* Same sources as ESP-IDF esp_clk_tree_lp_fast_get_freq_hz(). */
	switch (clk_ll_rtc_fast_get_src()) {
	case SOC_RTC_FAST_CLK_SRC_XTAL:
		return clk_ll_xtal_get_freq_mhz() * 1000000U;
	case SOC_RTC_FAST_CLK_SRC_RC_FAST:
		return SOC_CLK_RC_FAST_FREQ_APPROX;
	default:
		return 0;
	}
}

static uint32_t set_speed(uint32_t speed)
{
	uint32_t source = clock_source();
	uint32_t div, pre, n;
	lp_spi_clock_reg_t clock = { .val = 0 };

	if (!source)
		return 0;

	/* LP SPI clock register: 4-bit pre-divider and 6-bit period. */
	div = (source + speed - 1) / speed;
	if (div < 2)
		div = 2;
	pre = (div + 63) / 64;
	n = (div + pre - 1) / pre;
	if (pre > 16 || n < 2 || n > 64)
		return 0;

	clock.reg_clkdiv_pre = pre - 1;
	clock.reg_clkcnt_n = n - 1;
	clock.reg_clkcnt_l = n - 1;
	clock.reg_clkcnt_h = n / 2 - 1;
	lp_spi_ll_set_clock_val(bus, clock.val);
	return source / (pre * n);
}

static void select_device(bool active, uint32_t mode)
{
	rtcio_ll_set_level(cs_pin, active == !!(mode & S31_LP_SPI_CS_HIGH));
	selected = active;
}

void s31_lp_spi_release(void)
{
	if (selected)
		select_device(false, applied_mode);
}

static void configure_pad(uint32_t pin)
{
	rtcio_ll_output_disable(pin);
	rtcio_ll_function_select(pin, RTCIO_LL_FUNC_RTC);
	rtcio_ll_iomux_func_sel(pin, RTCIO_LL_PIN_FUNC);
	rtcio_ll_pullup_disable(pin);
	rtcio_ll_pulldown_disable(pin);
	rtcio_ll_output_mode_set(pin, RTCIO_LL_OUTPUT_NORMAL);
}

static void configure_outputs(uint32_t packed, uint32_t mode)
{
	uint32_t sclk = packed & 0xff;
	uint32_t mosi = (packed >> 8) & 0xff;
	uint32_t miso = (packed >> 16) & 0xff;

	/* TRM ch.5.3 LP GPIO Matrix and ESP-IDF lp_spi_bus_init_io(). */
	configure_pad(cs_pin);
	select_device(false, mode);
	rtcio_ll_set_output_enable_ctrl(cs_pin, false, false);
	rtcio_ll_output_enable(cs_pin);

	configure_pad(miso);
	rtcio_ll_input_enable(miso);
	rtcio_ll_set_input_signal_matrix_source(miso, LP_SPI_Q_PAD_IN_IDX, false);
	configure_pad(sclk);
	rtcio_ll_set_output_signal_matrix_source(sclk, LP_SPI_CK_PAD_OUT_IDX, false);
	rtcio_ll_set_output_enable_ctrl(sclk, true, false);
	configure_pad(mosi);
	rtcio_ll_set_output_signal_matrix_source(mosi, LP_SPI_D_PAD_OUT_IDX, false);
	rtcio_ll_set_output_enable_ctrl(mosi, true, false);
}

static bool apply_config(void)
{
	uint32_t start = ulp_lp_core_lp_timer_get_cycle_count();
	uint32_t ticks = ulp_lp_core_lp_timer_calculate_sleep_ticks(SPI_TIMEOUT_US);

	/* ESP-IDF lp_spi_ll_apply_config(): wait for register-domain update. */
	bus->spi_cmd.reg_update = 1;
	while (bus->spi_cmd.reg_update) {
		if ((uint32_t)(ulp_lp_core_lp_timer_get_cycle_count() - start) >= ticks)
			return false;
	}
	return true;
}

static uint32_t configure(uint32_t packed, uint32_t claimed, uint32_t mode, uint32_t speed,
			  uint32_t *effective)
{
	bool cpol = mode & S31_LP_SPI_CPOL;
	bool cpha = mode & S31_LP_SPI_CPHA;
	bool lsb = mode & S31_LP_SPI_LSB_FIRST;

	if (pin_mask && pins != packed)
		return S31_LP_RPC_BUSY;

	if (!initialized) {
		lp_spi_ll_enable_clock(bus);
		LP_PERI_CLKRST.iomux_ctrl.lp_iomux_clk_en = 1;
		LP_GPIO.clock_gate.clk_en = 1;
		if (!LP_PERI_CLKRST.iomux_ctrl.lp_iomux_clk_en || !LP_GPIO.clock_gate.clk_en)
			return S31_LP_RPC_IO;
		lp_spi_ll_set_slave_mode(bus, false);
		lp_spi_ll_set_slave_clk_mode(bus, 0);
		lp_spi_ll_reset_cs_timing(bus);
		lp_spi_ll_disable_highpart(bus);
		lp_spi_ll_set_command_en(bus, false);
		lp_spi_ll_set_address_en(bus, false);
		lp_spi_ll_set_dummy_en(bus, false);
		lp_spi_ll_set_full_duplex(bus, true);
		lp_spi_ll_set_sio_mode(bus, false);
		lp_spi_ll_enable_cs0(bus);
	}
	if (selected && mode != applied_mode)
		return S31_LP_RPC_BUSY;

	*effective = set_speed(speed);
	if (!*effective)
		return S31_LP_RPC_INVALID;
	lp_spi_ll_set_ck_idle_edge(bus, cpol);
	lp_spi_ll_set_ck_out_edge(bus, cpol != cpha);
	lp_spi_ll_set_bit_order(bus, lsb, lsb);

	if (!apply_config())
		return S31_LP_RPC_TIMEOUT;
	initialized = true;
	if (!pin_mask) {
		cs_pin = packed >> 24;
		configure_outputs(packed, mode);
		pins = packed;
		pin_mask = claimed;
	} else if (!selected && mode != applied_mode) {
		select_device(false, mode);
	}
	applied_mode = mode;
	return S31_LP_RPC_OK;
}

static uint32_t shift(volatile struct s31_lp_rpc_slot *slot)
{
	uint32_t written = slot->input_len, read = slot->output_len;
	uint32_t count = written ? written : read;
	uint32_t start, ticks;

	lp_spi_ll_reset_fifos(bus);
	lp_spi_ll_clear_int_trans_done(bus);
	lp_spi_ll_set_mosi_en(bus, true);
	lp_spi_ll_set_miso_en(bus, read != 0);
	lp_spi_ll_set_data_bitlen(bus, count * 8 - 1);
	for (uint32_t i = 0; i < count; i += 4) {
		uint32_t word = 0;

		for (uint32_t j = 0; j < 4 && i + j < written; j++)
			word |= (uint32_t)slot->data[i + j] << (j * 8);
		lp_spi_ll_write_buffer_word(bus, i / 4, word);
	}

	/* IDF lp_core_lp_spi_master_transfer() synchronizes configuration first. */
	if (!apply_config())
		return S31_LP_RPC_TIMEOUT;
	ticks = ulp_lp_core_lp_timer_calculate_sleep_ticks(SPI_TIMEOUT_US);
	start = ulp_lp_core_lp_timer_get_cycle_count();
	lp_spi_ll_start_user_transaction(bus);
	while (!lp_spi_ll_get_int_trans_done(bus) || lp_spi_ll_is_busy(bus)) {
		if ((uint32_t)(ulp_lp_core_lp_timer_get_cycle_count() - start) >= ticks)
			return S31_LP_RPC_TIMEOUT;
	}

	for (uint32_t i = 0; i < read; i += 4) {
		uint32_t word = lp_spi_ll_read_buffer_word(bus, i / 4);

		for (uint32_t j = 0; j < 4 && i + j < read; j++)
			slot->data[i + j] = word >> (8 * j);
	}
	lp_spi_ll_clear_int_trans_done(bus);
	return S31_LP_RPC_OK;
}

uint32_t s31_lp_spi_transfer(volatile struct s31_lp_rpc_slot *slot, uint32_t owned)
{
	uint32_t packed = slot->args[0], speed = slot->args[1], mode = slot->args[2];
	uint32_t action = slot->args[3], written = slot->input_len, read = slot->output_len;
	uint32_t claimed = 0, effective, result;

	if (action > S31_LP_SPI_RELEASE || mode & ~SPI_MODE_MASK)
		return S31_LP_RPC_INVALID;
	if (action != S31_LP_SPI_CLOCK && (written || read))
		return S31_LP_RPC_INVALID;
	if (action == S31_LP_SPI_RELEASE) {
		if (pin_mask && pins != packed)
			return S31_LP_RPC_INVALID;
		s31_lp_spi_release();
		return S31_LP_RPC_OK;
	}
	if (speed < S31_LP_SPI_MIN_HZ || speed > S31_LP_SPI_MAX_HZ ||
	    written > S31_LP_SPI_CHUNK_BYTES || read > S31_LP_SPI_CHUNK_BYTES ||
	    (written && read && written != read) ||
	    (action == S31_LP_SPI_CLOCK && !written && !read))
		return S31_LP_RPC_INVALID;
	for (uint32_t i = 0; i < 32; i += 8) {
		uint32_t pin = (packed >> i) & 0xff;

		if (pin >= S31_LP_GPIO_COUNT || (claimed & (1U << pin)))
			return S31_LP_RPC_INVALID;
		claimed |= 1U << pin;
	}
	if (claimed & owned)
		return S31_LP_RPC_BUSY;

	result = configure(packed, claimed, mode, speed, &effective);
	if (result == S31_LP_RPC_OK) {
		slot->args[1] = effective;
		if (action == S31_LP_SPI_SELECT)
			select_device(true, mode);
		else
			result = shift(slot);
	}
	if (result == S31_LP_RPC_TIMEOUT) {
		s31_lp_spi_release();
		/* SPI_SLAVE.SOFT_RESET is only writable in CONF state. */
		LP_PERI_CLKRST.spi_ctrl.lp_spi_rst_en = 1;
		LP_PERI_CLKRST.spi_ctrl.lp_spi_rst_en = 0;
		initialized = false;
	}
	return result;
}
