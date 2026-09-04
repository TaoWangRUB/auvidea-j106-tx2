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

/* I2C1 -> PB6 (SCL) / PB7 (SDA), AF4 — the Garmin LIDAR-Lite (ranger.c).
 *
 * Port B was untouched before this, so it collides with nothing: PA0-PA3 are
 * TIM2, PA5 the delta echo, PA9/PA10 the console, PA11/PA12 USB, PC13 the LED.
 *
 * GPIO_PULLUP here is a safety net, NOT the design.  The F4's internal pull-ups
 * are tens of kOhm, far too weak to pull a metre of cable up inside an I2C rise
 * time; fit 4.7k externals to 3V3, as WIRING.md section 4.5 says.  The internal
 * ones only stop the bus floating when nothing is plugged in, which would
 * otherwise read as phantom errors on a board with no sensor attached. */
void HAL_I2C_MspInit(I2C_HandleTypeDef *hi2c)
{
	GPIO_InitTypeDef g = {0};

	if (hi2c->Instance != I2C1)
		return;

	__HAL_RCC_GPIOB_CLK_ENABLE();

	g.Pin       = GPIO_PIN_6 | GPIO_PIN_7;
	g.Mode      = GPIO_MODE_AF_OD;
	g.Pull      = GPIO_PULLUP;
	g.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
	g.Alternate = GPIO_AF4_I2C1;
	HAL_GPIO_Init(GPIOB, &g);

	__HAL_RCC_I2C1_CLK_ENABLE();

	/* Reset the peripheral before HAL_I2C_Init() configures it.
	 *
	 * Enabling the I2C clock while SCL/SDA are already sitting in some
	 * arbitrary state can leave the block with BUSY latched in SR2, and
	 * nothing HAL_I2C_Init() writes clears it: the peripheral then refuses
	 * to generate a START forever and every transfer times out, on a bus
	 * that is electrically perfect.  The symptom is a hardware scan that
	 * finds nothing while a bit-banged scan on the same two pins finds the
	 * device immediately — which is exactly how this was diagnosed on
	 * 2026-09-04.  SWRST via the RCC is the only thing that clears it. */
	__HAL_RCC_I2C1_FORCE_RESET();
	__HAL_RCC_I2C1_RELEASE_RESET();
}

void HAL_I2C_MspDeInit(I2C_HandleTypeDef *hi2c)
{
	if (hi2c->Instance != I2C1)
		return;

	/* The pins are deliberately left configured.  The only caller is
	 * ranger_bus_reset(), which de-initialises the peripheral and then
	 * immediately takes PB6/PB7 over as open-drain GPIO to clock a stuck
	 * slave free; releasing them here would only add a window in which the
	 * bus floats. */
	__HAL_RCC_I2C1_CLK_DISABLE();
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
