/* stm32h7xx_hal_timebase_tim.c — HAL tick on TIM6 instead of SysTick.
 *
 * FreeRTOS requires SysTick for its own scheduler tick.  Leaving HAL_InitTick()
 * on SysTick as well is the best-known way to make a CubeMX+FreeRTOS project
 * fail in ways that present as random hangs (design D6), so the HAL timebase
 * moves to TIM6 — otherwise unused here, and CubeMX's own recommendation.
 *
 * Derived from ST's stm32h7xx_hal_timebase_tim_template.c.
 */
#include "main.h"

static TIM_HandleTypeDef htim6;

HAL_StatusTypeDef HAL_InitTick(uint32_t TickPriority)
{
	RCC_ClkInitTypeDef clk;
	uint32_t latency, timclk, presc;
	HAL_StatusTypeDef st;

	if (TickPriority >= (1UL << __NVIC_PRIO_BITS))
		return HAL_ERROR;

	__HAL_RCC_TIM6_CLK_ENABLE();

	HAL_RCC_GetClockConfig(&clk, &latency);

	/* TIM6 is on APB1 (D2).  With TIMPRE = 0 the timer kernel clock is
	 * PCLK1 when the APB1 prescaler is 1, and 2 x PCLK1 otherwise. */
	if (clk.APB1CLKDivider == RCC_HCLK_DIV1)
		timclk = HAL_RCC_GetPCLK1Freq();
	else
		timclk = 2UL * HAL_RCC_GetPCLK1Freq();

	presc = (timclk / 1000000U) - 1U;	/* 1 MHz counter */

	htim6.Instance           = TIM6;
	htim6.Init.Prescaler     = presc;
	htim6.Init.CounterMode   = TIM_COUNTERMODE_UP;
	htim6.Init.Period        = (1000000U / 1000U) - 1U;	/* 1 kHz tick */
	htim6.Init.ClockDivision = 0;

	st = HAL_TIM_Base_Init(&htim6);
	if (st != HAL_OK)
		return st;

	st = HAL_TIM_Base_Start_IT(&htim6);
	if (st != HAL_OK)
		return st;

	HAL_NVIC_SetPriority(TIM6_DAC_IRQn, TickPriority, 0);
	HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn);
	uwTickPrio = TickPriority;

	return HAL_OK;
}

void HAL_SuspendTick(void)
{
	__HAL_TIM_DISABLE_IT(&htim6, TIM_IT_UPDATE);
}

void HAL_ResumeTick(void)
{
	__HAL_TIM_ENABLE_IT(&htim6, TIM_IT_UPDATE);
}

/* Called from TIM6_DAC_IRQHandler in stm32h7xx_it.c. */
void camtrig_hal_tick_irq(void)
{
	HAL_TIM_IRQHandler(&htim6);
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
	if (htim->Instance == TIM6)
		HAL_IncTick();
}
