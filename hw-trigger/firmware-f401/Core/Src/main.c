/* main.c — camtrig bring-up: clock, GPIO, TIM1, USART1.
 *
 * See ../Inc/camtrig.h and camtrig.c for the trigger itself.  This file only
 * gets the silicon into the state the trigger expects.
 */
#include "main.h"
#include "camtrig.h"
#include "cmsis_os2.h"
#include "usb_device.h"

TIM_HandleTypeDef  htim2;
UART_HandleTypeDef huart1;

osThreadId_t g_trig_thread;
osThreadId_t g_cli_thread;

static const osThreadAttr_t trig_attr = {
	.name       = "trig",
	.stack_size = 512 * 4,
	.priority   = osPriorityAboveNormal,
};
static const osThreadAttr_t cli_attr = {
	.name       = "cli",
	/* 4 KB: this task now also runs MX_USB_DEVICE_Init(), whose HAL_PCD_Init
	 * / USB_DevInit call chain is deeper than anything else here.  It used
	 * to run on the pre-scheduler MSP, which had the whole of DTCM below it.
	 * configCHECK_FOR_STACK_OVERFLOW = 2 turns a bad guess into blink code 3
	 * rather than a silent corruption. */
	.stack_size = 1024 * 4,
	.priority   = osPriorityNormal,
};

static int g_on_hse;

/* ------------------------------------------------------------------ *
 * Clock — HSE 25 MHz -> PLL1 -> SYSCLK 240 MHz, HCLK/AXI 120 MHz.
 *
 * This supersedes the pre-restructure "no PLL" decision, whose rationale was
 * avoiding hand-written PLL/VOS/flash-latency sequencing.  The HAL performs
 * that sequencing now, so the cost is gone and the finer timer clock is free.
 *
 * VOS1 + FLASH_LATENCY_1 is NOT what the WeAct reference code uses.  It pairs
 * VOS2 with FLASH_LATENCY_1, which RM0433 Rev 7 Table 17 says is wrong: at
 * VOS2 an AXI clock of 120 MHz falls in ]110 MHz; 165 MHz] and needs *2* wait
 * states.  Under-provisioned flash wait states are the classic silent brick,
 * so this uses VOS1, where 120 MHz falls in ]70 MHz; 140 MHz] and 1 WS is
 * correct.  Do not "simplify" this back to the vendor's numbers.
 * ------------------------------------------------------------------ */
void SystemClock_Config(void)
{
	RCC_OscInitTypeDef osc = {0};
	RCC_ClkInitTypeDef clk = {0};

	__HAL_RCC_PWR_CLK_ENABLE();
	__HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

	osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
	osc.HSEState       = RCC_HSE_ON;
	osc.PLL.PLLState   = RCC_PLL_ON;
	osc.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
	osc.PLL.PLLM       = 25;		/* 25 MHz / 25 = 1 MHz PLL input   */
	osc.PLL.PLLN       = 336;		/* 1 * 336    = 336 MHz VCO        */
	osc.PLL.PLLP       = RCC_PLLP_DIV4;	/* 336 / 4    = 84 MHz SYSCLK      */
	osc.PLL.PLLQ       = 7;			/* 336 / 7    = 48 MHz exactly, USB */
	if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
		/* No crystal.  Unlike the H7 build there is no HSI fallback that
		 * keeps USB alive: OTG_FS needs 48 MHz derived from a real
		 * crystal, and HSI cannot supply it to spec.  The trigger still
		 * matters more than the console, so come up on HSI anyway and let
		 * `status` report it. */
		g_on_hse = 0;
		osc.OscillatorType = RCC_OSCILLATORTYPE_HSI;
		osc.HSIState       = RCC_HSI_ON;
		osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
		osc.PLL.PLLSource  = RCC_PLLSOURCE_HSI;
		osc.PLL.PLLM       = 16;	/* 16 MHz / 16 = 1 MHz, same VCO */
		if (HAL_RCC_OscConfig(&osc) != HAL_OK)
			Error_Handler();
	} else {
		g_on_hse = 1;
	}

	clk.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
			   | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
	clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
	clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
	clk.APB1CLKDivider = RCC_HCLK_DIV2;	/* 42 MHz; TIM2 sees 84 */
	clk.APB2CLKDivider = RCC_HCLK_DIV1;	/* 84 MHz               */
	if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2) != HAL_OK)
		Error_Handler();
}

