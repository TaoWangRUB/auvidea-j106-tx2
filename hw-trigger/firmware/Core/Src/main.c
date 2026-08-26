/* main.c — camtrig bring-up: clock, GPIO, TIM1, USART1.
 *
 * See ../Inc/camtrig.h and camtrig.c for the trigger itself.  This file only
 * gets the silicon into the state the trigger expects.
 */
#include "main.h"
#include "camtrig.h"
#include "cmsis_os2.h"
#include "usb_device.h"

TIM_HandleTypeDef  htim5;
TIM_HandleTypeDef  htim1;	/* backlight PWM only */
SPI_HandleTypeDef  hspi4;
UART_HandleTypeDef huart1;

osThreadId_t g_trig_thread;
osThreadId_t g_cli_thread;

static const osThreadAttr_t trig_attr = {
	.name       = "trig",
	.stack_size = 512 * 4,
	.priority   = osPriorityAboveNormal,
};
static const osThreadAttr_t lcd_attr = {
	.name       = "lcd",
	.stack_size = 512 * 4,
	.priority   = osPriorityLow,	/* below cli: a slow SPI panel must never
					 * delay a command, let alone the trigger */
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

	HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

	__HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
	while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY))
		;

	/* WRHIGHFREQ is the *programming* delay, not read latency (RM0433
	 * §FLASH_ACR).  This firmware never writes flash at runtime, so it is
	 * set for correctness rather than necessity: 01 for ]70;140] MHz. */
	MODIFY_REG(FLASH->ACR, FLASH_ACR_WRHIGHFREQ, FLASH_ACR_WRHIGHFREQ_0);

	osc.OscillatorType   = RCC_OSCILLATORTYPE_HSE;
	osc.HSEState         = RCC_HSE_ON;
	osc.PLL.PLLState     = RCC_PLL_ON;
	osc.PLL.PLLSource    = RCC_PLLSOURCE_HSE;
	osc.PLL.PLLM         = 5;	/* 25 / 5      = 5 MHz  ref  */
	osc.PLL.PLLN         = 96;	/* 5 * 96      = 480 MHz VCO */
	osc.PLL.PLLP         = 2;	/* 480 / 2     = 240 MHz SYSCLK */
	osc.PLL.PLLQ         = 10;	/* 480 / 10    = 48 MHz  USB    */
	osc.PLL.PLLR         = 2;
	osc.PLL.PLLRGE       = RCC_PLL1VCIRANGE_2;
	osc.PLL.PLLVCOSEL    = RCC_PLL1VCOWIDE;
	osc.PLL.PLLFRACN     = 0;

	if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
		/* Crystal did not start.  1% instead of 20 ppm: costs absolute
		 * rate accuracy, not synchronisation, since every camera is
		 * driven from this one timer.  Reported by `status`.
		 * Preserved from the pre-restructure build on purpose - a
		 * cold-solder crystal must degrade the rig, not brick it. */
		osc.OscillatorType = RCC_OSCILLATORTYPE_HSI;
		osc.HSIState       = RCC_HSI_DIV1;	/* undivided => 64 MHz */
		osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
		osc.PLL.PLLState   = RCC_PLL_NONE;	/* leave the PLL alone */
		if (HAL_RCC_OscConfig(&osc) != HAL_OK)
			Error_Handler();

		clk.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
				     RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2 |
				     RCC_CLOCKTYPE_D3PCLK1 | RCC_CLOCKTYPE_D1PCLK1;
		clk.SYSCLKSource   = RCC_SYSCLKSOURCE_HSI;
		clk.SYSCLKDivider  = RCC_SYSCLK_DIV1;
		clk.AHBCLKDivider  = RCC_HCLK_DIV1;
		clk.APB3CLKDivider = RCC_APB3_DIV1;
		clk.APB1CLKDivider = RCC_APB1_DIV1;
		clk.APB2CLKDivider = RCC_APB2_DIV1;
		clk.APB4CLKDivider = RCC_APB4_DIV1;
		/* 64 MHz AXI at VOS1 is inside ]0;70] MHz -> 0 WS. */
		if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_0) != HAL_OK)
			Error_Handler();
		g_on_hse = 0;
		return;
	}

	clk.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
			     RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2 |
			     RCC_CLOCKTYPE_D3PCLK1 | RCC_CLOCKTYPE_D1PCLK1;
	clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
	clk.SYSCLKDivider  = RCC_SYSCLK_DIV1;	/* CPU  240 MHz */
	clk.AHBCLKDivider  = RCC_HCLK_DIV2;	/* AXI  120 MHz */
	clk.APB3CLKDivider = RCC_APB3_DIV1;
	clk.APB1CLKDivider = RCC_APB1_DIV1;
	clk.APB2CLKDivider = RCC_APB2_DIV1;	/* TIM1 120 MHz */
	clk.APB4CLKDivider = RCC_APB4_DIV1;

	if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_1) != HAL_OK)
		Error_Handler();

	/* USB gets its 48 MHz from PLL1Q (Q=10 -> 480/10).  Only reachable on
	 * this path: the HSI fallback runs with the PLL off, so USB is
	 * unavailable there and USART1 remains the only transport.  That is the
	 * right trade — the fallback exists so a dead crystal degrades the rig
	 * instead of bricking it, and the trigger itself does not need USB. */
	{
		RCC_PeriphCLKInitTypeDef pclk = {0};

		pclk.PeriphClockSelection = RCC_PERIPHCLK_USB;
		pclk.UsbClockSelection    = RCC_USBCLKSOURCE_PLL;
		if (HAL_RCCEx_PeriphCLKConfig(&pclk) != HAL_OK)
			Error_Handler();
	}

	g_on_hse = 1;
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
	return g_on_hse ? "hse25-pll240" : "hsi64-FALLBACK";
}

