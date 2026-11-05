/* SPDX-License-Identifier: LicenseRef-Pinscope-Device-1.0
 * Copyright (c) 2026 Pinscope. See LICENSE: for use with the Pinscope service only. */
/*
 * CONFIG_PINSCOPE_AUTOSTART: Pinscope starts at boot with the settings from
 * Kconfig, so a project needs no code for it. pinscope_start() called later
 * by the application changes nothing while Pinscope runs.
 */
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#ifdef CONFIG_PINSCOPE_START_DHCP
#include <zephyr/net/dhcpv4.h>
#endif

#include "pinscope.h"

static int autostart(void)
{
#ifdef CONFIG_PINSCOPE_START_DHCP
    /* Zephyr does not start DHCP by itself; a static address is left alone. */
    struct net_if *iface = net_if_get_default();
    if (iface && !net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED)) {
        net_dhcpv4_start(iface);
    }
#endif
    pinscope_start(NULL);
    return 0;
}

SYS_INIT(autostart, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
