/* vim: ts=4 cc=80
 * TCP ping, used to test latency between two hosts by using TCP RST
 *
 * Created on:	2010/01/08
 * Author:	zyin (Zhuo Yin)
 */

#include <errno.h>
#include <features.h>
#include <float.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/tcp.h>

#include <netinet/ether.h>
#include <netinet/in.h>
#include <net/if.h>
#include <netdb.h>
#include <arpa/inet.h>

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>

/* default settings */
#define BUF_SIZE 512
#define PATH_PROC_NET_ARP "/proc/net/arp"
#define PATH_PROC_NET_ROUTE "/proc/net/route"

#define DEFAULT_CHAR_LENGTH 32
#define DEFAULT_WINDOWS_SIZE 5480
#define DEFAULT_MSS_SIZE 1460



/* type definitions */
typedef struct ethhdr eth_header_t;
typedef struct iphdr ip_header_t;
typedef struct tcphdr tcp_header_t;

typedef struct tcp_packet_s {
	unsigned char* packet;
	int packet_len;
} tcp_packet_t;

typedef struct tcp_opt_mss_s {
	uint8_t kind_mss;
	uint8_t len_mss;
	uint16_t mss;
	
	uint8_t kind_sack;
	uint8_t len_sack;
	
	uint8_t kind_ts;
	uint8_t len_ts;
	uint32_t tsval;
	uint32_t tsecr;

	uint8_t nop;
	
	uint8_t kind_wscale;
	uint8_t len_wscale;
	uint8_t wscale;
} tcp_opt_mss_t;

typedef struct tcp_mss_header_s {
	tcp_header_t tcp_header;
	tcp_opt_mss_t tcp_mss;
} tcp_mss_header_t;

typedef struct pseudo_header_s {
	unsigned long int source_ip;
	unsigned long int dest_ip;
	unsigned char reserved;
	unsigned char protocol;
	unsigned short int tcp_length;
} pseudo_header_t;


// global value
static tcp_packet_t g_tcp_packet;



inline static uint64_t get_mono_time() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int
fool_kernel_tcpping_socket_create(
	struct in_addr ip,
	int port)
{
	// fool the kernel, otherwise kernel will send RST
	int s = socket(AF_INET, SOCK_STREAM, 0); 
	if (s < 0) {
		perror("fool kernel failed create socket:");
		exit(-1);
	}

	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = ip.s_addr;
	addr.sin_port = htons(port);
	if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < -1){
		perror("fool kernel failed binding:");
		exit(-1);
	}
	if (listen(s, 5) < -1){
		perror("fool kernel failed listen:");
		exit(-1);
	};

	return s;
}

static int
create_raw_socket(int protocol)
{
	// Create real rawsock;
	int rawsock;
	if((rawsock = socket(PF_PACKET, SOCK_RAW, htons(protocol)))== -1){
		perror("Error creating raw socket: ");
		exit(-1);
	}

	return rawsock;
}

static int bind_sock_to_dev(char *device, int rawsock, int protocol)
{
	struct sockaddr_ll sll;
	struct ifreq ifr;

	bzero(&sll, sizeof(sll));
	bzero(&ifr, sizeof(ifr));
	
	// First Get the Interface Index  
	strncpy((char *)ifr.ifr_name, device, IFNAMSIZ);
	if((ioctl(rawsock, SIOCGIFINDEX, &ifr)) == -1){
		printf("Error: getting Interface index !\n");
		exit(-1);
	}

	// Bind our raw socket to this interface 
	sll.sll_family = AF_PACKET;
	sll.sll_ifindex = ifr.ifr_ifindex;
	sll.sll_protocol = htons(protocol); 

	if((bind(rawsock, (struct sockaddr *)&sll, sizeof(sll)))== -1){
		perror("Error binding raw socket to interface\n");
		exit(-1);
	}

	return 1;
}

inline static int send_raw_packet(int rawsock, unsigned char* pkt, int pkt_len)
{
	if(write(rawsock, pkt, pkt_len) != pkt_len)
		return 0;

	return 1;
}

