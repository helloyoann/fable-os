/* net.h — bring up networking and ask the AI a question over HTTPS. */
#ifndef NET_H
#define NET_H

/* Initialise lwIP + the e1000 netif, the TLS config, and resolve the API host.
 * Call once, after drivers_init(). Returns 0 on success. */
int net_init(void);

/* Send `question` to the Anthropic Messages API over TLS and print the reply.
 * Returns 0 if a response was received (even an API error like 401), -1 on a
 * connection/TLS failure. */
int net_ask(const char *question);

#endif /* NET_H */
