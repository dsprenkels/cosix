#include "hw/net/virtio.hpp"
#include "hw/cpu_io.hpp"
#include "global.hpp"
#include "memory/allocator.hpp"
#include "memory/page_allocator.hpp"
#include "oslibc/error.h"
#include "oslibc/string.h"
#include "net/protocol_store.hpp"
#include "net/dhcp.hpp" // TODO remove

extern uint32_t _kernel_virtual_base;

using namespace cloudos;

namespace cloudos {

struct virtq_buffer {
	/* Address (guest-physical). */
	uint64_t addr;
	/* Length. */
	uint32_t len;

/* This marks a buffer as continuing via the next field. */
#define VIRTQ_DESC_F_NEXT   1
/* This marks a buffer as device write-only (otherwise device read-only). */
#define VIRTQ_DESC_F_WRITE     2
/* This means the buffer contains a list of buffer descriptors. */
//#define VIRTQ_DESC_F_INDIRECT   4
	/* The flags as indicated above. */
	uint16_t flags;
	/* Next field if flags & NEXT */
	uint16_t next;
};

struct virtq_avail {
//#define VIRTQ_AVAIL_F_NO_INTERRUPT      1
	uint16_t flags;
	uint16_t idx;
	uint16_t ring[];
	/* an additional uint16_t here if VIRTIO_F_EVENT_IDX is set */
} __attribute__((__packed__));

struct virtq_used {
//#define VIRTQ_USED_F_NO_NOTIFY  1
	uint16_t flags;
	uint16_t idx;
	struct {
		/* Index of start of used descriptor chain. */
		uint32_t id;
		/* Total length of the descriptor chain which was used (written to) */
		uint32_t len;
	} ring[];
	/* an additional uint16_t here if VIRTIO_F_EVENT_IDX is set */
} __attribute__((__packed__));

struct virtq {
	virtq(uint16_t q, uint32_t s)
	: data(0), queue_select(q), queue_size(s), padding_used(0) {
		/* Each virtq occupies two or more physically-contiguous pages (usually defined as
		   4096 bytes, but depending on the transport; henceforth referred to as Queue Align) */
		size_t iovirtq_bytes = 0;
		/* the ring buffers */
		iovirtq_bytes += sizeof(virtq_buffer) * queue_size;
		/* availability list */
		iovirtq_bytes += 2 + queue_size * 2;
		/* pad to the next page */
		padding_used = 4096 - (iovirtq_bytes % 4096);
		iovirtq_bytes += padding_used;
		/* used list */
		iovirtq_bytes += 2 + queue_size * 8;
		data = reinterpret_cast<uint8_t*>(get_allocator()->allocate_aligned(iovirtq_bytes, 4096));
		for(size_t i = 0; i < iovirtq_bytes; ++i) {
			data[i] = 0;
		}
	}

	uint16_t get_queue_select() {
		return queue_select;
	}

	uint16_t get_queue_size() {
		return queue_size;
	}

	void *get_virtq_addr_phys() {
		return get_page_allocator()->to_physical_address(data);
	}

	virtq_buffer *get_virtq_buffer(int i) {
		if(i >= queue_size) {
			kernel_panic("get_virtq_buffer: buffer index too high");
		}
		auto buf = reinterpret_cast<virtq_buffer*>(data);
		return buf + i;
	}

	virtq_avail *get_virtq_avail() {
		return reinterpret_cast<virtq_avail*>(data + sizeof(virtq_buffer) * queue_size);
	}

	uint8_t *get_padding() {
		return data + 2 + (sizeof(virtq_buffer) + 2) * queue_size;
	}

	virtq_used *get_virtq_used() {
		return reinterpret_cast<virtq_used*>(data + 2 + (sizeof(virtq_buffer) + 2) * queue_size + padding_used);
	}

private:
	uint8_t *data;
	uint16_t queue_select;
	uint16_t queue_size;
	uint32_t padding_used;
};
}

