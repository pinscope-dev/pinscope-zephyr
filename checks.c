/* SPDX-License-Identifier: LicenseRef-Pinscope-Device-1.0
 * Copyright (c) 2026 Pinscope. See LICENSE: for use with the Pinscope service only. */
/* What a build needs for Pinscope, checked at compile time with the fix in
 * the message, instead of a link error or a device that never connects. */
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#if defined(CONFIG_SETTINGS_NVS) && !DT_NODE_EXISTS(DT_NODELABEL(storage_partition))
#error "Pinscope keeps its device token in settings, and this board has no storage_partition. \
Add one in a board overlay (see ports/zephyr/README), or choose another settings backend."
#endif

#if !defined(CONFIG_ENTROPY_HAS_DRIVER) && defined(CONFIG_SOC_SERIES_RP2040)
#error "The RP2040 has no random generator Zephyr knows. Build with -S pinscope-rp2040, \
which adds the one in the Pinscope module."
#endif

/* CONFIG_TEST_CSPRNG_GENERATOR (emulators, CI) is let through: Zephyr itself
 * marks such a build as not secure and warns about it. */
#if !defined(CONFIG_CSPRNG_ENABLED) && !defined(CONFIG_TEST_CSPRNG_GENERATOR)
#error "Pinscope needs a cryptographic random generator (CONFIG_CSPRNG_ENABLED) for its keys, \
and this board has no entropy driver enabled."
#endif

#if defined(CONFIG_PINSCOPE_AUTOSTART)
BUILD_ASSERT(sizeof(CONFIG_PINSCOPE_PROJECT_KEY) > 1,
             "Pinscope starts at boot but has no project key. Copy it from the dashboard (Connect a device) into "
             "prj.conf as CONFIG_PINSCOPE_PROJECT_KEY=\"psk_...\", or build the sample with PINSCOPE_KEY=psk_... in "
             "the environment. Starting Pinscope from code instead? Set CONFIG_PINSCOPE_AUTOSTART=n.");
#endif

BUILD_ASSERT(CONFIG_NET_MAX_CONN >= 6,
             "Pinscope needs CONFIG_NET_MAX_CONN of at least 6: the tunnel, a local server, "
             "the stream to it, DHCP and DNS");
