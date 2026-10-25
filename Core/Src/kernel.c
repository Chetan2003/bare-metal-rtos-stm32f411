#include "kernel.h"
#include "stm32f4xx.h"

#define STACK_FILL 0xA5A5A5A5u

static tcb_t    tasks[KERNEL_MAX_TASKS];
static uint32_t num_tasks;
static volatile uint32_t tick_count;
static volatile uint32_t kernel_running;

tcb_t *volatile currentTCB;

void dwt_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

void kernel_init(void)
{
    num_tasks = 0;
    tick_count = 0;
    kernel_running = 0;
    dwt_init();
}

static void task_exit_trap(void)
{
    for (;;) { }
}

tcb_t *kernel_task_create(void (*entry)(void), uint32_t *stack, uint32_t words,
                          uint8_t priority, const char *name)
{
    if (num_tasks >= KERNEL_MAX_TASKS) {
        return NULL;
    }

    for (uint32_t i = 0; i < words; i++) {
        stack[i] = STACK_FILL;
    }

    /* Hand-built frame: hardware-stacked xPSR/PC/LR/R12/R3-R0 on top,
     * software-saved R4-R11 below, exactly what PendSV pops on first dispatch. */
    uint32_t *sp = (uint32_t *)(((uint32_t)(stack + words)) & ~7u);
    *--sp = 0x01000000u;                /* xPSR (Thumb bit) */
    *--sp = (uint32_t)entry;            /* PC */
    *--sp = (uint32_t)task_exit_trap;   /* LR */
    *--sp = 0;                          /* R12 */
    *--sp = 0;                          /* R3 */
    *--sp = 0;                          /* R2 */
    *--sp = 0;                          /* R1 */
    *--sp = 0;                          /* R0 */
    for (int i = 0; i < 8; i++) {
        *--sp = 0;                      /* R11..R4 */
    }

    tcb_t *t = &tasks[num_tasks++];
    t->sp            = sp;
    t->priority      = priority;
    t->base_priority = priority;
    t->state         = TASK_READY;
    t->wake_tick     = 0;
    t->release_cyc   = 0;
    t->blocked_on    = NULL;
    t->name          = name;
    return t;
}

/* Pick the highest-priority ready task. Scanning starts after the current
 * task and only a strictly better priority displaces the pick, so equal
 * priorities rotate round-robin. Runs inside PendSV. */
void kernel_schedule(void);
void kernel_schedule(void)
{
    uint32_t start = (uint32_t)(currentTCB - tasks);
    tcb_t *best = NULL;

    for (uint32_t i = 1; i <= num_tasks; i++) {
        tcb_t *t = &tasks[(start + i) % num_tasks];
        if (t->state != TASK_READY) {
            continue;
        }
        if (best == NULL || t->priority < best->priority) {
            best = t;
        }
    }
    currentTCB = best;
}

__attribute__((naked)) void PendSV_Handler(void)
{
    __asm volatile(
        "mrs   r0, psp            \n"
        "stmdb r0!, {r4-r11}      \n"
        "ldr   r1, =currentTCB    \n"
        "ldr   r2, [r1]           \n"
        "str   r0, [r2]           \n"
        "push  {r1, lr}           \n"
        "bl    kernel_schedule    \n"
        "pop   {r1, lr}           \n"
        "ldr   r2, [r1]           \n"
        "ldr   r0, [r2]           \n"
        "ldmia r0!, {r4-r11}      \n"
        "msr   psp, r0            \n"
        "bx    lr                 \n"
    );
}

__attribute__((naked, noreturn)) static void start_first_task(void)
{
    __asm volatile(
        "ldr   r0, =currentTCB    \n"
        "ldr   r0, [r0]           \n"
        "ldr   r0, [r0]           \n"
        "adds  r0, r0, #32        \n"   /* skip software-saved R4-R11 */
        "msr   psp, r0            \n"
        "movs  r0, #2             \n"
        "msr   control, r0        \n"   /* thread mode uses PSP */
        "isb                      \n"
        "pop   {r0-r3}            \n"
        "pop   {r12}              \n"
        "pop   {lr}               \n"
        "pop   {r4, r5}           \n"   /* r4 = PC, r5 = xPSR */
        "cpsie i                  \n"
        "bx    r4                 \n"
    );
}

void kernel_start(void)
{
    __disable_irq();

    NVIC_SetPriority(PendSV_IRQn, 15);
    NVIC_SetPriority(SysTick_IRQn, 15);

    uint32_t now = dwt_cycles();
    for (uint32_t i = 0; i < num_tasks; i++) {
        tasks[i].release_cyc = now;
    }

    tick_count = 0;
    currentTCB = &tasks[0];
    kernel_schedule();
    kernel_running = 1;

    start_first_task();
}

uint32_t kernel_ticks(void)
{
    return tick_count;
}

void kernel_tick(void)
{
    if (!kernel_running) {
        return;
    }

    tick_count++;
    uint32_t cyc = dwt_cycles();

    for (uint32_t i = 0; i < num_tasks; i++) {
        tcb_t *t = &tasks[i];
        if (t->state == TASK_SLEEPING && (int32_t)(tick_count - t->wake_tick) >= 0) {
            t->state       = TASK_READY;
            t->release_cyc = cyc;
        }
    }

    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
}

void kernel_sleep_until(uint32_t wake_tick)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    if ((int32_t)(wake_tick - tick_count) > 0) {
        currentTCB->wake_tick = wake_tick;
        currentTCB->state     = TASK_SLEEPING;
        SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
    } else {
        /* Release instant already passed: this is an overrun. */
        currentTCB->release_cyc = dwt_cycles();
    }

    __set_PRIMASK(primask);
    __ISB();
}

void mutex_lock(mutex_t *m)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    tcb_t *cur = currentTCB;

    if (m->owner == NULL) {
        m->owner = cur;
        __set_PRIMASK(primask);
        return;
    }

    cur->state      = TASK_BLOCKED;
    cur->blocked_on = m;

#if PRIORITY_INHERITANCE_ENABLED
    if (m->owner->priority > cur->priority) {
        m->owner->priority = cur->priority;
    }
#endif

    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
    __set_PRIMASK(primask);
    __ISB();
    /* Resumes here only after mutex_unlock handed ownership to us. */
}

void mutex_unlock(mutex_t *m)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    tcb_t *cur = currentTCB;
    if (m->owner != cur) {
        __set_PRIMASK(primask);
        return;
    }

    cur->priority = cur->base_priority;

    tcb_t *waiter = NULL;
    for (uint32_t i = 0; i < num_tasks; i++) {
        tcb_t *t = &tasks[i];
        if (t->state == TASK_BLOCKED && t->blocked_on == m &&
            (waiter == NULL || t->priority < waiter->priority)) {
            waiter = t;
        }
    }

    if (waiter != NULL) {
        m->owner           = waiter;
        waiter->blocked_on = NULL;
        waiter->state      = TASK_READY;
    } else {
        m->owner = NULL;
    }

    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
    __set_PRIMASK(primask);
    __ISB();
}
