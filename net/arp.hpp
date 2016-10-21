#pragma once

#include "global.hpp"
#include "hw/vga_stream.hpp"

namespace cloudos {

struct arp_implementation {
	cloudabi_errno_t received_arp(interface*, uint8_t*, size_t) {
		get_vga_stream() << "Received ARP packet on some interface, ignoring for now\n";
		return EINVAL;
	}
};

}
