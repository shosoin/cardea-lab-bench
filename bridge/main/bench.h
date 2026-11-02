/* se05x-lab-bridge — bench helper (BOOTSEL / RUN lines of the host board). See bench.c. */
#ifndef BRIDGE_BENCH_H
#define BRIDGE_BENCH_H

/* Configures both lines as open-drain outputs (released) and starts the console command task. */
void bench_start(void);

#endif
