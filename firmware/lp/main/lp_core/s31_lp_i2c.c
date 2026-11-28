/* SPDX-License-Identifier: MIT */
#include <stdbool.h>
#include <stdint.h>

#include "hal/clk_tree_ll.h"
#include "hal/i2c_ll.h"
#include "hal/rtc_io_ll.h"
#include "soc/lp_gpio_sig_map.h"
#include "ulp_lp_core_lp_timer_shared.h"
#include "s31_lp_i2c.h"

#define STRETCH_US 25000U
#define TRANSFER_US 50000U
#define I2C_STATUS (I2C_LL_INTR_NACK | I2C_LL_INTR_ARBITRATION | \
		    I2C_LL_INTR_TIMEOUT | I2C_LL_INTR_MST_COMPLETE)

static i2c_dev_t * const bus = &LP_I2C;
static uint32_t pin_mask;
static uint32_t configured_pins;
static uint32_t configured_hz;
static uint32_t configured_flags;

_Static_assert(S31_LP_I2C_FIFO_BYTES == I2C_LL_LP_FIFO_LEN, "LP I2C FIFO size");
_Static_assert(I2C_LL_CMD_REG_NUM >= 7, "LP I2C command count");

uint32_t s31_lp_i2c_pins(void)
{
	return pin_mask;
}

static void configure_pad(uint32_t pin, uint32_t signal, bool pullup)
{
	/*
	 * TRM ch.5.3 LP IO MUX and GPIO Matrix. Follow lp_i2c_configure_io()
	 * and rtcio_hal_matrix_out()/rtcio_hal_matrix_in() from ESP-IDF.
	 * Set the GPIO latch high before handing output enable to I2C.
	 */
	rtcio_ll_set_level(pin, 1);
	rtcio_ll_function_select(pin, RTCIO_LL_FUNC_RTC);
	rtcio_ll_input_enable(pin);
	rtcio_ll_output_mode_set(pin, RTCIO_LL_OUTPUT_OD);
	rtcio_ll_pulldown_disable(pin);
	if (pullup)
		rtcio_ll_pullup_enable(pin);
	else
		rtcio_ll_pullup_disable(pin);
	rtcio_ll_iomux_func_sel(pin, RTCIO_LL_PIN_FUNC);
	rtcio_ll_set_input_signal_matrix_source(pin, signal, false);
	rtcio_ll_set_output_signal_matrix_source(pin, signal, false);
	rtcio_ll_set_output_enable_ctrl(pin, true, false);
}

static uint32_t configure(uint32_t pins, uint32_t hz, uint32_t flags)
{
	i2c_hal_clk_config_t timing = { 0 };
	uint32_t frequency = clk_ll_xtal_get_freq_mhz() * 1000000U;

	if (pin_mask) {
		if (pins == configured_pins && hz == configured_hz && flags == configured_flags)
			return S31_LP_RPC_OK;
		return S31_LP_RPC_BUSY;
	}
	if (frequency < hz * 20U)
		return S31_LP_RPC_IO;

	/*
	 * TRM ch.26 LP_PERICLKRST_LP_I2C_CLK_SEL assigns 0 to XTAL.
	 * ESP-IDF v6.1 lp_i2c_ll_set_source_clk() lists the opposite mapping.
	 * Use the TRM mapping and the IDF LL timing calculation.
	 */
	lp_i2c_ll_enable_bus_clock(0, true);
	lp_i2c_ll_reset_register(0);
	LP_PERI_CLKRST.i2c_ctrl.lp_i2c_clk_sel = 0;
	i2c_ll_set_mode(bus, I2C_BUS_MODE_MASTER);
	i2c_ll_enable_arbitration(bus, true);
	i2c_ll_enable_pins_open_drain(bus, true);
	i2c_ll_set_data_mode(bus, I2C_DATA_MODE_MSB_FIRST, I2C_DATA_MODE_MSB_FIRST);
	i2c_ll_master_rx_full_ack_level(bus, true);
	i2c_ll_master_set_filter(bus, 7);
	i2c_ll_master_cal_bus_clk(frequency, hz, &timing);
	i2c_ll_master_set_bus_timing(bus, &timing);
	i2c_ll_set_tout(bus, i2c_ll_calculate_timeout_us_to_reg_val(
		frequency / timing.clkm_div, STRETCH_US));
	i2c_ll_update(bus);

	/* Apply the gates in ESP-IDF _rtcio_ll_enable_io_clock(). */
	LP_PERI_CLKRST.iomux_ctrl.lp_iomux_clk_en = 1;
	LP_GPIO.clock_gate.clk_en = 1;
	if (!LP_PERI_CLKRST.iomux_ctrl.lp_iomux_clk_en || !LP_GPIO.clock_gate.clk_en)
		return S31_LP_RPC_IO;
	/* ESP-IDF lp_i2c_set_pin() configures SCL before SDA. */
	configure_pad(pins >> 8, LP_I2C_SCL_PAD_OUT_IDX, flags & S31_LP_I2C_PULLUP);
	configure_pad(pins & 0xff, LP_I2C_SDA_PAD_OUT_IDX, flags & S31_LP_I2C_PULLUP);
	configured_pins = pins;
	configured_hz = hz;
	configured_flags = flags;
	pin_mask = (1U << (pins >> 8)) | (1U << (pins & 0xff));
	return S31_LP_RPC_OK;
}

