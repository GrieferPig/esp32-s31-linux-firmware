/* SPDX-License-Identifier: MIT */
#include <stdint.h>

#include "esp_err.h"
#include "ulp_lp_core_mailbox.h"
#include "ulp_lp_core_interrupts.h"
#include "ulp_lp_core_gpio.h"
#include "ulp_lp_core_lp_timer_shared.h"
#include "ulp_lp_core_utils.h"
#include "hal/clk_tree_ll.h"
#include "hal/lp_mailbox_ll.h"
#include "hal/rtc_timer_ll.h"

#include "../../../../shared/s31_lp_protocol.h"
#include "s31_lp_i2c.h"
#include "s31_lp_spi.h"
#include "s31_lp_uart.h"
#include "s31_lp_gdma.h"

#define S31_LP_SYS_STORE0 (*(volatile uint32_t *)0x2070002cU)
#define S31_LP_SYS_STORE3 (*(volatile uint32_t *)0x20700038U)
#define S31_LP_STAGE_MAIN 0x4c500001U
#define S31_LP_STAGE_MAILBOX_READY 0x4c500002U
#define S31_LP_STAGE_READY_SEND 0x4c500003U
#define S31_LP_STAGE_RUNNING 0x4c500004U
#define S31_RTC_SLOW_CAL_32K 16000000U
#define S31_LP_APPWR_CTRL (*(volatile uint32_t *)0x20804030U)
#define S31_LP_APPWR_SW_WAKEUP_REQ (1U << 1)
static lp_mailbox_t mailbox;
static uint32_t gpio_armed_mask;
static uint64_t sleep_timer_target;
static volatile bool sleep_wake_pending;
static volatile struct s31_lp_sleep_control * const sleep_control =
	(volatile struct s31_lp_sleep_control *)S31_LP_SLEEP_CONTROL_ADDR;

static volatile struct s31_lp_rpc_ring * const peripheral_ring =
	(volatile struct s31_lp_rpc_ring *)S31_LP_RPC_ADDR;

_Static_assert(sizeof(struct s31_lp_rpc_ring) <= S31_LP_RPC_BYTES, "LP RPC ring size");
_Static_assert(S31_LP_SLEEP_CONTROL_ADDR + sizeof(struct s31_lp_sleep_control) <=
	       S31_LP_RPC_ADDR, "LP RPC overlaps sleep control");

static void peripheral_rpc_init(void)
{
	peripheral_ring->producer = 0;
	peripheral_ring->consumer = 0;
	peripheral_ring->version = S31_LP_RPC_VERSION;
	peripheral_ring->magic = S31_LP_RPC_MAGIC;
}

static uint32_t peripheral_rpc(uint32_t sequence)
{
	volatile struct s31_lp_rpc_slot *slot;
	uint32_t consumer, producer;
	uint32_t result = S31_LP_RPC_UNSUPPORTED;
	uint32_t output_len = 0;

	/*
	 * TRM ch.11 LP Mailbox: lp_core_mailbox_receive() reads the message
	 * and acknowledges its slot. Order the following SRAM reads after it.
	 */
	__asm__ volatile ("fence iorw, iorw" ::: "memory");
	consumer = peripheral_ring->consumer;
	producer = peripheral_ring->producer;
	slot = &peripheral_ring->slots[consumer % S31_LP_RPC_SLOTS];
	if (producer - consumer != 1 || slot->generation != producer ||
	    slot->sequence != sequence)
		return S31_LP_RSP_ERROR | sequence;

	if (slot->input_len > sizeof(slot->data) || slot->output_len > sizeof(slot->data)) {
		result = S31_LP_RPC_INVALID;
	} else if (slot->operation == S31_LP_RPC_CAPABILITIES) {
		result = slot->input_len || slot->output_len ? S31_LP_RPC_INVALID : S31_LP_RPC_OK;
		slot->args[0] = S31_LP_SERVICE_I2C | S31_LP_SERVICE_SPI |
				S31_LP_SERVICE_UART | S31_LP_SERVICE_GDMA;
	} else if (slot->operation == S31_LP_RPC_I2C_XFER ||
		   slot->operation == S31_LP_RPC_SPI ||
		   slot->operation == S31_LP_RPC_UART ||
		   slot->operation == S31_LP_RPC_GDMA) {
		uint32_t state = sleep_control->state;
		bool release_spi = slot->operation == S31_LP_RPC_SPI &&
				   slot->args[3] == S31_LP_SPI_RELEASE;
		bool stop_uart = slot->operation == S31_LP_RPC_UART &&
				 slot->args[0] == S31_LP_UART_STOP;
		bool gdma_info = slot->operation == S31_LP_RPC_GDMA &&
				 slot->args[0] == S31_LP_GDMA_INFO;

		if (!release_spi && !stop_uart && !gdma_info &&
		    (state == S31_LP_SLEEP_PREPARED || state == S31_LP_SLEEP_ARMED ||
		     state == S31_LP_SLEEP_HP_ASLEEP || state == S31_LP_SLEEP_WAKING)) {
			result = S31_LP_RPC_BUSY;
		} else if (slot->operation == S31_LP_RPC_I2C_XFER) {
			result = s31_lp_i2c_transfer(slot, s31_lp_spi_pins() | s31_lp_uart_pins());
		} else if (slot->operation == S31_LP_RPC_SPI) {
			result = s31_lp_spi_transfer(slot, s31_lp_i2c_pins() | s31_lp_uart_pins());
		} else if (slot->operation == S31_LP_RPC_UART) {
			result = s31_lp_uart_rpc(slot, s31_lp_i2c_pins() | s31_lp_spi_pins());
		} else {
			result = s31_lp_gdma_rpc(slot);
		}
		if (result == S31_LP_RPC_OK)
			output_len = slot->output_len;
	}
	slot->output_len = output_len;
	slot->result = result;
	__asm__ volatile ("fence iorw, iorw" ::: "memory");
	peripheral_ring->consumer = producer;
	__asm__ volatile ("fence iorw, iorw" ::: "memory");
	return S31_LP_RSP_PERIPHERAL_RPC | sequence;
}

