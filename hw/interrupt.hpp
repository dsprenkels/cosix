#pragma once

#include <stddef.h>
#include <stdint.h>
#include "interrupt_table.hpp"

namespace cloudos {

struct interrupt_state_t {
	// ds is the segment selector for the data, code, etc. registers; fs is
	// the selector for the fs and gs registers
	uint32_t ds, fs;
	uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
	uint32_t int_no, err_code;
	uint32_t eip, cs, eflags, useresp, ss;
};

struct irq_handler {
	virtual ~irq_handler();
	void register_irq(uint8_t irq);

	virtual void handle_irq(uint8_t irq) = 0;
};

struct interrupt_handler {
	interrupt_handler();

	void reprogram_pic();
	void setup(interrupt_table&);
	void enable_interrupts();
	void disable_interrupts();

	void register_irq_handler(uint8_t irq, irq_handler *handler);

	void handle(interrupt_state_t*);
	void handle_irq(uint8_t irq);

private:
	irq_handler *irq_handlers[0x10];
};

}
