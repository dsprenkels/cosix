#include "process_fd.hpp"
#include <oslibc/string.h>
#include <hw/vga_stream.hpp>
#include <net/elfrun.hpp> /* for elf_endian */
#include <memory/allocator.hpp>
#include <memory/page_allocator.hpp>
#include <global.hpp>
#include <fd/vga_fd.hpp>
#include <fd/memory_fd.hpp>
#include <fd/procfs.hpp>
#include <fd/bootfs.hpp>
#include <fd/pseudo_fd.hpp>
#include <fd/socket_fd.hpp>
#include <fd/scheduler.hpp>
#include <userland/vdso_support.h>
#include <elf.h>

using namespace cloudos;

process_fd::process_fd(const char *n)
: fd_t(CLOUDABI_FILETYPE_PROCESS, n)
, threads(nullptr)
{
	page_allocation p;
	auto res = get_page_allocator()->allocate(&p);
	if(res != error_t::no_error) {
		kernel_panic("Failed to allocate process paging directory");
	}
	page_directory = reinterpret_cast<uint32_t*>(p.address);
	memset(page_directory, 0, PAGE_DIRECTORY_SIZE * sizeof(uint32_t));
	get_page_allocator()->fill_kernel_pages(page_directory);

	res = get_page_allocator()->allocate(&p);
	if(res != error_t::no_error) {
		kernel_panic("Failed to allocate page table list");
	}
	page_tables = reinterpret_cast<uint32_t**>(p.address);
	for(size_t i = 0; i < 0x300; ++i) {
		page_tables[i] = nullptr;
	}
}

void process_fd::add_initial_fds() {
	vga_fd *fd = get_allocator()->allocate<vga_fd>();
	new (fd) vga_fd("vga_fd");
	add_fd(fd, CLOUDABI_RIGHT_FD_WRITE);

	char *fd_buf = get_allocator()->allocate<char>(200);
	strncpy(fd_buf, "These are the contents of my buffer!\n", 200);

	memory_fd *fd2 = get_allocator()->allocate<memory_fd>();
	new (fd2) memory_fd(fd_buf, strlen(fd_buf) + 1, "memory_fd");
	add_fd(fd2, CLOUDABI_RIGHT_FD_READ);

	add_fd(procfs::get_root_fd(), CLOUDABI_RIGHT_FILE_OPEN, CLOUDABI_RIGHT_FD_READ | CLOUDABI_RIGHT_FILE_OPEN);

	add_fd(bootfs::get_root_fd(), CLOUDABI_RIGHT_FILE_OPEN, CLOUDABI_RIGHT_FD_READ | CLOUDABI_RIGHT_FILE_OPEN | CLOUDABI_RIGHT_PROC_EXEC);

	socket_fd *my_reverse, *their_reverse;
	socket_fd::socketpair(&my_reverse, &their_reverse, 1024);
	add_fd(their_reverse, CLOUDABI_RIGHT_FD_READ | CLOUDABI_RIGHT_FD_WRITE);

	pseudo_fd *pseudo = get_allocator()->allocate<pseudo_fd>();
	new (pseudo) pseudo_fd(0, my_reverse, CLOUDABI_FILETYPE_DIRECTORY, "toplevel pseudo fd");
	add_fd(pseudo,
		/* base rights */
		CLOUDABI_RIGHT_FILE_CREATE_DIRECTORY |
		CLOUDABI_RIGHT_FILE_CREATE_FILE |
		CLOUDABI_RIGHT_FILE_CREATE_FIFO |
		CLOUDABI_RIGHT_FILE_LINK_SOURCE |
		CLOUDABI_RIGHT_FILE_LINK_TARGET |
		CLOUDABI_RIGHT_FILE_OPEN |
		CLOUDABI_RIGHT_FILE_READDIR,
		/* inherited rights */
		CLOUDABI_RIGHT_FILE_CREATE_DIRECTORY |
		CLOUDABI_RIGHT_FILE_CREATE_FILE |
		CLOUDABI_RIGHT_FILE_CREATE_FIFO |
		CLOUDABI_RIGHT_FILE_LINK_SOURCE |
		CLOUDABI_RIGHT_FILE_LINK_TARGET |
		CLOUDABI_RIGHT_FILE_OPEN |
		CLOUDABI_RIGHT_FD_READ |
		CLOUDABI_RIGHT_FD_SEEK |
		CLOUDABI_RIGHT_FD_WRITE |
		CLOUDABI_RIGHT_PROC_EXEC |
		CLOUDABI_RIGHT_FILE_READDIR);
}

