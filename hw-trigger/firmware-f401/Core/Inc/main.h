/* main.h — shared declarations for the camtrig application. */
#ifndef MAIN_H
#define MAIN_H

#include "stm32f4xx_hal.h"

/* Clock ------------------------------------------------------------- */
void        SystemClock_Config(void);
uint32_t    timer_clock_hz(void);	/* TIM2 kernel clock, APB1 x2 rule applied */
int         clock_usb_available(void);	/* false on the HSI fallback path   */
const char *clock_source_name(void);	/* "hse25-pll84" | "hsi16-FALLBACK"      */

/* Transports (cli.c) ------------------------------------------------- */
void cli_init(void);			/* queue + USART1 RX interrupt */

/* On-board 0.96" ST7735 TFT (160x80).  Reachable again now that the trigger
 * moved off TIM1/port E onto TIM2/PA0-PA3 — see camtrig.h. */



/* Status LED — PC13 on the WeAct MiniSTM32F4x1, ACTIVE LOW:
 * 3V3 -> R5 1.5K -> LED -> PC13 (MiniF4x1Cx_V31 schematic). */
#define LED_GPIO_Port        GPIOC
#define LED_Pin              GPIO_PIN_13

void Error_Handler(void);

/* Reboot into the STM32 ROM bootloader so the board can be reflashed over the
 * same USB-C, with no BOOT0 button press. */
void request_bootloader(void);

/* Park and signal a fault code on the LED.  Deliberately leaves TIM1 running:
 * a camera in Fast Trigger mode stalls if XTRIG stops, so a control-path fault
 * must not become a capture outage. */
void panic_blink(int code);

/* Remaining stack, in bytes, for `status`.  0 = trig, 1 = cli. */
uint32_t task_stack_free(int which);

/* Commands lost to a full queue, for `status`. */
uint32_t cli_dropped(void);

#endif /* MAIN_H */
