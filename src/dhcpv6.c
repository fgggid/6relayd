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
 *
 */

#include <errno.h>
#include <unistd.h>
#include <stddef.h>
#include <sys/timerfd.h>

#include "6relayd.h"
#include "dhcpv6.h"


static void relay_client_request(struct sockaddr_in6 *source,
		const void *data, size_t len, struct relayd_interface *iface);
static void relay_client_request_broken(struct sockaddr_in6 *source,
		void *data, size_t len, struct relayd_interface *iface);
static void relay_server_response(uint8_t *data, size_t len);

static int create_socket(uint16_t port);

static void handle_dhcpv6(void *addr, void *data, size_t len,
		struct relayd_interface *iface);
static void handle_client_request(void *addr, void *data, size_t len,
		struct relayd_interface *iface);

static struct relayd_event dhcpv6_event = {-1, NULL, handle_dhcpv6};
static struct relayd_event broken_dhcpv6_event = {-1, NULL, handle_dhcpv6};

static const struct relayd_config *config = NULL;



// Create socket and register events
int init_dhcpv6_relay(const struct relayd_config *relayd_config)
{
	config = relayd_config;

	if (!config->enable_dhcpv6_relay || config->slavecount < 1)
		return 0;

	if ((dhcpv6_event.socket = create_socket(DHCPV6_SERVER_PORT)) < 0) {
		syslog(LOG_ERR, "Failed to open DHCPv6 server socket: %s",
				strerror(errno));
		return -1;
	}


	// Configure multicast settings
	struct ipv6_mreq mreq = {ALL_DHCPV6_RELAYS, 0};
	for (size_t i = 0; i < config->slavecount; ++i) {
		mreq.ipv6mr_interface = config->slaves[i].ifindex;
		setsockopt(dhcpv6_event.socket, IPPROTO_IPV6,
				IPV6_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
	}

	if (config->enable_dhcpv6_server)
		dhcpv6_event.handle_dgram = handle_client_request;
	relayd_register_event(&dhcpv6_event);


	// Use broken DHCPv6 server
	if (config->compat_broken_dhcpv6 && config->enable_dhcpv6_relay) {
		broken_dhcpv6_event.socket =
				create_socket(DHCPV6_CLIENT_PORT);
		if (broken_dhcpv6_event.socket < 0) {
			syslog(LOG_ERR, "Failed to open DHCPv6 client socket: "
					" %s", strerror(errno));
			return -1;
		}

		setsockopt(broken_dhcpv6_event.socket, SOL_SOCKET,
				SO_BINDTODEVICE, config->master.ifname,
				sizeof(config->master.ifname));
		relayd_register_event(&broken_dhcpv6_event);
	}

	return 0;
}


// Create server socket
static int create_socket(uint16_t port)
{
	int sock = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
	if (sock < 0)
		return -1;

	// Basic IPv6 configuration
	int val = 1;
	setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &val, sizeof(val));
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
	setsockopt(sock, IPPROTO_IPV6, IPV6_RECVPKTINFO, &val, sizeof(val));

	val = DHCPV6_HOP_COUNT_LIMIT;
	setsockopt(sock, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &val, sizeof(val));

	struct sockaddr_in6 bind_addr = {AF_INET6, htons(port),
				0, IN6ADDR_ANY_INIT, 0};
	if (bind(sock, (struct sockaddr*)&bind_addr, sizeof(bind_addr))) {
		close(sock);
		return -1;
	}

	return sock;
}


static void handle_nested_message(uint8_t *data, size_t len,
		uint8_t **opts, uint8_t **end, struct iovec iov[2])
{
	struct dhcpv6_relay_header *hdr = (struct dhcpv6_relay_header*)data;
	if (iov[0].iov_base == NULL)
		iov[0].iov_base = data;

	if (len < sizeof(struct dhcpv6_client_header))
		return;

	if (hdr->msg_type != DHCPV6_MSG_RELAY_FORW) {
		iov[0].iov_len = data - (uint8_t*)iov[0].iov_base;
		struct dhcpv6_client_header *hdr = (void*)data;
		*opts = hdr->options;
		*end = data + len;
		return;
	}

	uint16_t otype, olen;
	uint8_t *odata;
	dhcpv6_for_each_option(hdr->options, data + len, otype, olen, odata) {
		if (otype == DHCPV6_OPT_RELAY_MSG) {
			iov[3].iov_base = odata + olen;
			iov[3].iov_len = (data + len) - (odata + olen);
			handle_nested_message(odata, olen, opts, end, iov);
			return;
		}
	}
}


