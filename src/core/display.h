/*
 * display.h - terminal output helpers shared by server and client:
 * colours (only on a terminal), human-readable sizes and progress bars.
 */
#ifndef ENC_DISPLAY_H
#define ENC_DISPLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef enum { UI_RESET, UI_BOLD, UI_DIM, UI_RED, UI_GREEN, UI_YELLOW, UI_CYAN } ui_style_t;

/*
 * Turn colours and Unicode symbols on if stdout is a terminal and NO_COLOR is not
 * set. ENC_ASCII=1 keeps colours but uses plain ASCII symbols; ENC_UNICODE=1
 * uses Unicode symbols even when not on a terminal (for logs shown later).
 */
void ui_init(void);

/* Escape sequence for a style, or "" when colours are off. */
const char *ui(ui_style_t style);

/* True when stdout is a terminal with colours on: progress lines redraw in place. */
bool ui_live(void);

/* Symbols: tick, cross, arrow; ASCII fallbacks when Unicode is off. */
const char *ui_ok(void);
const char *ui_fail(void);
const char *ui_arrow(void);

/* "512 B", "48.2 KB", "2.0 MB". Returns buf. */
char *ui_size(char *buf, size_t cap, uint64_t bytes);

/* "3,565,210". Returns buf. */
char *ui_count(char *buf, size_t cap, uint64_t n);

/* "12.345 ms" style time from nanoseconds, 3 decimals below 100 ms. Returns buf. */
char *ui_ms(char *buf, size_t cap, uint64_t ns);

/* "s" unless n is 1, for plurals. */
const char *ui_s(uint64_t n);

/* "41.9 MB/s" or "312.5 KB/s" for bytes moved in ns nanoseconds. Returns buf. */
char *ui_rate(char *buf, size_t cap, uint64_t bytes, uint64_t ns);

/* A bar of `filled` out of `width` cells. Returns buf. */
char *ui_bar(char *buf, size_t cap, unsigned filled, unsigned width);

/* "encclient  encrypt  detail" title line on stdout. */
void ui_title(const char *program, const char *action, const char *detail);

/* One "  label        text" row on stdout. */
void ui_row(const char *label, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/*
 * Progress row: "  progress     ██████░░░░  3/6 Data Blocks". On a terminal it redraws
 * in place; otherwise only the final call (done == total) prints.
 */
void ui_progress(const char *label, uint64_t done, uint64_t total, const char *unit);

/* Hex + text row of up to 16 bytes: "    original   48 65 6c ...   Hello". */
void ui_hex_row(const char *label, const uint8_t *bytes, size_t n);

/* "program: ✖ message" on stderr, in red on a terminal. */
void ui_error(const char *program, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif /* ENC_DISPLAY_H */
