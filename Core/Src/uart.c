#include "uart.h"
#include "stm32f4xx.h"

void uart_init(uint32_t baud)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC->APB1ENR |= RCC_APB1ENR_USART2EN;
    (void)RCC->APB1ENR;

    /* PA2 = USART2_TX, PA3 = USART2_RX, AF7 */
    GPIOA->MODER   &= ~((3u << (2 * 2)) | (3u << (3 * 2)));
    GPIOA->MODER   |=  ((2u << (2 * 2)) | (2u << (3 * 2)));
    GPIOA->AFR[0]  &= ~((0xFu << (2 * 4)) | (0xFu << (3 * 4)));
    GPIOA->AFR[0]  |=  ((7u << (2 * 4)) | (7u << (3 * 4)));
    GPIOA->OSPEEDR |=  ((3u << (2 * 2)) | (3u << (3 * 2)));

    uint32_t pclk1 = SystemCoreClock >> 1;      /* APB1 = HCLK / 2 */
    USART2->BRR = (pclk1 + baud / 2) / baud;    /* 16x oversampling */
    USART2->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
}

static void uart_putc(char c)
{
    while (!(USART2->SR & USART_SR_TXE)) { }
    USART2->DR = (uint8_t)c;
}

void uart_puts(const char *s)
{
    while (*s) {
        uart_putc(*s++);
    }
}

void uart_put_u32(uint32_t v)
{
    char buf[10];
    int n = 0;
    do {
        buf[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) {
        uart_putc(buf[--n]);
    }
}
