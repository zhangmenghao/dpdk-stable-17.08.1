#include <stdint.h>
#include <inttypes.h>
#include <getopt.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h> 
#include <rte_mbuf.h> 
#include <rte_byteorder.h>
#include <rte_ip.h>
#include <rte_ether.h>
#include <rte_common.h>
#include <rte_arp.h>
#include <rte_tcp.h>
#include <rte_udp.h>
#include <rte_hash.h>
#include "main.h"
struct states_5tuple_pair {
    struct ipv4_5tuple l4_5tuple;
    struct nf_states states;
} __rte_cache_aligned;
static struct rte_mbuf*
build_backup_packet(uint8_t port, uint32_t backup_machine_ip, 
 					struct ipv4_5tuple* ip_5tuple, struct nf_states* states)
{
    struct rte_mbuf* backup_packet;
    struct ether_hdr* eth_h;
    struct ipv4_hdr* ip_h;
    struct states_5tuple_pair* payload;
    struct ether_addr self_eth_addr;
    /* Allocate space */
    backup_packet = rte_pktmbuf_alloc(single_port_param.manager_mempool);
    eth_h = (struct ether_hdr *)
 			rte_pktmbuf_append(backup_packet, sizeof(struct ether_hdr));
    ip_h = (struct ipv4_hdr *)
  			rte_pktmbuf_append(backup_packet, sizeof(struct ipv4_hdr));
    payload = (struct states_5tuple_pair*)
   		rte_pktmbuf_append(backup_packet, sizeof(struct states_5tuple_pair));
    /* Set the packet ether header */
    eth_h->ether_type =  rte_cpu_to_be_16(ETHER_TYPE_IPv4);
    ether_addr_copy(&interface_MAC, &(eth_h->d_addr));
    rte_eth_macaddr_get(port, &self_eth_addr);
    ether_addr_copy(&self_eth_addr, &(eth_h->s_addr));
    /* Set the packet ip header */
    memset((char *)ip_h, 0, sizeof(struct ipv4_hdr));
    ip_h->src_addr=rte_cpu_to_be_32(this_machine->ip);
    ip_h->dst_addr=rte_cpu_to_be_32(backup_machine_ip);
    ip_h->version_ihl = (4 << 4) | 5;
    ip_h->total_length = rte_cpu_to_be_16(20+sizeof(struct states_5tuple_pair));
    ip_h->packet_id= 0x36;/* NO USE */
    ip_h->time_to_live=4;
    ip_h->next_proto_id = 0x0;
    ip_h->hdr_checksum = rte_ipv4_cksum(ip_h);
    /* Set the packet payload(5tuple:states) */
    payload->l4_5tuple.ip_dst = ip_5tuple->ip_dst;
    payload->l4_5tuple.ip_src = ip_5tuple->ip_src;
    payload->l4_5tuple.port_dst = ip_5tuple->port_dst;
    payload->l4_5tuple.port_src = ip_5tuple->port_src;
    payload->l4_5tuple.proto = ip_5tuple->proto;
    payload->states.ipserver = states->ipserver;
    payload->states.dip = states->dip;
    payload->states.dport = states->dport;
    payload->states.bip = states->bip;
    return backup_packet;
}
/*
 * gateway manager.
 */