static eth_header_t* create_eth_header(char *src_mac, char *dst_mac, int protocol)
{
	eth_header_t *ethernet_header;
	ethernet_header = (eth_header_t*)malloc(sizeof(eth_header_t));
	
	// copy the Src mac addr 
	memcpy(ethernet_header->h_source, (void *)ether_aton(src_mac), ETH_ALEN);

	// copy the Dst mac addr 
	memcpy(ethernet_header->h_dest, (void *)ether_aton(dst_mac), ETH_ALEN);

	// copy the protocol 
	ethernet_header->h_proto = htons(protocol);

	// return the header
	return ethernet_header;
}

// Ripped from Richard Stevans Book 
static unsigned short
ComputeChecksum(unsigned char *data, int len)
{
	long sum = 0;  // assume 32 bit long, 16 bit short 
	unsigned short *temp = (unsigned short *)data;

	while(len > 1){
		sum += *temp++;
		if(sum & 0x80000000)   // if high order bit set, fold 
		sum = (sum & 0xFFFF) + (sum >> 16);
		len -= 2;
	}

	// take care of leftover byte 
	if (len) sum += (unsigned short) *((unsigned char *)temp);

	while(sum>>16) sum = (sum & 0xFFFF) + (sum >> 16);
	return ~sum;
}


static ip_header_t* create_ip_header(char* src_ip, char* dst_ip, int data_size)
{
	ip_header_t *ip_header;

	ip_header = (ip_header_t *)malloc(sizeof(ip_header_t));

	ip_header->version = 4;
	ip_header->ihl = (sizeof(ip_header_t))/4 ;
	ip_header->tos = 0;
	ip_header->tot_len = htons(sizeof(ip_header_t) + sizeof(tcp_mss_header_t) + data_size);
	ip_header->id = htons(1);
	ip_header->frag_off = 0x40; // Don't fragmant 
	ip_header->ttl = 64;
	ip_header->protocol = IPPROTO_TCP;
	ip_header->check = 0; // We will calculate the checksum later 
	ip_header->saddr = inet_addr(src_ip);
	ip_header->daddr = inet_addr(dst_ip);

	// Calculate the IP checksum now : 
	// The IP Checksum is only over the IP header 
	ip_header->check = ComputeChecksum((unsigned char *)ip_header, ip_header->ihl*4);

	return ip_header;
}

static tcp_mss_header_t*
create_tcp_header(unsigned int src_port, unsigned int dst_port, int seq,
	int ack, int window, int mss)
{
	tcp_mss_header_t* tcp_hdr_mss;

	// Create tcp header with 
	tcp_hdr_mss = (tcp_mss_header_t*)malloc(sizeof(tcp_mss_header_t));
	
	tcp_hdr_mss->tcp_header.source = htons(src_port);
	tcp_hdr_mss->tcp_header.dest = htons(dst_port);
	tcp_hdr_mss->tcp_header.seq = htonl(seq);
	tcp_hdr_mss->tcp_header.ack_seq = htonl(ack);
	tcp_hdr_mss->tcp_header.res1 = 0;

	// Add 1 for the Tcp Option Mss 
	tcp_hdr_mss->tcp_header.doff = (sizeof(tcp_mss_header_t))/4; 
	tcp_hdr_mss->tcp_header.syn = 1;
	tcp_hdr_mss->tcp_header.window = htons(window);
	
	// Will calculate the checksum with pseudo-header later 
	tcp_hdr_mss->tcp_header.check = 0; 
	tcp_hdr_mss->tcp_header.urg_ptr = 0;

	// Maxium Segment Size 
	tcp_hdr_mss->tcp_mss.kind_mss = 0x02; 
	tcp_hdr_mss->tcp_mss.len_mss = 0x04; // 4 bytes(include kind and length) 
	tcp_hdr_mss->tcp_mss.mss = htons(mss); 
	
	// SACK Permitted 
	tcp_hdr_mss->tcp_mss.kind_sack = 0x04;
	tcp_hdr_mss->tcp_mss.len_sack = 0x02; // 2 bytes 
	
	// Time Stamp 
	tcp_hdr_mss->tcp_mss.kind_ts = 0x08;
	tcp_hdr_mss->tcp_mss.len_ts = 0x0a; // 10 bytes 
	tcp_hdr_mss->tcp_mss.tsval = 0x047ccff8;
	tcp_hdr_mss->tcp_mss.tsecr = 0x00;

	// NOP 
	tcp_hdr_mss->tcp_mss.nop = 0x01;

	// Window Scale 
	tcp_hdr_mss->tcp_mss.kind_wscale = 0x03;
	tcp_hdr_mss->tcp_mss.len_wscale = 0x03; // 3 bytes 
	tcp_hdr_mss->tcp_mss.wscale = 0x05;

	return tcp_hdr_mss;
}

