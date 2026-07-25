#include "bt_rssi.h"
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

int bt_read_rssi(const char *target_addr, int *rssi_out) {
    if (!target_addr || !rssi_out) return -1;

    int dev_id = hci_get_route(NULL);
    if (dev_id < 0) return -1;

    int sock = hci_open_dev(dev_id);
    if (sock < 0) return -1;          /* usually EPERM: needs CAP_NET_RAW */

    bdaddr_t bdaddr;
    if (str2ba(target_addr, &bdaddr) < 0) { close(sock); return -1; }

    struct hci_conn_info_req *cr =
        calloc(1, sizeof(*cr) + sizeof(struct hci_conn_info));
    if (!cr) { close(sock); return -1; }

    memcpy(&cr->bdaddr, &bdaddr, sizeof(bdaddr));
    cr->type = ACL_LINK;

    if (ioctl(sock, HCIGETCONNINFO, (unsigned long)cr) < 0) {
        free(cr); close(sock); return -1;   /* not connected */
    }

    uint16_t handle = cr->conn_info->handle;
    free(cr);

    struct {
        uint8_t  status;
        uint16_t handle;
        int8_t   rssi;
    } __attribute__((packed)) rp = {0};

    struct hci_request rq;
    memset(&rq, 0, sizeof(rq));
    rq.ogf    = OGF_STATUS_PARAM;
    rq.ocf    = OCF_READ_RSSI;
    rq.cparam = &handle;
    rq.clen   = sizeof(handle);
    rq.rparam = &rp;
    rq.rlen   = sizeof(rp);

    if (hci_send_req(sock, &rq, 1000) < 0) { close(sock); return -1; }
    close(sock);

    if (rp.status != 0) return -1;
    *rssi_out = rp.rssi;
    return 0;
}
