# Source: http://wiki.osdev.org/Bare_Bones

.set ALIGN,    1<<0             # align loaded modules on page boundaries
.set MEMINFO,  1<<1             # provide memory map
.set FLAGS,    ALIGN | MEMINFO  # this is the Multiboot 'flag' field
.set MAGIC,    0x1BADB002       # 'magic number' lets bootloader find the header
.set CHECKSUM, -(MAGIC + FLAGS) # checksum of above, to prove we are multiboot

.section .multiboot
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM

.section .bootstrap_stack, "aw", @nobits
stack_bottom:
.skip 16384 # 16 KiB
.global stack_top
stack_top:

.section .text
.align 4
.global _start
.type _start, @function
_start:
	cli
	movl $stack_top, %esp

	# We expect a Multiboot boot, where %eax contains the Multiboot magic
	# and %ebx a pointer to a struct with hardware information. These are
	# passed as parameters to the kernel_start C function.
	push %ebx
	push %eax
	call kernel_start

	hlt
.Lhang:
	jmp .Lhang

.size _start, . - _start
