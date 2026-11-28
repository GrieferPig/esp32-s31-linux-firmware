/* SPDX-License-Identifier: MIT */
#ifndef S31_LP_I2C_H
#define S31_LP_I2C_H

#include "../../../../shared/s31_lp_protocol.h"

uint32_t s31_lp_i2c_pins(void);
uint32_t s31_lp_i2c_transfer(volatile struct s31_lp_rpc_slot *slot, uint32_t owned);

#endif
