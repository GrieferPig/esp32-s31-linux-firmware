/* SPDX-License-Identifier: MIT */
#include "s31_lp_gdma.h"

#include "hal/ahb_dma_ll.h"
#include "hal/gdma_ll.h"
#include "ulp_lp_core_lp_timer_shared.h"

#define LP_GDMA_CH 0
#define LP_GDMA_DUMMY 2
#define LP_GDMA_TIMEOUT_US 20000U

/* TRM ch.3.4.1 GDMA Descriptor — size, length, suc_eof, owner. */
#define DESC_LENGTH_SHIFT 12
#define DESC_LENGTH_MASK 0xfffU
#define DESC_SUC_EOF (1U << 30)
#define DESC_DMA_OWNER (1U << 31)

#define RX_ERRORS (GDMA_LL_EVENT_RX_DESC_EMPTY | GDMA_LL_EVENT_RX_DESC_ERROR | \
		   GDMA_LL_EVENT_RX_FIFO_OVF | GDMA_LL_EVENT_RX_FIFO_UDF | \
		   AHB_DMA_IN_AHBINF_RESP_ERR_CH0_INT_RAW)
#define TX_ERRORS (GDMA_LL_EVENT_TX_DESC_ERROR | GDMA_LL_EVENT_TX_FIFO_OVF | \
		   GDMA_LL_EVENT_TX_FIFO_UDF | AHB_DMA_OUT_AHBINF_RESP_ERR_CH0_INT_RAW)

struct s31_lp_gdma_descriptor {
	uint32_t control;
	uint32_t buffer;
	uint32_t next;
};

static volatile struct s31_lp_gdma_descriptor tx_desc, rx_desc;
static uint8_t tx_data[S31_LP_RPC_DATA_BYTES] __attribute__((aligned(4)));
static volatile uint8_t rx_data[S31_LP_RPC_DATA_BYTES] __attribute__((aligned(4)));

_Static_assert(sizeof(tx_desc) == 12, "AHB GDMA descriptor size");
_Static_assert(_Alignof(struct s31_lp_gdma_descriptor) == 4, "AHB GDMA descriptor alignment");

static uint32_t wait_for_copy(ahb_dma_dev_t *dev)
{
	uint32_t start = ulp_lp_core_lp_timer_get_cycle_count();
	uint32_t ticks = ulp_lp_core_lp_timer_calculate_sleep_ticks(LP_GDMA_TIMEOUT_US);

	for (;;) {
		uint32_t rx = ahb_dma_ll_rx_get_interrupt_status(dev, LP_GDMA_CH, true);
		uint32_t tx = ahb_dma_ll_tx_get_interrupt_status(dev, LP_GDMA_CH, true);

		if ((rx & RX_ERRORS) || (tx & TX_ERRORS))
			return S31_LP_RPC_IO;
		if ((rx & GDMA_LL_EVENT_RX_SUC_EOF) &&
		    ahb_dma_ll_rx_is_desc_fsm_idle(dev, LP_GDMA_CH) &&
		    ahb_dma_ll_tx_is_desc_fsm_idle(dev, LP_GDMA_CH))
			return S31_LP_RPC_OK;
		if ((uint32_t)(ulp_lp_core_lp_timer_get_cycle_count() - start) >= ticks)
			return S31_LP_RPC_TIMEOUT;
	}
}

static uint32_t copy_data(volatile struct s31_lp_rpc_slot *slot, uint32_t length)
{
	const uint32_t group = GDMA_LL_LP_AHB_GROUP_START_ID;
	ahb_dma_dev_t *dev = AHB_DMA_LL_GET_HW(GDMA_LL_LP_AHB_GROUP_START_ID);
	uint32_t result, control;

	for (uint32_t i = 0; i < length; i++)
		tx_data[i] = slot->data[i];

	tx_desc.control = length | (length << DESC_LENGTH_SHIFT) | DESC_DMA_OWNER | DESC_SUC_EOF;
	tx_desc.buffer = (uintptr_t)tx_data;
	tx_desc.next = 0;
	rx_desc.control = length | DESC_DMA_OWNER;
	rx_desc.buffer = (uintptr_t)rx_data;
	rx_desc.next = 0;

	gdma_ll_enable_bus_clock(group, true);
	gdma_ll_reset_register(group);
	ahb_dma_ll_force_enable_reg_clock(dev, true);
	lp_ahb_dma_ll_set_default_memory_range(dev);

	/* TRM ch.3.7.3 — LP_AHB_DMA_{IN,OUT}_CONF0 and LP_AHB_DMA_{IN,OUT}_LINK. */
	ahb_dma_ll_tx_reset_channel(dev, LP_GDMA_CH);
	ahb_dma_ll_rx_reset_channel(dev, LP_GDMA_CH);
	ahb_dma_ll_tx_set_desc_addr(dev, LP_GDMA_CH, (uintptr_t)&tx_desc);
	ahb_dma_ll_rx_set_desc_addr(dev, LP_GDMA_CH, (uintptr_t)&rx_desc);
	ahb_dma_ll_tx_connect_to_mem(dev, LP_GDMA_CH, LP_GDMA_DUMMY);
	ahb_dma_ll_rx_connect_to_mem(dev, LP_GDMA_CH, LP_GDMA_DUMMY);
	__asm__ volatile ("fence iorw, iorw" ::: "memory");
	ahb_dma_ll_tx_start(dev, LP_GDMA_CH);
	ahb_dma_ll_rx_start(dev, LP_GDMA_CH);
	result = wait_for_copy(dev);

	/* Release the bus before reusing the descriptors, including after a timeout. */
	gdma_ll_reset_register(group);
	gdma_ll_enable_bus_clock(group, false);
	__asm__ volatile ("fence iorw, iorw" ::: "memory");
	if (result != S31_LP_RPC_OK)
		return result;

	control = rx_desc.control;
	if ((control & DESC_DMA_OWNER) ||
	    ((control >> DESC_LENGTH_SHIFT) & DESC_LENGTH_MASK) != length)
		return S31_LP_RPC_IO;
	for (uint32_t i = 0; i < length; i++)
		slot->data[i] = rx_data[i];

	return S31_LP_RPC_OK;
}

uint32_t s31_lp_gdma_rpc(volatile struct s31_lp_rpc_slot *slot)
{
	uint32_t length = slot->input_len;

	if (slot->args[1] || slot->args[2] || slot->args[3])
		return S31_LP_RPC_INVALID;
	switch (slot->args[0]) {
	case S31_LP_GDMA_INFO:
		if (length || slot->output_len)
			return S31_LP_RPC_INVALID;
		slot->args[1] = S31_LP_RPC_DATA_BYTES;
		return S31_LP_RPC_OK;
	case S31_LP_GDMA_COPY:
		if (!length || length > S31_LP_RPC_DATA_BYTES || slot->output_len != length)
			return S31_LP_RPC_INVALID;
		return copy_data(slot, length);
	default:
		return S31_LP_RPC_UNSUPPORTED;
	}
}
