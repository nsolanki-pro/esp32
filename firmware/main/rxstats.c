// Received-frame counters, by wrapping the lwIP netif input function of the Wi-Fi station.
// Every frame the driver hands to the IP stack passes through here, so this sees unicast,
// broadcast and multicast traffic (ARP, DHCP, mDNS, SSDP, IPv6 ND, ...). It cannot see frames
// the driver drops itself, or beacons.

#include "rxstats.h"

#include <stdio.h>
#include <string.h>

#include "esp_netif_net_stack.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"

#define MAX_ENTRIES 24
#define ETH_HDR     14

typedef struct {
    char label[28];
    uint32_t count;
    uint32_t bytes;
    char last_src[18];  // IPv4 address or MAC of the last sender
} entry_t;

static netif_input_fn s_orig_input;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static entry_t s_tab[MAX_ENTRIES];
static int s_n;
static uint32_t s_total, s_other;
static int64_t s_since_us;

static const char *port_name(unsigned port)
{
    switch (port) {
    case 53: return "DNS";
    case 67: case 68: return "DHCP";
    case 80: return "HTTP";
    case 123: return "NTP";
    case 137: case 138: return "NetBIOS";
    case 546: case 547: return "DHCPv6";
    case 1900: return "SSDP";
    case 3702: return "WS-Discovery";
    case 5353: return "mDNS";
    case 5355: return "LLMNR";
    default: return NULL;
    }
}

static void fmt_port(char *out, size_t len, const char *proto, unsigned port)
{
    const char *name = port_name(port);
    if (name) {
        snprintf(out, len, "%s:%u %s", proto, port, name);
    } else {
        snprintf(out, len, "%s:%u", proto, port);
    }
}

static void classify(const struct pbuf *p)
{
    if (p->len < ETH_HDR) {
        return;
    }
    const uint8_t *f = p->payload;
    const uint8_t *dst = f, *smac = f + 6;
    unsigned type = f[12] << 8 | f[13];
    const char *cast = (dst[0] & dst[1] & dst[2] & dst[3] & dst[4] & dst[5]) == 0xff ? "bcast"
                       : (dst[0] & 1) ? "mcast" : "ucast";

    char what[20];
    char src[18];
    snprintf(src, sizeof(src), "%02x:%02x:%02x:%02x:%02x:%02x",
             smac[0], smac[1], smac[2], smac[3], smac[4], smac[5]);

    if (type == 0x0806) {
        snprintf(what, sizeof(what), "ARP");
    } else if (type == 0x0800 && p->len >= ETH_HDR + 20) {
        const uint8_t *ip = f + ETH_HDR;
        unsigned ihl = (ip[0] & 0x0f) * 4, proto = ip[9];
        snprintf(src, sizeof(src), "%u.%u.%u.%u", ip[12], ip[13], ip[14], ip[15]);
        if ((proto == 6 || proto == 17) && p->len >= ETH_HDR + ihl + 4) {
            unsigned dport = ip[ihl + 2] << 8 | ip[ihl + 3];
            fmt_port(what, sizeof(what), proto == 6 ? "TCP" : "UDP", dport);
        } else if (proto == 1) {
            snprintf(what, sizeof(what), "ICMP");
        } else if (proto == 2) {
            snprintf(what, sizeof(what), "IGMP");
        } else {
            snprintf(what, sizeof(what), "IPv4 proto %u", proto);
        }
    } else if (type == 0x86dd && p->len >= ETH_HDR + 40) {
        const uint8_t *ip6 = f + ETH_HDR;
        unsigned nh = ip6[6];
        if (nh == 58) {
            snprintf(what, sizeof(what), "ICMPv6");
        } else if ((nh == 6 || nh == 17) && p->len >= ETH_HDR + 44) {
            unsigned dport = ip6[42] << 8 | ip6[43];
            fmt_port(what, sizeof(what), nh == 6 ? "TCP6" : "UDP6", dport);
        } else {
            snprintf(what, sizeof(what), "IPv6 nh %u", nh);
        }
    } else {
        snprintf(what, sizeof(what), "eth 0x%04x", type);
    }

    char label[28];
    snprintf(label, sizeof(label), "%s %s", cast, what);

    portENTER_CRITICAL(&s_lock);
    s_total++;
    int i;
    for (i = 0; i < s_n && strcmp(s_tab[i].label, label) != 0; i++) {
    }
    if (i == s_n && s_n < MAX_ENTRIES) {
        strcpy(s_tab[s_n].label, label);
        s_n++;
    }
    if (i < s_n) {
        s_tab[i].count++;
        s_tab[i].bytes += p->tot_len;
        strcpy(s_tab[i].last_src, src);
    } else {
        s_other++;
    }
    portEXIT_CRITICAL(&s_lock);
}

static err_t input_wrap(struct pbuf *p, struct netif *inp)
{
    classify(p);
    return s_orig_input(p, inp);
}

void rxstats_install(esp_netif_t *netif)
{
    struct netif *n = esp_netif_get_netif_impl(netif);
    if (!n || n->input == input_wrap) {
        return;
    }
    s_since_us = esp_timer_get_time();
    s_orig_input = n->input;
    n->input = input_wrap;
}

size_t rxstats_json(char *buf, size_t len, bool reset)
{
    entry_t tab[MAX_ENTRIES];
    int n;
    uint32_t total, other;
    int64_t since;

    portENTER_CRITICAL(&s_lock);
    memcpy(tab, s_tab, sizeof(tab));
    n = s_n;
    total = s_total;
    other = s_other;
    since = s_since_us;
    if (reset) {
        memset(s_tab, 0, sizeof(s_tab));
        s_n = 0;
        s_total = s_other = 0;
        s_since_us = esp_timer_get_time();
    }
    portEXIT_CRITICAL(&s_lock);

    // Sort by count, descending (tiny table: insertion sort).
    for (int i = 1; i < n; i++) {
        entry_t e = tab[i];
        int j = i - 1;
        for (; j >= 0 && tab[j].count < e.count; j--) {
            tab[j + 1] = tab[j];
        }
        tab[j + 1] = e;
    }

    double secs = (esp_timer_get_time() - since) / 1e6;
    size_t o = snprintf(buf, len, "{\"seconds\":%.0f,\"frames\":%lu,\"per_min\":%.1f,\"other\":%lu,\"kinds\":[",
                        secs, (unsigned long)total, secs > 0 ? total * 60.0 / secs : 0.0,
                        (unsigned long)other);
    for (int i = 0; i < n && o < len; i++) {
        o += snprintf(buf + o, len - o,
                      "%s{\"kind\":\"%s\",\"count\":%lu,\"per_min\":%.1f,\"bytes\":%lu,\"last_src\":\"%s\"}",
                      i ? "," : "", tab[i].label, (unsigned long)tab[i].count,
                      secs > 0 ? tab[i].count * 60.0 / secs : 0.0, (unsigned long)tab[i].bytes, tab[i].last_src);
    }
    if (o < len) {
        o += snprintf(buf + o, len - o, "]}");
    }
    return o < len ? o : len - 1;
}
