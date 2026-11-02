/* Lab bench — line-based command console shared by all modules. See lab_console.c. */
#ifndef LAB_CONSOLE_H
#define LAB_CONSOLE_H

/* Handler of one command. `arg` is the rest of the line after the command word (NULL if none).
 * It runs in the console task and may block (e.g. a button press of a few hundred ms). It must
 * answer with exactly one lab_console_reply(). */
typedef void (*lab_cmd_fn)(char *arg);

/* Register a command (before or after lab_console_start). `usage` is shown in the error for an
 * unknown command, e.g. "press [ms]". Up to LAB_CONSOLE_MAX_CMDS commands. */
#define LAB_CONSOLE_MAX_CMDS 32
void lab_console_register(const char *name, const char *usage, lab_cmd_fn fn);

/* Reply line: "BENCH <text>". Convention: "OK ..." or "ERR ...". Safe from any task. */
void lab_console_reply(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Start reading commands from the console UART. Built-in command: ping. */
void lab_console_start(void);

#endif
