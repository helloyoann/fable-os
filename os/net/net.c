/* net.c — lwIP netif driver for the e1000, plus the TLS transport that carries
 * the model protocol to api.anthropic.com.
 *
 * The kernel does TLS itself (mbedTLS via lwIP's altcp_tls layer), so there is
 * no host proxy: we DNS-resolve api.anthropic.com, open a TLS connection on 443
 * (with SNI), POST /v1/messages, and hand the response body back up.
 *
 * CERTIFICATE VERIFICATION is a build-time choice, default OFF.
 *
 *   plain `make`                         the historical behaviour: no CA chain,
 *                                        MBEDTLS_SSL_VERIFY_NONE. The server's
 *                                        certificate is parsed and then
 *                                        believed. Encrypted, not
 *                                        authenticated, and MITM-able by
 *                                        anything that can answer on 443.
 *   `make EXTRA_CFLAGS=-DTALKOS_VERIFY_CERTS`
 *                                        the chain must build to one of the
 *                                        roots pinned in net/tls_ca.c, the
 *                                        certificate must name the host we
 *                                        asked for, and notBefore/notAfter must
 *                                        contain the CMOS clock's idea of now.
 *
 * Every TALKOS_VERIFY_CERTS block below is additive; with the flag absent this
 * file compiles to exactly what it did before. See include/tls_ca.h for which
 * roots are trusted, why those, and what breaks when they rotate.
 *
 * The request is HTTP/1.0 so the response is close-delimited (no chunked
 * encoding to parse). We buffer the whole response, split off the headers, and
 * return the body.
 *
 * LAYERING — this file is deliberately the *only* place that knows about lwIP:
 *
 *     net_ask()            prompt -> JSON -> print       (protocol, no sockets)
 *       model_build_request()  net/model.c   builds & escapes the request body
 *       model_send()           net/model.c   dispatches through a transport
 *         tls_send()           here          HTTP/1.0 over altcp_tls
 *       json_msg_text()        net/json.c    reads the reply
 *
 * Swap model_tls_transport() for model_mock_transport() and the exact same
 * net_ask() logic runs on the host with no network — which is how the tool-use
 * loop will be tested.
 */

#include "kernel.h"
#include "net.h"
#include "e1000.h"
#include "json.h"
#include "model.h"

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/dns.h"
#include "lwip/altcp.h"
#include "lwip/altcp_tls.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
#include "mbedtls/ssl.h"

#ifdef TALKOS_VERIFY_CERTS
#include "tls_ca.h"
#include "rtc.h"
#include "mbedtls/x509_crt.h"

/* The verify mode is not set here — it is set in port/lwipopts.h and consumed
 * by lwIP's altcp_tls_mbedtls.c. Refuse to build a kernel that claims to verify
 * while that knob says otherwise, so the two cannot drift apart quietly. */
#if !defined(ALTCP_MBEDTLS_AUTHMODE) || ALTCP_MBEDTLS_AUTHMODE != MBEDTLS_SSL_VERIFY_REQUIRED
#error "TALKOS_VERIFY_CERTS requires ALTCP_MBEDTLS_AUTHMODE == MBEDTLS_SSL_VERIFY_REQUIRED (see port/lwipopts.h)"
#endif
#endif

/* Streaming is opt-in at build time (`make EXTRA_CFLAGS=-DTALKOS_STREAM`).
 * Without it this file behaves exactly as it always has: one buffered response,
 * printed when it is complete. With it, the request carries "stream":true and
 * the response is a text/event-stream that net/sse.c turns back into the same
 * response body while printing the model's text deltas as they land. Every
 * TALKOS_STREAM block below is additive; the default path is untouched (proven
 * by compiling this file with and without the streaming source and diffing the
 * generated code — identical).
 *
 * KNOWN GAP under the flag: net_ask() below suppresses its own second print of
 * text the transport already streamed, but net/chat.c cannot — it needs the
 * same one-line sse_http_streamed() guard in print_text_block(), and chat.c is
 * not this change's to edit. See the SCOPE section of include/sse.h. */
#ifdef TALKOS_STREAM
#include "sse.h"
#endif

#ifndef TALKOS_API_KEY
#define TALKOS_API_KEY ""
#endif

