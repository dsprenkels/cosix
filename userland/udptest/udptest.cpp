#include <stdio.h>
#include <stdlib.h>
#include <program.h>
#include <argdata.h>
#include <sched.h>
#include <pthread.h>
#include <atomic>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <argdata.hpp>
#include <cloudabi_syscalls.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/procdesc.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int stdout;
int tmpdir;
int networkd;

int networkd_get_socket(int type, std::string connect, std::string bind) {
	std::string command;
	if(type == SOCK_DGRAM) {
		command = "udpsock";
	} else if(type == SOCK_STREAM) {
		command = "tcpsock";
	} else {
		throw std::runtime_error("Unknown type of socket to get");
	}

	std::unique_ptr<argdata_t> keys[] =
		{argdata_t::create_str("command"), argdata_t::create_str("connect"), argdata_t::create_str("bind")};
	std::unique_ptr<argdata_t> values[] =
		{argdata_t::create_str(command.c_str()), argdata_t::create_str(connect.c_str()), argdata_t::create_str(bind.c_str())};
	std::vector<argdata_t*> key_ptrs;
	std::vector<argdata_t*> value_ptrs;
	
	for(auto &key : mstd::range<std::unique_ptr<argdata_t>>(keys)) {
		key_ptrs.push_back(key.get());
	}
	for(auto &value : mstd::range<std::unique_ptr<argdata_t>>(values)) {
		value_ptrs.push_back(value.get());
	}
	auto map = argdata_t::create_map(key_ptrs, value_ptrs);

	std::vector<unsigned char> rbuf;
	map->serialize(rbuf);

	write(networkd, rbuf.data(), rbuf.size());
	// TODO: set non-blocking flag once kernel supports it
	// this way, we can read until EOF instead of only 200 bytes
	// TODO: for a generic implementation, MSG_PEEK to find the number
	// of file descriptors
	uint8_t buf[1500];
	struct iovec iov = {.iov_base = buf, .iov_len = sizeof(buf)};
	alignas(struct cmsghdr) char control[CMSG_SPACE(sizeof(int))];
	struct msghdr msg = {
		.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = control, .msg_controllen = sizeof(control),
	};
	ssize_t size = recvmsg(networkd, &msg, 0);
	if(size < 0) {
		perror("read");
		exit(1);
	}
	auto response = argdata_t::create_from_buffer(mstd::range<unsigned char const>(&buf[0], size));
	int fdnum = -1;
	for(auto i : response->as_map()) {
		auto key = i.first->as_str();
		if(key == "error") {
			throw std::runtime_error("Failed to retrieve UDP socket from networkd: " + i.second->as_str().to_string());
		} else if(key == "fd") {
			fdnum = *i.second->get_fd();
		}
	}
	if(fdnum != 0) {
		throw std::runtime_error("Ifstore UDP socket not received");
	}
	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
	if(cmsg == nullptr || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_len != CMSG_LEN(sizeof(int))) {
		dprintf(stdout, "Ifstore socket requested, but not given\n");
		exit(1);
	}
	int *fdbuf = reinterpret_cast<int*>(CMSG_DATA(cmsg));
	return fdbuf[0];
}

