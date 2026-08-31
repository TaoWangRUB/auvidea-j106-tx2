/* stm32f4xx_it.c — exception and interrupt handlers.
 *
 * The pre-restructure build trapped every vector but Reset and said so:
 * "there is no ISR to get wrong".  That property is gone (design D13).  What
 * remains true, and is the invariant the restructure rests on, is that no
 * handler here participates in generating the trigger waveform — TIM1 emits
 * edges in hardware.  These ISRs only count pulses and move bytes.
 *
 * SVC_Handler, PendSV_Handler and SysTick_Handler are deliberately absent:
 * FreeRTOSConfig.h maps them onto the kernel's own implementations, and the
 * HAL tick lives on TIM6 instead (design D6).
 */
#include "main.h"
#include "camtrig.h"
#include "FreeRTOS.h"
#include "task.h"

extern UART_HandleTypeDef huart1;
extern TIM_HandleTypeDef  htim2;
extern PCD_HandleTypeDef  hpcd_USB_OTG_FS;

void camtrig_hal_tick_irq(void);

/* ------------------------------------------------------------------ *
 * Faults — signal which one, rather than freezing silently.
 *
 * These were bare `for (;;)` loops.  That is actively harmful on a board with
 * no debugger: a fault froze the LED at whatever state the heartbeat last left
 * it, which is indistinguishable from a deadlock and says nothing about the
 * cause.  Each now blinks its own code, continuing the panic_blink numbering
 * the RTOS hooks use (1-5).
 * ------------------------------------------------------------------ */
void HardFault_Handler(void)  { panic_blink(6);  }
void MemManage_Handler(void)  { panic_blink(7);  }
void BusFault_Handler(void)   { panic_blink(8);  }
void UsageFault_Handler(void) { panic_blink(9);  }
void NMI_Handler(void)        { panic_blink(10); }
void DebugMon_Handler(void)   { }

/* ------------------------------------------------------------------ *
 * HAL tick (TIM6) — priority 15, below configMAX_SYSCALL (5).  It calls no
 * FreeRTOS API, so its priority is unconstrained; keeping it lowest means it
 * can never delay the trigger bookkeeping.
 * ------------------------------------------------------------------ */
void TIM6_DAC_IRQHandler(void)
{
	camtrig_hal_tick_irq();
}

/* ------------------------------------------------------------------ *
 * TIM2 update — one interrupt per emitted frame trigger.
 *
 * This does NOT produce the pulse; the pulse already happened in hardware.
 * It exists only so the trigger task can count pulses and terminate a burst.
 * If this interrupt were delayed, masked, or lost entirely, the waveform on
 * PA0/PA1/PA2/PA3 would be completely unaffected.
 *
 * Priority 5 == configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY, the highest
 * priority from which ...FromISR may be called (design D7).
 * ------------------------------------------------------------------ */
void TIM2_IRQHandler(void)
{
	if (htim2.Instance->SR & TIM_SR_UIF) {
		BaseType_t woken = pdFALSE;

		htim2.Instance->SR = ~TIM_SR_UIF;
		camtrig_notify_pulse_from_isr(&woken);
		portYIELD_FROM_ISR(woken);
	}
}

/* ------------------------------------------------------------------ *
 * USART1 receive — feeds the command line assembler.
 * ------------------------------------------------------------------ */
void USART1_IRQHandler(void)
{
	BaseType_t woken = pdFALSE;

	/* Clear the error flags a floating or noisy line sets; without this an
	 * ORE latches and RXNE never asserts again — the port goes deaf with
	 * no other symptom.
	 *
	 * The F4 has no ICR: ORE/FE/NE are cleared by reading SR and then DR,
	 * which is exactly what the read below does.  (The H7 build clears them
	 * explicitly via UART_CLEAR_OREF and friends; those do not exist here.) */
	if (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_ORE) ||
	    __HAL_UART_GET_FLAG(&huart1, UART_FLAG_FE)  ||
	    __HAL_UART_GET_FLAG(&huart1, UART_FLAG_NE)) {
		(void)huart1.Instance->SR;
		(void)huart1.Instance->DR;
	}

	while (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_RXNE))
		cli_rx_from_isr(SINK_UART,
				(char)(huart1.Instance->DR & 0xFF), &woken);

	portYIELD_FROM_ISR(woken);
}

/* ------------------------------------------------------------------ *
 * USB OTG_FS — the CDC control transport.
 *
 * Priority 5 is set in usbd_conf.c's HAL_PCD_MspInit, not here, because that
 * is where the reference code puts it and where anyone looking for it will
 * check.  It must stay >= configMAX_SYSCALL_INTERRUPT_PRIORITY: the CDC
 * receive callback posts assembled lines to the command queue.
 * ------------------------------------------------------------------ */
void OTG_FS_IRQHandler(void)
{
	HAL_PCD_IRQHandler(&hpcd_USB_OTG_FS);
}
