/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "../../../../shared/s31_lp_protocol.h"

uint32_t s31_lp_uart_pins(void);
bool s31_lp_uart_poll(void);
uint32_t s31_lp_uart_rpc(volatile struct s31_lp_rpc_slot *slot, uint32_t claimed);
