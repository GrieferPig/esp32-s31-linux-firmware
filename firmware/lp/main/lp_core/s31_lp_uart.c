/* SPDX-License-Identifier: MIT */
#include "s31_lp_uart.h"

#include "hal/clk_tree_ll.h"
#include "hal/rtc_io_ll.h"
#include "hal/uart_ll.h"
#include "soc/lp_gpio_sig_map.h"
#include "soc/lp_gpio_struct.h"
#include "ulp_lp_core_lp_timer_shared.h"

#define UART_UPDATE_TIMEOUT_US 20000U
#define UART_RX_BYTES 256U
#define UART_RAW_ERRORS (LP_UART_RXFIFO_OVF_INT_RAW | LP_UART_FRM_ERR_INT_RAW | \
			 LP_UART_PARITY_ERR_INT_RAW | LP_UART_BRK_DET_INT_RAW)

static uint32_t pins;
static uint32_t pin_mask;
static uint32_t rx_head;
static uint32_t rx_tail;
static uint32_t rx_errors;
static bool tx_pending;
static bool event_pending;
static uint8_t rx_data[UART_RX_BYTES];

_Static_assert(S31_LP_UART_TX_BYTES <= SOC_LP_UART_FIFO_LEN, "LP UART TX burst");

uint32_t s31_lp_uart_pins(void)
{
	return pin_mask;
}

static uint32_t rx_count(void)
{
	return rx_head - rx_tail;
}

static void receive_chars(void)
{
	uint32_t errors = uart_ll_get_intraw_mask(&LP_UART) & UART_RAW_ERRORS;
	uint32_t count = uart_ll_get_rxfifo_len(&LP_UART);

	if (errors)
		uart_ll_clr_intsts_mask(&LP_UART, errors);
	if (errors & LP_UART_RXFIFO_OVF_INT_RAW)
		rx_errors |= S31_LP_UART_OVERRUN;
	if (errors & LP_UART_FRM_ERR_INT_RAW)
		rx_errors |= S31_LP_UART_BAD_FRAME;
	if (errors & LP_UART_PARITY_ERR_INT_RAW)
		rx_errors |= S31_LP_UART_BAD_PARITY;
	if (errors & LP_UART_BRK_DET_INT_RAW)
		rx_errors |= S31_LP_UART_BREAK;

	while (count--) {
		/* TRM ch.25.8.2 LP_UART_FIFO_REG: a full-word read pops RX. */
		uint8_t byte = LP_UART.fifo.val;

		if (rx_count() == sizeof(rx_data)) {
			rx_errors |= S31_LP_UART_OVERRUN;
			continue;
		}
		rx_data[rx_head++ % sizeof(rx_data)] = byte;
	}
}

bool s31_lp_uart_poll(void)
{
	if (!pin_mask)
		return false;

	receive_chars();
	if (event_pending || (!rx_count() && !rx_errors &&
			      !(tx_pending && uart_ll_is_tx_idle(&LP_UART))))
		return false;

	event_pending = true;
	return true;
}

