/* The layout of the C structs the network stack in Rust (rust/src/netstack.rs, kernel.rs)
 * shares with C. rust_init() compares these with its own copies at boot. */
#include "kernel.h"
#include "net.h"

const uint32_t rust_net_layout[12] = {
    sizeof(net_arp_t), sizeof(net_ipv4_t), sizeof(net_udp_t), sizeof(net_tcp_t), sizeof(net_dhcp_t),
    sizeof(netif_t), offsetof(netif_t, mac), offsetof(netif_t, ip), offsetof(netif_t, rx_dropped),
    sizeof(fw_event_t), sizeof(nic_ops_t), sizeof(pci_dev_t),
};

/* http_response_t, for rust/src/http.rs (the HTTP client) */
const uint32_t rust_http_layout[6] = {
    sizeof(http_response_t), offsetof(http_response_t, location), offsetof(http_response_t, body),
    offsetof(http_response_t, error), offsetof(http_response_t, secure), offsetof(http_response_t, tls_info),
};
