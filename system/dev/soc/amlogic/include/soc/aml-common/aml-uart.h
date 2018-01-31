// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#define S905_UART_WFIFO         (0x0)
#define S905_UART_RFIFO         (0x4)
#define S905_UART_CONTROL       (0x8)
#define S905_UART_STATUS        (0xc)
#define S905_UART_IRQ_CONTROL   (0x10)
#define S905_UART_REG5          (0x14)


#define S905_UART_CONTROL_INVRTS    (1 << 31)
#define S905_UART_CONTROL_MASKERR   (1 << 30)
#define S905_UART_CONTROL_INVCTS    (1 << 29)
#define S905_UART_CONTROL_TXINTEN   (1 << 28)
#define S905_UART_CONTROL_RXINTEN   (1 << 27)
#define S905_UART_CONTROL_INVTX     (1 << 26)
#define S905_UART_CONTROL_INVRX     (1 << 25)
#define S905_UART_CONTROL_CLRERR    (1 << 24)
#define S905_UART_CONTROL_RSTRX     (1 << 23)
#define S905_UART_CONTROL_RSTTX     (1 << 22)
#define S905_UART_CONTROL_XMITLEN   (1 << 20)
#define S905_UART_CONTROL_XMITLEN_MASK   (0x3 << 20)
#define S905_UART_CONTROL_PAREN     (1 << 19)
#define S905_UART_CONTROL_PARTYPE   (1 << 18)
#define S905_UART_CONTROL_STOPLEN   (1 << 16)
#define S905_UART_CONTROL_STOPLEN_MASK   (0x3 << 16)
#define S905_UART_CONTROL_TWOWIRE   (1 << 15)
#define S905_UART_CONTROL_RXEN      (1 << 13)
#define S905_UART_CONTROL_TXEN      (1 << 12)
#define S905_UART_CONTROL_BAUD0     (1 << 0)
#define S905_UART_CONTROL_BAUD0_MASK     (0xfff << 0)

#define S905_UART_STATUS_RXBUSY         (1 << 26)
#define S905_UART_STATUS_TXBUSY         (1 << 25)
#define S905_UART_STATUS_RXOVRFLW       (1 << 24)
#define S905_UART_STATUS_CTSLEVEL       (1 << 23)
#define S905_UART_STATUS_TXEMPTY        (1 << 22)
#define S905_UART_STATUS_TXFULL         (1 << 21)
#define S905_UART_STATUS_RXEMPTY        (1 << 20)
#define S905_UART_STATUS_RXFULL         (1 << 19)
#define S905_UART_STATUS_TXOVRFLW       (1 << 18)
#define S905_UART_STATUS_FRAMEERR       (1 << 17)
#define S905_UART_STATUS_PARERR         (1 << 16)
#define S905_UART_STATUS_TXCOUNT_POS    (8)
#define S905_UART_STATUS_TXCOUNT_MASK   (0x7f << S905_UART_STATUS_TXCOUNT_POS)
#define S905_UART_STATUS_RXCOUNT_POS    (0)
#define S905_UART_STATUS_RXCOUNT_MASK   (0x7f << S905_UART_STATUS_RXCOUNT_POS)

#define UARTREG(base, reg)  (*(volatile uint32_t*)((base)  + (reg)))