static void update_nested_message(uint8_t *data, size_t len, ssize_t pdiff)
{
	struct dhcpv6_relay_header *hdr = (struct dhcpv6_relay_header*)data;
	if (hdr->msg_type != DHCPV6_MSG_RELAY_FORW)
		return;

	hdr->msg_type = DHCPV6_MSG_RELAY_REPL;

	uint16_t otype, olen;
	uint8_t *odata;
	dhcpv6_for_each_option(hdr->options, data + len, otype, olen, odata) {
		if (otype == DHCPV6_OPT_RELAY_MSG) {
			olen += pdiff;
			odata[-2] = (olen >> 8) & 0xff;
			odata[-1] = olen & 0xff;
			update_nested_message(odata, olen - pdiff, pdiff);
			return;
		}
	}
}


// Simple DHCPv6-server for information requests
static void handle_client_request(void *addr, void *data, size_t len,
		struct relayd_interface *iface)
{
	struct dhcpv6_client_header *hdr = data;
	if (len < sizeof(*hdr))
		return;

	syslog(LOG_NOTICE, "Got DHCPv6 request");

	// Construct reply message
	struct __attribute__((packed)) {
		uint8_t msg_type;
		uint8_t tr_id[3];
		uint16_t dns_type;
		uint16_t dns_length;
		struct in6_addr addr;
		uint16_t serverid_type;
		uint16_t serverid_length;
		uint16_t duid_type;
		uint16_t hardware_type;
		uint8_t mac[6];
		uint16_t clientid_type;
		uint16_t clientid_length;
		uint8_t clientid_buf[130];
	} dest = {
		.msg_type = DHCPV6_MSG_REPLY,
		.dns_type = htons(DHCPV6_OPT_DNS_SERVERS),
		.dns_length = htons(sizeof(struct in6_addr)),
		.serverid_type = htons(DHCPV6_OPT_SERVERID),
		.serverid_length = htons(10),
		.duid_type = htons(3),
		.hardware_type = htons(1),
		.clientid_type = htons(DHCPV6_OPT_CLIENTID),
		.clientid_buf = {0}
	};
	memcpy(dest.tr_id, hdr->transaction_id, sizeof(dest.tr_id));
	memcpy(dest.mac, iface->mac, sizeof(dest.mac));

	struct __attribute__((packed)) {
		uint16_t type;
		uint16_t len;
		uint16_t value;
	} stat = {htons(DHCPV6_OPT_STATUS), htons(sizeof(stat) - 4),
			htons(DHCPV6_STATUS_NOADDRSAVAIL)};

	struct iovec iov[] = {{NULL, 0}, {&dest, (uint8_t*)&dest.clientid_type
			- (uint8_t*)&dest}, {&stat, 0}, {NULL, 0}};

	uint8_t *opts = hdr->options, *opts_end = (uint8_t*)data + len;
	if (hdr->msg_type == DHCPV6_MSG_RELAY_FORW)
		handle_nested_message(data, len, &opts, &opts_end, iov);
	if (opts[-4] == DHCPV6_MSG_SOLICIT)
		dest.msg_type = DHCPV6_MSG_ADVERTISE;
	else if (opts[-4] == DHCPV6_MSG_REBIND)
		return; // Don't answer rebinds, as we don't do stateful

	// Go through options and find what we need
	uint16_t otype, olen;
	uint8_t *odata;
	dhcpv6_for_each_option(opts, opts_end, otype, olen, odata) {
		if (otype == DHCPV6_OPT_CLIENTID && olen <= 130) {
			dest.clientid_length = htons(olen);
			memcpy(dest.clientid_buf, odata, olen);
			iov[1].iov_len += 4 + olen;
		} else if (otype == DHCPV6_OPT_SERVERID) {
			if (olen != ntohs(dest.serverid_length) ||
					memcmp(odata, &dest.duid_type, olen))
				return; // Not for us
		} else if (otype == DHCPV6_OPT_IA_NA) {
			iov[2].iov_len = sizeof(stat);
		}
	}

	if (iov[0].iov_len > 0) // Update length
		update_nested_message(data, len, iov[1].iov_len +
				iov[2].iov_len - (4 + opts_end - opts));

	if (relayd_get_interface_address(&dest.addr, iface->ifname, true))
		return; // Failed to detect a local address

	relayd_forward_packet(dhcpv6_event.socket, addr, iov, 4, iface);
}


// Central DHCPv6-relay handler
static void handle_dhcpv6(void *addr, void *data, size_t len,
		struct relayd_interface *iface)
{
	if (iface == &config->master)
		relay_server_response(data, len);
	else
		if (!config->compat_broken_dhcpv6)
			relay_client_request(addr, data, len, iface);
		else
			relay_client_request_broken(addr, data, len, iface);
}