int process_fd::add_fd(fd_t *fd, cloudabi_rights_t rights_base, cloudabi_rights_t rights_inheriting) {
	if(last_fd >= MAX_FD - 1) {
		// TODO: instead of keeping a last_fd counter, put mappings
		// into a freelist when they are closed, and allow them to be
		// reused. Then, return an error when there is no more free
		// space for fd's.
		kernel_panic("fd's expired for process");
	}
	fd_mapping_t *mapping = get_allocator()->allocate<fd_mapping_t>();
	mapping->fd = fd;
	mapping->rights_base = rights_base;
	mapping->rights_inheriting = rights_inheriting;

	int fdnum = ++last_fd;
	fds[fdnum] = mapping;
	return fdnum;
}

error_t process_fd::get_fd(fd_mapping_t **r_mapping, size_t num, cloudabi_rights_t has_rights) {
	*r_mapping = 0;
	if(num >= MAX_FD) {
		get_vga_stream() << "fdnum " << num << " is too high for an fd\n";
		return error_t::resource_exhausted;
	}
	fd_mapping_t *mapping = fds[num];
	if(mapping == 0 || mapping->fd == 0) {
		get_vga_stream() << "fdnum " << num << " is not a valid fd\n";
		return error_t::invalid_argument;
	}
	if((mapping->rights_base & has_rights) != has_rights) {
		get_vga_stream() << "get_fd: fd " << num << " has insufficient rights 0x" << hex << has_rights << dec << "\n";
		return error_t::not_capable;
	}
	*r_mapping = mapping;
	return error_t::no_error;
}

error_t process_fd::close_fd(size_t num) {
	fd_mapping_t *mapping;
	auto res = get_fd(&mapping, num, 0);
	if(res != error_t::no_error) {
		return res;
	}
	mapping->fd = 0;
	fds[num] = 0;
	return error_t::no_error;
}

uint32_t *process_fd::get_page_table(int i) {
	if(i >= 0x300) {
		kernel_panic("process_fd::get_page_table() cannot answer for kernel pages");
	}
	if(page_directory[i] & 0x1 /* present */) {
		return page_tables[i];
	} else {
		return nullptr;
	}
}

uint32_t *process_fd::ensure_get_page_table(int i) {
	if(i >= 0x300) {
		kernel_panic("process_fd::ensure_page_table() cannot answer for kernel pages");
	}
	if(page_directory[i] & 0x1 /* present */) {
		return page_tables[i];
	}

	// allocate page table
	page_allocation p;
	auto res = get_page_allocator()->allocate(&p);
	if(res != error_t::no_error) {
		kernel_panic("Failed to allocate paging table");
	}

	auto address = get_page_allocator()->to_physical_address(p.address);
	if((reinterpret_cast<uint32_t>(address) & 0xfff) != 0) {
		kernel_panic("physically allocated memory is not page-aligned");
	}

	page_directory[i] = reinterpret_cast<uint64_t>(address) | 0x07;
	page_tables[i] = reinterpret_cast<uint32_t*>(p.address);
	return page_tables[i];
}

