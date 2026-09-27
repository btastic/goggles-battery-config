/*
 * battery-config: configurable battery warning threshold for the DJI FPV
 * Goggles V1 and V2.
 *
 * The goggles warn (on-screen message + buzzer) when
 *
 *     pack_voltage < cell_count * 3.5
 *
 * The 3.5 is a single instruction in gs_get_pigeon_battery_info:
 *
 *     vmov.f64 d19, #3.5    F0 EE 0C 3B
 *
 * Its VFP immediate encodes (1 + n/16) * 2 V, where n is the low nibble of
 * byte 2, so every 0.125 V step between 2.0 and 3.875 V is a one-byte change.
 * This library is preloaded into the goggles app, finds the instruction and
 * rewrites that byte in memory. It patches nothing unless the instruction is
 * found exactly once and still holds the stock value.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <android/log.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "battery-config", __VA_ARGS__)

#define CONFIG_PATH "/opt/etc/package-config/battery-config/config.json"
#define CONFIG_KEY  "\"per_cell_mv\""

static const uint8_t PATTERN[4] = { 0xF0, 0xEE, 0x0C, 0x3B };
#define IMM_OFFSET  2
#define STOCK_IMM   0x0C

#define MIN_MV   2000
#define MAX_MV   3875
#define STEP_MV  125

/* Goggles app as started by the wtfOS modloader: V2, V1. */
static const char *const TARGETS[] = {
    "/dji_gls_wm150_original",
    "/dji_glasses_original",
};

/* Returns the configured per-cell threshold in mV, or -1. */
static long read_config_mv(void)
{
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f)
        return -1;

    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    char *k = strstr(buf, CONFIG_KEY);
    if (!k || !(k = strchr(k, ':')))
        return -1;
    return strtol(k + 1, NULL, 10);
}

static int is_target(const char *path)
{
    size_t plen = strlen(path);
    for (size_t i = 0; i < sizeof(TARGETS) / sizeof(TARGETS[0]); i++) {
        size_t tlen = strlen(TARGETS[i]);
        if (plen >= tlen && strcmp(path + plen - tlen, TARGETS[i]) == 0)
            return 1;
    }
    return 0;
}

static void scan(uintptr_t start, uintptr_t end, uint8_t **hit, int *count)
{
    for (uint8_t *p = (uint8_t *)start; p + sizeof(PATTERN) <= (uint8_t *)end; p++) {
        if (memcmp(p, PATTERN, sizeof(PATTERN)) == 0) {
            *hit = p;
            (*count)++;
        }
    }
}

/*
 * Scans the app's executable mappings for PATTERN. Adjacent mappings are
 * scanned as one range, since mprotect() calls split a segment.
 */
static uint8_t *find_instruction(int *count)
{
    FILE *m = fopen("/proc/self/maps", "r");
    if (!m)
        return NULL;

    char line[512];
    uintptr_t run_start = 0, run_end = 0;
    uint8_t *hit = NULL;
    *count = 0;

    while (fgets(line, sizeof(line), m)) {
        unsigned long s, e;
        char perms[5], path[400] = "";
        if (sscanf(line, "%lx-%lx %4s %*s %*s %*s %399[^\n]", &s, &e, perms, path) < 3)
            continue;
        if (perms[2] != 'x' || !is_target(path))
            continue;
        if (s == run_end) {
            run_end = e;
            continue;
        }
        if (run_start)
            scan(run_start, run_end, &hit, count);
        run_start = s;
        run_end = e;
    }
    fclose(m);

    if (run_start)
        scan(run_start, run_end, &hit, count);
    return hit;
}

static int patch_byte(uint8_t *addr, uint8_t value)
{
    long pg = sysconf(_SC_PAGESIZE);
    void *page = (void *)((uintptr_t)addr & ~(uintptr_t)(pg - 1));

    if (mprotect(page, pg, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        LOG("mprotect failed: %s", strerror(errno));
        return -1;
    }
    *addr = value;
    __builtin___clear_cache((char *)addr, (char *)addr + 1);
    mprotect(page, pg, PROT_READ | PROT_EXEC);
    return 0;
}

__attribute__((constructor))
static void battery_config_init(void)
{
    long mv = read_config_mv();
    if (mv < MIN_MV || mv > MAX_MV) {
        LOG("no valid %s in %s, keeping stock threshold", CONFIG_KEY, CONFIG_PATH);
        return;
    }

    uint8_t imm = (uint8_t)((mv - MIN_MV + STEP_MV / 2) / STEP_MV);
    if (imm == STOCK_IMM) {
        LOG("stock threshold configured, nothing to do");
        return;
    }

    int count;
    uint8_t *insn = find_instruction(&count);
    if (count != 1) {
        LOG("threshold instruction found %d times, expected 1; not patching", count);
        return;
    }

    uint8_t *target = insn + IMM_OFFSET;
    if (*target != STOCK_IMM) {
        LOG("unexpected immediate 0x%02x; not patching", *target);
        return;
    }

    if (patch_byte(target, imm) == 0)
        LOG("warning threshold set to %ld mV/cell", (long)(MIN_MV + imm * STEP_MV));
}
