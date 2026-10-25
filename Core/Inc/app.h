#ifndef APP_H
#define APP_H

/* Pick which experiment to run. */
#define DEMO_CONTEXT_SWITCH 1   /* two equal-priority tasks, round-robin */
#define DEMO_RM_SCHEDULER   2   /* T1/T2/T3 = 10/20/50 ms, response times over UART */
#define DEMO_PRIORITY_INV   3   /* H/M/L with a shared mutex (see PRIORITY_INHERITANCE_ENABLED) */

#ifndef DEMO
#define DEMO DEMO_RM_SCHEDULER
#endif

void app_run(void);   /* does not return */

#endif