void process_fd::install_page_directory() {
	/* some sanity checks to warn early if the page directory looks incorrect */
	if(get_page_allocator()->to_physical_address(this, reinterpret_cast<void*>(0xc00b8000)) != reinterpret_cast<void*>(0xb8000)) {
		kernel_panic("Failed to map VGA page, VGA stream will fail later");
	}
	if(get_page_allocator()->to_physical_address(this, reinterpret_cast<void*>(0xc01031c6)) != reinterpret_cast<void*>(0x1031c6)) {
		kernel_panic("Kernel will fail to execute");
	}

#ifndef TESTING_ENABLED
	auto page_phys_address = get_page_allocator()->to_physical_address(&page_directory[0]);
	if((reinterpret_cast<uint32_t>(page_phys_address) & 0xfff) != 0) {
		kernel_panic("Physically allocated memory is not page-aligned");
	}
	// Set the paging directory in cr3
	asm volatile("mov %0, %%cr3" : : "a"(reinterpret_cast<uint32_t>(page_phys_address)) : "memory");

	// Turn on paging in cr0
	int cr0;
	asm volatile("mov %%cr0, %0" : "=a"(cr0));
	cr0 |= 0x80000000;
	asm volatile("mov %0, %%cr0" : : "a"(cr0) : "memory");
#endif
}

error_t process_fd::add_mem_mapping(mem_mapping_t *mapping, bool overwrite)
{
	mem_mapping_list *covering_mappings = nullptr;
	remove_all(&mappings, [&](mem_mapping_list *item) {
		bool covers = item->data->covers(mapping->virtual_address);
		if(covers && !overwrite) {
			get_vga_stream() << "Trying to create a " << mapping->number_of_pages << "-page mapping at address " << mapping->virtual_address << "\n";
			get_vga_stream() << "Found a " << item->data->number_of_pages << "-page mapping at address " << item->data->virtual_address << "\n";
			kernel_panic("add_mem_mapping(mapping, false) called for a mapping that overlaps with an existing one");
		}
		return covers;
	}, [&](mem_mapping_list *item) {
		append(&covering_mappings, item);
	});

	if(covering_mappings != nullptr) {
		// - split the old mappings in covered and uncovered parts
		// - unmap the covered parts
		// - add the covered parts back into the mappings list
		kernel_panic("TODO: implement solving covered mappings in add_mem_mapping");
	}

	mem_mapping_list *entry = get_allocator()->allocate<mem_mapping_list>();
	entry->data = mapping;
	entry->next = nullptr;
	append(&mappings, entry);
	return error_t::no_error;

	// the page tables already contain all zeroes for this mapping. when we page
	// fault for the first time, or ensure_backed() is called on the mapping,
	// we will allocate physical pages and alter the page table.
}

void process_fd::mem_unmap(void *addr, size_t )
{
	// TODO: find all mappings within this range
	// - if they are fully covered, unmap them
	// - if they are partially covered, split them and unmap the covered part
	// - for now, assume that we only unmap full mappings
	mem_mapping_list *remove_mappings = nullptr;
	remove_all(&mappings, [&](mem_mapping_list *item) {
		return item->data->virtual_address == addr;
	}, [&](mem_mapping_list *item) {
		append(&remove_mappings, item);
	});
	iterate(remove_mappings, [&](mem_mapping_list *item) {
		item->data->unmap_completely();
	});
}

void *process_fd::find_free_virtual_range(size_t num_pages)
{
	uint32_t address = 0x90000000;
	while(address + num_pages * PAGE_SIZE < 0xc0000000) {
		// - find the first lowest map after address
		mem_mapping_t *lowest = nullptr;
		iterate(mappings, [&](mem_mapping_list *item) {
			uint32_t virtual_address = reinterpret_cast<uint32_t>(item->data->virtual_address);
			if(virtual_address + item->data->number_of_pages * PAGE_SIZE > address) {
				if(!lowest || lowest->virtual_address > item->data->virtual_address) {
					lowest = item->data;
				}
			}
		});

		if(lowest == nullptr) {
			// No mappings yet
			return reinterpret_cast<void*>(address);
		}

		uint32_t virtual_address = reinterpret_cast<uint32_t>(lowest->virtual_address);
		if(address + num_pages * PAGE_SIZE <= virtual_address) {
			return reinterpret_cast<void*>(address);
		}

		address = virtual_address + lowest->number_of_pages * PAGE_SIZE;
	}
	return nullptr;
}

