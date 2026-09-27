/*
 * battery-config: configurable battery warning for the DJI FPV Goggles V1
 * and V2.
 *
 * gs_get_pigeon_battery_info detects the cell count once, on the first
 * reading after the goggles app starts:
 *
 *     cells = (uint)(voltage * 100.0 / 430.0 + 1.0)
 *
 * and warns (on-screen message + buzzer) when
 *
 *     voltage < cells * 3.5
 *
 * Both are patched in memory:
 *
 *  - Threshold: vmov.f64 d19, #3.5 (F0 EE 0C 3B). Its VFP immediate encodes
 *    (1 + n/16) * 2 V with n in the low nibble of byte 2, so every 0.125 V
 *    step between 2.0 and 3.875 V is a one-byte change.
 *
 *  - Cell count: the "+ 1.0" vadd.f32 s6, s4, s5 is replaced with
 *    vmov.f32 s6, #cells. Detection guesses too few cells when a pack is
 *    powered on below 4.3 V * (cells - 1), e.g. a 4S Li-ion under 12.9 V.
 *
 * A patch is only applied when its instruction sequence is found exactly
 * once and still holds the stock bytes.
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

/* vmov.f64 d19, #3.5 */
static const uint8_t THRESHOLD_SEQ[] = { 0xF0, 0xEE, 0x0C, 0x3B };
#define THRESHOLD_OFFSET 2
#define MIN_MV   2000
#define MAX_MV   3875
#define STEP_MV  125

/* vmul.f32 s2,s0,s1; vmov.f32 s5,#1.0; vdiv.f32 s4,s2,s3;
 * vadd.f32 s6,s4,s5; vcvt.u32.f32 s7,s6 */
static const uint8_t CELLS_SEQ[] = {
    0x20, 0xEE, 0x20, 0x1A,  0xF7, 0xEE, 0x00, 0x2A,  0x81, 0xEE, 0x21, 0x2A,
    0x32, 0xEE, 0x22, 0x3A,  0xFC, 0xEE, 0xC3, 0x3A,
};
#define CELLS_OFFSET 12
#define MIN_CELLS 2
#define MAX_CELLS 4

/* Goggles app as started by the wtfOS modloader: V2, V1. */
static const char *const TARGETS[] = {
    "/dji_gls_wm150_original",
    "/dji_glasses_original",
};

#define MAX_RANGES 8
static uintptr_t range_start[MAX_RANGES], range_end[MAX_RANGES];
static int range_count;

/* Returns the integer value of "key" in the config file, or -1. */
static long read_config(const char *key)
{
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f)
        return -1;

    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    char quoted[64];
    snprintf(quoted, sizeof(quoted), "\"%s\"", key);
    char *k = strstr(buf, quoted);
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

/*
 * Collects the app's executable mappings. Adjacent mappings are merged,
 * since mprotect() calls split a segment.
 */
static void load_ranges(void)
{
    FILE *m = fopen("/proc/self/maps", "r");
    if (!m)
        return;

    char line[512];
    while (fgets(line, sizeof(line), m)) {
        unsigned long s, e;
        char perms[5], path[400] = "";
        if (sscanf(line, "%lx-%lx %4s %*s %*s %*s %399[^\n]", &s, &e, perms, path) < 3)
            continue;
        if (perms[2] != 'x' || !is_target(path))
            continue;
        if (range_count && range_end[range_count - 1] == s)
            range_end[range_count - 1] = e;
        else if (range_count < MAX_RANGES) {
            range_start[range_count] = s;
            range_end[range_count] = e;
            range_count++;
        }
    }
    fclose(m);
}

/* Returns the only occurrence of seq in the app's code, or NULL. */
static uint8_t *find_unique(const uint8_t *seq, size_t len)
{
    uint8_t *hit = NULL;
    int count = 0;
    for (int r = 0; r < range_count; r++) {
        for (uint8_t *p = (uint8_t *)range_start[r]; p + len <= (uint8_t *)range_end[r]; p++) {
            if (memcmp(p, seq, len) == 0) {
                hit = p;
                count++;
            }
        }
    }
    return count == 1 ? hit : NULL;
}

static int write_code(uint8_t *addr, const uint8_t *bytes, size_t len)
{
    long pg = sysconf(_SC_PAGESIZE);
    uintptr_t first = (uintptr_t)addr & ~(uintptr_t)(pg - 1);
    size_t span = (uintptr_t)addr + len - first;

    if (mprotect((void *)first, span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        LOG("mprotect failed: %s", strerror(errno));
        return -1;
    }
    memcpy(addr, bytes, len);
    __builtin___clear_cache((char *)addr, (char *)addr + len);
    mprotect((void *)first, span, PROT_READ | PROT_EXEC);
    return 0;
}

/* Replaces len bytes at offset in the unique occurrence of seq. */
static int patch(const char *name, const uint8_t *seq, size_t seq_len,
                 size_t offset, const uint8_t *bytes, size_t len)
{
    uint8_t *hit = find_unique(seq, seq_len);
    if (!hit) {
        LOG("%s: instruction not found exactly once; not patching", name);
        return -1;
    }
    return write_code(hit + offset, bytes, len);
}

static void apply_threshold(void)
{
    long mv = read_config("per_cell_mv");
    if (mv < MIN_MV || mv > MAX_MV) {
        LOG("threshold: no valid per_cell_mv, keeping stock");
        return;
    }

    uint8_t imm = (uint8_t)((mv - MIN_MV + STEP_MV / 2) / STEP_MV);
    if (imm == THRESHOLD_SEQ[THRESHOLD_OFFSET])
        return;

    if (patch("threshold", THRESHOLD_SEQ, sizeof(THRESHOLD_SEQ), THRESHOLD_OFFSET, &imm, 1) == 0)
        LOG("warning threshold set to %ld mV/cell", (long)(MIN_MV + imm * STEP_MV));
}

static void apply_cells(void)
{
    /* VFP immediates for 2.0, 3.0 and 4.0 */
    static const uint8_t VFP_IMM[] = { 0x00, 0x08, 0x10 };

    long cells = read_config("cells");
    if (cells < MIN_CELLS || cells > MAX_CELLS)
        return;

    /* vmov.f32 s6, #cells */
    uint8_t imm = VFP_IMM[cells - MIN_CELLS];
    uint8_t insn[4] = { 0xB0 | (imm >> 4), 0xEE, imm & 0x0F, 0x3A };

    if (patch("cells", CELLS_SEQ, sizeof(CELLS_SEQ), CELLS_OFFSET, insn, sizeof(insn)) == 0)
        LOG("cell count set to %ldS", cells);
}

__attribute__((constructor))
static void battery_config_init(void)
{
    load_ranges();
    apply_threshold();
    apply_cells();
}