/* The endpoint. Both names are overridable, but ONLY in a verification build,
 * because their only purpose is to prove that the verifier rejects things:
 *
 *   -DTALKOS_TLS_HOST='"example.com"'    resolve and connect somewhere else, so
 *                                        the presented chain is anchored in a
 *                                        root this kernel does not trust
 *                                        -> MBEDTLS_X509_BADCERT_NOT_TRUSTED
 *   -DTALKOS_TLS_SNI='"wrong.invalid"'   ask the verifier for a name the
 *                                        certificate does not carry
 *                                        -> MBEDTLS_X509_BADCERT_CN_MISMATCH
 *   -DTALKOS_TLS_PORT=8443               with TALKOS_TLS_HOST='"10.0.2.2"',
 *                                        point the kernel at a TLS server on
 *                                        the QEMU host — the offline way to
 *                                        present a self-signed certificate and
 *                                        watch it be refused. lwIP's
 *                                        dns_gethostbyname() short-circuits a
 *                                        dotted quad, so no DNS is involved.
 *
 * A verifier that has never been watched saying no is not known to work, and
 * these three lines are how that gets watched. In the default build none of the
 * macros can have any effect: the #ifdefs collapse to the original literals. */
#if defined(TALKOS_VERIFY_CERTS) && defined(TALKOS_TLS_HOST)
#define API_HOST   TALKOS_TLS_HOST
#else
#define API_HOST   "api.anthropic.com"
#endif

/* The name the certificate must carry. Sent as SNI in every build; under
 * TALKOS_VERIFY_CERTS it is additionally the name mbedTLS matches the
 * certificate's CN/subjectAltName against, which is the half of verification
 * that a CA bundle alone does not give you — a valid certificate for some
 * other host is still the wrong certificate. */
#if defined(TALKOS_VERIFY_CERTS) && defined(TALKOS_TLS_SNI)
#define API_SNI    TALKOS_TLS_SNI
#else
#define API_SNI    API_HOST
#endif

#if defined(TALKOS_VERIFY_CERTS) && defined(TALKOS_TLS_PORT)
#define API_PORT   TALKOS_TLS_PORT
#else
#define API_PORT   443
#endif
#define API_MODEL  "claude-opus-4-8"
#define API_TOKENS 1024

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

#ifdef TALKOS_VERIFY_CERTS
/* ---------------------------------------------------------------------- */
/* certificate verification: setup and reporting                           */
/* ---------------------------------------------------------------------- */

/* The ssl context of the connection currently being attempted, so a handshake
 * failure can be turned into a sentence instead of "[net error -15]".
 *
 * LIFETIME, because this is a raw pointer into lwIP's allocation: it is set
 * immediately after altcp_tls_new() and cleared the moment it is read or the
 * attempt ends. lwIP calls the error callback *before* it frees the mbedTLS
 * state (altcp_mbedtls_lower_recv_process() and altcp_mbedtls_lower_err() both
 * invoke conn->err and only then close/free), so the context is still alive
 * inside err_cb — and nowhere else. Nothing outside this file may touch it. */
static mbedtls_ssl_context *cur_ssl;

/* One line describing why a certificate was refused, pointed at by tls_err.
 * Static because tls_err outlives the call that produced it. */
static char verify_line[192];

