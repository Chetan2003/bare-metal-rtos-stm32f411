#include "app.h"
#include "kernel.h"
#include "uart.h"
#include "stm32f4xx.h"

#define STACK_WORDS      256
#define IDLE_STACK_WORDS 384
#define LOG_SIZE         64

/* ---- busy-loop load, calibrated at startup ---------------------------- */

static uint32_t cycles_per_iter_q8;     /* CPU cycles per loop iteration, x256 */

static void busy_iters(uint32_t n)
{
    for (volatile uint32_t i = 0; i < n; i++) { }
}

static void calibrate(void)
{
    const uint32_t n = 100000;
    uint32_t t0 = dwt_cycles();
    busy_iters(n);
    uint32_t cyc = dwt_cycles() - t0;
    cycles_per_iter_q8 = (uint32_t)(((uint64_t)cyc << 8) / n);
}

static void busy_us(uint32_t us)
{
    uint64_t cyc = (uint64_t)us * (SystemCoreClock / 1000000u);
    busy_iters((uint32_t)((cyc << 8) / cycles_per_iter_q8));
}

static uint32_t cycles_to_us(uint32_t cyc)
{
    return cyc / (SystemCoreClock / 1000000u);
}

/* ---- log ring buffer, drained by the idle task ------------------------ */

typedef struct {
    const char *tag;
    uint32_t    a;
    uint32_t    b;
} log_entry_t;

static volatile log_entry_t log_buf[LOG_SIZE];
static volatile uint32_t    log_head, log_tail, log_dropped;

static void log_put(const char *tag, uint32_t a, uint32_t b)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    uint32_t next = (log_head + 1) % LOG_SIZE;
    if (next == log_tail) {
        log_dropped++;
    } else {
        log_buf[log_head].tag = tag;
        log_buf[log_head].a   = a;
        log_buf[log_head].b   = b;
        log_head = next;
    }
    __set_PRIMASK(primask);
}

static void log_drain(void)
{
    while (log_tail != log_head) {
        log_entry_t e = log_buf[log_tail];
        log_tail = (log_tail + 1) % LOG_SIZE;
        uart_puts(e.tag);
        uart_puts(",");
        uart_put_u32(e.a);
        uart_puts(",");
        uart_put_u32(e.b);
        uart_puts("\r\n");
    }
}

/* ---- idle task --------------------------------------------------------- */

static uint32_t idle_stack[IDLE_STACK_WORDS];

static void idle_task(void)
{
    for (;;) {
        log_drain();
        __WFI();
    }
}

/* ---- demo 1: context switch ------------------------------------------- */
#if DEMO == DEMO_CONTEXT_SWITCH

static volatile uint32_t task1_ticks, task2_ticks;
static uint32_t stack_a[STACK_WORDS], stack_b[STACK_WORDS];

static void task_a(void) { for (;;) { busy_iters(200000); task1_ticks++; } }
static void task_b(void) { for (;;) { busy_iters(600000); task2_ticks++; } }

#endif

/* ---- demo 2: RM scheduler --------------------------------------------- */
#if DEMO == DEMO_RM_SCHEDULER

static uint32_t stack_t1[STACK_WORDS], stack_t2[STACK_WORDS], stack_t3[STACK_WORDS];

static void periodic_task(const char *tag, uint32_t period_ms, uint32_t c_us)
{
    uint32_t next = 0, job = 0;
    for (;;) {
        busy_us(c_us);
        log_put(tag, job++, cycles_to_us(dwt_cycles() - currentTCB->release_cyc));
        next += period_ms;
        kernel_sleep_until(next);
    }
}

static void t1_task(void) { periodic_task("T1", 10, 1400); }
static void t2_task(void) { periodic_task("T2", 20, 7400); }
static void t3_task(void) { periodic_task("T3", 50, 5000); }

#endif

/* ---- demo 3: priority inversion --------------------------------------- */
#if DEMO == DEMO_PRIORITY_INV

static uint32_t stack_h[STACK_WORDS], stack_m[STACK_WORDS], stack_l[STACK_WORDS];
static mutex_t  shared;
static volatile uint32_t m_preempts;

#define L_CS_US 25000u

static void h_task(void)
{
    uint32_t next = 0;
    for (;;) {
        uint32_t t0 = dwt_cycles();
        mutex_lock(&shared);
        uint32_t wait = cycles_to_us(dwt_cycles() - t0);
        uint32_t mp = m_preempts;
        busy_us(100);
        mutex_unlock(&shared);
        log_put("H", wait, mp);
        next += 100;
        kernel_sleep_until(next);
    }
}

static void m_task(void)
{
    uint32_t next = 0;
    for (;;) {
        if (shared.owner != NULL) {
            m_preempts++;
        }
        busy_us(1000);
        next += 5;
        kernel_sleep_until(next);
    }
}

static void l_task(void)
{
    uint32_t next = 0;
    for (;;) {
        mutex_lock(&shared);
        m_preempts = 0;
        busy_us(L_CS_US);
        mutex_unlock(&shared);
        next += 95;
        kernel_sleep_until(next);
    }
}

#endif

/* ---- entry -------------------------------------------------------------- */

void app_run(void)
{
    uart_init(115200);
    kernel_init();
    calibrate();

    uart_puts("\r\nRTOS demo=");
    uart_put_u32(DEMO);
    uart_puts(" cycles/iter x256=");
    uart_put_u32(cycles_per_iter_q8);
    uart_puts(" pip=");
    uart_put_u32(PRIORITY_INHERITANCE_ENABLED);
    uart_puts("\r\n");

#if DEMO == DEMO_CONTEXT_SWITCH
    kernel_task_create(task_a, stack_a, STACK_WORDS, 2, "A");
    kernel_task_create(task_b, stack_b, STACK_WORDS, 2, "B");
#elif DEMO == DEMO_RM_SCHEDULER
    kernel_task_create(t1_task, stack_t1, STACK_WORDS, 1, "T1");
    kernel_task_create(t2_task, stack_t2, STACK_WORDS, 2, "T2");
    kernel_task_create(t3_task, stack_t3, STACK_WORDS, 3, "T3");
#elif DEMO == DEMO_PRIORITY_INV
    kernel_task_create(h_task, stack_h, STACK_WORDS, 1, "H");
    kernel_task_create(m_task, stack_m, STACK_WORDS, 2, "M");
    kernel_task_create(l_task, stack_l, STACK_WORDS, 3, "L");
#endif
    kernel_task_create(idle_task, idle_stack, IDLE_STACK_WORDS, KERNEL_IDLE_PRIO, "idle");

    kernel_start();
    for (;;) { }
}
