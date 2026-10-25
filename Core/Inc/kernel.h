#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>
#include <stddef.h>

/* Set to 0 to reproduce unbounded priority inversion, 1 to enable PIP. */
#ifndef PRIORITY_INHERITANCE_ENABLED
#define PRIORITY_INHERITANCE_ENABLED 1
#endif

#define KERNEL_MAX_TASKS   8
#define KERNEL_IDLE_PRIO   255   /* numerically lower = higher priority */

typedef enum {
    TASK_READY = 0,     /* running or runnable */
    TASK_SLEEPING,      /* waiting for wake_tick */
    TASK_BLOCKED        /* waiting on a mutex */
} task_state_t;

struct mutex;

/* sp must stay the first member: PendSV reads/writes it through the TCB pointer. */
typedef struct tcb {
    uint32_t       *sp;
    uint8_t         priority;       /* effective (may be boosted by PIP) */
    uint8_t         base_priority;
    task_state_t    state;
    uint32_t        wake_tick;
    uint32_t        release_cyc;    /* DWT cycle count when last released */
    struct mutex   *blocked_on;
    const char     *name;
} tcb_t;

typedef struct mutex {
    tcb_t *owner;
} mutex_t;

extern tcb_t *volatile currentTCB;

void     kernel_init(void);
tcb_t   *kernel_task_create(void (*entry)(void), uint32_t *stack, uint32_t words,
                            uint8_t priority, const char *name);
void     kernel_start(void);                 /* does not return */

uint32_t kernel_ticks(void);                 /* 1 ms ticks since kernel_start */
void     kernel_sleep_until(uint32_t wake_tick);
void     kernel_tick(void);                  /* call from SysTick_Handler */

void     mutex_lock(mutex_t *m);
void     mutex_unlock(mutex_t *m);

/* DWT cycle counter (free-running, 1 count per CPU clock) */
void     dwt_init(void);
static inline uint32_t dwt_cycles(void) { return *(volatile uint32_t *)0xE0001004u; }

#endif
