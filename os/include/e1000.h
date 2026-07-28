/* e1000.h — Intel 82540EM (QEMU's default "e1000") NIC driver. */
#ifndef E1000_H
#define E1000_H

#include <stdint.h>

/* Returns 0 on success, negative on failure. */
int  e1000_init(void);

/* Copy out the 6-byte MAC address. */
void e1000_get_mac(uint8_t mac[6]);

/* Transmit a raw Ethernet frame (copies `data`). Returns 0 on success. */
int  e1000_send(const void *data, uint16_t len);

/* Poll for one received frame. Returns 1 and fills buf/len if one was
 * available (buf must hold at least 2048 bytes), 0 otherwise. */
int  e1000_receive(void *buf, uint16_t *len);

#endif /* E1000_H */
