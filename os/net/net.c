/* net.c — lwIP netif driver for the e1000, plus an HTTPS client that talks
 * directly to the Anthropic Messages API.
 *
 * The kernel now does TLS itself (mbedTLS via lwIP's altcp_tls layer), so there
 * is no host proxy: we DNS-resolve api.anthropic.com, open a TLS connection on
 * 443 (with SNI), POST /v1/messages, and print the reply.
 *
 * Certificate verification is OFF (see lwipopts.h / README): the connection is
 * encrypted but not authenticated. Good enough for a hobby OS, not for secrets.
 *
 * The request is HTTP/1.0 so the response is close-delimited (no chunked
 * encoding to parse). We buffer the whole response, then print the status line
 * and the assistant text extracted from the JSON. */

#include "kernel.h"
#include "net.h"
#include "e1000.h"
#include <string.h>            /* strstr, via the port shim */

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/dns.h"
#include "lwip/altcp.h"
#include "lwip/altcp_tls.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
#include "mbedtls/ssl.h"

#ifndef TALKOS_API_KEY
#define TALKOS_API_KEY ""
#endif

#define API_HOST  "api.anthropic.com"
#define API_PORT  443
#define API_MODEL "claude-opus-4-8"

/* ====================================================================== */
/* netif driver                                                            */
/* ====================================================================== */

static struct netif e1000_netif;

static err_t e1000_linkoutput(struct netif *netif, struct pbuf *p) {
    (void)netif;
    static uint8_t buf[1600];
    if (p->tot_len > sizeof buf) return ERR_IF;
    pbuf_copy_partial(p, buf, p->tot_len, 0);
    if (e1000_send(buf, p->tot_len) != 0) return ERR_IF;
    return ERR_OK;
}

static err_t e1000_netif_init(struct netif *netif) {
    netif->name[0] = 'e';
    netif->name[1] = 'n';
    netif->output = etharp_output;
    netif->linkoutput = e1000_linkoutput;
    netif->mtu = 1500;
    netif->hwaddr_len = 6;
    e1000_get_mac(netif->hwaddr);
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

static void net_poll_rx(void) {
    static uint8_t frame[2048];
    uint16_t len;
    while (e1000_receive(frame, &len)) {
        struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
        if (!p) continue;
        pbuf_take(p, frame, len);
        if (e1000_netif.input(p, &e1000_netif) != ERR_OK)
            pbuf_free(p);
    }
}

static void net_service(void) {
    net_poll_rx();
    sys_check_timeouts();
}

/* ====================================================================== */
/* setup: netif, TLS config, DNS                                           */
/* ====================================================================== */

static struct altcp_tls_config *tls_conf;
static ip_addr_t server_ip;
static volatile int dns_done, dns_ok;

static void dns_cb(const char *name, const ip_addr_t *ipaddr, void *arg) {
    (void)name; (void)arg;
    if (ipaddr) { server_ip = *ipaddr; dns_ok = 1; }
    dns_done = 1;
}

int net_init(void) {
    lwip_init();

    ip4_addr_t ip, nm, gw;
    IP4_ADDR(&ip, 10, 0, 2, 15);
    IP4_ADDR(&nm, 255, 255, 255, 0);
    IP4_ADDR(&gw, 10, 0, 2, 2);
    netif_add(&e1000_netif, &ip, &nm, &gw, NULL, e1000_netif_init, netif_input);
    netif_set_default(&e1000_netif);
    netif_set_up(&e1000_netif);

    ip_addr_t dnssrv;
    IP_ADDR4(&dnssrv, 10, 0, 2, 3);
    dns_setserver(0, &dnssrv);
    kprintf("net: up at 10.0.2.15/24\n");

    /* TLS client config with no CA -> handshake but no trust check. Seeds the
     * CTR_DRBG from mbedtls_hardware_poll(), so this is where entropy is used. */
    tls_conf = altcp_tls_create_config_client(NULL, 0);
    if (!tls_conf) { kprintf("net: TLS config init failed\n"); return -1; }

    kprintf("net: resolving %s ...\n", API_HOST);
    err_t e = dns_gethostbyname(API_HOST, &server_ip, dns_cb, NULL);
    if (e == ERR_OK) { dns_ok = 1; dns_done = 1; }
    else if (e != ERR_INPROGRESS) { kprintf("net: dns error %d\n", e); return -1; }

    uint64_t deadline = millis() + 10000;
    while (!dns_done && millis() < deadline) net_service();
    if (!dns_ok) { kprintf("net: DNS resolution failed\n"); return -1; }
    kprintf("net: %s -> %s, TLS ready\n", API_HOST, ipaddr_ntoa(&server_ip));
    return 0;
}

/* ====================================================================== */
/* HTTPS request / response                                                */
/* ====================================================================== */

static char         resp[65536];
static volatile int resp_len;
static volatile int ask_done;
static int          ask_ok;
static const char  *req_buf;
static int          req_len;

static err_t recv_cb(void *arg, struct altcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg; (void)err;
    if (p == NULL) { altcp_close(pcb); ask_done = 1; return ERR_OK; }
    for (struct pbuf *q = p; q; q = q->next) {
        const char *d = q->payload;
        for (int i = 0; i < q->len; i++)
            if (resp_len < (int)sizeof resp - 1) resp[resp_len++] = d[i];
    }
    altcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void err_cb(void *arg, err_t err) {
    (void)arg;
    kprintf("\n[net error %d]\n", err);
    ask_done = 1;
}

static err_t connected_cb(void *arg, struct altcp_pcb *pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK) { ask_done = 1; return err; }
    ask_ok = 1;                                  /* TLS handshake completed */
    altcp_write(pcb, req_buf, req_len, TCP_WRITE_FLAG_COPY);
    altcp_output(pcb);
    return ERR_OK;
}

/* --- small string builders (no libc snprintf in the hot path) --- */
static void put(char *dst, int *off, int cap, const char *s) {
    while (*s && *off < cap - 1) dst[(*off)++] = *s++;
}
static void put_uint(char *dst, int *off, int cap, unsigned v) {
    char t[10]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = '0' + v % 10; v /= 10; }
    while (n && *off < cap - 1) dst[(*off)++] = t[--n];
}
/* Append `s` as a JSON string body (no quotes), escaping as needed. */
static void put_json_escaped(char *dst, int *off, int cap, const char *s) {
    static const char hex[] = "0123456789abcdef";
    for (; *s && *off < cap - 8; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { dst[(*off)++] = '\\'; dst[(*off)++] = c; }
        else if (c == '\n') { dst[(*off)++] = '\\'; dst[(*off)++] = 'n'; }
        else if (c == '\r') { dst[(*off)++] = '\\'; dst[(*off)++] = 'r'; }
        else if (c == '\t') { dst[(*off)++] = '\\'; dst[(*off)++] = 't'; }
        else if (c < 0x20) {
            dst[(*off)++] = '\\'; dst[(*off)++] = 'u'; dst[(*off)++] = '0';
            dst[(*off)++] = '0';  dst[(*off)++] = hex[(c >> 4) & 0xF];
            dst[(*off)++] = hex[c & 0xF];
        } else dst[(*off)++] = c;
    }
}