static int tls_verify_init(void) {
    /* Refuse to come up with a trust store we cannot vouch for. A bundle that
     * silently failed to parse would leave mbedTLS with an empty CA chain and
     * VERIFY_REQUIRED, i.e. every handshake failing for a reason that looks
     * like the network. Check the shape here so the message is the truth. */
    int rc = tls_ca_bundle_selftest();
    if (rc != TLS_CA_OK) {
        kprintf("net: embedded CA bundle is malformed (%d) - refusing to run "
                "with a trust store this kernel cannot parse\n", rc);
        return -1;
    }

    tls_conf = altcp_tls_create_config_client((const u8_t *)tls_ca_bundle(),
                                              tls_ca_bundle_len());
    if (!tls_conf) {
        /* lwIP folds "out of memory" and "mbedtls_x509_crt_parse rejected the
         * bundle" into one NULL, and its own diagnostics go to LWIP_DEBUGF,
         * which is compiled out here. Say both possibilities out loud. */
        kprintf("net: TLS config init failed - the CA bundle did not parse, "
                "or the heap could not hold it\n");
        return -1;
    }

    kprintf("net: certificate verification ON (%d pinned root%s, "
            "hostname + validity enforced)\n",
            tls_ca_root_count(), tls_ca_root_count() == 1 ? "" : "s");
    for (int i = 0; i < tls_ca_root_count(); i++) {
        const tls_ca_root_t *r = tls_ca_root(i);
        kprintf("net:   trust %s (%s, pin expires %s) - %s\n",
                r->name, r->key, r->not_after, r->why);
    }

    /* PROVE the date check is compiled in, do not trust that it is. Without
     * MBEDTLS_HAVE_TIME_DATE, mbedTLS compiles mbedtls_x509_time_is_past() to
     * `return 0` — every certificate is forever in date and this banner would be
     * a lie. The defines live in include/mbedtls_config.h and reach mbedTLS
     * through a different set of object files than this one, so one stale object
     * is enough to separate the claim from the behaviour. Ask a question whose
     * answer can only be yes: is 1971 in the past?
     *
     * (Not decorative. `make EXTRA_CFLAGS=-DTALKOS_VERIFY_CERTS` in a tree that
     * was already built does not rebuild mbedTLS, so this is the difference
     * between finding out at boot and finding out never.) */
    {
        mbedtls_x509_time past;
        past.year = 1971; past.mon = 1; past.day = 1;
        past.hour = 0;    past.min = 0; past.sec = 0;
        if (!mbedtls_x509_time_is_past(&past)) {
            kprintf("net: REFUSING to claim verification: this build's mbedTLS "
                    "does not check certificate dates (MBEDTLS_HAVE_TIME_DATE "
                    "missing, or stale objects). Rebuild from clean.\n");
            altcp_tls_free_config(tls_conf);
            tls_conf = (struct altcp_tls_config *)0;
            return -1;
        }
    }

    /* The clock is half of the check, so it gets stated, not assumed. */
    int64_t   now = tls_ca_now_unix();
    rtc_time_t t;
    char       iso[RTC_ISO8601_MAX];
    if (now != TLS_CA_NO_CLOCK && rtc_from_unix(now, &t) == RTC_OK &&
        rtc_format_iso8601(&t, iso, sizeof iso) > 0) {
        kprintf("net:   validity is checked against %s (CMOS RTC, assumed UTC)\n",
                iso);
    } else {
        kprintf("net:   WARNING: no readable wall clock. Every certificate will "
                "look not-yet-valid and every handshake will fail.\n");
    }
    return 0;
}

/* Fold mbedTLS's multi-line verify_info into one line, print the full thing,
 * and hand the caller a string it can use as the transport's last error.
 * Returns NULL when the failure was not a trust failure.
 *
 * Called only from err_cb, only before lwIP frees the mbedTLS state. */
static const char *tls_verify_failure_reason(void) {
    if (!cur_ssl) return (const char *)0;
    uint32_t flags = mbedtls_ssl_get_verify_result(cur_ssl);
    cur_ssl = (mbedtls_ssl_context *)0;   /* one look; the context dies next */

    /* 0xFFFFFFFF is mbedTLS's "there is no session", i.e. we never got as far
     * as a certificate — a TCP reset, a timeout, an alert. 0 means the chain
     * verified and something else went wrong. Neither is a trust failure and
     * neither should be reported as one. */
    if (flags == 0 || flags == 0xFFFFFFFFu) return (const char *)0;

    /* Do not blame the peer for our own broken hardware. tls_ca_now_unix()
     * fails closed to the epoch when the CMOS is unreadable, which makes every
     * certificate look not-yet-valid — so BADCERT_FUTURE plus a dead clock is a
     * clock fault, and reporting it as "the certificate validity starts in the
     * future" sends the operator hunting a certificate that is perfectly fine.
     * The boot banner only covers a clock that was already dead at boot; this
     * covers one that stops later, because mbedTLS re-reads it every
     * comparison. */
    if ((flags & MBEDTLS_X509_BADCERT_FUTURE) &&
        tls_ca_now_unix() == TLS_CA_NO_CLOCK) {
        kprintf("\n[tls] cannot judge %s: the CMOS clock is unreadable, so every "
                "certificate looks not-yet-valid. This is a clock fault, not a "
                "certificate fault.\n", API_SNI);
        return "wall clock unreadable - certificate dates cannot be checked";
    }

    char info[256];
    int  n = mbedtls_x509_crt_verify_info(info, sizeof info, "", flags);
    if (n <= 0) return "certificate rejected";
    if (n > (int)sizeof info - 1) n = (int)sizeof info - 1;
    info[n] = '\0';

    /* verify_info emits one reason per line; squash to "a; b" so the single
     * line net_ask() prints carries every reason rather than the first. */
    int w = 0;
    for (int i = 0; i < n && w < (int)sizeof verify_line - 3; i++) {
        if (info[i] == '\n') {
            if (i + 1 >= n) break;                 /* trailing newline */
            verify_line[w++] = ';';
            verify_line[w++] = ' ';
        } else {
            verify_line[w++] = info[i];
        }
    }
    verify_line[w] = '\0';

    kprintf("\n[tls] certificate REJECTED for %s (verify flags 0x%x)\n",
            API_SNI, (unsigned)flags);
    kprintf("[tls] %s\n", verify_line);
    return verify_line;
}