int
lcore_manager(__attribute__((unused)) void *arg)
{
	const uint8_t nb_ports = rte_eth_dev_count();
	uint8_t port;
	int i;
    struct ether_hdr* eth_h;
    struct ipv4_hdr* ip_h;
	// uint16_t eth_type;
	uint8_t ip_proto;
    u_char* payload;
    struct ipv4_5tuple* ip_5tuple;
	printf("\nCore %u manage states in gateway.\n",
			rte_lcore_id());
	/* Run until the application is quit or killed. */
	for (;;) {
		for (port = 0; port < nb_ports; port++) {
			if ((enabled_port_mask & (1 << port)) == 0) {
				//printf("Skipping %u\n", port);
				continue;
			}
			struct rte_mbuf *bufs[BURST_SIZE];
			const uint16_t nb_rx = rte_eth_rx_burst(port, 1,
					bufs, BURST_SIZE);
			if (unlikely(nb_rx == 0))
				continue;
			for (i = 0; i < nb_rx; i ++){
				printf("packet comes from port %u queue 1\n", port);
				eth_h = rte_pktmbuf_mtod(bufs[i], struct ether_hdr *);
				ip_h = (struct ipv4_hdr*)
  				  		((u_char*)eth_h + sizeof(struct ether_hdr));
				ip_proto = ip_h->next_proto_id;
				printf("receive ip "IPv4_BYTES_FMT " \n", IPv4_BYTES(ip_h->dst_addr));
				printf("proto: %x\n",ip_proto);
 				if (ip_proto == 0x06 || ip_proto == 0x11) {
  				    /* Control message about ECMP */
  				    if ((ip_h->dst_addr & 0x00FF0000) == (0xFD << 16)) {
  				        /* Destination ip is 172.16.253.X */
  				        /* This is ECMP predict request message */
 				        backup_receive_probe_packet(bufs[i]);
  				        rte_eth_tx_burst(port, 0, &probing_packet, 1);
  				        printf("This is ECMP predict request message\n");
   				    }
  				    // else if ((ip_h->dst_addr & 0x00FF0000) == 0) {
  				    else {
  				        /* Destination ip is 172.16.X.Y */
  				        /* This is ECMP predict reply message */
  				        // ecmp_receive_reply(bufs[i]);
   				        struct rte_mbuf* backup_packet;
   				        struct nf_states* states;
   				        struct ipv4_5tuple tmp_tuple;
   				        uint32_t backup_ip;
   				        master_receive_probe_reply(
   				            bufs[i], &backup_ip,
    			 	        &tmp_tuple.ip_src, &tmp_tuple.ip_dst,
    			 	        &tmp_tuple.port_src, &tmp_tuple.port_dst
   				        );
   				        tmp_tuple.proto = 0x6;
   				        ip_5tuple = &tmp_tuple;
   				        getStates(ip_5tuple, &states);
   				        backup_packet = build_backup_packet(
    			            port, backup_ip, ip_5tuple, states
    			 	    );
   				        rte_eth_tx_burst(port, 0, &backup_packet, 1);
   				        printf("This is ECMP pedict reply message\n");
   				    }
  				}
 				else if (ip_proto == 0) {
  				    /* Control message about state */
   				    if ((ip_h->dst_addr & 0xFF000000) == (255U << 24)) {
   				        /* Destination ip is 172.16.0.255 */
   				        /* This is flow broadcast message */
    			        printf("This is flow broadcast message\n");
   				    }
  				    else{
  				        /* Destination ip is 172.16.0.X */
  				        /* This is state backup message */
  				        printf("This is state backup message\n");
   				        payload = (u_char*)ip_h + ((ip_h->version_ihl)&0x0F)*4;
   				        struct states_5tuple_pair* backup_pair = (struct states_5tuple_pair*)payload;
				        printf("ip_src is "IPv4_BYTES_FMT " \n", IPv4_BYTES(backup_pair->l4_5tuple.ip_src));
				        printf("ip_dst is "IPv4_BYTES_FMT " \n", IPv4_BYTES(backup_pair->l4_5tuple.ip_dst));
				        printf("port_src is 0x%x\n", backup_pair->l4_5tuple.port_src);
				        printf("port_dst is 0x%x\n", backup_pair->l4_5tuple.port_dst);
				        printf("proto is 0x%x\n", backup_pair->l4_5tuple.proto);
				        printf("ip_server is "IPv4_BYTES_FMT " \n", IPv4_BYTES(backup_pair->states.ipserver));
				        printf("dip is "IPv4_BYTES_FMT " \n", IPv4_BYTES(backup_pair->states.dip));
				        printf("dport is 0x%x\n", backup_pair->states.dport);
				        printf("dip is "IPv4_BYTES_FMT " \n", IPv4_BYTES(backup_pair->states.bip));
   				        setStates(&(backup_pair->l4_5tuple), &(backup_pair->states));
   				    }
  				}
				printf("\n");
			}
			/* Free any unsent packets. */
			// if (unlikely(nb_tx < nb_rx)) {
				// uint16_t buf;
				// for (buf = nb_tx; buf < nb_rx; buf++)
					// rte_pktmbuf_free(bufs[buf]);
			// }
		}
	}
	return 0;
}
int
lcore_manager_slave(__attribute__((unused)) void *arg)
{
	const uint8_t nb_ports = rte_eth_dev_count();
	uint8_t port;
    struct ipv4_5tuple* ip_5tuple;
	printf("\nCore %u process request from nf\n",
			rte_lcore_id());
	for (;;) {
		for (port = 0; port < nb_ports; port++) {
			if ((enabled_port_mask & (1 << port)) == 0) {
				//printf("Skipping %u\n", port);
				continue;
			}
 			if (rte_ring_dequeue(nf_manager_ring, (void**)&ip_5tuple) == 0) {
   			    build_probe_packet(
    		 	    ip_5tuple->ip_dst, ip_5tuple->ip_src,
   			        ip_5tuple->port_dst, ip_5tuple->port_src
   			    );
  			    printf("Receive backup request from nf\n");
			    printf("ip_dst is "IPv4_BYTES_FMT " \n", IPv4_BYTES(ip_5tuple->ip_dst));
			    printf("ip_src is "IPv4_BYTES_FMT " \n", IPv4_BYTES(ip_5tuple->ip_src));
			    printf("port_src is 0x%x\n", ip_5tuple->port_src);
			    printf("port_dst is 0x%x\n", ip_5tuple->port_dst);
			    printf("proto is 0x%x\n", ip_5tuple->proto);
			    printf("\n");
			    rte_eth_tx_burst(port, 0, &probing_packet, 1);
  			}
		}
	}
	return 0;
}