static void write_u64(volatile uint32_t *lo, volatile uint32_t *hi,
		      uint64_t value)
{
	*lo = (uint32_t)value;
	*hi = (uint32_t)(value >> 32);
}

static uint64_t read_u64(volatile uint32_t *lo, volatile uint32_t *hi)
{
	return (uint64_t)*lo | ((uint64_t)*hi << 32);
}

static uint32_t crc32_bytes(const volatile void *buffer, uint32_t length)
{
	const volatile uint8_t *bytes = buffer;
	uint32_t crc = ~0U;

	while (length--) {
		crc ^= *bytes++;
		for (uint32_t bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xedb88320U &
					      (0U - (crc & 1U)));
	}

	return ~crc;
}

static void sleep_control_update_crc(void)
{
	sleep_control->response_crc = crc32_bytes(sleep_control,
		__builtin_offsetof(struct s31_lp_sleep_control, response_crc));
}

static void sleep_control_set_result(uint32_t state, uint32_t result)
{
	sleep_control->capabilities = S31_LP_CAP_HANDSHAKE |
				      S31_LP_CAP_TIMER_WAKE |
				      S31_LP_CAP_GPIO_WAKE |
				      S31_LP_CAP_WAKE_LOG |
				      S31_LP_CAP_RETENTION_DESC |
				      S31_LP_CAP_PERIPHERAL_RPC;
	sleep_control->state = state;
	sleep_control->result = result;
	sleep_control_update_crc();
}

static void sleep_gpio_disarm(void)
{
	for (uint32_t pin = 0; pin < 8; pin++) {
		if (!(gpio_armed_mask & (1U << pin)))
			continue;
		ulp_lp_core_gpio_pullup_disable((lp_io_num_t)pin);
		ulp_lp_core_gpio_pulldown_disable((lp_io_num_t)pin);
		ulp_lp_core_gpio_wakeup_disable((lp_io_num_t)pin);
		rtcio_ll_clear_edge_wakeup_latch(pin);
		ulp_lp_core_gpio_input_disable((lp_io_num_t)pin);
		rtcio_ll_function_select(pin, RTCIO_LL_FUNC_DIGITAL);
	}
	gpio_armed_mask = 0;
}

