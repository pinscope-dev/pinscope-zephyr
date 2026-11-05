/*
 * Pinscope on Zephyr: a small web page on port 80, reachable through the
 * device's Pinscope address, plus metrics and logs.
 */
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_ip.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef CONFIG_WIFI
#include <zephyr/net/wifi_mgmt.h>
#endif
#ifdef CONFIG_BT
#include <zephyr/bluetooth/bluetooth.h>
#endif

#if defined(CONFIG_BOARD_W55RP20_PINSCOPE) || defined(CONFIG_BOARD_W55RP20_EVB_PICO)
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/ethernet_mgmt.h>
#endif

#include "pinscope.h"

#if defined(CONFIG_BOARD_W55RP20_PINSCOPE) || defined(CONFIG_BOARD_W55RP20_EVB_PICO)
/* Every board ships with the same MAC address (00:00:00:01:02:03); make one
 * from the flash chip's ID, locally administered, so two boards can share a LAN. */
static void unique_mac(void)
{
    uint8_t id[8] = { 0 };
    struct ethernet_req_params params = { 0 };
    hwinfo_get_device_id(id, sizeof(id));
    params.mac_address.addr[0] = 0x02;
    memcpy(&params.mac_address.addr[1], &id[3], 5);
    struct net_if *iface = net_if_get_default();
    net_if_down(iface);
    net_mgmt(NET_REQUEST_ETHERNET_SET_MAC_ADDRESS, iface, &params, sizeof(params));
    net_if_up(iface);
}
#endif

/* The address and Pinscope's state, whenever they change: for bring-up. */
static void report(void *a, void *b, void *c)
{
    static const char *states[] = { "stopped", "connecting", "online", "rejected" };
    char last[96] = "";
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);
    for (;;) {
        char ip[NET_IPV4_ADDR_LEN] = "none", now[96];
        struct net_if *iface = net_if_get_default();
        struct in_addr *addr = iface ? net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED) : NULL;
        if (addr) {
            net_addr_ntop(AF_INET, addr, ip, sizeof(ip));
        }
        snprintf(now, sizeof(now), "link %s, ip %s, pinscope %s", iface && net_if_is_up(iface) ? "up" : "down", ip,
                 states[pinscope_state()]);
        if (strcmp(now, last)) {
            printk("status: %s %s\n", now, pinscope_url());
            strcpy(last, now);
        }
        k_sleep(K_SECONDS(1));
    }
}
K_THREAD_DEFINE(report_thread, 1024, report, NULL, NULL, NULL, 10, 0, 1000);

static void serve(void)
{
    int server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(80) };
    int one = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(server, 2) != 0) {
        printk("cannot listen on port 80\n");
        return;
    }
    for (;;) {
        int client = accept(server, NULL, NULL);
        if (client < 0) {
            continue;
        }
        char request[1024], page[256];
        /* Read the whole request: closing with unread bytes resets the
         * connection, and the reply would be lost. */
        size_t got = 0;
        while (got < sizeof(request) - 1) {
            int n = (int)recv(client, request + got, sizeof(request) - 1 - got, 0);
            if (n <= 0) {
                break;
            }
            got += (size_t)n;
            request[got] = '\0';
            if (strstr(request, "\r\n\r\n")) {
                break;
            }
        }
        /* For testing crash reports: a NULL write, like a real bug. */
        if (!strncmp(request, "GET /crash ", 11)) {
            printk("crashing on purpose\n");
            k_msleep(500); /* time for the line to reach the server */
            *(volatile uint32_t *)0 = 0xdead;
        }
        int body = snprintf(page, sizeof(page),
                            "<!doctype html><title>%s</title><h1>Hello from %s</h1><p>Up for %lld s.</p>",
                            CONFIG_BOARD, CONFIG_BOARD, k_uptime_get() / 1000);
        char head[128];
        int n = snprintf(head, sizeof(head),
                         "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", body);
        send(client, head, n, 0);
        send(client, page, body, 0);
        close(client);
    }
}

int main(void)
{
#ifdef CONFIG_BT
    bt_enable(NULL); /* the random generator for the keys is behind it */
#endif
#if defined(CONFIG_BOARD_W55RP20_PINSCOPE) || defined(CONFIG_BOARD_W55RP20_EVB_PICO)
    unique_mac();
#endif
#ifdef CONFIG_WIFI
    struct wifi_connect_req_params wifi = {
        .ssid = (const uint8_t *)CONFIG_SAMPLE_WIFI_SSID,
        .ssid_length = sizeof(CONFIG_SAMPLE_WIFI_SSID) - 1,
        .psk = (const uint8_t *)CONFIG_SAMPLE_WIFI_PSK,
        .psk_length = sizeof(CONFIG_SAMPLE_WIFI_PSK) - 1,
        .security = WIFI_SECURITY_TYPE_PSK,
        .channel = WIFI_CHANNEL_ANY,
        .band = WIFI_FREQ_BAND_2_4_GHZ,
    };
    net_mgmt(NET_REQUEST_WIFI_CONNECT, net_if_get_default(), &wifi, sizeof(wifi));
#endif
    serve();
    return 0;
}