// Relay server response (regular relay or broken server handling)
static void relay_server_response(uint8_t *data, size_t len)
{
	// Information we need to gather
	uint8_t *payload_data = NULL;
	size_t payload_len = 0;
	int32_t ifaceidx = 0;
	struct sockaddr_in6 target = {AF_INET6, htons(DHCPV6_CLIENT_PORT),
		0, IN6ADDR_ANY_INIT, 0};

	syslog(LOG_NOTICE, "Got a DHCPv6-reply");

	int otype, olen;
	uint8_t *odata, *end = data + len;

	// Relay DHCPv6 reply from server to client
	if (!config->compat_broken_dhcpv6) {
		struct dhcpv6_relay_header *h = (void*)data;
		if (len < sizeof(*h) || h->msg_type != DHCPV6_MSG_RELAY_REPL)
			return;

		memcpy(&target.sin6_addr, &h->peer_address,
				sizeof(struct in6_addr));

		// Go through options and find what we need
		dhcpv6_for_each_option(h->options, end, otype, olen, odata) {
			if (otype == DHCPV6_OPT_INTERFACE_ID
					&& olen == sizeof(ifaceidx)) {
				memcpy(&ifaceidx, odata, sizeof(ifaceidx));
			} else if (otype == DHCPV6_OPT_RELAY_MSG) {
				payload_data = odata;
				payload_len = olen;
			}
		}
	// Forward DHCPv6 reply from broken server to client
	} else {
		struct dhcpv6_client_header *h = (void*)data;

		// Go through options and find what we need
		dhcpv6_for_each_option(h->options, end, otype, olen, odata) {
			if (otype == DHCPV6_OPT_AUTH)
				return; // Cannot rewrite: stop.
			else if (otype != DHCPV6_OPT_CLIENTID || olen <=
					(int)sizeof(struct dhcpv6_broken_duid)
					|| olen > 130)
				continue;

			// Get our rewritten DUID and extract information
			struct dhcpv6_broken_duid du;
			memcpy(&du, odata, sizeof(du));

			if (du.duid_type == htons(DHCPV6_DUID_VENDOR) &&
					du.vendor == htonl(DHCPV6_ENT_NO) &&
					du.subtype == htons(DHCPV6_ENT_TYPE)) {
				ifaceidx = du.iface_index;
				target.sin6_addr = du.link_addr;

				// Fixup length
				odata[-1] = olen - sizeof(du);
				len -= sizeof(du);

				// Move package back together
				memmove(odata, odata + sizeof(du),
						len - (odata - data));

				payload_data = data;
				payload_len = len;
			}
		}
	}


	struct relayd_interface *iface = NULL;
	for (size_t i = 0; !iface && i < config->slavecount; ++i)
		if (config->slaves[i].ifindex == ifaceidx)
			iface = &config->slaves[i];
	// Invalid interface-id or basic payload
	if (!iface || !payload_data || payload_len < 4)
		return;

	bool is_authenticated = false;
	bool rewrite_dns = false;
	struct in6_addr *dns_ptr = NULL;
	size_t dns_count = 0;

	// If the payload is relay-reply we have to send to the server port
	if (payload_data[0] == DHCPV6_MSG_RELAY_REPL) {
		target.sin6_port = htons(DHCPV6_SERVER_PORT);
	} else { // Go through the payload data
		struct dhcpv6_client_header *h = (void*)payload_data;
		end = payload_data + payload_len;

		dhcpv6_for_each_option(h->options, end, otype, olen, odata) {
			if (otype == DHCPV6_OPT_DNS_SERVERS && olen >= 16) {
				rewrite_dns = config->always_rewrite_dns;
				dns_ptr = (struct in6_addr*)odata;
				dns_count = olen / 16;

				// If there is a link-local DNS we must rewrite
				for (size_t i = 0; !rewrite_dns &&
						i < dns_count; ++i)
					if (IN6_IS_ADDR_LINKLOCAL(&dns_ptr[i]))
						rewrite_dns = true;
			} else if (otype == DHCPV6_OPT_AUTH) {
				is_authenticated = true;
			}
		}
	}

	// Rewrite DNS servers if requested
	if (rewrite_dns && dns_ptr && dns_count > 0) {
		if (is_authenticated)
			return; // Impossible to rewrite

		if (relayd_get_interface_address(&dns_ptr[0],
				iface->ifname, true))
			return; // Unable to get interface address

		// Copy over any other addresses
		for (size_t i = 1; i < dns_count; ++i)
			memcpy(&dns_ptr[i], &dns_ptr[0],
					sizeof(struct in6_addr));
	}

	struct iovec iov = {payload_data, payload_len};
	relayd_forward_packet(dhcpv6_event.socket, &target, &iov, 1, iface);
}


