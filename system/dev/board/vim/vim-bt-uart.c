// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/protocol/platform-defs.h>
#include <soc/aml-s912/s912-hw.h>

#include "vim.h"

static const pbus_mmio_t bt_uart_mmios[] = {
    {
        .base = 0xc9000000,
        .length = 0x100000,
    },
};

static const pbus_irq_t bt_uart_irqs[] = {
    {
        .irq = 62,
        .mode = ZX_INTERRUPT_MODE_EDGE_HIGH,
    },
};

static const pbus_dev_t bt_uart_dev = {
    .name = "bt-uart",
    .vid = PDEV_VID_AMLOGIC,
    .pid = PDEV_PID_GENERIC,
    .did = PDEV_DID_AMLOGIC_BT_UART,
    .mmios = bt_uart_mmios,
    .mmio_count = countof(bt_uart_mmios),
    .irqs = bt_uart_irqs,
    .irq_count = countof(bt_uart_irqs),
};

zx_status_t vim_bt_uart_init(vim_bus_t* bus) {
    return pbus_device_add(&bus->pbus, &bt_uart_dev, 0);
}