const uint64_t VIRTIO_NET_F_CSUM = 1 << 0; // Checksum offloading
const uint64_t VIRTIO_NET_F_MAC = 1 << 5; // Device has given MAC address
const uint64_t VIRTIO_NET_F_MRG_RXBUF = 1 << 15; // Guest can merge receive buffers
const uint64_t VIRTIO_NET_F_STATUS = 1 << 16; // Configuration status field is available
const uint64_t VIRTIO_F_NOTIFY_ON_EMPTY = 1 << 24;

device *virtio_net_driver::probe_pci_device(pci_bus *bus, int device)
{
	// VirtIO vendor
	if(bus->get_vendor_id(device) != 0x1af4) {
		return nullptr;
	}

	// VirtIO legacy device
	uint16_t device_id = bus->get_device_id(device);
	if(device_id < 0x1000 || device_id > 0x103f) {
		return nullptr;
	}

	// VirtIO NIC
	if(bus->get_subsystem_id(device) != 0x01) {
		return nullptr;
	}

	auto *res = get_allocator()->allocate<virtio_net_device>();
	new(res) virtio_net_device(bus, device);
	return res;
}

const char *virtio_net_driver::description()
{
	return "VirtIO legacy network card driver";
}

const char *virtio_net_device::description()
{
	return "VirtIO legacy network card";
}

virtio_net_device::virtio_net_device(pci_bus *parent, int d)
: ethernet_device(parent)
, bus(parent)
, bus_device(d)
, last_readq_used_idx(0)
, last_writeq_idx(0)
, readq(nullptr)
, writeq(nullptr)
, mappings(0)
{
}

cloudabi_errno_t virtio_net_device::add_buffer_to_avail(virtq *queue, int buffer_id) {
	auto *avail = queue->get_virtq_avail();
	avail->ring[avail->idx % queue->get_queue_size()] = buffer_id;

	// memory barrier
	asm volatile ("": : :"memory");

	avail->idx += 1;

	// memory barrier
	asm volatile ("": : :"memory");

	uint32_t bar0 = bus->get_bar0(bus_device);
	if((bar0 & 0x01) != 1) {
		get_vga_stream() << "Not I/O space BAR type, skipping.\n";
		return ENOTSUP;
	}
	bar0 = bar0 & 0xfffffffc;

	uint32_t const queue_notify = bar0 + 0x10;
	outw(queue_notify, queue->get_queue_select());

	return 0;
}

