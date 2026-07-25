#ifndef BT_RSSI_H
#define BT_RSSI_H

/* Reads the RSSI of the active ACL connection to `target_addr` via a raw HCI
   Read_RSSI command.

   Returns 0 and stores dBm in *rssi_out on success, negative on failure
   (no HCI adapter, no connection to that address, or insufficient privileges).

   NOTE: opening an HCI socket needs CAP_NET_RAW. Either run as root or grant it
   once:  sudo setcap cap_net_raw+ep ./aether_sender
   Without it this always fails and the caller should treat the link as unknown
   rather than assume the worst. */
int bt_read_rssi(const char *target_addr, int *rssi_out);

#endif /* BT_RSSI_H */
