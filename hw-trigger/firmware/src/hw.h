/* hw.h — the STM32H743 registers this firmware touches, and nothing else.
 *
 * Hand-written rather than pulled from CMSIS/HAL: the whole point of this
 * firmware is that it is small enough to audit in one sitting.  Offsets are
 * from RM0433 (STM32H742/743/753 reference manual).
 */
#ifndef HW_H
#define HW_H

#include <stdint.h>

#define REG32(addr) (*(volatile uint32_t *)(addr))

/* ---- Cortex-M7 core ---------------------------------------------------- */
#define SCB_VTOR        REG32(0xE000ED08UL)

/* ---- RCC (0x5802_4400) ------------------------------------------------- */
#define RCC_BASE        0x58024400UL
#define RCC_CR          REG32(RCC_BASE + 0x000)
#define RCC_CFGR        REG32(RCC_BASE + 0x010)
#define RCC_D1CFGR      REG32(RCC_BASE + 0x018)
#define RCC_D2CFGR      REG32(RCC_BASE + 0x01C)
#define RCC_AHB4ENR     REG32(RCC_BASE + 0x0E0)
#define RCC_APB2ENR     REG32(RCC_BASE + 0x0F0)

#define RCC_CR_HSION    (1UL << 0)
#define RCC_CR_HSIRDY   (1UL << 2)
#define RCC_CR_HSEON    (1UL << 16)
#define RCC_CR_HSERDY   (1UL << 17)

#define RCC_CFGR_SW_HSI 0UL
#define RCC_CFGR_SW_HSE 2UL
#define RCC_CFGR_SW_MSK 7UL
#define RCC_CFGR_SWS_SH 3

#define RCC_AHB4ENR_GPIOAEN (1UL << 0)
#define RCC_AHB4ENR_GPIOEEN (1UL << 4)
#define RCC_APB2ENR_TIM1EN  (1UL << 0)
#define RCC_APB2ENR_USART1EN (1UL << 4)

/* ---- GPIO -------------------------------------------------------------- */
#define GPIOA_BASE      0x58020000UL
#define GPIOE_BASE      0x58021000UL
#define GPIO_MODER(b)   REG32((b) + 0x00)
#define GPIO_OTYPER(b)  REG32((b) + 0x04)
#define GPIO_OSPEEDR(b) REG32((b) + 0x08)
#define GPIO_PUPDR(b)   REG32((b) + 0x0C)
#define GPIO_BSRR(b)    REG32((b) + 0x18)
#define GPIO_AFRL(b)    REG32((b) + 0x20)
#define GPIO_AFRH(b)    REG32((b) + 0x24)

#define GPIO_MODE_OUT   1UL
#define GPIO_MODE_AF    2UL

/* ---- TIM1 (advanced-control, APB2) ------------------------------------- */
#define TIM1_BASE       0x40010000UL
#define TIM1_CR1        REG32(TIM1_BASE + 0x00)
#define TIM1_SR         REG32(TIM1_BASE + 0x10)
#define TIM1_EGR        REG32(TIM1_BASE + 0x14)
#define TIM1_CCMR1      REG32(TIM1_BASE + 0x18)
#define TIM1_CCMR2      REG32(TIM1_BASE + 0x1C)
#define TIM1_CCER       REG32(TIM1_BASE + 0x20)
#define TIM1_CNT        REG32(TIM1_BASE + 0x24)
#define TIM1_PSC        REG32(TIM1_BASE + 0x28)
#define TIM1_ARR        REG32(TIM1_BASE + 0x2C)
#define TIM1_CCR1       REG32(TIM1_BASE + 0x34)
#define TIM1_CCR2       REG32(TIM1_BASE + 0x38)
#define TIM1_CCR3       REG32(TIM1_BASE + 0x3C)
#define TIM1_CCR4       REG32(TIM1_BASE + 0x40)
#define TIM1_BDTR       REG32(TIM1_BASE + 0x44)

#define TIM_SR_UIF      (1UL << 0)
#define TIM_CR1_CEN     (1UL << 0)
#define TIM_CR1_ARPE    (1UL << 7)
#define TIM_EGR_UG      (1UL << 0)
/* Both CCMRx registers lay their two channels out identically: the low half
 * is the even channel, the high half the odd one, 8 bits apart. */
#define TIM_CCMR_LO_PWM1 ((6UL << 4) | (1UL << 3))   /* OCxM=PWM1, OCxPE */
#define TIM_CCMR_HI_PWM1 ((6UL << 12) | (1UL << 11))
#define TIM_CCER_CCxE(n) (1UL << (4 * (n)))          /* n = 0..3 */
#define TIM_CCER_CCxP(n) (1UL << (4 * (n) + 1))
#define TIM_BDTR_MOE    (1UL << 15)     /* advanced timers need this */

/* ---- USART1 (APB2) ----------------------------------------------------- */
#define USART1_BASE     0x40011000UL
#define USART1_CR1      REG32(USART1_BASE + 0x00)
#define USART1_BRR      REG32(USART1_BASE + 0x0C)
#define USART1_ISR      REG32(USART1_BASE + 0x1C)
#define USART1_RDR      REG32(USART1_BASE + 0x24)
#define USART1_TDR      REG32(USART1_BASE + 0x28)

#define USART_CR1_UE    (1UL << 0)
#define USART_CR1_RE    (1UL << 2)
#define USART_CR1_TE    (1UL << 3)
#define USART_ISR_RXNE  (1UL << 5)
#define USART_ISR_TXE   (1UL << 7)

#endif /* HW_H */