void program_main(const argdata_t *ad) {
	argdata_map_iterator_t it;
	const argdata_t *key;
	const argdata_t *value;
	argdata_map_iterate(ad, &it);
	while (argdata_map_get(&it, &key, &value)) {
		const char *keystr;
		if(argdata_get_str_c(key, &keystr) != 0) {
			argdata_map_next(&it);
			continue;
		}

		if(strcmp(keystr, "stdout") == 0) {
			argdata_get_fd(value, &stdout);
		} else if(strcmp(keystr, "networkd") == 0) {
			argdata_get_fd(value, &networkd);
		} else if(strcmp(keystr, "tmpdir") == 0) {
			argdata_get_fd(value, &tmpdir);
		}
		argdata_map_next(&it);
	}

	FILE *out = fdopen(stdout, "w");
	fswap(stderr, out);

	// First test, to see IP stack sending behaviour in PCAP dumps:
	// Connect to 8.8.8.8:1234, send packet.
	int sock0 = networkd_get_socket(SOCK_DGRAM, "8.8.8.8:1234", "");
	write(sock0, "Hello Google!", 13);

	// Socket A: bound to 0.0.0.0:1234, connected to 127.0.0.1:5678
	// Send packet over A: nothing happens
	int sockA = networkd_get_socket(SOCK_DGRAM, "127.0.0.1:5678", "0.0.0.0:1234");
	write(sockA, "Hello world!", 12);

	// Socket B: bound to 127.0.0.1:5678, connected to 127.0.0.1:1234
	// Send packet over B, read from A
	// Send packet over A, read from B
	int sockB = networkd_get_socket(SOCK_DGRAM, "127.0.0.1:1234", "127.0.0.1:5678");
	if(write(sockB, "Foo bar!", 8) != 8) {
		dprintf(stdout, "Failed to write data over UDP (%s)\n", strerror(errno));
		exit(1);
	}
	char buf[16];
	if(read(sockA, buf, sizeof(buf)) != 8 || memcmp(buf, "Foo bar!", 8) != 0) {
		dprintf(stdout, "Failed to receive data over UDP (%s)\n", strerror(errno));
		exit(1);
	}

	// Socket C: bound to 0.0.0.0:1234, not connected, will accept() to respond to requests
	// Send packet from B, D=accept(C), sock_stat_get(), recvfrom() from C, check addresses
	// Respond from new socket D, read from B
	int sockC = networkd_get_socket(SOCK_DGRAM, "", "0.0.0.0:1234");
	if(write(sockB, "Baz quux", 8) != 8) {
		dprintf(stdout, "Failed to write data over UDP (%s)\n", strerror(errno));
		exit(1);
	}
	sockaddr_in address;
	sockaddr *address_ptr = reinterpret_cast<sockaddr*>(&address);
	size_t address_size = sizeof(address);
	int sockD = accept(sockC, address_ptr, &address_size);
	if(sockD < 0) {
		dprintf(stdout, "Failed to accept() UDP socket (%s)\n", strerror(errno));
		exit(1);
	}
	char ip[16];
	inet_ntop(address.sin_family, &address.sin_addr, ip, sizeof(ip));
	if(address.sin_family != AF_INET || strcmp(ip, "127.0.0.1") != 0 /* peer port is random */)
	{
		dprintf(stdout, "Address on socket is incorrect\n");
		exit(1);
	}
	cloudabi_sockstat_t sockstat;
	auto res = cloudabi_sys_sock_stat_get(sockD, &sockstat, 0);
	if(res != 0) {
		dprintf(stdout, "Failed to sys_sock_stat_get (%s)\n", strerror(res));
		exit(1);
	}
	inet_ntop(sockstat.ss_peername.sa_family, &sockstat.ss_peername.sa_inet.addr[0], ip, sizeof(ip));
	if(sockstat.ss_peername.sa_family != AF_INET || strcmp(ip, "127.0.0.1") != 0 || sockstat.ss_peername.sa_inet.port != 5678)
	{
		dprintf(stdout, "Address on socket is incorrect (%s:%d)\n", ip, sockstat.ss_peername.sa_inet.port);
		exit(1);
	}
	address_size = sizeof(address);
	if(recvfrom(sockD, buf, sizeof(buf), 0, address_ptr, &address_size) != 8
	|| memcmp(buf, "Baz quux", 8) != 0) {
		dprintf(stdout, "Failed to recvfrom (%s)\n", strerror(res));
		exit(1);
	}
	// TODO: address on recvform() is not supported
	/*
	inet_ntop(address.sin_family, &address.sin_addr, ip, sizeof(ip));
	if(address.sin_family != AF_INET || strcmp(ip, "127.0.0.1") != 0 || address.sin_port != 5678)
	{
		dprintf(stdout, "Address on socket is incorrect (%s:%d)\n", ip, address.sin_port);
		exit(1);
	}
	*/
	if(write(sockD, "Noot noot", 9) != 9) {
		dprintf(stdout, "Failed to write data over UDP (%s)\n", strerror(errno));
		exit(1);
	}
	if(read(sockB, buf, sizeof(buf)) != 9 || memcmp(buf, "Noot noot", 9) != 0) {
		dprintf(stdout, "Failed to receive data over UDP (%s)\n", strerror(errno));
		exit(1);
	}

	// Socket E: connected to 127.0.0.1:1234, not bound
	// Send packet from E, F=accept(C), read from F, respond, read from E, works!
	int sockE = networkd_get_socket(SOCK_DGRAM, "127.0.0.1:1234", "");
	if(write(sockE, "Mumblebumble", 12) != 12) {
		dprintf(stdout, "Failed to write data over UDP (%s)\n", strerror(errno));
		exit(1);
	}
	address_size = sizeof(address);
	int sockF = accept(sockC, address_ptr, &address_size);
	if(sockF < 0) {
		dprintf(stdout, "Failed to accept() UDP socket (%s)\n", strerror(errno));
		exit(1);
	}
	inet_ntop(address.sin_family, &address.sin_addr, ip, sizeof(ip));
	if(address.sin_family != AF_INET || strcmp(ip, "127.0.0.1") != 0 /* peer port is random */)
	{
		dprintf(stdout, "Address on socket is incorrect (%s:%d)\n", ip, address.sin_port);
		exit(1);
	}
	res = cloudabi_sys_sock_stat_get(sockF, &sockstat, 0);
	if(res != 0) {
		dprintf(stdout, "Failed to sys_sock_stat_get (%s)\n", strerror(res));
		exit(1);
	}
	inet_ntop(sockstat.ss_peername.sa_family, &sockstat.ss_peername.sa_inet.addr, ip, sizeof(ip));
	if(sockstat.ss_peername.sa_family != AF_INET || strcmp(ip, "127.0.0.1") != 0)
	{
		dprintf(stdout, "Peer address on socket is incorrect (%s:%d)\n", ip, sockstat.ss_peername.sa_inet.port);
		exit(1);
	}
	inet_ntop(sockstat.ss_sockname.sa_family, &sockstat.ss_sockname.sa_inet.addr, ip, sizeof(ip));
	if(sockstat.ss_sockname.sa_family != AF_INET || strcmp(ip, "127.0.0.1") != 0
	|| sockstat.ss_sockname.sa_inet.port != 1234)
	{
		dprintf(stdout, "Local address on socket is incorrect (%s:%d)\n", ip, sockstat.ss_sockname.sa_inet.port);
		exit(1);
	}
	address_size = sizeof(address);
	if(recvfrom(sockF, buf, sizeof(buf), 0, address_ptr, &address_size) != 12
	|| memcmp(buf, "Mumblebumble", 12) != 0) {
		dprintf(stdout, "Failed to recvfrom (%s)\n", strerror(res));
		exit(1);
	}
	// TODO: address on recvform() is not supported
	/*
	inet_ntop(address.sin_family, &address.sin_addr, ip, sizeof(ip));
	if(address.sin_family != AF_INET || strcmp(ip, "127.0.0.1") != 0
	|| address.sin_port == 5678 / * can't be 5678, that's already taken * /)
	{
		dprintf(stdout, "Address on socket is incorrect\n");
		exit(1);
	}
	*/
	if(write(sockF, "Dumbleflumble", 13) != 13) {
		dprintf(stdout, "Failed to write data over UDP (%s)\n", strerror(errno));
		exit(1);
	}
	if(read(sockE, buf, sizeof(buf)) != 13 || memcmp(buf, "Dumbleflumble", 13) != 0) {
		dprintf(stdout, "Failed to receive data over UDP (%s)\n", strerror(errno));
		exit(1);
	}

	dprintf(stdout, "All UDP traffic seems correct!\n");
	exit(0);
}