static void sleep_wake(uint32_t reason, uint32_t raw, uint64_t now)
{
	bool deep_reboot = sleep_control->flags & S31_LP_SLEEP_F_DEEP_REBOOT;

	ulp_lp_core_lp_timer_intr_enable(false);
	ulp_lp_core_lp_timer_disable();
	sleep_timer_target = 0;
	sleep_gpio_disarm();
	sleep_control->wake_reason |= reason;
	sleep_control->wake_raw = raw;
	write_u64(&sleep_control->wake_ticks_lo,
		  &sleep_control->wake_ticks_hi, now);
	sleep_control->state = S31_LP_SLEEP_WAKING;
	sleep_control_update_crc();
	if (deep_reboot)
		S31_LP_SYS_STORE3 = reason;
	/* RTC target 0 drives the PMU back to HP_ACTIVE.  Explicitly request the
	 * AP power-state transition as well so a sleep profile using
	 * dig_cpu_stall cannot leave both HP harts stalled after the LP timer has
	 * published the wake record. */
	S31_LP_APPWR_CTRL = S31_LP_APPWR_SW_WAKEUP_REQ;
	ulp_lp_core_wakeup_main_processor();
	if (sleep_control->flags & S31_LP_SLEEP_F_S2IDLE)
		sleep_wake_pending = true;
}

static void sleep_timer_fired(uint64_t now)
{
	sleep_wake(S31_LP_WAKE_TIMER, S31_LP_WAKE_TIMER, now);
}

static void sleep_timer_arm_from_now(void)
{
	uint64_t now = ulp_lp_core_lp_timer_get_cycle_count();
	uint64_t duration_us;
	uint64_t duration_ticks;

	write_u64(&sleep_control->sleep_ticks_lo,
		  &sleep_control->sleep_ticks_hi, now);
	duration_us = read_u64(&sleep_control->deadline_lo,
			       &sleep_control->deadline_hi);
	duration_ticks = ulp_lp_core_lp_timer_calculate_sleep_ticks(duration_us);
	sleep_timer_target = now + duration_ticks;
	rtc_timer_ll_set_wakeup_time(1, sleep_timer_target);
	ulp_lp_core_lp_timer_intr_enable(true);
	sleep_control_update_crc();
}

void LP_CORE_ISR_ATTR ulp_lp_core_lp_timer_intr_handler(void)
{
	uint64_t now;

	ulp_lp_core_lp_timer_intr_clear();
	now = ulp_lp_core_lp_timer_get_cycle_count();
	/* HP suspend sequencing can produce an early target-1 interrupt.  Do not
	 * turn that edge into a false timer wake before the programmed main-counter
	 * deadline has actually elapsed. */
	if ((sleep_control->state != S31_LP_SLEEP_ARMED &&
	     sleep_control->state != S31_LP_SLEEP_HP_ASLEEP) ||
	    now < sleep_timer_target)
		return;
	ulp_lp_core_lp_timer_disable();
	sleep_timer_fired(now);
}

static uint32_t sleep_gpio_triggered(void)
{
	uint32_t levels = 0;

	if (!gpio_armed_mask)
		return 0;
	for (uint32_t pin = 0; pin < 8; pin++)
		if ((gpio_armed_mask & (1U << pin)) &&
		    ulp_lp_core_gpio_get_level((lp_io_num_t)pin))
			levels |= 1U << pin;
	return ~(levels ^ sleep_control->gpio_level_lo) & gpio_armed_mask;
}

static void gpio_wake_poll(void)
{
	uint32_t triggered;

	if (!(sleep_control->flags & S31_LP_SLEEP_F_DRY_RUN) &&
	    sleep_control->state != S31_LP_SLEEP_HP_ASLEEP)
		return;
	triggered = sleep_gpio_triggered();
	if (!triggered)
		return;
	sleep_wake(S31_LP_WAKE_GPIO, triggered,
		   ulp_lp_core_get_cpu_cycles());
}

void LP_CORE_ISR_ATTR ulp_lp_core_lp_io_intr_handler(void)
{
	uint32_t triggered;

	ulp_lp_core_gpio_clear_intr_status();
	if (sleep_control->state != S31_LP_SLEEP_HP_ASLEEP)
		return;
	triggered = sleep_gpio_triggered();
	if (!triggered)
		return;
	sleep_wake(S31_LP_WAKE_GPIO, triggered,
		   ulp_lp_core_get_cpu_cycles());
}