int clock_usb_available(void)
{
	return g_on_hse;
}

int camtrig_on_hse(void)
{
	return g_on_hse;
}

const char *clock_source_name(void)
{
	return g_on_hse ? "hse25-pll84" : "hsi16-FALLBACK";
}

/* TIM2 is on APB1.  The timer kernel clock is PCLK1 when the APB1
 * prescaler is 1, and 2 x PCLK1 otherwise (RM0368 6.2).  Derived rather than
 * hard-coded so the HSI fallback path stays correct too. */
uint32_t timer_clock_hz(void)
{
	uint32_t pclk1 = HAL_RCC_GetPCLK1Freq();

	if ((RCC->CFGR & RCC_CFGR_PPRE1) != RCC_CFGR_PPRE1_DIV1)
		return pclk1 * 2u;
	return pclk1;
}

/* ------------------------------------------------------------------ *
 * GPIO
 * ------------------------------------------------------------------ */
static void MX_GPIO_Init(void)
{
	GPIO_InitTypeDef g = {0};

	__HAL_RCC_GPIOA_CLK_ENABLE();
	__HAL_RCC_GPIOC_CLK_ENABLE();

	/* Status LED PC13, active low on this board (3V3 -> R5 1.5K -> LED ->
	 * PC13, WeAct MiniF4x1Cx_V31 schematic).  Drive it off before enabling
	 * the output so it cannot flash on during bring-up. */
	HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_SET);
	g.Pin   = LED_Pin;
	g.Mode  = GPIO_MODE_OUTPUT_PP;
	g.Pull  = GPIO_NOPULL;
	g.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(LED_GPIO_Port, &g);
}

/* ------------------------------------------------------------------ *
 * TIM2 — four PWM channels on one 32-bit counter
 *
 * TIM2 is a general-purpose timer, so unlike TIM1 there is no MOE bit and no
 * break/dead-time block to configure: the outputs are live as soon as the
 * channel is enabled.  One counter still drives all four channels, which is
 * what makes the four cameras expose simultaneously by construction.
 * ------------------------------------------------------------------ */
static void MX_TIM2_Init(void)
{
	TIM_ClockConfigTypeDef  src = {0};
	TIM_MasterConfigTypeDef mst = {0};
	TIM_OC_InitTypeDef      oc  = {0};
	unsigned i;
	static const uint32_t ch[NCH] = {
		TIM_CHANNEL_1, TIM_CHANNEL_2, TIM_CHANNEL_3, TIM_CHANNEL_4
	};

	htim2.Instance               = TIM2;
	htim2.Init.Prescaler         = 0;
	htim2.Init.CounterMode       = TIM_COUNTERMODE_UP;
	htim2.Init.Period            = 0xFFFFFFFFu;	/* 32-bit */
	htim2.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
	htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
	if (HAL_TIM_PWM_Init(&htim2) != HAL_OK)
		Error_Handler();

	src.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
	if (HAL_TIM_ConfigClockSource(&htim2, &src) != HAL_OK)
		Error_Handler();

	mst.MasterOutputTrigger = TIM_TRGO_RESET;
	mst.MasterSlaveMode     = TIM_MASTERSLAVEMODE_DISABLE;
	if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &mst) != HAL_OK)
		Error_Handler();

	oc.OCMode     = TIM_OCMODE_PWM1;
	oc.Pulse      = 0;		/* parked: idle until camtrig_init() */
	oc.OCPolarity = TIM_OCPOLARITY_HIGH;
	oc.OCFastMode = TIM_OCFAST_DISABLE;
	for (i = 0; i < NCH; i++) {
		if (HAL_TIM_PWM_ConfigChannel(&htim2, &oc, ch[i]) != HAL_OK)
			Error_Handler();
	}
}



/* ------------------------------------------------------------------ *
 * USART1 — the transport that works with no host enumeration
 * ------------------------------------------------------------------ */