// Relay client request (regular DHCPv6-relay)
static void relay_client_request(struct sockaddr_in6 *source,
		const void *data, size_t len, struct relayd_interface *iface)
{
	const struct dhcpv6_relay_header *h = data;
	if (h->msg_type == DHCPV6_MSG_RELAY_REPL ||
			h->msg_type == DHCPV6_MSG_RECONFIGURE ||
			h->msg_type == DHCPV6_MSG_REPLY ||
			h->msg_type == DHCPV6_MSG_ADVERTISE)
		return; // Invalid message types for client

	syslog(LOG_NOTICE, "Got a DHCPv6-request");

	// Construct our forwarding envelope
	struct dhcpv6_relay_forward_envelope hdr = {
		.msg_type = DHCPV6_MSG_RELAY_FORW,
		.hop_count = 0,
		.interface_id_type = htons(DHCPV6_OPT_INTERFACE_ID),
		.interface_id_len = htons(sizeof(uint32_t)),
		.relay_message_type = htons(DHCPV6_OPT_RELAY_MSG),
		.relay_message_len = htons(len),
	};

	if (h->msg_type == DHCPV6_MSG_RELAY_FORW) { // handle relay-forward
		if (h->hop_count >= DHCPV6_HOP_COUNT_LIMIT)
			return; // Invalid hop count
		else
			hdr.hop_count = h->hop_count + 1;
	}

	// use memcpy here as the destination fields are unaligned
	uint32_t ifindex = iface->ifindex;
	memcpy(&hdr.peer_address, &source->sin6_addr, sizeof(struct in6_addr));
	memcpy(&hdr.interface_id_data, &ifindex, sizeof(ifindex));

	// Detect public IP of slave interface to use as link-address
	if (relayd_get_interface_address(&hdr.link_address,
			iface->ifname, false)) {
		// No suitable address! Is the slave not configured yet?
		// Detect public IP of master interface and use it instead
		// This is WRONG and probably violates the RFC. However
		// otherwise we have a hen and egg problem because the
		// slave-interface cannot be auto-configured.
		if (relayd_get_interface_address(&hdr.link_address,
				config->master.ifname, false))
			return; // Could not obtain a suitable address
	}

	struct sockaddr_in6 dhcpv6_servers = {AF_INET6,
			htons(DHCPV6_SERVER_PORT), 0, ALL_DHCPV6_SERVERS, 0};
	struct iovec iov[2] = {{&hdr, sizeof(hdr)}, {(void*)data, len}};
	relayd_forward_packet(dhcpv6_event.socket, &dhcpv6_servers,
			iov, 2, &config->master);
}


// Forward client request to broken DHCPv6 server
static void relay_client_request_broken(struct sockaddr_in6 *source,
		void *data, size_t len, struct relayd_interface *iface)
{
	struct dhcpv6_client_header *h = data;
	if (h->msg_type == DHCPV6_MSG_RELAY_REPL ||
			h->msg_type == DHCPV6_MSG_RECONFIGURE ||
			h->msg_type == DHCPV6_MSG_REPLY ||
			h->msg_type == DHCPV6_MSG_ADVERTISE)
		return; // Invalid message types for client

	if (len + sizeof(struct dhcpv6_broken_duid) > RELAYD_BUFFER_SIZE)
		return; // Message already too big

	syslog(LOG_NOTICE, "Got a DHCPv6-request");

	int otype, olen;
	uint8_t *odata, *end = ((uint8_t*)data) + len;

	// Go through options and find what we need
	bool rewrite_done = false;
	dhcpv6_for_each_option(h->options, end, otype, olen, odata) {
		if (otype == DHCPV6_OPT_AUTH)
			return; // Cannot rewrite: stop.
		else if (otype != DHCPV6_OPT_CLIENTID)
			continue;

		// Rewrite DUID
		struct dhcpv6_broken_duid du = {
			htons(DHCPV6_DUID_VENDOR),
			htonl(DHCPV6_ENT_NO),
			htons(DHCPV6_ENT_TYPE),
			iface->ifindex,
			source->sin6_addr
		};

		// Assemble package back together
		memmove(odata + sizeof(du), odata,
				len - (odata - (uint8_t*)data));
		memcpy(odata, &du, sizeof(du));

		// Fixup length
		odata[-1] = olen + sizeof(du);
		len += sizeof(du);
		rewrite_done = true;
	}

	if (!rewrite_done)
		return;

	struct sockaddr_in6 dhcpv6_servers = {AF_INET6,
			htons(DHCPV6_SERVER_PORT), 0, ALL_DHCPV6_RELAYS,
			config->master.ifindex};
	struct iovec iov = {(void*)data, len};
	relayd_forward_packet(broken_dhcpv6_event.socket, &dhcpv6_servers,
			&iov, 1, &config->master);
}