/* Print the JSON value of the first "text":"..." in [body,end), unescaping the
 * common escapes. Returns 1 if found. */
static int print_answer_text(const char *body) {
    const char *m = strstr(body, "\"text\":\"");
    if (!m) return 0;
    const char *p = m + 8;
    for (; *p; p++) {
        if (*p == '\\') {
            p++;
            switch (*p) {
                case 'n': kputc('\n'); break;
                case 't': kputc('\t'); break;
                case 'r': break;
                case 'u': {            /* \uXXXX: render ASCII, else '?' */
                    int v = 0;
                    for (int k = 0; k < 4 && p[1]; k++) {
                        char h = *++p; v <<= 4;
                        if (h >= '0' && h <= '9') v |= h - '0';
                        else if (h >= 'a' && h <= 'f') v |= h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') v |= h - 'A' + 10;
                    }
                    kputc(v >= 0x20 && v < 0x7F ? (char)v : '?');
                    break;
                }
                case '\0': return 1;
                default: kputc(*p); break;   /* \" \\ \/ etc. */
            }
        } else if (*p == '"') {
            break;                            /* end of the string value */
        } else {
            kputc(*p);
        }
    }
    return 1;
}

int net_ask(const char *question) {
    /* Build the JSON body. */
    static char body[8192];
    int boff = 0;
    put(body, &boff, sizeof body,
        "{\"model\":\"" API_MODEL "\",\"max_tokens\":1024,"
        "\"messages\":[{\"role\":\"user\",\"content\":\"");
    put_json_escaped(body, &boff, sizeof body, question);
    put(body, &boff, sizeof body, "\"}]}");
    body[boff] = '\0';

    /* Build the HTTP/1.0 request (close-delimited response, no chunking). */
    static char req[12288];
    int off = 0;
    put(req, &off, sizeof req,
        "POST /v1/messages HTTP/1.0\r\n"
        "Host: " API_HOST "\r\n"
        "x-api-key: " TALKOS_API_KEY "\r\n"
        "anthropic-version: 2023-06-01\r\n"
        "content-type: application/json\r\n"
        "connection: close\r\n"
        "content-length: ");
    put_uint(req, &off, sizeof req, (unsigned)boff);
    put(req, &off, sizeof req, "\r\n\r\n");
    for (int i = 0; i < boff && off < (int)sizeof req; i++) req[off++] = body[i];
    req_buf = req;
    req_len = off;

    resp_len = 0;
    ask_done = ask_ok = 0;

    struct altcp_pcb *pcb = altcp_tls_new(tls_conf, IPADDR_TYPE_V4);
    if (!pcb) { kprintf("[net: altcp_tls_new failed]\n"); return -1; }

    /* Send SNI so CDN-fronted hosts present the right certificate. */
    mbedtls_ssl_context *ssl = (mbedtls_ssl_context *)altcp_tls_context(pcb);
    if (ssl) mbedtls_ssl_set_hostname(ssl, API_HOST);

    altcp_recv(pcb, recv_cb);
    altcp_err(pcb, err_cb);
    altcp_connect(pcb, &server_ip, API_PORT, connected_cb);

    uint64_t deadline = millis() + 90000;   /* handshake + model latency */
    while (!ask_done && millis() < deadline) net_service();
    if (!ask_done) { kprintf("\n[net: timed out]\n"); return -1; }
    if (!ask_ok)   { kprintf("[net: TLS/connect failed]\n"); return -1; }

    resp[resp_len] = '\0';

    /* Show the HTTP status line, then the answer (or the raw JSON on error). */
    const char *eol = strstr(resp, "\r\n");
    kputs("[http] ");
    for (const char *s = resp; s != eol && *s; s++) kputc(*s);
    kputc('\n');

    const char *body_start = strstr(resp, "\r\n\r\n");
    body_start = body_start ? body_start + 4 : resp;
    if (!print_answer_text(body_start))
        kputs(body_start);                  /* e.g. a 401 error JSON */
    kputc('\n');
    return 0;
}
