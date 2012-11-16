/**
 * Copyright (C) 2012 Steven Barth <steven@midlink.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License v2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/ip6.h>
#include <netpacket/packet.h>
#include <linux/netlink.h>

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <sys/types.h>

#include <ifaddrs.h>
#include <fcntl.h>

#include "6relayd.h"


static struct relayd_config config;

static int epoll;
static size_t epoll_registered = 0;
static volatile bool do_stop = false;

static int print_usage(const char *name);
static void set_stop(_unused int signal);
static int open_interface(struct relayd_interface *iface,
		const char *ifname, bool external);
static void relayd_receive_packets(struct relayd_event *event);


int main(int argc, char* const argv[])
{
	const char *pidfile = "/var/run/6relayd.pid";
	bool daemonize = false;
	int verbosity = 0;
	int c;
	while ((c = getopt(argc, argv, "ASR:D:NFslnrp:dvh")) != -1) {
		switch (c) {
		case 'A':
			config.enable_router_discovery_relay = true;
			config.enable_dhcpv6_relay = true;
			config.enable_ndp_relay = true;
			config.enable_forwarding = true;
			config.send_router_solicitation = true;
			config.enable_route_learning = true;
			config.force_address_assignment = true;
			break;

		case 'S':
			config.enable_router_discovery_relay = true;
			config.enable_router_discovery_server = true;
			config.enable_dhcpv6_relay = true;
			config.enable_dhcpv6_server = true;
			break;

		case 'R':
			config.enable_router_discovery_relay = true;
			if (!strcmp(optarg, "server"))
				config.enable_router_discovery_server = true;
			else if (strcmp(optarg, "relay"))
				return print_usage(argv[0]);
			break;

		case 'D':
			config.enable_dhcpv6_relay = true;
			if (!strcmp(optarg, "transparent"))
				config.compat_broken_dhcpv6 = true;
			else if (!strcmp(optarg, "server"))
				config.enable_dhcpv6_server = true;
			else if (strcmp(optarg, "relay"))
				return print_usage(argv[0]);
			break;

		case 'N':
			config.enable_ndp_relay = true;
			break;

		case 'F':
			config.enable_forwarding = true;
			break;

		case 's':
			config.send_router_solicitation = true;
			break;

		case 'l':
			config.force_address_assignment = true;
			break;

		case 'n':
			config.always_rewrite_dns = true;
			break;

		case 'r':
			config.enable_route_learning = true;
			break;

		case 'p':
			pidfile = optarg;
			break;

		case 'd':
			daemonize = true;
			break;

		case 'v':
			verbosity++;
			break;

		default:
			return print_usage(argv[0]);
		}
	}

	openlog("6relayd", LOG_PERROR | LOG_PID, LOG_DAEMON);
	if (verbosity == 0)
		setlogmask(LOG_UPTO(LOG_WARNING));
	else if (verbosity == 1)
		setlogmask(LOG_UPTO(LOG_INFO));

	if (argc - optind < 1)
		return print_usage(argv[0]);

	if (getuid() != 0) {
		syslog(LOG_ERR, "Must be run as root. stopped.");
		return 2;
	}

	if ((epoll = epoll_create1(EPOLL_CLOEXEC)) < 0) {
		syslog(LOG_ERR, "Unable to open epoll: %s", strerror(errno));
		return 2;
	}

	if (open_interface(&config.master, argv[optind++], false))
		return 3;

	config.slavecount = argc - optind;
	config.slaves = calloc(config.slavecount, sizeof(*config.slaves));

	for (size_t i = 0; i < config.slavecount; ++i) {
		const char *name = argv[optind + i];
		bool external = (name[0] == '~');
		if (external)
			++name;

		if (open_interface(&config.slaves[i], name, external))
			return 3;
	}

	srandom(clock() ^ getpid());

	if (init_router_discovery_relay(&config))
		return 4;

	if (init_dhcpv6_relay(&config))
		return 4;

	if (init_ndp_proxy(&config))
		return 4;

	if (config.enable_forwarding)
		relayd_sysctl_interface("all", "forwarding", "1");

	if (epoll_registered == 0) {
		syslog(LOG_WARNING, "No relays enabled or no slave "
				"interfaces specified. stopped.");
		return 5;
	}

	if (daemonize) {
		openlog("6relayd", LOG_PID, LOG_DAEMON); // Disable LOG_PERROR
		if (daemon(0, 0)) {
			syslog(LOG_ERR, "Failed to daemonize: %s",
					strerror(errno));
			return 6;
		}
		FILE *fp = fopen(pidfile, "w");
		if (fp) {
			fprintf(fp, "%i\n", getpid());
			fclose(fp);
		}
	}

	signal(SIGTERM, set_stop);
	signal(SIGHUP, set_stop);
	signal(SIGINT, set_stop);

	// Main loop
	while (!do_stop) {
		struct epoll_event ev[16];
		int len = epoll_wait(epoll, ev, 16, -1);
		for (int i = 0; i < len; ++i) {
			struct relayd_event *event = ev[i].data.ptr;
			if (event->handle_event)
				event->handle_event(event);
			else if (event->handle_dgram)
				relayd_receive_packets(event);
		}
	}

	syslog(LOG_WARNING, "Termination requested by signal.");

	// Deinitializing
	if (config.enable_forwarding)
		relayd_sysctl_interface("all", "forwarding", "0");

	deinit_ndp_proxy();
	free(config.slaves);
	return 0;
}


static int print_usage(const char *name)
{
	fprintf(stderr,
	"Usage: %s [options] <master> [[~]<slave1> [[~]<slave2> [...]]]\n"
	"\nNote: to use server features only (no relaying) set master to lo.\n"
	"\nFeatures:\n"
	"	-A		Automatic relay (defaults: RrelayDrelayNFslr)\n"
	"	-S		Automatic server (defaults: RserverDserver)\n"
	"	-R <mode>	Enable Router Discovery support (RD)\n"
	"	   relay	relay mode\n"
	"	   server	mini-server for Router Discovery on slaves\n"
	"	-D <mode>	Enable DHCPv6-support\n"
	"	   relay	standards-compliant relay\n"
	"	   transparent	transparent relay for broken servers\n"
	"	   server	mini-server for stateless DHCPv6 on slaves\n"
	"	-N		Enable Neighbor Discovery Proxy (NDP)\n"
	"	-F		Enable Forwarding for interfaces\n"
	"\nFeature options:\n"
	"	-s		Send initial RD-Solicitation to <master>\n"
	"	-l		RD: Force local address assignment\n"
	"	-n		RD/DHCPv6: always rewrite name server\n"
	"	-r		NDP: learn routes to neighbors\n"
	"	slave prefix ~	NDP: don't proxy NDP for hosts and only\n"
	"			serve NDP for DAD and traffic to router\n"
	"\nInvocation options:\n"
	"	-p <pidfile>	Set pidfile (/var/run/6relayd.pid)\n"
	"	-d		Daemonize\n"
	"	-v		Increase logging verbosity\n"
	"	-h		Show this help\n\n",
	name);
	return 1;
}


static void set_stop(_unused int signal)
{
	do_stop = true;
}


// Create an interface context
static int open_interface(struct relayd_interface *iface,
		const char *ifname, bool external)
{
	int status = 0;
	int sock = socket(AF_INET6, SOCK_DGRAM, 0);

	size_t ifname_len = strlen(ifname) + 1;
	if (ifname_len > IF_NAMESIZE)
		ifname_len = IF_NAMESIZE;

	struct ifreq ifr;
	memcpy(ifr.ifr_name, ifname, ifname_len);

	// Detect interface index
	if (ioctl(sock, SIOCGIFINDEX, &ifr) < 0)
		goto err;

	iface->ifindex = ifr.ifr_ifindex;

	ioctl(sock, SIOCGIFMTU, &ifr);
	iface->mtu = ifr.ifr_mtu;

	// Detect MAC-address of interface
	if (ioctl(sock, SIOCGIFHWADDR, &ifr) < 0)
		goto err;

	// Fill interface structure
	memcpy(iface->mac, ifr.ifr_hwaddr.sa_data, sizeof(iface->mac));
	memcpy(iface->ifname, ifname, ifname_len);
	iface->external = external;

	goto out;

err:
	syslog(LOG_ERR, "Unable to open interface %s (%s)",
			ifname, strerror(errno));
	status = -1;
out:
	close(sock);
	return status;
}


int relayd_sysctl_interface(const char *ifname, const char *option,
		const char *data)
{
	char pathbuf[64];
	const char *sysctl_pattern = "/proc/sys/net/ipv6/conf/%s/%s";
	snprintf(pathbuf, sizeof(pathbuf), sysctl_pattern, ifname, option);

	int fd = open(pathbuf, O_WRONLY);
	int written = write(fd, data, strlen(data));
	close(fd);

	return (written > 0) ? 0 : -1;
}


// Register events for the multiplexer
int relayd_register_event(struct relayd_event *event)
{
	struct epoll_event ev = {EPOLLIN | EPOLLET, {event}};
	if (!epoll_ctl(epoll, EPOLL_CTL_ADD, event->socket, &ev)) {
		++epoll_registered;
		return 0;
	} else {
		return -1;
	}
}


// Forwards a packet on a specific interface
ssize_t relayd_forward_packet(int socket, struct sockaddr_in6 *dest,
		struct iovec *iov, size_t iov_len,
		const struct relayd_interface *iface)
{
	// Construct headers
	uint8_t cmsg_buf[CMSG_SPACE(sizeof(struct in6_pktinfo))] = {0};
	struct msghdr msg = {(void*)dest, sizeof(*dest), iov, iov_len,
				cmsg_buf, sizeof(cmsg_buf), 0};

	// Set control data (define destination interface)
	struct cmsghdr *chdr = CMSG_FIRSTHDR(&msg);
	chdr->cmsg_level = IPPROTO_IPV6;
	chdr->cmsg_type = IPV6_PKTINFO;
	chdr->cmsg_len = CMSG_LEN(sizeof(struct in6_pktinfo));
	struct in6_pktinfo *pktinfo = (struct in6_pktinfo*)CMSG_DATA(chdr);
	pktinfo->ipi6_ifindex = iface->ifindex;

	// Also set scope ID if link-local
	if (IN6_IS_ADDR_LINKLOCAL(&dest->sin6_addr)
			|| IN6_IS_ADDR_MC_LINKLOCAL(&dest->sin6_addr))
		dest->sin6_scope_id = iface->ifindex;

	// IPV6_PKTINFO doesn't really work for IPv6-raw sockets (bug?)
	if (dest->sin6_port == 0) {
		msg.msg_control = NULL;
		msg.msg_controllen = 0;
	}

	char ipbuf[INET6_ADDRSTRLEN];
	inet_ntop(AF_INET6, &dest->sin6_addr, ipbuf, sizeof(ipbuf));

	ssize_t sent = sendmsg(socket, &msg, MSG_DONTWAIT);
	if (sent < 0)
		syslog(LOG_WARNING, "Failed to relay to %s%%%s (%s)",
				ipbuf, iface->ifname, strerror(errno));
	else
		syslog(LOG_NOTICE, "Relayed %li bytes to %s%%%s",
				(long)sent, ipbuf, iface->ifname);
	return sent;
}


// Detect an IPV6-address currently assigned to the given interface
int relayd_get_interface_address(struct in6_addr *dest,
		const char *ifname, bool allow_linklocal)
{
	bool found_address = false;
	struct ifaddrs *ifaddrs;
	if (getifaddrs(&ifaddrs))
		return -1;

	for (struct ifaddrs *c = ifaddrs; found_address == false && c != NULL;
			c = c->ifa_next) {
		if (!c->ifa_addr || c->ifa_addr->sa_family != AF_INET6 ||
				strcmp(c->ifa_name, ifname))
			continue; // Skip unrelated

		struct sockaddr_in6 *dst = (struct sockaddr_in6*)c->ifa_addr;
		if (!allow_linklocal && IN6_IS_ADDR_LINKLOCAL(&dst->sin6_addr))
			continue;

		found_address = true;
		memcpy(dest, &dst->sin6_addr, sizeof(struct in6_addr));
	}

	freeifaddrs(ifaddrs);
	if (!found_address) {
		syslog(LOG_WARNING, "failed to detect suitable "
				"source address for %s", ifname);
		return -1;
	} else {
		return 0;
	}
}


struct relayd_interface* relayd_get_interface_by_index(int ifindex)
{
	if (config.master.ifindex == ifindex)
		return &config.master;

	for (size_t i = 0; i < config.slavecount; ++i)
		if (config.slaves[i].ifindex == ifindex)
			return &config.slaves[i];

	return NULL;
}


// Convenience function to receive and do basic validation of packets
static void relayd_receive_packets(struct relayd_event *event)
{
	uint8_t data_buf[RELAYD_BUFFER_SIZE], cmsg_buf[128];
	union {
		struct sockaddr_in6 in6;
		struct sockaddr_ll ll;
		struct sockaddr_nl nl;
	} addr;

	while (true) {
		struct iovec iov = {data_buf, sizeof(data_buf)};
		struct msghdr msg = {&addr, sizeof(addr), &iov, 1,
				cmsg_buf, sizeof(cmsg_buf), 0};

		ssize_t len = recvmsg(event->socket, &msg, MSG_DONTWAIT);
		if (len < 0 && errno == EAGAIN)
			break;

		// Extract destination interface
		int destiface = 0;
		struct in6_pktinfo *pktinfo;
		for (struct cmsghdr *ch = CMSG_FIRSTHDR(&msg); ch != NULL &&
				destiface == 0; ch = CMSG_NXTHDR(&msg, ch)) {
			if (ch->cmsg_level == IPPROTO_IPV6 &&
					ch->cmsg_type == IPV6_PKTINFO) {
				pktinfo = (struct in6_pktinfo*)CMSG_DATA(ch);
				destiface = pktinfo->ipi6_ifindex;
			}
		}

		// Detect interface for packet sockets
		if (addr.ll.sll_family == AF_PACKET)
			destiface = addr.ll.sll_ifindex;

		struct relayd_interface *iface =
				relayd_get_interface_by_index(destiface);

		if (!iface && addr.nl.nl_family != AF_NETLINK)
			continue;

		char ipbuf[INET6_ADDRSTRLEN] = "kernel";
		if (addr.ll.sll_family == AF_PACKET &&
				len >= (ssize_t)sizeof(struct ip6_hdr))
			inet_ntop(AF_INET6, &data_buf[8], ipbuf, sizeof(ipbuf));
		else if (addr.in6.sin6_family == AF_INET6)
			inet_ntop(AF_INET6, &addr.in6.sin6_addr, ipbuf, sizeof(ipbuf));

		syslog(LOG_NOTICE, "--");
		syslog(LOG_NOTICE, "Received %li Bytes from %s%%%s", (long)len,
				ipbuf, (iface) ? iface->ifname : "netlink");

		event->handle_dgram(&addr, data_buf, len, iface);
	}
}