/* TIM5 is on APB1 (D2).  With TIMPRE = 0 the timer kernel clock is PCLK1 when
 * the APB1 prescaler is 1, and 2 x PCLK1 otherwise.  Derived rather than
 * hard-coded so the HSI fallback path stays correct too. */
uint32_t timer_clock_hz(void)
{
	uint32_t pclk1 = HAL_RCC_GetPCLK1Freq();

	if ((RCC->D2CFGR & RCC_D2CFGR_D2PPRE1) != RCC_D2CFGR_D2PPRE1_DIV1)
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
	__HAL_RCC_GPIOE_CLK_ENABLE();

	/* LED PE3, active low on this board — drive it off before enabling. */
	HAL_GPIO_WritePin(GPIOE, 1U << LED_PIN, GPIO_PIN_SET);
	g.Pin   = 1U << LED_PIN;
	g.Mode  = GPIO_MODE_OUTPUT_PP;
	g.Pull  = GPIO_NOPULL;
	g.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(GPIOE, &g);

	/* ST7735 chip-select and data/command, driven by the panel driver. */
	HAL_GPIO_WritePin(GPIOE, LCD_CS_Pin | LCD_WR_RS_Pin, GPIO_PIN_SET);
	g.Pin   = LCD_CS_Pin | LCD_WR_RS_Pin;
	g.Mode  = GPIO_MODE_OUTPUT_PP;
	g.Pull  = GPIO_NOPULL;
	g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
	HAL_GPIO_Init(GPIOE, &g);
}

/* ------------------------------------------------------------------ *
 * TIM5 — four PWM channels on one 32-bit counter
 *
 * TIM5 is a general-purpose timer, so unlike TIM1 there is no MOE bit and no
 * break/dead-time block to configure: the outputs are live as soon as the
 * channel is enabled.  One counter still drives all four channels, which is
 * what makes the four cameras expose simultaneously by construction.
 * ------------------------------------------------------------------ */
static void MX_TIM5_Init(void)
{
	TIM_ClockConfigTypeDef  src = {0};
	TIM_MasterConfigTypeDef mst = {0};
	TIM_OC_InitTypeDef      oc  = {0};
	unsigned i;
	static const uint32_t ch[NCH] = {
		TIM_CHANNEL_1, TIM_CHANNEL_2, TIM_CHANNEL_3, TIM_CHANNEL_4
	};

	htim5.Instance               = TIM5;
	htim5.Init.Prescaler         = 0;
	htim5.Init.CounterMode       = TIM_COUNTERMODE_UP;
	htim5.Init.Period            = 0xFFFFFFFFu;	/* 32-bit */
	htim5.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
	htim5.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
	if (HAL_TIM_PWM_Init(&htim5) != HAL_OK)
		Error_Handler();

	src.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
	if (HAL_TIM_ConfigClockSource(&htim5, &src) != HAL_OK)
		Error_Handler();

	mst.MasterOutputTrigger = TIM_TRGO_RESET;
	mst.MasterSlaveMode     = TIM_MASTERSLAVEMODE_DISABLE;
	if (HAL_TIMEx_MasterConfigSynchronization(&htim5, &mst) != HAL_OK)
		Error_Handler();

	oc.OCMode     = TIM_OCMODE_PWM1;
	oc.Pulse      = 0;		/* parked: idle until camtrig_init() */
	oc.OCPolarity = TIM_OCPOLARITY_HIGH;
	oc.OCFastMode = TIM_OCFAST_DISABLE;
	for (i = 0; i < NCH; i++) {
		if (HAL_TIM_PWM_ConfigChannel(&htim5, &oc, ch[i]) != HAL_OK)
			Error_Handler();
	}
}

