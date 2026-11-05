/* SPDX-License-Identifier: LicenseRef-Pinscope-Device-1.0
 * Copyright (c) 2026 Pinscope. See LICENSE: for use with the Pinscope service only. */
/*
 * Entropy for the RP2040, which has no random generator Zephyr knows. The ring
 * oscillator gives one noisy bit per read: 8192 of them, with the cycle counter
 * and the previous pool, are hashed into each 32 bytes. Weaker than a TRNG,
 * far better than a seeded PRNG.
 */
#define DT_DRV_COMPAT pinscope_rp2040_rosc_entropy

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/kernel.h>
#include <hardware/structs/rosc.h>

#include "ps_crypto.h"

static uint8_t s_pool[32];
static uint32_t s_count;
static struct k_spinlock s_lock;

static void fill(uint8_t *p, size_t len)
{
    while (len) {
        ps_sha256_t c;
        uint8_t out[32];
        ps_sha256_init(&c);
        ps_sha256_update(&c, s_pool, sizeof(s_pool));
        for (int i = 0; i < 256; i++) {
            uint32_t w = 0;
            for (int b = 0; b < 32; b++) {
                w = (w << 1) | (rosc_hw->randombit & 1);
            }
            ps_sha256_update(&c, &w, sizeof(w));
        }
        uint32_t extra[2] = { k_cycle_get_32(), ++s_count };
        ps_sha256_update(&c, extra, sizeof(extra));
        ps_sha256_final(&c, s_pool);
        /* The output is a hash of the pool, so it never reveals the pool. */
        ps_sha256_init(&c);
        ps_sha256_update(&c, s_pool, sizeof(s_pool));
        ps_sha256_update(&c, "out", 3);
        ps_sha256_final(&c, out);
        size_t n = len < sizeof(out) ? len : sizeof(out);
        memcpy(p, out, n);
        p += n;
        len -= n;
    }
}

static int get_entropy(const struct device *dev, uint8_t *buf, uint16_t len)
{
    ARG_UNUSED(dev);
    k_spinlock_key_t key = k_spin_lock(&s_lock);
    fill(buf, len);
    k_spin_unlock(&s_lock, key);
    return 0;
}

static int get_entropy_isr(const struct device *dev, uint8_t *buf, uint16_t len, uint32_t flags)
{
    ARG_UNUSED(flags);
    get_entropy(dev, buf, len);
    return len;
}

/* The bootrom starts the ring oscillator; make sure nothing stopped it. */
static int init(const struct device *dev)
{
    ARG_UNUSED(dev);
    if ((rosc_hw->ctrl & ROSC_CTRL_ENABLE_BITS) != (ROSC_CTRL_ENABLE_VALUE_ENABLE << ROSC_CTRL_ENABLE_LSB)) {
        rosc_hw->ctrl = (rosc_hw->ctrl & ~ROSC_CTRL_ENABLE_BITS) | (ROSC_CTRL_ENABLE_VALUE_ENABLE << ROSC_CTRL_ENABLE_LSB);
    }
    while (!(rosc_hw->status & ROSC_STATUS_STABLE_BITS)) {
    }
    return 0;
}

static DEVICE_API(entropy, api) = {
    .get_entropy = get_entropy,
    .get_entropy_isr = get_entropy_isr,
};

DEVICE_DT_INST_DEFINE(0, init, NULL, NULL, NULL, PRE_KERNEL_1, CONFIG_ENTROPY_INIT_PRIORITY, &api);
