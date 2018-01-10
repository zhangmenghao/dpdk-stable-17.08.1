/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2015 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdint.h>
#include <inttypes.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_timer.h>

#include <rte_byteorder.h>
#include <rte_ip.h>
#include <rte_ether.h>
#include <rte_tcp.h>
#include <rte_arp.h>

#define RX_RING_SIZE 128
#define TX_RING_SIZE 512

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32

#ifndef IPv4_BYTES
#define IPv4_BYTES_FMT "%" PRIu8 ".%" PRIu8 ".%" PRIu8 ".%" PRIu8
#define IPv4_BYTES(addr) \
		(uint8_t) (((addr) >> 24) & 0xFF),\
		(uint8_t) (((addr) >> 16) & 0xFF),\
		(uint8_t) (((addr) >> 8) & 0xFF),\
		(uint8_t) ((addr) & 0xFF)
#endif

static const struct rte_eth_conf port_conf_default = {
	.rxmode = { .max_rx_pkt_len = ETHER_MAX_LEN }
};

struct ether_addr interface_MAC = {
    .addr_bytes[0] = 0x48,
    .addr_bytes[1] = 0x6E,
    .addr_bytes[2] = 0x73,
    .addr_bytes[3] = 0x00,
    .addr_bytes[4] = 0x04,
    .addr_bytes[5] = 0xDB,
};

unsigned long long rx_byte = 0;
unsigned long long tx_byte = 0;
unsigned long long last_rx_byte = 0;
unsigned long long last_tx_byte = 0;

unsigned long long rx_pkts = 0;
unsigned long long tx_pkts = 0;
unsigned long long last_rx_pkts = 0;
unsigned long long last_tx_pkts = 0;

/* basicfwd.c: Basic DPDK skeleton forwarding example. */

/*
 * Initializes a given port using global settings and with the RX buffers
 * coming from the mbuf_pool passed as a parameter.
 */
static inline int
port_init(uint8_t port, struct rte_mempool *mbuf_pool)
{
	struct rte_eth_conf port_conf = port_conf_default;
	const uint16_t rx_rings = 1, tx_rings = 1;
	uint16_t nb_rxd = RX_RING_SIZE;
	uint16_t nb_txd = TX_RING_SIZE;//64k
	int retval;
	uint16_t q;

	if (port >= rte_eth_dev_count())
		return -1;

	/* Configure the Ethernet device. */
	retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
	if (retval != 0)
		return retval;

	retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
	if (retval != 0)
		return retval;

	/* Allocate and set up 1 RX queue per Ethernet port. */
	for (q = 0; q < rx_rings; q++) {
		retval = rte_eth_rx_queue_setup(port, q, nb_rxd,
				rte_eth_dev_socket_id(port), NULL, mbuf_pool);
		if (retval < 0)
			return retval;
	}

	/* Allocate and set up 1 TX queue per Ethernet port. */
	for (q = 0; q < tx_rings; q++) {
		retval = rte_eth_tx_queue_setup(port, q, nb_txd,
				rte_eth_dev_socket_id(port), NULL);
		if (retval < 0)
			return retval;
	}

	/* Start the Ethernet port. */
	retval = rte_eth_dev_start(port);
	if (retval < 0)
		return retval;

	/* Display the port MAC address. */
	struct ether_addr addr;
	rte_eth_macaddr_get(port, &addr);
	printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
			   " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
			(unsigned)port,
			addr.addr_bytes[0], addr.addr_bytes[1],
			addr.addr_bytes[2], addr.addr_bytes[3],
			addr.addr_bytes[4], addr.addr_bytes[5]);

	/* Enable RX in promiscuous mode for the Ethernet device. */
	rte_eth_promiscuous_enable(port);

	return 0;
}

static void
print_ethaddr(const char *name, struct ether_addr *eth_addr)
{
	char buf[ETHER_ADDR_FMT_SIZE];
	ether_format_addr(buf, ETHER_ADDR_FMT_SIZE, eth_addr);
	printf("%s is %s\n", name, buf);
}

/*
 * The lcore main. This is the main thread that does the work, reading from
 * an input port and writing to an output port.
 */
