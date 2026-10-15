/*
 * display.c - terminal output helpers shared by server and client.
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "display.h"

/* Set once per process by ui_init(); read-only afterwards. */
static bool use_colour;
static bool use_unicode;
static pthread_once_t init_once = PTHREAD_ONCE_INIT;

static void init(void)
{
    const char *ascii = getenv("ENC_ASCII");
    const char *unicode = getenv("ENC_UNICODE");
    const char *no_colour = getenv("NO_COLOR");     /* set and not empty: no colours */

    use_colour = isatty(STDOUT_FILENO) && (no_colour == NULL || no_colour[0] == '\0');
    use_unicode = (use_colour || (unicode != NULL && strcmp(unicode, "1") == 0)) &&
                  (ascii == NULL || strcmp(ascii, "1") != 0);
}

void ui_init(void)
{
    pthread_once(&init_once, init);
}

const char *ui(ui_style_t style)
{
    static const char *const codes[] = {
        [UI_RESET] = "\033[0m",   [UI_BOLD] = "\033[1m",    [UI_DIM] = "\033[2m",
        [UI_RED] = "\033[1;31m",  [UI_GREEN] = "\033[1;32m", [UI_YELLOW] = "\033[33m",
        [UI_CYAN] = "\033[36m",
    };

    return use_colour ? codes[style] : "";
}

bool ui_live(void)
{
    return use_colour;
}

const char *ui_ok(void)    { return use_unicode ? "\xe2\x9c\x94" : "OK"; }    /* ✔ */
const char *ui_fail(void)  { return use_unicode ? "\xe2\x9c\x96" : "!!"; }    /* ✖ */
const char *ui_arrow(void) { return use_unicode ? "\xe2\x86\x92" : "->"; }    /* → */

char *ui_size(char *buf, size_t cap, uint64_t bytes)
{
    if (bytes < 1024) {
        snprintf(buf, cap, "%u B", (unsigned)bytes);
    } else if (bytes < 1024u * 1024u) {
        snprintf(buf, cap, "%.1f KB", (double)bytes / 1024.0);
    } else if (bytes < 1024ull * 1024u * 1024u) {
        snprintf(buf, cap, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else {
        snprintf(buf, cap, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    }
    return buf;
}

char *ui_count(char *buf, size_t cap, uint64_t n)
{
    char digits[32];
    size_t len, out = 0;

    snprintf(digits, sizeof digits, "%llu", (unsigned long long)n);
    len = strlen(digits);
    for (size_t i = 0; i < len && out + 1 < cap; i++) {
        if (i > 0 && (len - i) % 3 == 0 && out + 2 < cap) {
            buf[out++] = ',';
        }
        buf[out++] = digits[i];
    }
    buf[out] = '\0';
    return buf;
}

char *ui_ms(char *buf, size_t cap, uint64_t ns)
{
    double ms = (double)ns / 1e6;

    snprintf(buf, cap, ms < 100.0 ? "%.3f ms" : "%.1f ms", ms);
    return buf;
}

char *ui_bar(char *buf, size_t cap, unsigned filled, unsigned width)
{
    const char *full = use_unicode ? "\xe2\x96\x88" : "#";    /* █ */
    const char *empty = use_unicode ? "\xe2\x96\x91" : ".";   /* ░ */
    size_t out = 0;

    buf[0] = '\0';
    for (unsigned i = 0; i < width; i++) {
        const char *cell = i < filled ? full : empty;
        size_t n = strlen(cell);
        if (out + n + 1 > cap) {
            break;
        }
        memcpy(buf + out, cell, n);
        out += n;
    }
    buf[out] = '\0';
    return buf;
}

#define UI_LABEL_WIDTH 12
#define UI_BAR_WIDTH   30

void ui_title(const char *program, const char *action, const char *detail)
{
    printf("\n%s%s%s  %s%s%s  %s\n", ui(UI_BOLD), program, ui(UI_RESET), ui(UI_CYAN), action,
           ui(UI_RESET), detail);
}

void ui_row(const char *label, const char *fmt, ...)
{
    va_list ap;

    printf("  %s%-*s%s ", ui(UI_DIM), UI_LABEL_WIDTH, label, ui(UI_RESET));
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

void ui_progress(const char *label, uint64_t done, uint64_t total, const char *unit)
{
    char bar[UI_BAR_WIDTH * 4 + 1];
    unsigned filled = total > 0 ? (unsigned)(done * UI_BAR_WIDTH / total) : UI_BAR_WIDTH;
    bool final = done >= total;

    if (!use_colour && !final) {
        return;
    }
    printf("%s  %s%-*s%s %s%s%s  %llu/%llu %s%s", use_colour ? "\r" : "", ui(UI_DIM),
           UI_LABEL_WIDTH, label, ui(UI_RESET), ui(UI_CYAN),
           ui_bar(bar, sizeof bar, filled, UI_BAR_WIDTH), ui(UI_RESET), (unsigned long long)done,
           (unsigned long long)total, unit, final ? "\n" : "");
    fflush(stdout);
}

void ui_hex_row(const char *label, const uint8_t *bytes, size_t n)
{
    printf("    %-11s ", label);
    for (size_t i = 0; i < 16; i++) {
        if (i == 8) {
            putchar(' ');
        }
        if (i < n) {
            printf("%02x ", bytes[i]);
        } else {
            printf("   ");
        }
    }
    printf("  %s", ui(UI_DIM));
    for (size_t i = 0; i < n && i < 16; i++) {
        putchar(bytes[i] >= 0x20 && bytes[i] < 0x7f ? bytes[i] : '.');
    }
    printf("%s\n", ui(UI_RESET));
}

void ui_error(const char *program, const char *fmt, ...)
{
    va_list ap;

    fflush(stdout);
    fprintf(stderr, "%s: %s%s ", program, ui(UI_RED), ui_fail());
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s\n", ui(UI_RESET));
}

const char *ui_s(uint64_t n)
{
    return n == 1 ? "" : "s";
}

char *ui_rate(char *buf, size_t cap, uint64_t bytes, uint64_t ns)
{
    double per_s = ns > 0 ? (double)bytes / ((double)ns / 1e9) : 0.0;

    if (per_s >= 1024.0 * 1024.0) {
        snprintf(buf, cap, "%.1f MB/s", per_s / (1024.0 * 1024.0));
    } else {
        snprintf(buf, cap, "%.1f KB/s", per_s / 1024.0);
    }
    return buf;
}