error_t process_fd::exec(fd_t *fd, size_t fdslen, fd_mapping_t **new_fds, void const *argdata, size_t argdatalen) {
	// read from this fd until it gives EOF, then exec(buf, buf_size)
	// TODO: once all fds implement seek(), we can read() only the header,
	// then seek to the phdr offset, then read() phdrs, then for every LOAD
	// phdr, seek() to the binary data and read() only that into the
	// process address space
	uint8_t *elf_buffer = get_allocator()->allocate<uint8_t>(10 * 1024 * 1024);
	size_t buffer_size = 0;
	do {
		size_t read = fd->read(buffer_size, &elf_buffer[buffer_size], 1024);
		buffer_size += read;
		if(read == 0) {
			break;
		}
	} while(fd->error == error_t::no_error);

	if(fd->error != error_t::no_error) {
		return fd->error;
	}

	uint8_t *argdata_buffer = get_allocator()->allocate<uint8_t>(argdatalen);
	memcpy(argdata_buffer, argdata, argdatalen);

	char old_name[sizeof(name)];
	strncpy(old_name, name, sizeof(name));
	uint32_t *old_page_directory = page_directory;
	uint32_t **old_page_tables = page_tables;
	mem_mapping_list *old_mappings = mappings;

	strncpy(name, "exec<-", sizeof(name));
	strncat(name, fd->name, sizeof(name) - strlen(name) - 1);

	page_allocation p;
	auto res = get_page_allocator()->allocate(&p);
	if(res != error_t::no_error) {
		kernel_panic("Failed to allocate process paging directory");
	}
	page_directory = reinterpret_cast<uint32_t*>(p.address);
	memset(page_directory, 0, PAGE_DIRECTORY_SIZE * sizeof(uint32_t));

	get_page_allocator()->fill_kernel_pages(page_directory);
	res = get_page_allocator()->allocate(&p);
	if(res != error_t::no_error) {
		kernel_panic("Failed to allocate page table list");
	}
	page_tables = reinterpret_cast<uint32_t**>(p.address);
	for(size_t i = 0; i < 0x300; ++i) {
		page_tables[i] = nullptr;
	}
	mappings = nullptr;
	install_page_directory();

	uint8_t *argdata_address = reinterpret_cast<uint8_t*>(0x80100000);
	mem_mapping_t *argdata_mapping = get_allocator()->allocate<mem_mapping_t>();
	new (argdata_mapping) mem_mapping_t(this, argdata_address, len_to_pages(argdatalen), NULL, 0, CLOUDABI_PROT_READ | CLOUDABI_PROT_WRITE);
	add_mem_mapping(argdata_mapping);
	argdata_mapping->ensure_completely_backed();
	memcpy(argdata_address, argdata_buffer, argdatalen);

	res = exec(elf_buffer, buffer_size, argdata_address, argdatalen);
	if(res != error_t::no_error) {
		page_directory = old_page_directory;
		page_tables = old_page_tables;
		mappings = old_mappings;
		strncpy(name, old_name, sizeof(name));
		install_page_directory();
		return res;
	}

	// Close all unused FDs
	for(int i = 0; i <= last_fd; ++i) {
		bool fd_is_used = false;
		for(size_t j = 0; j < fdslen; ++j) {
			// TODO: cloudabi does not allow an fd to be mapped twice in exec()
			if(new_fds[j]->fd == fds[i]->fd) {
				fd_is_used = true;
			}
		}
		if(!fd_is_used) {
			// TODO: actually close
			fds[i]->fd = nullptr;
		}
	}

	for(size_t i = 0; i < fdslen; ++i) {
		fds[i] = new_fds[i];
	}
	last_fd = fdslen - 1;

	// now, when process is scheduled again, we will return to the entrypoint of the new binary
	return error_t::no_error;
}