static bool sleep_control_request_valid(uint32_t sequence)
{
	uint32_t expected;

	if (sleep_control->magic != S31_LP_SLEEP_CONTROL_MAGIC ||
	    sleep_control->version != S31_LP_ABI_VERSION ||
	    sleep_control->size != sizeof(*sleep_control)) {
		sleep_control_set_result(S31_LP_SLEEP_REJECTED,
					 S31_LP_SLEEP_ERR_ABI);
		return false;
	}
	if ((sleep_control->sequence & S31_LP_SEQUENCE_MASK) != sequence) {
		sleep_control_set_result(S31_LP_SLEEP_REJECTED,
					 S31_LP_SLEEP_ERR_SEQUENCE);
		return false;
	}
	expected = crc32_bytes(sleep_control,
		__builtin_offsetof(struct s31_lp_sleep_control, request_crc));
	if (expected != sleep_control->request_crc) {
		sleep_control_set_result(S31_LP_SLEEP_REJECTED,
					 S31_LP_SLEEP_ERR_CRC);
		return false;
	}

	return true;
}

static uint32_t sleep_command(uint32_t command, uint32_t sequence)
{
	switch (command) {
	case S31_LP_CMD_SLEEP_PREPARE:
		if (!sleep_control_request_valid(sequence))
			return S31_LP_RSP_ERROR | sequence;
		if (s31_lp_uart_pins()) {
			sleep_control_set_result(S31_LP_SLEEP_REJECTED,
						 S31_LP_SLEEP_ERR_STATE);
			return S31_LP_RSP_ERROR | sequence;
		}
		if (sleep_control->wake_mask &
		    ~(S31_LP_WAKE_TIMER | S31_LP_WAKE_GPIO |
		      S31_LP_WAKE_LP_UART)) {
			sleep_control_set_result(S31_LP_SLEEP_REJECTED,
						 S31_LP_SLEEP_ERR_WAKE_MASK);
			return S31_LP_RSP_ERROR | sequence;
		}
		if ((sleep_control->flags & S31_LP_SLEEP_F_GPIO_PULL_UP) &&
		    (sleep_control->flags & S31_LP_SLEEP_F_GPIO_PULL_DOWN)) {
			sleep_control_set_result(S31_LP_SLEEP_REJECTED,
					 S31_LP_SLEEP_ERR_WAKE_MASK);
			return S31_LP_RSP_ERROR | sequence;
		}
		if (sleep_control->wake_mask & S31_LP_WAKE_GPIO) {
			if (!sleep_control->gpio_mask_lo ||
			    (sleep_control->gpio_mask_lo & ~0xffU) ||
			    (sleep_control->gpio_mask_lo & (s31_lp_i2c_pins() |
							   s31_lp_spi_pins())) ||
			    sleep_control->gpio_mask_hi ||
			    sleep_control->gpio_level_hi ||
			    (sleep_control->gpio_level_lo &
			     ~sleep_control->gpio_mask_lo)) {
				sleep_control_set_result(S31_LP_SLEEP_REJECTED,
						 S31_LP_SLEEP_ERR_WAKE_MASK);
				return S31_LP_RSP_ERROR | sequence;
			}
		}
		if ((sleep_control->wake_mask & S31_LP_WAKE_TIMER) &&
		    read_u64(&sleep_control->deadline_lo,
			     &sleep_control->deadline_hi) < 1000U) {
			sleep_control_set_result(S31_LP_SLEEP_REJECTED,
						 S31_LP_SLEEP_ERR_DEADLINE);
			return S31_LP_RSP_ERROR | sequence;
		}
		/*
		 * Powered suspend always retains a bounded timer as a recovery source.
		 * An LP GPIO level may be armed in parallel; the always-on LP core polls
		 * RTCIO and requests the same APPWR wake transition as the timer ISR.
		 * OpenSBI independently validates the descriptor before touching PMU.
		 */
		if (!(sleep_control->flags & S31_LP_SLEEP_F_DRY_RUN) &&
		    ((sleep_control->flags &
		      ~(S31_LP_SLEEP_F_MEM | S31_LP_SLEEP_F_DEEP_REBOOT |
			S31_LP_SLEEP_F_GPIO_PULL_UP |
			S31_LP_SLEEP_F_GPIO_PULL_DOWN)) ||
		     (!(sleep_control->flags & S31_LP_SLEEP_F_MEM) ==
		      !(sleep_control->flags & S31_LP_SLEEP_F_DEEP_REBOOT)) ||
		     ((sleep_control->flags &
		       (S31_LP_SLEEP_F_GPIO_PULL_UP |
			S31_LP_SLEEP_F_GPIO_PULL_DOWN)) &&
		      !(sleep_control->wake_mask & S31_LP_WAKE_GPIO)) ||
		     !(sleep_control->wake_mask & S31_LP_WAKE_TIMER) ||
		     (sleep_control->wake_mask &
		      ~(S31_LP_WAKE_TIMER | S31_LP_WAKE_GPIO)) ||
		     ((sleep_control->flags & S31_LP_SLEEP_F_MEM) &&
		      (sleep_control->retention_mask != 0xffffffffU ||
		       sleep_control->domain_mask != 0xffffffffU ||
		       sleep_control->clock_mask != 0xffffffffU)) ||
		     ((sleep_control->flags & S31_LP_SLEEP_F_DEEP_REBOOT) &&
		      (sleep_control->retention_mask ||
		       sleep_control->domain_mask ||
		       sleep_control->clock_mask)))) {
			sleep_control_set_result(S31_LP_SLEEP_REJECTED,
						 S31_LP_SLEEP_ERR_UNSUPPORTED);
			return S31_LP_RSP_ERROR | sequence;
		}
		s31_lp_spi_release();
		sleep_control_set_result(S31_LP_SLEEP_PREPARED,
					 S31_LP_SLEEP_OK);
		return S31_LP_RSP_SLEEP_PREPARED | sequence;

	case S31_LP_CMD_SLEEP_ARM: {
		uint64_t now;
		uint64_t duration_ticks;
		uint64_t duration_us;

		if (!sleep_control_request_valid(sequence))
			return S31_LP_RSP_ERROR | sequence;
		if (sleep_control->state != S31_LP_SLEEP_PREPARED) {
			sleep_control_set_result(S31_LP_SLEEP_REJECTED,
						 S31_LP_SLEEP_ERR_STATE);
			return S31_LP_RSP_ERROR | sequence;
		}
		/*
		 * Re-arm from a known quiescent state.  Target 1 and its interrupt
		 * status live in the always-on RTC block, so both can survive an HP
		 * suspend attempt (and some reset paths).  Enabling the interrupt on
		 * top of a stale status bit turns a newly armed request into WAKING
		 * before OpenSBI can enter the PMU.
		 */
		ulp_lp_core_lp_timer_intr_enable(false);
		ulp_lp_core_lp_timer_disable();
		sleep_control->wake_reason = 0;
		sleep_control->wake_raw = 0;
		sleep_wake_pending = false;
		now = ulp_lp_core_lp_timer_get_cycle_count();
		write_u64(&sleep_control->sleep_ticks_lo,
			  &sleep_control->sleep_ticks_hi, now);
		sleep_control_set_result(S31_LP_SLEEP_ARMED, S31_LP_SLEEP_OK);
		duration_us = read_u64(&sleep_control->deadline_lo,
				       &sleep_control->deadline_hi);
		/* RTC target 0 is the primary deep-sleep wake source.  Keep target 1
		 * five seconds behind it as an LP-owned fail-safe: a successful cold
		 * boot resets the chip before this interrupt, while a stalled PMU
		 * transition gets an APPWR wake request instead of wedging forever. */
		if (sleep_control->wake_mask & S31_LP_WAKE_TIMER) {
			if (sleep_control->flags & S31_LP_SLEEP_F_DEEP_REBOOT) {
				duration_us += 5000000ULL;
				duration_ticks =
					ulp_lp_core_lp_timer_calculate_sleep_ticks(duration_us);
				sleep_timer_target = now + duration_ticks;
				rtc_timer_ll_set_wakeup_time(1, sleep_timer_target);
				ulp_lp_core_lp_timer_intr_enable(true);
				sleep_control_update_crc();
			} else if (sleep_control->flags & S31_LP_SLEEP_F_MEM) {
				/* Device suspend can take longer than the requested sleep.
				 * Start the MEM timer only after OpenSBI publishes HP_ASLEEP,
				 * so it cannot expire while Linux is still quiescing devices. */
				sleep_timer_target = 0;
			} else {
				/* S2idle and awake diagnostics do not enter the OpenSBI
				 * MEM path and therefore never publish HP_ASLEEP.
				 */
				sleep_timer_arm_from_now();
			}
		}
		if (sleep_control->wake_mask & S31_LP_WAKE_GPIO) {
			gpio_armed_mask = sleep_control->gpio_mask_lo;
			for (uint32_t pin = 0; pin < 8; pin++) {
				if (!(gpio_armed_mask & (1U << pin)))
					continue;
				ulp_lp_core_gpio_init((lp_io_num_t)pin);
				ulp_lp_core_gpio_output_disable((lp_io_num_t)pin);
				ulp_lp_core_gpio_input_enable((lp_io_num_t)pin);
				if (sleep_control->flags &
				    S31_LP_SLEEP_F_GPIO_PULL_UP)
					ulp_lp_core_gpio_pullup_enable((lp_io_num_t)pin);
				if (sleep_control->flags &
				    S31_LP_SLEEP_F_GPIO_PULL_DOWN)
					ulp_lp_core_gpio_pulldown_enable((lp_io_num_t)pin);
				/*
				 * APPWR sleep may clock-gate the resident LP loop.  Configure
				 * RTCIO as an LP wake source after the input is known inactive;
				 * its ISR records the GPIO reason and requests the APPWR wake.
				 * The main loop still polls to cover the transition window.
				 */
			}
			/* RTCIO input passes through an LP-domain synchronizer.  Let the
			 * selected pad level settle before rejecting an already-active
			 * request; otherwise a stale inactive sample can incorrectly arm a
			 * level which will never produce the requested edge. */
			ulp_lp_core_delay_us(100);
			/* A level already active at ARM is not a wake transition. */
			if (sleep_gpio_triggered()) {
				sleep_gpio_disarm();
				ulp_lp_core_lp_timer_intr_enable(false);
				ulp_lp_core_lp_timer_disable();
				sleep_timer_target = 0;
				sleep_control_set_result(S31_LP_SLEEP_REJECTED,
						 S31_LP_SLEEP_ERR_WAKE_MASK);
				return S31_LP_RSP_ERROR | sequence;
			}
			ulp_lp_core_gpio_clear_intr_status();
			for (uint32_t pin = 0; pin < 8; pin++) {
				gpio_int_type_t edge;

				if (!(gpio_armed_mask & (1U << pin)))
					continue;
				rtcio_ll_clear_edge_wakeup_latch(pin);
				edge = (sleep_control->gpio_level_lo & (1U << pin)) ?
					GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL;
				ulp_lp_core_gpio_wakeup_enable((lp_io_num_t)pin, edge);
			}
		}
		return S31_LP_RSP_SLEEP_ARMED | sequence;
	}

	case S31_LP_CMD_SLEEP_ABORT:
		sleep_gpio_disarm();
		ulp_lp_core_lp_timer_intr_enable(false);
		ulp_lp_core_lp_timer_disable();
		sleep_timer_target = 0;
		sleep_control_set_result(S31_LP_SLEEP_ABORTED, S31_LP_SLEEP_OK);
		return S31_LP_RSP_SLEEP_ABORTED | sequence;

	case S31_LP_CMD_SLEEP_QUERY:
		sleep_control_update_crc();
		return S31_LP_RSP_SLEEP_STATUS | sequence;

	case S31_LP_CMD_SLEEP_RECLAIM:
		sleep_gpio_disarm();
		ulp_lp_core_lp_timer_intr_enable(false);
		ulp_lp_core_lp_timer_disable();
		sleep_timer_target = 0;
		sleep_control_set_result(S31_LP_SLEEP_READY, S31_LP_SLEEP_OK);
		return S31_LP_RSP_SLEEP_RECLAIMED | sequence;

	default:
		return S31_LP_RSP_ERROR | sequence;
	}
}