static void create_pseudo_header(ip_header_t *ip_header,
	tcp_mss_header_t* tcp_hdr_mss, unsigned char *data, int data_size)
{
	// The TCP Checksum is calculated over the pseudo_header_t + TCP header
	// with option mss + Data 

	// Find the size of the TCP Header + Data 
	int segment_len = ntohs(ip_header->tot_len) - ip_header->ihl*4; 

	// Total length over which TCP checksum will be computed 
	int header_len = sizeof(pseudo_header_t) + segment_len;

	// Allocate the memory 
	unsigned char *hdr = (unsigned char *)malloc(header_len);

	// Fill in the pseudo header first 
	
	pseudo_header_t* pseudo_header = (pseudo_header_t*)hdr;

	pseudo_header->source_ip = ip_header->saddr;
	pseudo_header->dest_ip = ip_header->daddr;
	pseudo_header->reserved = 0;
	pseudo_header->protocol = ip_header->protocol;
	pseudo_header->tcp_length = htons(segment_len);

	// Now copy TCP 
	memcpy((hdr + sizeof(pseudo_header_t)), (void *)tcp_hdr_mss, tcp_hdr_mss->tcp_header.doff*4);

	// Now copy the Data 
	memcpy((hdr + sizeof(pseudo_header_t) + tcp_hdr_mss->tcp_header.doff*4), data, data_size);

	// Calculate the Checksum 
	tcp_hdr_mss->tcp_header.check = ComputeChecksum(hdr, header_len);

	// Free the pseudo_header_t 
	free(hdr);
	return;
}

static int create_tcp_packet(char* src_mac, char* dst_mac, char* src_ip,
	char* dst_ip, unsigned int src_port, unsigned int dst_port, int seq,
	int ack, int window, int mss, unsigned char* data,
	tcp_packet_t* packer)
{
	unsigned char *packet;
	eth_header_t* ethernet_header;
	ip_header_t *ip_header;
	tcp_mss_header_t* tcp_header_mss;
	int packet_len;
	unsigned char* offset;

	int data_size = strlen((char*)data);

	// create Ethernet header 
	ethernet_header = create_eth_header(src_mac, dst_mac, ETHERTYPE_IP);

	// Create IP Header 
	ip_header = create_ip_header(src_ip, dst_ip, data_size);

	// Create TCP Header 
	tcp_header_mss = create_tcp_header(src_port, dst_port, seq, ack, window, mss);

	// Create pseudo_header_t and compute TCP Checksum  
	create_pseudo_header(ip_header, tcp_header_mss, data, data_size);

	// Packet length = ETH + IP header + TCP header + Data
	packet_len = sizeof(eth_header_t) + ntohs(ip_header->tot_len);

	// Allocate memory and reset to zero
	packet = (unsigned char *)malloc(packet_len);
	memset(packet, 0, packet_len);
	offset = packet;

	// Copy the Ethernet header first 
	memcpy(offset, ethernet_header, sizeof(eth_header_t));

	// Copy the IP header -- but after the ethernet header 
	offset += sizeof(eth_header_t);
	memcpy(offset, ip_header, ip_header->ihl*4);

	// Copy the TCP header after the IP header 
	offset += ip_header->ihl*4;
	memcpy(offset, tcp_header_mss, tcp_header_mss->tcp_header.doff*4);
	
	// Copy the Data after the TCP header 
	offset += tcp_header_mss->tcp_header.doff*4; 
	memcpy(offset, data, data_size);

	// Free the headers back to the heavenly heap 
	free(ethernet_header);
	free(ip_header);
	free(tcp_header_mss);

	packer->packet = packet;
	packer->packet_len = packet_len;

	return 0;
}