static void MX_USART1_UART_Init(void)
{
	huart1.Instance                    = USART1;
	huart1.Init.BaudRate               = 115200;
	huart1.Init.WordLength             = UART_WORDLENGTH_8B;
	huart1.Init.StopBits               = UART_STOPBITS_1;
	huart1.Init.Parity                 = UART_PARITY_NONE;
	huart1.Init.Mode                   = UART_MODE_TX_RX;
	huart1.Init.HwFlowCtl              = UART_HWCONTROL_NONE;
	huart1.Init.OverSampling           = UART_OVERSAMPLING_16;
	/* The F4 UART has no OneBitSampling / ClockPrescaler / AdvancedInit
	 * fields -- those are H7-family additions.  Everything this firmware
	 * needs from the UART is in the seven settings above. */
	if (HAL_UART_Init(&huart1) != HAL_OK)
		Error_Handler();
}

/* ------------------------------------------------------------------ *
 * Main
 * ------------------------------------------------------------------ */
/* Set by the `dfu` command, then survives the software reset (see the .noinit
 * section in the linker script). */
#define BOOT_MAGIC   0xB00710ADu

/* STM32F401 system memory: 0x1FFF0000..0x1FFF77FF (RM0368 Rev 5, Table 3).
 *
 * This is NOT the H7 build's 0x1FF09800 — that address was established by
 * measurement on the H743 and is meaningless here.  Carrying it over would
 * send `dfu` into unmapped space, taking the board off the bus and leaving
 * BOOT0 as the only way back, which is precisely what `dfu` exists to avoid. */
#define SYSMEM_BASE  0x1FFF0000u
#define SYSMEM_END   0x1FFF7800u

__attribute__((section(".noinit"))) uint32_t g_boot_request;

/* Captured at boot before g_boot_request is cleared, so `status` can say why a
 * `dfu` request did or did not take effect.  Without this the two failure modes
 * — magic lost across the reset, versus vector rejected by the guard — are
 * indistinguishable from outside. */
uint32_t g_boot_seen, g_sysmem_sp, g_sysmem_pc, g_sysmem_alt;

void request_bootloader(void)
{
	g_boot_request = BOOT_MAGIC;
	__DSB();
	NVIC_SystemReset();
}

/* Reached at the very top of main(), before HAL_Init() and before any clock or
 * peripheral is touched — which is the entire point.  Jumping into the ROM
 * bootloader out of a running FreeRTOS + USB context means tearing all of that
 * down correctly first; coming through a reset means there is nothing to tear
 * down. */
static void maybe_enter_bootloader(void)
{
	uint32_t sp, pc;

	g_boot_seen = g_boot_request;
	g_boot_request = 0;

	sp = *(volatile uint32_t *)SYSMEM_BASE;
	pc = *(volatile uint32_t *)(SYSMEM_BASE + 4U);
	g_sysmem_sp = sp;
	g_sysmem_pc = pc;
	/* Recorded so a failed jump can be diagnosed from `status` rather than
	 * guessed at, exactly as on the H7 where the correct base had to be
	 * found by measurement. */
	g_sysmem_alt = 0;

	if (g_boot_seen != BOOT_MAGIC)
		return;

	/* Refuse to jump anywhere implausible.  Without this, a bad vector takes
	 * the board off the bus entirely and the only way back is the BOOT0
	 * button — which is the very thing this exists to avoid.  Falling through
	 * boots the application normally, which is always recoverable. */
	/* The F401 bootloader stacks in the one SRAM block, 0x2000_0000 + 96K. */
	if (!(sp >= 0x20000000U && sp <= 0x20018000U))
		return;
	if ((pc & 1U) == 0U || pc < SYSMEM_BASE || pc >= SYSMEM_END)
		return;

	SCB->VTOR = SYSMEM_BASE;
	__DSB();
	__ISB();

	/* Switching MSP and branching MUST be one asm block.  Doing
	 *     __set_MSP(sp); boot = (void(*)(void))pc; boot();
	 * looks right and is not: at -Os `pc` is a stack-spilled local, so the
	 * read after the MSP switch comes from the *new* stack and yields
	 * garbage.  Keeping both in registers removes the stack from the
	 * sequence entirely. */
	__asm volatile (
		"msr  msp, %0\n\t"
		"bx   %1\n"
		:
		: "r" (sp), "r" (pc)
		: "memory"
	);

	for (;;)		/* not reached */
		;
}