cloudabi_errno_t virtio_net_device::eth_init()
{
	uint32_t bar0 = bus->get_bar0(bus_device);
	if((bar0 & 0x01) != 1) {
		get_vga_stream() << "Not I/O space BAR type, skipping.\n";
		return ENOTSUP;
	}
	bar0 = bar0 & 0xfffffffc;
	//uint8_t int_info = read_pci_config(bus, device, 0, 0x3c) & 0xff;
	//get_vga_stream() << "Will use IRQ " << int_info << "\n";
	uint32_t mem_base = bar0;
	uint32_t const device_features = mem_base + 0x00; // 4 bytes
	uint32_t const driver_features = mem_base + 0x04; // 4 bytes
	uint32_t const queue_address = mem_base + 0x08; // 4 bytes
	uint32_t const queue_size = mem_base + 0x0c; // 2 bytes
	uint32_t const queue_select = mem_base + 0x0e; // 2 bytes
	uint32_t const device_status = mem_base + 0x12; // 1 byte

	// acknowledge device
	outb(device_status, 0); /* reset device */
	outb(device_status, 1); /* device acknowledged */
	outb(device_status, 2); /* driver available */

	uint64_t sup_features = inl(device_features);
	sup_features |= uint64_t(inl(device_features + 1)) << 32;
	get_vga_stream() << "Device features: 0x" << hex << sup_features << dec << "\n";

	if((sup_features & VIRTIO_NET_F_MAC) == 0) {
		// TODO: if the VIRTIO_NET_F_MAC feature is not in
		// device_features, we must randomly generate our own MAC
		get_vga_stream() << "MAC setting not supported. Skipping device.\n";
		outb(device_status, 128); /* driver failed */
		return ENOTSUP;
	}

	if((sup_features & VIRTIO_NET_F_STATUS) == 0) {
		get_vga_stream() << "Device status bit not supported. Skipping device.\n";
		outb(device_status, 128); /* driver failed */
		return ENOTSUP;
	}

	if((sup_features & VIRTIO_NET_F_MRG_RXBUF) == 0) {
		get_vga_stream() << "Merging received buffers not supported. Skipping device.\n";
		outb(device_status, 128); /* driver failed */
		return ENOTSUP;
	}

	if((sup_features & VIRTIO_F_NOTIFY_ON_EMPTY) == 0) {
		get_vga_stream() << "Notify on empty not supported. Skipping device.\n";
		outb(device_status, 128); /* driver failed */
		return ENOTSUP;
	}

	uint64_t drv_features = sup_features & (VIRTIO_NET_F_MAC
			| VIRTIO_NET_F_STATUS
			| VIRTIO_NET_F_CSUM
			| VIRTIO_NET_F_MRG_RXBUF
			| VIRTIO_F_NOTIFY_ON_EMPTY);
	outl(driver_features, drv_features & 0xffffffff);
	outl(driver_features + 1, drv_features >> 32);
	get_vga_stream() << "Driver features: 0x" << hex << inl(driver_features) << dec << "\n";

	mac[0] = inb(mem_base + 0x14);
	mac[1] = inb(mem_base + 0x15);
	mac[2] = inb(mem_base + 0x16);
	mac[3] = inb(mem_base + 0x17);
	mac[4] = inb(mem_base + 0x18);
	mac[5] = inb(mem_base + 0x19);

	get_vga_stream() << "MAC address: " << hex << mac[0] << ":" << mac[1] << ":" << mac[2] << ":" << mac[3] << ":" << mac[4] << ":" << mac[5] << "\n";

	for(int queue = 0; queue < 2; ++queue) {
		outw(queue_select, queue);
		auto number_of_entries = inw(queue_size);

		virtq *q = get_allocator()->allocate<virtq>();
		new(q) virtq(queue, number_of_entries);

		uint64_t virtq_addr_phys = reinterpret_cast<uint64_t>(q->get_virtq_addr_phys());
		if((virtq_addr_phys % 4096) != 0) {
			get_vga_stream() << "Failed to allocate descriptor table aligned to 4096 bytes\n";
			return ENOMEM;
		}
		outl(queue_address, virtq_addr_phys >> 12);
		(queue == 0 ? readq : writeq) = q;
	}

	// make some buffers available -- the more buffers we allocate,
	// the more data the virtio NIC can feed us between processing, but the
	// more memory we're using.
	auto avail = readq->get_virtq_avail();
	avail->flags = 0;
	for(int i = 0; i < 20; ++i) {
		auto buffer = readq->get_virtq_buffer(i);

		// TODO: use MTU instead of fixed size
		buffer->len = 2048;
		void *address = get_allocator()->allocate(buffer->len);
		buffer->addr = reinterpret_cast<uint64_t>(get_page_allocator()->to_physical_address(address));
		buffer->flags = VIRTQ_DESC_F_WRITE;
		buffer->next = 0;

		address_mapping *mapping = get_allocator()->allocate<address_mapping>();
		address_mapping_list *mappingl = get_allocator()->allocate<address_mapping_list>();
		mapping->logical = address;
		mapping->physical = reinterpret_cast<void*>(buffer->addr);
		mappingl->data = mapping;
		mappingl->next = nullptr;
		append(&mappings, mappingl);

		auto res = add_buffer_to_avail(readq, i);
		if(res != 0) {
			kernel_panic("Failed to add initial buffers to virtio NIC");
		}
	}

	outb(device_status, 4); /* driver ready */

	// TODO: make something other than the ethernet_device responsible for starting
	// the DHCP client
	return get_protocol_store()->dhcp->start_dhcp_discover_for(get_interface());
}