inline static int parse_packet(unsigned char *packet, int len, uint32_t* seq,
	uint32_t* ack)
{
	static eth_header_t* pEthHeader;
	static ip_header_t* pip_header_t;
	static tcp_header_t* ptcp_header_t;
	unsigned char* offset;

	// Check if enough bytes are there for TCP Header 
	size_t all_header_len = 0;
	all_header_len += sizeof(eth_header_t);
	all_header_len += sizeof(ip_header_t);
	all_header_len += sizeof(tcp_header_t);
	if (len < all_header_len) {
		printf("Error: Not a valid TCP packet\n");
		return -1;
	}

	/* check if the ETH header contains IP packet */
	offset = packet;
	pEthHeader = (eth_header_t *)offset;
	if(ntohs(pEthHeader->h_proto) != ETH_P_IP)
		return -1;

	/*
	// First set of 6 bytes are Destination MAC 
	printf("Destination MAC: ");
	PrintInHex(pEthHeader->h_dest, 6);
	printf("\n");
		
	// Second set of 6 bytes are Source MAC 
	printf("Source MAC: ");
	PrintInHex(pEthHeader->h_source, 6);
	printf("\n");

	// Last 2 bytes in the Ethernet header are the protocol it carries 
	printf("Protocol: ");
	PrintInHex((void *)&pEthHeader->h_proto, 2);
	printf("\n");
	*/

	/* check if the IP header contains TCP header */
	offset += sizeof(eth_header_t);
	pip_header_t = (ip_header_t*)offset;
	if(pip_header_t->protocol != IPPROTO_TCP)
		return -1;

	// calculate the tcp header offset
	offset += pip_header_t->ihl*4;
	ptcp_header_t = (tcp_header_t*)offset;
	*seq = ntohl(ptcp_header_t->seq);
	*ack = ntohl(ptcp_header_t->ack_seq);

	/*
	// Print the Dest and Src ports 
	printf("Source Port: %d\n", ntohs(ptcp_header_t->source));
	printf("Dest Port: %d\n", ntohs(ptcp_header_t->dest));


	// data and data length
	offset += sizeof(tcp_header_t); 
	int data_len =
		ntohs(pip_header_t->tot_len) - pip_header_t->ihl*4 - sizeof(tcp_header_t);
	*/

	return 0;
}

int get_gateway(struct in_addr target_addr, struct in_addr* gateway_addr,
	struct in_addr* mask_addr, char* interface) {
	char mask[DEFAULT_CHAR_LENGTH];
	char dest[DEFAULT_CHAR_LENGTH];
	char gateway[DEFAULT_CHAR_LENGTH];
	unsigned int flags;
	unsigned int refcnt;
	unsigned int use;
	unsigned int metric;
	unsigned int mtu;
	unsigned int window;
	unsigned int irtt;

	uint32_t target_in;
	uint32_t dest_in;
	uint32_t gateway_in;
	uint32_t mask_in;

	uint32_t dest_net;
	uint32_t target_net;
	int num;

	target_in = target_addr.s_addr;
	
	// check if file can be opened
	FILE *fp;
	if ((fp = fopen(PATH_PROC_NET_ROUTE, "r")) == NULL) {
		perror(PATH_PROC_NET_ROUTE);
		return -1;
	}
	
	char line[512];

	// Ignore the first line
	char* result = fgets(line, sizeof(line), fp);
	if (result == NULL) {
		printf("Error: Cannot read %s", PATH_PROC_NET_ROUTE);
		return -1;
	}

	// find the gateway
	for (;;) {
		char* result = fgets(line, sizeof(line), fp);
		if (result == NULL)
			break;

		num = sscanf(line, "%s %8s %8s %4x %d %d %d %8s %1d %1d %1d\n",
			interface, dest, gateway, &flags, &refcnt, &use, &metric,
			mask, &mtu, &window, &irtt);
		if (num < 11)
			continue;

		sscanf(dest, "%8x", &dest_in);
		sscanf(gateway, "%8x", &gateway_in);
		sscanf(mask, "%8x", &mask_in);

		dest_net = (dest_in & mask_in);
		target_net = (target_in & mask_in);
		if (dest_net == target_net)
			break;
	}

	gateway_addr->s_addr = gateway_in;
	mask_addr->s_addr = mask_in;

	fclose(fp);
	return 0;
}

