/* stm32h7xx_hal_msp.c — peripheral clocks and pin muxing.
 *
 * Pin choices are board facts from the WeAct MiniSTM32H7xx V1.2 schematic;
 * see ../Inc/camtrig.h and ../../WIRING.md.
 */
#include "main.h"
#include "camtrig.h"

void HAL_MspInit(void)
{
	__HAL_RCC_SYSCFG_CLK_ENABLE();
}

/* TIM5_CH1..CH4 -> PA0 / PA1 / PA2 / PA3, AF2.
 *
 * Pull DOWN, not up: idle must be LED-off.  If this board is in reset or
 * unplugged the pins float, and a pull-down keeps the optos dark rather than
 * holding four sensors inside an exposure.
 *
 * High speed: these pins source ~10 mA into an LED, and a crisp edge matters
 * because the pulse width IS the exposure.
 */
void HAL_TIM_PWM_MspInit(TIM_HandleTypeDef *htim)
{
	GPIO_InitTypeDef g = {0};

	if (htim->Instance != TIM5)
		return;

	__HAL_RCC_TIM5_CLK_ENABLE();
	__HAL_RCC_GPIOA_CLK_ENABLE();

	g.Pin       = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3;
	g.Mode      = GPIO_MODE_AF_PP;
	g.Pull      = GPIO_PULLDOWN;
	g.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
	g.Alternate = GPIO_AF2_TIM5;
	HAL_GPIO_Init(GPIOA, &g);
}

void HAL_TIM_PWM_MspDeInit(TIM_HandleTypeDef *htim)
{
	if (htim->Instance != TIM5)
		return;

	__HAL_RCC_TIM5_CLK_DISABLE();
	HAL_GPIO_DeInit(GPIOA, GPIO_PIN_0 | GPIO_PIN_1 |
				GPIO_PIN_2 | GPIO_PIN_3);
}

/* USART1 -> PA9 (TX, header P1-27) / PA10 (RX, P1-26), AF7.
 *
 * These are also the pins OTG_FS would use for VBUS sense and ID, which is
 * one reason the USB transport must run device-only with VBUS sensing off. */
void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
	GPIO_InitTypeDef g = {0};
	RCC_PeriphCLKInitTypeDef pclk = {0};

	if (huart->Instance != USART1)
		return;

	pclk.PeriphClockSelection = RCC_PERIPHCLK_USART1;
	pclk.Usart16ClockSelection = RCC_USART16CLKSOURCE_D2PCLK2;
	if (HAL_RCCEx_PeriphCLKConfig(&pclk) != HAL_OK)
		Error_Handler();

	__HAL_RCC_USART1_CLK_ENABLE();
	__HAL_RCC_GPIOA_CLK_ENABLE();

	g.Pin       = GPIO_PIN_9 | GPIO_PIN_10;
	g.Mode      = GPIO_MODE_AF_PP;
	g.Pull      = GPIO_PULLUP;	/* idle-high line, so RX does not float */
	g.Speed     = GPIO_SPEED_FREQ_LOW;
	g.Alternate = GPIO_AF7_USART1;
	HAL_GPIO_Init(GPIOA, &g);
}

void HAL_UART_MspDeInit(UART_HandleTypeDef *huart)
{
	if (huart->Instance != USART1)
		return;

	__HAL_RCC_USART1_CLK_DISABLE();
	HAL_GPIO_DeInit(GPIOA, GPIO_PIN_9 | GPIO_PIN_10);
}

/* ------------------------------------------------------------------ *
 * SPI4 -> the on-board ST7735 TFT.  PE12 = SCK, PE14 = MOSI, AF5.
 *
 * Transmit-only: the panel's MISO is not wired on this board, and nothing here
 * reads from it.  PE11 (CS) and PE13 (WR_RS) are plain GPIOs driven by the
 * driver, configured in MX_GPIO_Init.
 * ------------------------------------------------------------------ */
void HAL_SPI_MspInit(SPI_HandleTypeDef *hspi)
{
	GPIO_InitTypeDef g = {0};

	if (hspi->Instance != SPI4)
		return;

	__HAL_RCC_SPI4_CLK_ENABLE();
	__HAL_RCC_GPIOE_CLK_ENABLE();

	g.Pin       = GPIO_PIN_12 | GPIO_PIN_14;
	g.Mode      = GPIO_MODE_AF_PP;
	g.Pull      = GPIO_NOPULL;
	g.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
	g.Alternate = GPIO_AF5_SPI4;
	HAL_GPIO_Init(GPIOE, &g);
}

void HAL_SPI_MspDeInit(SPI_HandleTypeDef *hspi)
{
	if (hspi->Instance != SPI4)
		return;

	__HAL_RCC_SPI4_CLK_DISABLE();
	HAL_GPIO_DeInit(GPIOE, GPIO_PIN_12 | GPIO_PIN_14);
}

/* TIM1_CH2N -> PE10, the TFT backlight.  TIM1 is free for this precisely
 * because the trigger moved to TIM5; on the stock pinout TIM1_CH2 drove
 * camera 2 and its complementary output was this backlight. */
void HAL_TIM_Base_MspInit(TIM_HandleTypeDef *htim)
{
	if (htim->Instance == TIM1)
		__HAL_RCC_TIM1_CLK_ENABLE();
	else if (htim->Instance == TIM6)
		__HAL_RCC_TIM6_CLK_ENABLE();
}

void HAL_TIMEx_BreakCallback(TIM_HandleTypeDef *htim)
{
	(void)htim;
}