/* On the way up, say what was actually proven. "TLS ok" is not evidence; the
 * subject, the issuer and the expiry date are. */
static void tls_report_verify_success(mbedtls_ssl_context *ssl) {
    /* Ask the live session what mode it actually ran in, rather than inferring
     * it from the flag this file was compiled with. ALTCP_MBEDTLS_AUTHMODE is
     * consumed by lwIP's altcp_tls_mbedtls.c — a different object — and under
     * VERIFY_NONE mbedTLS skips verification and leaves verify_result at 0,
     * which is indistinguishable from success. So the only honest source for
     * "was this chain actually checked" is the config the handshake used. */
    if (!ssl || !ssl->conf || ssl->conf->authmode != MBEDTLS_SSL_VERIFY_REQUIRED) {
        kprintf("[tls] NOT VERIFIED: this connection ran with authmode %d, not "
                "VERIFY_REQUIRED. The certificate was not trust-checked.\n",
                (ssl && ssl->conf) ? (int)ssl->conf->authmode : -1);
        return;
    }

    kprintf("[tls] verified: chain trusted, hostname %s matches", API_SNI);

    const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(ssl);
    if (peer) {
        kprintf(", valid %d-%d-%d..%d-%d-%d UTC",
                peer->valid_from.year, peer->valid_from.mon, peer->valid_from.day,
                peer->valid_to.year,   peer->valid_to.mon,   peer->valid_to.day);
    }
    kputc('\n');
}
#endif /* TALKOS_VERIFY_CERTS */

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

    /* The TLS client config. Either way this is where the CTR_DRBG is seeded
     * from mbedtls_hardware_poll() — see the entropy caveat in the READMEs. */
#ifdef TALKOS_VERIFY_CERTS
    if (tls_verify_init() != 0) return -1;
#else
    /* No CA -> handshake but no trust check. */
    tls_conf = altcp_tls_create_config_client(NULL, 0);
    if (!tls_conf) { kprintf("net: TLS config init failed\n"); return -1; }
#endif

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
/* the TLS transport (implements model_transport_t)                        */
/* ====================================================================== */

/* Receive state. The caller's response buffer doubles as the receive buffer:
 * headers land in it too and the body is shifted to the front afterwards, so
 * the transport needs no 64 KiB static of its own. */
static char        *rx_buf;
static int          rx_cap;
static volatile int rx_len;
static volatile int rx_overflow;

static volatile int ask_done;
static int          ask_ok;
static const char  *req_buf;
static int          req_len;

/* How much of req_buf is still waiting to go out (see tx_pump). */
static const char  *tx_ptr;
static int          tx_left;

static const char  *tls_err = "";

#ifdef TALKOS_STREAM
/* ~20 KiB of parser state. Static on purpose: the kernel stack is 64 KiB. */
static sse_http_t stream;

/* The model's voice, one delta at a time — the whole point of the flag. The
 * operator watches the sentence being written instead of waiting for it. */
static void stream_text(void *ctx, const char *text, size_t len) {
    (void)ctx;
    for (size_t i = 0; i < len; i++) kputc(text[i]);
}
#endif