static void disable_uart(void)
{
	if (pin_mask) {
		uint32_t tx = pins & 0xff;
		uint32_t rx = pins >> 8;

		rtcio_ll_output_disable(tx);
		rtcio_ll_function_select(tx, RTCIO_LL_FUNC_DIGITAL);
		rtcio_ll_input_disable(rx);
		rtcio_ll_function_select(rx, RTCIO_LL_FUNC_DIGITAL);
	}
	pin_mask = 0;
	rx_head = rx_tail = rx_errors = 0;
	tx_pending = false;
	event_pending = false;
	lp_uart_ll_sclk_disable(0);
	lp_uart_ll_enable_bus_clock(0, false);
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

static bool apply_config(void)
{
	uint32_t start = ulp_lp_core_lp_timer_get_cycle_count();
	uint32_t ticks = ulp_lp_core_lp_timer_calculate_sleep_ticks(UART_UPDATE_TIMEOUT_US);

	/* TRM ch.25.8.2 LP_UART_REG_UPDATE_REG, ESP-IDF uart_ll_update(). */
	LP_UART.reg_update.reg_update = 1;
	while (LP_UART.reg_update.reg_update) {
		if ((uint32_t)(ulp_lp_core_lp_timer_get_cycle_count() - start) >= ticks)
			return false;
	}
	return true;
}

static uint32_t configure_uart(volatile struct s31_lp_rpc_slot *slot, uint32_t claimed)
{
	uint32_t packed = slot->args[1];
	uint32_t baud = slot->args[2];
	uint32_t format = slot->args[3];
	uint32_t tx = packed & 0xff;
	uint32_t rx = packed >> 8;
	uint32_t mask, source, pre, div;
	bool first_config = !pin_mask;

	if (tx >= S31_LP_GPIO_COUNT || rx >= S31_LP_GPIO_COUNT || tx == rx ||
	    baud < S31_LP_UART_MIN_BAUD || baud > S31_LP_UART_MAX_BAUD ||
	    format & ~(S31_LP_UART_BITS_MASK | S31_LP_UART_STOP_2 |
		       S31_LP_UART_PARITY | S31_LP_UART_ODD))
		return S31_LP_RPC_INVALID;

	mask = (1U << tx) | (1U << rx);
	if (mask & claimed ||
	    (pin_mask && (pins != packed || !uart_ll_is_tx_idle(&LP_UART))))
		return S31_LP_RPC_BUSY;

	/* TRM ch.25.8.2 LP_UART_CLKDIV_SYNC_REG, IDF lp_uart_ll_set_baudrate(). */
	source = clk_ll_xtal_get_freq_mhz() * 1000000U;
	pre = (source + baud * UART_CLKDIV_V - 1) / (baud * UART_CLKDIV_V);
	if (!pre || pre > LP_PERICLKRST_LP_UART_CLK_DIV_NUM_V + 1)
		return S31_LP_RPC_INVALID;
	div = ((source << 4) + (baud * pre) / 2) / (baud * pre);
	if (div < 16 || div > (UART_CLKDIV_V << 4) + 15)
		return S31_LP_RPC_INVALID;

	if (first_config) {
		lp_uart_ll_enable_bus_clock(0, true);
		lp_uart_ll_reset_register(0);
		lp_uart_ll_set_source_clk(&LP_UART, LP_UART_SCLK_XTAL);
		lp_uart_ll_sclk_enable(0);
		lp_uart_ll_mem_force_power_on();
		LP_UART.int_ena.val = 0;
		LP_PERI_CLKRST.iomux_ctrl.lp_iomux_clk_en = 1;
		LP_GPIO.clock_gate.clk_en = 1;
		if (!LP_PERI_CLKRST.iomux_ctrl.lp_iomux_clk_en || !LP_GPIO.clock_gate.clk_en) {
			disable_uart();
			return S31_LP_RPC_IO;
		}
	}

	LP_PERI_CLKRST.uart_ctrl.lp_uart_clk_div_num = pre - 1;
	LP_UART.clkdiv_sync.clkdiv = div >> 4;
	LP_UART.clkdiv_sync.clkdiv_frag = div & 15;
	LP_UART.conf0_sync.bit_num = format & S31_LP_UART_BITS_MASK;
	LP_UART.conf0_sync.stop_bit_num = (format & S31_LP_UART_STOP_2) ?
					  UART_STOP_BITS_2 : UART_STOP_BITS_1;
	LP_UART.conf0_sync.parity_en = !!(format & S31_LP_UART_PARITY);
	LP_UART.conf0_sync.parity = !!(format & S31_LP_UART_ODD);
	if (!apply_config()) {
		disable_uart();
		return S31_LP_RPC_TIMEOUT;
	}

	if (first_config) {
		/* TRM ch.5.3 LP GPIO Matrix, IDF lp_uart_config_io(). */
		configure_pad(rx);
		rtcio_ll_input_enable(rx);
		rtcio_ll_set_input_signal_matrix_source(rx, LP_UART_RXD_PAD_IN_IDX, false);
		configure_pad(tx);
		rtcio_ll_set_output_signal_matrix_source(tx, LP_UART_TXD_PAD_OUT_IDX, false);
		rtcio_ll_set_output_enable_ctrl(tx, true, false);
	}
	pins = packed;
	pin_mask = mask;
	slot->args[2] = (source << 4) / (div * pre);
	return S31_LP_RPC_OK;
}

static uint32_t exchange(volatile struct s31_lp_rpc_slot *slot)
{
	bool idle;
	uint32_t accepted = 0;
	uint32_t count = 0;

	if (!pin_mask || slot->input_len > S31_LP_UART_TX_BYTES)
		return S31_LP_RPC_INVALID;

	idle = uart_ll_is_tx_idle(&LP_UART);
	if (idle && slot->input_len) {
		/* As in uart_ll_write_txfifo(), writes must be full words. */
		for (accepted = 0; accepted < slot->input_len; accepted++)
			LP_UART.fifo.val = slot->data[accepted];
		idle = false;
	}
	receive_chars();
	while (count < slot->output_len && rx_count())
		slot->data[count++] = rx_data[rx_tail++ % sizeof(rx_data)];

	slot->args[1] = accepted;
	slot->args[2] = idle;
	slot->args[3] = rx_errors;
	slot->output_len = count;
	rx_errors = 0;
	tx_pending = !idle;
	event_pending = false;
	return S31_LP_RPC_OK;
}

uint32_t s31_lp_uart_rpc(volatile struct s31_lp_rpc_slot *slot, uint32_t claimed)
{
	if (slot->args[0] == S31_LP_UART_EXCHANGE)
		return exchange(slot);
	if (slot->input_len || slot->output_len)
		return S31_LP_RPC_INVALID;
	switch (slot->args[0]) {
	case S31_LP_UART_CONFIG:
		return configure_uart(slot, claimed);
	case S31_LP_UART_STOP:
		if (pin_mask)
			disable_uart();
		return S31_LP_RPC_OK;
	default:
		return S31_LP_RPC_INVALID;
	}
}
