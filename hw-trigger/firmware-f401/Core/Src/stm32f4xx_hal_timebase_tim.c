/* stm32f4xx_hal_timebase_tim.c — HAL time base on TIM11, not SysTick.
 *
 * FreeRTOS owns SysTick, so the HAL needs its own 1 kHz tick or HAL_Delay and
 * every HAL timeout become dependent on the scheduler being up — which they
 * are not during bring-up, and HAL_PCD_Init in particular runs timeouts.
 *
 * TIM11 rather than the H7 build's TIM6: the F401 has no basic timers.  TIM11
 * is otherwise unused here (TIM2 generates the trigger).
 */

#include "main.h"

static TIM_HandleTypeDef htim11;

HAL_StatusTypeDef HAL_InitTick(uint32_t TickPriority)
{
	RCC_ClkInitTypeDef clk;
	uint32_t pclk2, presc, flatency;

	__HAL_RCC_TIM11_CLK_ENABLE();
	HAL_RCC_GetClockConfig(&clk, &flatency);

	/* TIM11 is on APB2: the kernel clock is PCLK2 when the APB2 prescaler
	 * is 1, and 2 x PCLK2 otherwise (RM0368 6.2). */
	pclk2 = HAL_RCC_GetPCLK2Freq();
	if (clk.APB2CLKDivider != RCC_HCLK_DIV1)
		pclk2 *= 2u;

	presc = (pclk2 / 1000000u) - 1u;	/* -> 1 MHz */

	htim11.Instance           = TIM11;
	htim11.Init.Prescaler     = presc;
	htim11.Init.CounterMode   = TIM_COUNTERMODE_UP;
	htim11.Init.Period        = 1000u - 1u;		/* 1 kHz */
	htim11.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
	if (HAL_TIM_Base_Init(&htim11) != HAL_OK)
		return HAL_ERROR;
	if (HAL_TIM_Base_Start_IT(&htim11) != HAL_OK)
		return HAL_ERROR;

	HAL_NVIC_SetPriority(TIM1_TRG_COM_TIM11_IRQn, TickPriority, 0);
	HAL_NVIC_EnableIRQ(TIM1_TRG_COM_TIM11_IRQn);
	uwTickPrio = TickPriority;
	return HAL_OK;
}

void HAL_SuspendTick(void)  { __HAL_TIM_DISABLE_IT(&htim11, TIM_IT_UPDATE); }
void HAL_ResumeTick(void)   { __HAL_TIM_ENABLE_IT(&htim11, TIM_IT_UPDATE); }

void TIM1_TRG_COM_TIM11_IRQHandler(void)
{
	if (__HAL_TIM_GET_FLAG(&htim11, TIM_FLAG_UPDATE) &&
	    __HAL_TIM_GET_IT_SOURCE(&htim11, TIM_IT_UPDATE)) {
		__HAL_TIM_CLEAR_IT(&htim11, TIM_IT_UPDATE);
		HAL_IncTick();
	}
}