/* ------------------------------------------------------------------ *
 * SPI4 + TIM1_CH2N — the on-board ST7735 TFT and its backlight
 * ------------------------------------------------------------------ */
static void MX_SPI4_Init(void)
{
	hspi4.Instance               = SPI4;
	hspi4.Init.Mode              = SPI_MODE_MASTER;
	hspi4.Init.Direction         = SPI_DIRECTION_2LINES_TXONLY;
	hspi4.Init.DataSize          = SPI_DATASIZE_8BIT;
	hspi4.Init.CLKPolarity       = SPI_POLARITY_LOW;
	hspi4.Init.CLKPhase          = SPI_PHASE_1EDGE;
	hspi4.Init.NSS               = SPI_NSS_SOFT;
	/* PCLK2 120 MHz / 8 = 15 MHz.  The ST7735 tolerates more, but the panel
	 * is refreshed twice a second with a few hundred bytes — there is
	 * nothing to gain by pushing it. */
	hspi4.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;
	hspi4.Init.FirstBit          = SPI_FIRSTBIT_MSB;
	hspi4.Init.TIMode            = SPI_TIMODE_DISABLE;
	hspi4.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
	hspi4.Init.NSSPMode          = SPI_NSS_PULSE_DISABLE;
	if (HAL_SPI_Init(&hspi4) != HAL_OK)
		Error_Handler();
}

/* Backlight on PE10 = TIM1_CH2N.  0..100 maps directly to the compare value,
 * which is what LCD_SetBrightness() in the vendored driver writes. */
static void MX_TIM1_Backlight_Init(void)
{
	TIM_OC_InitTypeDef oc = {0};
	TIM_BreakDeadTimeConfigTypeDef bdt = {0};
	GPIO_InitTypeDef g = {0};

	__HAL_RCC_TIM1_CLK_ENABLE();
	__HAL_RCC_GPIOE_CLK_ENABLE();

	htim1.Instance           = TIM1;
	/* Matches WeAct's own backlight config for this panel: 120 MHz / 12 /
	 * 1000 = 10 kHz, brightness 0..999. */
	htim1.Init.Prescaler     = 12 - 1;
	htim1.Init.CounterMode   = TIM_COUNTERMODE_UP;
	htim1.Init.Period        = 1000 - 1;
	htim1.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
	htim1.Init.RepetitionCounter = 0;
	htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
	if (HAL_TIM_PWM_Init(&htim1) != HAL_OK)
		Error_Handler();

	oc.OCMode       = TIM_OCMODE_PWM1;
	oc.Pulse        = 0;
	oc.OCPolarity   = TIM_OCPOLARITY_HIGH;
	/* LOW, not HIGH.  PE10 is TIM1_CH2N -- the *complementary* output -- so
	 * its polarity decides whether a large compare means bright or dark.
	 * With HIGH the sense inverts and full brightness renders as nearly
	 * off, which is exactly how this was first got wrong. WeAct's own
	 * driver for this panel uses LOW. */
	oc.OCNPolarity  = TIM_OCNPOLARITY_LOW;
	oc.OCFastMode   = TIM_OCFAST_DISABLE;
	oc.OCIdleState  = TIM_OCIDLESTATE_RESET;
	oc.OCNIdleState = TIM_OCNIDLESTATE_RESET;
	if (HAL_TIM_PWM_ConfigChannel(&htim1, &oc, TIM_CHANNEL_2) != HAL_OK)
		Error_Handler();

	bdt.OffStateRunMode  = TIM_OSSR_DISABLE;
	bdt.OffStateIDLEMode = TIM_OSSI_DISABLE;
	bdt.LockLevel        = TIM_LOCKLEVEL_OFF;
	bdt.BreakState       = TIM_BREAK_DISABLE;
	bdt.BreakPolarity    = TIM_BREAKPOLARITY_HIGH;
	bdt.Break2State      = TIM_BREAK2_DISABLE;
	bdt.Break2Polarity   = TIM_BREAK2POLARITY_HIGH;
	bdt.AutomaticOutput  = TIM_AUTOMATICOUTPUT_DISABLE;
	if (HAL_TIMEx_ConfigBreakDeadTime(&htim1, &bdt) != HAL_OK)
		Error_Handler();

	g.Pin       = LCD_BL_Pin;
	g.Mode      = GPIO_MODE_AF_PP;
	g.Pull      = GPIO_NOPULL;
	g.Speed     = GPIO_SPEED_FREQ_LOW;
	g.Alternate = GPIO_AF1_TIM1;
	HAL_GPIO_Init(LCD_BL_GPIO_Port, &g);

	HAL_TIMEx_PWMN_Start(&htim1, TIM_CHANNEL_2);
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
	huart1.Init.OneBitSampling         = UART_ONE_BIT_SAMPLE_DISABLE;
	huart1.Init.ClockPrescaler         = UART_PRESCALER_DIV1;
	huart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
	if (HAL_UART_Init(&huart1) != HAL_OK)
		Error_Handler();
}