error_t process_fd::exec(uint8_t *buffer, size_t buffer_size, uint8_t *argdata, size_t argdatalen) {
	if(buffer_size < sizeof(Elf32_Ehdr)) {
		// Binary too small
		return error_t::exec_format;
	}

	Elf32_Ehdr *header = reinterpret_cast<Elf32_Ehdr*>(buffer);
	if(memcmp(header->e_ident, "\x7F" "ELF", 4) != 0) {
		// Not an ELF binary
		return error_t::exec_format;
	}

	if(header->e_ident[EI_CLASS] != ELFCLASS32) {
		// Not a 32-bit ELF binary
		return error_t::exec_format;
	}

	if(header->e_ident[EI_DATA] != ELFDATA2LSB) {
		// Not least-significant byte first, unsupported at the moment
		return error_t::exec_format;
	}

	if(header->e_ident[EI_VERSION] != 1) {
		// Not ELF version 1
		return error_t::exec_format;
	}

	if(header->e_ident[EI_OSABI] != ELFOSABI_CLOUDABI
	|| header->e_ident[EI_ABIVERSION] != 0) {
		// Not CloudABI v0
		return error_t::exec_format;
	}

	if(header->e_type != ET_EXEC && header->e_type != ET_DYN) {
		// Not an executable or shared object file
		// (CloudABI binaries can be shipped as shared object files,
		// which are actually executables, so that the kernel knows it
		// can map them anywhere in address space for ASLR)
		return error_t::exec_format;
	}

	if(header->e_machine != EM_386) {
		// TODO: when we support different machine types, check that
		// header->e_machine is supported.
		return error_t::exec_format;
	}

	if(header->e_version != EV_CURRENT) {
		// Not a current version ELF
		return error_t::exec_format;
	}

	// Save the phdrs
	size_t elf_phnum = header->e_phnum;
	size_t elf_ph_size = header->e_phentsize * elf_phnum;

	if(header->e_phoff >= buffer_size || (header->e_phoff + elf_ph_size) >= buffer_size) {
		// Phdrs weren't shipped in this ELF
		return error_t::exec_format;
	}

	void *elf_phdr = reinterpret_cast<uint8_t*>(0x80060000);
	mem_mapping_t *phdr_mapping = get_allocator()->allocate<mem_mapping_t>();
	new (phdr_mapping) mem_mapping_t(this, elf_phdr, len_to_pages(elf_ph_size), NULL, 0, CLOUDABI_PROT_READ | CLOUDABI_PROT_WRITE);
	add_mem_mapping(phdr_mapping);
	phdr_mapping->ensure_completely_backed();

	memcpy(elf_phdr, buffer + header->e_phoff, elf_ph_size);

	// Map the LOAD sections
	for(size_t phi = 0; phi < elf_phnum; ++phi) {
		size_t offset = header->e_phoff + phi * header->e_phentsize;
		if(offset >= buffer_size || (offset + sizeof(Elf32_Phdr)) >= buffer_size) {
			// Phdr wasn't shipped in this ELF
			return error_t::exec_format;
		}

		Elf32_Phdr *phdr = reinterpret_cast<Elf32_Phdr*>(buffer + offset);

		if(phdr->p_type == PT_LOAD) {
			if(phdr->p_offset >= buffer_size || (phdr->p_offset + phdr->p_filesz) >= buffer_size) {
				// Phdr data wasn't shipped in this ELF
				return error_t::exec_format;
			}
			if((phdr->p_vaddr % PAGE_SIZE) != 0) {
				// Phdr load section wasn't aligned
				return error_t::exec_format;
			}
			uint8_t *vaddr = reinterpret_cast<uint8_t*>(phdr->p_vaddr);
			uint8_t *code_offset = buffer + phdr->p_offset;
			mem_mapping_t *t = get_allocator()->allocate<mem_mapping_t>();
			new (t) mem_mapping_t(this, vaddr, len_to_pages(phdr->p_memsz), NULL, 0, CLOUDABI_PROT_EXEC | CLOUDABI_PROT_READ);
			add_mem_mapping(t);
			t->ensure_completely_backed();
			memcpy(vaddr, code_offset, phdr->p_filesz);
			memset(vaddr + phdr->p_filesz, 0, phdr->p_memsz - phdr->p_filesz);
		}
	}

	// initialize vdso address
	size_t vdso_size = vdso_blob_size;
	uint8_t *vdso_address = reinterpret_cast<uint8_t*>(0x80040000);
	mem_mapping_t *vdso_mapping = get_allocator()->allocate<mem_mapping_t>();
	new (vdso_mapping) mem_mapping_t(this, vdso_address, len_to_pages(vdso_size), NULL, 0, CLOUDABI_PROT_READ | CLOUDABI_PROT_WRITE);
	add_mem_mapping(vdso_mapping);
	vdso_mapping->ensure_completely_backed();
	memcpy(vdso_address, vdso_blob, vdso_size);

	// initialize auxv
	size_t auxv_entries = 8; // including CLOUDABI_AT_NULL
	size_t auxv_size = auxv_entries * sizeof(cloudabi_auxv_t);
	uint8_t *auxv_address = reinterpret_cast<uint8_t*>(0x80010000);
	mem_mapping_t *auxv_mapping = get_allocator()->allocate<mem_mapping_t>();
	new (auxv_mapping) mem_mapping_t(this, auxv_address, len_to_pages(auxv_size), NULL, 0, CLOUDABI_PROT_READ | CLOUDABI_PROT_WRITE);
	add_mem_mapping(auxv_mapping);
	auxv_mapping->ensure_completely_backed();
	
	cloudabi_auxv_t *auxv = reinterpret_cast<cloudabi_auxv_t*>(auxv_address);
	auxv->a_type = CLOUDABI_AT_ARGDATA;
	auxv->a_ptr = argdata;
	auxv++;
	auxv->a_type = CLOUDABI_AT_ARGDATALEN;
	auxv->a_val = argdatalen;
	auxv++;
	auxv->a_type = CLOUDABI_AT_BASE;
	auxv->a_ptr = nullptr; /* because we don't do address randomization */
	auxv++;
	auxv->a_type = CLOUDABI_AT_PAGESZ;
	auxv->a_val = PAGE_SIZE;
	auxv++;
	auxv->a_type = CLOUDABI_AT_SYSINFO_EHDR;
	auxv->a_ptr = vdso_address;
	auxv++;
	auxv->a_type = CLOUDABI_AT_PHDR;
	auxv->a_ptr = elf_phdr;
	auxv++;
	auxv->a_type = CLOUDABI_AT_PHNUM;
	auxv->a_val = elf_phnum;
	auxv++;
	auxv->a_type = CLOUDABI_AT_NULL;

	// remove all existing threads
	auto scheduler = get_scheduler();
	iterate(threads, [&](thread_list *item) {
		scheduler->thread_exiting(item->data);
		item->data->thread_exit();
	});
	threads = nullptr;
	last_thread = MAIN_THREAD - 1;

	// set running state
	running = true;
	exitcode = 0;
	exitsignal = 0;

	// create the initial stack
	size_t userland_stack_size = 0x10000 /* 64 kb */;
	uint8_t *userland_stack_top = reinterpret_cast<uint8_t*>(0x80000000);
	uint8_t *userland_stack_bottom = userland_stack_top - userland_stack_size;
	mem_mapping_t *stack_mapping = get_allocator()->allocate<mem_mapping_t>();
	new (stack_mapping) mem_mapping_t(this, userland_stack_bottom, len_to_pages(userland_stack_size), NULL, 0, CLOUDABI_PROT_READ | CLOUDABI_PROT_WRITE);
	add_mem_mapping(stack_mapping);
	stack_mapping->ensure_completely_backed();

	// create the main thread
	add_thread(userland_stack_top, auxv_address, reinterpret_cast<void*>(header->e_entry));

	return error_t::no_error;
}