int main(void)
{
	maybe_enter_bootloader();

	HAL_Init();		/* also sets NVIC_PRIORITYGROUP_4 (design D7) */
	SystemClock_Config();

	MX_GPIO_Init();
	MX_TIM2_Init();
	MX_USART1_UART_Init();

	osKernelInitialize();

	/* Start triggering BEFORE the scheduler.  If anything below fails, the
	 * rig is still emitting a valid waveform from the compiled-in defaults
	 * — the contract is that the trigger never depends on the control
	 * path, and that includes the control path's own bring-up. */
	camtrig_init();
	cli_init();

	/* USB is deliberately NOT started here.  MX_USB_DEVICE_Init() enables
	 * OTG_FS_IRQn, and that ISR posts to a FreeRTOS queue and calls
	 * portYIELD_FROM_ISR — neither legal before the scheduler runs.
	 * cli_task brings USB up as its first act instead. */

	g_trig_thread = osThreadNew(camtrig_task, NULL, &trig_attr);
	g_cli_thread  = osThreadNew(cli_task,     NULL, &cli_attr);
	configASSERT(g_trig_thread != NULL && g_cli_thread != NULL);

	/* TIM2 update: pulse bookkeeping only, never pulse generation.
	 * Priority 5 == configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY, the
	 * highest from which ...FromISR may be called. */
	HAL_NVIC_SetPriority(TIM2_IRQn, 5, 0);
	HAL_NVIC_EnableIRQ(TIM2_IRQn);
	__HAL_TIM_ENABLE_IT(&htim2, TIM_IT_UPDATE);

	osKernelStart();

	/* Only reached if the scheduler could not start.  The waveform is
	 * already running, so signal and leave it running. */
	panic_blink(2);
}

/* ------------------------------------------------------------------ *
 * Failure signalling
 *
 * These deliberately do NOT stop the trigger.  A camera in Fast Trigger mode
 * stalls when XTRIG stops, so killing the waveform turns a control-path bug
 * into a capture outage.  TIM1's registers are not affected by RAM corruption
 * on this side, so the safest thing a corrupted control path can do is keep
 * its hands off the timer and say so — which is also what the
 * `camera-hw-trigger` spec requires ("Waveform survives a control-path fault").
 * ------------------------------------------------------------------ */
void panic_blink(int code)
{
	__disable_irq();
	for (;;) {
		int i;
		volatile uint32_t d;

		for (i = 0; i < code; i++) {
			HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);
			for (d = 0; d < 2000000u; d++) ;
			HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_SET);
			for (d = 0; d < 2000000u; d++) ;
		}
		for (d = 0; d < 12000000u; d++) ;
	}
}

uint32_t task_stack_free(int which)
{
	osThreadId_t t = (which == 0) ? g_trig_thread : g_cli_thread;

	return t ? (uint32_t)osThreadGetStackSpace(t) : 0;
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
	(void)task;
	(void)name;
	panic_blink(3);
}

void vApplicationMallocFailedHook(void)
{
	panic_blink(4);
}

void vApplicationAssertFailed(const char *file, int line)
{
	(void)file;
	(void)line;
	panic_blink(5);
}

/* configSUPPORT_STATIC_ALLOCATION = 1 requires these. */
void vApplicationGetIdleTaskMemory(StaticTask_t **tcb, StackType_t **stack,
				   uint32_t *size)
{
	static StaticTask_t idle_tcb;
	static StackType_t  idle_stack[configMINIMAL_STACK_SIZE];

	*tcb = &idle_tcb;
	*stack = idle_stack;
	*size = configMINIMAL_STACK_SIZE;
}

void vApplicationGetTimerTaskMemory(StaticTask_t **tcb, StackType_t **stack,
				    uint32_t *size)
{
	static StaticTask_t timer_tcb;
	static StackType_t  timer_stack[configTIMER_TASK_STACK_DEPTH];

	*tcb = &timer_tcb;
	*stack = timer_stack;
	*size = configTIMER_TASK_STACK_DEPTH;
}

void Error_Handler(void)
{
	panic_blink(1);
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
	(void)file;
	(void)line;
	Error_Handler();
}
#endif
