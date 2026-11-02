/* Lab bench — BOOTSEL / RUN lines of the host board. See bench_io.c. */
#ifndef BENCH_IO_H
#define BENCH_IO_H

/* Releases both lines (open drain, floating) and registers the console commands. Call it as early
 * as possible in app_main, so the lines do not float into a "pressed" state for long. */
void bench_io_start(void);

#endif