static int get_gateway_mac(const struct in_addr gateway_addr,
	char* gateway_mac) {
	
	FILE *fp;
	char line[BUF_SIZE];
	char ip[100];
	char mac[100];
	char mask[100];
	char dev[100];
	int type, flags;
	int num;

	struct in_addr ip_addr;

	/* Open the PROCps kernel table. */
	if ((fp = fopen(PATH_PROC_NET_ARP, "r")) == NULL) {
		perror(PATH_PROC_NET_ARP);
		return -1;
	}

	// Ignore the first line
	char* result = fgets(line, sizeof(line), fp);
	if (result == NULL) {
		printf("Error: Cannot read %s\n", PATH_PROC_NET_ARP);
		return -1;
	}

	// start reading arp table
	int found_mac = -1;
	for (;;) {
		char* result = fgets(line, sizeof(line), fp);
		if (result == NULL)
			break;

		num = sscanf(line, "%s 0x%x 0x%x %s %s %s\n",
			ip, &type, &flags, mac, mask, dev);
		if (num < 6)
			continue;

		inet_aton(ip, &ip_addr);

		if (gateway_addr.s_addr == ip_addr.s_addr) {
			found_mac = 0;
			strcpy(gateway_mac, mac);
			break;
		}
	}

	fclose(fp);
	return found_mac;
}

static int get_local_ip_mac(struct ifreq ifr, char* addr, char* mask,
	char* mac) {

	int sock, j, k;
	char* p;
	struct in_addr ip;

	sock=socket(PF_INET, SOCK_STREAM, 0);
	if (-1 == sock) {
		perror("socket() ");
		return -1;
	}

	if (-1 == ioctl(sock, SIOCGIFADDR, &ifr)) {
		perror("ioctl(SIOCGIFADDR) ");
		return -1;
	} 
	ip = ((struct sockaddr_in *)(&ifr.ifr_addr))->sin_addr;
	p = inet_ntoa(ip);

	strncpy(addr,p,31);
	addr[31]='\0';


	if (-1 == ioctl(sock, SIOCGIFNETMASK, &ifr)) {
		perror("ioctl(SIOCGIFNETMASK) ");
		return -1;
	}   

	ip = ((struct sockaddr_in *)(&ifr.ifr_netmask))->sin_addr;
	p=inet_ntoa(ip);

	strncpy(mask,p,31);
	mask[31]='\0';


	if (-1 == ioctl(sock, SIOCGIFHWADDR, &ifr)) {
		perror("ioctl(SIOCGIFHWADDR) ");
		return -1;
	}   
	for (j=0, k=0; j<6; j++) {
		k += snprintf(mac+k, 31-k, j ? ":%02X" : "%02X",
			(int)(unsigned int)(unsigned char)ifr.ifr_hwaddr.sa_data[j]);
	}   
	mac[31]='\0';

	close(sock);
	return 0;
}

static void collect_packets(int sock, int timeout) {
	fd_set mask;
	FD_ZERO(&mask);
	FD_SET(sock, &mask);
	struct sockaddr* packet_info = NULL;
	socklen_t* packet_info_size = NULL;;

	int ret;
	struct timeval to;
	to.tv_sec = timeout;
	to.tv_usec = 0;
	char buf[BUF_SIZE];

	while (1) {
		ret = select(sock + 1, &mask, NULL, NULL, &to);
		if (ret == 0)
			break;

		recvfrom(sock, buf, BUF_SIZE, 0, packet_info, packet_info_size);
	}
}

