/* stm32f4xx_hal_msp.c — board-level init for the peripherals camtrig uses.
 *
 * Written for the WeAct MiniSTM32F4x1 rather than adapted from the H7 build:
 * the F4 has no per-USART clock mux (RCC_PERIPHCLK_USART1 and friends are H7
 * additions), and this board has neither the H7's SPI panel nor a TIM6.
 *
 * USB's MspInit lives in USB_DEVICE/Target/usbd_conf.c, which is WeAct's own
 * validated code for this board — deliberately not duplicated here.
 */

#include "main.h"

void HAL_MspInit(void)
{
	__HAL_RCC_SYSCFG_CLK_ENABLE();
	__HAL_RCC_PWR_CLK_ENABLE();
}

/* TIM2_CH1..4 -> PA0/PA1/PA2/PA3, AF1.
 *
 * The same four pins the H7 build drives with TIM5_CH1..4, so the harness in
 * WIRING.md section 4.1 is unchanged.  Note PA0 is also this board's User KEY
 * (via R1 330R to GND): pressing it while a channel is high sources ~10 mA,
 * inside the per-pin limit, and there is no capacitor on the net to slow the
 * edge.  Harmless, but do not press it while measuring skew.
 */
/* Clock AND pins are both set up here, in MspInit, which HAL_TIM_PWM_Init()
 * calls for us.
 *
 * ⚠ Do NOT move the GPIO setup into HAL_TIM_MspPostInit(): HAL never calls that
 * itself — CubeMX emits an explicit call at the end of MX_TIMx_Init, and a
 * hand-written MX_TIM2_Init that omits it leaves PA0-PA3 in their reset state.
 * The failure is silent and highly misleading: TIM2 still runs, the update
 * interrupt still fires, the pulse counter still advances and the status LED
 * still blinks at exactly the right rate — but no edge ever reaches a pin, so
 * every camera reports zero frames at every rate and both polarities, and the
 * fault looks like wiring. (Cost a long session on 2026-08-31.)  The H7 build
 * does it this way for the same reason. */
void HAL_TIM_PWM_MspInit(TIM_HandleTypeDef *htim)
{
	GPIO_InitTypeDef g = {0};

	if (htim->Instance != TIM2)
		return;

	__HAL_RCC_TIM2_CLK_ENABLE();
	__HAL_RCC_GPIOA_CLK_ENABLE();
	g.Pin       = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3;
	g.Mode      = GPIO_MODE_AF_PP;
	g.Pull      = GPIO_NOPULL;
	/* These drive optocoupler LEDs and the edge is what the sensor times
	 * against, so do not economise on slew rate here. */
	g.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
	g.Alternate = GPIO_AF1_TIM2;
	HAL_GPIO_Init(GPIOA, &g);

	/* ---- Delta echo: a second, OPEN-DRAIN copy of CH1 on PA5 ----------------
	 *
	 * For measuring the camera<->IMU offset the TX2 needs to timestamp the real
	 * trigger edge (WIRING.md section 4.4).  The Tegra pad it goes to is 1.8 V
	 * UNBUFFERED, and this board's outputs are 3.3 V, so a push-pull tap would
	 * destroy the pad.  Open-drain removes the problem instead of dividing it
	 * down: the pin can only ever pull the line LOW, and the high level comes
	 * from the Tegra's internal pull-up at 1.8 V.  Nothing on that wire is ever
	 * driven above 1.8 V, so no external components are needed.
	 *
	 * PA5 is TIM2_CH1's alternate pin, so this is the SAME compare event that
	 * drives PA0 - a hardware copy with no interrupt and no jitter, not a
	 * software toggle.  It also follows the `pol` setting automatically, since
	 * polarity lives in the channel, not the pad.
	 *
	 * ⚠ Timestamp the FALLING edge.  That is the one this pin drives hard; the
	 * rising edge is limited by a weak internal pull-up charging the wire, so it
	 * is slow and its timing is not trustworthy.  Under `pol 0` (active_low, the
	 * working polarity on this rig) the falling edge is also the start of the
	 * exposure, which is what you want to measure anyway.
	 *
	 * Costs one pin.  Harmless if nothing is connected.
	 */
	g.Pin       = GPIO_PIN_5;
	g.Mode      = GPIO_MODE_AF_OD;
	g.Pull      = GPIO_NOPULL;      /* the pull-up is the Tegra's, at 1.8 V */
	g.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
	g.Alternate = GPIO_AF1_TIM2;
	HAL_GPIO_Init(GPIOA, &g);
}

/* USART1 -> PA9 (TX) / PA10 (RX), AF7 — the same pins as the H7 build, so the
 * M110 J22 link in WIRING.md section 4.2 needs no rewiring. */
void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
	GPIO_InitTypeDef g = {0};

	if (huart->Instance != USART1)
		return;

	__HAL_RCC_USART1_CLK_ENABLE();
	__HAL_RCC_GPIOA_CLK_ENABLE();

	g.Pin       = GPIO_PIN_9 | GPIO_PIN_10;
	g.Mode      = GPIO_MODE_AF_PP;
	g.Pull      = GPIO_PULLUP;
	g.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
	g.Alternate = GPIO_AF7_USART1;
	HAL_GPIO_Init(GPIOA, &g);
}
