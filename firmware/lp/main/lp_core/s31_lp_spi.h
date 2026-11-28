/* SPDX-License-Identifier: MIT */
#ifndef S31_LP_SPI_H
#define S31_LP_SPI_H

#include "../../../../shared/s31_lp_protocol.h"

uint32_t s31_lp_spi_pins(void);
void s31_lp_spi_release(void);
uint32_t s31_lp_spi_transfer(volatile struct s31_lp_rpc_slot *slot, uint32_t owned);

#endif