static err_t recv_cb(void *arg, struct altcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg; (void)err;
    if (p == NULL) { altcp_close(pcb); ask_done = 1; return ERR_OK; }
    for (struct pbuf *q = p; q; q = q->next) {
        const char *d = q->payload;
#ifdef TALKOS_STREAM
        /* Straight into the state machine: it prints text deltas as they land
         * and reassembles the response body in place. Segments split an event
         * at arbitrary byte boundaries, which is sse.c's whole job. */
        sse_http_feed(&stream, d, (size_t)q->len);
#else
        for (int i = 0; i < q->len; i++) {
            if (rx_len < rx_cap - 1) rx_buf[rx_len++] = d[i];
            else                     rx_overflow = 1;
        }
#endif
    }
    altcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void err_cb(void *arg, err_t err) {
    (void)arg;
#ifdef TALKOS_VERIFY_CERTS
    /* A refused certificate arrives here as a bare ERR_CLSD, which is
     * indistinguishable from the peer hanging up. Ask mbedTLS what it decided
     * before lwIP tears the context down; if it was a trust failure, that
     * becomes the transport's error and net_ask() prints it instead of "-15". */
    const char *why = tls_verify_failure_reason();
    if (why) { tls_err = why; ask_done = 1; return; }
#endif
    kprintf("\n[net error %d]\n", err);
    ask_done = 1;
}

/* Feed as much of the request into the connection as the send buffer will take
 * right now, and stop.
 *
 * The tool-use loop's request carries every registered tool's schema, so a body
 * is tens of kilobytes where the original one-shot prompt was a few hundred
 * bytes. That is far more than TCP_SND_BUF (lwipopts.h), and a single
 * altcp_write() of the whole thing simply returns ERR_MEM and sends nothing —
 * which looks, from up here, like the server resetting the connection. So:
 * write what fits, and let the sent callback pump the rest as the peer acks. */
static void tx_pump(struct altcp_pcb *pcb) {
    while (tx_left > 0) {
        u16_t room = altcp_sndbuf(pcb);
        if (room == 0) break;

        u16_t n = (tx_left < (int)room) ? (u16_t)tx_left : room;
        u8_t  flags = TCP_WRITE_FLAG_COPY |
                      (tx_left > (int)n ? TCP_WRITE_FLAG_MORE : 0);

        err_t e = altcp_write(pcb, tx_ptr, n, flags);
        if (e == ERR_MEM) break;                 /* try again from sent_cb */
        if (e != ERR_OK) { tls_err = "write failed"; ask_done = 1; return; }

        tx_ptr  += n;
        tx_left -= n;
    }
    altcp_output(pcb);
}

static err_t sent_cb(void *arg, struct altcp_pcb *pcb, u16_t len) {
    (void)arg; (void)len;
    tx_pump(pcb);
    return ERR_OK;
}

static err_t connected_cb(void *arg, struct altcp_pcb *pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK) { ask_done = 1; return err; }
    ask_ok = 1;                                  /* TLS handshake completed */
#ifdef TALKOS_VERIFY_CERTS
    /* Reaching here under VERIFY_REQUIRED means the chain built to a pinned
     * root, the name matched, and the validity period contained now. */
    if (cur_ssl) { tls_report_verify_success(cur_ssl); cur_ssl = (mbedtls_ssl_context *)0; }
#endif
    tx_ptr  = req_buf;
    tx_left = req_len;
    altcp_sent(pcb, sent_cb);
    tx_pump(pcb);
    return ERR_OK;
}

#ifndef TALKOS_STREAM
/* Find `needle` in [p, p+n). Returns the offset, or -1. (No strstr: the buffer
 * is network data and may legitimately contain NUL bytes.) */
static int find(const char *p, int n, const char *needle, int nlen) {
    for (int i = 0; i + nlen <= n; i++) {
        int k = 0;
        while (k < nlen && p[i + k] == needle[k]) k++;
        if (k == nlen) return i;
    }
    return -1;
}

/* Split "HTTP/1.1 401 Unauthorized\r\n..." into out->status_line and
 * out->http_status, then move the body to the front of the buffer. */
static int split_http(char *buf, int len, model_response_t *out) {
    int eol = find(buf, len, "\r\n", 2);
    if (eol < 0) return MODEL_EPROTO;

    int n = eol < (int)sizeof out->status_line - 1
          ? eol : (int)sizeof out->status_line - 1;
    for (int i = 0; i < n; i++) out->status_line[i] = buf[i];
    out->status_line[n] = '\0';

    /* status code = the digits after the first space of the status line. Exactly
     * three digits: an unbounded run is signed overflow (undefined behaviour on
     * network-controlled input) and a long enough run wraps to a value that
     * passes `http_status == 200` in chat.c. */
    int sp = 0;
    while (sp < eol && buf[sp] != ' ') sp++;
    int code = 0, digits = 0, i = sp + 1;
    while (i < eol && buf[i] >= '0' && buf[i] <= '9' && digits < 3) {
        code = code * 10 + (buf[i] - '0');
        i++; digits++;
    }
    if (digits != 3) return MODEL_EPROTO;
    if (i < eol && buf[i] >= '0' && buf[i] <= '9') return MODEL_EPROTO;
    out->http_status = code;

    int hdr = find(buf, len, "\r\n\r\n", 4);
    int off = hdr >= 0 ? hdr + 4 : len;      /* no body: empty, not an error */
    int blen = len - off;
    if (blen > 0) memmove(buf, buf + off, (size_t)blen);
    buf[blen] = '\0';

    out->body     = buf;
    out->body_len = (size_t)blen;
    return MODEL_OK;
}
#endif /* !TALKOS_STREAM — sse.c does the splitting on the streaming path */