/* ------------------------------------------------------------------ *
 * Main
 * ------------------------------------------------------------------ */
/* Set by the `dfu` command, then survives the software reset (see the .noinit
 * section in the linker script).  0x1FF00000 is the STM32H743 system bootloader
 * base — RM0433 Rev 7 Table 9, "System bootloader at 0x1FF0 0000".  NOT the
 * 0x1FF09800 that circulates for the F4 family. */
#define BOOT_MAGIC   0xB00710ADu

/* 0x1FF09800, established by measurement, not by the reference manual.
 *
 * RM0433 Table 9 describes the *boot address* as 0x1FF0 0000, and that is what
 * BOOT_ADD1 selects — but reading that address from user code returns all
 * zeros, so there is no vector to jump to.  Reading 0x1FF09800 instead returns
 * 0x240044b0, a stack pointer in AXI SRAM, which is what a bootloader vector
 * looks like.  Both were logged in `status` (sysmem_sp / sysmem_alt) rather
 * than argued about. */
#define SYSMEM_BASE  0x1FF09800u

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
	/* Second candidate: the address that circulates for the F4 family and
	 * gets copy-pasted into H7 projects.  Recorded so the question is
	 * settled by measurement rather than by folklore. */
	g_sysmem_alt = *(volatile uint32_t *)0x1FF09800u;

	if (g_boot_seen != BOOT_MAGIC)
		return;

	/* Refuse to jump anywhere implausible.  Without this, a bad vector takes
	 * the board off the bus entirely and the only way back is the BOOT0
	 * button — which is the very thing this exists to avoid.  Falling through
	 * boots the application normally, which is always recoverable. */
	/* The bootloader stacks in AXI SRAM (0x2400_0000), not DTCM — an
	 * earlier, DTCM-only check rejected the correct vector. */
	if (!((sp >= 0x20000000U && sp <= 0x20020000U) ||
	      (sp >= 0x24000000U && sp <= 0x24080000U)))
		return;
	if ((pc & 1U) == 0U || pc < 0x1FF00000U || pc >= 0x1FF20000U)
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
	MX_TIM5_Init();
	MX_SPI4_Init();
	MX_TIM1_Backlight_Init();
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
	(void)osThreadNew(lcd_task, NULL, &lcd_attr);
	configASSERT(g_trig_thread != NULL && g_cli_thread != NULL);

	/* TIM5 update: pulse bookkeeping only, never pulse generation.
	 * Priority 5 == configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY, the
	 * highest from which ...FromISR may be called. */
	HAL_NVIC_SetPriority(TIM5_IRQn, 5, 0);
	HAL_NVIC_EnableIRQ(TIM5_IRQn);
	__HAL_TIM_ENABLE_IT(&htim5, TIM_IT_UPDATE);

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
			HAL_GPIO_WritePin(GPIOE, 1U << LED_PIN, GPIO_PIN_RESET);
			for (d = 0; d < 2000000u; d++) ;
			HAL_GPIO_WritePin(GPIOE, 1U << LED_PIN, GPIO_PIN_SET);
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