/*
 * get destination ip address
 * host:	input hostname string or ip string
 * ip:		output ip address
 * return:	0,sucessed; -1,failed;
 */
int get_dst_ip(const char *host, char *ip)
{
	struct hostent *hp;
#if 0
	// host is hostname or ip address ?
	if (INADDR_NONE != inet_addr(host)) {	// is ip address
		strcpy(ip, host);
		return 0;
	}
#endif
	
	hp = gethostbyname(host);
	if (NULL == hp) {
		printf("unknown host %s\n", host);
		return -1;
	}
	inet_ntop(hp->h_addrtype, hp->h_addr, ip, 16);
	
	return 0;
}


/*
 * create socket
 * if_name:		interface name, eg:eth0/wlan0
 * return:		socket fd
 */
int tcpping_socket_create(char* if_name)
{
	int sock;

	// Create the raw socket 
	sock = create_raw_socket(ETH_P_ALL);

	// Bind raw socket to interface 
	bind_sock_to_dev(if_name, sock, ETH_P_ALL);
	
	return sock;
}

/*
 * init tcp package, ready for tcp ping
 * if_name: 	interface name, eg:eth0/wlan0
 * dst_ip: 		target machine ip address
 * dst_port:	target machine port number
 * tcp_packet: 	output package data
 * return:		0,sucessed; -1,failed;
 */
int tcpping_packet_init(char *if_name, char *dst_ip, unsigned int dst_port, unsigned int seq, tcp_packet_t *tcp_packet)
{
	struct ifreq infr;
	struct in_addr target_addr;
	struct in_addr gateway_addr;
	struct in_addr gateway_mask;

	// local net info
	char src_mac[DEFAULT_CHAR_LENGTH];
	char src_ip[DEFAULT_CHAR_LENGTH];
	char local_mask[DEFAULT_CHAR_LENGTH];	
	unsigned int src_port = 54321; // can be random()%65536;
	// remote net info
	char dst_mac[DEFAULT_CHAR_LENGTH];
	//char dst_ip[DEFAULT_CHAR_LENGTH];
	//unsigned int dst_port = 80;
	// tcp net info
	int window = DEFAULT_WINDOWS_SIZE;
	int mss = DEFAULT_MSS_SIZE;
	//unsigned int seq = 10000;
	unsigned int ack = 0;
	// Data 
	unsigned char content[] = "";
	
	// get local interface information
	strncpy(infr.ifr_name, if_name, sizeof(infr.ifr_name)-1);
	infr.ifr_name[sizeof(infr.ifr_name)-1] = '\0';
	get_local_ip_mac(infr, src_ip, local_mask, src_mac);
	//printf("src ip = %s, src mac = %s\n", src_ip, src_mac);
	
	// get gateway info
	inet_aton(dst_ip, &target_addr);
	get_gateway(target_addr, &gateway_addr, &gateway_mask, &if_name[0]);
	// dst mac
	int found_mac;
	if (gateway_addr.s_addr == 0)
		found_mac = get_gateway_mac(target_addr, dst_mac);
	else
		found_mac = get_gateway_mac(gateway_addr, dst_mac);

	if (found_mac > 0)
		strcpy(dst_mac, src_mac);
	//printf("dst ip = %s, dst mac = %s\n", dst_ip, dst_mac);
	

	// Create RAW TCP packet
	// (ip header + TCP header + TCP option + data) 
	create_tcp_packet(src_mac, dst_mac, src_ip, dst_ip, src_port, dst_port, seq, ack, window, mss, content, tcp_packet);
		
	return 0;
}

/*
 * tcp ping core function
 * sock:		socket fd
 * tcp_packet:	tcp package, inited by tcpping_packet_init
 * buf:			used for receive data buffer
 * return:		receive data len
 */
