/* This file is part of BadgeVMS
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "badgevms/device.h"

#include <stddef.h>
#include <stdint.h>

// When enabled the serial keyboard (tca8418.c) is the only reader of the
// UART0 RX FIFO and forwards all non-keyboard bytes to the tty.
#ifndef BADGEVMS_SERIAL_KBD
#define BADGEVMS_SERIAL_KBD 1
#endif

device_t *tty_create(bool is_stdout, bool is_stdin);

#if BADGEVMS_SERIAL_KBD
// Queue received bytes for stdin, returns the number of bytes accepted
size_t tty_rx_push(uint8_t const *buf, size_t len);

// Drain the UART0 RX FIFO (implemented in tca8418.c)
void serial_kbd_poll(void);
#endif
