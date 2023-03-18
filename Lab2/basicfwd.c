// Lab2 DPDK


// DPDK headers
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>

// Platform headers
#include <netinet/udp.h>
#include <linux/if_ether.h>
#include <netinet/ip.h>


// Standard headers
#include <stdint.h>
#include <stdio.h>

#define MAKE_IPV4_ADDR(a, b, c, d) (a + (b<<8) + (c<<16) + (d<<24))
static uint32_t g_src_ip = MAKE_IPV4_ADDR(10, 0, 0, 100);
static uint32_t g_dest_ip = MAKE_IPV4_ADDR(10, 0, 0, 101);
static uint8_t g_dest_mac_addr[ETH_ALEN] = { 0xe2, 0x7e, 0x25, 0xde, 0xbb, 0xe6 };

#define TX_RING_SIZE 128
#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 0
#define BURST_SIZE 32


static uint8_t g_src_mac_addr[ETH_ALEN]; // This will be set automatically at run time.

// DPDK supports many queues per port. Most simple apps only need one TX queue
// and one RX queue. You only need more than one if you are doing something
// like scatter/gather.
#define DPDK_QUEUE_ID_TX 0


// In DPDK, a "port" is a NIC. We will use the first NIC DPDK finds.
int g_dpdkPortId = -1;

// Use skeleton default settings
static const struct rte_eth_conf port_conf_default = {
	.rxmode = {
		.max_rx_pkt_len = RTE_ETHER_MAX_LEN,
	},
};


static void port_init(struct rte_mempool *mbuf_pool);
static uint16_t gen_checksum(const char *buf, int num_bytes);
static void do_send(struct rte_mempool *mbuf_pool, int num_to_send);


int main(int argc, char *argv[]) {
    // Initialize the Environment Abstraction Layer. All DPDK apps must do this.
    if (rte_eal_init(argc, argv) < 0) {
        rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");
    }

    // Creates a new mempool in memory to hold the mbufs.
    struct rte_mempool *mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS,
        MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (!mbuf_pool) {
        rte_exit(EXIT_FAILURE, "Couldn't create mbuf pool\n");
    }

    port_init(mbuf_pool);

    rte_eth_macaddr_get(g_dpdkPortId, (struct ether_addr *)g_src_mac_addr);
    printf("Our MAC: %02x %02x %02x %02x %02x %02x\n",
            g_src_mac_addr[0], g_src_mac_addr[1],
            g_src_mac_addr[2], g_src_mac_addr[3],
            g_src_mac_addr[4], g_src_mac_addr[5]);

    do_send(mbuf_pool, 10);

    return 0;
}




// 初始化端口
static void port_init(struct rte_mempool *mbuf_pool) {

    g_dpdkPortId = 0;
    while (g_dpdkPortId < RTE_MAX_ETHPORTS &&
	       rte_eth_devices[g_dpdkPortId].data->owner.id != RTE_ETH_DEV_NO_OWNER) {
		g_dpdkPortId++;
    }
    if (g_dpdkPortId == RTE_MAX_ETHPORTS) {
        rte_exit(EXIT_FAILURE, "There were no DPDK ports free.\n");
    }
 
    // Configure the Ethernet device.
    const int num_rx_queues = 0;
    const int num_tx_queues = 1;
    struct rte_eth_conf port_conf = port_conf_default;
    if (rte_eth_dev_configure(g_dpdkPortId, num_rx_queues, num_tx_queues, &port_conf)) {
        rte_exit(EXIT_FAILURE, "rte_eth_dev_configure() failed.\n");
    }

    // Set up TX queue.
    if (rte_eth_tx_queue_setup(g_dpdkPortId, DPDK_QUEUE_ID_TX, TX_RING_SIZE,
            rte_eth_dev_socket_id(g_dpdkPortId), NULL) < 0) {
        rte_exit(EXIT_FAILURE, "Couldn't setup TX queue.\n");
    }

    // Start the Ethernet port.
    if (rte_eth_dev_start(g_dpdkPortId) < 0) {
        rte_exit(EXIT_FAILURE, "Device start failed.\n");
    }
}