static void sleep_control_init(void)
{
	volatile uint32_t *word = (volatile uint32_t *)sleep_control;

	for (uint32_t i = 0; i < sizeof(*sleep_control) / sizeof(*word); i++)
		word[i] = 0;
	sleep_control->magic = S31_LP_SLEEP_CONTROL_MAGIC;
	sleep_control->version = S31_LP_ABI_VERSION;
	sleep_control->size = sizeof(*sleep_control);
	sleep_control_set_result(S31_LP_SLEEP_READY, S31_LP_SLEEP_OK);
}

int main(void)
{
	lp_message_t message;

	S31_LP_SYS_STORE0 = S31_LP_STAGE_MAIN;
	/*
	 * U-Boot/Linux do not otherwise claim the always-on RTC timer.  Enable
	 * its APB register clock and release the main counter from reset/stall
	 * before programming LP target 1.  The counter itself runs from RTC_SLOW
	 * and therefore remains available while HP clocks are stopped.
	 */
	RTC_TIMER.date.clk_en = 1;
	RTC_TIMER.update.main_timer_sys_rst = 1;
	RTC_TIMER.update.main_timer_sys_rst = 0;
	RTC_TIMER.update.main_timer_sys_stall = 0;
	/* SPL currently leaves the IDF RTC slow-clock calibration store at zero.
	 * Avoid an all-ones duration from the generic Q13.19 conversion.  Preserve
	 * any measured value once boot firmware starts providing one. */
	if (!clk_ll_rtc_slow_load_cal())
		clk_ll_rtc_slow_store_cal(S31_RTC_SLOW_CAL_32K);
	ulp_lp_core_intr_enable();
	ulp_lp_core_lp_timer_intr_enable(false);
	ulp_lp_core_lp_timer_disable();
	peripheral_rpc_init();
	sleep_control_init();
	if (lp_core_mailbox_init(&mailbox, NULL) != ESP_OK)
		for (;;)
			;
	/* S31 TRM ch.11 LP Mailbox: MB_REG_CLK_EN gates register access.
	 * ESP-IDF lp_core_mailbox_impl_init() resets it after enabling it.
	 */
	lp_mailbox_ll_enable_clock(&LP_MAILBOX);
	__asm__ volatile ("fence iorw, iorw" ::: "memory");
	S31_LP_SYS_STORE0 = S31_LP_STAGE_MAILBOX_READY;
	S31_LP_SYS_STORE0 = S31_LP_STAGE_READY_SEND;
	(void)lp_core_mailbox_send(mailbox, S31_LP_MSG_READY, -1);
	S31_LP_SYS_STORE0 = S31_LP_STAGE_RUNNING;

	for (;;) {
		uint32_t command;
		uint32_t sequence;
		uint32_t response;

		/* Publish the wake IRQ outside the timer ISR.  The HP interrupt
		 * controller retains it until Linux resumes the mailbox handler,
		 * which then acknowledges this transaction.
		 */
		if (sleep_wake_pending) {
			sleep_wake_pending = false;
			(void)lp_core_mailbox_send(mailbox, S31_LP_MSG_WAKE, -1);
		}

		if (sleep_control->state == S31_LP_SLEEP_HP_ASLEEP &&
		    (sleep_control->flags & S31_LP_SLEEP_F_MEM) &&
		    (sleep_control->wake_mask & S31_LP_WAKE_TIMER) &&
		    !sleep_timer_target)
			sleep_timer_arm_from_now();

		/* Once a timer-only deep reboot is armed, stop issuing LP/HP mailbox
		 * transactions and let RTC target 0 drive the PMU state machine.  A
		 * busy resident LP loop keeps the cross-domain fabric active and can
		 * degrade HP deep sleep into a partial APPWR transition.  The next
		 * Linux boot reloads this LP image after the PMU cold boot. */
		if ((sleep_control->state == S31_LP_SLEEP_ARMED ||
		     sleep_control->state == S31_LP_SLEEP_HP_ASLEEP) &&
		    (sleep_control->flags & S31_LP_SLEEP_F_DEEP_REBOOT) &&
		    sleep_control->wake_mask == S31_LP_WAKE_TIMER) {
			__asm__ volatile ("wfi");
			continue;
		}

		if (s31_lp_uart_poll())
			(void)lp_core_mailbox_send(mailbox, S31_LP_MSG_UART_EVENT, -1);

		if (lp_core_mailbox_receive(mailbox, &message, 0) != ESP_OK) {
			gpio_wake_poll();
			continue;
		}

		command = (uint32_t)message & S31_LP_MESSAGE_MASK;
		sequence = (uint32_t)message & S31_LP_SEQUENCE_MASK;

		switch (command) {
		case S31_LP_CMD_PING:
			response = S31_LP_RSP_PONG | sequence;
			break;
		case S31_LP_CMD_STATUS:
			response = S31_LP_RSP_STATUS | S31_LP_ABI_VERSION;
			break;
		case S31_LP_CMD_PERIPHERAL_RPC:
			response = peripheral_rpc(sequence);
			break;
		case S31_LP_CMD_SLEEP_PREPARE:
		case S31_LP_CMD_SLEEP_ARM:
		case S31_LP_CMD_SLEEP_ABORT:
		case S31_LP_CMD_SLEEP_QUERY:
		case S31_LP_CMD_SLEEP_RECLAIM:
			response = sleep_command(command, sequence);
			break;
		default:
			response = S31_LP_RSP_ERROR | sequence;
			break;
		}

		(void)lp_core_mailbox_send(mailbox, response, -1);
	}
}