int tcpping(int sock, tcp_packet_t tcp_packet, unsigned char *buf)
{
	fd_set fdmask;
	FD_ZERO(&fdmask);
	FD_SET(sock, &fdmask);
	struct timeval to;
	struct sockaddr_in packet_info;
	int ret_select = 0, len = 0;

	
	if(!send_raw_packet(sock, tcp_packet.packet, tcp_packet.packet_len)) {
		perror("Error sending packet");
		return -1;
	}

	to.tv_sec = 2;	// 2s time out
	to.tv_usec = 0;
	packet_info.sin_family = AF_INET;
	packet_info.sin_port = 0;
	socklen_t packet_info_size = 0;
	// receive real package;
	for(;;) {
		ret_select = select(sock + 1, &fdmask, NULL, NULL, &to);
		if (ret_select == 0) {
			printf("Time out!\n");
			break;
		}

		len = recvfrom(sock, buf, BUF_SIZE, 0, (struct sockaddr*)&packet_info, &packet_info_size);
		printf("packet_info len = %d!\n", len);
		if (len == 74)
			break;
		//printf("packet_info.sin_family = %d!\n", packet_info.sin_family);
		
		packet_info.sin_family = AF_INET;
		packet_info_size = 0;
	}			
	
	return len;
}

/*
 * tcp ping ack package parse and check
 * packet:	received package
 * len:		packet's length
 * seq:		send data's seq num
 * return:	0,sucessed; -1,failed;
 */
int tcpping_ack_check(unsigned char *packet, int len, unsigned int seq)
{
	unsigned int echo_seq = 0, echo_ack = 0, ret = 0;
	
	ret = parse_packet(packet, len, &echo_seq, &echo_ack);
	if ((ret < 0) || (echo_ack != seq + 1)) {
		return -1;
	}
	
	return 0;
}

/*
 * tcpping reset, ready for next tcpping
 * sock:		socket fd
 * tcp_packet:	tcp package, inited by tcpping_packet_init
 * timeout:		clean socket buffer, then timeout
 */
int tcpping_packet_reset(int sock, tcp_packet_t tcp_packet, int timeout)
{
	// Free packet
	free(tcp_packet.packet);
	tcp_packet.packet = NULL;
	
	collect_packets(sock, timeout);
	
	return 0;
}

int tcpping_close(int sock)
{
	close(sock);
	
	return 0;
}





//=======================
int main(int argc, char** argv){

	char if_name[DEFAULT_CHAR_LENGTH] = "eth0";

	char* target = argv[1];
	//char target[] = "www.sina.com.cn";
	unsigned int seq = 10000;
	char dst_ip[DEFAULT_CHAR_LENGTH];

	
	unsigned char buf[BUF_SIZE] = "\0";
	fd_set fdmask;
	unsigned long long timestamp_begin = 0;
	unsigned long long timestamp_end = 0;
	double ms = 0;
	int ret = 0;
	unsigned int len = 0;
	unsigned int dst_port = 80;	// windows pc 一般未监听80端口
	int sock = 0;
	

	
	seq = random();
	
	get_dst_ip(target, dst_ip);	
	printf("%s, ip = %s\n", target, dst_ip);
	
	FD_ZERO(&fdmask);
	FD_SET(sock, &fdmask);

	// Initialize the socket 
	//fake_sock = fool_kernel_tcpping_socket_create(local_addr, src_port);
	sock = tcpping_socket_create(if_name);

	
	tcpping_packet_init(if_name, dst_ip, dst_port, seq, &g_tcp_packet);
	
	// Record time before the packet sent 
	timestamp_begin = get_mono_time();	
	len = tcpping(sock, g_tcp_packet, buf);
	// Get the end time stamp immediately after received it
	timestamp_end = get_mono_time();
	
	ret = tcpping_ack_check(buf, len, seq);
	if (ret >= 0) {
		ms = (timestamp_end - timestamp_begin)*1.0/1000000;
		printf("ACK/RST packet from %s: seq=%d time=%.4f ms\n", dst_ip, seq, ms);
	}
	
	tcpping_packet_reset(sock, g_tcp_packet, 1);
	
	tcpping_close(sock);


	
	

	
	return 0;
}