static uint16_t gen_checksum(const char *buf, int num_bytes) {
    const uint16_t *half_words = (const uint16_t *)buf;
    unsigned sum = 0;
    for (int i = 0; i < num_bytes / 2; i++)
        sum += half_words[i];

    if (num_bytes & 1)
        sum += buf[num_bytes - 1];

    sum = (sum & 0xffff) + (sum >> 16);
    sum += (sum & 0xff0000) >> 16;
    sum = ~sum & 0xffff;

    return sum;
}



// Packet: Eth  |  IP  |  UDP  |  <payload>
static void create_eth_ip_udp(uint8_t *msg, size_t total_len, uint8_t dst_mac[ETH_ALEN],
    uint32_t src_ip, uint32_t dst_ip, uint16_t udp_src_port, uint16_t udp_dst_port) {

    struct ethhdr *eth = (struct ethhdr *)msg;
    memcpy(eth->h_dest, dst_mac, ETH_ALEN);
    memcpy(eth->h_source, g_src_mac_addr, ETH_ALEN);
    eth->h_proto = htons(ETH_P_IP);

    struct iphdr *ip = (struct iphdr *)(eth + 1);
    size_t ip_len = total_len - sizeof(struct ethhdr);
    ip->ihl = 5;
    ip->version = 4;
    ip->tos = 0;
    ip->tot_len = htons((uint16_t)ip_len);
    ip->id = 0;
    ip->frag_off = 0;
    ip->ttl = 64;
    ip->protocol = IPPROTO_UDP;
    ip->check = 0;
    ip->saddr = src_ip;
    ip->daddr = dst_ip;
    ip->check = gen_checksum((char *)ip, sizeof(struct iphdr));

    struct udphdr *udp = (struct udphdr *)(ip + 1);
    size_t udp_len = ip_len - sizeof(struct iphdr);
    udp->source = htons(udp_src_port);
    udp->dest = htons(udp_dst_port);
    udp->len = htons((uint16_t)udp_len);

    // Set the UDP checksum to zero for simplicity. This is perfectly legal. It
    // just tells the the receiver not to check the checksum.
    udp->check = 0;

    // Use the packet count as the payload.
    char *dataStr = "Hi zzq,packetID";
    char *payload = (char *)(udp + 1);
    memcpy(payload ,dataStr, 15);
    static char packID = '0'; 
    memcpy(payload + 15 , &packID, 1);
    packID++;
}


// Send the specified number of packets, using as many bursts as necessary.
static void do_send(struct rte_mempool *mbuf_pool, int num_to_send) {
    // The smallest packet allowed by Ethernet.
    const unsigned eth_total_len = 64;

    struct rte_mbuf *mbufs[BURST_SIZE];
    for (int i = 0; i < BURST_SIZE; i++) {
        mbufs[i] = rte_pktmbuf_alloc(mbuf_pool);
        if (!mbufs[i]) {
            rte_exit(EXIT_FAILURE, "Cannot alloc mbuf\n");
        }

        mbufs[i]->pkt_len = eth_total_len;
        mbufs[i]->data_len = eth_total_len;
    }

    for (int num_packets_left = num_to_send; num_packets_left > 0;) {
        int num_to_send_this_burst = BURST_SIZE;
        if (num_packets_left < BURST_SIZE) {
            num_to_send_this_burst = num_packets_left;
        }

        for (int i = 0; i < num_to_send_this_burst; i++) {
            uint8_t *packet_data = rte_pktmbuf_mtod(mbufs[i], uint8_t *);
            const int UDP_PORT = 1234;
            create_eth_ip_udp(packet_data, eth_total_len, g_dest_mac_addr,
                g_src_ip, g_dest_ip, UDP_PORT, UDP_PORT);
        }

        // Send as many packets as will fit in the TX ring.
        int num_sent = rte_eth_tx_burst(g_dpdkPortId, DPDK_QUEUE_ID_TX, mbufs, num_to_send_this_burst);

        printf("Sent %i packets\n", num_sent);
        num_packets_left -= num_sent;
    }
}