static int tls_send(model_transport_t *t,
                    const char *body, size_t body_len,
                    char *resp_buf, size_t resp_cap,
                    model_response_t *out) {
    (void)t;

    if (!tls_conf) { tls_err = "TLS not initialised"; return MODEL_ETRANSPORT; }
    if (resp_cap < 64) { tls_err = "response buffer too small"; return MODEL_EINVAL; }

    /* Build the HTTP/1.0 request (close-delimited response, no chunking). The
     * JSON writer is just a bounded appender, so it serves for plain text too. */
    /* Headers plus the whole JSON body. The turn loop's request carries every
     * registered tool's schema (~8 KiB for 18 tools and growing) on top of the
     * system prompt and the conversation, so this has to be well clear of
     * chat.h's CHAT_REQ_BYTES or the tool-use path dies with ENOSPC. */
    static char   req[65536];
    json_writer_t w;
    json_writer_init(&w, req, sizeof req);
    json_put(&w,
        "POST /v1/messages HTTP/1.0\r\n"
        "Host: " API_HOST "\r\n"
        "x-api-key: " TALKOS_API_KEY "\r\n"
        "anthropic-version: 2023-06-01\r\n"
        "content-type: application/json\r\n"
        "connection: close\r\n"
        "content-length: ");
#ifdef TALKOS_STREAM
    /* Ask for Server-Sent Events. net/model.c owns the body and must not learn
     * that transports exist, so the flag is spliced in as the first member of
     * the object it already built — the smallest change that leaves model.c
     * untouched. Refused unless the body really is an object with at least one
     * member; and the *response* is sniffed for its content-type regardless, so
     * a request we could not mark simply comes back unstreamed and buffered. */
    static const char STREAM_FLAG[] = "{\"stream\":true,";
    if (body && body_len >= 2 && body[0] == '{' && body[1] == '"') {
        json_put_uint(&w, (uint64_t)(body_len - 1 + sizeof STREAM_FLAG - 1));
        json_put(&w, "\r\n\r\n");
        json_put(&w, STREAM_FLAG);
        json_put_n(&w, body + 1, body_len - 1);
    } else {
        json_put_uint(&w, (uint64_t)body_len);
        json_put(&w, "\r\n\r\n");
        json_put_n(&w, body, body_len);
    }
#else
    json_put_uint(&w, (uint64_t)body_len);
    json_put(&w, "\r\n\r\n");
    json_put_n(&w, body, body_len);
#endif
    if (!json_writer_ok(&w)) { tls_err = "request too large"; return MODEL_ENOSPC; }

    req_buf = req;
    req_len = (int)json_writer_len(&w);

    rx_buf      = resp_buf;
    rx_cap      = (int)(resp_cap > 0x7FFFFFFF ? 0x7FFFFFFF : resp_cap);
    rx_len      = 0;
    rx_overflow = 0;
    ask_done    = 0;
    ask_ok      = 0;
    tx_ptr      = req;
    tx_left     = 0;
    tls_err     = "";
#ifdef TALKOS_STREAM
    sse_http_init(&stream, resp_buf, resp_cap, stream_text, (void *)0);
#endif

    struct altcp_pcb *pcb = altcp_tls_new(tls_conf, IPADDR_TYPE_V4);
    if (!pcb) { tls_err = "altcp_tls_new failed"; return MODEL_ETRANSPORT; }

    /* Send SNI so CDN-fronted hosts present the right certificate. Under
     * TALKOS_VERIFY_CERTS this same string is what mbedTLS matches the
     * certificate's CN/subjectAltName against, so a failure to set it would
     * quietly downgrade verification to "signed by someone we trust, for
     * whatever name they liked" — hence the hard failure below. */
    mbedtls_ssl_context *ssl = (mbedtls_ssl_context *)altcp_tls_context(pcb);
#ifdef TALKOS_VERIFY_CERTS
    if (!ssl || mbedtls_ssl_set_hostname(ssl, API_SNI) != 0) {
        altcp_close(pcb);
        tls_err = "could not set the hostname to verify against";
        return MODEL_ETRANSPORT;
    }
    cur_ssl = ssl;
#else
    if (ssl) mbedtls_ssl_set_hostname(ssl, API_SNI);
#endif

    altcp_recv(pcb, recv_cb);
    altcp_err(pcb, err_cb);
    altcp_connect(pcb, &server_ip, API_PORT, connected_cb);

    uint64_t deadline = millis() + 90000;   /* handshake + model latency */
    while (!ask_done && millis() < deadline) net_service();

#ifdef TALKOS_VERIFY_CERTS
    /* Past this point the pcb (and the mbedTLS state inside it) may already be
     * gone, and on the timeout path it is still alive but about to be. Either
     * way nobody may follow this pointer again. */
    cur_ssl = (mbedtls_ssl_context *)0;
#endif

    if (!ask_done) { tls_err = "timed out";           return MODEL_ETIMEOUT; }
    /* Do not overwrite a reason we already have. err_cb turns mbedTLS's verify
     * flags into a sentence and parks it here; clobbering that with a generic
     * string is how "the certificate expired" used to reach the operator as
     * "TLS/connect failed". */
    if (!ask_ok)   { if (!tls_err || !tls_err[0]) tls_err = "TLS/connect failed";
                     return MODEL_ETRANSPORT; }

#ifdef TALKOS_STREAM
    /* The body was consumed as it arrived; all that is left is to close the
     * reassembled document and hand up the same model_response_t the buffered
     * path produces. A non-SSE response (a 401, an error page) was buffered
     * verbatim by sse.c and comes back here unchanged. */
    int rc = sse_http_finish(&stream, out);
    if (rc != SSE_OK) {
        const char *why = sse_http_error(&stream);
        tls_err = (why && why[0]) ? why : sse_strerror(rc);
        return rc == SSE_ENOSPC ? MODEL_ENOSPC : MODEL_EPROTO;
    }
    return MODEL_OK;
#else
    resp_buf[rx_len] = '\0';
    int rc = split_http(resp_buf, rx_len, out);
    if (rc != MODEL_OK) { tls_err = "malformed HTTP response"; return rc; }
    out->truncated = rx_overflow;
    return MODEL_OK;
#endif
}

