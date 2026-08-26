/* sysmem.c — _sbrk for newlib-nano.
 *
 * Nothing in this firmware calls malloc on purpose; FreeRTOS (task group 5)
 * uses its own heap.  This exists so that if a library path ever does, it
 * fails loudly at the linker-defined limit rather than walking into the stack.
 */
#include <errno.h>
#include <stdint.h>

extern uint8_t _end;      /* set by the linker: first free byte after .bss */
extern uint8_t _estack;
extern uint32_t _Min_Stack_Size;

void *_sbrk(ptrdiff_t incr)
{
	static uint8_t *brk;
	const uint8_t *limit = &_estack - (uintptr_t)&_Min_Stack_Size;
	uint8_t *prev;

	if (brk == 0)
		brk = &_end;

	if (brk + incr > limit) {
		errno = ENOMEM;
		return (void *)-1;
	}
	prev = brk;
	brk += incr;
	return prev;
}