static __attribute__((noreturn)) void
lcore_main(void)
{
	const uint8_t nb_ports = rte_eth_dev_count();
	uint8_t port;
	int i;

	/*
	 * Check that the port is on the same NUMA node as the polling thread
	 * for best performance.
	 */
	for (port = 0; port < nb_ports; port++)
		if (rte_eth_dev_socket_id(port) > 0 &&
				rte_eth_dev_socket_id(port) !=
						(int)rte_socket_id())
			printf("WARNING, port %u is on remote NUMA node to "
					"polling thread.\n\tPerformance will "
					"not be optimal.\n", port);

	printf("\nCore %u forwarding packets. [Ctrl+C to quit]\n",
			rte_lcore_id());

	/* Run until the application is quit or killed. */
	for (;;) {
		/*
		 * Receive packets on a port and forward them on the paired
		 * port. The mapping is 0 -> 1, 1 -> 0, 2 -> 3, 3 -> 2, etc.
		 */
		for (port = 0; port < nb_ports; port++) {

			/* Get burst of RX packets, from first port of pair. */
			struct rte_mbuf *bufs[BURST_SIZE];
			const uint16_t nb_rx = rte_eth_rx_burst(port, 0,
					bufs, BURST_SIZE);

			if (unlikely(nb_rx == 0))
				continue;
			if (port == 1){
				for (i = 0; i < nb_rx; i ++){
					struct ether_hdr *eth_hdr = rte_pktmbuf_mtod(bufs[i], struct ether_hdr *);
					if(eth_hdr->ether_type == rte_cpu_to_be_16(ETHER_TYPE_IPv4)){
						printf("packet comes from %u\n", port);
						rx_pkts ++;
						rx_byte += bufs[i]->data_len;
						printf("packet size is %u\n", bufs[i]->data_len);
					}
	 				else if (eth_hdr->ether_type == rte_be_to_cpu_16(ETHER_TYPE_ARP)) {
	  				    /* arp message to keep live with switch */
	  				    printf("processing arp request\n");
	   				    struct arp_hdr* arp_h;
	   				    struct ether_addr self_eth_addr;
	   				    uint32_t ip_addr;
	   				    
	  				    arp_h = (struct arp_hdr*)
	  				    		((u_char*)eth_hdr + sizeof(struct ether_hdr));
	  				    rte_eth_macaddr_get(port, &self_eth_addr);
	  				    printf("Network Card Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
						   " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
						1,
						self_eth_addr.addr_bytes[0], self_eth_addr.addr_bytes[1],
						self_eth_addr.addr_bytes[2], self_eth_addr.addr_bytes[3],
						self_eth_addr.addr_bytes[4], self_eth_addr.addr_bytes[5]);

	  				    ether_addr_copy(&(eth_hdr->s_addr), &(eth_hdr->d_addr));
	  				    printf("Switch %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
						   " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
						1,
						(eth_hdr->d_addr).addr_bytes[0], (eth_hdr->d_addr).addr_bytes[1],
						(eth_hdr->d_addr).addr_bytes[2], (eth_hdr->d_addr).addr_bytes[3],
						(eth_hdr->d_addr).addr_bytes[4], (eth_hdr->d_addr).addr_bytes[5]);
	  				    //ether_addr_copy(&(eth_hdr->s_addr), &interface_MAC);
	  				    /* Set source MAC address with MAC of TX Port */
	  				    ether_addr_copy(&self_eth_addr, &(eth_hdr->s_addr));
	  				    arp_h->arp_op = rte_cpu_to_be_16(ARP_OP_REPLY);
	  				    ether_addr_copy(&(arp_h->arp_data.arp_sha)
	   				    			, &(arp_h->arp_data.arp_tha));
	  				    ether_addr_copy(&(eth_hdr->s_addr)
	   				    			, &(arp_h->arp_data.arp_sha));
	  				    /* Swap IP address in ARP payload */
	  				    ip_addr = arp_h->arp_data.arp_sip;
	  				    arp_h->arp_data.arp_sip = arp_h->arp_data.arp_tip;
	  				    arp_h->arp_data.arp_tip = ip_addr;

	  				    uint32_t ipv4_addr = arp_h->arp_data.arp_sip;
	                    printf("%d.%d.%d.%d\n", (ipv4_addr >> 24) & 0xFF,
	                                (ipv4_addr >> 16) & 0xFF, (ipv4_addr >> 8) & 0xFF,
	                                ipv4_addr & 0xFF);

						ipv4_addr = ip_addr;
	                    printf("%d.%d.%d.%d\n", (ipv4_addr >> 24) & 0xFF,
	                                (ipv4_addr >> 16) & 0xFF, (ipv4_addr >> 8) & 0xFF,
	                                ipv4_addr & 0xFF);
	  				    rte_eth_tx_burst(port, 0, &bufs[i], 1);
	  					 printf("processing arp request end!\n");
					    
	  				}
	  				else{
	  					rte_pktmbuf_free(bufs[i]);
	  				}
	  				printf("\n");
	  			}

			}
			else{
				
				for (i = 0; i < nb_rx; i ++){
					printf("packet comes from %u\n", port);
					struct ether_hdr *eth_hdr;
					eth_hdr = rte_pktmbuf_mtod(bufs[i], struct ether_hdr *);
					ether_addr_copy(&interface_MAC, &eth_hdr->d_addr);
					struct ether_addr addr;
					rte_eth_macaddr_get(port^1, &addr);
					ether_addr_copy(&addr, &eth_hdr->s_addr);
					print_ethaddr("eth_s_addr", &(eth_hdr->s_addr));
					print_ethaddr("eth_d_addr", &(eth_hdr->d_addr));

					struct ipv4_hdr *ip_hdr = (struct ipv4_hdr*)((char*)eth_hdr + sizeof(struct ether_hdr));
					ip_hdr->dst_addr = rte_cpu_to_be_32(IPv4(172,17,17,2));
					ip_hdr->hdr_checksum = 0;
					uint16_t ck1 = rte_ipv4_cksum(ip_hdr);
    				ip_hdr->hdr_checksum = ck1;
					printf("ip_src is "IPv4_BYTES_FMT " \n", IPv4_BYTES(rte_be_to_cpu_32(ip_hdr->src_addr)));
					printf("ip_dst is "IPv4_BYTES_FMT " \n", IPv4_BYTES(rte_be_to_cpu_32(ip_hdr->dst_addr)));
					printf("nf: next_proto_id is %u\n", ip_hdr->next_proto_id);
					if (ip_hdr->next_proto_id == 6){
						struct tcp_hdr * tcp_hdrs = (struct tcp_hdr*)((char*)ip_hdr + sizeof(struct ipv4_hdr));
						printf("nf: this is very important! port_src and port_dst is %u and %u\n", 
							rte_be_to_cpu_16(tcp_hdrs->src_port), rte_be_to_cpu_16(tcp_hdrs->dst_port));
						printf("nf: tcp_flags is %u\n", tcp_hdrs->tcp_flags);
					}
					tx_pkts ++;
					tx_byte += bufs[i]->data_len;
					printf("packet size is %u\n", bufs[i]->data_len);
				}
				/* Send burst of TX packets, to second port of pair. */
				const uint16_t nb_tx = rte_eth_tx_burst(port ^ 1, 0,
						bufs, nb_rx);
				printf("send packet to port %u\n\n", port^1);

				/* Free any unsent packets. */
				if (unlikely(nb_tx < nb_rx)) {
					uint16_t buf;
					for (buf = nb_tx; buf < nb_rx; buf++)
						rte_pktmbuf_free(bufs[buf]);
				}
			}			
		}
	}
}

/*
 * The main function, which does initialization and calls the per-lcore
 * functions.
 */
int
main(int argc, char *argv[])
{
	struct rte_mempool *mbuf_pool;
	unsigned nb_ports;
	uint8_t portid;

	/* Initialize the Environment Abstraction Layer (EAL). */
	int ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

	argc -= ret;
	argv += ret;

	/* Check that there is an even number of ports to send/receive on. */
	nb_ports = rte_eth_dev_count();
	if (nb_ports < 2 || (nb_ports & 1))
		rte_exit(EXIT_FAILURE, "Error: number of ports must be even\n");

	/* Creates a new mempool in memory to hold the mbufs. */
	mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * nb_ports,
		MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

	if (mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

	/* Initialize all ports. */
	for (portid = 0; portid < nb_ports; portid++)
		if (port_init(portid, mbuf_pool) != 0)
			rte_exit(EXIT_FAILURE, "Cannot init port %"PRIu8 "\n",
					portid);

	if (rte_lcore_count() > 1)
		printf("\nWARNING: Too many lcores enabled. Only 1 used.\n");

	/* Call lcore_main on the master core only. */
	lcore_main();

	return 0;
}