void process_fd::fork(thread *otherthread) {
	process_fd *otherprocess = otherthread->get_process();

	if(threads != nullptr) {
		kernel_panic("Cannot call fork() on a process_fd that already has threads");
	}

	strncpy(name, otherprocess->name, sizeof(name));
	strncat(name, "->forked", sizeof(name) - strlen(name) - 1);

	thread *mainthread = reinterpret_cast<thread*>(get_allocator()->allocate_aligned(sizeof(thread), 16));
	new (mainthread) thread(this, otherthread);

	running = true;
	if(!otherprocess->running) {
		kernel_panic("Forked from a process that wasn't running");
	}

	// dup all fd's
	last_fd = otherprocess->last_fd;
	for(int i = 0; i <= last_fd; ++i) {
		fd_mapping_t *old_mapping = otherprocess->fds[i];

		fd_mapping_t *mapping = get_allocator()->allocate<fd_mapping_t>();
		mapping->fd = old_mapping->fd;
		mapping->rights_base = old_mapping->rights_base;
		mapping->rights_inheriting = old_mapping->rights_inheriting;
		fds[i] = mapping;
	}

	iterate(otherprocess->mappings, [&](mem_mapping_list *item) {
		mem_mapping_t *mapping = get_allocator()->allocate<mem_mapping_t>();
		new (mapping) mem_mapping_t(this, item->data);
		add_mem_mapping(mapping);

		mem_mapping_list *ml = get_allocator()->allocate<mem_mapping_list>();
		ml->data = mapping;
		ml->next = nullptr;

		append(&mappings, ml);

		// TODO: implement copy-on-write
		mapping->copy_from(item->data);
	});

	add_thread(mainthread);
}