cloudabi_errno_t virtio_net_device::check_new_packets() {
	if(readq == nullptr) {
		return EINVAL;
	}

	auto *virtq_used = readq->get_virtq_used();
	while(virtq_used->idx > last_readq_used_idx) {
		uint32_t buffer_id = virtq_used->ring[last_readq_used_idx % readq->get_queue_size()].id;
		uint32_t length = virtq_used->ring[last_readq_used_idx % readq->get_queue_size()].len;
		auto *buffer = readq->get_virtq_buffer(buffer_id);

		uint32_t nethdr_phys = buffer->addr & 0xffffffff;

		address_mapping_list *found = find(mappings, [nethdr_phys](address_mapping_list *item) {
			return reinterpret_cast<uint32_t>(item->data->physical) == nethdr_phys;
		});
		if(found == nullptr) {
			kernel_panic("Did not find logical address for physical kernel address of nethdr");
		}
		uint8_t *nethdr = reinterpret_cast<uint8_t*>(found->data->logical);
		if(reinterpret_cast<uint32_t>(nethdr) < _kernel_virtual_base) {
			kernel_panic("Virtio frame ptr is too low");
		}
		auto res = ethernet_frame_received(nethdr + 12, length - 12);

		last_readq_used_idx++;
		auto res2 = add_buffer_to_avail(readq, buffer_id);

		if(res != 0) {
			return res;
		}
		if(res2 != 0) {
			return res2;
		}
	}

	return 0;
}

cloudabi_errno_t virtio_net_device::get_mac_address(char m[6]) {
	if(readq == nullptr) {
		return EINVAL;
	}

	memcpy(m, mac, 6);
	return 0;
}

cloudabi_errno_t virtio_net_device::send_ethernet_frame(uint8_t *frame, size_t length) {
	uint8_t net_hdr[] = {
		0x01, /* needs checksum */
		0x00, /* no segmentation */
		0x00, 0x00, /* header length */
		0x00, 0x00, /* segment size */
		0x00, 0x00, /* checksum start */
		0x00, 0x00, /* checksum offset */
		0x00, 0x00, /* buffer count */
	};

	size_t first_desc = last_writeq_idx++;
	size_t second_desc = last_writeq_idx++;

	if(last_writeq_idx >= writeq->get_queue_size()) {
		// TODO: instead of adding our buffers to the virtio NIC constantly,
		// allocate some actual write buffers in the constructor and cycle
		// through their indices in the writeq like we do for the readq
		// --- or, alternatively, reuse buffer indices from earlier sent
		// packets that aren't used anymore, so we don't have to copy into
		// send buffers.
		kernel_panic("Trying to write more buffers to virtio device than fit in the queue, fix the virtio driver");
	}

	auto d0 = writeq->get_virtq_buffer(first_desc);
	auto d1 = writeq->get_virtq_buffer(second_desc);

	address_mapping *mapping0 = get_allocator()->allocate<address_mapping>();
	address_mapping_list *mappingl0 = get_allocator()->allocate<address_mapping_list>();
	address_mapping *mapping1 = get_allocator()->allocate<address_mapping>();
	address_mapping_list *mappingl1 = get_allocator()->allocate<address_mapping_list>();
	mapping0->logical = net_hdr;
	mapping0->physical = get_page_allocator()->to_physical_address(mapping0->logical);
	mappingl0->data = mapping0;
	mappingl0->next = mappingl1;
	mapping1->logical = frame;
	mapping1->physical = get_page_allocator()->to_physical_address(mapping1->logical);
	mappingl1->data = mapping1;
	mappingl1->next = nullptr;
	append(&mappings, mappingl0);

	d0->addr = reinterpret_cast<uint64_t>(mapping0->physical);
	d0->len = sizeof(net_hdr);
	d0->flags = VIRTQ_DESC_F_NEXT;
	d0->next = second_desc;

	d1->addr = reinterpret_cast<uint64_t>(mapping1->physical);
	d1->len = length;
	d1->flags = 0;
	d1->next = 0;

	auto avail = writeq->get_virtq_avail();
	avail->flags = 0;
	return add_buffer_to_avail(writeq, first_desc);
}

void virtio_net_device::timer_event() {
	// TODO: do this when the correct interrupt arrives, instead of
	// when the timer fires
	auto res = check_new_packets();
	if(res != 0) {
		get_vga_stream() << "check_new_packets failed: " << res << "\n";
	}
}
