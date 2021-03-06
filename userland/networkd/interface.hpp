#pragma once

#include <sys/uio.h>
#include <string>
#include <vector>
#include <thread>
#include <memory>
#include <list>
#include <cloudabi_types.h>

#include <mstd/optional.hpp>

namespace networkd {

struct interface_ipv4addr {
	char ip[4];
	uint8_t cidr_prefix;
	/* TODO: flag bits, like 'temporary'? */
};

struct arp_pending_frame {
	std::string frame;
	std::string ip_hop;
	cloudabi_timestamp_t timeout;
};

struct interface : public std::enable_shared_from_this<interface> {
	interface(std::string name, std::string mac, std::string hwtype, int rawsock);
	~interface();
	void start();

	inline std::string get_name() const { return name; }
	// Get MAC as a packed string (6 bytes in the case of Ethernet)
	inline std::string get_mac() const { return mac; }
	inline std::string get_hwtype() const { return hwtype; }
	inline std::vector<interface_ipv4addr> const &get_ipv4addrs() const { return ipv4addrs; }
	mstd::optional<std::string> get_primary_ipv4addr() const;

	void add_ipv4addr(const char *ip, uint8_t cidr_prefix);

	cloudabi_errno_t send_ip_packet(std::vector<iovec> const&, std::string ip_hop);
	cloudabi_errno_t send_frame(std::vector<iovec>, std::string mac);
	cloudabi_errno_t send_frame(std::vector<iovec>);
	void check_pending_arp_list();

private:
	/* Read frames from the rawsock, hand them to the right protocol implementation */
	void run();

	std::string name;
	std::string mac;
	std::string hwtype;

	std::thread thr;
	std::vector<interface_ipv4addr> ipv4addrs;
	std::list<arp_pending_frame> frames_pending_arp;

	int rawsock;
};

}