static void command(uint32_t index, uint32_t opcode, uint32_t bytes, bool nack)
{
	i2c_ll_hw_cmd_t cmd = {
		.op_code = opcode,
		.byte_num = bytes,
		.ack_en = opcode == I2C_LL_CMD_WRITE,
		.ack_val = nack,
	};

	i2c_ll_master_write_cmd_reg(bus, cmd, index);
}

static uint32_t wait_for_stop(uint32_t stop)
{
	uint32_t status, result = S31_LP_RPC_TIMEOUT;
	uint32_t ticks = ulp_lp_core_lp_timer_calculate_sleep_ticks(TRANSFER_US);
	uint32_t start = ulp_lp_core_lp_timer_get_cycle_count();

	do {
		i2c_ll_get_intr_raw_mask(bus, &status);
		if (status & I2C_LL_INTR_ARBITRATION) {
			result = S31_LP_RPC_ARBITRATION;
			break;
		}
		if (status & I2C_LL_INTR_TIMEOUT)
			break;
		if (status & I2C_LL_INTR_NACK) {
			result = S31_LP_RPC_NACK;
			break;
		}
		if (status & I2C_LL_INTR_MST_COMPLETE) {
			result = i2c_ll_master_is_cmd_done(bus, stop) ? S31_LP_RPC_OK : S31_LP_RPC_IO;
			break;
		}
	} while ((uint32_t)(ulp_lp_core_lp_timer_get_cycle_count() - start) < ticks);

	if (result == S31_LP_RPC_TIMEOUT || result == S31_LP_RPC_IO)
		i2c_ll_master_fsm_rst(bus);
	i2c_ll_clear_intr_mask(bus, I2C_STATUS);
	return result;
}

uint32_t s31_lp_i2c_transfer(volatile struct s31_lp_rpc_slot *slot)
{
	uint32_t addr = slot->args[0], pins = slot->args[1], hz = slot->args[2];
	uint32_t sda = pins & 0xff, scl = pins >> 8;
	uint32_t flags = slot->args[3];
	uint32_t written = slot->input_len, read = slot->output_len;
	uint32_t result, next = 0, bytes = 0;
	uint8_t data[S31_LP_I2C_FIFO_BYTES];

	if (addr > 0x7f || sda >= S31_LP_GPIO_COUNT || scl >= S31_LP_GPIO_COUNT ||
	    sda == scl || flags & ~S31_LP_I2C_PULLUP ||
	    (hz != S31_LP_I2C_STANDARD_HZ && hz != S31_LP_I2C_FAST_HZ) ||
	    (!written && !read) || read > sizeof(data) ||
	    written > sizeof(data) - 1 - (read != 0))
		return S31_LP_RPC_INVALID;

	result = configure(pins, hz, flags);
	if (result != S31_LP_RPC_OK)
		return result;
	if (i2c_ll_is_bus_busy(bus))
		return S31_LP_RPC_BUSY;

	/*
	 * TRM ch.26 LP_I2C_CMD registers. Keep the address byte(s) and
	 * write data in one TX FIFO load. NACK the final read byte.
	 */
	i2c_ll_txfifo_rst(bus);
	i2c_ll_rxfifo_rst(bus);
	i2c_ll_clear_intr_mask(bus, I2C_STATUS);
	command(next++, I2C_LL_CMD_RESTART, 0, false);
	if (written) {
		data[bytes++] = addr << 1;
		for (uint32_t i = 0; i < written; i++)
			data[bytes++] = slot->data[i];
		command(next++, I2C_LL_CMD_WRITE, bytes, false);
		if (read)
			command(next++, I2C_LL_CMD_RESTART, 0, false);
	}
	if (read) {
		data[bytes++] = (addr << 1) | 1;
		command(next++, I2C_LL_CMD_WRITE, 1, false);
		if (read > 1)
			command(next++, I2C_LL_CMD_READ, read - 1, false);
		command(next++, I2C_LL_CMD_READ, 1, true);
	}
	command(next, I2C_LL_CMD_STOP, 0, false);
	i2c_ll_write_txfifo(bus, data, bytes);
	i2c_ll_update(bus);
	i2c_ll_start_trans(bus);
	result = wait_for_stop(next);
	if (result != S31_LP_RPC_OK)
		return result;

	i2c_ll_get_rxfifo_cnt(bus, &bytes);
	if (bytes != read)
		return S31_LP_RPC_IO;
	if (read) {
		i2c_ll_read_rxfifo(bus, data, read);
		for (uint32_t i = 0; i < read; i++)
			slot->data[i] = data[i];
	}
	return S31_LP_RPC_OK;
}
