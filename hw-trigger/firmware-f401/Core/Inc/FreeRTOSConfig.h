/* FreeRTOSConfig.h — camtrig.
 *
 * The waveform does not depend on anything in this file.  TIM1 emits edges in
 * hardware; every task could stop and the four cameras would keep exposing in
 * lockstep at the last commanded parameters.  What the scheduler owns is the
 * command path and the pulse *bookkeeping*, not the pulses.
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>
extern uint32_t SystemCoreClock;

/* ------------------------------------------------------------------ *
 * Scheduling
 * ------------------------------------------------------------------ */
#define configUSE_PREEMPTION                     1
#define configUSE_TIME_SLICING                   1
/* The CMSIS-RTOS2 shim does `#include CMSIS_device_header` to reach the NVIC
 * and SysTick definitions.  CubeMX injects this from project settings; defined
 * here instead so the Makefile does not have to fight shell quoting. */
#define CMSIS_device_header                      "stm32f4xx.h"

#define configCPU_CLOCK_HZ                       (SystemCoreClock)
#define configTICK_RATE_HZ                       ((TickType_t)1000)
#define configUSE_16_BIT_TICKS                   0	/* CMSIS-RTOS2 requires 0 */
#define configIDLE_SHOULD_YIELD                  1

/* CMSIS-RTOS v2 requires exactly 56 — freertos_os2.h #errors otherwise.
 * With more than 32 priorities the port-optimised task selection (which uses a
 * 32-bit CLZ over the ready-list bitmap) cannot be used. */
#define configMAX_PRIORITIES                     (56)
#define configUSE_PORT_OPTIMISED_TASK_SELECTION  0

#define configMINIMAL_STACK_SIZE                 ((uint16_t)128)
#define configMAX_TASK_NAME_LEN                  (16)

/* ------------------------------------------------------------------ *
 * Memory
 * ------------------------------------------------------------------ */
#define configSUPPORT_STATIC_ALLOCATION          1
#define configSUPPORT_DYNAMIC_ALLOCATION         1
#define configTOTAL_HEAP_SIZE                    ((size_t)20480)
#define configAPPLICATION_ALLOCATED_HEAP         0

/* ------------------------------------------------------------------ *
 * Features cmsis_os2.c needs
 * ------------------------------------------------------------------ */
#define configUSE_MUTEXES                        1
#define configUSE_RECURSIVE_MUTEXES              1
#define configUSE_COUNTING_SEMAPHORES            1
#define configUSE_TASK_NOTIFICATIONS             1
#define configUSE_TRACE_FACILITY                 1
#define configUSE_TIMERS                         1
#define configTIMER_TASK_PRIORITY                (2)
#define configTIMER_QUEUE_LENGTH                 10
#define configTIMER_TASK_STACK_DEPTH             256
#define configQUEUE_REGISTRY_SIZE                8
#define configUSE_NEWLIB_REENTRANT               0

/* ------------------------------------------------------------------ *
 * Diagnostics — kept ON in the shipping build.
 *
 * This board runs with no debugger attached.  A loud, immediate failure is
 * worth its code size when the alternative is a hang hours later that has to
 * be diagnosed from a blinking LED.  configASSERT in particular catches the
 * HAL/FreeRTOS interrupt-priority misconfiguration (design D7) at the moment
 * it happens — the one bug class this restructure introduces.
 * ------------------------------------------------------------------ */
#define configCHECK_FOR_STACK_OVERFLOW           2
#define configUSE_MALLOC_FAILED_HOOK             1
#define configUSE_IDLE_HOOK                      0
#define configUSE_TICK_HOOK                      0
#define configRECORD_STACK_HIGH_ADDRESS          1

void vApplicationAssertFailed(const char *file, int line);
#define configASSERT(x) \
	do { if ((x) == 0) vApplicationAssertFailed(__FILE__, __LINE__); } while (0)

/* ------------------------------------------------------------------ *
 * Optional API
 * ------------------------------------------------------------------ */
#define INCLUDE_vTaskPrioritySet                 1
#define INCLUDE_uxTaskPriorityGet                1
#define INCLUDE_vTaskDelete                      1
#define INCLUDE_vTaskSuspend                     1
#define INCLUDE_vTaskDelayUntil                  1
#define INCLUDE_xTaskDelayUntil                  1
#define INCLUDE_vTaskDelay                       1
#define INCLUDE_xTaskGetSchedulerState           1
#define INCLUDE_xTaskGetCurrentTaskHandle        1
#define INCLUDE_uxTaskGetStackHighWaterMark      1
#define INCLUDE_xTaskGetIdleTaskHandle           1
#define INCLUDE_eTaskGetState                    1
#define INCLUDE_xTimerPendFunctionCall           1
#define INCLUDE_xTaskAbortDelay                  1
#define INCLUDE_xQueueGetMutexHolder             1
#define INCLUDE_xSemaphoreGetMutexHolder         1
#define INCLUDE_xTaskResumeFromISR               1

/* ------------------------------------------------------------------ *
 * Cortex-M7 interrupt priorities  (design D7)
 *
 * HAL_Init() sets NVIC_PRIORITYGROUP_4: all four bits are pre-emption
 * priority, no sub-priority.  On ARM, *numerically higher means logically
 * lower*, so:
 *
 *   0  .. 4   too high to call any FreeRTOS API — nothing here uses these
 *   5  .. 15  may call ...FromISR (TIM1_UP, USART1, later OTG_FS all live here)
 *   15         the kernel's own PendSV/SysTick, always lowest
 *
 * Every ISR this firmware enables must be set to >= 5 explicitly.  Leaving one
 * at the reset default of 0 is the classic silent failure, which is exactly
 * why configASSERT stays enabled above — the port asserts on it.
 * ------------------------------------------------------------------ */
#define configPRIO_BITS                          4

#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY        15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY   5

#define configKERNEL_INTERRUPT_PRIORITY \
	(configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
	(configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/* The kernel owns these vectors.  stm32f4xx_it.c must therefore NOT define
 * SVC_Handler / PendSV_Handler / SysTick_Handler; the HAL tick moves to TIM6
 * (design D6) so SysTick belongs to FreeRTOS alone.
 *
 * SysTick is deliberately NOT mapped here.  cmsis_os2.c supplies its own
 * SysTick_Handler which calls xPortSysTickHandler() only once the scheduler is
 * running; mapping it as well gives two definitions of the same symbol, and
 * taking port.c's would lose that guard. */
#define vPortSVCHandler                          SVC_Handler
#define xPortPendSVHandler                       PendSV_Handler

#endif /* FREERTOS_CONFIG_H */