static const char *tls_last_error(model_transport_t *t) { (void)t; return tls_err; }

static model_transport_t tls_transport = {
    "tls", tls_send, tls_last_error, NULL
};

model_transport_t *model_tls_transport(void) {
    return tls_conf ? &tls_transport : NULL;
}

/* ====================================================================== */
/* net_ask — the protocol side: build, send, read, print                   */
/* ====================================================================== */

int net_ask(const char *question) {
    static char req_body[8192];
    static char resp[65536];
    static char text[16384];

    int n = model_build_request(req_body, sizeof req_body,
                                API_MODEL, API_TOKENS, question);
    if (n < 0) { kprintf("\n[net: %s]\n", model_strerror(n)); return -1; }

    model_transport_t *t = model_tls_transport();
    model_response_t   r;

    int rc = model_send(t, req_body, (size_t)n, resp, sizeof resp, &r);
    if (rc != MODEL_OK) {
        const char *why = model_last_error(t);
        kprintf("\n[net: %s]\n", (why && why[0]) ? why : model_strerror(rc));
        return -1;
    }

    /* Show the HTTP status line, then the answer (or the raw JSON on error). */
    kprintf("[http] %s\n", r.status_line);

    json_value_t root;
    int prc = json_parse(r.body, r.body_len, &root);
    int trc = prc == JSON_OK ? json_msg_text(&root, text, sizeof text, NULL)
                             : prc;
#ifdef TALKOS_STREAM
    /* Already on screen, delta by delta, as it arrived. */
    if (sse_http_streamed(&stream)) { kputc('\n'); return 0; }
#endif
    if (trc == JSON_OK || trc == JSON_ENOSPC) kputs(text);
    else                                      kputs(r.body);   /* e.g. a 401 */
    kputc('\n');
    return 0;
}