void process_fd::add_thread(thread *thr)
{
	auto item = get_allocator()->allocate<thread_list>();
	item->data = thr;
	item->next = nullptr;
	append(&threads, item);
	get_scheduler()->thread_ready(thr);
}

thread *process_fd::add_thread(void *stack_address, void *auxv_address, void *entrypoint)
{
	if(!running) {
		kernel_panic("add_thread on a process that is dead");
	}
	thread *thr = reinterpret_cast<thread*>(get_allocator()->allocate_aligned(sizeof(thread), 16));
	new (thr) thread(this, stack_address, auxv_address, entrypoint, ++last_thread);
	add_thread(thr);
	return thr;
}

void process_fd::exit(cloudabi_exitcode_t c, cloudabi_signal_t s)
{
	if(this == global_state_->init) {
		get_vga_stream() << "init exited with signal " << s << ", exit code " << c << "\n";
		kernel_panic("init exited");
	}
	running = false;
	exitsignal = s;
	if(exitsignal == 0) {
		exitcode = c;
	} else {
		exitcode = 0;
	}

	get_vga_stream() << "Process \"" << name << "\" exited with signal " << exitsignal << ", code " << exitcode << ".\n";

	// TODO: close all file descriptors (this also kills sub-processes)
	// TODO: clean up all memory maps
	// TODO: free all allocations

	// unschedule all threads
	auto scheduler = get_scheduler();
	iterate(threads, [&](thread_list *item) {
		scheduler->thread_exiting(item->data);
		item->data->thread_exit();
	});
	threads = nullptr;

	// now yield, so we can schedule a ready thread
	scheduler->thread_yield();
}

void process_fd::signal(cloudabi_signal_t s)
{
	switch(s) {
	case CLOUDABI_SIGABRT:
	case CLOUDABI_SIGALRM:
	case CLOUDABI_SIGBUS:
	case CLOUDABI_SIGFPE:
	case CLOUDABI_SIGHUP:
	case CLOUDABI_SIGILL:
	case CLOUDABI_SIGINT:
	case CLOUDABI_SIGKILL:
	case CLOUDABI_SIGQUIT:
	case CLOUDABI_SIGSEGV:
	case CLOUDABI_SIGSYS:
	case CLOUDABI_SIGTERM:
	case CLOUDABI_SIGTRAP:
	case CLOUDABI_SIGUSR1:
	case CLOUDABI_SIGUSR2:
	case CLOUDABI_SIGVTALRM:
	case CLOUDABI_SIGXCPU:
	case CLOUDABI_SIGXFSZ:
		exit(0, s);
		return;
	default:
		// Signals cannot be handled in CloudABI, so signals either
		// kill the process, or are ignored.
		;
	}
}
