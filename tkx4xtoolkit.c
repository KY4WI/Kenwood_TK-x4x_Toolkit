/*
 * tkx4xtoolkit - Kenwood TK-x4x service / firmware utility
 * Version: v1.0.0-beta.1
 * Copyright (c) 2026 Dave, KY4WI
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Kenwood TK-840/940/941 firmware and channel backup, restoration,
 * recovery, diagnostics, firmware normalization, and optional enhancements.
 * This implementation combines radio protocol and terminal user-interface
 * code in one translation unit.
 */

#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "fwtransmute.h"

#include "firmware_hex.h"
#include "tkx4x_plus25_patch.h"
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <process.h>
#include <conio.h>
#else
#include <fcntl.h>
#include <dirent.h>
#include <glob.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#endif

#define TOOL_VERSION "v1.0.0-beta.1"
static const char *tool_display_version(void)
{
#if defined(TOOL_GIT_VERSION) && !defined(TOOL_RELEASE_BUILD)
    return TOOL_VERSION " (git " TOOL_GIT_VERSION ")";
#else
    return TOOL_VERSION;
#endif
}


#define ACK 0x06
#define NAK 0x15
#define BLOCK_SIZE 0x80u
#define FLASH_PAGE_SIZE 0x40u
#define HEX_DATA_SIZE 0x10u
#define TIMEOUT_MS 1500

#define MASKROM_START  0x0000u
#define MASKROM_END    0x1FFFu
#define MASKROM_SIZE   (MASKROM_END - MASKROM_START + 1u)

#define PROGRAM_START  0x8000u
#define PROGRAM_END    0xE5FFu
#define PROGRAM_SIZE   (PROGRAM_END - PROGRAM_START + 1u)
#define PROGRAM_BLOCK_COUNT (PROGRAM_SIZE / BLOCK_SIZE)
#define FLASH_PAGE_COUNT    (PROGRAM_SIZE / FLASH_PAGE_SIZE)
#define CHANNEL_START  0xE600u
#define CHANNEL_END    0xFDFFu
#define CHANNEL_SIZE   (CHANNEL_END - CHANNEL_START + 1u)
#define FULL_SIZE      (PROGRAM_SIZE + CHANNEL_SIZE)

#define RECOVERY_ADDR   0xE66Cu
#define RECOVERY_OFFSET (RECOVERY_ADDR - CHANNEL_START)
#define RECOVERY_MASK   0x04u

#define PASSWORD_ADDR   0xE670u
#define PASSWORD_OFFSET (PASSWORD_ADDR - CHANNEL_START)
#define PASSWORD_LENGTH 10u

#define FW_KENWOOD_ADDR   0xE580u
#define FW_KENWOOD_OFFSET (FW_KENWOOD_ADDR - PROGRAM_START)
#define FW_KENWOOD_TEXT   "KENWOOD"
#define FW_KENWOOD_LEN    7u

/* Project-identification fields in patched firmware. */
#define AUTHOR_SIG_OFFSET 0x65D0u
#define AUTHOR_SIG_TEXT   "KY4WI FW MODS  "
#define AUTHOR_SIG_LEN    15u
#define MOD_TAG_OFFSET          0x65DFu
/* Firmware patch metadata: 0x80 prefix plus feature bits; FF = no metadata.
   Bit 0 = complete Enhanced TOT Patch (840 or 940/941).
   Bit 1 = complete Enhanced TX Patch (940/941, both TX paths).
   Bit 2 = reserved for future EEPROM/calibration support.
   Preserve unrelated feature bits. For defensive decoding, recognize the
   alternate 0x40 prefix with separate channel/dealer TX bits (0 and 1)
   and TOT bit 3; normalize these bits before recording patch metadata. */
#define MOD_TAG_PREFIX          0x80u
#define MOD_TAG_COMPAT_PREFIX   0x40u
#define MOD_TAG_PREFIX_MASK     0xC0u
#define MOD_TAG_TOT             0x01u
#define MOD_TAG_TX             0x02u
#define MOD_TAG_EEPROM          0x04u
#define MOD_TAG_KNOWN_MASK      (MOD_TAG_TOT | MOD_TAG_TX | MOD_TAG_EEPROM)
#define MOD_TAG_NO_FIXED_OFFSET (MOD_TAG_PREFIX | MOD_TAG_TX)

#define PATCH_SELECT_TX       0x01u
#define PATCH_SELECT_TOT      0x02u
#define PATCH_SELECT_BOTH     (PATCH_SELECT_TX | PATCH_SELECT_TOT)

#define SYSTEM_COUNT 32u
#define SYSTEM_PTR_BASE 0x0000u
#define ACTIVE_SYSTEM_COUNT 0x0040u
#define GROUP_COUNT_BASE 0x0041u
#define SYSTEM_DATA_PTR_BASE 0x2600u
#define CONV_HEADER_SIZE 0x0Eu
#define CONV_RECORD_SIZE 0x13u
#define CH_RX_FREQ 0x01u
#define CH_NAME 0x0Bu

#define PREVIEW_LINES 10
#define STATUS_LEFT_WIDTH 46
#define STATUS_RIGHT_WIDTH 29
#define MAX_PORT_CHOICES 64
#define MAX_PATH_LEN 1024
#define MAX_SOURCE_LEN 160
#define USB_DETECT_INTERVAL_MS 500u
#define USB_DETECT_TIMEOUT_MS 60000u
#define USB_SETTLE_MS 3000u
#define USB_MISSING_STEPS 3u

#ifdef _WIN32
#define PATH_SEP '\\'
#else
#define PATH_SEP '/'
#endif

enum model_family {
    MODEL_UNKNOWN = 0,
    MODEL_840,
    MODEL_940,
    MODEL_941
};

enum tx_behavior {
    TX_UNKNOWN = 0,
    TX_FIXED_OFFSET,
    TX_FIXED
};

enum dealer_tx_behavior {
    DEALER_TX_UNKNOWN = 0,
    DEALER_TX_FIXED_OFFSET,
    DEALER_TX_FIELD
};

enum tot_behavior {
    TOT_UNSUPPORTED = 0,
    TOT_STOCK,
    TOT_PATCHED,
    TOT_LEGACY,     /* older unconditional two-byte TOT bypass */
    TOT_CONFLICT
};


struct radio_port {
#ifdef _WIN32
    HANDLE h;
    DCB saved_dcb;
    COMMTIMEOUTS saved_timeouts;
    int saved_dcb_valid;
    int saved_timeouts_valid;
#else
    int fd;
    struct termios saved_tio;
    int saved_tio_valid;
    int saved_modem_bits;
    int saved_modem_valid;
#endif
    int is_open;
    int password_known;
    int password_enabled;
    char password[PASSWORD_LENGTH + 1];
};

struct channel_preview {
    int valid;
    unsigned logical;
    uint32_t rx_hz;
    char name[9];
};

#define FW_FINGERPRINT_ADDR 0xDF00u
#define FW_FINGERPRINT_OFFSET (FW_FINGERPRINT_ADDR - PROGRAM_START)

struct firmware_revision {
    const char *label;
    enum model_family model;
    uint16_t checksum;
    const char *full_sha;
    const char *fingerprint_sha;
    uint16_t tx_addr;
    uint8_t stock_tx[16];
    uint8_t fixed_tx[16];
    uint16_t dealer_tx_addr;
    int latest;
};

static const struct firmware_revision fw_db[] = {
    {
        "TK-941/940 2CCA", MODEL_941, 0x2CCA,
        "8B5238681ECA1E64427320E83E2D4372DBBF7A7A5FF075A4C83B76A25ADCCB2B",
        "CA97CBE94E06E993CE06671E8D5E8FB0ED5E2356B167C25AA874821205DD7196",
        0x9A13,
        {0x28,0x42,0x9A,0x71,0x9E,0x0A,0x2D,0xF0,0x09,0x1A,0x20,0x3A,0x22,0x01,0x14,0x18},
        {0x28,0x3B,0xD9,0x83,0x29,0x47,0x47,0x47,0x59,0x24,0x01,0x5D,0x03,0x9F,0x14,0xC7},
        0x9839,
        0
    },
    {
        "TK-941/940 8E9D", MODEL_940, 0x8E9D,
        "CBDAADC339CD9DF2B9EEEC9C9A844703A694A6DC0F2C630EA8F1C4AD49556B9F",
        "D8F4812B6359AEB456739059FA7995148616763E96CDAA175588D699F7B4386D",
        0x9A41,
        {0x28,0x70,0x9A,0x71,0x9E,0x0A,0x2D,0xF0,0x09,0x1A,0x20,0x3A,0x22,0x01,0x14,0x18},
        {0x28,0x11,0xDA,0x83,0x29,0x47,0x47,0x47,0x59,0x24,0x01,0x5D,0x03,0x9F,0x14,0xC7},
        0x9867,
        0
    },
    {
        "TK-941/940 AB00", MODEL_941, 0xAB00,
        "45E9E8CFB4070FD14A9C6339F97CD19140FAF4ACF145DC043A9E78C3E8757BCF",
        "4D53B1384E082AEDD5587CF1E8E4C5C87A88FF65FF67E784DF27125D1B929F61",
        0x9C61,
        {0x28,0x90,0x9C,0x71,0x9E,0x0A,0x2D,0xF0,0x09,0x1A,0x20,0x3A,0x22,0x01,0x14,0x18},
        {0x28,0x6F,0xDD,0x83,0x29,0x47,0x47,0x47,0x59,0x24,0x01,0x5D,0x03,0x9F,0x14,0xC7},
        0x9A8E,
        1
    },
    {
        "TK-941/940 B5EC", MODEL_941, 0xB5EC,
        "1005AD3F16F127A8BD8613FD3168A2F8285CEEDEBB378B6C9F9F87786CB61818",
        "122298DB0E22E18DB531B62B4E436B00E7EB88514596CD4755C7027D532F5F3E",
        0x9C6E,
        {0x28,0x9D,0x9C,0x71,0x9E,0x0A,0x2D,0xF0,0x09,0x1A,0x20,0x3A,0x22,0x01,0x14,0x18},
        {0x28,0x9A,0xDE,0x83,0x29,0x47,0x47,0x47,0x59,0x24,0x01,0x5D,0x03,0x9F,0x14,0xC7},
        0x9A94,
        0
    },
    {
        "TK-840 8355", MODEL_840, 0x8355,
        "7B061DC79AD69475484D68340EDD872BD5E1A8F67EFEB27BFFBE3812A0EE7258",
        "9E5C43F511294D9269F1CB20D179D7B5DBA0DF12C77F742D63A95B69B358CCC3",
        0,
        {0}, {0}, 0, 0
    },
    /* Reconstructed 9306 reference, not an independent factory image.
       Its known two-byte unconditional TOT bypass can be restored by
       writing 81 01 at B173-B174. The resulting checksum and SHA-256
       identify the exact accepted stock image. */
    {
        "TK-840 9306", MODEL_840, 0x9306,
        "5EAC89945D2A199919AB12B11CA5FFB226452BD3FF1788DF898467BA69AB38CE",
        "09B613B6AE83BA86A754E7226482D4332121D9C1E9B64DC05B9C818D582A8DB7",
        0,
        {0}, {0}, 0, 0
    }
};
#define FW_DB_COUNT (sizeof(fw_db)/sizeof(fw_db[0]))

/* Reported checksums are references, not firmware fingerprints or a patch
   whitelist. A 16-bit sum cannot uniquely identify an image. */
struct reported_fw_checksum {
    uint16_t sum;
    enum model_family family;
    const char *name;
};
static const struct reported_fw_checksum reported_fw[] = {
    {0x6641,MODEL_840,"TK-840 6641"},
    {0x9C20,MODEL_840,"TK-840 9C20 (mobile data)"},
    {0xAAEB,MODEL_940,"TK-940/941 AAEB"},
    {0x3885,MODEL_940,"TK-940/941 3885 (mobile data)"}
};
#define REPORTED_FW_COUNT (sizeof(reported_fw)/sizeof(reported_fw[0]))

static int reference_model_matches(enum model_family actual,
                                   enum model_family candidate)
{
    return actual==MODEL_UNKNOWN || actual==candidate ||
           ((actual==MODEL_940 || actual==MODEL_941) &&
            (candidate==MODEL_940 || candidate==MODEL_941));
}

static void print_unknown_firmware_guidance(enum model_family model,
                                             int have_checksum,uint16_t checksum)
{
    size_t i;
    const struct reported_fw_checksum *reported=NULL;
    const struct firmware_revision *known=NULL;
    int unknown=0;
    if(have_checksum){
        for(i=0;i<REPORTED_FW_COUNT;i++)
            if(reported_fw[i].sum==checksum &&
               reference_model_matches(model,reported_fw[i].family)){
                reported=&reported_fw[i];break;
            }
        for(i=0;i<FW_DB_COUNT;i++)
            if(fw_db[i].checksum==checksum &&
               reference_model_matches(model,fw_db[i].model)){
                known=&fw_db[i];break;
            }
        if(reported){
            printf("Checksum %04X matches documented %s.\n",checksum,reported->name);
            printf("This is a documented Kenwood firmware revision, but we do not\n");
            printf("currently have a copy available for analysis.\n");
            printf("A copy of this firmware would allow us to investigate adding\n");
            printf("support for it in a future release.\n");
            printf("A matching checksum does not prove identity.\n");
        }else if(known){
            printf("Checksum %04X matches %s, but the firmware fingerprint does not.\n",
                   checksum,known->label);
            printf("The image may be modified or otherwise different from our reference.\n");
            unknown=1;
        }else{
            unknown=1;
        }
    }else{
        unknown=1;
    }
    if(unknown){
        printf("This firmware is not recognized by the toolkit. It may be a\n");
        printf("modified version of known firmware, or an entirely different revision.\n");
        printf("A reference copy would help us identify this firmware and\n");
        printf("determine whether support can be added in a future release.\n");
    }
    if(!have_checksum)
        printf("A full firmware read is needed for its checksum.\n");
    printf("IMPORTANT: Kenwood firmware is copyrighted. DO NOT upload Kenwood\n");
    printf("firmware files, or links to them, to GitHub, including bug reports\n");
    printf("and discussions.\n");
}

struct app_state {
    char port_name[MAX_PATH_LEN];
    int connected;
    char ident[8];
    enum model_family model;
    const struct firmware_revision *quick_rev; /* installed radio: DF00 firmware fingerprint */
    int radio_fingerprinted;
    int radio_full_read; /* full radio read or verified completed write */
    enum tx_behavior radio_tx;
    enum dealer_tx_behavior radio_dealer_tx;
    enum tot_behavior radio_tot;
    int radio_chirp_tag_known;
    int radio_chirp_tag_present;
    uint8_t radio_firmware_patch_flags;

    /* The following firmware state refers ONLY to the full image in memory. */
    enum tx_behavior tx;
    enum dealer_tx_behavior dealer_tx;
    enum tot_behavior tot;
    int recovery_known;
    int recovery_enabled;
    int password_known;
    int password_enabled;
    char password[PASSWORD_LENGTH + 1];

    /* Loaded-channel image and radio identification are separate. */
    struct channel_preview preview[PREVIEW_LINES];
    size_t preview_count;
    struct channel_preview radio_preview[PREVIEW_LINES];
    size_t radio_preview_count;

    uint8_t firmware[PROGRAM_SIZE];
    int firmware_loaded;
    char firmware_source[MAX_SOURCE_LEN];
    int firmware_modified;
    int firmware_hex_family; /* trailer identifier from validated HEX source */
    int guided_loading; /* do not prompt to keep dangerous test images */
    uint16_t firmware_sum;
    int tot_test_10x; /* exact two-byte acceleration, for diagnostics ONLY */
    char firmware_sha[65];
    const struct firmware_revision *firmware_rev;

    uint8_t channels[CHANNEL_SIZE];
    int channels_loaded;
    enum model_family channels_model;
    char channels_source[MAX_SOURCE_LEN];

    char backup_dir[MAX_PATH_LEN];
    int include_author_signature;
    int include_modified_tag;
    unsigned patch_selection;
    int patch_selection_explicit;
    enum model_family patch_model; /* recognized firmware model */
    int compare_before_firmware_write;
    int development_firmware_mode;
    uint8_t dev_baseline[PROGRAM_SIZE];
    uint8_t dev_known_pages[FLASH_PAGE_COUNT];
    char dev_baseline_source[MAX_SOURCE_LEN];
    enum model_family dev_baseline_model;
    int dev_baseline_restored;
    int dev_baseline_sampled;
    int chirp_tag_known;
    int chirp_tag_present;
    uint8_t firmware_patch_flags;
    char file_dir[MAX_PATH_LEN];
};

struct ui_item {
    char key;
    const char *label;
    const char *detail;
    const char *help;
};

static int g_plain_text = 0;
static int g_ansi = 0;
static int g_native_console = 0;
#ifdef _WIN32
static WORD g_native_console_attrs = 0;
#endif
static int g_quit_requested = 0;
static int g_main_menu_requested = 0;
static int g_cli_noninteractive = 0;
static int g_cli_confirm_write = 0;
static int g_cli_needs_interaction = 0;

static void cli_mark_needs_interaction(const char *reason)
{
    g_cli_needs_interaction = 1;
    if (reason && *reason)
        fprintf(stderr, "Interaction required: %s\n", reason);
}

static void update_preview_from_full_channels(struct app_state *st);
static int open_program_session_retry(struct app_state *st, struct radio_port *p,
                                      char ident[8], const char *purpose);
static int confirm_firmware_program_start(void);
static int select_port(struct app_state *st);
static const char *port_display_name(const char *name);
static void dev_clear_baseline(struct app_state *st);
static void invalidate_radio_connection(struct app_state *st);
static size_t dev_known_page_count(const struct app_state *st);
static void dev_probe_compare_baseline(struct app_state *st, struct radio_port *p,
                                       const uint8_t first[BLOCK_SIZE],
                                       const uint8_t tail[BLOCK_SIZE]);
static void dev_load_persisted_baseline(struct app_state *st);
static void dev_forget_persisted_baseline(const struct app_state *st);
static void dev_quarantine_persisted_baseline(const struct app_state *st);
static void dev_set_full_baseline(struct app_state *st, const uint8_t *image, const char *source);
static void advanced_menu(struct app_state *st);
static void update_password_state(struct app_state *st, const struct radio_port *p);

/* ----------------------------- Small helpers ----------------------------- */

static void sleep_ms(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#elif defined(__APPLE__)
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) { }
#else
    usleep((useconds_t)ms * 1000u);
#endif
}

static uint64_t monotonic_ms(void)
{
#ifdef _WIN32
    /*
     * GetTickCount64 is unavailable on Windows XP. Resolve it at runtime,
     * and extend XP's 32-bit GetTickCount across the 49.7-day rollover.
     * The Toolkit's timing and UI code runs in a single thread.
     */
    typedef ULONGLONG (WINAPI *tick64_fn)(void);
    static tick64_fn get_tick64 = NULL;
    static int resolved = 0;
    static DWORD previous_tick = 0;
    static uint64_t wrap_base = 0;
    if (!resolved) {
        HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
        if (kernel32) {
            FARPROC proc = GetProcAddress(kernel32, "GetTickCount64");
            if (proc) memcpy(&get_tick64, &proc, sizeof(get_tick64));
        }
        resolved = 1;
    }
    if (get_tick64) return (uint64_t)get_tick64();
    {
        DWORD tick = GetTickCount();
        if (tick < previous_tick)
            wrap_base += (UINT64_C(1) << 32);
        previous_tick = tick;
        return wrap_base + (uint64_t)tick;
    }
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
    return (uint64_t)time(NULL) * 1000u;
#endif
}

static void local_time_now(struct tm *out)
{
    time_t t = time(NULL);
#ifdef _WIN32
    localtime_s(out, &t);
#else
    localtime_r(&t, out);
#endif
}


static int detect_terminal_size(int *cols, int *rows)
{
    int c = 0, r = 0;
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO ci;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &ci)) {
        c = ci.srWindow.Right - ci.srWindow.Left + 1;
        r = ci.srWindow.Bottom - ci.srWindow.Top + 1;
    }
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        c = ws.ws_col;
        r = ws.ws_row;
    }
#endif
    if (cols) *cols = c;
    if (rows) *rows = r;
    return c > 0 && r > 0;
}

static void get_terminal_size(int *cols, int *rows)
{
    int c = 0, r = 0;
    (void)detect_terminal_size(&c, &r);
    if (c <= 0) c = 80;
    if (r <= 0) r = 24;
    if (cols) *cols = c;
    if (rows) *rows = r;
}

static void progress_line(const char *verb, const char *what,
                          size_t done, size_t count, size_t total_units,
                          unsigned addr_first, unsigned addr_last)
{
    int cols = 80, rows = 24;
    char prefix[160];
    size_t i;
    int pct;
    int barw;
    (void)rows;
    get_terminal_size(&cols, &rows);
    if (cols < 60) cols = 80;
    if (cols > 160) cols = 160;
    pct = count ? (int)((done * 100u) / count) : 100;
    if (total_units != count) {
        snprintf(prefix, sizeof(prefix), "%s %s %zu/%zu (%zu total) %04X-%04X ",
                 verb, what, done, count, total_units, addr_first, addr_last);
    } else {
        snprintf(prefix, sizeof(prefix), "%s %s %zu/%zu %04X-%04X ",
                 verb, what, done, count, addr_first, addr_last);
    }
    barw = cols - (int)strlen(prefix) - 8; /* brackets, space, 100% */
    if (barw < 10) barw = 10;
    if (barw > 42) barw = 42;
    fprintf(stderr, "\r%s[", prefix);
    for (i = 0; i < (size_t)barw; i++) {
        size_t filled = count ? (done * (size_t)barw) / count : (size_t)barw;
        fputc(i < filled ? '#' : '-', stderr);
    }
    fprintf(stderr, "] %3d%%", pct);
    fflush(stderr);
}

static void progress_done(void)
{
    fputc('\n', stderr);
    fflush(stderr);
}

static int make_dir_if_needed(const char *path)
{
#ifdef _WIN32
    if (_mkdir(path) == 0 || errno == EEXIST)
        return 0;
#else
    if (mkdir(path, 0755) == 0 || errno == EEXIST)
        return 0;
#endif
    return -1;
}

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

static uint16_t rd16le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static const char *model_name(enum model_family m)
{
    switch (m) {
    case MODEL_840: return "TK-840";
    case MODEL_940: return "TK-940";
    case MODEL_941: return "TK-941";
    default: return "TK-x4x";
    }
}

static enum model_family model_from_ident(const char ident[8])
{
    if (!strncmp(ident, "M941", 4)) return MODEL_941;
    if (!strncmp(ident, "M940", 4)) return MODEL_940;
    if (!strncmp(ident, "M890", 4) || !strncmp(ident, "M840", 4)) return MODEL_840;
    return MODEL_UNKNOWN;
}

static int ident_plausible(const char ident[8])
{
    int i;
    if (ident[0] != 'M')
        return 0;
    for (i = 0; i < 7; ++i) {
        unsigned c = (unsigned char)ident[i];
        if (c < 0x20 || c > 0x7E)
            return 0;
    }
    return 1;
}

static void copy_string(char *out, size_t cap, const char *src)
{
    size_t n;
    if (!cap) return;
    n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(out, src, n);
    out[n] = '\0';
}

static void basename_copy(const char *path, char *out, size_t cap)
{
    const char *p = path, *last = path;
    while (*p) {
        if (*p == '/' || *p == '\\') last = p + 1;
        ++p;
    }
    copy_string(out, cap, last);
}

static void sanitize_filename(char *s)
{
    for (; *s; ++s)
        if (!isalnum((unsigned char)*s) && *s != '-' && *s != '_' && *s != '.')
            *s = '_';
}

static int stdin_is_terminal(void);
static void ui_flush_input(void);

static int ask_yes_no(const char *question, int default_yes)
{
    char line[64];
    if (g_cli_noninteractive) {
        cli_mark_needs_interaction(question);
        return 0;
    }
    ui_flush_input();
    printf("%s %s: ", question, default_yes ? "[Y/n]" : "[y/N]");
    fflush(stdout);
    if (!fgets(line, sizeof(line), stdin))
        return default_yes;
    if (line[0] == '\n' || line[0] == '\0')
        return default_yes;
    return line[0] == 'y' || line[0] == 'Y';
}

static int wait_enter(const char *message)
{
    char line[16];
    if (g_cli_noninteractive) {
        cli_mark_needs_interaction(message);
        return -1;
    }
    ui_flush_input();
    printf("%s", message);
    fflush(stdout);
    return fgets(line, sizeof(line), stdin) ? 0 : -1;
}

static void ui_flush_input(void)
{
#ifdef _WIN32
    if (stdin_is_terminal()) {
        while (_kbhit()) (void)_getch();
    }
#else
    if (stdin_is_terminal())
        (void)tcflush(STDIN_FILENO, TCIFLUSH);
#endif
}

static int stdin_is_terminal(void)
{
#ifdef _WIN32
    return _isatty(_fileno(stdin));
#else
    return isatty(STDIN_FILENO);
#endif
}

static int stdout_is_terminal(void)
{
#ifdef _WIN32
    return _isatty(_fileno(stdout));
#else
    return isatty(STDOUT_FILENO);
#endif
}

static void ui_init(int force_plain)
{
    g_plain_text = force_plain ? 1 : 0;
    g_ansi = 0;
    g_native_console = 0;
    (void)setvbuf(stdin, NULL, _IONBF, 0);
    if (g_plain_text || !stdin_is_terminal() || !stdout_is_terminal()) return;
    if (getenv("NO_COLOR")) {
        g_plain_text = 1;
        return;
    }
#ifdef _WIN32
    {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode;
        if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
            if (SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
                g_ansi = 1;
            } else {
                /* Windows 2000/XP and other pre-VT consoles have native
                   cursor and attribute APIs even without ANSI support. */
                CONSOLE_SCREEN_BUFFER_INFO ci;
                if (GetConsoleScreenBufferInfo(h, &ci)) {
                    g_native_console = 1;
                    g_native_console_attrs = ci.wAttributes;
                }
            }
        }
    }
#else
    {
        const char *term = getenv("TERM");
        if (!term || strcmp(term, "dumb")) g_ansi = 1;
    }
#endif
}

static void ui_force_plain(void)
{
    g_plain_text = 1;
    g_ansi = 0;
    g_native_console = 0;
    printf("\nDisplay reset. Plain-text mode enabled.\n");
}

/* Native Win32 console calls let Windows 2000/XP redraw in place without
   relying on ANSI/VT escapes. Explicit plain-text mode still uses scrollback. */
static int ui_cursor_control(void)
{
    return g_ansi || g_native_console;
}

static void ui_cursor_up(size_t n)
{
    if (!n) return;
    if (g_ansi) {
        printf("\033[%zuA", n);
    }
#ifdef _WIN32
    else if (g_native_console) {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO ci;
        COORD pos;
        fflush(stdout);
        if (GetConsoleScreenBufferInfo(h, &ci)) {
            pos = ci.dwCursorPosition;
            pos.X = 0;
            pos.Y = (SHORT)((size_t)pos.Y >= n ? (size_t)pos.Y - n : 0u);
            (void)SetConsoleCursorPosition(h, pos);
        }
    }
#endif
}

static void ui_clear_line(void)
{
    if (g_ansi) {
        fputs("\r\033[2K", stdout);
    }
#ifdef _WIN32
    else if (g_native_console) {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO ci;
        COORD start;
        DWORD written;
        fflush(stdout);
        if (GetConsoleScreenBufferInfo(h, &ci)) {
            start.X = 0;
            start.Y = ci.dwCursorPosition.Y;
            (void)FillConsoleOutputCharacterA(h, ' ', (DWORD)ci.dwSize.X,
                                              start, &written);
            (void)FillConsoleOutputAttribute(h, g_native_console_attrs,
                                             (DWORD)ci.dwSize.X, start, &written);
            (void)SetConsoleCursorPosition(h, start);
        }
    }
#endif
}

static void ui_inverse(int selected)
{
    if (g_ansi) {
        fputs(selected ? "\033[7m" : "\033[0m", stdout);
    }
#ifdef _WIN32
    else if (g_native_console) {
        WORD attrs = g_native_console_attrs;
        if (selected) {
            WORD fg = attrs & 0x0Fu;
            WORD bg = (attrs >> 4) & 0x0Fu;
            attrs = (WORD)((attrs & (WORD)~0x00FFu) | (fg << 4) | bg);
        }
        fflush(stdout);
        (void)SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), attrs);
    }
#endif
}

enum ui_key {
    UI_KEY_NONE = 0,
    UI_KEY_UP = 0x100,
    UI_KEY_DOWN,
    UI_KEY_ENTER,
    UI_KEY_SPACE,
    UI_KEY_ESCAPE,
    UI_KEY_SELECTED,
    UI_KEY_EOF
};

static int ui_read_key(void)
{
    if (!stdin_is_terminal()) {
        char line[64];
        if (!fgets(line, sizeof(line), stdin)) return UI_KEY_EOF;
        if (line[0] == '\n' || line[0] == '\r' || line[0] == '\0') return UI_KEY_ENTER;
        if (line[0] == ' ') return UI_KEY_SPACE;
        if ((unsigned char)line[0] == 27) return UI_KEY_ESCAPE;
        return (unsigned char)line[0];
    }
#ifdef _WIN32
    {
        int c = _getch();
        if (c == 0 || c == 224) {
            int k = _getch();
            if (k == 72) return UI_KEY_UP;
            if (k == 80) return UI_KEY_DOWN;
            return UI_KEY_NONE;
        }
        if (c == 13 || c == 10) return UI_KEY_ENTER;
        if (c == ' ') return UI_KEY_SPACE;
        if (c == 27) return UI_KEY_ESCAPE;
        if (c == 3) return 'q';
        return (unsigned char)c;
    }
#else
    {
        struct termios oldt, raw;
        unsigned char c;
        if (tcgetattr(STDIN_FILENO, &oldt) != 0) return UI_KEY_EOF;
        raw = oldt;
        raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return UI_KEY_EOF;
        if (read(STDIN_FILENO, &c, 1) != 1) {
            tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
            return UI_KEY_EOF;
        }
        if (c == 3) {
            tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
            return 'q';
        }
        if (c == '\r' || c == '\n') {
            tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
            return UI_KEY_ENTER;
        }
        if (c == ' ') {
            tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
            return UI_KEY_SPACE;
        }
        if (c == 27) {
            struct pollfd pfd;
            unsigned char seq[2];
            int pr;
            pfd.fd = STDIN_FILENO;
            pfd.events = POLLIN;
            pfd.revents = 0;
            pr = poll(&pfd, 1, 75);
            if (pr > 0 && read(STDIN_FILENO, &seq[0], 1) == 1) {
                if (seq[0] == '[' || seq[0] == 'O') {
                    pfd.revents = 0;
                    pr = poll(&pfd, 1, 75);
                    if (pr > 0 && read(STDIN_FILENO, &seq[1], 1) == 1) {
                        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
                        if (seq[1] == 'A') return UI_KEY_UP;
                        if (seq[1] == 'B') return UI_KEY_DOWN;
                    }
                }
            }
            tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
            return UI_KEY_ESCAPE;
        }
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        return (unsigned char)c;
    }
#endif
}

static int ui_read_key_timeout(unsigned timeout_ms)
{
#ifdef _WIN32
    unsigned waited = 0;
    while (waited < timeout_ms) {
        if (_kbhit()) {
            int c = _getch();
            if (c == 3) return 'q';
            if (c == 27) return UI_KEY_ESCAPE;
            if (c == 0 || c == 224) {
                (void)_getch();
                return UI_KEY_NONE;
            }
            return tolower((unsigned char)c);
        }
        {
            unsigned slice = timeout_ms - waited;
            if (slice > 50u) slice = 50u;
            Sleep(slice);
            waited += slice;
        }
    }
    return UI_KEY_NONE;
#else
    struct termios oldt, raw;
    struct pollfd pfd;
    unsigned char c;
    int pr;
    if (!stdin_is_terminal()) {
        sleep_ms(timeout_ms);
        return UI_KEY_NONE;
    }
    if (tcgetattr(STDIN_FILENO, &oldt) != 0) return UI_KEY_NONE;
    raw = oldt;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return UI_KEY_NONE;
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    pfd.revents = 0;
    do {
        pr = poll(&pfd, 1, (int)timeout_ms);
    } while (pr < 0 && errno == EINTR);
    if (pr > 0 && read(STDIN_FILENO, &c, 1) == 1) {
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        if (c == 3) return 'q';
        if (c == 27) return UI_KEY_ESCAPE;
        return tolower(c);
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return UI_KEY_NONE;
#endif
}

static int ui_item_alt_key(const struct ui_item *item)
{
    const char *p;
    if (!item || !item->label) return 0;
    p = strchr(item->label, '[');
    if (p && p[1] && p[2] == ']') return tolower((unsigned char)p[1]);
    return 0;
}

/* A split-menu row prints two prefix characters ("  " or "> "),
   then its padded label, then " | ". Match the status separator exactly. */
#define UI_MENU_LEFT_WIDTH STATUS_LEFT_WIDTH
#define UI_MENU_RIGHT_WIDTH STATUS_RIGHT_WIDTH
#define UI_HELP_MAX_LINES 12

static int ui_menu_has_help(const struct ui_item *items, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++)
        if (items[i].help && *items[i].help) return 1;
    return 0;
}

/* Split menus reserve the left column for the option name.
   Long setting values appear in the wrapping info pane. */
static void ui_format_item_text(const struct ui_item *item, char *out,
                                size_t cap, int include_detail)
{
    size_t used = 0;
    if (!cap) return;
    out[0] = '\0';
    if (item->key && !(item->key >= '0' && item->key <= '9'))
        used = (size_t)snprintf(out, cap, "[%c] %s", item->key, item->label ? item->label : "");
    else
        used = (size_t)snprintf(out, cap, "%s", item->label ? item->label : "");
    if (item->detail && *item->detail && used < cap) {
        size_t detail_len = strlen(item->detail);
        /* Keep short status values visible across the whole menu.
           Long paths and other settings appear in the selected info pane
           instead of being cut off mid-string. */
        if (include_detail ||
            (used < UI_MENU_LEFT_WIDTH - 2 &&
             detail_len + 2 <= UI_MENU_LEFT_WIDTH - 2 - used))
            snprintf(out + used, cap - used, "  %s", item->detail);
    }
}

static size_t ui_wrap_help(const char *text,
                           char lines[UI_HELP_MAX_LINES][UI_MENU_RIGHT_WIDTH + 1])
{
    size_t n = 0;
    const char *p = text;
    while (p && *p && n < UI_HELP_MAX_LINES) {
        size_t len = 0, last_space = 0, take;
        while (p[len] && p[len] != '\n' && len < UI_MENU_RIGHT_WIDTH) {
            if (p[len] == ' ') last_space = len;
            len++;
        }
        if (!p[len] || p[len] == '\n') {
            take = len;
        } else if (last_space) {
            take = last_space;
        } else {
            take = len;
        }
        while (take && p[take - 1] == ' ') take--;
        memcpy(lines[n], p, take);
        lines[n][take] = '\0';
        n++;
        p += (last_space && p[len] && p[len] != '\n') ? last_space + 1 : len;
        if (*p == '\n') p++;
        while (*p == ' ') p++;
    }
    return n;
}

static void ui_print_item(const struct ui_item *item, int selected, int newline,
                          const char *help_line, int split)
{
    char text[512];
    ui_format_item_text(item, text, sizeof(text), !split);
    if (split) {
        if (selected && ui_cursor_control()) ui_inverse(1);
        printf("%c %-*.*s", selected ? '>' : ' ',
               UI_MENU_LEFT_WIDTH - 2, UI_MENU_LEFT_WIDTH - 2, text);
        if (selected && ui_cursor_control()) ui_inverse(0);
        printf(" | %-*.*s", UI_MENU_RIGHT_WIDTH, UI_MENU_RIGHT_WIDTH,
               help_line ? help_line : "");
    } else {
        if (selected) {
            if (ui_cursor_control()) ui_inverse(1);
            fputs("> ", stdout);
        } else {
            fputs("  ", stdout);
        }
        fputs(text, stdout);
        if (selected && ui_cursor_control()) ui_inverse(0);
    }
    if (newline) fputc('\n', stdout);
}

static void ui_redraw_menu(const struct ui_item *items, size_t count, size_t sel, int initial)
{
    size_t i, help_count = 0;
    int split = ui_menu_has_help(items, count);
    char help_lines[UI_HELP_MAX_LINES][UI_MENU_RIGHT_WIDTH + 1];
    char info[MAX_PATH_LEN + 512];
    const struct ui_item *selected = &items[sel];
    memset(help_lines, 0, sizeof(help_lines));
    info[0] = '\0';
    if (split) {
        const char *detail = selected->detail;
        const char *help = selected->help;
        if (detail && *detail && help && *help)
            snprintf(info, sizeof(info), "Value: %s\n%s", detail, help);
        else if (detail && *detail)
            snprintf(info, sizeof(info), "Value: %s", detail);
        else if (help)
            copy_string(info, sizeof(info), help);
        help_count = ui_wrap_help(info, help_lines);
    }
    if (ui_cursor_control()) {
        if (!initial && count > 1) {
            /* The cursor is left on the last menu line. Relative movement
               also works after the terminal has scrolled. */
            ui_cursor_up(count - 1);
        }
        for (i = 0; i < count; i++) {
            ui_clear_line();
            ui_print_item(&items[i], i == sel, i + 1 < count,
                          i < help_count ? help_lines[i] : "", split);
        }
    } else {
        /* A genuinely dumb terminal cannot move the cursor vertically. Keep
           the fallback simple and readable: arrow movement redraws the menu,
           but never clears the screen. */
        if (!initial) fputc('\n', stdout);
        for (i = 0; i < count; i++)
            ui_print_item(&items[i], i == sel, 1,
                          i < help_count ? help_lines[i] : "", split);
    }
}

static int ui_menu(const struct ui_item *items, size_t count, size_t *selected,
                   void (*draw)(void *ctx), void *ctx)
{
    size_t sel = selected ? *selected : 0;
    int initial = 1;
    int draw_header = 1;
    if (!count) return UI_KEY_EOF;
    if (sel >= count) sel = 0;
    for (;;) {
        if (initial && draw && draw_header) draw(ctx);
        ui_redraw_menu(items, count, sel, initial);
        fflush(stdout);
        {
            int k = ui_read_key();
            if (k == UI_KEY_EOF) return UI_KEY_EOF;
            if (k == UI_KEY_ESCAPE) return UI_KEY_ESCAPE;
            if (k == UI_KEY_UP) {
                if (sel == 0) sel = count - 1; else sel--;
                initial = 0;
                continue;
            }
            if (k == UI_KEY_DOWN) {
                if (sel + 1 >= count) sel = 0; else sel++;
                initial = 0;
                continue;
            }
            if (k == UI_KEY_ENTER || k == UI_KEY_SPACE) {
                if (selected) *selected = sel;
                /* The current line is already highlighted. Move below the menu
                   and leave a normal history line in terminal scrollback. */
                fputc('\n', stdout);
                printf("Selected: ");
                if (items[sel].key && !(items[sel].key >= '0' && items[sel].key <= '9'))
                    printf("[%c] ", items[sel].key);
                printf("%s", items[sel].label);
                if (items[sel].detail && *items[sel].detail) printf(" %s", items[sel].detail);
                fputc('\n', stdout);
                return items[sel].key ? tolower((unsigned char)items[sel].key) : UI_KEY_SELECTED;
            }
            if (tolower((unsigned char)k) == 'x') {
                size_t j;
                int shortcut_used = 0;
                for (j = 0; j < count; j++)
                    if (tolower((unsigned char)items[j].key) == 'x' ||
                        ui_item_alt_key(&items[j]) == 'x')
                        shortcut_used = 1;
                if (!shortcut_used) {
                    ui_force_plain();
                    initial = 1;
                    draw_header = 0;
                    continue;
                }
            }
            if (k > 0 && k < 256) {
                size_t i;
                for (i = 0; i < count; i++) {
                    int pressed = tolower((unsigned char)k);
                    int primary = items[i].key ? tolower((unsigned char)items[i].key) : 0;
                    int alternate = ui_item_alt_key(&items[i]);
                    if ((primary && primary == pressed) || (alternate && alternate == pressed)) {
                        if (selected) *selected = i;
                        /* Make direct shortcuts look exactly like arrow-key
                           selection before activating the item. */
                        ui_redraw_menu(items, count, i, 0);
                        fflush(stdout);
                        fputc('\n', stdout);
                        if (items[i].key >= '0' && items[i].key <= '9')
                            printf("Selected: %s", items[i].label);
                        else
                            printf("Selected: [%c] %s", items[i].key, items[i].label);
                        if (items[i].detail && *items[i].detail) printf(" %s", items[i].detail);
                        fputc('\n', stdout);
                        return primary ? primary : UI_KEY_SELECTED;
                    }
                }
            }
        }
        initial = 0;
        draw_header = 1;
    }
}

static void redraw_edit_line(const char *prompt, const char *buf,
                             size_t len, size_t pos, size_t *old_len)
{
    size_t i, clear_to = (*old_len > len ? *old_len : len);
    fputc('\r', stdout);
    fputs(prompt, stdout);
    fwrite(buf, 1, len, stdout);
    for (i = len; i < clear_to; i++) fputc(' ', stdout);
    fputc('\r', stdout);
    fputs(prompt, stdout);
    fwrite(buf, 1, pos, stdout);
    fflush(stdout);
    *old_len = len;
}

static int edit_line(const char *prompt, const char *initial,
                     char *out, size_t cap)
{
    size_t len = 0, pos = 0, old_len = 0;
    int insert_mode = 1;
    if (!cap) return -1;
    if (initial) {
        len = strlen(initial);
        if (len >= cap) len = cap - 1;
        memcpy(out, initial, len);
    }
    out[len] = '\0';
    pos = len;
    if (!stdin_is_terminal()) {
        char line[MAX_PATH_LEN];
        printf("%s", prompt);
        if (initial && *initial) printf("[%s] ", initial);
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) return -1;
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0] && initial) copy_string(out, cap, initial);
        else copy_string(out, cap, line);
        return 0;
    }

    fputs(prompt, stdout);
    fwrite(out, 1, len, stdout);
    fflush(stdout);
    old_len = len;

#ifdef _WIN32
    for (;;) {
        int c = _getch();
        if (c == 13 || c == 10) break;
        if (c == 3) { fputs("^C\n", stdout); return -1; }
        if (c == 8) {
            if (pos > 0) {
                memmove(out + pos - 1, out + pos, len - pos + 1);
                pos--; len--;
            }
        } else if (c == 0 || c == 224) {
            int k = _getch();
            if (k == 75) { if (pos > 0) pos--; }          /* Left */
            else if (k == 77) { if (pos < len) pos++; }   /* Right */
            else if (k == 71) pos = 0;                    /* Home */
            else if (k == 79) pos = len;                  /* End */
            else if (k == 83) {                           /* Delete */
                if (pos < len) { memmove(out + pos, out + pos + 1, len - pos); len--; }
            } else if (k == 82) insert_mode = !insert_mode; /* Insert */
        } else if (c >= 0x20 && c <= 0x7e) {
            if (!insert_mode && pos < len) {
                out[pos++] = (char)c;
            } else if (len + 1 < cap) {
                memmove(out + pos + 1, out + pos, len - pos + 1);
                out[pos++] = (char)c;
                len++;
            }
        }
        out[len] = '\0';
        redraw_edit_line(prompt, out, len, pos, &old_len);
    }
#else
    {
        struct termios oldt, raw;
        if (tcgetattr(STDIN_FILENO, &oldt) != 0) return -1;
        raw = oldt;
        raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return -1;
        for (;;) {
            unsigned char c = 0;
            if (read(STDIN_FILENO, &c, 1) != 1) { tcsetattr(STDIN_FILENO, TCSANOW, &oldt); return -1; }
            if (c == '\r' || c == '\n') break;
            if (c == 3) { tcsetattr(STDIN_FILENO, TCSANOW, &oldt); fputs("^C\n", stdout); return -1; }
            if (c == 1) pos = 0;                           /* Ctrl-A */
            else if (c == 5) pos = len;                    /* Ctrl-E */
            else if (c == 21) { len = pos = 0; out[0] = '\0'; } /* Ctrl-U */
            else if (c == 4) {                             /* Ctrl-D */
                if (pos < len) { memmove(out + pos, out + pos + 1, len - pos); len--; }
            } else if (c == 8 || c == 127) {
                if (pos > 0) {
                    memmove(out + pos - 1, out + pos, len - pos + 1);
                    pos--; len--;
                }
            } else if (c == 27) {
                unsigned char seq[8] = {0};
                struct pollfd pfd;
                int pr;
                pfd.fd = STDIN_FILENO;
                pfd.events = POLLIN;
                pfd.revents = 0;
                do { pr = poll(&pfd, 1, 75); } while (pr < 0 && errno == EINTR);
                if (pr > 0 && read(STDIN_FILENO, &seq[0], 1) == 1 &&
                    (seq[0] == '[' || seq[0] == 'O')) {
                    pfd.revents = 0;
                    do { pr = poll(&pfd, 1, 75); } while (pr < 0 && errno == EINTR);
                    if (pr > 0 && read(STDIN_FILENO, &seq[1], 1) == 1) {
                        if (seq[1] == 'D') { if (pos > 0) pos--; }
                        else if (seq[1] == 'C') { if (pos < len) pos++; }
                        else if (seq[1] == 'H') pos = 0;
                        else if (seq[1] == 'F') pos = len;
                        else if (seq[0] == '[' && isdigit(seq[1])) {
                            size_t q = 2;
                            while (q < sizeof(seq)-1 && seq[q-1] != '~') {
                                pfd.revents = 0;
                                do { pr = poll(&pfd, 1, 75); } while (pr < 0 && errno == EINTR);
                                if (pr <= 0 || read(STDIN_FILENO, &seq[q], 1) != 1) break;
                                q++;
                                if (seq[q-1] == '~') break;
                            }
                            if (seq[1] == '3' && seq[2] == '~') {
                                if (pos < len) { memmove(out + pos, out + pos + 1, len - pos); len--; }
                            } else if (seq[1] == '2' && seq[2] == '~') insert_mode = !insert_mode;
                            else if ((seq[1] == '1' || seq[1] == '7') && seq[2] == '~') pos = 0;
                            else if ((seq[1] == '4' || seq[1] == '8') && seq[2] == '~') pos = len;
                        }
                    }
                }
            } else if (c >= 0x20 && c <= 0x7e) {
                if (!insert_mode && pos < len) {
                    out[pos++] = (char)c;
                } else if (len + 1 < cap) {
                    memmove(out + pos + 1, out + pos, len - pos + 1);
                    out[pos++] = (char)c;
                    len++;
                }
            }
            out[len] = '\0';
            redraw_edit_line(prompt, out, len, pos, &old_len);
        }
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    }
#endif
    fputc('\n', stdout);
    out[len] = '\0';
    return 0;
}

static void prompt_string(const char *label, const char *def, char *out, size_t cap)
{
    char prompt[256];
    ui_flush_input();
    snprintf(prompt, sizeof(prompt), "%s: ", label);
    if (edit_line(prompt, def ? def : "", out, cap) < 0)
        copy_string(out, cap, def ? def : "");
}

/* ------------------------------- SHA-256 -------------------------------- */

struct sha256_ctx { uint32_t s[8]; uint64_t bits; uint8_t buf[64]; size_t used; };
static uint32_t rotr32(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
static void sha256_block(struct sha256_ctx *c, const uint8_t b[64])
{
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,
        0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,
        0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,
        0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
        0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,
        0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
        0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,
        0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,
        0xc67178f2
    };
    uint32_t w[64], a,bv,d,e,f,g,h,t1,t2,cc;
    unsigned i;
    for (i=0;i<16;i++) w[i]=((uint32_t)b[i*4]<<24)|((uint32_t)b[i*4+1]<<16)|((uint32_t)b[i*4+2]<<8)|b[i*4+3];
    for (i=16;i<64;i++) { uint32_t s0=rotr32(w[i-15],7)^rotr32(w[i-15],18)^(w[i-15]>>3); uint32_t s1=rotr32(w[i-2],17)^rotr32(w[i-2],19)^(w[i-2]>>10); w[i]=w[i-16]+s0+w[i-7]+s1; }
    a=c->s[0];bv=c->s[1];cc=c->s[2];d=c->s[3];e=c->s[4];f=c->s[5];g=c->s[6];h=c->s[7];
    for(i=0;i<64;i++){uint32_t S1=rotr32(e,6)^rotr32(e,11)^rotr32(e,25);uint32_t ch=(e&f)^((~e)&g);uint32_t S0=rotr32(a,2)^rotr32(a,13)^rotr32(a,22);uint32_t maj=(a&bv)^(a&cc)^(bv&cc);t1=h+S1+ch+k[i]+w[i];t2=S0+maj;h=g;g=f;f=e;e=d+t1;d=cc;cc=bv;bv=a;a=t1+t2;}
    c->s[0]+=a;c->s[1]+=bv;c->s[2]+=cc;c->s[3]+=d;c->s[4]+=e;c->s[5]+=f;c->s[6]+=g;c->s[7]+=h;
}
static void sha256_init(struct sha256_ctx *c){static const uint32_t iv[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};memcpy(c->s,iv,sizeof(iv));c->bits=0;c->used=0;}
static void sha256_update(struct sha256_ctx*c,const void*vp,size_t n){const uint8_t*p=(const uint8_t*)vp;c->bits+=(uint64_t)n*8;while(n){size_t q=64-c->used;if(q>n)q=n;memcpy(c->buf+c->used,p,q);c->used+=q;p+=q;n-=q;if(c->used==64){sha256_block(c,c->buf);c->used=0;}}}
static void sha256_final(struct sha256_ctx*c,uint8_t out[32]){size_t i;c->buf[c->used++]=0x80;if(c->used>56){while(c->used<64)c->buf[c->used++]=0;sha256_block(c,c->buf);c->used=0;}while(c->used<56)c->buf[c->used++]=0;for(i=0;i<8;i++)c->buf[63-i]=(uint8_t)(c->bits>>(i*8));sha256_block(c,c->buf);for(i=0;i<8;i++){out[i*4]=(uint8_t)(c->s[i]>>24);out[i*4+1]=(uint8_t)(c->s[i]>>16);out[i*4+2]=(uint8_t)(c->s[i]>>8);out[i*4+3]=(uint8_t)c->s[i];}}
static void sha256_hex(const uint8_t*p,size_t n,char out[65]){static const char h[]="0123456789ABCDEF";struct sha256_ctx c;uint8_t d[32];size_t i;sha256_init(&c);sha256_update(&c,p,n);sha256_final(&c,d);for(i=0;i<32;i++){out[i*2]=h[d[i]>>4];out[i*2+1]=h[d[i]&15];}out[64]=0;}

static uint16_t byte_sum16(const uint8_t *p, size_t len)
{
    uint32_t s = 0;
    size_t i;
    for (i=0;i<len;i++) s += p[i];
    return (uint16_t)s;
}

/* --------------------------- Serial abstraction -------------------------- */

#ifdef _WIN32
static int port_set_baud(struct radio_port *p, DWORD baud)
{
    DCB dcb;
    COMMTIMEOUTS to;
    memset(&dcb,0,sizeof(dcb)); dcb.DCBlength=sizeof(dcb);
    if (!GetCommState(p->h,&dcb)) return -1;
    dcb.BaudRate=baud; dcb.ByteSize=8; dcb.Parity=NOPARITY; dcb.StopBits=TWOSTOPBITS;
    dcb.fBinary=TRUE; dcb.fParity=FALSE; dcb.fOutxCtsFlow=FALSE; dcb.fOutxDsrFlow=FALSE;
    dcb.fDtrControl=DTR_CONTROL_ENABLE; dcb.fDsrSensitivity=FALSE; dcb.fOutX=FALSE; dcb.fInX=FALSE;
    dcb.fRtsControl=RTS_CONTROL_ENABLE;
    if (!SetCommState(p->h,&dcb)) return -1;
    memset(&to,0,sizeof(to)); to.ReadIntervalTimeout=MAXDWORD; to.ReadTotalTimeoutConstant=TIMEOUT_MS; to.ReadTotalTimeoutMultiplier=0;
    to.WriteTotalTimeoutConstant=TIMEOUT_MS; to.WriteTotalTimeoutMultiplier=0;
    if (!SetCommTimeouts(p->h,&to)) return -1;
    return 0;
}
static int port_open(struct radio_port *p,const char *name)
{
    char full[64];
    DWORD winerr;
    memset(p,0,sizeof(*p));
    if (!strncmp(name,"\\\\.\\",4)) snprintf(full,sizeof(full),"%s",name); else snprintf(full,sizeof(full),"\\\\.\\%s",name);
    p->h=CreateFileA(full,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,0,NULL);
    if(p->h==INVALID_HANDLE_VALUE){
        winerr=GetLastError();
        if(winerr==ERROR_SHARING_VIOLATION) errno=EBUSY;
        else if(winerr==ERROR_ACCESS_DENIED) errno=EACCES;
        else if(winerr==ERROR_FILE_NOT_FOUND||winerr==ERROR_PATH_NOT_FOUND||winerr==ERROR_INVALID_NAME) errno=ENOENT;
        else if(winerr==ERROR_DEVICE_NOT_CONNECTED) errno=ENODEV;
        else errno=EIO;
        return -1;
    }
    p->is_open=1;
    memset(&p->saved_dcb,0,sizeof(p->saved_dcb));
    p->saved_dcb.DCBlength=sizeof(p->saved_dcb);
    p->saved_dcb_valid=GetCommState(p->h,&p->saved_dcb)?1:0;
    p->saved_timeouts_valid=GetCommTimeouts(p->h,&p->saved_timeouts)?1:0;
    SetupComm(p->h,4096,4096); EscapeCommFunction(p->h,SETDTR); EscapeCommFunction(p->h,SETRTS);
    return 0;
}
/* The firmware loader transfer already drains its EOF. */
static void port_close_internal(struct radio_port*p,int drain_on_close)
{
    if(p->is_open){
        if(drain_on_close)(void)FlushFileBuffers(p->h);
        (void)PurgeComm(p->h,PURGE_RXCLEAR);
        if(p->saved_dcb_valid)(void)SetCommState(p->h,&p->saved_dcb);
        if(p->saved_timeouts_valid)(void)SetCommTimeouts(p->h,&p->saved_timeouts);
        CloseHandle(p->h);
        p->is_open=0;
        p->saved_dcb_valid=0;
        p->saved_timeouts_valid=0;
    }
}
static void port_close(struct radio_port*p){port_close_internal(p,1);}
static void port_close_after_firmware(struct radio_port*p){port_close_internal(p,0);}
static int port_set_speed(struct radio_port*p,unsigned baud){return port_set_baud(p,(DWORD)baud);}
static void port_flush(struct radio_port*p){PurgeComm(p->h,PURGE_RXCLEAR|PURGE_TXCLEAR);}
static int port_drain(struct radio_port*p){return FlushFileBuffers(p->h)?0:-1;}
static int port_write_all(struct radio_port*p,const void*buf,size_t len){const uint8_t*q=(const uint8_t*)buf;while(len){DWORD n=0;if(!WriteFile(p->h,q,(DWORD)len,&n,NULL)||n==0){errno=EIO;return -1;}q+=n;len-=n;}return 0;}
static int port_read_exact_timeout(struct radio_port*p,void*buf,size_t len,int timeout_ms){uint8_t*q=(uint8_t*)buf;size_t got=0;COMMTIMEOUTS to;GetCommTimeouts(p->h,&to);to.ReadTotalTimeoutConstant=(DWORD)timeout_ms;SetCommTimeouts(p->h,&to);while(got<len){DWORD n=0;if(!ReadFile(p->h,q+got,(DWORD)(len-got),&n,NULL)){errno=EIO;return -1;}if(!n){errno=ETIMEDOUT;return -1;}got+=n;}return 0;}
#else
#ifdef __linux__
/*
 * TIOCEXCL prevents later opens, but Linux deliberately allows it to be set
 * even when another process already has the tty open.  That is dangerous for
 * us because termios settings belong to the tty, not to one file descriptor:
 * changing 1200/9600 baud can disrupt a terminal/debugger that was there
 * first.  Inspect /proc for another open file descriptor before touching the
 * device, then repeat the check after TIOCEXCL to close the small race window.
 */
static int linux_tty_rdev_open_elsewhere(dev_t rdev)
{
    DIR *proc;
    struct dirent *pe;
    pid_t self=getpid();
    int found=0;

    proc=opendir("/proc");
    if(!proc)return 0; /* Best effort; TIOCEXCL still protects later opens. */
    while(!found && (pe=readdir(proc))!=NULL){
        char *end=NULL;
        long pid;
        char fd_dir[64];
        DIR *fds;
        struct dirent *fe;

        if(!isdigit((unsigned char)pe->d_name[0]))continue;
        errno=0;
        pid=strtol(pe->d_name,&end,10);
        if(errno || !end || *end || pid<=0 || (pid_t)pid==self)continue;
        if(snprintf(fd_dir,sizeof(fd_dir),"/proc/%ld/fd",pid)>=(int)sizeof(fd_dir))continue;
        fds=opendir(fd_dir);
        if(!fds)continue;
        while((fe=readdir(fds))!=NULL){
            char fd_path[96];
            struct stat st;
            if(fe->d_name[0]=='.')continue;
            if(snprintf(fd_path,sizeof(fd_path),"%s/%s",fd_dir,fe->d_name)>=(int)sizeof(fd_path))continue;
            if(stat(fd_path,&st)==0 && S_ISCHR(st.st_mode) && st.st_rdev==rdev){
                found=1;
                break;
            }
        }
        closedir(fds);
    }
    closedir(proc);
    return found;
}

static int linux_tty_path_open_elsewhere(const char *name)
{
    struct stat st;
    if(stat(name,&st)<0 || !S_ISCHR(st.st_mode))return 0;
    return linux_tty_rdev_open_elsewhere(st.st_rdev);
}
#endif

static int port_open(struct radio_port *p,const char *name)
{
    int saved_errno;
    memset(p,0,sizeof(*p));
    p->fd=-1;
#ifdef __linux__
    if(linux_tty_path_open_elsewhere(name)){
        errno=EBUSY;
        return -1;
    }
#endif
    p->fd=open(name,O_RDWR|O_NOCTTY|O_SYNC);
    if(p->fd<0)return -1;
#ifdef TIOCEXCL
    /* Claim the tty exclusively so no other process can open it after us. */
    if(ioctl(p->fd,TIOCEXCL)<0){
        saved_errno=errno;
        close(p->fd);
        p->fd=-1;
        errno=saved_errno;
        return -1;
    }
#endif
#ifdef __linux__
    {
        struct stat st;
        if(fstat(p->fd,&st)==0 && S_ISCHR(st.st_mode) &&
           linux_tty_rdev_open_elsewhere(st.st_rdev)){
            saved_errno=EBUSY;
#ifdef TIOCNXCL
            (void)ioctl(p->fd,TIOCNXCL);
#endif
            close(p->fd);
            p->fd=-1;
            errno=saved_errno;
            return -1;
        }
    }
#endif
    if(tcgetattr(p->fd,&p->saved_tio)<0){
        saved_errno=errno;
#ifdef TIOCNXCL
        (void)ioctl(p->fd,TIOCNXCL);
#endif
        close(p->fd);
        p->fd=-1;
        errno=saved_errno;
        return -1;
    }
    p->saved_tio_valid=1;
#ifdef TIOCMGET
    if(ioctl(p->fd,TIOCMGET,&p->saved_modem_bits)==0)p->saved_modem_valid=1;
#endif
    p->is_open=1;
    return 0;
}
/* Other close paths retain their existing drain; firmware writes already
   drained EOF, and repeating tcdrain here can stall on a bad USB-UART. */
static void port_close_internal(struct radio_port*p,int drain_on_close)
{
    if(p->is_open){
        int saved_errno=errno;
        if(drain_on_close)(void)tcdrain(p->fd);
        (void)tcflush(p->fd,TCIFLUSH);
        if(p->saved_tio_valid)(void)tcsetattr(p->fd,TCSANOW,&p->saved_tio);
#if defined(TIOCMBIS) && defined(TIOCMBIC)
        if(p->saved_modem_valid){
            int mask=TIOCM_DTR|TIOCM_RTS;
            int setbits=p->saved_modem_bits&mask;
            int clearbits=(~p->saved_modem_bits)&mask;
            if(setbits)(void)ioctl(p->fd,TIOCMBIS,&setbits);
            if(clearbits)(void)ioctl(p->fd,TIOCMBIC,&clearbits);
        }
#endif
#ifdef TIOCNXCL
        (void)ioctl(p->fd,TIOCNXCL);
#endif
        close(p->fd);
        p->fd=-1;
        p->is_open=0;
        p->saved_tio_valid=0;
        p->saved_modem_valid=0;
        errno=saved_errno;
    }
}
static void port_close(struct radio_port*p){port_close_internal(p,1);}
static void port_close_after_firmware(struct radio_port*p){port_close_internal(p,0);}
static int port_set_speed(struct radio_port *p, unsigned baud)
{
    struct termios tio;
    speed_t speed = (baud == 1200) ? B1200 : B9600;
    if (tcgetattr(p->fd, &tio) < 0) return -1;
    cfmakeraw(&tio);
    tio.c_cflag &= ~(PARENB | PARODD | CSIZE);
#ifdef CRTSCTS
    tio.c_cflag &= ~CRTSCTS;
#endif
#ifdef CCTS_OFLOW
    tio.c_cflag &= ~CCTS_OFLOW;
#endif
#ifdef CRTS_IFLOW
    tio.c_cflag &= ~CRTS_IFLOW;
#endif
    tio.c_cflag |= CS8 | CSTOPB | CLOCAL | CREAD;
    tio.c_iflag &= ~(IXON | IXOFF | IXANY);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    if (cfsetispeed(&tio, speed) < 0 || cfsetospeed(&tio, speed) < 0) return -1;
    if (tcsetattr(p->fd, TCSANOW, &tio) < 0) return -1;
#ifdef TIOCMBIS
    { int bits = TIOCM_DTR | TIOCM_RTS; (void)ioctl(p->fd, TIOCMBIS, &bits); }
#endif
    return 0;
}
static void port_flush(struct radio_port*p){tcflush(p->fd,TCIOFLUSH);}
static int port_drain(struct radio_port*p){return tcdrain(p->fd);}
static int port_write_all(struct radio_port*p,const void*buf,size_t len){const uint8_t*q=(const uint8_t*)buf;while(len){ssize_t n=write(p->fd,q,len);if(n<0){if(errno==EINTR)continue;return -1;}if(n==0){errno=EIO;return -1;}q+=n;len-=(size_t)n;}return 0;}
static int port_read_exact_timeout(struct radio_port*p,void*buf,size_t len,int timeout_ms){uint8_t*q=(uint8_t*)buf;size_t got=0;while(got<len){struct pollfd f;int pr;f.fd=p->fd;f.events=POLLIN;f.revents=0;do{pr=poll(&f,1,timeout_ms);}while(pr<0&&errno==EINTR);if(pr==0){errno=ETIMEDOUT;return -1;}if(pr<0)return -1;if(f.revents&(POLLERR|POLLHUP|POLLNVAL)){errno=EIO;return -1;}{ssize_t n=read(p->fd,q+got,len-got);if(n<0){if(errno==EINTR||errno==EAGAIN)continue;return -1;}if(n==0)continue;got+=(size_t)n;}}return 0;}
#endif

static int port_read_exact(struct radio_port*p,void*buf,size_t len){return port_read_exact_timeout(p,buf,len,TIMEOUT_MS);}

static void report_port_open_error(const char *name)
{
    int e=errno;
    const char *shown=port_display_name(name);
    if(e==EBUSY){
        fprintf(stderr,"Serial port is already in use by another process: %s\n",shown);
    }else if(e==EACCES||e==EPERM){
#ifdef _WIN32
        fprintf(stderr,"Access denied opening serial port %s. It may already be in use by another process.\n",shown);
#else
        fprintf(stderr,"Permission denied opening serial port: %s\n",shown);
#endif
    }else if(e==ENOENT||e==ENODEV||e==ENXIO){
        fprintf(stderr,"Serial port is not available: %s\n",shown);
    }else{
        errno=e;
        perror(name);
    }
}

/* Keep a busy-port failure visible and let the user fix the common case
   without falling back through several menus.  Returns 1 to retry, 0 to abort. */
static int open_firmware_loader_port_retry(struct app_state *st,struct radio_port *p);

static int busy_port_retry_prompt(const char *name)
{
    int k;
    (void)name;
    if (g_cli_noninteractive) {
        cli_mark_needs_interaction("serial port is busy; retry or abort decision required");
        return 0;
    }
    for(;;){
        ui_flush_input();
        printf("Press Enter to retry, or A to abort: ");
        fflush(stdout);
        k=ui_read_key();
        if(k==UI_KEY_ENTER || k==UI_KEY_SPACE){
            printf("\nSelected: Retry serial port\n");
            return 1;
        }
        if(tolower((unsigned char)k)=='a' || k==UI_KEY_ESCAPE){
            printf("\nSelected: Abort\n");
            return 0;
        }
        if(tolower((unsigned char)k)=='x'){
            ui_force_plain();
            continue;
        }
    }
}

/* ----------------------------- Radio protocol ---------------------------- */

static int read_block(struct radio_port *p, uint16_t addr, uint8_t data[BLOCK_SIZE])
{
    uint8_t req[4]={'R',(uint8_t)(addr>>8),(uint8_t)addr,BLOCK_SIZE};
    uint8_t first,hdr[4],ack; uint16_t raddr;
    if(port_write_all(p,req,sizeof(req))<0||port_read_exact(p,&first,1)<0)return -1;
    if(first==ACK){if(port_read_exact(p,hdr,sizeof(hdr))<0)return -1;}
    else if(first=='W'){hdr[0]=first;if(port_read_exact(p,hdr+1,3)<0)return -1;}
    else {fprintf(stderr,"read %04X: unexpected reply %02X\n",addr,first);errno=EPROTO;return -1;}
    raddr=((uint16_t)hdr[1]<<8)|hdr[2];
    if(hdr[0]!='W'||raddr!=addr||hdr[3]!=BLOCK_SIZE){fprintf(stderr,"read %04X: bad header\n",addr);errno=EPROTO;return -1;}
    if(port_read_exact(p,data,BLOCK_SIZE)<0)return -1;
    ack=ACK;if(port_write_all(p,&ack,1)<0||port_read_exact(p,&ack,1)<0)return -1;
    if(ack!=ACK){errno=EPROTO;return -1;}return 0;
}

static int write_block(struct radio_port *p,uint16_t addr,const uint8_t data[BLOCK_SIZE])
{
    uint8_t hdr[4]={'W',(uint8_t)(addr>>8),(uint8_t)addr,BLOCK_SIZE},ack;
    if(addr<CHANNEL_START){errno=EPERM;return -1;}
    if(port_write_all(p,hdr,sizeof(hdr))<0||port_write_all(p,data,BLOCK_SIZE)<0||port_read_exact(p,&ack,1)<0)return -1;
    if(ack!=ACK){fprintf(stderr,"write %04X: expected ACK, got %02X\n",addr,ack);errno=EPROTO;return -1;}return 0;
}

static void store_recovered_password(struct radio_port *p, const uint8_t raw[PASSWORD_LENGTH])
{
    size_t i, n = PASSWORD_LENGTH;
    while (n && (raw[n - 1] == 0x00 || raw[n - 1] == 0xFF)) n--;
    for (i = 0; i < n; i++)
        p->password[i] = (raw[i] >= 0x20 && raw[i] <= 0x7E) ? (char)raw[i] : '?';
    p->password[n] = '\0';
    p->password_known = 1;
    p->password_enabled = 1;
}

static int authenticate_if_needed(struct radio_port *p,const char ident[8])
{
    uint8_t block[BLOCK_SIZE],msg[1+PASSWORD_LENGTH],ack;
    if(ident[6]!='Y'){
        p->password_known=1;
        p->password_enabled=0;
        p->password[0]='\0';
        return 0;
    }
    printf("Radio programming password is enabled. Recovering the stored password...\n");
    if(read_block(p,CHANNEL_START,block)<0)return -1;
    store_recovered_password(p,block+PASSWORD_OFFSET);
    msg[0]=0x04;memcpy(msg+1,block+PASSWORD_OFFSET,PASSWORD_LENGTH);
    if(port_write_all(p,msg,sizeof(msg))<0||port_read_exact(p,&ack,1)<0)return -1;
    if(ack!=ACK){errno=EACCES;return -1;}
    printf("Programming password: %s (recovered and verified)\n",
           p->password[0] ? p->password : "<empty>");
    return 0;
}

static int request_program_id_timeout(struct radio_port *p, char ident[8], int timeout_ms)
{
    uint8_t b = 0x02;
    if (port_write_all(p, &b, 1) < 0 ||
        port_read_exact_timeout(p, ident, 7, timeout_ms) < 0)
        return -1;
    ident[7] = '\0';
    if (!ident_plausible(ident)) {
        errno = ENODEV;
        return -1;
    }
    b = ACK;
    if (port_write_all(p, &b, 1) < 0) return -1;
    if (authenticate_if_needed(p, ident) < 0) {
        /* A valid ID proves the radio is already in normal PROGRAM mode.
           Do not let the caller mistake a password/authentication failure for
           "not in PROGRAM mode" and send the 1200-baud PROGRAM string. */
        errno = EACCES;
        return -1;
    }
    return 0;
}

static int request_program_id(struct radio_port *p, char ident[8])
{
    return request_program_id_timeout(p, ident, TIMEOUT_MS);
}

static void exit_program_mode(struct radio_port *p);

static int send_program_probe(struct radio_port *p, int timeout_ms, uint8_t *reply)
{
    static const uint8_t program[] = "PROGRAM";
    uint8_t b = 0;
    port_flush(p);
    if (port_write_all(p, program, sizeof(program) - 1) < 0 ||
        port_drain(p) < 0)
        return -1;
    if (port_read_exact_timeout(p, &b, 1, timeout_ms) < 0) {
        if (reply) *reply = 0;
        return -1;
    }
    if (reply) *reply = b;
    return b == ACK ? 0 : 1;
}

static void recover_failed_program_probe(struct radio_port *p)
{
    uint8_t e = 'E';

    /* If PROGRAM was accepted but its ACK was lost, the radio has already
       switched to the 9600-baud clone protocol. Reset that state at 9600;
       never send a 1200-baud recovery byte into an active PROGRAM session. */
    if (port_set_speed(p, 9600) < 0) return;
    port_flush(p);
    if (port_write_all(p, &e, 1) == 0) (void)port_drain(p);
    sleep_ms(75);
    port_flush(p);
}

static int enter_program_mode(struct radio_port *p,char ident[8])
{
    uint8_t b = 0;
    int rc;

    /* A previous failed connection may have left the radio sitting in normal
       PROGRAM mode. Check that state first at its native 9600 baud. Sending
       the 1200-baud PROGRAM entry string first can desynchronize it. */
    if (port_set_speed(p, 9600) < 0) return -1;
    port_flush(p);
    rc = request_program_id_timeout(p, ident, 300);
    if (rc == 0)
        return 0;
    if (errno == EACCES) {
        fprintf(stderr, "Radio is already in PROGRAM mode, but password authentication failed.\n");
        return -1;
    }

    /* Not already in PROGRAM mode. Enter it normally at 1200 baud. */
    port_flush(p);
    if (port_set_speed(p, 1200) < 0) return -1;
    rc = send_program_probe(p, TIMEOUT_MS, &b);
    if (rc != 0) {
        if (b != 0)
            fprintf(stderr,"PROGRAM: expected ACK 06, got %02X\n",b);
        else
            fprintf(stderr,"PROGRAM: no ACK received\n");

        /* The ACK can be lost after the radio has already switched to 9600.
           Before resetting anything, see whether PROGRAM mode is actually up. */
        if (port_set_speed(p, 9600) < 0) return -1;
        port_flush(p);
        sleep_ms(50);
        rc = request_program_id_timeout(p, ident, 500);
        if (rc == 0)
            return 0;
        if (errno == EACCES) {
            fprintf(stderr, "Radio entered PROGRAM mode, but password authentication failed.\n");
            return -1;
        }

        printf("Retrying after programming-state reset...\n");
        recover_failed_program_probe(p);
        if (port_set_speed(p, 1200) < 0) return -1;
        b = 0;
        rc = send_program_probe(p, TIMEOUT_MS, &b);
        if (rc != 0) {
            if (b != 0)
                fprintf(stderr,"PROGRAM retry: expected ACK 06, got %02X\n",b);

            /* The retry ACK may also have been lost. One final 9600-baud ID
               probe distinguishes that from a real entry failure. */
            if (port_set_speed(p, 9600) < 0) return -1;
            port_flush(p);
            sleep_ms(50);
            rc = request_program_id_timeout(p, ident, 500);
            if (rc == 0)
                return 0;
            if (errno == EACCES) {
                fprintf(stderr, "Radio entered PROGRAM mode, but password authentication failed.\n");
                return -1;
            }

            errno = EPROTO;
            return -1;
        }
    }

    if (port_set_speed(p, 9600) < 0) return -1;

    /* The radio needs a short settling period after entering PROGRAM mode.
       If the first ID request misses that window, retry once. */
    sleep_ms(50);
    if (request_program_id(p, ident) < 0) {
        if (errno == EACCES) {
            fprintf(stderr, "Radio entered PROGRAM mode, but password authentication failed.\n");
            exit_program_mode(p);
            errno = EACCES;
            return -1;
        }
        port_flush(p);
        sleep_ms(100);
        if (request_program_id(p, ident) < 0) {
            int saved_errno = errno;
            exit_program_mode(p);
            if (saved_errno == EACCES) {
                fprintf(stderr, "Radio entered PROGRAM mode, but password authentication failed.\n");
                errno = EACCES;
            } else {
                errno = ENODEV;
            }
            return -1;
        }
    }
    return 0;
}

static void exit_program_mode(struct radio_port*p)
{
    uint8_t e='E';(void)port_write_all(p,&e,1);(void)port_drain(p);sleep_ms(50);
}

static int read_region(struct radio_port *p, uint16_t start, size_t len,
                       uint8_t *out, const char *what)
{
    size_t pos, total;
    if ((start & (BLOCK_SIZE - 1u)) || (len & (BLOCK_SIZE - 1u))) {
        errno = EINVAL;
        return -1;
    }
    total = len / BLOCK_SIZE;
    for (pos = 0; pos < len; pos += BLOCK_SIZE) {
        uint16_t a = (uint16_t)(start + pos);
        size_t done = pos / BLOCK_SIZE + 1;
        progress_line("Reading", what, done, total, total, a,
                      (unsigned)(a + BLOCK_SIZE - 1));
        if (read_block(p, a, out + pos) < 0) {
            progress_done();
            return -1;
        }
    }
    progress_done();
    return 0;
}

static int verify_block(struct radio_port*p,uint16_t addr,const uint8_t target[BLOCK_SIZE])
{
    uint8_t b[BLOCK_SIZE];if(read_block(p,addr,b)<0)return -1;if(memcmp(b,target,BLOCK_SIZE)){fprintf(stderr,"Immediate verification failed at %04X\n",addr);errno=EIO;return -1;}return 0;
}

/* --------------------------- Firmware matching --------------------------- */

static const struct firmware_revision *find_full_revision(const char sha[65])
{
    size_t i;for(i=0;i<FW_DB_COUNT;i++)if(!strcmp(sha,fw_db[i].full_sha))return &fw_db[i];return NULL;
}


/* Informational 512-byte fingerprints of five factory-reference images
   and one reconstructed TK-840 9306 reference. Store only 64-bit SHA-256
   prefixes (51 x 8 bytes per revision), never copyrighted image data.
   Similarity MUST NOT authorize patching or conversion. */
#define FW_FINGERPRINT_BLOCK_SIZE 512u
#define FW_FINGERPRINT_COUNT (PROGRAM_SIZE / FW_FINGERPRINT_BLOCK_SIZE)
static const char *const firmware_block_fingerprints[FW_DB_COUNT] = {
    "8F7E6A604B2EBF352F0D5CF82800728BE8D428154DA828C3220B2373869D9B248CC1CEE7D93E27CA0F210C8B0157A9F5B81C386CC7A9AC292DD300931C4E758A38E011D82D809299F6B86E65767486594D28CBBA485B893DE0168EC71A37AF0871E60B32D2A9234D189C3D87FD9C5F0A3BA9CE9ADFE67B5173B557F49B8950E7A6A86B9F6DBBA8DFE3B9DDA12F913C4DC059115CF037C6C6FDD110260465EA25F45DCF0727C8B84C1E8B189CB5750C44921EB86F9C37113D06D74F2DBEEA4128DB0CC6499C529FDF51CD7194BF581D1AC076F885A63001D446DE75234DF206788D53EE576037235D97933F9119660FAE4C31B4C67D72C5FA8B5532FA903ACC7D49249AD191DDCED77BF5D79A7BCC0E0987C76CC6B08B4537B5ADEFCE3B455E72CF692587DDDE2D6121BEE373E7F8D65092628C0040AE6E66CF77DBC7F510496DD198155713B7954CD815151C9DA3B7B311DBD28D155BE2E2FE3DAF9C33476F87A0B1709BACDDA7D9384EE9BA2F4E397E69759383B1CFA05E8222FA9EF6787B039F56CDA75FEFEAB99F56CDA75FEFEAB96000186B8CF558D8",
    "452812B9BF14E660E725A19548B39EBC3345DD3569E606A3220B2373869D9B240D7F144CA20910E0AD332187788BE2E9EE0A3E52F466133E3BAAF51331774D599E1832E9CFFB559BE02B1BC22DC2FA62B6020662185CF1F7504AF506ACF61BF8E9D08B7A5E810809959018A4406C0F4EA473E8EF89397D19D46B3326EB6EA0716757FA983D8AFEE6C543889E41FEE01890DF44DFB00D34829F2E3F7B4DBE690252FA1357BB6AE9E8ACB8C77ED8017B4F85322E81E6A2273BBA911F3E25BF821BB4A24CC14A48A2F19C7D3E35463B0ADDE8E47F4F666A09AFBE37962B919426463ECCF3C737030D85E74BCD39033FD5C7F15D540C661290A8658AE387DDB411FB3972A6EA519F41D60FA944D669E90BFD8F937F19B38EDE0410EBC0575B1829FBFFC70EE4BF9A42DE5099A612106C65A0DB8FE32CA2C179C7F951D3EFCA8FBDDC580D3D91C983E70A5F491027028B729C37990C19257AFB38ABCAEC3C9FB5D40458A2669CD10B91F5F3B33853A8E87FF1232BC959C1F037AB52B178A4FD19C2A6CD387C062EBC13C19F56CDA75FEFEAB96000186B8CF558D8",
    "7028921ABDEF2D8E705B0C00710FA95C7BAE54C79FA58EDFF3AEF97152B669E62779DA55AD2DF031BB11BAB4028DAA843920B7525E8B10E850475374AA17236A0677C21F8E7D58F36848A4DFC564A4C20F1B39D5BB2C6CF3767640FAB1CF5DACABFCE951722464C48808FEFC0852BB44E9AD076E1152BE63209737C5C3E80509C256ED75E70D8E5121C564857ACA440663F7DBAFE6087A2FAB27AE73E8C3340BFB8CE55EB361207EC817554F4D9F4C6ED36AA937B98172F36D7D8112A03F81442FAFC6EE1B12FEA31F015A9A804EC12CF1B8BE0E580DE060E56309B1CA3ECF970A18BAA4A3A01698352F6A7EE956C163A910C1B0869A745552EBB925434A6886FE346979A8346C3EACD2CCCDE543FA1DBD796A9A364DC1875981119039600F4D23051E973CBE5141BC55701F7F68A3EB55A4ED22C43ED2348432B02B382F84CF60571AEE43E1C091EB8FB8C298ACB0BE59AC70E6D0BB11EB19D00E6D6E5E85D61752DF9FE4BAD3338F6F0DAD065171CB5B32CD9F020917C971F684EE1A0E2F637448C0C683AAD22D15A6DACE4CE56BF86000186B8CF558D8",
    "518F2D2F1E4A59ECC49A7E827BB6F3C4D02BB012CF970E37FC67C0327A8DCABFFE50EB8AA882768D3358C109728BD16D8748508C8FB64B4F0CF2A5454F2F1AB8F8D261695A25FDB1AADCCC88387B116DC96492F40B216767F2270CDB320F83E2E18706465234466B866FC78101B16388E47981CF87069014B868CC6E57130A6A0988BAF9CDB5AAD107F0364E58D9CC2FE60EEAA171C657645893EB4D2072D1A995D349DD4EEA7F5FCC14E0E0C4F5C1FA0652CBC65B4598B65F48496E99A3F3D640A0C5674FF7FBC09DBC8C3E6C314BB5FA701FCBE0B309521D6D9F6C0FD0B4B2F360A4359BB786962477FF3622264843D39CDE77E1AE9377C3A47872C0231416F1772C5AFE5D98D0089F828077FF2ECCA41AD2EBB298521F927FF17668F380AE0E86AF97271E175AC88E59A3D948C4685E2FA2FA176D8500D12414C78363DA3CC3F1E72DF96913A5DEA3AAE3D92515C439C7C3EE19683E031527059F069EA5438FF695192EF26F5839E2AC66183106F2A6FFDF890D1B4FABEC7745F23C5173DEB77BEF9C9F803719B405418D3DC54305A243D3C2D7BF5BFB",
    "159E78DA512C46363A6923089CA4E4220A3ABC764337E0D4D28F5B72326B8DC738813C8846F2E0023E3DE18CCBA61C3FADA66726F3B1574775FA6E56D435FD967F307BA61F3C94C063A841E60C1EF0A116B7C527B5D692C89ADACED5D2D36C42923EAABA9501A3DD8616088F5AB5E73EFCB78DAC0C480DB6BAEC2F6B5FA04017A3D98721234921133B20C1D114126FC1DD28FB662B560B9FC02A33E5BD97D3DBBCE74B7A9A0A334F8B0DE00C433BF655457284A44B96D45267EB65CB37B8980C9D5128C158E87B970A02AB93A86C4CE4B6DC6B06EC0DCBB44F29FFD15B891719746238C0A3664EADE68E8DA09CD2A7F01919F4D32F9AA86469C9F5D4B38FA997752FF79EF1284C62E0057DD34FFA1D5CC63E6FDB905DD786BB2F583BBB407736579CB7A12CF8FA4DB47A7FBEF24D49845051C9787675B466F7D1EC55262AB36ABF7346D23274E697715ADC1B8D5C509C91BCD149B3415D662D045ED6D41C880BB26E04A6201A4B6FAD2E5FE77E341EF697890AF8435886F3E22821137415775B13AB56AC89AE8FE0E37B91BC542E6D5CAB0216F8AC1956AC",
    "F8931FDB32EF1B92C62E54100D8116B0E0987B2F5F310F1148D08D2E1E23699A38E52CDE52FF8B69AC3FCCE18D124737478AECFAC6B871FA34BA2448269101798C2FFB7ED5B6F0C2411A28558FF2EF9802F28F02007C8F7A770853384BA1621D511FC3772F6668D4F290BFFA611D40560501CB05427E94CC2540C70F339A2A0BCD9436CD8E9332617BE86161CF36881FC7FA0CCAE22A85850655B4400AB273D9B7D762C4B3F21D2F132314FCBE3BF164690C3F1410DC53BF82F6B664ADBBB744F480FCF2C25FE87F0794249E7FD8A20573AF7297619DF5D8509DC59A7D9C70E83AD023A8F07441CC8F22BD5E52540EBA9FBEA0EEB8048B6E3D63373DD6E97811AC00F968F0E40EC4191BC24E62493FF0E15CD4574E64B37E6C27A4B8CEB91540B3861F251EAF3B6E10ADB0C662375B3334E7AB164A269E510AACE13118924FD03CE9E2AC7D82E02CD2B001FF550E3930E30260D9A9BDA9C6793EB8068AC53693DA07732AEF81067A021E031E4E01A9EA016B7BC9834F6F1925671129E9211B3364DAFA10AF3B36861E967B5233A49465D7C6A09F9FF4B6D1",
};

static void print_firmware_similarity(const uint8_t *image, const char whole_sha[65])
{
    char blocks[FW_FINGERPRINT_COUNT][17];
    char digest[65];
    size_t i, revision, match, best_count = 0, best_revision = FW_DB_COUNT;
    int tie = 0;
    if (find_full_revision(whole_sha)) return; /* already exactly identified */
    for (i = 0; i < FW_FINGERPRINT_COUNT; i++) {
        sha256_hex(image + i * FW_FINGERPRINT_BLOCK_SIZE,
                   FW_FINGERPRINT_BLOCK_SIZE, digest);
        memcpy(blocks[i], digest, 16);
        blocks[i][16] = '\0';
    }
    for (revision = 0; revision < FW_DB_COUNT; revision++) {
        match = 0;
        for (i = 0; i < FW_FINGERPRINT_COUNT; i++)
            if (!strncmp(blocks[i],
                         firmware_block_fingerprints[revision] + i * 16, 16))
                match++;
        if (match > best_count) {
            best_count = match;
            best_revision = revision;
            tie = 0;
        } else if (match == best_count && match) {
            tie = 1;
        }
    }
    /* Below a majority match is not a useful tentative attribution. */
    if (best_count < (FW_FINGERPRINT_COUNT / 2 + 1) ||
        best_revision == FW_DB_COUNT || tie) {
        printf("Block comparison: no confident match to a known stock image.\n");
        return;
    }
    if (best_count >= FW_FINGERPRINT_COUNT - 1u)
        printf("Firmware appears to be a modified %s image.\n",
               fw_db[best_revision].label);
    else
        printf("Firmware resembles %s; changes are not identified.\n",
               fw_db[best_revision].label);
    printf("Stock block match: %zu/%zu (512 bytes per block).\n",
           best_count, (size_t)FW_FINGERPRINT_COUNT);
    printf("Blocks differing from stock (CPU address ranges):\n");
    {
        int in_run = 0;
        size_t first = 0, shown = 0;
        for (i = 0; i <= FW_FINGERPRINT_COUNT; i++) {
            int changed = i < FW_FINGERPRINT_COUNT &&
                strncmp(blocks[i],
                        firmware_block_fingerprints[best_revision] + i * 16,
                        16) != 0;
            if (changed && !in_run) {
                first = i;
                in_run = 1;
            }
            if (!changed && in_run) {
                if (shown < 8) {
                    unsigned lo = PROGRAM_START +
                                  (unsigned)(first * FW_FINGERPRINT_BLOCK_SIZE);
                    unsigned hi = PROGRAM_START +
                                  (unsigned)(i * FW_FINGERPRINT_BLOCK_SIZE) - 1u;
                    printf("  %04X-%04X\n", lo, hi);
                }
                shown++;
                in_run = 0;
            }
        }
        if (shown > 8) printf("  ... (%zu ranges total)\n", shown);
        if (!shown) printf("  None\n");
    }
    printf("These are block boundaries, not the exact modified addresses.\n");
    printf("Block similarity alone does not verify a patch or conversion.\n");
}

static const struct firmware_revision *find_fingerprint_revision(
    const uint8_t block[BLOCK_SIZE])
{
    char sha[65];
    size_t i;
    sha256_hex(block,BLOCK_SIZE,sha);
    for(i=0;i<FW_DB_COUNT;i++)
        if(!strcmp(sha,fw_db[i].fingerprint_sha))return &fw_db[i];
    return NULL;
}

static const struct firmware_revision *find_quick_revision(const uint8_t *img)
{
    return find_fingerprint_revision(img+FW_FINGERPRINT_OFFSET);
}

static int masked_match(const uint8_t*p,const uint8_t*pat,const uint8_t*mask,size_t n){size_t i;for(i=0;i<n;i++)if(mask[i]&&p[i]!=pat[i])return 0;return 1;}

static int find_stock_tx(const uint8_t*img,size_t*where)
{
    static const uint8_t pat[16]={0x28,0,0,0x71,0x9E,0x0A,0x2D,0xF0,0x09,0x1A,0x20,0x3A,0x22,0x01,0x14,0};
    static const uint8_t msk[16]={1,0,0,1,1,1,1,1,1,1,1,1,1,1,1,0};
    size_t i,n=0,last=0;for(i=0;i+16<=PROGRAM_SIZE;i++)if(masked_match(img+i,pat,msk,16)){last=i;n++;}if(n!=1)return(int)n;*where=last;return 1;
}
static int find_fixed_tx(const uint8_t*img,size_t*where)
{
    static const uint8_t tail[10]={0x47,0x47,0x47,0x59,0x24,0x01,0x5D,0x03,0x9F,0x14};
    size_t i,n=0,last=0;for(i=0;i+16<=PROGRAM_SIZE;i++)if(img[i]==0x28&&img[i+3]==0x83&&!memcmp(img+i+5,tail,10)){last=i;n++;}if(n!=1)return(int)n;*where=last;return 1;
}
static int cpu_to_off(uint16_t a,size_t*off){if(a<PROGRAM_START||a>=PROGRAM_START+PROGRAM_SIZE)return -1;*off=(size_t)(a-PROGRAM_START);return 0;}
static int resolve_wrapper(const uint8_t*img,size_t site,uint16_t*helper,uint16_t*errret)
{
    size_t wo;uint16_t w=rd16le(img+site+1);static const uint8_t pat[13]={0x28,0,0,0x83,0,0x47,0x59,0x24,0x01,0x5D,0x03,0x9F,0x56};static const uint8_t msk[13]={1,0,0,1,0,1,1,1,1,1,1,1,1};
    if (cpu_to_off(w, &wo) < 0 || wo + sizeof(pat) > PROGRAM_SIZE) return -1;
    if (!masked_match(img + wo, pat, msk, sizeof(pat)) || wo == 0 || img[wo-1] != 0x56) return -1;
    *helper = rd16le(img + wo + 1);
    *errret = (uint16_t)(w - 1);
    return 0;
}
static int find_zero_path(const uint8_t*img,size_t site,uint16_t*target)
{
    static const uint8_t pat[9]={0x2D,0xF0,0x09,0x1A,0x20,0x3A,0x22,0x01,0x14};size_t lo=site>0x100?site-0x100:0,i,n=0,last=0;for(i=lo;i+sizeof(pat)<=site;i++)if(!memcmp(img+i,pat,sizeof(pat))){last=i;n++;}if(n!=1)return(int)n;*target=(uint16_t)(PROGRAM_START+last);return 1;
}
static int rel8(uint16_t next,uint16_t target,uint8_t*out){int d=(int)target-(int)next;if(d< -128||d>127)return -1;*out=(uint8_t)(int8_t)d;return 0;}
static int build_tx_patch(const uint8_t*img,size_t site,uint8_t patch[16])
{
    uint16_t helper = 0, errret = 0, zero = 0;
    uint16_t cpu = (uint16_t)(PROGRAM_START + site);
    uint8_t e, z;
    if (resolve_wrapper(img, site, &helper, &errret) < 0) return -1;
    if (find_zero_path(img, site, &zero) != 1) return -1;
    if (rel8((uint16_t)(cpu + 5), errret, &e) < 0) return -1;
    if (rel8((uint16_t)(cpu + 16), zero, &z) < 0) return -1;
    patch[0]=0x28;patch[1]=(uint8_t)helper;patch[2]=(uint8_t)(helper>>8);patch[3]=0x83;patch[4]=e;patch[5]=0x47;patch[6]=0x47;patch[7]=0x47;patch[8]=0x59;patch[9]=0x24;patch[10]=0x01;patch[11]=0x5D;patch[12]=0x03;patch[13]=0x9F;patch[14]=0x14;patch[15]=z;return 0;
}

static enum tx_behavior classify_tx_full(const uint8_t*img,size_t*site_out)
{
    size_t s=0;int n=find_stock_tx(img,&s);if(n==1){if(site_out)*site_out=s;return TX_FIXED_OFFSET;}n=find_fixed_tx(img,&s);if(n==1){if(site_out)*site_out=s;return TX_FIXED;}return TX_UNKNOWN;
}

/* ---------------------- Dealer/alignment TX patch ----------------------- */

/* E680 + (FE23-1)*4 dealer/alignment-record lookup. */
static int valid_dealer_lookup(const uint8_t *img,uint16_t addr)
{
    static const uint8_t pat[]={
        0x20,0x23,0xAF,0x01,0x80,0x05,0x60,0x80,0xE6,0x14,0x08,
        0xC9,0xBB,0x04,0x05,0x0B,0x2D,0x80,0xE6,0x56
    };
    size_t o;
    if(cpu_to_off(addr,&o)<0 || o+sizeof(pat)>PROGRAM_SIZE) return 0;
    return !memcmp(img+o,pat,sizeof(pat));
}

/* Validate the stock synthesizer writer called by the dealer TX routine. */
static int valid_dealer_synth(const uint8_t *img,uint16_t addr)
{
    static const uint8_t pat[]={0x1C,0x20,0x38,0x22,0xFE,0x35,0x01,0x35,
                                0x80,0xA0,0x00,0x20,0x22,0x38,0x21,0xF0};
    size_t o;
    if(cpu_to_off(addr,&o)<0 || o+sizeof(pat)>PROGRAM_SIZE) return 0;
    return !memcmp(img+o,pat,sizeof(pat));
}

/* Validate the stock dealer settle/wait routine. */
static int valid_dealer_settle(const uint8_t *img,uint16_t addr)
{
    static const uint8_t pat[]={0x0B,0x8E,0x10,0x27,0xA5,0xD5,0x2B,
                                0x82,0x80,0x00,0x08,0xA5,0xD5};
    size_t o;
    if(cpu_to_off(addr,&o)<0 || o+sizeof(pat)>PROGRAM_SIZE) return 0;
    return !memcmp(img+o,pat,sizeof(pat));
}

/* Find the stock dealer/alignment TX routine.  Absolute CALL destinations are
   deliberately wildcarded, then independently validated, so this relocates
   safely across the known TK-941/940 firmware revisions. */
static int find_stock_dealer_tx(const uint8_t *img,size_t *where,uint16_t *lookup,
                                int *plus25)
{
    static const uint8_t pat[]={
        0x28,0,0, 0x24,0xC8,0x58,0x24,0x01,0x5C,0x03,0x9F,
        0x08,0xA4,0x78,0x0A, 0x2D,0xF0,0x09, 0x1A,0x20, 0x3A,0x22,0x01,
        0x14,0x18, 0x08,0xA0,0xDA,0x05, 0x2D,0xE0,0xFB, 0x14,0x03,
        0x2D,0xC0,0xFD, 0x1A,0x20, 0x82,0x05, 0x3A,0x22,0x01,
        0x14,0x03, 0x3A,0x22,0x00, 0x28,0,0, 0x28,0,0, 0x56
    };
    uint8_t mask[sizeof(pat)];
    size_t i,n=0,last=0;
    uint16_t l=0;
    memset(mask,1,sizeof(mask));
    mask[1]=mask[2]=0;
    /* The seven-byte historical +25 MHz change includes a two-byte NOP.
       Wildcard this complete site, then validate it as an exact known
       stock, short, or full instruction sequence. */
    for(i=34u;i<41u;i++)mask[i]=0;
    mask[50]=mask[51]=0; mask[53]=mask[54]=0;
    for(i=0;i+sizeof(pat)<=PROGRAM_SIZE;i++){
        uint16_t tl,ts,tw;
        if(!masked_match(img+i,pat,mask,sizeof(pat))) continue;
        /* The older +25 MHz modification changes this dealer conversion from
           ADDW AX,#FDC0 to ADDW AX,#0220.  Accept either as an input state.
           Once dealer TX is changed to use the stored E680 TX word, it must
           rejoin the original FDC0 conversion because that word is encoded
           from the dealer table's 812 MHz TX base. */
        if(tkx4x_plus25_site_kind(img+i+34)<0)continue;
        tl=rd16le(img+i+1); ts=rd16le(img+i+50); tw=rd16le(img+i+53);
        if(!valid_dealer_lookup(img,tl) || !valid_dealer_synth(img,ts) ||
           !valid_dealer_settle(img,tw)) continue;
        last=i; l=tl; n++;
    }
    if(n!=1) return (int)n;
    *where=last; *lookup=l;
    if(plus25) *plus25=!memcmp(img+last+34,"\x2D\x20\x02",3);
    return 1;
}

/* Corrected dealer/alignment TX-field patch signature.  It selects the second
   word of each E680 record, then rejoins Kenwood's untouched ADDW AX,#FDC0
   conversion path. */
static int find_fixed_dealer_tx(const uint8_t *img,size_t *where)
{
    static const uint8_t pat[]={
        0x28,0,0, 0x24,0xC8, 0x46,0x46, 0x58, 0x24,0x01,
        0x5C, 0x03,0x9F, 0x14,0x13
    };
    uint8_t mask[sizeof(pat)];
    size_t i,n=0,last=0;
    memset(mask,1,sizeof(mask)); mask[1]=mask[2]=0;
    for(i=0;i+sizeof(pat)<=PROGRAM_SIZE;i++){
        uint16_t lookup;
        if(!masked_match(img+i,pat,mask,sizeof(pat))) continue;
        lookup=rd16le(img+i+1);
        if(!valid_dealer_lookup(img,lookup)) continue;
        if(i+37>PROGRAM_SIZE || memcmp(img+i+34,"\x2D\xC0\xFD",3)) continue;
        last=i; n++;
    }
    if(n!=1) return (int)n;
    *where=last; return 1;
}

static void build_dealer_tx_patch(uint16_t lookup,uint8_t patch[15])
{
    static const uint8_t tmpl[]={
        0x28,0,0, 0x24,0xC8, 0x46,0x46, 0x58, 0x24,0x01,
        0x5C, 0x03,0x9F, 0x14,0x13
    };
    memcpy(patch,tmpl,sizeof(tmpl));
    patch[1]=(uint8_t)lookup;
    patch[2]=(uint8_t)(lookup>>8);
}

static enum dealer_tx_behavior classify_dealer_tx_full(const uint8_t *img,size_t *site_out)
{
    size_t site=0;
    uint16_t lookup=0;
    int n=find_stock_dealer_tx(img,&site,&lookup,NULL);
    if(n==1){if(site_out)*site_out=site;return DEALER_TX_FIXED_OFFSET;}
    n=find_fixed_dealer_tx(img,&site);
    if(n==1){if(site_out)*site_out=site;return DEALER_TX_FIELD;}
    return DEALER_TX_UNKNOWN;
}

/* Two validated layouts. The TK-840 8355 uses the same setup
   technique as AB00 but has only a conventional countdown. Both
   regions must match stock or patched bytes in their entirety. */
struct tot_patch_region {
    uint16_t address;
    size_t length;
    uint8_t original[48];
    uint8_t modified[48];
};
static const struct tot_patch_region ab00_tot_regions[3] = {
    {0x94E6u, 48u, {0x08, 0xA1, 0x79, 0x03, 0x72, 0x7F, 0x0C, 0x09, 0xF0, 0x62, 0xE6, 0xAF, 0x28, 0x83, 0x0E, 0xB9, 0x03, 0x14, 0x0A, 0x09, 0xF0, 0x61, 0xE6, 0xAF, 0x28, 0x83, 0x02, 0xB9, 0x0B, 0xC1, 0xB8, 0x00, 0xD8, 0x62, 0xF4, 0x01, 0x05, 0x2A, 0x24, 0x0A, 0x1A, 0x40, 0xA0, 0x7D, 0x3A, 0x42, 0x0A, 0x56},
     {0x08, 0xA1, 0x79, 0x03, 0x72, 0x7F, 0x20, 0x09, 0xF0, 0x62, 0xE6, 0xB8, 0x03, 0xAF, 0xFF, 0x42, 0x08, 0x11, 0x7D, 0xAF, 0x28, 0x83, 0x01, 0xD0, 0xC1, 0xBB, 0xFA, 0x05, 0x0B, 0x31, 0xC8, 0x1A, 0x40, 0xA0, 0x7D, 0x3A, 0x42, 0x0A, 0x56, 0x09, 0xF0, 0x61, 0xE6, 0xB8, 0x0B, 0x40, 0x14, 0xE0}},
    {0xB19Au, 29u, {0x08, 0xD5, 0xD5, 0x01, 0x56, 0x27, 0x42, 0x80, 0x13, 0x3A, 0x42, 0x0A, 0x07, 0xE9, 0x40, 0x1C, 0x40, 0x2F, 0x00, 0x00, 0x81, 0x01, 0x56, 0xB0, 0x7D, 0x28, 0xDC, 0xC2, 0x56},
     {0x08, 0xD5, 0xD5, 0x01, 0x56, 0x27, 0x42, 0x80, 0xFB, 0x3A, 0x42, 0x0A, 0x71, 0x7D, 0xF5, 0x1C, 0x40, 0x2E, 0x01, 0x00, 0x1A, 0x40, 0x80, 0xEC, 0xB0, 0x7D, 0x2C, 0xDC, 0xC2}},
    {0xD6F2u, 40u, {0x07, 0xE9, 0xA0, 0x1C, 0xA0, 0x2F, 0x00, 0x00, 0x80, 0x02, 0xA5, 0xC4, 0x08, 0xA7, 0x78, 0x0C, 0x07, 0xE9, 0x40, 0x1C, 0x40, 0x2F, 0x00, 0x00, 0x80, 0x02, 0xB0, 0x7D, 0x07, 0xE9, 0xAE, 0x1C, 0xAE, 0x2F, 0x00, 0x00, 0x80, 0x02, 0xB1, 0xDB},
     {0x1C, 0xA0, 0x2E, 0x01, 0x00, 0x1A, 0xA0, 0x80, 0x02, 0xA5, 0xC4, 0x08, 0xA7, 0x78, 0x0E, 0x71, 0x7D, 0x0B, 0x1C, 0x40, 0x2E, 0x01, 0x00, 0x1A, 0x40, 0x80, 0x02, 0xB0, 0x7D, 0x1C, 0xAE, 0x2E, 0x01, 0x00, 0x1A, 0xAE, 0x80, 0x02, 0xB1, 0xDB}}
};


static const struct tot_patch_region tk840_tot_regions[2] = {
    {0x94E9u, 48u, {0x08, 0xA1, 0x79, 0x03, 0x72, 0x7F, 0x0C, 0x09, 0xF0, 0x62, 0xE6, 0xAF, 0x28, 0x83, 0x0E, 0xB9, 0x03, 0x14, 0x0A, 0x09, 0xF0, 0x61, 0xE6, 0xAF, 0x28, 0x83, 0x02, 0xB9, 0x0B, 0xC1, 0xB8, 0x00, 0xD8, 0x62, 0xF4, 0x01, 0x05, 0x2A, 0x24, 0x0A, 0x1A, 0x40, 0xA0, 0x7D, 0x3A, 0x42, 0x0A, 0x56},
     {0x08, 0xA1, 0x79, 0x03, 0x72, 0x7F, 0x20, 0x09, 0xF0, 0x62, 0xE6, 0xB8, 0x03, 0xAF, 0xFF, 0x42, 0x08, 0x11, 0x7D, 0xAF, 0x28, 0x83, 0x01, 0xD0, 0xC1, 0xBB, 0xFA, 0x05, 0x0B, 0x31, 0xC8, 0x1A, 0x40, 0xA0, 0x7D, 0x3A, 0x42, 0x0A, 0x56, 0x09, 0xF0, 0x61, 0xE6, 0xB8, 0x0B, 0x40, 0x14, 0xE0}},
    {0xB0DEu, 29u, {0x08, 0xD5, 0xD5, 0x01, 0x56, 0x27, 0x42, 0x80, 0x13, 0x3A, 0x42, 0x0A, 0x07, 0xE9, 0x40, 0x1C, 0x40, 0x2F, 0x00, 0x00, 0x81, 0x01, 0x56, 0xB0, 0x7D, 0x28, 0x39, 0xC6, 0x56},
     {0x08, 0xD5, 0xD5, 0x01, 0x56, 0x27, 0x42, 0x80, 0xFB, 0x3A, 0x42, 0x0A, 0x71, 0x7D, 0xF5, 0x1C, 0x40, 0x2E, 0x01, 0x00, 0x1A, 0x40, 0x80, 0xEC, 0xB0, 0x7D, 0x2C, 0x39, 0xC6}}
};

static const struct tot_patch_region *tot_regions_for_revision(
    const struct firmware_revision *rev, size_t *count)
{
    if(count)*count=0;
    if(!rev)return NULL;
    if(rev->checksum==0xAB00 && rev->model!=MODEL_840){
        if(count)*count=3;
        return ab00_tot_regions;
    }
    if(rev->checksum==0x8355 && rev->model==MODEL_840){
        if(count)*count=2;
        return tk840_tot_regions;
    }
    if(rev->checksum==0x9306 && rev->model==MODEL_840){
        /* Both 9306 routines are instruction-identical to 8355 except
           for relocation and the C639 -> C6F2 jump destination.
           Derive the recipes so the hardware-tested 8355 template
           remains the single source of patch bytes. */
        static struct tot_patch_region r9306[2];
        static int ready=0;
        if(!ready){
            memcpy(r9306,tk840_tot_regions,sizeof(r9306));
            r9306[0].address=0x94EFu;
            r9306[1].address=0xB15Fu;
            r9306[1].original[26]=0xF2;
            r9306[1].modified[27]=0xF2;
            ready=1;
        }
        if(count)*count=2;
        return r9306;
    }
    return NULL;
}

/* Known firmware only. Verify every original or patched byte at the
   documented CPU addresses; never search for relocated code or attempt
   to transpose a recipe from another revision. */
#define TOT_MAX_REGIONS 3u
struct tot_patch_match {
    const struct tot_patch_region *regions;
    size_t count;
    size_t offsets[TOT_MAX_REGIONS];
};

/* The accelerated test image changes exactly two bytes in the fully patched
   8355/AB00 routines. Reject partial acceleration and unrelated edits.
   These are CPU addresses, not offsets in a binary firmware image. */
static int tot_test_10x_matches(const uint8_t *img,
                               const struct firmware_revision *rev)
{
    const struct tot_patch_region *regions;
    size_t count=0,i,j;
    regions=tot_regions_for_revision(rev,&count);
    if(!regions)return 0;
    for(i=0;i<count;i++){
        const struct tot_patch_region *r=&regions[i];
        const uint8_t *p=img+(r->address-PROGRAM_START);
        size_t speed_index=(i==0u)?37u:11u;
        if(i>=2u){
            if(memcmp(p,r->modified,r->length))return 0;
            continue;
        }
        if(speed_index>=r->length || r->modified[speed_index]!=0x0A ||
           p[speed_index]!=0x01)return 0;
        for(j=0;j<r->length;j++)
            if(j!=speed_index && p[j]!=r->modified[j])return 0;
    }
    return 1;
}

static int tot_test_10x_remove(uint8_t *img,
                               const struct firmware_revision *rev)
{
    const struct tot_patch_region *regions;
    size_t count=0;
    if(!tot_test_10x_matches(img,rev))return 0;
    regions=tot_regions_for_revision(rev,&count);
    if(!regions || count<2u)return 0;
    img[regions[0].address-PROGRAM_START+37u]=0x0A;
    img[regions[1].address-PROGRAM_START+11u]=0x0A;
    return 1;
}

static int tot_region_bytes_match(const uint8_t *p,
                                   const struct tot_patch_region *r,
                                   size_t region_index,int patched)
{
    (void)region_index;
    return !memcmp(p,patched?r->modified:r->original,r->length);
}

static enum tot_behavior tot_match_at(const uint8_t *img,
                                      const struct tot_patch_region *regions,
                                      size_t count,struct tot_patch_match *out)
{
    unsigned stock=0,patched=0;
    size_t i;
    if(!regions || !count || count>TOT_MAX_REGIONS)return TOT_UNSUPPORTED;
    for(i=0;i<count;i++){
        const struct tot_patch_region *r=&regions[i];
        size_t off=(size_t)(r->address-PROGRAM_START);
        if(off+r->length>PROGRAM_SIZE)return TOT_UNSUPPORTED;
        if(tot_region_bytes_match(img+off,r,i,0))stock++;
        else if(tot_region_bytes_match(img+off,r,i,1))patched++;
        else return TOT_UNSUPPORTED;
        if(out)out->offsets[i]=off;
    }
    if(stock!=count && patched!=count)return TOT_CONFLICT;
    if(out){out->regions=regions;out->count=count;}
    return stock==count?TOT_STOCK:TOT_PATCHED;
}

/* The historical two-byte NOP/NOP TOT bypass keeps the rest of the
   countdown routine unchanged. Detect it at known revision-specific
   addresses, never by a blind search. */
static int tot_legacy_region(const uint8_t *p,
                             const struct tot_patch_region *r)
{
    size_t i;
    if(r->length<=21u || r->original[20]!=0x81 || r->original[21]!=0x01 ||
       p[20]!=0x00 || p[21]!=0x00)return 0;
    for(i=0;i<r->length;i++)
        if(i!=20u && i!=21u && p[i]!=r->original[i])return 0;
    return 1;
}

static int tot_legacy_for_revision(const uint8_t *img,
                                    const struct firmware_revision *rev)
{
    const struct tot_patch_region *regions;
    size_t count=0,i;
    if(!rev || (rev->checksum!=0xAB00 && rev->checksum!=0x8355))
        return 0;
    regions=tot_regions_for_revision(rev,&count);
    if(!regions || count<2u)return 0;
    for(i=0;i<count;i++){
        const struct tot_patch_region *r=&regions[i];
        const uint8_t *p=img+(r->address-PROGRAM_START);
        if(i==1u){
            if(!tot_legacy_region(p,r))return 0;
        }else if(!tot_region_bytes_match(p,r,i,0))return 0;
    }
    return 1;
}

/* 9306 has a known two-byte unconditional TOT bypass at B173-B174.
   Its byte-for-byte original was reconstructed from a user-supplied image.
   Do not accept the checksum alone: only the exact reconstructed full
   SHA-256, before or after undoing this two-byte change, is supported.
   This is intentionally NOT authorization to apply the 8355 TOT patch. */
#define TK840_9306_TOT_ADDR 0xB173u
static enum tot_behavior tk840_9306_tot_state(
    const uint8_t *img,const struct firmware_revision *rev)
{
    uint8_t normalized[PROGRAM_SIZE];
    size_t off=TK840_9306_TOT_ADDR-PROGRAM_START;
    char sha[65];
    if(!rev || rev->model!=MODEL_840 || rev->checksum!=0x9306)
        return TOT_UNSUPPORTED;
    sha256_hex(img,PROGRAM_SIZE,sha);
    if(!strcmp(sha,rev->full_sha))return TOT_STOCK;
    if(img[off]!=0x00 || img[off+1u]!=0x00)return TOT_UNSUPPORTED;
    memcpy(normalized,img,PROGRAM_SIZE);
    normalized[off]=0x81;
    normalized[off+1u]=0x01;
    sha256_hex(normalized,PROGRAM_SIZE,sha);
    return !strcmp(sha,rev->full_sha)?TOT_LEGACY:TOT_UNSUPPORTED;
}

static enum tot_behavior tot_classify_for_revision(
    const uint8_t *img,const struct firmware_revision *rev,
    enum model_family model,struct tot_patch_match *out)
{
    const struct tot_patch_region *regions;
    size_t count=0;
    if(!rev)return TOT_UNSUPPORTED;
    if(model!=MODEL_UNKNOWN && model!=rev->model &&
       !(model==MODEL_940 && rev->model==MODEL_941) &&
       !(model==MODEL_941 && rev->model==MODEL_940))
        return TOT_UNSUPPORTED;
    if(rev->model==MODEL_840 && rev->checksum==0x9306){
        enum tot_behavior exact=tk840_9306_tot_state(img,rev);
        /* Stock 9306 must continue into tot_match_at() so the caller's
           region offsets are initialized before applying the patch. */
        if(exact==TOT_LEGACY)return exact;
    }
    regions=tot_regions_for_revision(rev,&count);
    if(!regions)return TOT_UNSUPPORTED;
    if(tot_legacy_for_revision(img,rev))
        return TOT_LEGACY;
    if(tot_test_10x_matches(img,rev))return TOT_PATCHED;
    return tot_match_at(img,regions,count,out);
}

static enum tot_behavior locate_tot_patch_for_model(
    const uint8_t *img,enum model_family model,struct tot_patch_match *out)
{
    return tot_classify_for_revision(img,find_quick_revision(img),model,out);
}

static enum tot_behavior classify_tot_firmware(const uint8_t *img)
{
    return locate_tot_patch_for_model(img,MODEL_UNKNOWN,NULL);
}

static enum tot_behavior classify_tot_firmware_for_model(
    const uint8_t *img,enum model_family model)
{
    return locate_tot_patch_for_model(img,model,NULL);
}

static int apply_tot_patch_for_revision(uint8_t *img,
                                        const struct firmware_revision *rev,
                                        enum model_family model)
{
    struct tot_patch_match match={0};
    enum tot_behavior state=tot_classify_for_revision(img,rev,model,&match);
    size_t i;
    if(state==TOT_PATCHED){
        printf("Enhanced TOT Patch already present. No changes needed.\n");
        return 0;
    }
    if(state==TOT_LEGACY){
        printf("This firmware already has the older unconditional TOT-disable patch.\n");
        printf("The selectable Enhanced TOT Patch was not applied.\n");
        return -1;
    }
    if(state==TOT_CONFLICT){
        printf("Part of the Enhanced TOT Patch is already present, but not all "
               "of it. No changes were made.\n");
        return -1;
    }
    if(state!=TOT_STOCK){
        printf("The required Enhanced TOT Patch bytes were not found at the expected "
               "locations. No changes were made.\n");
        return -1;
    }
    /* A stock classification alone must not authorize use of an
       uninitialized patch descriptor. Leave the image unchanged. */
    if(!match.regions || !match.count || match.count>TOT_MAX_REGIONS){
        fprintf(stderr,"Enhanced TOT Patch region data unavailable. No changes made.\n");
        return -1;
    }
    if(rev && rev->model==MODEL_840 && rev->checksum==0x9306){
        char original_sha[65];
        sha256_hex(img,PROGRAM_SIZE,original_sha);
        if(strcmp(original_sha,rev->full_sha)){
            printf("9306 firmware does not match the reconstructed stock "
                   "reference. No changes made.\n");
            return -1;
        }
    }
    for(i=0;i<match.count;i++){
        const struct tot_patch_region *r=&match.regions[i];
        uint8_t *site=img+match.offsets[i];
        memcpy(site,r->modified,r->length);
    }
    if(tot_classify_for_revision(img,rev,model,NULL)!=TOT_PATCHED){
        fprintf(stderr,"Enhanced TOT Patch post-patch validation failed.\n");
        return -1;
    }

    printf("Enhanced TOT Patch applied to the firmware image at known locations.\n");
    return 0;
}

static int apply_tot_patch_for_model(uint8_t *img,enum model_family model)
{
    return apply_tot_patch_for_revision(img,find_quick_revision(img),model);
}

static uint8_t detected_firmware_patch_flags(const uint8_t *img,
                                               enum model_family model)
{
    uint8_t flags=0;
    if(classify_tx_full(img,NULL)==TX_FIXED &&
       classify_dealer_tx_full(img,NULL)==DEALER_TX_FIELD)flags|=MOD_TAG_TX;
    if(classify_tot_firmware_for_model(img,model)==TOT_PATCHED)
        flags|=MOD_TAG_TOT;
    return flags;
}

static int patch_metadata_value(uint8_t v,uint8_t *flags)
{
    if(v==0xFF){if(flags)*flags=0;return 0;}
    if((v&MOD_TAG_PREFIX_MASK)==MOD_TAG_PREFIX){
        if(flags)*flags=(uint8_t)(v&~MOD_TAG_PREFIX_MASK);
        return 1;
    }
    if((v&MOD_TAG_PREFIX_MASK)==MOD_TAG_COMPAT_PREFIX){
        /* The alternate encoding indicates complete Enhanced TX only when both
           TX-path bits are set; bit 3 encodes TOT. */
        uint8_t old=(uint8_t)(v&~MOD_TAG_PREFIX_MASK);
        uint8_t result=(uint8_t)(old & (MOD_TAG_EEPROM|0x30u));
        if((old&0x03u)==0x03u)result|=MOD_TAG_TX;
        if(old&0x08u)result|=MOD_TAG_TOT;
        if(flags)*flags=result;
        return 1;
    }
    if(flags)*flags=0;
    return -1;
}

static int author_signature_writable(const uint8_t *p)
{
    size_t i;
    int erased=1;
    /* Accept only the exact project signature or a completely erased slot.
       Preserve all other contents, including partial signatures. */
    if(!memcmp(p,AUTHOR_SIG_TEXT,AUTHOR_SIG_LEN))return 1;
    for(i=0;i<AUTHOR_SIG_LEN;i++)
        if(p[i]!=0xFF)erased=0;
    return erased;
}

static int apply_project_markings(uint8_t *img,const struct app_state *st)
{
    int changed=0;
    const uint8_t *sig=img+AUTHOR_SIG_OFFSET;
    uint8_t prior_tag=img[MOD_TAG_OFFSET];
    int trusted=find_quick_revision(img)!=NULL;
    int signed_by_us=!memcmp(sig,AUTHOR_SIG_TEXT,AUTHOR_SIG_LEN);
    int erased=1;
    size_t j;
    for(j=0;j<AUTHOR_SIG_LEN;j++)if(sig[j]!=0xFF)erased=0;
    /* Check prior values before touching the signature. */
    if(st->include_author_signature){
        int okay=author_signature_writable(sig);
        if(okay){
            if(memcmp(sig,AUTHOR_SIG_TEXT,AUTHOR_SIG_LEN)){
                memcpy(img+AUTHOR_SIG_OFFSET,AUTHOR_SIG_TEXT,AUTHOR_SIG_LEN);
                changed=1;
            }
        }else printf("Note: author-signature area is already in use; leaving it unchanged.\n");
    }
    if(st->include_modified_tag){
        uint8_t v=prior_tag,existing=0;
        uint8_t detected=detected_firmware_patch_flags(img,st->patch_model);
        int metadata=patch_metadata_value(v,&existing);
        if(!trusted && (v!=0xFF || (!erased && !signed_by_us))){
            printf("Patch-flags byte at E5DF is not empty in this firmware; "
                   "patch flags were not recorded.\n");
        }else if(v==0xFF || metadata==1){
            uint8_t keep=(metadata==1)?(uint8_t)(existing&~(MOD_TAG_TX|MOD_TAG_TOT)):0;
            uint8_t nv=(uint8_t)(MOD_TAG_PREFIX|keep|detected);
            if(v!=nv){img[MOD_TAG_OFFSET]=nv;changed=1;}
        }else{
            printf("Note: firmware patch-flags byte is already in use; leaving it unchanged.\n");
        }
    }
    return changed;
}

/* User-facing Enhanced TX Patch.  It is intentionally one operation made
   from two independently detected pieces: conventional channel TX and the
   dealer/alignment/test-frequency TX path.  Both must be safely recognized
   before either mutation is committed. */
static int apply_tx_fix(uint8_t *img,const struct app_state*st)
{
    size_t tx_site=0,dealer_site=0;
    uint16_t dealer_lookup=0;
    int dealer_plus25=0;
    uint8_t tx_patch[16],dealer_patch[15];
    enum tx_behavior tx;
    enum dealer_tx_behavior dealer;
    const struct firmware_revision *rev=find_quick_revision(img);
    int change_tx=0,change_dealer=0;

    /* Enhanced TX supersedes the old fixed +25 MHz modification. Normalize
       all three sites, not only the dealer conversion, before applying it.
       All callers use a working image and commit only on verification. */
    if(rev && rev->model!=MODEL_840 &&
       tkx4x_plus25_state(img,rev->checksum)==TKX4X_PLUS25_PRESENT){
        if(tkx4x_plus25_restore(img,rev->checksum)<0)return -1;
        printf("Replaced legacy +25 MHz offset patch with Enhanced TX Patch.\n");
    }
    tx=classify_tx_full(img,&tx_site);
    dealer=classify_dealer_tx_full(img,&dealer_site);
    if(tx==TX_FIXED_OFFSET){
        if(build_tx_patch(img,tx_site,tx_patch)<0){
            fprintf(stderr,"The channel TX code was found, but the toolkit could not safely determine how to patch it.\n");
            return -1;
        }
        change_tx=1;
    } else if(tx!=TX_FIXED){
        fprintf(stderr,"The toolkit could not safely identify the conventional TX code in this firmware.\n");
        return -1;
    }

    if(dealer==DEALER_TX_FIXED_OFFSET){
        if(find_stock_dealer_tx(img,&dealer_site,&dealer_lookup,&dealer_plus25)!=1){
            fprintf(stderr,"The toolkit could not safely resolve the dealer/alignment TX patch location.\n");
            return -1;
        }
        build_dealer_tx_patch(dealer_lookup,dealer_patch);
        change_dealer=1;
    } else if(dealer!=DEALER_TX_FIELD){
        fprintf(stderr,"The toolkit could not safely identify the dealer/alignment TX code in this firmware.\n");
        return -1;
    }

    if(change_tx){
        memcpy(img+tx_site,tx_patch,sizeof(tx_patch));
        printf("Enhanced TX Patch: channel TX updated at CPU %04X.\n",
               (unsigned)(PROGRAM_START+tx_site));
    }
    if(change_dealer){
        memcpy(img+dealer_site,dealer_patch,sizeof(dealer_patch));
        if(dealer_plus25)
            memcpy(img+dealer_site+34,"\x2D\xC0\xFD",3);
        printf("Enhanced TX Patch: dealer/test TX updated at CPU %04X.\n",
               (unsigned)(PROGRAM_START+dealer_site));
    }
    apply_project_markings(img,st);
    return 0;
}


static const struct firmware_revision *identify_firmware_image(const uint8_t *img,
                                                                  char full_sha[65])
{
    sha256_hex(img, PROGRAM_SIZE, full_sha);
    {
        const struct firmware_revision *rev = find_full_revision(full_sha);
        if (rev) return rev;
    }
    return find_quick_revision(img);
}

static void classify_loaded_firmware(struct app_state *st)
{
    uint8_t metadata_flags=0;
    int metadata;
    st->firmware_sum = byte_sum16(st->firmware, PROGRAM_SIZE);
    st->firmware_rev = identify_firmware_image(st->firmware, st->firmware_sha);
    st->tx = classify_tx_full(st->firmware, NULL);
    st->dealer_tx = classify_dealer_tx_full(st->firmware, NULL);
    st->tot = classify_tot_firmware_for_model(st->firmware,st->patch_model);
    st->tot_test_10x=tot_test_10x_matches(st->firmware,st->firmware_rev);
    metadata=patch_metadata_value(st->firmware[MOD_TAG_OFFSET],&metadata_flags);
    st->chirp_tag_known = metadata >= 0;
    st->chirp_tag_present = metadata == 1;
    st->firmware_patch_flags = metadata == 1 ? metadata_flags : 0;
}

/* Only after a completed, successful radio read/write may an in-memory
   classification be used as an observation of the installed firmware. */
static void remember_loaded_firmware_on_radio(struct app_state *st)
{
    st->quick_rev = st->firmware_rev;
    st->radio_fingerprinted = 1;
    st->radio_full_read = 1;
    st->radio_tx = st->tx;
    st->radio_dealer_tx = st->dealer_tx;
    st->radio_tot = st->tot;
    st->radio_chirp_tag_known = st->chirp_tag_known;
    st->radio_chirp_tag_present = st->chirp_tag_present;
    st->radio_firmware_patch_flags = st->firmware_patch_flags;
}

static void forget_radio_firmware_observation(struct app_state *st)
{
    st->quick_rev = NULL;
    st->radio_fingerprinted = 0;
    st->radio_full_read = 0;
    st->radio_tx = TX_UNKNOWN;
    st->radio_dealer_tx = DEALER_TX_UNKNOWN;
    st->radio_tot = TOT_UNSUPPORTED;
    st->radio_chirp_tag_known = 0;
    st->radio_chirp_tag_present = 0;
    st->radio_firmware_patch_flags = 0;
}

static int check_firmware_target_model(const struct app_state *st,
                                       const uint8_t *target,
                                       const char *operation)
{
    char sha[65];
    const char *op = operation ? operation : "firmware write";
    const struct firmware_revision *rev = identify_firmware_image(target, sha);
    if (st->model == MODEL_UNKNOWN) {
        fprintf(stderr,
                "WARNING: Before %s, the connected radio model could not be positively identified.\n"
                "The toolkit cannot verify that this firmware matches the radio model.\n", op);
        if (!ask_yes_no("Write this firmware anyway?", 0)) {
            printf("Firmware write aborted.\n");
            return -1;
        }
        printf("Selected: write firmware to unidentified radio anyway\n");
        return 0;
    }
    if (!rev) {
        fprintf(stderr,
                "WARNING: For this %s, the firmware image is not a revision recognized by this toolkit.\n"
                "The image passed the basic firmware-format checks, but its target model\n"
                "cannot be positively verified against the connected %s.\n",
                op, model_name(st->model));
        if (!ask_yes_no("Write this unrecognized firmware anyway?", 0)) {
            printf("Firmware write aborted.\n");
            return -1;
        }
        printf("Selected: write unrecognized firmware anyway\n");
        return 0;
    }
    if (rev->model != st->model) {
        /* TK-940 and TK-941 are identical from a programming standpoint.
           Their differences are hardware-only. */
        if ((rev->model == MODEL_940 || rev->model == MODEL_941) &&
            (st->model == MODEL_940 || st->model == MODEL_941))
            return 0;

        fprintf(stderr,
                "WARNING: %s firmware is for %s, but the connected radio is %s.\n",
                rev->label, model_name(rev->model), model_name(st->model));
        if (!ask_yes_no("Write this firmware anyway?", 0)) {
            printf("Firmware write aborted.\n");
            return -1;
        }
        printf("Selected: write mismatched firmware anyway\n");
    }
    return 0;
}

/* ---------------------------- Channel preview ---------------------------- */

struct cache_entry {uint16_t addr;int valid;uint8_t data[BLOCK_SIZE];};
/* Ten preview channels can cross many different EEPROM blocks. */
struct block_cache {struct radio_port*p;struct cache_entry e[64];size_t n;};

static int cache_get_block(struct block_cache*c,uint16_t addr,uint8_t **out)
{
    size_t i;addr=(uint16_t)(addr&~(BLOCK_SIZE-1u));for(i=0;i<c->n;i++)if(c->e[i].valid&&c->e[i].addr==addr){*out=c->e[i].data;return 0;}if(c->n>=sizeof(c->e)/sizeof(c->e[0])){errno=ENOSPC;return -1;}c->e[c->n].addr=addr;c->e[c->n].valid=1;if(read_block(c->p,addr,c->e[c->n].data)<0)return -1;*out=c->e[c->n].data;c->n++;return 0;
}
static int cache_read_span(struct block_cache*c,uint16_t addr,uint8_t*out,size_t len)
{
    while(len){uint8_t*b;uint16_t ba=(uint16_t)(addr&~(BLOCK_SIZE-1u));size_t off=addr-ba,n=BLOCK_SIZE-off;if(n>len)n=len;if(cache_get_block(c,ba,&b)<0)return -1;memcpy(out,b+off,n);addr=(uint16_t)(addr+n);out+=n;len-=n;}return 0;
}
static void clean_name(const uint8_t raw[8],char out[9])
{
    int i,last=-1;for(i=0;i<8;i++){uint8_t c=raw[i];out[i]=(c>=0x20&&c<=0x7e)?(char)c:' ';if(out[i]!=' ')last=i;}out[last+1]='\0';
}
static uint32_t decode_rx_word(enum model_family model,uint16_t w)
{
    if(model==MODEL_940||model==MODEL_941){if(w==0xFFFF)return 0;return 441400000u+(uint32_t)w*12500u;}
    if(model==MODEL_840){uint16_t code=(uint16_t)(w&0x7FFF);if(code==0x7FFF)return 0;return 300000000u+(uint32_t)code*12500u;}
    return 0;
}

static const char *model_frequency_name(enum model_family model)
{
    switch (model) {
    case MODEL_840: return "TK-840 family";
    case MODEL_940: return "TK-940 (800 MHz)";
    case MODEL_941: return "TK-941 (900 MHz)";
    default: return "unknown TK-x4x model";
    }
}

static enum model_family infer_channel_model(const uint8_t *channels)
{
    unsigned score840 = 0, score940 = 0, score941 = 0;
    unsigned sys;

    /* The channel encoding itself is enough to distinguish the supported
       frequency families in normal use.  Keep these ranges deliberately
       broad so amateur-band programming near the commercial bands still
       classifies sensibly.

       Do NOT use bit 15 as a TK-840 discriminator.  The TK-840 frequency
       decoder ignores/masks bit 15, but valid TK-840 channel words can still
       have that bit set.  A 900-MHz word can therefore decode into a plausible
       TK-840 frequency too.  If the scores tie, MODEL_UNKNOWN is intentional:
       warning suppression is safer than confidently guessing the wrong family. */
    for (sys = 0; sys < SYSTEM_COUNT; sys++) {
        uint16_t ptr = rd16le(channels + sys * 2);
        uint8_t groups = channels[GROUP_COUNT_BASE + sys];
        int off;
        unsigned g;
        if ((ptr & 0x8000) || !groups) continue;
        off = (int)(ptr & 0x3FFF) - (int)SYSTEM_DATA_PTR_BASE;
        if (off < 0 || off + CONV_HEADER_SIZE + (int)groups * (int)CONV_RECORD_SIZE > (int)CHANNEL_SIZE) continue;
        if (channels[off] & 1) continue;
        for (g = 0; g < groups; g++) {
            const uint8_t *rec = channels + off + CONV_HEADER_SIZE + g * CONV_RECORD_SIZE;
            uint16_t w = rd16le(rec + CH_RX_FREQ);
            uint32_t f840 = decode_rx_word(MODEL_840, w);
            uint32_t f9 = decode_rx_word(MODEL_941, w);
            if (f840 >= 380000000u && f840 <= 530000000u) score840++;
            if (f9 >= 780000000u && f9 < 890000000u) score940++;
            if (f9 >= 890000000u && f9 <= 960000000u) score941++;
        }
    }
    if (score840 > score940 && score840 > score941) return MODEL_840;
    if (score940 > score840 && score940 > score941) return MODEL_940;
    if (score941 > score840 && score941 > score940) return MODEL_941;
    return MODEL_UNKNOWN;
}

static int confirm_channel_model_mismatch(const struct app_state *st,
                                          enum model_family channels_model)
{
    if (channels_model == MODEL_UNKNOWN || st->model == MODEL_UNKNOWN ||
        channels_model == st->model)
        return 0;
    fprintf(stderr,
            "WARNING: The loaded channel frequencies appear to be for %s,\n"
            "but the connected radio identifies as %s.\n",
            model_frequency_name(channels_model),
            model_frequency_name(st->model));
    if (!ask_yes_no("Write these channels anyway?", 0)) {
        printf("Channel write aborted.\n");
        return -1;
    }
    printf("Selected: write frequency-mismatched channels anyway\n");
    return 0;
}

static int quick_channel_preview(struct radio_port*p,enum model_family model,const uint8_t first[BLOCK_SIZE],struct channel_preview out[PREVIEW_LINES],size_t*out_count)
{
    struct block_cache c;unsigned sys;size_t count=0;memset(&c,0,sizeof(c));c.p=p;
    /* Seed cache with E600-E67F, which the quick probe already read. */
    c.e[0].addr=CHANNEL_START;c.e[0].valid=1;memcpy(c.e[0].data,first,BLOCK_SIZE);c.n=1;
    for (sys = 0; sys < SYSTEM_COUNT && count < PREVIEW_LINES; sys++) {
        uint16_t ptr = rd16le(first + SYSTEM_PTR_BASE + sys * 2);
        uint8_t groups = first[GROUP_COUNT_BASE + sys];
        int off;
        uint8_t hdr[CONV_HEADER_SIZE];
        unsigned g;
        if ((ptr & 0x8000) || groups == 0) continue;
        off = (int)(ptr & 0x3FFF) - (int)SYSTEM_DATA_PTR_BASE;
        if (off < 0 || off + CONV_HEADER_SIZE + (int)groups * (int)CONV_RECORD_SIZE > (int)CHANNEL_SIZE) continue;
        if (cache_read_span(&c, (uint16_t)(CHANNEL_START + off), hdr, sizeof(hdr)) < 0) return -1;
        if (hdr[0] & 0x01) continue;
        for (g = 0; g < groups && count < PREVIEW_LINES; g++) {
            uint8_t rec[CONV_RECORD_SIZE];
            uint16_t a = (uint16_t)(CHANNEL_START + off + CONV_HEADER_SIZE + g * CONV_RECORD_SIZE);
            if (cache_read_span(&c, a, rec, sizeof(rec)) < 0) return -1;
            out[count].valid = 1;
            out[count].logical = (unsigned)count + 1;
            out[count].rx_hz = decode_rx_word(model, rd16le(rec + CH_RX_FREQ));
            clean_name(rec + CH_NAME, out[count].name);
            count++;
        }
    }
    *out_count=count;return 0;
}

/* Sample just enough channel records to recognize a physical radio.
   Never overwrite channel information loaded from an unrelated file. */
static void sample_radio_preview(struct app_state *st, struct radio_port *p,
                                 const uint8_t *first_block)
{
    uint8_t first[BLOCK_SIZE];
    st->radio_preview_count = 0;
    memset(st->radio_preview, 0, sizeof(st->radio_preview));
    if (!first_block) {
        if (read_block(p, CHANNEL_START, first) < 0) return;
        first_block = first;
    }
    if (quick_channel_preview(p, st->model, first_block,
                              st->radio_preview, &st->radio_preview_count) < 0) {
        memset(st->radio_preview, 0, sizeof(st->radio_preview));
        st->radio_preview_count = 0;
    }
}

static void radio_preview_from_loaded_channels(struct app_state *st)
{
    memcpy(st->radio_preview, st->preview, sizeof(st->radio_preview));
    st->radio_preview_count = st->preview_count;
}

static void print_memory_jogger(const struct app_state *st)
{
    size_t i;
    printf("\nMemory jogger: First %u available channels from the radio\n",
           (unsigned)PREVIEW_LINES);
    printf("to help you identify it (preview only):\n");
    if (!st->radio_preview_count) {
        printf("  No usable channels available for this preview.\n");
        return;
    }
    for (i = 0; i < st->radio_preview_count; i++) {
        const struct channel_preview *c = &st->radio_preview[i];
        if (!c->valid) continue;
        if (c->rx_hz)
            printf("  %2u. %3u.%04u MHz  %.8s\n",
                   c->logical, c->rx_hz / 1000000u,
                   (c->rx_hz % 1000000u) / 100u, c->name);
        else
            printf("  %2u. RX frequency unavailable  %.8s\n",
                   c->logical, c->name);
    }
}

/* ----------------------------- Quick probing ----------------------------- */

static enum tx_behavior quick_tx_from_known(struct radio_port*p,const struct firmware_revision*r)
{
    uint8_t b[BLOCK_SIZE];uint16_t base=(uint16_t)(r->tx_addr&~(BLOCK_SIZE-1u));size_t off=r->tx_addr-base;if(!r->tx_addr)return TX_UNKNOWN;if(read_block(p,base,b)<0)return TX_UNKNOWN;if(off+16>BLOCK_SIZE)return TX_UNKNOWN;if(!memcmp(b+off,r->stock_tx,16))return TX_FIXED_OFFSET;if(!memcmp(b+off,r->fixed_tx,16))return TX_FIXED;return TX_UNKNOWN;
}

static enum tx_behavior quick_scan_tx_block(const uint8_t b[BLOCK_SIZE])
{
    static const uint8_t stockpat[16]={0x28,0,0,0x71,0x9E,0x0A,0x2D,0xF0,0x09,0x1A,0x20,0x3A,0x22,0x01,0x14,0};
    static const uint8_t stockmsk[16]={1,0,0,1,1,1,1,1,1,1,1,1,1,1,1,0};
    static const uint8_t fixtail[10]={0x47,0x47,0x47,0x59,0x24,0x01,0x5D,0x03,0x9F,0x14};size_t i;int ns=0,nf=0;
    for(i=0;i+16<=BLOCK_SIZE;i++){if(masked_match(b+i,stockpat,stockmsk,16))ns++;if(b[i]==0x28&&b[i+3]==0x83&&!memcmp(b+i+5,fixtail,10))nf++;}
    if (ns == 1 && nf == 0) return TX_FIXED_OFFSET;
    if (nf == 1 && ns == 0) return TX_FIXED;
    return TX_UNKNOWN;
}

/* Read only the patch's affected bytes for an AB00 quick probe. */
static enum tot_behavior quick_probe_tot(struct radio_port *p,
                                          const struct firmware_revision *rev)
{
    size_t i,count=0;
    const struct tot_patch_region *regions=tot_regions_for_revision(rev,&count);
    unsigned stock=0,patched=0,legacy=0;
    if(!regions)return TOT_UNSUPPORTED;
    for(i=0;i<count;i++){
        const struct tot_patch_region *r=&regions[i];
        uint16_t base=(uint16_t)(r->address&~(BLOCK_SIZE-1u));
        uint8_t blocks[2*BLOCK_SIZE];
        size_t offset=r->address-base;
        if(read_block(p,base,blocks)<0)return TOT_UNSUPPORTED;
        if(offset+r->length>BLOCK_SIZE &&
           read_block(p,(uint16_t)(base+BLOCK_SIZE),blocks+BLOCK_SIZE)<0)
            return TOT_UNSUPPORTED;
        if(tot_region_bytes_match(blocks+offset,r,i,0))stock++;
        else if(tot_region_bytes_match(blocks+offset,r,i,1))patched++;
        else if(rev && (rev->checksum==0xAB00 || rev->checksum==0x8355) &&
                i==1u && tot_legacy_region(blocks+offset,r))legacy++;
        else return TOT_CONFLICT;
    }
    if(stock==count)return TOT_STOCK;
    if(patched==count)return TOT_PATCHED;
    if(legacy==1u && stock==count-1u)return TOT_LEGACY;
    return TOT_CONFLICT;
}

static int quick_probe(struct app_state *st)
{
    struct radio_port p;
    uint8_t b8000[BLOCK_SIZE], fingerprint[BLOCK_SIZE];
    uint8_t firstcp[BLOCK_SIZE], fend[BLOCK_SIZE];
    char ident[8];
    enum tx_behavior tx = TX_UNKNOWN;
    enum tot_behavior tot = TOT_UNSUPPORTED;

    invalidate_radio_connection(st);
    for (;;) {
        if (port_open(&p, st->port_name) == 0) break;
        {
            int e=errno;
            report_port_open_error(st->port_name);
            if(e==EBUSY && busy_port_retry_prompt(st->port_name))continue;
        }
        return -1;
    }
    if (enter_program_mode(&p, ident) < 0) {
        port_close(&p);
        return -1;
    }
    if (read_block(&p, PROGRAM_START, b8000) < 0 ||
        read_block(&p, FW_FINGERPRINT_ADDR, fingerprint) < 0 ||
        read_block(&p, 0xE580, fend) < 0 ||
        read_block(&p, CHANNEL_START, firstcp) < 0) {
        exit_program_mode(&p);
        port_close(&p);
        return -1;
    }

    st->quick_rev = find_fingerprint_revision(fingerprint);
    st->model = model_from_ident(ident);
    if (st->quick_rev && st->quick_rev->tx_addr)
        tx = quick_tx_from_known(&p, st->quick_rev);
    else if (st->model == MODEL_940 || st->model == MODEL_941) {
        uint8_t a[BLOCK_SIZE], c[BLOCK_SIZE];
        enum tx_behavior ta, tc;
        if (read_block(&p, 0x9A00, a) == 0 && read_block(&p, 0x9C00, c) == 0) {
            ta = quick_scan_tx_block(a);
            tc = quick_scan_tx_block(c);
            if (ta != TX_UNKNOWN && tc == TX_UNKNOWN) tx = ta;
            else if (tc != TX_UNKNOWN && ta == TX_UNKNOWN) tx = tc;
        }
    }

    if(st->quick_rev)tot=quick_probe_tot(&p,st->quick_rev);
    sample_radio_preview(st, &p, firstcp);
    dev_probe_compare_baseline(st, &p, b8000, fend);
    exit_program_mode(&p);
    port_close(&p);

    snprintf(st->ident, sizeof(st->ident), "%s", ident);
    update_password_state(st, &p);
    st->connected = 1;
    st->radio_fingerprinted = 1;
    st->radio_full_read = 0;
    st->radio_tx = tx;
    st->radio_tot = tot;
    st->recovery_known = 1;
    st->recovery_enabled = !!(firstcp[RECOVERY_OFFSET] & RECOVERY_MASK);
    {
        uint8_t flags=0;
        int metadata=patch_metadata_value(
            fend[MOD_TAG_OFFSET - (0xE580u - PROGRAM_START)],&flags);
        st->radio_chirp_tag_known = metadata >= 0;
        st->radio_chirp_tag_present = metadata == 1;
        st->radio_firmware_patch_flags = metadata == 1 ? flags : 0;
        if(st->model==MODEL_840)
            st->radio_dealer_tx=DEALER_TX_UNKNOWN;
        else if(metadata==1 && (flags&MOD_TAG_TX))
            st->radio_dealer_tx=DEALER_TX_FIELD;
        else if(metadata==1)
            st->radio_dealer_tx=DEALER_TX_FIXED_OFFSET;
        else
            st->radio_dealer_tx=DEALER_TX_UNKNOWN;
    }
    return 0;
}

/* ----------------------------- File helpers ------------------------------ */

static int read_file_sized(const char*path,uint8_t**data,size_t*size)
{
    FILE*f=fopen(path,"rb");long sz;uint8_t*p;if(!f){perror(path);return -1;}if(fseek(f,0,SEEK_END)|| (sz=ftell(f))<0 || fseek(f,0,SEEK_SET)){perror(path);fclose(f);return -1;}p=(uint8_t*)malloc((size_t)sz? (size_t)sz:1);if(!p){fclose(f);return -1;}if((size_t)sz&&fread(p,1,(size_t)sz,f)!=(size_t)sz){perror(path);free(p);fclose(f);return -1;}fclose(f);*data=p;*size=(size_t)sz;return 0;
}
static int write_file_new_or_confirm(const char*path,const uint8_t*data,size_t size,int never_overwrite)
{
    FILE*f;if(file_exists(path)){if(never_overwrite){fprintf(stderr,"Cannot overwrite existing file: %s\n",path);return -1;}if(!ask_yes_no("File already exists. Overwrite it?",0))return -2;}
    f=fopen(path,"wb");if(!f){perror(path);return -1;}if(fwrite(data,1,size,f)!=size||fclose(f)!=0){perror(path);return -1;}return 0;
}

/* Diagnostic firmware must never silently be treated as a normal image.
   The HEX editor changes only two bytes; removal edits memory, not the file. */
static void tot_test_10x_warning(void)
{
    printf("\nWARNING: 10x TOT TIMER TEST FIRMWARE DETECTED.\n");
    printf("This is ONLY for testing the Enhanced TOT Patch, NOT normal use.\n");
    printf("A 3-minute timeout becomes 18 seconds; 10 minutes becomes 1 minute.\n");
    printf("FF (Off) still disables Dispatch TOT.\n");
    printf("Restore normal-speed firmware after testing.\n");
}

static void tot_test_10x_offer_on_load(struct app_state *st)
{
    if(!st->tot_test_10x)return;
    tot_test_10x_warning();
    if(st->guided_loading){
        printf("Guided workflow will reject test modifications it cannot normalize.\n");
        return;
    }
    if(g_cli_noninteractive){
        printf("Automatic removal is unavailable in non-interactive mode.\n");
        return;
    }
    if(ask_yes_no("Remove the 10x test modification from the loaded image (not the source file)?",1)){
        if(tot_test_10x_remove(st->firmware,st->firmware_rev)){
            st->firmware_modified=1;
            classify_loaded_firmware(st);
            printf("Selected: Removed 10x timer test modification from memory.\n");
        }
    }else{
        printf("Selected: Keep 10x TIMER TEST modification in loaded image.\n");
    }
}

/* Call before ANY write/restore setup, while the image can still be changed
   without invalidating precomputed page maps and checksums. */
static int tot_test_10x_confirm_write(uint8_t *firmware)
{
    const struct firmware_revision *rev=find_quick_revision(firmware);
    if(!tot_test_10x_matches(firmware,rev))return 0;
    tot_test_10x_warning();
    if(g_cli_noninteractive){
        cli_mark_needs_interaction(
            "10x timer test firmware cannot be written non-interactively; use --interactive");
        return 1;
    }
    for(;;){
        int key;
        printf("\n[R] Remove test acceleration before writing (recommended)\n");
        printf("[T] Write 10x TEST firmware for controlled testing ONLY\n");
        printf("[A] Abort firmware write\n");
        printf("Select [R/T/A] (default R): ");
        fflush(stdout);
        ui_flush_input();
        key=ui_read_key();
        if(key==UI_KEY_ENTER || key==UI_KEY_SPACE ||
           tolower((unsigned char)key)=='r'){
            if(!tot_test_10x_remove(firmware,rev)){
                fprintf(stderr,"Could not safely remove 10x acceleration. Write aborted.\n");
                return 1;
            }
            printf("\nSelected: Restore normal TOT speed before writing.\n");
            return 0;
        }
        if(tolower((unsigned char)key)=='t'){
            printf("\nSelected: Write 10x timer TEST firmware. Not for normal use.\n");
            return 0;
        }
        if(key==UI_KEY_ESCAPE || key==UI_KEY_EOF ||
           tolower((unsigned char)key)=='a' || tolower((unsigned char)key)=='q'){
            printf("\nSelected: Abort firmware write.\n");
            return 1;
        }
    }
}

static int load_firmware_file(struct app_state *st, const char *path)
{
    uint8_t *d;
    size_t n;
    char base[MAX_SOURCE_LEN];
    if (read_file_sized(path, &d, &n) < 0) return -1;
    if (n != PROGRAM_SIZE && n != FULL_SIZE) {
        fprintf(stderr, "The firmware file has an invalid size.\n"
                        "It does not appear to be a valid TK-x4x firmware file.\n");
        free(d);
        return -2;
    }
    memcpy(st->firmware, d, PROGRAM_SIZE);
    free(d);
    st->patch_model=MODEL_UNKNOWN;
    st->firmware_loaded = 1;
    st->firmware_modified = 0;
    st->firmware_hex_family = 0;
    basename_copy(path, base, sizeof(base));
    copy_string(st->firmware_source, sizeof(st->firmware_source), base);
    if(!st->connected)st->model=MODEL_UNKNOWN;
    classify_loaded_firmware(st);
    printf("Firmware successfully loaded from %s.\n", base);
    tot_test_10x_offer_on_load(st);
    printf("Checksum: %04X\nSHA-256: %s\n", st->firmware_sum, st->firmware_sha);
    print_firmware_similarity(st->firmware, st->firmware_sha);
    if(!st->firmware_rev)print_unknown_firmware_guidance(st->model,1,st->firmware_sum);
    return 0;
}



/* Reject malformed HEX structure, but show checksum errors as warnings.
   A stale checksum must never prevent an otherwise readable image from loading. */
static int load_hex_firmware_file(struct app_state *st, const char *path)
{
    uint8_t *source = NULL, *decoded = NULL;
    size_t size = 0;
    int family = 0, rc;
    struct kenwood_hex_warnings warnings = {0};
    char why[384], base[MAX_SOURCE_LEN];
    if (read_file_sized(path, &source, &size) < 0) return -1;
    decoded = (uint8_t *)malloc(PROGRAM_SIZE);
    if (!decoded) {
        free(source);
        fprintf(stderr, "Out of memory loading HEX firmware.\n");
        return -1;
    }
    rc = kenwood_hex_decode_with_warnings(source, size, decoded,
                                          &family, &warnings, why, sizeof(why));
    free(source);
    if (rc != 0) {
        basename_copy(path, base, sizeof(base));
        fprintf(stderr, "\nERROR: Cannot load HEX firmware '%s'.\n%s\n",
                base, why);
        if (!strncmp(why, "Incompatible firmware size/layout:", 34))
            fprintf(stderr, "This firmware layout is not supported by the "
                            "TK-x4x Toolkit.\n");
        fprintf(stderr, "Previously loaded firmware, if any, is unchanged.\n");
        free(decoded);
        return -2;
    }
    if (warnings.bad_record_checksums)
        printf("WARNING: %u Intel HEX record checksum(s) mismatch (first at line %u).\n"
               "Firmware data was loaded as written in the HEX records.\n",
               warnings.bad_record_checksums,warnings.first_bad_record_line);
    if (warnings.firmware_checksum_mismatch)
        printf("WARNING: Kenwood firmware checksum mismatch "
               "(trailer %04X, calculated %04X).\n"
               "Continuing with the image as supplied.\n",
               warnings.trailer_checksum,warnings.calculated_checksum);
    memcpy(st->firmware, decoded, PROGRAM_SIZE);
    free(decoded);
    st->firmware_loaded = 1;
    st->firmware_modified = 0;
    st->firmware_hex_family = family;
    st->patch_model = MODEL_UNKNOWN;
    basename_copy(path, base, sizeof(base));
    copy_string(st->firmware_source, sizeof(st->firmware_source), base);
    if (!st->connected) st->model = MODEL_UNKNOWN;
    classify_loaded_firmware(st);
    printf("Firmware successfully loaded from HEX file: %s\n", base);
    if (family) printf("Kenwood HEX model-family byte: %02X\n", family);
    else printf("No Kenwood HEX checksum/model trailer was present.\n");
    tot_test_10x_offer_on_load(st);
    printf("Checksum: %04X\nSHA-256: %s\n", st->firmware_sum, st->firmware_sha);
    print_firmware_similarity(st->firmware, st->firmware_sha);
    if (!st->firmware_rev)
        print_unknown_firmware_guidance(st->model, 1, st->firmware_sum);
    return 0;
}

/* Single-radio development baseline, kept in the backup directory.
   File format: 8-byte magic, 1-byte model, page-known bitmap,
   0x6600 firmware bytes, 64-byte ASCII SHA-256 of everything preceding it.
   Saved only after a successful operation. It is a working-state cache,
   never a substitute for a real firmware backup. */
#define DEV_BASELINE_FILENAME "development-baseline.dat"
#define DEV_BASELINE_HEADER 9u
#define DEV_BASELINE_CHECKED_SIZE (DEV_BASELINE_HEADER + FLASH_PAGE_COUNT + PROGRAM_SIZE)
#define DEV_BASELINE_RECORD_SIZE (DEV_BASELINE_CHECKED_SIZE + 64u)

static int dev_baseline_path(const struct app_state *st, char *path, size_t cap)
{
    int n = snprintf(path, cap, "%s/%s", st->backup_dir, DEV_BASELINE_FILENAME);
    return n >= 0 && (size_t)n < cap ? 0 : -1;
}

static void dev_forget_persisted_baseline(const struct app_state *st)
{
    char path[MAX_PATH_LEN];
    if (dev_baseline_path(st, path, sizeof(path)) == 0 &&
        remove(path) < 0 && errno != ENOENT)
        fprintf(stderr, "Warning: could not remove saved development baseline: %s\n", path);
}

static void dev_quarantine_persisted_baseline(const struct app_state *st)
{
    char path[MAX_PATH_LEN], old[MAX_PATH_LEN + 16];
    if (dev_baseline_path(st, path, sizeof(path)) < 0 || !file_exists(path)) return;
    if (snprintf(old, sizeof(old), "%s.uncertain", path) >= (int)sizeof(old)) return;
#ifdef _WIN32
    if (!MoveFileExA(path, old, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
#else
    if (rename(path, old) != 0) {
#endif
        /* An outdated baseline must never be silently reloaded after an
           interrupted write, even if renaming the previous copy fails. */
        if (remove(path) < 0)
            fprintf(stderr, "WARNING: unsafe saved development baseline could not be quarantined: %s\n", path);
    } else
        fprintf(stderr, "Previous baseline preserved as %s (radio state uncertain).\n", old);
}

static void dev_save_persisted_baseline(const struct app_state *st)
{
    uint8_t *record;
    char path[MAX_PATH_LEN], tmp[MAX_PATH_LEN + 8], sha[65];
    FILE *f;
    int ok;
    if (dev_baseline_path(st, path, sizeof(path)) < 0) return;
    if (make_dir_if_needed(st->backup_dir) != 0) {
        fprintf(stderr, "Warning: cannot create development baseline directory %s\n", st->backup_dir);
        return;
    }
    record = (uint8_t *)malloc(DEV_BASELINE_RECORD_SIZE);
    if (!record) return;
    memcpy(record, "TK4DEV01", 8);
    record[8] = (uint8_t)st->dev_baseline_model;
    memcpy(record + DEV_BASELINE_HEADER, st->dev_known_pages, FLASH_PAGE_COUNT);
    memcpy(record + DEV_BASELINE_HEADER + FLASH_PAGE_COUNT,
           st->dev_baseline, PROGRAM_SIZE);
    sha256_hex(record, DEV_BASELINE_CHECKED_SIZE, sha);
    memcpy(record + DEV_BASELINE_CHECKED_SIZE, sha, 64);
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
        free(record);
        return;
    }
    f = fopen(tmp, "wb");
    if (!f) { perror(tmp); free(record); return; }
    ok = fwrite(record, 1, DEV_BASELINE_RECORD_SIZE, f) == DEV_BASELINE_RECORD_SIZE;
    if (fclose(f) != 0) ok = 0;
    free(record);
    if (!ok) {
        perror("Saving development baseline");
        remove(tmp);
        return;
    }
#ifdef _WIN32
    ok = MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    ok = rename(tmp, path) == 0;
#endif
    if (!ok) {
        fprintf(stderr, "Warning: could not store persistent development baseline: %s\n", path);
        remove(tmp);
    }
}

static void dev_load_persisted_baseline(struct app_state *st)
{
    uint8_t *record;
    char path[MAX_PATH_LEN], sha[65];
    FILE *f;
    size_t i;
    if (dev_baseline_path(st, path, sizeof(path)) < 0) return;
    f = fopen(path, "rb");
    if (!f) return;
    record = (uint8_t *)malloc(DEV_BASELINE_RECORD_SIZE);
    if (!record) { fclose(f); return; }
    if (fread(record, 1, DEV_BASELINE_RECORD_SIZE, f) != DEV_BASELINE_RECORD_SIZE ||
        fgetc(f) != EOF || memcmp(record, "TK4DEV01", 8) != 0) {
        fprintf(stderr, "Warning: ignoring invalid saved development baseline: %s\n", path);
        free(record); fclose(f); return;
    }
    fclose(f);
    sha256_hex(record, DEV_BASELINE_CHECKED_SIZE, sha);
    if (memcmp(record + DEV_BASELINE_CHECKED_SIZE, sha, 64) != 0) {
        fprintf(stderr, "Warning: ignoring corrupt saved development baseline: %s\n", path);
        free(record); return;
    }
    for (i = 0; i < FLASH_PAGE_COUNT; i++) {
        if (record[DEV_BASELINE_HEADER + i] > 1u) {
            fprintf(stderr, "Warning: invalid saved development page bitmap: %s\n", path);
            free(record); return;
        }
    }
    memcpy(st->dev_known_pages, record + DEV_BASELINE_HEADER, FLASH_PAGE_COUNT);
    memcpy(st->dev_baseline, record + DEV_BASELINE_HEADER + FLASH_PAGE_COUNT, PROGRAM_SIZE);
    st->dev_baseline_model = record[8] <= MODEL_941 ?
        (enum model_family)record[8] : MODEL_UNKNOWN;
    st->dev_baseline_restored = 1;
    st->dev_baseline_sampled = 0;
    copy_string(st->dev_baseline_source, sizeof(st->dev_baseline_source),
                "saved previous-session firmware");
    free(record);
    printf("Saved development baseline loaded (%zu/%u known pages); not yet verified against this radio.\n",
           dev_known_page_count(st), (unsigned)FLASH_PAGE_COUNT);
}

/* At most three 128-byte sample blocks, spread across the known part of
   the installed image. Not a complete verification or radio unique ID. */
static void dev_probe_compare_baseline(struct app_state *st, struct radio_port *p,
                                       const uint8_t first[BLOCK_SIZE],
                                       const uint8_t tail[BLOCK_SIZE])
{
    size_t candidates[PROGRAM_BLOCK_COUNT], n = 0, block, i;
    size_t checked = 0;
    int mismatch = 0;
    uint8_t buf[BLOCK_SIZE];
    if (!dev_known_page_count(st)) return;
    if (st->dev_baseline_model != MODEL_UNKNOWN && st->model != MODEL_UNKNOWN &&
        st->dev_baseline_model != st->model) {
        printf("WARNING: development baseline belongs to %s, not %s; baseline ignored.\n",
               model_name(st->dev_baseline_model), model_name(st->model));
        dev_clear_baseline(st);
        return;
    }
    for (block = 0; block < PROGRAM_BLOCK_COUNT; block++)
        if (st->dev_known_pages[block * 2] || st->dev_known_pages[block * 2 + 1])
            candidates[n++] = block;
    for (i = 0; i < 3 && n; i++) {
        size_t selected = candidates[(i * (n - 1)) / 2];
        size_t j;
        const uint8_t *bytes;
        if (i && selected == candidates[((i - 1) * (n - 1)) / 2]) continue;
        if (selected == 0) bytes = first;
        else if (PROGRAM_START + selected * BLOCK_SIZE == 0xE580u) bytes = tail;
        else if (read_block(p, (uint16_t)(PROGRAM_START + selected * BLOCK_SIZE), buf) == 0)
            bytes = buf;
        else {
            printf("Could not sample development baseline: retaining it as unverified.\n");
            st->dev_baseline_sampled = 0;
            return;
        }
        for (j = 0; j < 2; j++) {
            size_t page = selected * 2 + j;
            if (!st->dev_known_pages[page]) continue;
            checked++;
            if (memcmp(bytes + j * FLASH_PAGE_SIZE,
                       st->dev_baseline + page * FLASH_PAGE_SIZE,
                       FLASH_PAGE_SIZE) != 0) mismatch = 1;
        }
    }
    if (mismatch) {
        printf("WARNING: installed firmware differs from saved development baseline.\n");
        printf("Baseline disabled in memory; saved file retained for investigation.\n");
        dev_clear_baseline(st);
    } else if (checked) {
        st->dev_baseline_sampled = 1;
        printf("Development baseline: %zu flash page sample(s) match (NOT full verification).\n",
               checked);
    }
}

static size_t dev_known_page_count(const struct app_state *st)
{
    size_t i, n = 0;
    for (i = 0; i < FLASH_PAGE_COUNT; i++)
        if (st->dev_known_pages[i]) n++;
    return n;
}

static void dev_clear_baseline(struct app_state *st)
{
    memset(st->dev_known_pages, 0, sizeof(st->dev_known_pages));
    memset(st->dev_baseline, 0xFF, sizeof(st->dev_baseline));
    st->dev_baseline_source[0] = '\0';
    st->dev_baseline_model = MODEL_UNKNOWN;
    st->dev_baseline_restored = 0;
    st->dev_baseline_sampled = 0;
}

static void dev_set_full_baseline(struct app_state *st, const uint8_t *image,
                                  const char *source)
{
    memcpy(st->dev_baseline, image, PROGRAM_SIZE);
    memset(st->dev_known_pages, 1, sizeof(st->dev_known_pages));
    copy_string(st->dev_baseline_source, sizeof(st->dev_baseline_source),
                source && *source ? source : "known firmware");
    st->dev_baseline_model = st->model;
    st->dev_baseline_restored = 0;
    st->dev_baseline_sampled = 0;
    dev_save_persisted_baseline(st);
}

static int dev_load_baseline_file(struct app_state *st, const char *path)
{
    uint8_t *d = NULL;
    size_t n = 0;
    char base[MAX_SOURCE_LEN];
    if (read_file_sized(path, &d, &n) < 0) return -1;
    if (n != PROGRAM_SIZE && n != FULL_SIZE) {
        fprintf(stderr, "The development baseline has an invalid size.\n");
        free(d);
        return -1;
    }
    if (memcmp(d + FW_KENWOOD_OFFSET, FW_KENWOOD_TEXT, FW_KENWOOD_LEN)) {
        fprintf(stderr, "The development baseline does not appear to be a valid TK-x4x firmware image.\n");
        free(d);
        return -1;
    }
    basename_copy(path, base, sizeof(base));
    dev_set_full_baseline(st, d, base);
    free(d);
    printf("Development baseline set from %s. All %u firmware pages are known.\n",
           base, (unsigned)FLASH_PAGE_COUNT);
    return 0;
}



static int load_channels_file(struct app_state *st, const char *path)
{
    uint8_t *d;
    size_t n;
    char base[MAX_SOURCE_LEN];
    if (read_file_sized(path, &d, &n) < 0) return -1;
    if (n != CHANNEL_SIZE && n != FULL_SIZE) {
        fprintf(stderr, "The channel file has an invalid size.\n"
                        "It does not appear to be valid TK-x4x channel data.\n");
        free(d);
        return -2;
    }
    memcpy(st->channels, d + (n == FULL_SIZE ? PROGRAM_SIZE : 0), CHANNEL_SIZE);
    st->channels_model = infer_channel_model(st->channels);
    if (st->channels_model == MODEL_UNKNOWN && n == FULL_SIZE) {
        char sha[65];
        const struct firmware_revision *rev = identify_firmware_image(d, sha);
        /* Intentional fallback: when channel-frequency inference is ambiguous,
           the firmware contained in the same full backup is a useful decoding
           hint.  It may cause a conservative/spurious mismatch warning, but
           channel writes remain allowed after confirmation. */
        if (rev) st->channels_model = rev->model;
    }
    free(d);
    st->channels_loaded = 1;
    basename_copy(path, base, sizeof(base));
    copy_string(st->channels_source, sizeof(st->channels_source), base);
    update_preview_from_full_channels(st);
    printf("Channels successfully loaded from %s.\n", base);
    return 0;
}



enum browse_kind {
    BROWSE_FIRMWARE = 0,
    BROWSE_HEX,
    BROWSE_CHANNELS,
    BROWSE_FULL
};

struct browse_item {
    char name[MAX_PATH_LEN];
    int is_dir;
    size_t size;
};

static int ascii_casecmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = tolower((unsigned char)*a++);
        int cb = tolower((unsigned char)*b++);
        if (ca != cb) return ca - cb;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int browse_item_cmp(const void *va, const void *vb)
{
    const struct browse_item *a = (const struct browse_item *)va;
    const struct browse_item *b = (const struct browse_item *)vb;
    if (a->is_dir != b->is_dir) return b->is_dir - a->is_dir;
    return ascii_casecmp(a->name, b->name);
}

static int size_allowed(enum browse_kind kind, size_t size)
{
    switch (kind) {
    case BROWSE_FIRMWARE: return size == PROGRAM_SIZE || size == FULL_SIZE;
    case BROWSE_HEX: return 1; /* extension filtering is separate */
    case BROWSE_CHANNELS: return size == CHANNEL_SIZE || size == FULL_SIZE;
    case BROWSE_FULL: return size == FULL_SIZE;
    default: return 0;
    }
}

static int browse_file_allowed(enum browse_kind kind, const char *name,
                               size_t size)
{
    if (kind == BROWSE_HEX) {
        const char *ext = strrchr(name, '.');
        return ext && ascii_casecmp(ext, ".hex") == 0;
    }
    return size_allowed(kind, size);
}

/* A process can outlive its working directory (for example, when a user
   removes a build directory while the Toolkit is open). "." is not a
   usable fallback in that case: always start the browser on an absolute
   path that can be climbed toward a valid parent. */
static void get_current_dir(char out[MAX_PATH_LEN])
{
#ifdef _WIN32
    const char *home, *drive;
    if (_getcwd(out, MAX_PATH_LEN)) return;
    home = getenv("USERPROFILE");
    if (home && home[0] && strlen(home) < MAX_PATH_LEN) {
        copy_string(out, MAX_PATH_LEN, home);
        return;
    }
    drive = getenv("SystemDrive");
    if (drive && strlen(drive) == 2 && drive[1] == ':') {
        snprintf(out, MAX_PATH_LEN, "%s%c", drive, PATH_SEP);
        return;
    }
    copy_string(out, MAX_PATH_LEN, "C:\\");
#else
    const char *home;
    if (getcwd(out, MAX_PATH_LEN)) return;
    home = getenv("HOME");
    if (home && home[0] == '/' && strlen(home) < MAX_PATH_LEN) {
        copy_string(out, MAX_PATH_LEN, home);
        return;
    }
    copy_string(out, MAX_PATH_LEN, "/");
#endif
}

static int path_join(char out[MAX_PATH_LEN], const char *dir, const char *name)
{
    size_t dl = strlen(dir), nl = strlen(name);
    int need_sep = (dl > 0 && dir[dl - 1] != '/' && dir[dl - 1] != '\\');
    if (dl + (need_sep ? 1u : 0u) + nl + 1u > MAX_PATH_LEN) return -1;
    memcpy(out, dir, dl);
    if (need_sep) out[dl++] = PATH_SEP;
    memcpy(out + dl, name, nl + 1);
    return 0;
}

static void path_parent(char path[MAX_PATH_LEN])
{
    size_t n = strlen(path);
    if (!n) return;
    while (n > 1 && (path[n - 1] == '/' || path[n - 1] == '\\')) path[--n] = '\0';
#ifdef _WIN32
    if (n == 3 && isalpha((unsigned char)path[0]) && path[1] == ':' &&
        (path[2] == '/' || path[2] == '\\')) return;
#endif
    while (n > 0 && path[n - 1] != '/' && path[n - 1] != '\\') n--;
    if (n == 0) {
        copy_string(path, MAX_PATH_LEN, ".");
        return;
    }
#ifdef _WIN32
    if (n == 3 && isalpha((unsigned char)path[0]) && path[1] == ':') {
        path[3] = '\0';
        return;
    }
#endif
    if (n == 1) path[1] = '\0';
    else path[n - 1] = '\0';
}

/* The browser deliberately has no fixed entry limit. Read the whole directory
   before sorting so large directories do not silently omit arbitrary entries. */
static int browse_item_append(struct browse_item **items, size_t *count,
                              size_t *capacity, const char *name,
                              int is_dir, size_t size)
{
    struct browse_item *grown;
    if (*count == *capacity) {
        size_t next = *capacity ? *capacity * 2u : 64u;
        if (next < *capacity || next > SIZE_MAX / sizeof(**items)) {
            errno = ENOMEM;
            return -1;
        }
        grown = (struct browse_item *)realloc(*items, next * sizeof(**items));
        if (!grown) return -1;
        *items = grown;
        *capacity = next;
    }
    copy_string((*items)[*count].name, sizeof((*items)[*count].name), name);
    (*items)[*count].is_dir = is_dir;
    (*items)[*count].size = size;
    (*count)++;
    return 0;
}

static int list_browse_items(const char *dir, enum browse_kind kind,
                             struct browse_item **out_items, size_t *out_count)
{
    struct browse_item *items = NULL;    size_t n = 0, capacity = 0;
    int rc = 0;
    *out_items = NULL;
    *out_count = 0;
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char pattern[MAX_PATH_LEN];
    if (path_join(pattern, dir, "*") < 0) return -1;
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        ULARGE_INTEGER sz;
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (browse_item_append(&items, &n, &capacity, fd.cFileName, 1, 0) < 0) {
                rc = -1;
                break;
            }
        } else {
            sz.HighPart = fd.nFileSizeHigh;
            sz.LowPart = fd.nFileSizeLow;
            if (sz.QuadPart <= (ULONGLONG)SIZE_MAX && browse_file_allowed(kind, fd.cFileName, (size_t)sz.QuadPart)) {
                if (browse_item_append(&items, &n, &capacity, fd.cFileName, 0,
                                       (size_t)sz.QuadPart) < 0) {
                    rc = -1;
                    break;
                }
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    struct dirent *de;
    if (!d) return -1;
    while ((de = readdir(d)) != NULL) {
        char full[MAX_PATH_LEN];
        struct stat stbuf;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (path_join(full, dir, de->d_name) < 0) continue;
        if (stat(full, &stbuf) != 0) continue;
        if (S_ISDIR(stbuf.st_mode)) {
            if (browse_item_append(&items, &n, &capacity, de->d_name, 1, 0) < 0) {
                rc = -1;
                break;
            }
        } else if (S_ISREG(stbuf.st_mode) && stbuf.st_size >= 0 &&
                   browse_file_allowed(kind, de->d_name, (size_t)stbuf.st_size)) {
            if (browse_item_append(&items, &n, &capacity, de->d_name, 0,
                                   (size_t)stbuf.st_size) < 0) {
                rc = -1;
                break;
            }
        }
    }
    closedir(d);
#endif
    if (rc < 0) {
        free(items);
        return -1;
    }
    if (n > 1) qsort(items, n, sizeof(items[0]), browse_item_cmp);
    *out_items = items;
    *out_count = n;
    return 0;
}

/* If a directory disappeared after it was selected, walk upward until an
   accessible directory is found. A missing directory must never trap the
   user outside the file selector. Also covers a deleted process CWD. */
static int list_browse_items_recover(char dir[MAX_PATH_LEN],
                                     enum browse_kind kind,
                                     struct browse_item **out_items,
                                     size_t *out_count)
{
    char requested[MAX_PATH_LEN];
    int used_fallback = 0;
    copy_string(requested, sizeof(requested), dir);

    for (;;) {
        char parent[MAX_PATH_LEN];
        int saved_errno;
        if (list_browse_items(dir, kind, out_items, out_count) == 0) {
            if (strcmp(requested, dir) != 0)
                printf("Directory no longer accessible: %s\n"
                       "Continuing in: %s\n", requested, dir);
            return 0;
        }
        saved_errno = errno;
        if (saved_errno == ENOMEM) break;

        copy_string(parent, sizeof(parent), dir);
        path_parent(parent);
        if (strcmp(parent, dir) != 0 && strcmp(parent, ".") != 0) {
            copy_string(dir, MAX_PATH_LEN, parent);
            continue;
        }

        if (!used_fallback) {
            used_fallback = 1;
            get_current_dir(parent);
            if (strcmp(parent, dir) != 0) {
                copy_string(dir, MAX_PATH_LEN, parent);
                continue;
            }
        }
        break;
    }
    fprintf(stderr, "Unable to open a usable file-browser directory: %s\n",
            requested);
    return -1;
}

static const char *browse_kind_name(enum browse_kind kind)
{
    switch (kind) {
    case BROWSE_FIRMWARE: return "firmware or backup";
    case BROWSE_HEX: return "Intel HEX";
    case BROWSE_CHANNELS: return "channel data or backup";
    case BROWSE_FULL: return "complete backup";
    default: return "compatible";
    }
}

struct browse_screen_ctx {
    const char *title;
    const char *dir;
    enum browse_kind kind;
};

static void print_browser_line(const char *text)
{
    int cols = 80, rows = 24;
    (void)rows;
    get_terminal_size(&cols, &rows);
    if (cols < 2) cols = 2;
    printf("%.*s\n", cols - 1, text ? text : "");
}

static void draw_browse_screen(void *vctx)
{
    const struct browse_screen_ctx *ctx = (const struct browse_screen_ctx *)vctx;
    char line[MAX_PATH_LEN + 32];
    print_browser_line(ctx->title);
    snprintf(line, sizeof(line), "Directory: %s", ctx->dir);
    print_browser_line(line);
    if (ctx->kind == BROWSE_HEX)
        snprintf(line, sizeof(line), "Only .hex files are shown (case-insensitive).");
    else
        snprintf(line, sizeof(line), "Only %s files with the correct size are shown.",
                 browse_kind_name(ctx->kind));
    print_browser_line(line);
    fputc('\n', stdout);
}

static void browse_choice_text(const struct browse_item *items, size_t n,
                               size_t index, char *out, size_t cap)
{
    if (index == 0) {
        snprintf(out, cap, "[P] [..] Parent directory");
    } else if (index <= n) {
        const struct browse_item *item = &items[index - 1];
        if (item->is_dir)
            snprintf(out, cap, "%s  [DIR]", item->name);
        else
            snprintf(out, cap, "%s  (0x%zX bytes)", item->name, item->size);
    } else {
        snprintf(out, cap, "[B] Back  (Esc/Q/E)");
    }
}

/* Keep the scrolling file list at one line per entry, but show the
   selected filename in full underneath it. Firmware images often have long
   descriptive names, and clipping them at column 77 hides distinctions.
   Four rows can display a normal maximum-length (255-byte) filename at
   the supported 80-column terminal width. */
#define BROWSE_SELECTED_LINES 4

static void browse_print_selected(const struct browse_item *items, size_t n,
                                  size_t selected, int cols)
{
    char full[MAX_PATH_LEN + 64];
    const char *p;
    size_t remaining, row;
    browse_choice_text(items, n, selected, full, sizeof(full));
    p = full;
    remaining = strlen(full);
    for (row = 0; row < BROWSE_SELECTED_LINES; row++) {
        const char *prefix = row == 0 ? "Selected: " : "  ";
        size_t prefix_len = strlen(prefix);
        size_t width = cols > 1 ? (size_t)(cols - 1) : 1u;
        size_t take = width > prefix_len ? width - prefix_len : 0u;
        if (take > remaining) take = remaining;
        if (ui_cursor_control()) ui_clear_line();
        fputs(prefix, stdout);
        if (take) fwrite(p, 1, take, stdout);
        p += take;
        remaining -= take;
        fputc('\n', stdout);
    }
}

static size_t browse_visible_rows(size_t count)
{
    int cols = 80, rows = 24;
    size_t visible;
    (void)cols;
    get_terminal_size(&cols, &rows);
    /* Four header rows, four selected-name rows, one range/status row,
       and a spare terminal row to prevent scrolling at 80x24. */
    visible = rows > 6 + BROWSE_SELECTED_LINES
        ? (size_t)(rows - 6 - BROWSE_SELECTED_LINES) : 1u;
    if (visible > count) visible = count;
    if (!visible) visible = 1;
    return visible;
}

static void redraw_browse_menu(const struct browse_item *items, size_t n,
                               size_t sel, size_t *top, int initial,
                               size_t *drawn_lines)
{
    const size_t count = n + 2;
    size_t visible = browse_visible_rows(count);
    size_t i, last;
    int cols = 80, rows = 24;
    char text[MAX_PATH_LEN + 64];
    get_terminal_size(&cols, &rows);
    (void)rows;
    if (cols < 4) cols = 4;

    if (sel < *top) *top = sel;
    if (sel >= *top + visible) *top = sel - visible + 1;
    if (*top + visible > count) *top = count - visible;

    if (ui_cursor_control()) {
        if (!initial && *drawn_lines > 1)
            ui_cursor_up(*drawn_lines - 1);
        for (i = 0; i < visible; i++) {
            size_t index = *top + i;
            browse_choice_text(items, n, index, text, sizeof(text));
            ui_clear_line();
            if (index == sel) ui_inverse(1);
            printf("%c %-*.*s", index == sel ? '>' : ' ', cols - 3, cols - 3, text);
            if (index == sel) ui_inverse(0);
            fputc('\n', stdout);
        }
        browse_print_selected(items, n, sel, cols);
        ui_clear_line();
        last = *top + visible;
        snprintf(text, sizeof(text), "Showing %zu-%zu of %zu",
                 *top + 1, last, count);
        printf("%.*s", cols - 1, text);
        *drawn_lines = visible + BROWSE_SELECTED_LINES + 1;
    } else {
        if (!initial) fputc('\n', stdout);
        for (i = 0; i < visible; i++) {
            size_t index = *top + i;
            browse_choice_text(items, n, index, text, sizeof(text));
            printf("%c %.*s\n", index == sel ? '>' : ' ', cols - 3, text);
        }
        browse_print_selected(items, n, sel, cols);
        last = *top + visible;
        printf("Showing %zu-%zu of %zu\n", *top + 1, last, count);
        *drawn_lines = 0;
    }
}

/* File lists can be much taller than the terminal, unlike the toolkit's normal
   menus. Keep them in a bounded scrolling viewport so cursor-up redraws never
   depend on off-screen lines that have already scrolled out of the terminal. */
static int browse_menu(const struct browse_item *items, size_t n, size_t *selected,
                       void (*draw)(void *ctx), void *ctx)
{
    const size_t count = n + 2;
    size_t sel = selected ? *selected : 0;
    size_t top = 0, drawn_lines = 0;
    int initial = 1;
    if (sel >= count) sel = 0;
    for (;;) {
        char history[MAX_PATH_LEN + 64];
        int k;
        if (initial && draw) draw(ctx);
        redraw_browse_menu(items, n, sel, &top, initial, &drawn_lines);
        fflush(stdout);
        k = ui_read_key();
        if (k == UI_KEY_EOF) return UI_KEY_EOF;
        if (k == UI_KEY_ESCAPE) return 'b';
        if (k == UI_KEY_UP) {
            sel = sel == 0 ? count - 1 : sel - 1;
            initial = 0;
            continue;
        }
        if (k == UI_KEY_DOWN) {
            sel = sel + 1 >= count ? 0 : sel + 1;
            initial = 0;
            continue;
        }
        if (tolower((unsigned char)k) == 'x') {
            ui_force_plain();
            initial = 1;
            drawn_lines = 0;
            continue;
        }
        if (k > 0 && k < 256) {
            int pressed = tolower((unsigned char)k);
            size_t direct = count;
            if (pressed == 'p') direct = 0;
            else if (pressed == 'b' || pressed == 'q' || pressed == 'e') direct = n + 1;
            if (direct < count) {
                sel = direct;
                if (selected) *selected = sel;
                redraw_browse_menu(items, n, sel, &top, 0, &drawn_lines);
                fputc('\n', stdout);
                browse_choice_text(items, n, sel, history, sizeof(history));
                printf("Selected: %s\n", history);
                return direct == n + 1 ? 'b' : pressed;
            }
        }
        if (k == UI_KEY_ENTER || k == UI_KEY_SPACE) {
            if (selected) *selected = sel;
            fputc('\n', stdout);
            browse_choice_text(items, n, sel, history, sizeof(history));
            printf("Selected: %s\n", history);
            if (sel == 0) return 'p';
            if (sel == n + 1) return 'b';
            return UI_KEY_SELECTED;
        }
        initial = 0;
    }
}

static int browse_for_file(struct app_state *st, enum browse_kind kind,
                           const char *title, char out[MAX_PATH_LEN])
{
    char dir[MAX_PATH_LEN];
    size_t selected = 0;
    if (st->file_dir[0]) copy_string(dir, sizeof(dir), st->file_dir);
    else get_current_dir(dir);

    for (;;) {
        struct browse_item *items = NULL;
        struct browse_screen_ctx ctx;
        size_t n = 0;
        int choice;

        if (list_browse_items_recover(dir, kind, &items, &n) < 0)
            return -1;
        copy_string(st->file_dir, sizeof(st->file_dir), dir);
        if (selected >= n + 2) selected = 0;

        ctx.title = title;
        ctx.dir = dir;
        ctx.kind = kind;
        choice = browse_menu(items, n, &selected, draw_browse_screen, &ctx);
        if (choice == UI_KEY_EOF) {
            free(items);
            return -1;
        }
        if (choice == 'b' || choice == UI_KEY_ESCAPE) {
            free(items);
            return 1;
        }
        if (choice == 'p') {
            free(items);
            path_parent(dir);
            copy_string(st->file_dir, sizeof(st->file_dir), dir);
            selected = 0;
            continue;
        }
        if (choice != UI_KEY_SELECTED) {
            free(items);
            continue;
        }

        if (items[selected - 1].is_dir) {
            char next[MAX_PATH_LEN];
            if (path_join(next, dir, items[selected - 1].name) < 0) {
                printf("Path is too long.\n");
                free(items);
                continue;
            }
            copy_string(dir, sizeof(dir), next);
            copy_string(st->file_dir, sizeof(st->file_dir), dir);
            selected = 0;
            free(items);
            continue;
        }
        if (path_join(out, dir, items[selected - 1].name) < 0) {
            free(items);
            return -1;
        }
        copy_string(st->file_dir, sizeof(st->file_dir), dir);
        free(items);
        return 0;
    }
}

static int action_set_dev_baseline(struct app_state *st)
{
    char path[MAX_PATH_LEN];
    if (browse_for_file(st, BROWSE_FIRMWARE,
                        "Select firmware currently installed in the radio", path) != 0)
        return 1;
    return dev_load_baseline_file(st, path);
}

static void show_matching_files(const struct app_state *st, enum browse_kind kind)
{
    struct browse_item *items = NULL;
    size_t n = 0, i, shown = 0;
    const char *dir = st->file_dir[0] ? st->file_dir : ".";
    printf("\nSave directory: %s\n", dir);
    if (list_browse_items(dir, kind, &items, &n) < 0) {
        printf("Unable to list existing compatible files in this directory.\n");
        return;
    }
    for (i = 0; i < n; i++) {
        if (items[i].is_dir) continue;
        if (!shown) printf("Existing compatible files:\n");
        printf("  %s\n", items[i].name);
        shown++;
    }
    if (!shown) printf("No existing compatible files in this directory.\n");
    free(items);
}

static int make_save_path(struct app_state *st, enum browse_kind kind,
                          const char *label, const char *def,
                          char out[MAX_PATH_LEN])
{
    char name[MAX_PATH_LEN];
    show_matching_files(st, kind);
    prompt_string(label, def, name, sizeof(name));
    if (!name[0]) return 1;
#ifdef _WIN32
    if ((isalpha((unsigned char)name[0]) && name[1] == ':') ||
        name[0] == '/' || name[0] == '\\') {
#else
    if (name[0] == '/') {
#endif
        copy_string(out, MAX_PATH_LEN, name);
    } else if (strchr(name, '/') || strchr(name, '\\')) {
        copy_string(out, MAX_PATH_LEN, name);
    } else {
        const char *dir = st->file_dir[0] ? st->file_dir : ".";
        if (path_join(out, dir, name) < 0) return -1;
    }
    return 0;
}
/* ------------------------ Firmware loader protocol ----------------------- */

static char hex_digit(unsigned v){return "0123456789ABCDEF"[v&15];}
static size_t make_hex_record(char*out,size_t cap,uint16_t rel,const uint8_t*data,uint8_t len)
{
    uint8_t sum=0,ck;size_t pos=0;unsigned i;
#define PUTHEX(v) do{uint8_t _x=(uint8_t)(v);if(pos+2>=cap)return 0;out[pos++]=hex_digit(_x>>4);out[pos++]=hex_digit(_x);}while(0)
    out[pos++]=':';PUTHEX(len);sum=(uint8_t)(sum+len);PUTHEX(rel>>8);sum=(uint8_t)(sum+(rel>>8));PUTHEX(rel);sum=(uint8_t)(sum+rel);PUTHEX(0x00);for(i=0;i<len;i++){PUTHEX(data[i]);sum=(uint8_t)(sum+data[i]);}ck=(uint8_t)(0u-sum);PUTHEX(ck);out[pos++]='\r';out[pos++]='\n';out[pos]='\0';return pos;
#undef PUTHEX
}
#ifdef __APPLE__
/*
 * Some macOS USB-serial drivers can report the host-side queue drained before
 * the USB-UART has shifted every character onto the wire.  The firmware loader
 * needs the same inter-page pacing that tcdrain() effectively gives us on
 * Linux.  At 9600 baud, 8N2 is 11 bits per character, so enforce the physical
 * minimum wire time on macOS without adding delay when tcdrain() already waited.
 */
static void macos_ensure_wire_time(uint64_t started_ms,size_t chars,unsigned baud)
{
    uint64_t need_ms,now_ms,elapsed_ms;
    if(!baud)return;
    need_ms=((uint64_t)chars*11u*1000u+baud-1u)/baud;
    now_ms=monotonic_ms();
    if(!started_ms||!now_ms){sleep_ms((unsigned)need_ms);return;}
    elapsed_ms=now_ms-started_ms;
    if(elapsed_ms<need_ms)sleep_ms((unsigned)(need_ms-elapsed_ms));
}
#endif

static int send_firmware_page(struct radio_port*p,uint16_t rel,const uint8_t data[FLASH_PAGE_SIZE])
{
    unsigned n;
#ifdef __APPLE__
    uint64_t started_ms=monotonic_ms();
    size_t chars=0;
#endif
    for(n=0;n<FLASH_PAGE_SIZE;n+=HEX_DATA_SIZE){
        char rec[80];
        size_t l=make_hex_record(rec,sizeof(rec),(uint16_t)(rel+n),data+n,HEX_DATA_SIZE);
        if(!l||port_write_all(p,rec,l)<0)return -1;
#ifdef __APPLE__
        chars+=l;
#endif
    }
    if(port_drain(p)<0)return -1;
#ifdef __APPLE__
    macos_ensure_wire_time(started_ms,chars,9600u);
#endif
    sleep_ms(20);
    return 0;
}

static int firmware_stream(struct radio_port *p, const uint8_t *target,
                           const uint8_t *changed, size_t changed_pages)
{
    size_t page, total = PROGRAM_SIZE / FLASH_PAGE_SIZE, done = 0;
    uint16_t sum = byte_sum16(target, PROGRAM_SIZE);
    if (port_set_speed(p, 9600) < 0) return -1;
    port_flush(p);
    sleep_ms(100);
    if (port_write_all(p, "\r", 1) < 0 || port_drain(p) < 0) return -1;
    sleep_ms(20);

    if (changed_pages == total)
        printf("Firmware transfer: all %zu pages will be sent (%zu total pages).\n",
               changed_pages, total);
    else
        printf("Firmware transfer: %zu changed page(s) will be sent out of %zu total pages.\n",
               changed_pages, total);

    for (page = 0; page < total; page++) {
        uint16_t rel;
        unsigned a;
        if (!changed[page]) continue;
        rel = (uint16_t)(page * FLASH_PAGE_SIZE);
        a = PROGRAM_START + rel;
        done++;
        progress_line("Sending", "firmware", done, changed_pages, total,
                      a, a + FLASH_PAGE_SIZE - 1);
        if (send_firmware_page(p, rel, target + page * FLASH_PAGE_SIZE) < 0) {
            progress_done();
            return -1;
        }
    }
    progress_done();
    printf("Finishing firmware transfer...\n");
    fflush(stdout);
#ifdef __APPLE__
    {
        uint64_t eof_started_ms=monotonic_ms();
        if(port_write_all(p,":00\r\n",5)<0||port_drain(p)<0){
            perror("Unable to finish firmware transfer");
            return -1;
        }
        macos_ensure_wire_time(eof_started_ms,5u,9600u);
        /* Let the USB-UART and loader consume EOF before close(). */
        sleep_ms(100);
    }
#else
    if (port_write_all(p, ":00\r\n", 5) < 0 || port_drain(p) < 0) {
        perror("Unable to finish firmware transfer");
        return -1;
    }
    sleep_ms(100);
#endif
    printf("Firmware data sent.\n"
           "The radio should display checksum: %04X\n", sum);
    return 0;
}


static int firmware_stream_sparse(struct radio_port *p,const uint8_t *target,
                                  const uint8_t changed[FLASH_PAGE_COUNT],
                                  size_t changed_pages,
                                  int expected_checksum_known,
                                  uint16_t expected_checksum)
{
    size_t page,done=0;
    if(port_set_speed(p,9600)<0)return -1;
    port_flush(p);
    sleep_ms(100);
    if(port_write_all(p,"\r",1)<0 || port_drain(p)<0)return -1;
    sleep_ms(20);

    printf("Firmware transfer: %zu changed page(s) will be sent out of %u total pages.\n",
           changed_pages,(unsigned)FLASH_PAGE_COUNT);
    for(page=0;page<FLASH_PAGE_COUNT;page++){
        uint16_t rel;
        unsigned a;
        if(!changed[page])continue;
        rel=(uint16_t)(page*FLASH_PAGE_SIZE);
        a=PROGRAM_START+rel;
        done++;
        progress_line("Sending","firmware",done,changed_pages,FLASH_PAGE_COUNT,
                      a,a+FLASH_PAGE_SIZE-1);
        if(send_firmware_page(p,rel,target+page*FLASH_PAGE_SIZE)<0){
            progress_done();
            return -1;
        }
    }
    progress_done();
    printf("Finishing firmware transfer...\n");
    fflush(stdout);
#ifdef __APPLE__
    {
        uint64_t eof_started_ms=monotonic_ms();
        if(port_write_all(p,":00\r\n",5)<0||port_drain(p)<0){
            perror("Unable to finish firmware transfer");
            return -1;
        }
        macos_ensure_wire_time(eof_started_ms,5u,9600u);
        sleep_ms(100);
    }
#else
    if(port_write_all(p,":00\r\n",5)<0||port_drain(p)<0){
            perror("Unable to finish firmware transfer");
            return -1;
        }
    sleep_ms(100);
#endif
    if(expected_checksum_known)
        printf("Firmware data sent.\nExpected checksum: %04X\n",expected_checksum);
    else
        printf("Firmware data sent. The radio should display a 4-digit checksum.\n");
    return 0;
}

static int force_recovery_in_session(struct radio_port *p)
{
    uint8_t b[BLOCK_SIZE], t[BLOCK_SIZE];
    int was_enabled;

    if (read_block(p, CHANNEL_START, b) < 0) return -1;
    was_enabled = !!(b[RECOVERY_OFFSET] & RECOVERY_MASK);
    if (was_enabled) return 0;

    printf("\nFirmware loader access was disabled when this radio was read.\n");
    printf("The loader-enable bit must be set before firmware programming.\n");
    printf("This changes one bit in channel data and will remain enabled afterward.\n");

    memcpy(t, b, sizeof(t));
    t[RECOVERY_OFFSET] |= RECOVERY_MASK;
    if (write_block(p, CHANNEL_START, t) < 0 || verify_block(p, CHANNEL_START, t) < 0) return -1;
    printf("Firmware loader access has been enabled and verified.\n");
    return 0;
}

static int prepare_firmware_in_session(struct radio_port *p,
                                       const uint8_t *current,
                                       const uint8_t *target,
                                       uint8_t *changed,
                                       size_t *changed_pages)
{
    size_t page, pages = PROGRAM_SIZE / FLASH_PAGE_SIZE;
    *changed_pages = 0;
    memset(changed, 0, pages);
    for (page = 0; page < pages; page++) {
        if (memcmp(current + page * FLASH_PAGE_SIZE,
                   target + page * FLASH_PAGE_SIZE,
                   FLASH_PAGE_SIZE) != 0) {
            changed[page] = 1;
            (*changed_pages)++;
        }
    }
    if (!*changed_pages) return 0;
    if (force_recovery_in_session(p) < 0) return -1;
    return 0;
}

static int confirm_firmware_program_start(void)
{
    if (g_cli_noninteractive) {
        if (!g_cli_confirm_write) {
            cli_mark_needs_interaction("firmware write requires --confirm-write in non-interactive mode");
            return 1;
        }
        printf("\nNon-interactive firmware write authorized by --confirm-write.\n");
        return 0;
    }
    for (;;) {
        int k;
        printf("\nTurn radio off. Hold SYS-UP, hit power, and release SYS-UP.\n");
        printf("In a few seconds, it will flash \"PROGRAM\".\n");
        ui_flush_input();
        printf("Write firmware? n/Y: ");
        fflush(stdout);
        k = ui_read_key();
        if (k == UI_KEY_ENTER || k == UI_KEY_SPACE || tolower((unsigned char)k) == 'y') {
            printf("\nSelected: Write firmware\n");
            return 0;
        }
        if (tolower((unsigned char)k) == 'n' || k == UI_KEY_ESCAPE) {
            printf("\nSelected: Abort firmware write\nFirmware write aborted.\n");
            return 1;
        }
        if (tolower((unsigned char)k) == 'x') {
            ui_force_plain();
            continue;
        }
    }
}

static int verify_firmware_readback(struct app_state *st, const uint8_t *target)
{
    struct radio_port p;
    char ident[8];
    uint8_t *verify = (uint8_t *)malloc(PROGRAM_SIZE);

    if (!verify) return -1;

    for (;;) {
        int k;
        ui_flush_input();
        if (wait_enter("Power-cycle the radio normally, then press Enter: ") < 0) {
            free(verify);
            return 1;
        }

        if (port_open(&p, st->port_name) < 0) {
            report_port_open_error(st->port_name);
            /* The combined retry prompt below handles this case. */
        } else if (enter_program_mode(&p, ident) < 0) {
            port_close(&p);
        } else if (read_region(&p, PROGRAM_START, PROGRAM_SIZE, verify, "verify") < 0) {
            exit_program_mode(&p);
            port_close(&p);
            fprintf(stderr, "Firmware verification read failed.\n");
            ui_flush_input();
            printf("Retry verification? n/Y: ");
            fflush(stdout);
            {
                char line[16];
                if (!fgets(line, sizeof(line), stdin) ||
                    (line[0] && line[0] != '\n' && line[0] != '\r' &&
                     line[0] != 'y' && line[0] != 'Y')) {
                    printf("Verification aborted.\n");
                    free(verify);
                    return 1;
                }
            }
            continue;
        } else {
            exit_program_mode(&p);
            port_close(&p);
            if (memcmp(verify, target, PROGRAM_SIZE)) {
                free(verify);
                return 2;
            }
            printf("Firmware verified successfully. Every byte matches. Checksum: %04X\n",
                   byte_sum16(verify, PROGRAM_SIZE));
            free(verify);
            return 0;
        }

        ui_flush_input();
        printf("Unable to re-enter normal programming mode for verification.\n");
        printf("Power-cycle the radio normally, then press Enter to retry verification,\n");
        printf("or A to abort: ");
        fflush(stdout);
        k = ui_read_key();
        if (k == UI_KEY_ENTER || k == UI_KEY_SPACE) continue;
        if (tolower((unsigned char)k) == 'a') {
            printf("\nSelected: Abort verification\n");
            free(verify);
            return 1;
        }
        if (tolower((unsigned char)k) == 'x') {
            ui_force_plain();
            continue;
        }
    }
}

static int open_firmware_loader_port_retry(struct app_state *st,struct radio_port *p)
{
    for(;;){
        if(port_open(p,st->port_name)==0)return 0;
        {
            int e=errno;
            report_port_open_error(st->port_name);
            if(e==EBUSY && busy_port_retry_prompt(st->port_name))continue;
        }
        invalidate_radio_connection(st);
        return -1;
    }
}

static int stream_prepared_firmware(struct app_state *st,
                                    const uint8_t *target,
                                    uint8_t *changed,
                                    size_t changed_pages)
{
    struct radio_port p;
    const size_t total_pages = PROGRAM_SIZE / FLASH_PAGE_SIZE;
    int rc = -1;
    int full_write = (changed_pages == total_pages);

    if (!changed_pages) {
        printf("Firmware already matches the selected image. Nothing to write.\n");
        return 0;
    }

    st->recovery_known = 1;
    st->recovery_enabled = 1;

    for (;;) {
        uint16_t expected_sum = byte_sum16(target, PROGRAM_SIZE);
        if (confirm_firmware_program_start() != 0) return 1;

        if (open_firmware_loader_port_retry(st,&p) < 0) return -1;
        if (firmware_stream(&p, target, changed, changed_pages) < 0) {
            perror("firmware write");
            port_close_after_firmware(&p);
            invalidate_radio_connection(st);
            return -1;
        }
        port_close_after_firmware(&p);

        if (g_cli_noninteractive) {
            char why[96];
            snprintf(why, sizeof(why), "confirm the radio displays firmware checksum %04X", expected_sum);
            cli_mark_needs_interaction(why);
            return 1;
        }
        ui_flush_input();
        printf("Is the screen displaying \"%04X\"? n/Y: ", expected_sum);
        fflush(stdout);
        {
            char line[16];
            if (!fgets(line, sizeof(line), stdin))
                return -1;
            if (line[0] && line[0] != '\n' && line[0] != '\r' &&
                line[0] != 'y' && line[0] != 'Y') {
                printf("Firmware checksum does not match.\n");
                printf("Retry flashing? n/Y: ");
                fflush(stdout);
                if (!fgets(line, sizeof(line), stdin) ||
                    (line[0] && line[0] != '\n' && line[0] != '\r' &&
                     line[0] != 'y' && line[0] != 'Y')) {
                    if (!full_write) {
                        printf("Write entire firmware instead? n/Y: ");
                        fflush(stdout);
                        if (!fgets(line, sizeof(line), stdin) ||
                            (line[0] && line[0] != '\n' && line[0] != '\r' &&
                             line[0] != 'y' && line[0] != 'Y')) {
                            rc = 1;
                            goto out;
                        }
                        memset(changed, 1, total_pages);
                        changed_pages = total_pages;
                        full_write = 1;
                        printf("Retrying with a full firmware write.\n");
                        continue;
                    }
                    rc = 1;
                    goto out;
                }
                printf("Retrying firmware flash.\n");
                continue;
            }
        }

        if (ask_yes_no("Read the firmware back and perform a byte-for-byte verification?", 0)) {
            int verify_rc = verify_firmware_readback(st, target);
            if (verify_rc < 0)
                goto out;
            if (verify_rc == 2) {
                fprintf(stderr, "FIRMWARE VERIFY FAILED.\n");
                printf("FIRMWARE VERIFY FAILED, Retry flashing? n/Y: ");
                fflush(stdout);
                {
                    char line[16];
                    if (!fgets(line, sizeof(line), stdin) ||
                        (line[0] && line[0] != '\n' && line[0] != '\r' &&
                         line[0] != 'y' && line[0] != 'Y')) {
                        rc = 2;
                        goto out;
                    }
                }
                printf("Retrying firmware flash.\n");
                continue;
            }
            if (verify_rc > 0) {
                printf("Firmware verification aborted.\n");
                rc = 1;
                goto out;
            }
        }

        break;
    }

    rc = 0;
out:
    return rc;
}


#define DEV_GUARD_PAGES 2u

static int dev_page_has_data_before_signature(const uint8_t *target, size_t page)
{
    size_t start = page * FLASH_PAGE_SIZE;
    size_t i;
    if (start >= FW_KENWOOD_OFFSET) return 0;
    for (i = 0; i < FLASH_PAGE_SIZE && start + i < FW_KENWOOD_OFFSET; i++)
        if (target[start + i] != 0xFFu) return 1;
    return 0;
}

static size_t dev_plan_pages(struct app_state *st, const uint8_t *target,
                             uint8_t changed[FLASH_PAGE_COUNT],
                             int *expected_checksum_known)
{
    size_t page, changed_pages = 0;
    size_t last_used = 0;
    int have_used = 0;
    const size_t signature_page = FW_KENWOOD_OFFSET / FLASH_PAGE_SIZE;

    memset(changed, 0, FLASH_PAGE_COUNT);

    /* Where we genuinely know the installed page, ordinary offline diffing is
       exact and always wins. */
    for (page = 0; page < FLASH_PAGE_COUNT; page++) {
        if (st->dev_known_pages[page] &&
            memcmp(st->dev_baseline + page * FLASH_PAGE_SIZE,
                   target + page * FLASH_PAGE_SIZE, FLASH_PAGE_SIZE) != 0)
            changed[page] = 1;
    }

    /* Unknown installed firmware: write the actual linked/code/data body from
       the start of the image through its last non-FF page, then two erased guard
       pages.  The fixed KENWOOD signature page is deliberately outside this
       fallback region. */
    for (page = 0; page < signature_page; page++) {
        if (dev_page_has_data_before_signature(target, page)) {
            last_used = page;
            have_used = 1;
        }
    }
    if (have_used) {
        size_t end = last_used + DEV_GUARD_PAGES;        if (end >= signature_page) end = signature_page - 1u;
        for (page = 0; page <= end; page++)
            if (!st->dev_known_pages[page]) changed[page] = 1;
    }

    for (page = 0; page < FLASH_PAGE_COUNT; page++)
        if (changed[page]) changed_pages++;

    if (expected_checksum_known) {
        *expected_checksum_known = 1;
        for (page = 0; page < FLASH_PAGE_COUNT; page++) {
            if (!st->dev_known_pages[page] && !changed[page]) {
                *expected_checksum_known = 0;
                break;
            }
        }
    }
    return changed_pages;
}

static void dev_commit_written_pages(struct app_state *st, const uint8_t *target,
                                     const uint8_t changed[FLASH_PAGE_COUNT])
{
    size_t page;
    for (page = 0; page < FLASH_PAGE_COUNT; page++) {
        if (!changed[page]) continue;
        memcpy(st->dev_baseline + page * FLASH_PAGE_SIZE,
               target + page * FLASH_PAGE_SIZE, FLASH_PAGE_SIZE);
        st->dev_known_pages[page] = 1;
    }
    copy_string(st->dev_baseline_source, sizeof(st->dev_baseline_source),
                "remembered development writes");
    st->dev_baseline_model = st->model;
    st->dev_baseline_restored = 0;
    st->dev_baseline_sampled = 0;
    dev_save_persisted_baseline(st);
}

static int development_firmware_write(struct app_state *st, const uint8_t *target)
{
    uint8_t changed[FLASH_PAGE_COUNT];
    size_t changed_pages, known_before;
    int expected_known = 0;
    uint16_t expected_sum = 0;
    struct radio_port p;

    if (memcmp(target + FW_KENWOOD_OFFSET, FW_KENWOOD_TEXT, FW_KENWOOD_LEN)) {
        fprintf(stderr, "The firmware image does not appear to be a valid firmware image for this radio.\n");
        return -1;
    }
    if (st->recovery_known && !st->recovery_enabled) {
        fprintf(stderr, "Development firmware write refused: firmware loader access is known to be disabled.\n");
        fprintf(stderr, "Enable firmware loader access before using Development firmware mode.\n");
        return -1;
    }
    if (!st->port_name[0]) {
        if (g_cli_noninteractive) {
            cli_mark_needs_interaction("no serial port was supplied; use --port");
            return 1;
        }
        if (select_port(st) != 0) return 1;
    }

    known_before = dev_known_page_count(st);
    changed_pages = dev_plan_pages(st, target, changed, &expected_known);
    if (expected_known) expected_sum = byte_sum16(target, PROGRAM_SIZE);

    printf("\nDEVELOPMENT FIRMWARE WRITE\n");
    printf("The installed firmware will NOT be read first.\n");
    printf("This mode assumes firmware loader access is already enabled.\n");
    if (known_before == FLASH_PAGE_COUNT)
        printf("Baseline: %s (all pages known)\n",
               st->dev_baseline_source[0] ? st->dev_baseline_source : "known firmware");
    else if (known_before)
        printf("Baseline: %s (%zu of %u pages known)\n",
               st->dev_baseline_source[0] ? st->dev_baseline_source : "remembered session state",
               known_before, (unsigned)FLASH_PAGE_COUNT);
    else
        printf("Baseline: unknown; using linked image extent plus %u erased guard pages.\n",
               (unsigned)DEV_GUARD_PAGES);

    if (!changed_pages) {
        printf("No development firmware pages need writing.\n");
        return 0;
    }
    if (st->dev_baseline_restored && !st->dev_baseline_sampled) {
        printf("WARNING: saved development baseline has NOT been compared with the connected radio.\n");
        if (!ask_yes_no("Continue using this unverified baseline?", 0)) return 1;
    }
    if (changed_pages >= FLASH_PAGE_COUNT * 3u / 4u) {
        printf("WARNING: development mode plans to write %zu of %u firmware pages.\n",
               changed_pages, (unsigned)FLASH_PAGE_COUNT);
        printf("Check for wrong firmware, missing baseline, or incorrect image padding.\n");
        if (!ask_yes_no("Proceed with unexpectedly large development write?", 0)) return 1;
    }
    printf("Development write: %zu page(s), %zu byte(s).\n",
           changed_pages, changed_pages * (size_t)FLASH_PAGE_SIZE);
    if (!expected_known)
        printf("The final full-image checksum cannot be predicted because some untouched pages are unknown.\n");

    for (;;) {
        char line[16];
        if (confirm_firmware_program_start() != 0) return 1;
        if (open_firmware_loader_port_retry(st, &p) < 0) return -1;
        if (firmware_stream_sparse(&p, target, changed, changed_pages,
                                   expected_known, expected_sum) < 0) {
            perror("firmware write");
            port_close_after_firmware(&p);
            invalidate_radio_connection(st);
            dev_quarantine_persisted_baseline(st);
            dev_clear_baseline(st);
            printf("Development baseline forgotten because the write may have been partial.\n");
            return -1;
        }
        port_close_after_firmware(&p);

        ui_flush_input();
        if (expected_known)
            printf("Is the screen displaying \"%04X\"? n/Y: ", expected_sum);
        else
            printf("Is the radio displaying a 4-digit checksum? n/Y: ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            dev_quarantine_persisted_baseline(st);
            dev_clear_baseline(st);
            return -1;
        }
        if (!line[0] || line[0] == '\n' || line[0] == '\r' ||
            line[0] == 'y' || line[0] == 'Y')
            break;

        printf("Retry flashing? n/Y: ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin) ||
            (line[0] && line[0] != '\n' && line[0] != '\r' &&
             line[0] != 'y' && line[0] != 'Y')) {
            dev_quarantine_persisted_baseline(st);
            dev_clear_baseline(st);
            printf("Development baseline forgotten because the radio's final firmware state is uncertain.\n");
            return 1;
        }
        printf("Retrying development firmware flash.\n");
    }

    dev_commit_written_pages(st, target, changed);
    printf("Development firmware write completed. %zu of %u firmware pages are now known for this session.\n",
           dev_known_page_count(st), (unsigned)FLASH_PAGE_COUNT);
    return 0;
}

static int program_firmware_normal(struct app_state *st,
                                   const uint8_t *target,
                                   const uint8_t *known_current,
                                   const char *expected_ident,
                                   int *did_write)
{
    struct radio_port p;
    char ident[8];
    uint8_t *current = NULL, *changed = NULL;
    size_t pages = PROGRAM_SIZE / FLASH_PAGE_SIZE, changed_pages = 0;
    int rc = -1;
    int do_compare = (known_current != NULL) || st->compare_before_firmware_write;
    if(did_write)*did_write=0;

    if (memcmp(target + FW_KENWOOD_OFFSET, FW_KENWOOD_TEXT, FW_KENWOOD_LEN)) {
        fprintf(stderr, "The firmware image does not appear to be a valid firmware image for this radio.\n");
        return -1;
    }
    changed = (uint8_t *)calloc(pages, 1);
    if (!changed) goto out;
    if (do_compare) {
        current = (uint8_t *)malloc(PROGRAM_SIZE);
        if (!current) goto out;
    }

    if (open_program_session_retry(st, &p, ident, "Firmware write") < 0) goto out;
    if (expected_ident && strcmp(ident, expected_ident)) {
        printf("Radio identification differs from the original read.\n");
        printf("Stopping without writing. Start a new workflow.\n");
        exit_program_mode(&p);
        port_close(&p);
        goto out;
    }
    if (check_firmware_target_model(st, target, "firmware write") < 0) {
        exit_program_mode(&p);
        port_close(&p);
        goto out;
    }

    if (do_compare) {
        if (known_current) {
            memcpy(current, known_current, PROGRAM_SIZE);
        } else {
            printf("Reading installed firmware first so the target can be prepared and only changed pages written.\n");
            if (read_region(&p, PROGRAM_START, PROGRAM_SIZE, current, "firmware") < 0) {
                exit_program_mode(&p);
                port_close(&p);
                goto out;
            }
        }
        if (prepare_firmware_in_session(&p, current, target, changed,
                                        &changed_pages) < 0) {
            fprintf(stderr, "Could not enable/verify firmware loader access. Firmware write aborted.\n");
            exit_program_mode(&p);
            port_close(&p);
            goto out;
        }
    } else {
        memset(changed, 1, pages);
        changed_pages = pages;
        if (force_recovery_in_session(&p) < 0) {
            fprintf(stderr, "Could not enable/verify firmware loader access. Firmware write aborted.\n");
            exit_program_mode(&p);
            port_close(&p);
            goto out;
        }
    }

    exit_program_mode(&p);
    port_close(&p);
    rc = stream_prepared_firmware(st, target, changed, changed_pages);
    if(rc==0&&did_write)*did_write=(changed_pages!=0);
out:
    free(current);
    free(changed);
    return rc;
}

static int full_firmware_recovery(struct app_state *st, const uint8_t *target)
{
    uint8_t *changed;
    size_t pages = PROGRAM_SIZE / FLASH_PAGE_SIZE;
    int rc;

    if (memcmp(target + FW_KENWOOD_OFFSET, FW_KENWOOD_TEXT, FW_KENWOOD_LEN)) {
        fprintf(stderr, "The firmware image does not appear to be a valid firmware image for this radio.\n");
        return -1;
    }
    printf("\nFIRMWARE LOADER FULL WRITE\n");
    printf("This writes the complete firmware without reading the existing firmware first.\n");
    printf("The firmware loader is resident in mask ROM and does not require the installed\n");
    printf("application firmware to run. The loader-enable bit in channel data must be set.\n\n");
    if (!st->port_name[0]) {
        if (g_cli_noninteractive) {
            cli_mark_needs_interaction("no serial port was supplied; use --port");
            return 1;
        }
        if (select_port(st) != 0) return 1;
    }

    changed = (uint8_t *)malloc(pages);
    if (!changed) return -1;
    memset(changed, 1, pages);
    rc = stream_prepared_firmware(st, target, changed, pages);
    free(changed);
    if (rc == 0) dev_set_full_baseline(st, target, "full firmware recovery");
    return rc;
}

/* ------------------------------ Channel I/O ------------------------------ */

static int write_channels_in_session(struct radio_port *p, const uint8_t *input,
                                     int force_recovery)
{
    uint8_t *cur = (uint8_t *)malloc(CHANNEL_SIZE);
    uint8_t *target = (uint8_t *)malloc(CHANNEL_SIZE);
    uint8_t *verify = (uint8_t *)malloc(CHANNEL_SIZE);
    const size_t total = CHANNEL_SIZE / BLOCK_SIZE;
    size_t b, changed = 0, done = 0;
    int rc = -1;
    if (!cur || !target || !verify) goto out;
    memcpy(target, input, CHANNEL_SIZE);
    if (force_recovery && !(target[RECOVERY_OFFSET] & RECOVERY_MASK)) {
        printf("Enabling firmware loader access in the channel data before writing it.\n");
        target[RECOVERY_OFFSET] |= RECOVERY_MASK;
    }
    if (read_region(p, CHANNEL_START, CHANNEL_SIZE, cur, "channels") < 0) goto out;
    for (b = 0; b < total; b++)
        if (memcmp(cur + b * BLOCK_SIZE, target + b * BLOCK_SIZE, BLOCK_SIZE)) changed++;
    if (!changed) {
        printf("Channels already match. Nothing to write.\n");
        rc = 0;
        goto out;
    }
    printf("Channel write: %zu changed block(s) out of %zu total blocks.\n", changed, total);
    for (b = 0; b < total; b++) {
        uint16_t a;
        if (!memcmp(cur + b * BLOCK_SIZE, target + b * BLOCK_SIZE, BLOCK_SIZE)) continue;
        a = (uint16_t)(CHANNEL_START + b * BLOCK_SIZE);
        done++;
        progress_line("Writing", "channels", done, changed, total, a,
                      a + BLOCK_SIZE - 1);
        if (write_block(p, a, target + b * BLOCK_SIZE) < 0 ||
            verify_block(p, a, target + b * BLOCK_SIZE) < 0) {
            progress_done();
            goto out;
        }
    }
    progress_done();
    if (read_region(p, CHANNEL_START, CHANNEL_SIZE, verify, "verify") < 0 ||
        memcmp(verify, target, CHANNEL_SIZE)) {
        fprintf(stderr, "CHANNEL VERIFY FAILED.\n");
        goto out;
    }
    printf("Channels successfully written and verified.\n");
    rc = 0;
out:
    free(cur);
    free(target);
    free(verify);
    return rc;
}

/* -------------------------- Full read / backup --------------------------- */

static int read_full_radio(struct app_state *st, uint8_t firmware[PROGRAM_SIZE],
                           uint8_t channels[CHANNEL_SIZE], int keep_session,
                           struct radio_port *outp, char ident[8])
{
    struct radio_port p;
    if (open_program_session_retry(st, &p, ident, "Full radio read") < 0) return -1;
    if (read_region(&p, PROGRAM_START, PROGRAM_SIZE, firmware, "firmware") < 0 ||
        read_region(&p, CHANNEL_START, CHANNEL_SIZE, channels, "channels") < 0) {
        exit_program_mode(&p);
        port_close(&p);
        st->connected = 0;
        return -1;
    }
    if (keep_session) {
        *outp = p;
        return 0;
    }
    exit_program_mode(&p);
    port_close(&p);
    return 0;
}

static int backup_destination_writable(const struct app_state *st)
{
    char test[MAX_PATH_LEN], name[96];
    FILE *f;
    long pid = 0;
#ifdef _WIN32
    pid = (long)_getpid();
#else
    pid = (long)getpid();
#endif
    if (!st->backup_dir[0]) return -1;
    if (make_dir_if_needed(st->backup_dir) < 0) return -1;
    snprintf(name, sizeof(name), ".tkx4xtoolkit_write_test_%ld_%ld", pid, (long)time(NULL));
    if (path_join(test, st->backup_dir, name) < 0) return -1;
    f = fopen(test, "wb");
    if (!f) return -1;
    if (fclose(f) != 0) {
        remove(test);
        return -1;
    }
    if (remove(test) != 0) return -1;
    return 0;
}

static void make_auto_backup_name_suffix(struct app_state *st,
                                             const char *suffix,
                                             char out[MAX_PATH_LEN])
{
    struct tm tm;
    char stamp[32], model[32], leaf[160];
    const char *dir = st->backup_dir;
    int n = 1;
    size_t dl, ll;

    local_time_now(&tm);
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm);
    snprintf(model, sizeof(model), "%s", model_name(st->model));
    sanitize_filename(model);
    if (make_dir_if_needed(dir) < 0) perror(dir);

    for (;;) {
        if (n == 1) snprintf(leaf, sizeof(leaf), "%s_%s_%s.bin", model, stamp, suffix);
        else snprintf(leaf, sizeof(leaf), "%s_%s_%d_%s.bin", model, stamp, n, suffix);
        dl = strlen(dir);
        ll = strlen(leaf);
        if (dl + 1 + ll + 1 >= MAX_PATH_LEN) {
            fprintf(stderr, "Backup directory path is too long; using current directory.\n");
            copy_string(out, MAX_PATH_LEN, leaf);
        } else {
            memcpy(out, dir, dl);
            out[dl] = PATH_SEP;
            memcpy(out + dl + 1, leaf, ll + 1);
        }
        if (!file_exists(out)) break;
        n++;
    }
}

static void make_auto_backup_name(struct app_state *st, char out[MAX_PATH_LEN])
{
    make_auto_backup_name_suffix(st, "full", out);
}

static void populate_full_backup_state(struct app_state *st,
                                      const uint8_t firmware[PROGRAM_SIZE],
                                      const uint8_t channels[CHANNEL_SIZE],
                                      const char ident[8])
{
    if (ident && ident[0]) {
        copy_string(st->ident, sizeof(st->ident), ident);
        st->model = model_from_ident(ident);
    }
    st->connected = 1;

    memcpy(st->firmware, firmware, PROGRAM_SIZE);
    st->firmware_loaded = 1;
    st->firmware_modified = 0;
    classify_loaded_firmware(st);
    remember_loaded_firmware_on_radio(st);

    memcpy(st->channels, channels, CHANNEL_SIZE);
    st->channels_loaded = 1;
    st->channels_model = st->model;
    copy_string(st->channels_source, sizeof(st->channels_source), "Radio");
    update_preview_from_full_channels(st);
    radio_preview_from_loaded_channels(st);

    st->recovery_known = 1;
    st->recovery_enabled = !!(channels[RECOVERY_OFFSET] & RECOVERY_MASK);
    copy_string(st->firmware_source, sizeof(st->firmware_source), "Radio");
}

static int save_full_backup_to_path(struct app_state *st,
                                    const uint8_t firmware[PROGRAM_SIZE],
                                    const uint8_t channels[CHANNEL_SIZE],
                                    const char ident[8],
                                    const char *path)
{
    uint8_t *full = (uint8_t *)malloc(FULL_SIZE);
    if (!full) return -1;
    memcpy(full, firmware, PROGRAM_SIZE);
    memcpy(full + PROGRAM_SIZE, channels, CHANNEL_SIZE);
    if (write_file_new_or_confirm(path, full, FULL_SIZE, 0) < 0) {
        free(full);
        return -1;
    }
    free(full);
    populate_full_backup_state(st, firmware, channels, ident);
    printf("Full backup saved: %s\n", path);
    return 0;
}

static int save_full_backup_buffers(struct app_state *st,
                                    const uint8_t firmware[PROGRAM_SIZE],
                                    const uint8_t channels[CHANNEL_SIZE],
                                    const char ident[8],
                                    char saved_path[MAX_PATH_LEN])
{
    make_auto_backup_name(st, saved_path);
    return save_full_backup_to_path(st, firmware, channels, ident, saved_path);
}

static int full_backup(struct app_state *st, char saved_path[MAX_PATH_LEN])
{
    uint8_t firmware[PROGRAM_SIZE], channels[CHANNEL_SIZE];
    char ident[8];
    if (read_full_radio(st, firmware, channels, 0, NULL, ident) < 0) return -1;

    /* Treat the completed radio read as authoritative in-memory state before
       doing any file I/O. The UI therefore reflects the backup even if saving
       the output file later encounters a filesystem problem. */
    populate_full_backup_state(st, firmware, channels, ident);
    printf("Full backup read successfully. Firmware and channel data loaded.\n");
    print_memory_jogger(st);
    return save_full_backup_buffers(st, firmware, channels, ident, saved_path);
}

/* ------------------------------- UI status ------------------------------- */

static void sha256_short(const char *full, char out[40])
{
    if (!full || strlen(full) < 64) {
        copy_string(out, 40, "--");
        return;
    }
    /* 10-byte prefix + 4-byte suffix. Both portions are whole byte pairs,
       with the prefix intentionally longer for a useful compact fingerprint. */
    snprintf(out, 40, "%.20s...%.8s", full, full + 56);
}

static const char *password_display(const struct app_state *st)
{
    if(!st->connected) return "--";
    if(!st->password_known) return "<unavailable>";
    if(!st->password_enabled) return "<none>";
    return st->password[0] ? st->password : "<empty>";
}

static void format_patch_status(const struct app_state *st, char *out, size_t cap, int radio)
{
    enum tx_behavior tx = radio ? st->radio_tx : st->tx;
    enum dealer_tx_behavior dealer_tx = radio ? st->radio_dealer_tx : st->dealer_tx;
    enum tot_behavior tot = radio ? st->radio_tot : st->tot;
    if(!radio && st->firmware_loaded && st->tot_test_10x){
        if(tx==TX_FIXED && dealer_tx==DEALER_TX_FIELD)
            snprintf(out,cap,"Patched: Yes (Enhanced TX + TOT 10x TEST)");
        else
            snprintf(out,cap,"Patched: Yes (TOT 10x TEST MODE)");
        return;
    }
    /* Unknown loaded files must not inherit the connected radio's model
       or accidentally display its patch classification. */
    if (!radio && st->firmware_loaded && !st->firmware_rev) {
        snprintf(out, cap, "Patched: Unknown (unrecognized firmware)");
        return;
    }
    enum model_family patch_model = radio ? st->model :
        st->firmware_rev ? st->firmware_rev->model : MODEL_UNKNOWN;
    if(patch_model==MODEL_840){
        if(tot==TOT_LEGACY)
            snprintf(out,cap,"Patched: Yes (legacy unconditional TOT bypass)");
        else if(tot==TOT_PATCHED)snprintf(out,cap,"Patched: Yes (Enhanced TOT)");
        else if(tot==TOT_STOCK)snprintf(out,cap,"Patched: No (stock)");
        else if(tot==TOT_CONFLICT)
            snprintf(out,cap,"Patched: Unknown (TOT code changed)");
        else snprintf(out,cap,"Patched: Unknown");
        return;
    }
    if(patch_model!=MODEL_940 && patch_model!=MODEL_941){
        if(cap)out[0]='\0';
        return;
    }
    if(!radio && st->firmware_loaded && st->firmware_rev &&
       tkx4x_plus25_state(st->firmware,st->firmware_rev->checksum)==
           TKX4X_PLUS25_PRESENT){
        if(tot==TOT_LEGACY)
            snprintf(out,cap,"Patched: Yes (+25 MHz + legacy TOT)");
        else if(tot==TOT_PATCHED)
            snprintf(out,cap,"Patched: Yes (+25 MHz + Enhanced TOT)");
        else
            snprintf(out,cap,"Patched: Yes (+25 MHz offset)");
        return;
    }
    if(tot==TOT_LEGACY){
        snprintf(out,cap,"Patched: Yes (legacy TOT bypass%s)",
                 tx==TX_FIXED && dealer_tx==DEALER_TX_FIELD
                     ? " + Enhanced TX" : "");
    }else if(tot==TOT_CONFLICT){
        snprintf(out,cap,"Patched: Unknown (TOT code changed)");
    }else if(tx==TX_FIXED && dealer_tx==DEALER_TX_FIELD){
        snprintf(out,cap,tot==TOT_PATCHED
                 ? "Patched: Yes (Enhanced TX + Enhanced TOT)" : "Patched: Yes (Enhanced TX)");
    }else if(tx==TX_FIXED_OFFSET && dealer_tx==DEALER_TX_FIXED_OFFSET){
        /* Enhanced TOT retains its identity when Enhanced TX is removed. */
        snprintf(out,cap,tot==TOT_PATCHED
                 ? "Patched: Yes (Enhanced TOT)" : "Patched: No (stock)");
    }else if(tx==TX_FIXED){
        snprintf(out,cap,"Patched: Partial (channel TX only)");
    }else if(dealer_tx==DEALER_TX_FIELD){
        snprintf(out,cap,"Patched: Partial (dealer/test only)");
    }else if(tot==TOT_PATCHED){
        snprintf(out,cap,"Patched: Yes (Enhanced TOT; TX unknown)");
    }else if(tx==TX_FIXED_OFFSET){
        snprintf(out,cap,"Patched: No (stock)");
    }else snprintf(out,cap,"Patched: Unknown");
}

/* Memory state is deliberately independent of the connected radio. */
static void format_left_status(const struct app_state *st, int line, char *out, size_t cap)
{
    char tmp[160], short_sha[40];
    switch (line) {
    case 0:
        snprintf(tmp, sizeof(tmp), "Firmware: %s",
                 !st->firmware_loaded ? "Not loaded" :
                 st->firmware_rev ? st->firmware_rev->label : "Unknown");
        break;
    case 1:
        if (st->firmware_loaded) {
            const char *modified = st->firmware_modified &&
                !find_full_revision(st->firmware_sha) ? " (modified)" : "";
            snprintf(tmp, sizeof(tmp), "Source: %.*s%s",
                     *modified ? 24 : 37,
                     st->firmware_source[0] ? st->firmware_source : "--", modified);
        } else
            snprintf(tmp, sizeof(tmp), "Source: --");
        break;
    case 2:
        if (st->firmware_loaded)
            format_patch_status(st, tmp, sizeof(tmp), 0);
        else
            snprintf(tmp, sizeof(tmp), "Patched: --");
        break;
    case 3:
        if (st->firmware_loaded)
            snprintf(tmp, sizeof(tmp), "Checksum: %04X", st->firmware_sum);
        else
            snprintf(tmp, sizeof(tmp), "Checksum: --");
        break;
    case 4:
        if (st->firmware_loaded) {
            sha256_short(st->firmware_sha, short_sha);
            snprintf(tmp, sizeof(tmp), "SHA-256: %s", short_sha);
        } else
            snprintf(tmp, sizeof(tmp), "SHA-256: --");
        break;
    case 5:
        snprintf(tmp, sizeof(tmp), "Channels: %s",
                 st->channels_loaded ? "Loaded" : "Not loaded");
        break;
    case 6:
        snprintf(tmp, sizeof(tmp), "Channel source: %.29s",
                 st->channels_loaded ?
                 (st->channels_source[0] ? st->channels_source : "--") : "--");
        break;
    case 7:
        snprintf(tmp, sizeof(tmp), "Programmer: %.34s",
                 st->port_name[0] ? port_display_name(st->port_name) : "Not set");
        break;
    default:
        tmp[0] = '\0';
        break;
    }
    copy_string(out, cap, tmp);
}

/* Short, deliberately separate radio observations fit the 29-column pane. */
static void format_right_status(const struct app_state *st, int line,
                                char *out, size_t cap)
{
    char tmp[100];
    int probed = st->connected && st->radio_fingerprinted;
    switch (line) {
    case 0:
        snprintf(tmp, sizeof(tmp), "Model: %s",
                 st->connected ? model_name(st->model) : "Not probed");
        break;
    case 1:
        if (!probed)
            snprintf(tmp, sizeof(tmp), "Firmware: Not probed");
        else if (st->quick_rev)
            snprintf(tmp, sizeof(tmp), "FW: %04X (%s)",
                     st->quick_rev->checksum,
                     st->radio_full_read ? "verified" : "fingerprinted");
        else
            snprintf(tmp, sizeof(tmp), "FW: Unknown (fingerprinted)");
        break;
    case 2:
        snprintf(tmp, sizeof(tmp), "Radio ID: %s",
                 st->connected && st->ident[0] ? st->ident : "--");
        break;
    case 3:
        if (!probed || st->model == MODEL_840)
            snprintf(tmp, sizeof(tmp), "TX patch: %s",
                     probed && st->model == MODEL_840 ? "N/A" : "--");
        else
            snprintf(tmp, sizeof(tmp), "TX patch: %s",
                     st->radio_tx == TX_FIXED &&
                     st->radio_dealer_tx == DEALER_TX_FIELD ? "Enhanced" :
                     st->radio_tx == TX_FIXED_OFFSET &&
                     st->radio_dealer_tx == DEALER_TX_FIXED_OFFSET ? "Stock" :
                     "Unknown");
        break;
    case 4:
        snprintf(tmp, sizeof(tmp), "TOT patch: %s",
                 !probed ? "--" :
                 st->radio_tot == TOT_PATCHED ? "Enhanced" :
                 st->radio_tot == TOT_STOCK ? "Stock" :
                 st->radio_tot == TOT_LEGACY ? "Legacy bypass" :
                 st->radio_tot == TOT_CONFLICT ? "Unknown (code changed)" :
                 "Unknown");
        break;
    case 5:
        snprintf(tmp, sizeof(tmp), "Password: %s", password_display(st));
        break;
    default:
        tmp[0] = '\0';
        break;
    }
    copy_string(out, cap, tmp);
}

static void print_status(const struct app_state *st)
{
    int i;
    char left[STATUS_LEFT_WIDTH + 1], right[STATUS_RIGHT_WIDTH + 1];
    printf("\nKenwood TK-x4x Toolkit %s (c) copyright 2026 by Dave KY4WI\n\n",
           tool_display_version());
    printf("%-*s | %-*s\n", STATUS_LEFT_WIDTH, "LOADED IN MEMORY",
           STATUS_RIGHT_WIDTH, "CONNECTED RADIO");
    {
        char lrule[STATUS_LEFT_WIDTH + 1], rrule[STATUS_RIGHT_WIDTH + 1];
        memset(lrule, '-', STATUS_LEFT_WIDTH);
        memset(rrule, '-', STATUS_RIGHT_WIDTH);
        lrule[STATUS_LEFT_WIDTH] = '\0';
        rrule[STATUS_RIGHT_WIDTH] = '\0';
        printf("%s-+-%s\n", lrule, rrule);
    }
    for (i = 0; i < 8; i++) {
        format_left_status(st, i, left, sizeof(left));
        format_right_status(st, i, right, sizeof(right));
        printf("%-*.*s | %-*.*s\n",
               STATUS_LEFT_WIDTH, STATUS_LEFT_WIDTH, left,
               STATUS_RIGHT_WIDTH, STATUS_RIGHT_WIDTH, right);
    }
    printf("\n");
    if (st->connected && st->radio_fingerprinted && !st->quick_rev)
        printf("Radio firmware fingerprint not recognized; full read needed.\n\n");
    if (st->firmware_loaded && !st->firmware_rev)
        printf("Loaded firmware not recognized; it may be modified or another revision.\n\n");
}

/* ---------------------------- Port selection UI -------------------------- */

static size_t enumerate_ports(char ports[MAX_PORT_CHOICES][MAX_PATH_LEN])
{
    size_t n=0;
#ifdef _WIN32
    int i;for(i=1;i<=256&&n<MAX_PORT_CHOICES;i++){char name[16],target[256];snprintf(name,sizeof(name),"COM%d",i);if(QueryDosDeviceA(name,target,sizeof(target)))snprintf(ports[n++],MAX_PATH_LEN,"%s",name);}
#else
    const char *patterns[]={"/dev/ttyUSB*","/dev/ttyACM*","/dev/cu.usbserial*","/dev/cu.usbmodem*","/dev/cu.SLAB_USBtoUART*"};size_t pi;for(pi=0;pi<sizeof(patterns)/sizeof(patterns[0])&&n<MAX_PORT_CHOICES;pi++){glob_t g;size_t i;if(glob(patterns[pi],0,NULL,&g)==0){for(i=0;i<g.gl_pathc&&n<MAX_PORT_CHOICES;i++){size_t j;int dup=0;for(j=0;j<n;j++)if(!strcmp(ports[j],g.gl_pathv[i]))dup=1;if(!dup)snprintf(ports[n++],MAX_PATH_LEN,"%s",g.gl_pathv[i]);}}globfree(&g);}
#endif
    return n;
}

static int selected_port_is_present(const char *name)
{
    char ports[MAX_PORT_CHOICES][MAX_PATH_LEN];
    size_t n, i;
    if (!name || !*name) return 0;
    n = enumerate_ports(ports);
    for (i = 0; i < n; i++)
        if (!strcmp(name, ports[i])) return 1;
#ifndef _WIN32
    /* Preserve manually entered Unix device names that are not covered by
       the common USB/ACM enumeration patterns. */
    if (access(name, F_OK) == 0) return 1;
#endif
    return 0;
}

struct port_screen_ctx {
    const char *title;
};

static void draw_port_screen(void *vctx)
{
    const struct port_screen_ctx *ctx = (const struct port_screen_ctx *)vctx;
    printf("%-*.*s | INFO\n", STATUS_LEFT_WIDTH, STATUS_LEFT_WIDTH, ctx->title ? ctx->title : "Select Serial Port");
}

static int port_list_contains(char ports[MAX_PORT_CHOICES][MAX_PATH_LEN],
                              size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (!strcmp(ports[i], name)) return 1;
    return 0;
}

static int port_list_add_unique(char ports[MAX_PORT_CHOICES][MAX_PATH_LEN],
                                size_t *n, const char *name)
{
    if (*n >= MAX_PORT_CHOICES || port_list_contains(ports, *n, name)) return 0;
    copy_string(ports[*n], MAX_PATH_LEN, name);
    (*n)++;
    return 1;
}

static const char *port_display_name(const char *name)
{
    if (!name) return "";
#ifndef _WIN32
    if (!strncmp(name, "/dev/", 5)) return name + 5;
#endif
    return name;
}

static void normalize_port_name(char *out, size_t cap, const char *name)
{
    if (!name || !*name) {
        if (cap) out[0] = '\0';
        return;
    }
#ifdef _WIN32
    copy_string(out, cap, name);
#else
    if (name[0] == '/')
        copy_string(out, cap, name);
    else {
        size_t used;
        copy_string(out, cap, "/dev/");
        used = strlen(out);
        if (used < cap) copy_string(out + used, cap - used, name);
    }
#endif
}

/* A port pathname is not a live connection. Remove cached radio observations
   on serial errors and on manual reselection without discarding offline images. */
static void invalidate_radio_connection(struct app_state *st)
{
    st->connected = 0;
    st->ident[0] = '\0';
    st->model = MODEL_UNKNOWN;
    forget_radio_firmware_observation(st);
    st->recovery_known = 0;
    st->recovery_enabled = 0;
    st->password_known = 0;
    st->password_enabled = 0;
    st->password[0] = '\0';
    st->radio_preview_count = 0;
    memset(st->radio_preview, 0, sizeof(st->radio_preview));
    if (!st->channels_loaded) {
        memset(st->preview, 0, sizeof(st->preview));
        st->preview_count = 0;
    }
}

static void set_programmer_port(struct app_state *st, const char *name)
{
    normalize_port_name(st->port_name, sizeof(st->port_name), name);
    invalidate_radio_connection(st);
}

static void port_ordinal_label(char *out, size_t cap, size_t index, const char *name)
{
    size_t number = index + 1u;
    const char *suffix = "th";
    const char *display = port_display_name(name);
    if (number % 100u < 11u || number % 100u > 13u) {
        if (number % 10u == 1u) suffix = "st";
        else if (number % 10u == 2u) suffix = "nd";
        else if (number % 10u == 3u) suffix = "rd";
    }
    {
        int max_name = cap > 16u ? (int)cap - 16 : 0;
        snprintf(out, cap, "%zu%s port (%.*s)", number, suffix, max_name, display);
    }
}

static int choose_detected_ports(struct app_state *st,
                                 char ports[MAX_PORT_CHOICES][MAX_PATH_LEN],
                                 size_t n)
{
    struct ui_item items[MAX_PORT_CHOICES + 2];
    char labels[MAX_PORT_CHOICES][MAX_PATH_LEN + 32];
    size_t selected = 0, i;
    int choice;
    struct port_screen_ctx ctx = {"Select USB interface"};

    for (i = 0; i < n; i++) {
        port_ordinal_label(labels[i], sizeof(labels[i]), i, ports[i]);
        items[i].key = (i < 9u) ? (char)('1' + (int)i) : 0;
        items[i].label = labels[i];
        items[i].detail = NULL;
        items[i].help = "Use this detected serial interface as the radio programmer.";
    }
    items[n].key = 'M';
    items[n].label = "Enter port manually";
    items[n].detail = NULL;
    items[n].help = "Type a known serial port directly, such as ttyUSB0 or COM3.";
    items[n + 1].key = 'B';
    items[n + 1].label = "Back [Q]";
    items[n + 1].detail = NULL;
    items[n + 1].help = "Cancel this selection and return to the previous screen. Esc also works.";

    choice = ui_menu(items, n + 2, &selected, draw_port_screen, &ctx);
    if (choice == UI_KEY_EOF || choice == UI_KEY_ESCAPE ||
        choice == 'b' || choice == 'q' || choice == 'e') return 1;
    if (choice == 'm') {
        char manual[MAX_PATH_LEN];
        prompt_string("Serial port", port_display_name(st->port_name), manual, sizeof(manual));
        if (!manual[0]) return 1;
        set_programmer_port(st, manual);
        printf("Selected: programmer port %s\n", port_display_name(st->port_name));
        return 0;
    }
    if (choice == UI_KEY_SELECTED || (choice >= '1' && choice <= '9')) {
        if (choice >= '1' && choice <= '9') selected = (size_t)(choice - '1');
        if (selected < n) {
            set_programmer_port(st, ports[selected]);
            return 0;
        }
    }
    return 1;
}

static int probe_usb_port_for_radio(const char *name)
{
    struct radio_port p;
    char ident[8];

    if (port_open(&p, name) < 0) return 0;
    if (enter_program_mode(&p, ident) < 0) {
        port_close(&p);
        return 0;
    }

    /* Auto-detection only needs to prove that the port talks to a radio.
       Leave the radio in normal operation before returning to the caller. */
    exit_program_mode(&p);
    port_close(&p);
    return 1;
}

static int auto_detect_usb_port(struct app_state *st)
{
    char baseline[MAX_PORT_CHOICES][MAX_PATH_LEN];
    char current[MAX_PORT_CHOICES][MAX_PATH_LEN];
    char candidates[MAX_PORT_CHOICES][MAX_PATH_LEN];
    unsigned missing_steps[MAX_PORT_CHOICES];
    size_t baseline_n, current_n, candidate_n;
    uint64_t deadline_ms;

restart_detection:
    printf("\nAuto-detect USB Programmer\n\n");
    printf("Auto-detection only works with USB programmers.\n");
    printf("If you're using a non-USB programmer, press M to enter the serial port manually.\n\n");
    printf("Make sure the next USB device you plug in is your radio programmer.\n");
    printf("If it is already connected, unplug it and plug it back in.\n");
    printf("Twice a second, we're watching for a USB serial device to appear.\n");
    printf("A newly detected device will be probed for a Kenwood radio.\n");
    printf("Detection times out in 60 seconds. Press Esc or Q to stop detection.\n\n");
    fflush(stdout);

    baseline_n = enumerate_ports(baseline);
    memset(missing_steps, 0, sizeof(missing_steps));
    candidate_n = 0;
    deadline_ms = monotonic_ms() + USB_DETECT_TIMEOUT_MS;

    for (;;) {
        size_t i;
        int key;

        if (monotonic_ms() >= deadline_ms) goto timed_out;
        key = ui_read_key_timeout(USB_DETECT_INTERVAL_MS);
        if (key == 'q' || key == UI_KEY_ESCAPE) return 1;
        if (key == 'm') goto manual_port;
        if (monotonic_ms() >= deadline_ms) goto timed_out;

        current_n = enumerate_ports(current);
        candidate_n = 0;

        for (i = 0; i < baseline_n; i++) {
            if (!port_list_contains(current, current_n, baseline[i])) {
                if (missing_steps[i] < USB_MISSING_STEPS)
                    missing_steps[i]++;
            } else {
                if (missing_steps[i] >= USB_MISSING_STEPS)
                    port_list_add_unique(candidates, &candidate_n, baseline[i]);
                missing_steps[i] = 0;
            }
        }

        for (i = 0; i < current_n; i++) {
            if (!port_list_contains(baseline, baseline_n, current[i]))
                port_list_add_unique(candidates, &candidate_n, current[i]);
        }

        if (candidate_n) {
            unsigned settle_steps = USB_SETTLE_MS / USB_DETECT_INTERVAL_MS;
            unsigned step;

            for (step = 0; step < settle_steps; step++) {
                if (monotonic_ms() >= deadline_ms) goto timed_out;
                key = ui_read_key_timeout(USB_DETECT_INTERVAL_MS);
                if (key == 'q' || key == UI_KEY_ESCAPE) return 1;
                if (key == 'm') goto manual_port;
                current_n = enumerate_ports(current);
            }

            {
                char working[MAX_PORT_CHOICES][MAX_PATH_LEN];
                size_t working_n = 0;
                for (i = 0; i < candidate_n; i++) {
                    int responded = 0;
                    if (!port_list_contains(current, current_n, candidates[i]))
                        continue;
                    printf("Probing USB serial device: %s\n", port_display_name(candidates[i]));
                    fflush(stdout);
                    responded = probe_usb_port_for_radio(candidates[i]);
                    if (!responded && monotonic_ms() < deadline_ms) {
                        printf("No response. Waiting briefly and probing it once more...\n");
                        key = ui_read_key_timeout(USB_DETECT_INTERVAL_MS);
                        if (key == 'q' || key == UI_KEY_ESCAPE) return 1;
                        if (key == 'm') goto manual_port;
                        current_n = enumerate_ports(current);
                        if (port_list_contains(current, current_n, candidates[i]))
                            responded = probe_usb_port_for_radio(candidates[i]);
                    }
                    if (responded)
                        port_list_add_unique(working, &working_n, candidates[i]);
                }

                if (working_n == 1) {
                    char label[MAX_PATH_LEN + 32];
                    port_ordinal_label(label, sizeof(label), 0, working[0]);
                    set_programmer_port(st, working[0]);
                    printf("USB programmer detected: %s\n", label);
                    printf("Selected: %s\n", label);
                    return 0;
                }
                if (working_n > 1) {
                    printf("\nMultiple USB serial devices responded as Kenwood programmers.\n\n");
                    return choose_detected_ports(st, working, working_n);
                }
                printf("No candidate USB serial device responded as a Kenwood radio.\n");
                baseline_n = current_n;
                if (baseline_n) memcpy(baseline, current, baseline_n * sizeof(baseline[0]));
                memset(missing_steps, 0, sizeof(missing_steps));
            }
        }
    }

manual_port:
    {
        char manual[MAX_PATH_LEN], normalized[MAX_PATH_LEN];
        prompt_string("Serial port", port_display_name(st->port_name), manual, sizeof(manual));
        if (!manual[0]) return 1;
        normalize_port_name(normalized, sizeof(normalized), manual);
        set_programmer_port(st, normalized);
        printf("Selected: programmer port %s\n", port_display_name(st->port_name));
        return 0;
    }

timed_out:
    for (;;) {
        struct ui_item items[] = {
            {'R', "Retry detection", NULL,
             "Start USB programmer detection again."},
            {'M', "Enter port manually", NULL,
             "Type a known serial port directly."},
            {'B', "Back [Q]", NULL,
             "Stop detection and return to the previous screen. Esc also works."}
        };
        size_t selected = 0;
        struct port_screen_ctx ctx = {"Auto-detect timed out. No USB programmer responded."};
        int choice = ui_menu(items, sizeof(items) / sizeof(items[0]), &selected,
                             draw_port_screen, &ctx);
        if (choice == 'r') goto restart_detection;
        if (choice == 'm') goto manual_port;
        if (choice == UI_KEY_ESCAPE || choice == 'b' || choice == 'q' || choice == UI_KEY_EOF)
            return 1;
    }
}


static int select_port(struct app_state *st)
{
    for (;;) {
        char ports[MAX_PORT_CHOICES][MAX_PATH_LEN];
        struct ui_item items[MAX_PORT_CHOICES + 3];
        char labels[MAX_PORT_CHOICES][MAX_PATH_LEN + 32];
        size_t n = enumerate_ports(ports), i, selected = 0;
        int choice;
        struct port_screen_ctx ctx = {n ? "Set Programmer Port" : "No USB serial ports detected."};

        if (!n) {
            struct ui_item empty_items[] = {
                {'R', "Retry", NULL,
                 "Connect the programming cable, then scan again for USB serial ports."},
                {'M', "Enter port manually", NULL,
                 "Type a known serial port directly, such as ttyUSB0 or COM3."},
                {'D', "Detect USB programmer", NULL,
                 "Connect or reconnect the programming cable and let the toolkit identify it."},
                {'B', "Back [Q]", NULL,
                 "Return to the previous screen. Esc also works."}
            };
            choice = ui_menu(empty_items, sizeof(empty_items) / sizeof(empty_items[0]),
                             &selected, draw_port_screen, &ctx);            if (choice == 'r') continue;
            if (choice == 'm') {
                char manual[MAX_PATH_LEN], normalized[MAX_PATH_LEN];
                prompt_string("Serial port", port_display_name(st->port_name), manual, sizeof(manual));
                if (!manual[0]) continue;
                normalize_port_name(normalized, sizeof(normalized), manual);
                set_programmer_port(st, normalized);
                printf("Selected: programmer port %s\n", port_display_name(st->port_name));
                return 0;
            }
            if (choice == 'd') {
                if (auto_detect_usb_port(st) == 0) return 0;
                continue;
            }
            if (choice == UI_KEY_ESCAPE || choice == UI_KEY_EOF ||
                choice == 'b' || choice == 'q' || choice == 'e') return 1;
            continue;
        }

        for (i = 0; i < n; i++) {
            port_ordinal_label(labels[i], sizeof(labels[i]), i, ports[i]);
            items[i].key = (i < 9u) ? (char)('1' + (int)i) : 0;
            items[i].label = labels[i];
            items[i].detail = NULL;
            items[i].help = "Use this detected serial port as the radio programmer.";
        }
        items[n].key = 'M';
        items[n].label = "Enter port manually";
        items[n].detail = NULL;
        items[n].help = "Type a known serial port directly. Linux accepts ttyUSB0 or /dev/ttyUSB0.";
        items[n + 1].key = 'D';
        items[n + 1].label = "Detect USB programmer";
        items[n + 1].detail = NULL;
        items[n + 1].help = "Unplug and reconnect a USB programmer so the toolkit can identify its port.";
        items[n + 2].key = 'B';
        items[n + 2].label = "Back [Q]";
        items[n + 2].detail = NULL;
        items[n + 2].help = "Return to the previous screen. Esc also works.";

        choice = ui_menu(items, n + 3, &selected, draw_port_screen, &ctx);
        if (choice == UI_KEY_ESCAPE || choice == UI_KEY_EOF ||
            choice == 'b' || choice == 'q' || choice == 'e') return 1;
        if (choice == 'm') {
            char manual[MAX_PATH_LEN];
            char normalized[MAX_PATH_LEN];
            prompt_string("Serial port", port_display_name(st->port_name), manual, sizeof(manual));
            if (!manual[0]) continue;
            normalize_port_name(normalized, sizeof(normalized), manual);
            set_programmer_port(st, normalized);
            printf("Selected: programmer port %s\n", port_display_name(st->port_name));
            return 0;
        }
        if (choice == 'd') {
            if (auto_detect_usb_port(st) == 0) return 0;
            continue;
        }
        if (choice == UI_KEY_SELECTED || (choice >= '1' && choice <= '9')) {
            if (choice >= '1' && choice <= '9') selected = (size_t)(choice - '1');
            if (selected < n) {
                set_programmer_port(st, ports[selected]);
                return 0;
            }
        }
    }
}

static void print_connection_failure(const struct app_state *st)
{
    printf("\nConnection failed.\n");
    printf("Check the cable, serial port, and radio power.\n");
    printf("Make sure the radio screen is not flashing \"PROGRAM\" or displaying a 4-digit\n");
    printf("checksum. Non-flashing \"PROGRAM\" or normal radio mode is OK.\n");
    if (st->port_name[0] && !selected_port_is_present(st->port_name))
        printf("Selected serial port is no longer available.\n");
}

static int connection_failure_menu(const char *purpose)
{
    char retry_label[96];
    struct ui_item items[4];
    size_t selected = 0;
    struct port_screen_ctx ctx = {"Connection failed"};
    snprintf(retry_label, sizeof(retry_label), "Retry %s", purpose && *purpose ? purpose : "connection");
    items[0] = (struct ui_item){'R', retry_label, NULL,
        "Try the same programmer port and radio again."};
    items[1] = (struct ui_item){'S', "Set Programmer Port", NULL,
        "Choose or manually enter a different serial port."};
    items[2] = (struct ui_item){'D', "Detect USB programmer", NULL,
        "Use unplug/replug detection to find a USB programmer."};
    items[3] = (struct ui_item){'M', "Main Menu", NULL,
        "Stop this operation and return to the Main Menu."};
    return ui_menu(items, 4, &selected, draw_port_screen, &ctx);
}

static int connect_menu(struct app_state *st)
{
    int s;
    if (!st->port_name[0]) {
        s = select_port(st);
        if (s != 0) return s;
    }

    for (;;) {
        if (quick_probe(st) == 0) {
            printf("Connected successfully.\n");
            printf("Radio type: %s\n", model_name(st->model));
            printf("Radio ID: %s\n", st->ident);
            printf("Password: %s\n",password_display(st));
            print_memory_jogger(st);
            if (st->model == MODEL_UNKNOWN) {
                printf("This radio appears to use the TK-x4x programming protocol. "
                       "General backup/restore should still be usable.\n");
            }
            return 0;
        }
        invalidate_radio_connection(st);
        print_connection_failure(st);
        s = connection_failure_menu("connection");
        if (s == 'r') continue;
        if (s == 's') {
            if (select_port(st) != 0) return 1;
            continue;
        }
        if (s == 'd') {
            s = auto_detect_usb_port(st);
            if (s == 0) continue;
            if (s == 2 || g_main_menu_requested) return 1;
            continue;
        }
        if (s == 'm' || g_main_menu_requested) return 1;
        return 1;
    }
}

static void update_password_state(struct app_state *st, const struct radio_port *p)
{
    st->password_known = p->password_known;
    st->password_enabled = p->password_enabled;
    copy_string(st->password, sizeof(st->password), p->password);
}

static int open_program_session_retry(struct app_state *st, struct radio_port *p,
                                      char ident[8], const char *purpose)
{
    for (;;) {
        if (!st->port_name[0]) {
            if (g_cli_noninteractive) {
                cli_mark_needs_interaction("no serial port was supplied; use --port");
                return -1;
            }
            if (select_port(st) != 0) return -1;
        }
        if (port_open(p, st->port_name) == 0) {
            if (enter_program_mode(p, ident) == 0) {
                copy_string(st->ident, sizeof(st->ident), ident);
                st->model = model_from_ident(ident);
                update_password_state(st, p);
                st->connected = 1;
                return 0;
            }
            port_close(p);
        } else {
            int open_errno=errno;
            report_port_open_error(st->port_name);
            if(open_errno==EBUSY){
                invalidate_radio_connection(st);
                if(busy_port_retry_prompt(st->port_name))continue;
                return -1;
            }
        }
        invalidate_radio_connection(st);
        print_connection_failure(st);
        if (g_cli_noninteractive) {
            cli_mark_needs_interaction("radio connection failed; retry/port-selection decision required");
            return -1;
        }
        {
            int choice = connection_failure_menu(purpose ? purpose : "connection");
            if (choice == 'r') continue;
            if (choice == 's') {
                if (select_port(st) != 0) return -1;
                continue;
            }
            if (choice == 'd') {
                int detect = auto_detect_usb_port(st);
                if (detect == 0) continue;
                if (detect == 2 || g_main_menu_requested) return -1;
                continue;
            }
            return -1;
        }
    }
}

/* ------------------------------ Menu actions ----------------------------- */

static void update_preview_from_full_channels(struct app_state *st)
{
    enum model_family model = st->channels_model;
    unsigned sys;
    size_t count = 0;
    /* Intentional conservative fallback.  If the channel words do not identify
       a unique family, use known firmware/radio context so the preview can still
       show frequencies, and retain that hint in channels_model.  This can cause
       a spurious mismatch warning later, but never blocks a write: the user can
       explicitly continue.  Prefer that over hiding otherwise useful preview data. */
    if (model == MODEL_UNKNOWN && st->firmware_rev) model = st->firmware_rev->model;
    if (model == MODEL_UNKNOWN && st->model != MODEL_UNKNOWN) model = st->model;
    if (model == MODEL_UNKNOWN) model = infer_channel_model(st->channels);
    if (st->channels_model == MODEL_UNKNOWN) st->channels_model = model;
    memset(st->preview, 0, sizeof(st->preview));
    for (sys = 0; sys < SYSTEM_COUNT && count < PREVIEW_LINES; sys++) {
        uint16_t ptr = rd16le(st->channels + sys * 2);
        uint8_t groups = st->channels[GROUP_COUNT_BASE + sys];
        int off;
        unsigned g;
        if ((ptr & 0x8000) || !groups) continue;
        off = (int)(ptr & 0x3FFF) - (int)SYSTEM_DATA_PTR_BASE;
        if (off < 0 || off + CONV_HEADER_SIZE + (int)groups * (int)CONV_RECORD_SIZE > (int)CHANNEL_SIZE) continue;
        if (st->channels[off] & 1) continue;
        for (g = 0; g < groups && count < PREVIEW_LINES; g++) {
            const uint8_t *rec = st->channels + off + CONV_HEADER_SIZE + g * CONV_RECORD_SIZE;
            st->preview[count].valid = 1;
            st->preview[count].logical = (unsigned)count + 1;
            st->preview[count].rx_hz = decode_rx_word(model, rd16le(rec + CH_RX_FREQ));
            clean_name(rec + CH_NAME, st->preview[count].name);
            count++;
        }
    }
    st->preview_count = count;
}


static int action_read_firmware_radio(struct app_state *st)
{
    struct radio_port p;
    char ident[8];
    if (open_program_session_retry(st, &p, ident, "Firmware read") < 0) return -1;
    if (read_region(&p, PROGRAM_START, PROGRAM_SIZE, st->firmware, "firmware") < 0) {
        exit_program_mode(&p);
        port_close(&p);
        st->connected = 0;
        printf("Firmware read failed.\n");
        return -1;
    }
    sample_radio_preview(st, &p, NULL);
    exit_program_mode(&p);
    port_close(&p);
    st->firmware_loaded = 1;
    st->firmware_modified = 0;
    st->firmware_hex_family = 0;
    copy_string(st->firmware_source, sizeof(st->firmware_source), "Radio");
    classify_loaded_firmware(st);
    remember_loaded_firmware_on_radio(st);
    dev_set_full_baseline(st, st->firmware, "radio read");
    printf("Firmware successfully read from the radio.\n");
    print_memory_jogger(st);
    if(st->tot_test_10x)tot_test_10x_warning();
    printf("Checksum: %04X\nSHA-256: %s\n", st->firmware_sum, st->firmware_sha);
    print_firmware_similarity(st->firmware, st->firmware_sha);
    if (!st->firmware_rev) {
        printf("\nFirmware version not recognized by the toolkit.\n"
               "Radio type: %s\n"
               "Radio ID: %s\n", model_name(st->model), st->ident);
        if (st->model == MODEL_940 || st->model == MODEL_941) {
            char patch_status[80];
            format_patch_status(st,patch_status,sizeof(patch_status),0);
            printf("%s\n",patch_status);
        }
        print_unknown_firmware_guidance(st->model,1,st->firmware_sum);
    }
    return 0;
}

static int action_read_channels_radio(struct app_state *st)
{
    struct radio_port p;
    char ident[8];
    if (open_program_session_retry(st, &p, ident, "Channel read") < 0) return -1;
    if (read_region(&p, CHANNEL_START, CHANNEL_SIZE, st->channels, "channels") < 0) {
        exit_program_mode(&p);
        port_close(&p);
        st->connected = 0;
        printf("Channel read failed.\n");
        return -1;
    }
    exit_program_mode(&p);
    port_close(&p);
    st->channels_loaded = 1;
    copy_string(st->channels_source, sizeof(st->channels_source), "Radio");
    /* A successful radio read is authoritative; never leave a model inferred
       from previously loaded channel data attached to these new channels. */
    st->channels_model = st->model;
    update_preview_from_full_channels(st);
    radio_preview_from_loaded_channels(st);
    st->recovery_known = 1;
    st->recovery_enabled = !!(st->channels[RECOVERY_OFFSET] & RECOVERY_MASK);
    printf("Channels successfully read from the radio.\n");
    print_memory_jogger(st);
    return 0;
}


/* BIN and HEX remain separate actions; a conflicting typed extension prompts for format. */
/* The suggested name describes the image in memory, never a previous file
   or radio state. Only an exact stock SHA-256 is labeled STOCK. */
static void proposed_firmware_filename(const struct app_state *st,
                                        const char *extension,
                                        char out[MAX_PATH_LEN])
{
    const struct firmware_revision *rev = st->firmware_rev;
    enum model_family model = rev ? rev->model : st->model;
    char model_text[64], suffix[80] = "";
    unsigned checksum = rev ? rev->checksum : st->firmware_sum;
    int etx = rev && rev->model != MODEL_840 &&
              st->tx == TX_FIXED && st->dealer_tx == DEALER_TX_FIELD;
    int etot = st->tot == TOT_PATCHED;
    int stock = st->firmware_loaded &&
                find_full_revision(st->firmware_sha) != NULL;
    if (st->connected && st->model != MODEL_UNKNOWN &&
        !strcmp(st->firmware_source, "Radio") &&
        rev && ((st->model == MODEL_940 || st->model == MODEL_941) &&
                (rev->model == MODEL_940 || rev->model == MODEL_941)))
        model = st->model;
    copy_string(model_text, sizeof(model_text), model_name(model));
    sanitize_filename(model_text);
    if (etx) strcat(suffix, "_ETX");
    if (etot) strcat(suffix, "_ETOT");
    if (st->tot == TOT_LEGACY) strcat(suffix, "_LEGACYTOT");
    if (st->tot_test_10x) strcat(suffix, "_TEST10X");
    if (!suffix[0]) {
        if (stock) strcat(suffix, "_STOCK");
        else if (rev) strcat(suffix, "_MOD");
        else strcat(suffix, "_UNVERIFIED");
    }
    snprintf(out, MAX_PATH_LEN, "%s_FW_%04X%s.%s",
             model_text, checksum, suffix, extension);
}

/* File names are user decisions, not data-format validators. Binary and HEX
   writers both accept custom extensions after the user chooses a format. */
static const char *filename_extension(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *back = strrchr(path, '\\');
    const char *name = path;
    const char *dot;
    if (slash && slash >= name) name = slash + 1;
    if (back && back >= name) name = back + 1;
    dot = strrchr(name, '.');
    return dot ? dot : "";
}

/* 0 = BIN, 1 = HEX, 2 = edit filename, -1 = abort.
   The explicit filename extension, not the menu entry, supplies the default.
   Choosing the other format NEVER renames the file. */
static int choose_firmware_file_format(const char *path, int selected_hex)
{
    const char *ext = filename_extension(path);
    int typed_hex = ascii_casecmp(ext, ".hex") == 0;
    int typed_bin = ascii_casecmp(ext, ".bin") == 0;
    int typed_format = typed_hex ? 1 : typed_bin ? 0 : -1;
    if (typed_format < 0 || typed_format == selected_hex) return selected_hex;
    printf("\nFilename extension is %s, but %s output was selected.\n",
           typed_hex ? ".hex" : ".bin", selected_hex ? "HEX" : "BIN");
    for (;;) {
        int key;
        printf("Write as [H]EX or [B]IN? %s (ESC to edit filename): ",
               typed_hex ? "H/b" : "h/B");
        fflush(stdout);
        ui_flush_input();
        key = ui_read_key();
        if (key == UI_KEY_ENTER || key == UI_KEY_SPACE) return typed_format;
        if (tolower((unsigned char)key) == 'h') return 1;
        if (tolower((unsigned char)key) == 'b') return 0;
        if (key == UI_KEY_ESCAPE) return 2;
        if (key == UI_KEY_EOF) return -1;
    }
}

static int save_firmware_hex_to_path(struct app_state *st, const char *path)
{
    int family;
    uint8_t *encoded;
    size_t written = 0;
    int rc;
    if (!st->firmware_loaded) {
        printf("No firmware is loaded.\n");
        return -1;
    }
    family = st->firmware_hex_family;
    if (st->firmware_rev)
        family = st->firmware_rev->model == MODEL_840 ? 4 : 5;
    else if (!family) {
        if (st->model == MODEL_840) family = 4;
        if (st->model == MODEL_940 || st->model == MODEL_941) family = 5;
    }
    if (family != 4 && family != 5) {
        fprintf(stderr, "HEX export needs a known radio model to select its "
                        "Kenwood model-family trailer byte.\n");
        return -1;
    }
    encoded = (uint8_t *)malloc(KENWOOD_HEX_EXPORT_BYTES);
    if (!encoded) return -1;
    if (kenwood_hex_encode(st->firmware, family, encoded,
                           KENWOOD_HEX_EXPORT_BYTES, &written) != 0) {
        fprintf(stderr, "Unable to encode firmware as Kenwood HEX.\n");
        free(encoded);
        return -1;
    }
    rc = write_file_new_or_confirm(path, encoded, written, 0);
    free(encoded);
    if (rc < 0) return rc;
    printf("HEX firmware successfully written: %s\n", path);
    printf("Firmware checksum: %04X  Model-family byte: %02X\n",
           byte_sum16(st->firmware, PROGRAM_SIZE), family);
    return 0;
}

static int save_firmware_to_path(struct app_state *st, const char *path)
{
    if (!st->firmware_loaded) {
        printf("No firmware is loaded.\n");
        return -1;
    }
    {
        int rc = write_file_new_or_confirm(path, st->firmware, PROGRAM_SIZE, 0);
        if (rc < 0) return rc;
    }
    printf("BIN firmware successfully written: %s\n", path);
    return 0;
}


static int write_firmware_to_named_file(struct app_state *st,
                                        const char *input_path, int selected_hex)
{
    char path[MAX_PATH_LEN];
    copy_string(path, sizeof(path), input_path);
    for (;;) {
        int format = choose_firmware_file_format(path, selected_hex);
        if (format < 0) return 1;
        if (format == 2) {
            char edited[MAX_PATH_LEN];
            enum browse_kind kind = selected_hex ? BROWSE_HEX : BROWSE_FIRMWARE;
            if (make_save_path(st, kind, "Edit firmware filename",
                               path, edited) != 0)
                return 1;
            copy_string(path, sizeof(path), edited);
            continue;
        }
        printf("Writing %s firmware to: %s\n",
               format ? "HEX" : "BIN", path);
        return format ? save_firmware_hex_to_path(st, path)
                      : save_firmware_to_path(st, path);
    }
}

static int action_write_firmware_file(struct app_state *st, int selected_hex)
{
    char path[MAX_PATH_LEN], def[MAX_PATH_LEN];
    enum browse_kind kind = selected_hex ? BROWSE_HEX : BROWSE_FIRMWARE;
    if (!st->firmware_loaded) {
        printf("No firmware is loaded.\n");
        return -1;
    }
    proposed_firmware_filename(st, selected_hex ? "hex" : "bin", def);
    if (make_save_path(st, kind,
                       selected_hex ? "Write firmware to HEX file"
                                    : "Write firmware to BIN file",
                       def, path) != 0)
        return 1;
    return write_firmware_to_named_file(st, path, selected_hex);
}

static int action_save_firmware_hex(struct app_state *st)
{
    return action_write_firmware_file(st, 1);
}

static int save_channels_to_path(struct app_state *st, const char *path)
{
    if (!st->channels_loaded) {
        printf("No channels data is loaded.\n");
        return -1;
    }
    {
        int rc = write_file_new_or_confirm(path, st->channels, CHANNEL_SIZE, 0);
        if (rc < 0) return rc;
    }
    printf("Channel data successfully written: %s\n", path);
    return 0;
}

static int action_save_firmware_file(struct app_state *st)
{
    return action_write_firmware_file(st, 0);
}

static int action_save_channels_file(struct app_state *st)
{
    char path[MAX_PATH_LEN], def[MAX_PATH_LEN], model[32];
    if (!st->channels_loaded) {
        printf("No channels data is loaded.\n");
        return -1;
    }
    snprintf(model, sizeof(model), "%s",
             model_name(st->model != MODEL_UNKNOWN ? st->model : st->channels_model));
    sanitize_filename(model);
    snprintf(def, sizeof(def), "%s_CH.bin", model);
    if (make_save_path(st, BROWSE_CHANNELS, "Write channel data to file", def, path) != 0) return -1;
    return save_channels_to_path(st, path);
}

/* The TK-840 already honors programmed TX frequencies: only Enhanced TOT Patch
   applies (8355). TK-940/941 offer complete TX, AB00 TOT, or both. */
/* Only positively identified firmware revisions may be patched.
   The TX routine finder remains available for the four known 940/941
   layouts; the TOT patch uses exact bytes at fixed AB00/8355 addresses. */
static int choose_patch_selection(struct app_state *st,
                                  const struct firmware_revision *rev)
{
    int supports_tx,supports_tot;
    unsigned mask=0;
    if(!rev){
        printf("\nThis firmware revision is not recognized; no patches were applied.\n");
        /* File/radio loaders already printed the detailed unknown-revision
           explanation. Avoid repeating it when patching is refused. */
        if(st->model==MODEL_940 || st->model==MODEL_941)
            printf("Unknown firmware cannot be transmuted. Validate / Transmute Firmware\n"
                   "supports only recognized TK-940/941 B5EC, 8E9D, 2CCA and AB00.\n");
        if(st->model==MODEL_840)
            printf("Recommended: upgrade the TK-840 to firmware 8355, then patch.\n");
        else if(st->model==MODEL_940 || st->model==MODEL_941)
            printf("Recommended: upgrade the TK-940/941 to firmware AB00, then patch.\n");
        else
            printf("Recommended: TK-840 firmware 8355 or TK-940/941 firmware AB00.\n");
        return 0;
    }
    st->patch_model=rev->model;
    supports_tx=rev->model==MODEL_940 || rev->model==MODEL_941;
    supports_tot=tot_regions_for_revision(rev,NULL)!=NULL;
    if(st->patch_selection_explicit || g_cli_noninteractive){
        mask=st->patch_selection_explicit ? st->patch_selection :
             (supports_tx?PATCH_SELECT_TX:PATCH_SELECT_TOT);
    }else if(!supports_tx && supports_tot){
        printf("TK-840 %04X: selecting Enhanced TOT Patch.\n",rev->checksum);
        printf("Selected: Enhanced TOT\n");
        mask=PATCH_SELECT_TOT;
    }else if(!supports_tot && supports_tx){
        printf("Enhanced TOT Patch is available only on TK-940/941 AB00.\n");
        printf("Recommended: use Firmware Menu [T] Validate / Transmute\n"
               "in interactive mode to create stock AB00, then patch.\n");
        printf("Selected: Enhanced TX only\n");
        mask=PATCH_SELECT_TX;
    }else if(supports_tx && supports_tot){
        int k;
        for(;;){
            ui_flush_input();
            printf("\nSelect firmware patches:\n");
            printf("[B] Both: Enhanced TX Patch + Enhanced TOT Patch (recommended)\n");
            printf("[X] Enhanced TX Patch only\n");
            printf("[T] Enhanced TOT Patch only\n");
            printf("[A] Abort\n");
            printf("Select [B/X/T/A] (default B): ");
            fflush(stdout);
            k=ui_read_key();
            if(k==UI_KEY_ENTER || k==UI_KEY_SPACE ||
               tolower((unsigned char)k)=='b'){
                mask=PATCH_SELECT_BOTH;
                printf("\nSelected: Both patches\n");
                break;
            }
            if(tolower((unsigned char)k)=='x'){
                mask=PATCH_SELECT_TX;
                printf("\nSelected: Enhanced TX only\n");
                break;
            }
            if(tolower((unsigned char)k)=='t'){
                mask=PATCH_SELECT_TOT;
                printf("\nSelected: Enhanced TOT only\n");
                break;
            }
            if(k==UI_KEY_ESCAPE || tolower((unsigned char)k)=='a' ||
               tolower((unsigned char)k)=='q'){
                printf("\nSelected: Abort patch operation\n");
                return 0;
            }
        }
    }else{
        printf("No patches are available for this firmware revision.\n");
        return 0;
    }
    if((mask & PATCH_SELECT_TX) && !supports_tx){
        fprintf(stderr,"Enhanced TX Patch does not apply to TK-840.\n");
        return 0;
    }
    if((mask & PATCH_SELECT_TOT) && !supports_tot){
        fprintf(stderr,"Enhanced TOT Patch is not supported for %s.\n",rev->label);
        printf("Try Firmware Menu [T] Validate / Transmute Firmware to convert\n");
        printf("supported older revisions to stock AB00 or 8355, then patch.\n");
        return 0;
    }
    return (int)mask;
}

static int apply_selected_firmware_patches(uint8_t *img,
                                             const struct app_state *st, unsigned mask)
{
    const struct firmware_revision *rev=find_quick_revision(img);
    if(!rev){
        fprintf(stderr,"Firmware revision not recognized. No patches applied.\n");
        return -1;
    }
    if((mask & PATCH_SELECT_TX) && rev->model==MODEL_840){
        fprintf(stderr,"Enhanced TX Patch does not apply to TK-840.\n");
        return -1;
    }
    if((mask & PATCH_SELECT_TOT) && !tot_regions_for_revision(rev,NULL)){
        fprintf(stderr,"Enhanced TOT Patch requires TK-940/941 AB00 "
                       "or TK-840 8355/9306.\n");
        return -1;
    }
    if ((mask & PATCH_SELECT_TX) && apply_tx_fix(img, st) < 0) return -1;
    if ((mask & PATCH_SELECT_TOT) &&
        apply_tot_patch_for_model(img,st->patch_model) < 0)return -1;
    apply_project_markings(img, st);
    return 0;
}


/* Restore only known modifications, then demand the exact stock image hash.
   Nothing is changed in the loaded firmware unless verification succeeds. */
static int restore_tk840_stock(const struct app_state *st, uint8_t *out)
{
    const struct firmware_revision *rev = st->firmware_rev;
    const struct tot_patch_region *regions;
    enum tot_behavior tot;
    char sha[65];
    size_t count=0,i;
    if(!rev || rev->model!=MODEL_840 ||
       (rev->checksum!=0x8355 && rev->checksum!=0x9306))return -1;
    tot=tot_classify_for_revision(st->firmware,rev,MODEL_840,NULL);
    if(rev->checksum==0x9306){
        size_t off=TK840_9306_TOT_ADDR-PROGRAM_START;
        if(tot!=TOT_STOCK && tot!=TOT_LEGACY && tot!=TOT_PATCHED)return -1;
        memcpy(out,st->firmware,PROGRAM_SIZE);
        if(tot==TOT_LEGACY){
            out[off]=0x81;
            out[off+1u]=0x01;
        }else if(tot==TOT_PATCHED){
            regions=tot_regions_for_revision(rev,&count);
            if(!regions || count!=2u)return -1;
            for(i=0;i<count;i++)
                memcpy(out+(regions[i].address-PROGRAM_START),
                       regions[i].original,regions[i].length);
        }
        if(!memcmp(out+AUTHOR_SIG_OFFSET,AUTHOR_SIG_TEXT,AUTHOR_SIG_LEN))
            memset(out+AUTHOR_SIG_OFFSET,0xff,AUTHOR_SIG_LEN);
        if(patch_metadata_value(out[MOD_TAG_OFFSET],NULL)==1)
            out[MOD_TAG_OFFSET]=0xff;
        sha256_hex(out,PROGRAM_SIZE,sha);
        return !strcmp(sha,rev->full_sha)?0:-1;
    }
    if(tot!=TOT_STOCK && tot!=TOT_PATCHED && tot!=TOT_LEGACY)return -1;
    memcpy(out,st->firmware,PROGRAM_SIZE);
    if(tot==TOT_PATCHED || tot==TOT_LEGACY){
        regions=tot_regions_for_revision(rev,&count);
        if(!regions || count!=2u)return -1;
        for(i=0;i<count;i++)
            memcpy(out+(regions[i].address-PROGRAM_START),
                   regions[i].original,regions[i].length);
    }
    if(!memcmp(out+AUTHOR_SIG_OFFSET,AUTHOR_SIG_TEXT,AUTHOR_SIG_LEN))
        memset(out+AUTHOR_SIG_OFFSET,0xff,AUTHOR_SIG_LEN);
    if(patch_metadata_value(out[MOD_TAG_OFFSET],NULL)==1)
        out[MOD_TAG_OFFSET]=0xff;
    sha256_hex(out,PROGRAM_SIZE,sha);
    return !strcmp(sha,rev->full_sha)?0:-1;
}

/* The final removal must also erase our metadata. Insist on an exact
   known stock image, not merely the absence of recognized code patches. */
static int verify_patch_free_stock(const uint8_t *img,
                                   const struct firmware_revision *rev)
{
    char sha[65];
    if(!rev || !img)return -1;
    if(!memcmp(img+AUTHOR_SIG_OFFSET,AUTHOR_SIG_TEXT,AUTHOR_SIG_LEN) ||
       img[MOD_TAG_OFFSET]!=0xff)
        return -1;
    sha256_hex(img,PROGRAM_SIZE,sha);
    return !strcmp(sha,rev->full_sha)?0:-1;
}

static int restore_loaded_to_stock(const struct app_state *st, uint8_t *out)
{
    if(!st->firmware_rev)return -1;
    if(st->firmware_rev->model==MODEL_840)return restore_tk840_stock(st,out);
    return fwtransmute_restore_stock(st->firmware,PROGRAM_SIZE,out)==
           FWTRANSMUTE_OK?0:-1;
}

/* Remove every recognized firmware modification in one transaction. This
   includes Enhanced TX, Enhanced/legacy TOT, historical +25 MHz, and our
   patch metadata. Commit only when the full known stock SHA-256 matches.
   This restores the original revision, not a different firmware version. */
static int action_remove_all_patches(struct app_state *st)
{
    const struct firmware_revision *rev=st->firmware_rev;
    uint8_t stock[PROGRAM_SIZE];
    if(!st->firmware_loaded || !rev){
        printf("Load a recognized firmware image before removing patches.\n");
        return -1;
    }
    if(!strcmp(st->firmware_sha,rev->full_sha)){
        printf("Firmware is already stock. No changes made.\n");
        return 0;
    }
    if(restore_loaded_to_stock(st,stock)!=0 ||
       verify_patch_free_stock(stock,rev)!=0){
        printf("Cannot verify a complete stock restoration for %s. "
               "No changes made.\n",rev->label);
        return -1;
    }
    memcpy(st->firmware,stock,PROGRAM_SIZE);
    st->firmware_modified=1;
    st->patch_model=rev->model;
    classify_loaded_firmware(st);
    printf("Removed all recognized firmware patches in memory; radio unchanged.\n");
    printf("Restored: %s (stock)\n",rev->label);
    printf("Checksum: %04X\nSHA-256: %s\n",
           st->firmware_sum,st->firmware_sha);
    return 0;
}

/* Remove the historical fixed +25 MHz patch while preserving unrelated
   TX/TOT modifications. Check exact stock normalizability before and after
   editing, and never touch the loaded image if verification fails. */
static int action_remove_plus25(struct app_state *st)
{
    const struct firmware_revision *rev=st->firmware_rev;
    uint8_t copy[PROGRAM_SIZE];
    char sha[65];
    uint8_t before[PROGRAM_SIZE],after[PROGRAM_SIZE];
    if(!st->firmware_loaded || !rev || rev->model==MODEL_840 ||
       tkx4x_plus25_state(st->firmware,rev->checksum)!=TKX4X_PLUS25_PRESENT){
        printf("No fully recognized +25 MHz offset patch to remove.\n");
        return -1;
    }
    if(fwtransmute_restore_stock(st->firmware,PROGRAM_SIZE,before)!=FWTRANSMUTE_OK){
        printf("Cannot verify the original firmware. No changes made.\n");
        return -1;
    }
    memcpy(copy,st->firmware,PROGRAM_SIZE);
    if(tkx4x_plus25_restore(copy,rev->checksum)<0)return -1;
    if(fwtransmute_restore_stock(copy,PROGRAM_SIZE,after)!=FWTRANSMUTE_OK ||
       memcmp(before,after,PROGRAM_SIZE)){
        printf("Firmware verification failed. No changes made.\n");
        return -1;
    }
    memcpy(st->firmware,copy,PROGRAM_SIZE);
    st->firmware_modified=1;
    classify_loaded_firmware(st);
    sha256_hex(st->firmware,PROGRAM_SIZE,sha);
    printf("Removed +25 MHz Offset Patch in memory; radio unchanged.\n");
    printf("Checksum: %04X\nSHA-256: %s\n",st->firmware_sum,sha);
    return 0;
}

/* Toggle one patch in a copy. When removing it, reconstruct a hash-verified
   stock revision and reapply any independent patch that must remain. */
static int action_toggle_loaded_patch(struct app_state *st,unsigned toggle)
{
    uint8_t copy[PROGRAM_SIZE],stock[PROGRAM_SIZE];
    const struct firmware_revision *rev=st->firmware_rev;
    int has_tx,has_tot,remove,keep_legacy=0,keep_test=0;
    int plus25;
    unsigned keep=0;
    enum tot_behavior tot;
    if(!st->firmware_loaded || !rev){
        printf("Load a recognized firmware image before editing patches.\n");
        return -1;
    }
    st->patch_model=rev->model;
    if(toggle==PATCH_SELECT_TX && rev->model==MODEL_840)return -1;
    if(toggle==PATCH_SELECT_TOT && !tot_regions_for_revision(rev,NULL))
        return -1;
    has_tx=st->tx==TX_FIXED && st->dealer_tx==DEALER_TX_FIELD;
    tot=classify_tot_firmware_for_model(st->firmware,rev->model);
    has_tot=tot==TOT_PATCHED || tot==TOT_LEGACY;
    plus25=rev->model!=MODEL_840 ?
        tkx4x_plus25_state(st->firmware,rev->checksum) :
        TKX4X_PLUS25_UNKNOWN;
    remove=(toggle==PATCH_SELECT_TX)?has_tx:has_tot;
    if(remove){
        if(restore_loaded_to_stock(st,stock)!=0){
            printf("Cannot verify the original stock firmware. No changes made.\n");
            return -1;
        }
        memcpy(copy,stock,PROGRAM_SIZE);
        if(toggle!=PATCH_SELECT_TX && has_tx)keep|=PATCH_SELECT_TX;
        if(toggle!=PATCH_SELECT_TOT && tot==TOT_PATCHED){
            keep|=PATCH_SELECT_TOT;
            keep_test=st->tot_test_10x;
        }
        if(toggle!=PATCH_SELECT_TOT && tot==TOT_LEGACY)keep_legacy=1;
        if(keep && apply_selected_firmware_patches(copy,st,keep)<0)return -1;
        if(keep_test){
            const struct tot_patch_region *regions;
            size_t count=0,i;
            regions=tot_regions_for_revision(rev,&count);
            if(!regions || count<2u)return -1;
            for(i=0;i<2u;i++){
                size_t off=regions[i].address-PROGRAM_START+(i==0u?37u:11u);
                copy[off]=st->firmware[off];
            }
            if(!tot_test_10x_matches(copy,rev))return -1;
        }
        if(keep_legacy){
            size_t off=ab00_tot_regions[1].address-PROGRAM_START;
            copy[off+20u]=0;
            copy[off+21u]=0;
            if(!tot_legacy_for_revision(copy,rev))return -1;
        }
        /* Removing TOT must not silently discard the independent +25 MHz
           offset modification. Preserve its exact recognized site bytes. */
        if(toggle==PATCH_SELECT_TOT && plus25==TKX4X_PLUS25_PRESENT &&
           tkx4x_plus25_reapply(copy,st->firmware,rev->checksum)<0){
            printf("Cannot preserve the +25 MHz offset patch. No changes made.\n");
            return -1;
        }
        if(!keep && !keep_legacy &&
           !(toggle==PATCH_SELECT_TOT && plus25==TKX4X_PLUS25_PRESENT) &&
           verify_patch_free_stock(copy,rev)<0){
            printf("Stock image or firmware metadata verification failed. No changes made.\n");
            return -1;
        }
    }else{
        memcpy(copy,st->firmware,PROGRAM_SIZE);
        if(apply_selected_firmware_patches(copy,st,toggle)<0)return -1;
    }
    if(!memcmp(copy,st->firmware,PROGRAM_SIZE)){
        printf("Firmware image unchanged.\n");
        return 0;
    }
    if(toggle==PATCH_SELECT_TX){
        int result=classify_tx_full(copy,NULL)==TX_FIXED &&
                   classify_dealer_tx_full(copy,NULL)==DEALER_TX_FIELD;
        if(result==remove){
            printf("Enhanced TX Patch verification failed. No changes made.\n");
            return -1;
        }
    }else{
        enum tot_behavior result=classify_tot_firmware_for_model(copy,rev->model);
        if(remove ? result!=TOT_STOCK : result!=TOT_PATCHED){
            printf("Enhanced TOT Patch verification failed. No changes made.\n");
            return -1;
        }
    }
    memcpy(st->firmware,copy,PROGRAM_SIZE);
    st->firmware_modified=1;
    classify_loaded_firmware(st);
    if(toggle==PATCH_SELECT_TOT && remove)
        printf("Removed %s in memory; radio unchanged.\n",
               tot==TOT_LEGACY?"Legacy TOT Bypass":"Enhanced TOT Patch");
    else
        printf("%s Enhanced %s Patch in memory; radio unchanged.\n",
               remove?"Removed":"Applied",toggle==PATCH_SELECT_TX?"TX":"TOT");
    printf("Checksum: %04X\nSHA-256: %s\n",
           st->firmware_sum,st->firmware_sha);
    return 0;
}

static int action_patch_loaded(struct app_state *st)
{
    uint8_t copy[PROGRAM_SIZE];
    enum tx_behavior before_tx;
    enum dealer_tx_behavior before_dealer;
    enum tot_behavior before_tot;
    int selection;
    if (!st->firmware_loaded) {
        printf("Read firmware from the radio or a file first.\n");
        return -1;
    }
    selection = choose_patch_selection(st, st->firmware_rev);
    if (!selection) return 1;
    before_tx = classify_tx_full(st->firmware, NULL);
    before_dealer = classify_dealer_tx_full(st->firmware, NULL);
    before_tot = classify_tot_firmware(st->firmware);
    memcpy(copy, st->firmware, PROGRAM_SIZE);
    if (apply_selected_firmware_patches(copy, st, (unsigned)selection) < 0) return -1;
    if (!memcmp(copy, st->firmware, PROGRAM_SIZE)) {
        printf("Selected firmware patches are already present. Nothing changed.\n");
        return 0;
    }
    memcpy(st->firmware, copy, PROGRAM_SIZE);
    st->firmware_modified = 1;
    classify_loaded_firmware(st);
    if((selection & PATCH_SELECT_TOT) && before_tot!=TOT_PATCHED)
        printf("Enhanced TOT Patch applied to the firmware in memory.\n");
    if ((selection & PATCH_SELECT_TX) && before_tx == TX_FIXED && before_dealer == DEALER_TX_FIELD)
        printf("The Enhanced TX Patch was already present. Other selected changes were applied in memory.\n");
    else
        printf("Selected firmware patch(es) applied in memory. They have not yet been written to the radio.\n");
    printf("Checksum: %04X\nSHA-256: %s\n", st->firmware_sum, st->firmware_sha);
    return 0;
}

static int action_dump_maskrom(struct app_state *st)
{
    struct radio_port p;
    char ident[8], sha[65], model[32], leaf[192], path[MAX_PATH_LEN];
    uint8_t *rom;
    int rc = -1;

    printf("Mask-ROM dump: read-only access to CPU addresses %04X-%04X (%u bytes).\n",
           MASKROM_START, MASKROM_END, (unsigned)MASKROM_SIZE);
    printf("Normal PROGRAM mode can read this internal 8 KiB mask-ROM region.\n");
    if (!ask_yes_no("Dump mask ROM?", 1)) return 1;

    rom = (uint8_t *)malloc(MASKROM_SIZE);
    if (!rom) {
        perror("malloc");
        return -1;
    }

    if (open_program_session_retry(st, &p, ident, "Mask ROM dump") < 0)
        goto out;

    if (read_region(&p, MASKROM_START, MASKROM_SIZE, rom, "mask ROM") < 0) {
        int saved_errno = errno;
        exit_program_mode(&p);
        port_close(&p);
        errno = saved_errno;
        fprintf(stderr, "Mask-ROM read failed while reading %04X-%04X.\n",
                MASKROM_START, MASKROM_END);
        goto out;
    }
    exit_program_mode(&p);
    port_close(&p);

    sha256_hex(rom, MASKROM_SIZE, sha);
    snprintf(model, sizeof(model), "%s", model_name(st->model));
    sanitize_filename(model);
    if (make_dir_if_needed(st->backup_dir) < 0) {
        perror(st->backup_dir);
        goto out;
    }
    snprintf(leaf, sizeof(leaf), "%s_maskrom_%u_%.12s.bin",
             model, (unsigned)MASKROM_SIZE, sha);
    if (path_join(path, st->backup_dir, leaf) < 0) {
        fprintf(stderr, "Mask-ROM output path is too long.\n");
        goto out;
    }
    if (write_file_new_or_confirm(path, rom, MASKROM_SIZE, 1) < 0)
        goto out;

    printf("Mask-ROM dump saved: %s\n", path);
    printf("Address range: %04X-%04X\n", MASKROM_START, MASKROM_END);
    printf("Size: %u bytes\n", (unsigned)MASKROM_SIZE);
    printf("SHA-256: %s\n", sha);
    rc = 0;

out:
    free(rom);
    return rc;
}

static int action_write_channels_radio(struct app_state *st)
{
    struct radio_port p;
    char ident[8];
    if (!st->channels_loaded) {
        printf("No channels data is loaded.\n");
        return -1;
    }
    if (open_program_session_retry(st, &p, ident, "Channel write") < 0) return -1;
    if (confirm_channel_model_mismatch(st, st->channels_model) < 0) {
        exit_program_mode(&p);
        port_close(&p);
        return 1;
    }
    if (write_channels_in_session(&p, st->channels, 1) < 0) {
        exit_program_mode(&p);
        port_close(&p);
        return -1;
    }
    exit_program_mode(&p);
    port_close(&p);
    st->channels[RECOVERY_OFFSET] |= RECOVERY_MASK;
    st->recovery_known = 1;
    st->recovery_enabled = 1;
    return 0;
}

static int action_write_firmware_radio_mode(struct app_state *st, int no_sparse,
                                            const uint8_t *known_current,
                                            const char *expected_ident)
{
    int did_write=1;
    if (!st->firmware_loaded) {
        printf("No firmware is loaded.\n");
        return -1;
    }
    if(tot_test_10x_confirm_write(st->firmware))return 1;
    classify_loaded_firmware(st);
    if (no_sparse) {
        int rc = full_firmware_recovery(st, st->firmware);
        if (rc != 0) return rc;
    } else {
        int rc = program_firmware_normal(st, st->firmware, known_current,
                                         expected_ident, &did_write);
        if (rc != 0) return rc;
    }
    st->firmware_modified = 0;
    classify_loaded_firmware(st);
    remember_loaded_firmware_on_radio(st);
    dev_set_full_baseline(st, st->firmware, "last normal firmware write");
    if(did_write)printf("Firmware successfully written to the radio.\n");
    return 0;
}

static int action_write_firmware_radio(struct app_state *st)
{
    if (st->development_firmware_mode) {
        int rc;
        if (!st->firmware_loaded) {
            printf("No firmware is loaded.\n");
            return -1;
        }
        if(tot_test_10x_confirm_write(st->firmware))return 1;
        classify_loaded_firmware(st);
        rc = development_firmware_write(st, st->firmware);
        if (rc == 0) {
            st->firmware_modified = 0;
            classify_loaded_firmware(st);
            /* Sparse development writes don't establish a complete radio
               fingerprint. Reprobe instead of claiming the file is installed. */
            forget_radio_firmware_observation(st);
        }
        return rc;
    }
    return action_write_firmware_radio_mode(st, !st->compare_before_firmware_write,
                                            NULL, NULL);
}

static int action_full_restore_path(struct app_state *st, const char *requested_path)
{
    char path[MAX_PATH_LEN], ident[8], base_name[MAX_SOURCE_LEN];
    uint8_t *d = NULL, *changed = NULL;
    uint8_t current_fw[PROGRAM_SIZE];
    size_t n, changed_pages = 0, pages = PROGRAM_SIZE / FLASH_PAGE_SIZE;
    struct radio_port p;
    int rc = -1;

    if (requested_path && requested_path[0]) {
        copy_string(path, sizeof(path), requested_path);
    } else if (browse_for_file(st, BROWSE_FULL, "Select full backup to restore", path) != 0) return -1;
    if (read_file_sized(path, &d, &n) < 0) return -1;
    if (n != FULL_SIZE) {
        fprintf(stderr, "The backup file has an invalid size and cannot be restored.\n");
        goto out;
    }
    if (memcmp(d + FW_KENWOOD_OFFSET, FW_KENWOOD_TEXT, FW_KENWOOD_LEN)) {
        fprintf(stderr, "The backup contains firmware that does not appear to be valid for this radio.\n");
        goto out;
    }
    basename_copy(path, base_name, sizeof(base_name));
    if(tot_test_10x_confirm_write(d)){
        rc=1;
        goto out;
    }
    changed = (uint8_t *)calloc(pages, 1);
    if (!changed) goto out;

    if (open_program_session_retry(st, &p, ident, "Full restore") < 0) goto out;
    if (check_firmware_target_model(st, d, "full restore") < 0) {
        exit_program_mode(&p);
        port_close(&p);
        goto out;
    }
    {
        enum model_family restore_channels_model = infer_channel_model(d + PROGRAM_SIZE);
        /* Do not change the in-memory channel state until the restore has
           actually written the new channel data successfully. */
        if (confirm_channel_model_mismatch(st, restore_channels_model) < 0) {
            exit_program_mode(&p);
            port_close(&p);
            rc = 1;
            goto out;
        }
    }

    printf("Restoring channels first...\n");
    if (write_channels_in_session(&p, d + PROGRAM_SIZE, 1) < 0) {
        exit_program_mode(&p);
        port_close(&p);
        goto out;
    }

    if (st->compare_before_firmware_write) {
        printf("Reading installed firmware so unchanged pages can be skipped.\n");
        if (read_region(&p, PROGRAM_START, PROGRAM_SIZE, current_fw, "firmware") < 0) {
            exit_program_mode(&p);
            port_close(&p);
            goto out;
        }
        if (prepare_firmware_in_session(&p, current_fw, d, changed,
                                        &changed_pages) < 0) {
            exit_program_mode(&p);
            port_close(&p);
            goto out;
        }
    } else {
        memset(changed, 1, pages);
        changed_pages = pages;
    }
    exit_program_mode(&p);
    port_close(&p);

    memcpy(st->channels, d + PROGRAM_SIZE, CHANNEL_SIZE);
    st->channels[RECOVERY_OFFSET] |= RECOVERY_MASK;
    st->channels_loaded = 1;
    st->channels_model = infer_channel_model(st->channels);
    copy_string(st->channels_source, sizeof(st->channels_source), base_name);
    update_preview_from_full_channels(st);
    st->recovery_known = 1;
    st->recovery_enabled = 1;

    {
        int fwrc = stream_prepared_firmware(st, d, changed, changed_pages);
        if (fwrc != 0) {
            rc = fwrc;
            goto out;
        }
    }

    memcpy(st->firmware, d, PROGRAM_SIZE);
    st->firmware_loaded = 1;
    st->firmware_modified = 0;
    copy_string(st->firmware_source, sizeof(st->firmware_source), base_name);
    classify_loaded_firmware(st);
    remember_loaded_firmware_on_radio(st);
    dev_set_full_baseline(st, st->firmware, "full restore");

    printf("Full restore completed successfully.\n");
    rc = 0;
out:
    free(changed);
    free(d);
    return rc;
}

static int action_full_restore(struct app_state *st)
{
    return action_full_restore_path(st, NULL);
}

/* In-memory firmware transmutation. The user must separately save, patch,
   and write the result. Failed verification never changes the loaded image. */
static void transmute_pause(void)
{
    printf("\nPress any key to return to the Firmware Menu.\n");
    fflush(stdout);
    ui_flush_input();
    (void)ui_read_key();
}

static void transmute_9306_guidance(void)
{
    printf("\nRestored to stock TK-840 9306 (exact SHA-256 match).\n");
    printf("The recognized legacy modification was removed and stock firmware verified.\n");
    printf("Save the image with [S] before writing it to a radio.\n");
    printf("Enhanced TOT is also available on 9306 (hardware-tested).\n");
    printf("No firmware has been written to the radio.\n");
}

static void transmute_stock_guidance(int tk840)
{
    printf("\nThis is a verified, unmodified stock firmware image.\n");
    if(tk840)
        printf("The recommended Enhanced TOT Patch has not been applied.\n");
    else
        printf("The recommended Enhanced TX and Enhanced TOT patches\n"
               "have not been applied.\n");
    printf("\nRecommended next steps:\n");
    printf("  1. [S] Save this stock firmware to a file for archival.\n");
    if(tk840)
        printf("  2. [P] Apply the Enhanced TOT Patch.\n");
    else
        printf("  2. [P] Apply both recommended firmware patches.\n");
    printf("  3. [W] Write the patched firmware to the radio.\n");
    printf("No firmware has been written to the radio.\n");
}

static int action_transmute_firmware(struct app_state *st)
{
    int tk840;
    char path[MAX_PATH_LEN];

    if(!st->firmware_loaded){
        int choice;
        ui_flush_input();
        printf("\nNo firmware loaded. Load firmware now? [n/Y] ");
        fflush(stdout);
        choice=ui_read_key();
        printf("\n");
        if(choice!=UI_KEY_ENTER && choice!=UI_KEY_SPACE &&
           tolower((unsigned char)choice)!='y'){
            printf("Selected: Return to Firmware Menu\n");
            return 1;
        }
        printf("Selected: Load firmware from file\n");
        if(browse_for_file(st,BROWSE_FIRMWARE,"Select firmware file",path)!=0 ||
           load_firmware_file(st,path)!=0)
            return 1;
    }

    /* Identify a loaded file by its own recognized firmware revision,
       never by an unrelated currently connected radio. */
    tk840=st->firmware_rev && st->firmware_rev->model==MODEL_840;
    printf("\n          FIRMWARE VALIDATION / TRANSMUTATION\n\n");
    if(tk840){
        uint8_t converted[PROGRAM_SIZE];
        int is9306=st->firmware_rev &&
                   st->firmware_rev->model==MODEL_840 &&
                   st->firmware_rev->checksum==0x9306;
        printf("Target firmware: TK-840 8355 (stock)\n");
        if(is9306){
            char check[65];
            int rc=fwtransmute_to_8355(st->firmware,PROGRAM_SIZE,converted);
            if(rc!=FWTRANSMUTE_OK){
                printf("9306 firmware could not be normalized and converted safely.\n");
                printf("Unknown modifications are not supported.\n");
                printf("No changes were made to the loaded image.\n");
                transmute_pause();
                return -1;
            }
            sha256_hex(converted,PROGRAM_SIZE,check);
            if(strcmp(check,
               "7B061DC79AD69475484D68340EDD872BD5E1A8F67EFEB27BFFBE3812A0EE7258")){
                printf("8355 output failed independent SHA-256 verification.\n");
                printf("No changes were made to the loaded image.\n");
                transmute_pause();
                return -1;
            }
            printf("Source: recognized TK-840 9306 (patches normalized).\n");
            printf("Target: verified stock TK-840 8355.\n");
            printf("Source and target full-image SHA-256 checks passed.\n");
            printf("Only the loaded image in memory will change.\n");
            printf("Save your original 9306 firmware before conversion.\n");
            if(!ask_yes_no("Transmute loaded 9306 to stock 8355?",1)){
                printf("Selected: Abort transmutation. Image unchanged.\n");
                transmute_pause();
                return 1;
            }
            printf("Selected: Transmute TK-840 9306 to stock 8355\n");
            memcpy(st->firmware,converted,PROGRAM_SIZE);
            st->firmware_modified=1;
            st->patch_model=MODEL_840;
            snprintf(st->firmware_source,sizeof(st->firmware_source),
                     "Transmuted from 9306");
            classify_loaded_firmware(st);
            printf("Converted to TK-840 8355 (stock).\n");
            printf("Checksum: %04X\nSHA-256: %s\n",
                   st->firmware_sum,st->firmware_sha);
            transmute_stock_guidance(1);
            transmute_pause();
            return 0;
        }
        if(restore_tk840_stock(st,converted)<0){
            printf("This image cannot be restored to a recognized stock TK-840 revision.\n");
            printf("Unknown revisions or modifications are not supported.\n");
            printf("No changes were made to the loaded firmware.\n");
            transmute_pause();
            return -1;
        }
        if(!memcmp(st->firmware,converted,PROGRAM_SIZE)){
            printf("Firmware is already stock %s.\n",is9306?"9306":"8355");
            if(is9306)transmute_9306_guidance();
            else transmute_stock_guidance(1);
            transmute_pause();
            return 0;
        }
        printf("Recognized patches removed. Stock SHA-256 verified.\n");
        printf("Only the firmware image in memory will change.\n");
        if(!ask_yes_no(is9306?"Restore loaded firmware to stock 9306?":
                       "Restore loaded firmware to stock 8355?",1)){
            printf("Selected: Abort restoration. Image unchanged.\n");
            transmute_pause();
            return 1;
        }
        memcpy(st->firmware,converted,PROGRAM_SIZE);
        st->firmware_modified=1;
        st->patch_model=MODEL_840;
        classify_loaded_firmware(st);
        printf("Restored to stock %s (exact SHA-256 match).\n",
               is9306?"9306":"8355");
        printf("Checksum: %04X\nSHA-256: %s\n",
               st->firmware_sum,st->firmware_sha);
        if(is9306)transmute_9306_guidance();
        else transmute_stock_guidance(1);
        transmute_pause();
        return 0;
    }

    {
        struct fwtransmute_result info;
        uint8_t *converted=(uint8_t*)malloc(PROGRAM_SIZE);
        char check[65];
        int rc;
        if(!converted){
            fprintf(stderr,"Out of memory during firmware transmutation.\n");
            return -1;
        }
        rc=fwtransmute_to_ab00(st->firmware,PROGRAM_SIZE,converted,&info);
        if(rc!=FWTRANSMUTE_OK){
            if(rc==FWTRANSMUTE_UNSUPPORTED){
                printf("This firmware is not supported for verified conversion.\n");
                printf("Unrecognized or partially applied patches cannot be normalized.\n");
                print_unknown_firmware_guidance(MODEL_UNKNOWN,1,st->firmware_sum);
            }else{
                printf("Firmware conversion failed verification or another safety check.\n");
            }
            printf("No changes were made to the loaded firmware.\n");
            free(converted);
            transmute_pause();
            return -1;
        }
        printf("Current firmware: TK-940/941 %s\n",info.source_revision);
        printf("Target firmware:  TK-940/941 AB00 (stock)\n");
        if(info.was_stock_ab00){
            printf("Firmware is already verified stock AB00. No conversion necessary.\n");
            transmute_stock_guidance(0);
            free(converted);
            transmute_pause();
            return 0;
        }
        printf("\nRecognized patches were normalized to verified stock firmware.\n");
        if(info.removed_tx)printf("  Removed: conventional TX fix\n");
        if(info.removed_dealer)printf("  Removed: dealer/alignment TX fix\n");
        if(info.removed_plus25)printf("  Removed: +25 MHz offsets\n");
        if(info.removed_tot==1)printf("  Removed: Enhanced TOT Patch\n");
        if(info.removed_tot==2)printf("  Removed: legacy unconditional TOT bypass\n");
        if(info.removed_padding)printf("  Restored: unused firmware areas and metadata\n");
        printf("Source and AB00 destination SHA-256 verification passed.\n");
        printf("Only the image in memory will be replaced; not the radio.\n");
        printf("Consider saving the original image before conversion.\n");

        sha256_hex(converted,PROGRAM_SIZE,check);
        if(strcmp(check,
           "45E9E8CFB4070FD14A9C6339F97CD19140FAF4ACF145DC043A9E78C3E8757BCF")){
            fprintf(stderr,"Firmware conversion output failed final SHA-256 validation.\n");
            free(converted);
            transmute_pause();
            return -1;
        }
        if(!ask_yes_no(info.stages ?
           "Transmute loaded firmware to stock AB00?" :
           "Normalize loaded firmware to stock AB00?",1)){
            printf("Selected: Abort transmutation. Loaded firmware unchanged.\n");
            free(converted);
            transmute_pause();
            return 1;
        }
        printf("Selected: Replace loaded firmware with verified stock AB00\n");
        memcpy(st->firmware,converted,PROGRAM_SIZE);
        free(converted);
        st->firmware_modified=1;
        st->patch_model=MODEL_941;
        snprintf(st->firmware_source,sizeof(st->firmware_source),
                 "Transmuted from %s",info.source_revision);
        classify_loaded_firmware(st);
        printf("\nFirmware transmutation successful!\n");
        printf("Original firmware: %s\n",info.source_revision);
        printf("Current firmware:  AB00 (stock)\n");
        printf("Checksum: %04X\nSHA-256: %s\n",st->firmware_sum,st->firmware_sha);
        transmute_stock_guidance(0);
        transmute_pause();
        return 0;
    }
}


/* ------------------------ Guided firmware workflow ----------------------- */

/* The guided path makes the recommended choices, but never writes a radio
   until the operator explicitly selects that destination. Individual manual
   firmware operations remain in the Firmware Menu. */
static int guided_prepare_firmware(struct app_state *st)
{
    uint8_t stock[PROGRAM_SIZE], target[PROGRAM_SIZE];
    const struct firmware_revision *original, *final_rev;
    unsigned patches;
    int removed_modifications, converted = 0;
    char original_label[64];

    if (!st->firmware_loaded || !st->firmware_rev) {
        printf("Guided workflow requires a recognized full firmware image.\n");
        printf("Unknown firmware or modifications cannot be normalized safely.\n");
        return -1;
    }
    original = st->firmware_rev;
    copy_string(original_label, sizeof(original_label), original->label);

    if (restore_loaded_to_stock(st, stock) < 0 ||
        verify_patch_free_stock(stock, original) < 0) {
        printf("Cannot reconstruct and verify stock %s. Stopping safely.\n",
               original_label);
        printf("The original loaded firmware has not been modified.\n");
        return -1;
    }
    removed_modifications = memcmp(stock, st->firmware, PROGRAM_SIZE) != 0;
    memcpy(target, stock, PROGRAM_SIZE);

    if (original->model == MODEL_840 && original->checksum == 0x9306) {
        if (fwtransmute_to_8355(stock, PROGRAM_SIZE, target) != FWTRANSMUTE_OK) {
            printf("Verified conversion from 9306 to 8355 failed. Stopping.\n");
            return -1;
        }
        converted = 1;
    } else if (original->model == MODEL_940 || original->model == MODEL_941) {
        struct fwtransmute_result info;
        if (fwtransmute_to_ab00(stock, PROGRAM_SIZE, target, &info) !=
            FWTRANSMUTE_OK) {
            printf("Verified conversion to AB00 failed. Stopping safely.\n");
            return -1;
        }
        converted = original->checksum != 0xAB00;
    } else if (original->model != MODEL_840 ||
               original->checksum != 0x8355) {
        printf("This firmware revision is not supported by the guided workflow.\n");
        return -1;
    }

    final_rev = find_quick_revision(target);
    if (!final_rev ||
        final_rev->checksum != (original->model == MODEL_840 ? 0x8355 : 0xAB00) ||
        verify_patch_free_stock(target, final_rev) < 0) {
        printf("Prepared stock firmware did not match its trusted SHA-256.\n");
        printf("The original loaded firmware has not been modified.\n");
        return -1;
    }

    patches = final_rev->model == MODEL_840 ?
        PATCH_SELECT_TOT : PATCH_SELECT_BOTH;
    st->patch_model = final_rev->model;
    if (apply_selected_firmware_patches(target, st, patches) < 0 ||
        find_quick_revision(target) != final_rev ||
        classify_tot_firmware_for_model(target, final_rev->model) != TOT_PATCHED ||
        (patches & PATCH_SELECT_TX &&
         (classify_tx_full(target, NULL) != TX_FIXED ||
          classify_dealer_tx_full(target, NULL) != DEALER_TX_FIELD))) {
        printf("Prepared patches failed verification. Stopping safely.\n");
        printf("The original loaded firmware has not been modified.\n");
        return -1;
    }

    printf("\nGUIDED FIRMWARE PREPARATION\n");
    printf("Original: %s\n", original_label);
    if (removed_modifications)
        printf("Removed recognized modifications; restored SHA-256-verified stock.\n");
    else
        printf("Original firmware verified as stock.\n");
    if (converted)
        printf("Transmuted to verified stock %04X.\n", final_rev->checksum);
    else
        printf("Firmware revision retained: %04X.\n", final_rev->checksum);
    printf("Applied: %s\n", patches == PATCH_SELECT_TOT ?
           "Enhanced TOT Patch" : "Enhanced TX Patch + Enhanced TOT Patch");

    removed_modifications = memcmp(target, st->firmware, PROGRAM_SIZE) != 0;
    memcpy(st->firmware, target, PROGRAM_SIZE);
    st->firmware_modified = removed_modifications;
    st->firmware_hex_family = final_rev->model == MODEL_840 ? 4 : 5;
    classify_loaded_firmware(st);
    printf("Prepared: %s\n", st->firmware_rev ?
           st->firmware_rev->label : "Unknown");
    printf("Checksum: %04X\nSHA-256: %s\n",
           st->firmware_sum, st->firmware_sha);
    printf("The radio has not been changed.\n");
    return 0;
}

static int guided_backup_choice(void)
{
    for (;;) {
        int k;
        printf("\nSave a backup of the ORIGINAL radio firmware?\n");
        printf("[C] Firmware + channels (recommended)\n");
        printf("[F] Firmware only\n");
        printf("[N] No backup\n");
        printf("[A] Abort workflow\n");
        printf("Select [C/F/N/A] (default C): ");
        fflush(stdout);
        ui_flush_input();
        k = ui_read_key();
        if (k == UI_KEY_ENTER || k == UI_KEY_SPACE ||
            tolower((unsigned char)k) == 'c') {
            printf("\nSelected: Backup firmware + channels\n");
            return 'c';
        }
        if (tolower((unsigned char)k) == 'f') {
            printf("\nSelected: Backup firmware only\n");
            return 'f';
        }
        if (tolower((unsigned char)k) == 'n') {
            printf("\nSelected: No backup\n");
            return 'n';
        }
        if (k == UI_KEY_ESCAPE || k == UI_KEY_EOF ||
            tolower((unsigned char)k) == 'a' ||
            tolower((unsigned char)k) == 'q') {
            printf("\nSelected: Abort workflow\n");
            return 'a';
        }
    }
}

/* Read the target firmware without replacing the prepared file image.
   Do not rely on a quick fingerprint when choosing the radio to flash. */
static int guided_read_radio_original(struct app_state *st,
                                      uint8_t original[PROGRAM_SIZE],
                                      char ident[8])
{
    struct radio_port p;
    const struct firmware_revision *rev;
    char hash[65];
    if (open_program_session_retry(st, &p, ident, "Guided firmware read") < 0)
        return -1;
    /* This is a new full read; do not report a prior radio's loader state. */
    st->recovery_known = 0;
    if (read_region(&p, PROGRAM_START, PROGRAM_SIZE, original, "firmware") < 0) {
        exit_program_mode(&p);
        port_close(&p);
        printf("Firmware read failed. No changes have been made to the radio.\n");
        return -1;
    }
    sample_radio_preview(st, &p, NULL);
    exit_program_mode(&p);
    port_close(&p);
    print_memory_jogger(st);
    rev = identify_firmware_image(original, hash);
    if (!rev) {
        printf("The radio contains unrecognized firmware. Stopping safely.\n");
        return -1;
    }
    st->quick_rev = rev;
    st->radio_fingerprinted = 1;
    st->radio_full_read = 1;
    st->radio_tx = classify_tx_full(original, NULL);
    st->radio_dealer_tx = classify_dealer_tx_full(original, NULL);
    st->radio_tot = classify_tot_firmware_for_model(original, rev->model);
    printf("Radio firmware read: %s (full image).\n", rev->label);
    return 0;
}

/* A backup must never replace the prepared image in memory. Channel data is
   read only when the user requests a combined firmware + channels backup. */
static int guided_backup_radio_original(struct app_state *st,
                                         const uint8_t firmware[PROGRAM_SIZE],
                                         const char ident[8])
{
    int choice = guided_backup_choice();
    char path[MAX_PATH_LEN];
    uint8_t *combined = NULL;
    int rc;
    if (choice == 'a') return 1;
    if (choice == 'n') {
        printf("Continuing without a radio backup.\n");
        return 0;
    }
    if (backup_destination_writable(st) < 0) {
        fprintf(stderr, "Cannot use backup directory: %s\n", st->backup_dir);
        return -1;
    }
    if (choice == 'c') {
        struct radio_port p;
        char reconnected_id[8];
        combined = (uint8_t *)malloc(FULL_SIZE);
        if (!combined) return -1;
        if (open_program_session_retry(st, &p, reconnected_id,
                                       "Guided channel backup") < 0) {
            free(combined);
            return -1;
        }
        /* The full original firmware is already available in memory.
           Check the radio's identification response and read only channels. */
        if (strcmp(reconnected_id, ident) ||
            read_region(&p, CHANNEL_START, CHANNEL_SIZE,
                        combined + PROGRAM_SIZE, "channels") < 0) {
            exit_program_mode(&p);
            port_close(&p);
            free(combined);
            printf("The radio changed or channel backup failed. Stopping safely.\n");
            return -1;
        }
        exit_program_mode(&p);
        port_close(&p);
        memcpy(combined, firmware, PROGRAM_SIZE);
        make_auto_backup_name_suffix(st, "full", path);
        rc = write_file_new_or_confirm(path, combined, FULL_SIZE, 1);
        free(combined);
    } else {
        make_auto_backup_name_suffix(st, "firmware", path);
        rc = write_file_new_or_confirm(path, firmware, PROGRAM_SIZE, 1);
    }
    if (rc != 0) {
        printf("Original radio backup could not be saved. Stopping safely.\n");
        return -1;
    }
    printf("Original radio backup saved: %s\n", path);
    return 0;
}

static int guided_choose_file_source(struct app_state *st)
{
    char path[MAX_PATH_LEN];
    for (;;) {
        int k;
        printf("\nSelect a firmware file format:\n");
        printf("[B] BIN (recommended)\n");
        printf("[H] HEX (original Kenwood software compatibility)\n");
        printf("[A] Abort workflow\n");
        fflush(stdout);
        ui_flush_input();
        k = ui_read_key();
        if (k == UI_KEY_ENTER || k == UI_KEY_SPACE ||
            tolower((unsigned char)k) == 'b') {
            printf("\nSelected: Read BIN firmware file\n");
            if (browse_for_file(st, BROWSE_FIRMWARE,
                                "Select binary firmware or backup", path) != 0)
                return 1;
            {
                int rc;
                st->guided_loading = 1;
                rc = load_firmware_file(st, path);
                st->guided_loading = 0;
                return rc;
            }
        }
        if (tolower((unsigned char)k) == 'h') {
            printf("\nSelected: Read HEX firmware file\n");
            if (browse_for_file(st, BROWSE_HEX,
                                "Select Kenwood HEX firmware", path) != 0)
                return 1;
            {
                int rc;
                st->guided_loading = 1;
                rc = load_hex_firmware_file(st, path);
                st->guided_loading = 0;
                return rc;
            }
        }
        if (k == UI_KEY_ESCAPE || k == UI_KEY_EOF ||
            tolower((unsigned char)k) == 'a' ||
            tolower((unsigned char)k) == 'q')
            return 1;
    }
}

static int guided_save_prepared_file(struct app_state *st)
{
    for (;;) {
        int k;
        printf("\nWrite Prepared Firmware to File:\n");
        printf("[B] Write to BIN File (recommended)\n");
        printf("[H] Write to HEX File (original Kenwood software)\n");
        printf("[A] Back to prepared firmware\n");
        fflush(stdout);
        ui_flush_input();
        k = ui_read_key();
        if (k == UI_KEY_ENTER || k == UI_KEY_SPACE ||
            tolower((unsigned char)k) == 'b') {
            printf("\nSelected: Write prepared firmware to BIN file\n");
            return action_save_firmware_file(st);
        }
        if (tolower((unsigned char)k) == 'h') {
            printf("\nSelected: Write prepared firmware to HEX file\n");
            return action_save_firmware_hex(st);
        }
        if (k == UI_KEY_ESCAPE || k == UI_KEY_EOF ||
            tolower((unsigned char)k) == 'a' ||
            tolower((unsigned char)k) == 'q')
            return 1;
    }
}

static int guided_target_compatible(const struct app_state *st)
{
    const struct firmware_revision *rev = st->firmware_rev;
    if (!rev || st->model == MODEL_UNKNOWN) return 0;
    return rev->model == st->model ||
        ((rev->model == MODEL_940 || rev->model == MODEL_941) &&
         (st->model == MODEL_940 || st->model == MODEL_941));
}

static int action_guided_firmware(struct app_state *st)
{
    uint8_t *original = NULL, *prewrite = NULL;
    char original_id[8] = "";
    int from_radio = 0, saved = 0, written = 0, already_current = 0;
    int prepared_modified = 0, rc = 1;

    printf("\nGUIDED FIRMWARE WORKFLOW\n");
    printf("Prepare recommended firmware and patches; save to file or write to radio.\n\n");
    printf("[R] Read firmware from radio\n");
    printf("    Prepare, then save to file or write to radio.\n");
    printf("[F] Read firmware from file\n");
    printf("    Prepare, then save to file or write to radio.\n");
    printf("[B] Back to Firmware Menu\n");
    for (;;) {
        int k;
        printf("Select [R/F/B]: ");
        fflush(stdout);
        ui_flush_input();
        k = ui_read_key();
        if (tolower((unsigned char)k) == 'r') {
            from_radio = 1;
            printf("\nSelected: Read firmware from radio\n");
            break;
        }
        if (tolower((unsigned char)k) == 'f') {
            printf("\nSelected: Read firmware from file\n");
            break;
        }
        if (k == UI_KEY_ENTER || k == UI_KEY_SPACE ||
            k == UI_KEY_ESCAPE || k == UI_KEY_EOF ||
            tolower((unsigned char)k) == 'b' ||
            tolower((unsigned char)k) == 'q') return 1;
    }

    original = (uint8_t *)malloc(PROGRAM_SIZE);
    prewrite = (uint8_t *)malloc(PROGRAM_SIZE);
    if (!original || !prewrite) {
        fprintf(stderr, "Out of memory starting guided workflow.\n");
        rc = -1;
        goto done;
    }
    if (from_radio) {
        if (action_read_firmware_radio(st) != 0) goto done;
        memcpy(original, st->firmware, PROGRAM_SIZE);
        copy_string(original_id, sizeof(original_id), st->ident);
        if (!st->firmware_rev) {
            printf("Unrecognized firmware; guided workflow stopped safely.\n");
            goto done;
        }
    } else {
        if (guided_choose_file_source(st) != 0) goto done;
        if (!st->firmware_loaded || !st->firmware_rev) {
            printf("Unrecognized firmware; guided workflow stopped safely.\n");
            goto done;
        }
        memcpy(original, st->firmware, PROGRAM_SIZE);
    }
    if (guided_prepare_firmware(st) != 0) goto done;
    prepared_modified = st->firmware_modified;
    if (from_radio && !prepared_modified) {
        already_current = 1;
        printf("\nThe firmware in your radio is already at the recommended\n");
        printf("revision, with the recommended patches applied.\n");
        printf("No firmware update is needed.\n");
    } else if (from_radio &&
               guided_backup_radio_original(st, original, original_id) != 0) {
        goto done;
    }

    for (;;) {
        int k;
        printf("\nFIRMWARE READY\n");
        printf("Source: %s\n", from_radio ? "Radio" : st->firmware_source);
        printf("Prepared: %s\n",
               st->firmware_rev ? st->firmware_rev->label : "Unknown");
        printf("Recommended patches: %s\n",
               st->firmware_rev && st->firmware_rev->model == MODEL_840 ?
               "Enhanced TOT" : "Enhanced TX + Enhanced TOT");
        printf("[S] Write Prepared Firmware to File%s\n",
               saved ? " (saved)" : "");
        if (!written && !already_current)
            printf("[W] Write Prepared Firmware to Radio\n");
        printf("%s\n", written || already_current ?
               "[Q] Finish" : "[Q] Quit without writing to radio");
        printf("Select [%s]: ", written || already_current ? "S/Q" : "S/W/Q");
        fflush(stdout);
        ui_flush_input();
        k = ui_read_key();
        if (tolower((unsigned char)k) == 's') {
            printf("\nSelected: Write Prepared Firmware to File\n");
            if (guided_save_prepared_file(st) == 0) saved = 1;
            continue;
        }
        if (k == UI_KEY_ENTER || k == UI_KEY_SPACE ||
            k == UI_KEY_ESCAPE || k == UI_KEY_EOF ||
            tolower((unsigned char)k) == 'q' ||
            tolower((unsigned char)k) == 'a') {
            printf("\nSelected: Quit Guided Firmware Workflow\n");
            rc = 0;
            goto done;
        }
        if (tolower((unsigned char)k) != 'w' || written || already_current) continue;
        printf("\nSelected: Write Prepared Firmware to Radio\n");

        /* A radio source has already been read in full. File sources
           require one full read of the target radio before writing. */
        if (!from_radio &&
            guided_read_radio_original(st, prewrite, original_id) != 0) {
            rc = -1;
            goto done;
        }
        if (!guided_target_compatible(st)) {
            printf("Prepared firmware does not match the identified radio model.\n");
            printf("Guided workflow will not override a model mismatch.\n");
            rc = -1;
            goto done;
        }
        /* Compare against the full snapshot already read, not another firmware
           download. Reconnection still checks the radio identification. */
        const uint8_t *current = from_radio ? original : prewrite;
        if (!memcmp(current, st->firmware, PROGRAM_SIZE)) {
            printf("\nThe firmware in your radio is already at the recommended\n");
            printf("revision, with the recommended patches applied.\n");
            printf("No firmware update is needed.\n");
            already_current = 1;
            rc = 0;
            if (!saved) continue;
            goto done;
        }
        /* Only offer a backup when an actual firmware update is required.
           With a file source, save the radio's original firmware, not the file. */
        if (!from_radio &&
            guided_backup_radio_original(st, prewrite, original_id) != 0) {
            rc = 1;
            goto done;
        }
        /* Guided updates must not accidentally use development or full
           recovery mode selected earlier in Advanced / Special Options. */
        if (action_write_firmware_radio_mode(st, 0, current, original_id) != 0) {
            printf("Firmware write did not complete successfully.\n");
            rc = -1;
            goto done;
        }
        st->firmware_modified = prepared_modified;
        written = 1;
        printf("\nGuided firmware update successfully written and verified.\n");
        if (!saved) {
            printf("You can still save the prepared firmware to a file.\n");
            continue;
        }
        rc = 0;
        goto done;
    }
done:
    free(original);
    free(prewrite);
    return rc;
}

/* ------------------------------- Submenus -------------------------------- */

struct status_screen_ctx {
    const struct app_state *st;
    const char *title;
};

static void draw_status_screen(void *vctx)
{
    const struct status_screen_ctx *ctx = (const struct status_screen_ctx *)vctx;
    print_status(ctx->st);
    printf("%-*.*s | INFO\n", STATUS_LEFT_WIDTH, STATUS_LEFT_WIDTH, ctx->title);
}


struct patch_screen_ctx {
    const struct app_state *st;
    int supports_tx;
    int supports_tot;
    int supports_legacy;
    int has_plus25;
    int has_any_patch;
};

static int patch_tx_applied(const struct app_state *st)
{
    return st->tx==TX_FIXED && st->dealer_tx==DEALER_TX_FIELD;
}

static const char *patch_tx_status(const struct app_state *st)
{
    if(patch_tx_applied(st))return "Applied";
    if(st->tx==TX_FIXED_OFFSET && st->dealer_tx==DEALER_TX_FIXED_OFFSET)
        return "Not applied";
    return "Partial / unknown";
}

static const char *patch_tot_status(const struct app_state *st)
{
    if(st->tot==TOT_PATCHED)return "Applied";
    if(st->tot==TOT_STOCK)return "Not applied";
    if(st->tot==TOT_LEGACY)return "Legacy bypass applied";
    return "Partial / unknown";
}

/* Show useful guidance in the menu itself, including the Back-only screen.
   Do not suggest that an unknown revision can necessarily be transmuted. */
static void print_no_supported_patch_guidance(const struct app_state *st)
{
    const struct firmware_revision *rev=st->firmware_rev;
    enum model_family model=rev?rev->model:st->model;
    printf("No supported patches for this firmware version.\n");
    if(model==MODEL_840){
        printf("To apply Enhanced TOT, upgrade to TK-840 firmware 8355.\n");

    }else if(model==MODEL_940 || model==MODEL_941){
        printf("For Enhanced TX/TOT, use TK-940/941 firmware AB00.\n");
        printf("Transmute supported older revisions, or upgrade firmware.\n");
        if(!rev)
            printf("Unknown revisions cannot be transmuted automatically.\n");
    }else{
        printf("Use TK-840 8355 or TK-940/941 AB00 firmware.\n");
        printf("Transmute only recognized compatible firmware revisions.\n");
    }
}

static void draw_patch_screen(void *vctx)
{
    const struct patch_screen_ctx *ctx=(const struct patch_screen_ctx *)vctx;
    print_status(ctx->st);
    printf("Firmware patch status:\n");
    if(ctx->supports_tx)
        printf("  Enhanced TX:  %s\n",patch_tx_status(ctx->st));
    if(ctx->supports_tot || ctx->supports_legacy)
        printf("  TOT Patch:     %s\n",patch_tot_status(ctx->st));
    if(ctx->has_plus25)
        printf("  +25 MHz offset: Applied\n");
    if(!ctx->st->firmware_loaded){
        printf("No firmware loaded. Read or load firmware before changing patches.\n");
        printf("Select [I] for an explanation of the available patches.\n");
    }else if(!ctx->supports_tx && !ctx->supports_tot &&
             !ctx->has_any_patch && !ctx->has_plus25 &&
             !(ctx->supports_legacy && ctx->st->tot==TOT_LEGACY)){
        print_no_supported_patch_guidance(ctx->st);
    }
    printf("\n%-*.*s | INFO\n",STATUS_LEFT_WIDTH,STATUS_LEFT_WIDTH,
           "FIRMWARE PATCHES");
}

/* Dedicated in-tool explanation. No firmware is required to read it.
   Split into terminal-sized pages so the first section stays visible even
   at the minimum supported 80x24 terminal size. */
static void firmware_patch_help_screen(void)
{
    static const char *const lines[]={
        "FIRMWARE PATCHES - HELP / INFO",
        "",
        "HISTORICAL PATCHES (recognized for removal, not offered as new patches)",
        "  +25 MHz fixed TX offset (TK-940/941): substitutes another fixed offset.",
        "    It still ignores the channel's programmed TX-frequency field.",
        "  Legacy TOT bypass: NOPs skip the normal timeout action, but the",
        "    countdown continues. Other timeout paths may still be active.",
        "",
        "ENHANCED PATCHES",
        "  Enhanced TX (TK-940/941): uses each conventional channel's TX field.",
        "    Talk Around then controls the TA icon, not TX frequency.",
        "    Reprogram and verify TX frequencies before transmitting!",
        "    Conventional TX starts at 851 MHz (NOT TK-940's 806-825 MHz band).",
        "  Enhanced TOT: Adds Off to the normal transmit timeout settings.",
        "    The existing 15-second through 10-minute settings are unchanged,",
        "    and Telephone TOT is unaffected.",
        "    Without this patch, that same setting gives a 60-second timeout.",
        "    Select Off with CHIRP; Kenwood factory software cannot select it.",
        "",
        "FIRMWARE SUPPORT / TEST STATUS",
        "  TK-840 8355: Enhanced TOT (hardware-tested).",
        "  TK-840 9306: Enhanced TOT (hardware-tested).",
        "  TK-940/941 AB00: Enhanced TX and TOT (hardware-tested).",
        "  Older supported TK-940/941: Enhanced TX only; transmute for TOT.",
        "",
        "RECOMMENDED: Firmware Menu -> [G] Guided Firmware Workflow",
        "  Reads and validates firmware; offers a backup before radio writes.",
        "  Upgrades known older firmware and applies recommended patches.",
        "  For manual control, use [T] Validate / Transmute and [P] Patches.",
        "  Nothing is flashed until you explicitly choose Write to Radio.",
        "  Unknown modifications are refused. Keep an original backup."
    };
    const size_t count=sizeof(lines)/sizeof(lines[0]);
    size_t first=0,per_page;
    int cols=80,rows=24;

    get_terminal_size(&cols,&rows);
    (void)cols;
    per_page=rows>7 ? (size_t)(rows-2) : 22u;
    if(per_page<12u)per_page=12u;
    while(first<count){
        size_t i,last=first+per_page;
        int key;
        if(last>count)last=count;
        /* Leave the existing screen and terminal scrollback untouched.
           Additional pages follow naturally instead of clearing the screen. */
        fputc('\n',stdout);
        for(i=first;i<last;i++)printf("%s\n",lines[i]);
        printf("\n%s\n",last<count ?
               "Press any key for more, or B to return to Firmware Patches." :
               "Press any key to return to Firmware Patches.");
        fflush(stdout);
        ui_flush_input();
        key=ui_read_key();
        if(key==UI_KEY_EOF || key==UI_KEY_ESCAPE ||
           (last<count && (key=='b' || key=='B')))return;
        first=last;
    }
}

/* Name the exact installed TOT modification, not a generic patch. */
static const char *tot_patch_action_label(enum tot_behavior tot)
{
    if(tot==TOT_PATCHED)return "Remove Enhanced TOT Patch";
    if(tot==TOT_LEGACY)return "Remove Legacy TOT Bypass";
    return "Apply Enhanced TOT Patch";
}

static void firmware_patch_menu(struct app_state *st)
{
    size_t selected=0;
    /* Help must remain available before loading firmware. Patch actions
       are only offered when an image of a supported revision is loaded. */
    if(st->firmware_loaded && !st->firmware_rev)
        printf("Firmware revision is not recognized.\n");
    st->patch_model=st->firmware_rev ?
        st->firmware_rev->model : st->model;
    for(;;){
        struct ui_item items[6];
        struct patch_screen_ctx ctx;
        size_t count=0;
        int choice;
        ctx.st=st;
        ctx.supports_tx=st->firmware_rev &&
                        st->firmware_rev->model!=MODEL_840;
        ctx.supports_tot=tot_regions_for_revision(st->firmware_rev,NULL)!=NULL;
        ctx.supports_legacy=st->firmware_rev &&
                            st->firmware_rev->model==MODEL_840 &&
                            st->firmware_rev->checksum==0x9306;
        ctx.has_plus25=st->firmware_rev &&
                        st->firmware_rev->model!=MODEL_840 &&
                        tkx4x_plus25_state(st->firmware,
                            st->firmware_rev->checksum)==TKX4X_PLUS25_PRESENT;
        ctx.has_any_patch=st->firmware_rev &&
                          (st->tot==TOT_PATCHED || st->tot==TOT_LEGACY ||
                           st->tx==TX_FIXED || st->dealer_tx==DEALER_TX_FIELD ||
                           ctx.has_plus25 || st->chirp_tag_present ||
                           !memcmp(st->firmware+AUTHOR_SIG_OFFSET,
                                   AUTHOR_SIG_TEXT,AUTHOR_SIG_LEN));
        if(ctx.supports_tx)
            items[count++]=(struct ui_item){'X',
                patch_tx_applied(st)?"Remove Enhanced TX Patch":"Apply Enhanced TX Patch",
                NULL,"Toggle the Enhanced TX Patch in the loaded image."};
        /* One toggle for any recognized TOT variant. The 9306 recipe is
           revision-specific and hardware-tested. */
        if(ctx.supports_tot || (ctx.supports_legacy && st->tot==TOT_LEGACY))
            items[count++]=(struct ui_item){'T',
                tot_patch_action_label(st->tot),
                NULL,"Remove a recognized TOT modification or apply Enhanced TOT."};
        if(ctx.has_plus25)
            items[count++]=(struct ui_item){'O',"Remove +25 MHz Offset Patch",
                NULL,"Restore the original fixed-offset instructions."};
        if(ctx.has_any_patch)
            items[count++]=(struct ui_item){'A',"Remove All Patches",
                NULL,"Restore exact stock firmware, removing every recognized patch."};
        items[count++]=(struct ui_item){'I',"Patch Help / Info",NULL,
            "Explain legacy and Enhanced patches, compatibility, removal, and safe use."};
        items[count++]=(struct ui_item){'B',"Back",NULL,
            "Return to the Firmware Menu."};
        if(selected>=count)selected=count-1u;
        choice=ui_menu(items,count,&selected,draw_patch_screen,&ctx);
        if(choice==UI_KEY_EOF || choice==UI_KEY_ESCAPE || choice=='b' ||
           g_main_menu_requested || g_quit_requested)return;
        if(choice=='q'){g_quit_requested=1;return;}
        if(choice=='x')(void)action_toggle_loaded_patch(st,PATCH_SELECT_TX);
        else if(choice=='t')(void)action_toggle_loaded_patch(st,PATCH_SELECT_TOT);
        else if(choice=='o')(void)action_remove_plus25(st);
        else if(choice=='a')(void)action_remove_all_patches(st);
        else if(choice=='i'){
            printf("Selected: Patch Help / Info\n");
            firmware_patch_help_screen();
        }
        ui_flush_input();
        if(g_quit_requested || g_main_menu_requested)return;
    }
}

static void firmware_menu(struct app_state *st)
{
    static const struct ui_item items[] = {
        {'G', "Guided Firmware Workflow", NULL,
         "Prepare recommended firmware and patches from radio or file, then save to file or write to radio."},
        {'R', "Read from Radio", NULL,
         "Read the complete firmware from the radio into memory."},
        {'F', "Read from BIN File", NULL,
         "Read a binary firmware image, regardless of its filename extension."},
        {'H', "Read from HEX File", NULL,
         "Read a Kenwood Intel HEX firmware file; validate records and checksums."},
        {'W', "Write to Radio", NULL,
         "Program the loaded firmware with confirmation and readback verification."},
        {'S', "Write to BIN File", NULL,
         "Write raw binary firmware. The proposed .bin filename reflects the model, revision and patches, and is editable."},
        {'X', "Write to HEX File", NULL,
         "Write Kenwood Intel HEX for original Kenwood software. The proposed .hex filename is editable."},
        {'P', "Firmware Patches", NULL,
         "Apply or remove supported patches from the loaded firmware."},
        {'T', "Validate / Transmute Firmware", NULL,
         "Validate against known stock firmware, remove recognized patches, or upgrade supported older revisions. Changes only the loaded image, not the radio."},
        {'A', "Advanced / Special Options", NULL,
         "Open advanced firmware-writing, recovery, signature, and backup options."},
        {'B', "Back", NULL,
         "Return to the Main Menu."},
        {'Q', "Exit", NULL,
         "Exit TK-x4x Toolkit."}
    };
    size_t selected = 0;
    char path[MAX_PATH_LEN];
    for (;;) {
        struct status_screen_ctx ctx = {st, "FIRMWARE MENU"};
        int choice = ui_menu(items, sizeof(items) / sizeof(items[0]), &selected,
                             draw_status_screen, &ctx);
        if (choice == 'q') { g_quit_requested = 1; return; }
        if (choice == UI_KEY_EOF || choice == UI_KEY_ESCAPE || choice == 'b' || g_main_menu_requested) return;
        switch (choice) {
        case 'g':
            (void)action_guided_firmware(st);
            break;
        case 'r':
            (void)action_read_firmware_radio(st);
            break;
        case 'f':
            if (browse_for_file(st, BROWSE_FIRMWARE, "Select firmware file", path) == 0)
                (void)load_firmware_file(st, path);
            break;
        case 'h':
            if (browse_for_file(st, BROWSE_HEX, "Select Intel HEX file", path) == 0 &&
                load_hex_firmware_file(st, path) != 0)
                transmute_pause(); /* Keep the error visible before redrawing. */
            break;
        case 'w':
            (void)action_write_firmware_radio(st);
            break;
        case 's':
            (void)action_save_firmware_file(st);
            break;
        case 'x':
            (void)action_save_firmware_hex(st);
            break;
        case 'p':
            firmware_patch_menu(st);
            break;
        case 't':
            (void)action_transmute_firmware(st);
            break;
        case 'a':
            advanced_menu(st);
            break;
        default:
            break;
        }
        ui_flush_input();
        if (g_quit_requested || g_main_menu_requested) return;
    }
}

static void channels_menu(struct app_state *st)
{
    static const struct ui_item items[] = {
        {'R', "Read from Radio", NULL,
         "Read all channel information from the radio into memory."},
        {'F', "Read from File", NULL,         "Read raw channel data from a file into memory (6,144 bytes)."},
        {'W', "Write to Radio", NULL,
         "Write the currently loaded channel information to the radio."},
        {'S', "Write to File", NULL,
         "Write channel data to a binary file. Defaults to a model-specific .bin filename; it can be edited."},
        {'A', "Advanced / Special Options", NULL,
         "Open shared firmware, backup, and development settings."},
        {'B', "Back", NULL,
         "Return to the Main Menu."},
        {'Q', "Exit", NULL,
         "Exit TK-x4x Toolkit."}
    };
    size_t selected = 0;
    char path[MAX_PATH_LEN];
    for (;;) {
        struct status_screen_ctx ctx = {st, "CHANNELS MENU"};
        int choice = ui_menu(items, sizeof(items) / sizeof(items[0]), &selected,
                             draw_status_screen, &ctx);
        if (choice == 'q') { g_quit_requested = 1; return; }
        if (choice == UI_KEY_EOF || choice == UI_KEY_ESCAPE || choice == 'b' || g_main_menu_requested) return;
        switch (choice) {
        case 'r':
            (void)action_read_channels_radio(st);
            break;
        case 'f':
            if (browse_for_file(st, BROWSE_CHANNELS, "Select channels file", path) == 0)
                (void)load_channels_file(st, path);
            break;
        case 'w':
            (void)action_write_channels_radio(st);
            break;
        case 's':
            (void)action_save_channels_file(st);
            break;
        case 'a':
            advanced_menu(st);
            break;
        default:
            break;
        }
        ui_flush_input();
        if (g_quit_requested || g_main_menu_requested) return;
    }
}

static void advanced_menu(struct app_state *st)
{
    size_t selected = 0;
    char path[MAX_PATH_LEN];
    for (;;) {
        struct ui_item items[] = {
            {'D', "Backup directory", st->backup_dir,
             "Set the directory for backups, ROM dumps, and the development baseline."},
            {'W', "Firmware write mode", st->compare_before_firmware_write ? "(sparse write)" : "(full write)",
             "Toggle between full firmware writes and changed-page comparison."},
            {'V', "Development firmware mode", st->development_firmware_mode ? "(On)" : "(Off)",
             "Toggle fast firmware writes without reading the installed firmware first."},
            {'K', "Set development baseline", st->dev_baseline_source[0] ? st->dev_baseline_source : "(none)",
             "Select a firmware file matching what is currently installed in the radio."},
            {'U', "Forget development baseline", dev_known_page_count(st) ? "(baseline known)" : "(none)",
             "Delete the remembered baseline, including its saved file. Does not change the radio."},
            {'S', "Include optional KY4WI signature", st->include_author_signature ? "(Yes)" : "(No)",
             "Optional: write the KY4WI FW MODS signature into firmware."},
            {'C', "Include firmware feature flags", st->include_modified_tag ? "(Yes)" : "(No)",
             "Recommended: identifies enhanced TX capability to CHIRP."},
            {'F', "Firmware loader full write", "(complete firmware)",
             "Program the entire firmware image using the resident ROM loader."},
            {'O', "Dump mask ROM", "(read-only, 8 KiB)",
             "For developers only. Dump the CPU's 8 KiB internal ROM for analysis or annotated disassembly with 78k_disasm. Not needed for normal radio use."},
            {'B', "Back", NULL,
             "Return to the menu that opened Advanced / Special Options."},
            {'M', "Main Menu", NULL,
             "Return directly to the Main Menu."},
            {'Q', "Exit", NULL,
             "Exit TK-x4x Toolkit."}
        };
        struct status_screen_ctx ctx = {st, "ADVANCED / SPECIAL OPTIONS"};
        int choice = ui_menu(items, sizeof(items) / sizeof(items[0]), &selected,
                             draw_status_screen, &ctx);
        if (choice == 'q') { g_quit_requested = 1; return; }
        if (choice == UI_KEY_ESCAPE || choice == 'b') return;
        if (choice == 'm') {
            g_main_menu_requested = 1;
            return;
        }
        if (choice == UI_KEY_EOF || g_main_menu_requested) return;
        switch (choice) {
        case 'd':
            prompt_string("Backup directory", st->backup_dir, path, sizeof(path));
            if (path[0]) copy_string(st->backup_dir, sizeof(st->backup_dir), path);
            break;
        case 'w':
            st->compare_before_firmware_write = !st->compare_before_firmware_write;
            break;
        case 'v':
            st->development_firmware_mode = !st->development_firmware_mode;
            break;
        case 'k':
            (void)action_set_dev_baseline(st);
            break;
        case 'u':
            dev_forget_persisted_baseline(st);
            dev_clear_baseline(st);
            printf("Development baseline forgotten (including saved baseline file).\n");
            break;
        case 's':
            st->include_author_signature = !st->include_author_signature;
            break;
        case 'c':
            st->include_modified_tag = !st->include_modified_tag;
            break;
        case 'o':
            (void)action_dump_maskrom(st);
            break;
        case 'f':
            if (!st->firmware_loaded) {
                if (browse_for_file(st, BROWSE_FIRMWARE,
                                    "Select firmware for firmware-loader full write", path) != 0)
                    break;
                if (load_firmware_file(st, path) < 0) break;
            }
            if (ask_yes_no("Use the currently loaded firmware for a full firmware-loader write?", 1) &&
                !tot_test_10x_confirm_write(st->firmware)){
                classify_loaded_firmware(st);
                (void)full_firmware_recovery(st, st->firmware);
            }
            break;
        default:
            break;
        }
        ui_flush_input();
        if (g_quit_requested || g_main_menu_requested) return;
    }
}

/* ------------------------------- Help / Info ----------------------------- */

static void help_screen(int wait_for_return)
{
    if (wait_for_return) {
        if (g_ansi) {
            fputs("\033[2J\033[H", stdout);
        } else {
            int cols = 80, rows = 24, i;
            get_terminal_size(&cols, &rows);
            (void)cols;
            for (i = 0; i < rows + 2; i++) fputc('\n', stdout);
        }
    }

    printf("Kenwood TK-x4x Toolkit - by Dave, KY4WI\n");
    printf("Project: https://github.com/KY4WI/Kenwood_TK-x4x_Toolkit\n");
    printf("\n");
    printf("This project started as a CHIRP driver for the TK-840. It grew through\n");
    printf("tk840download, tk840upload, and tk941patch, and eventually became\n");
    printf("TK-x4x Toolkit. Because these radios use the same programming techniques, I\n");
    printf("can support more radios than the ones I own.\n");
    printf("\n");
    printf("Supported radios:\n");
    printf("  TK-840\n");
    printf("  TK-940 and TK-941\n");
    printf("  TK-840N: firmware and channel backup/restore ONLY\n");
    printf("  No TK-840N patches, transmutation, Guided Workflow, CHIRP\n");
    printf("  channel programming, or other operations are supported.\n");
    printf("TK-940 and TK-941 are identical from a programming standpoint. Their\n");
    printf("differences are hardware-only and determine usable frequencies.\n");
    printf("\n");
    printf("The KY4WI Enhanced Firmware Patches:\n");
    printf("  Enhanced TX Patch: The TK-940/941 channel data has a TX field\n");
    printf("  ignored by stock firmware. The patch enables that field.\n");
    printf("  Enhanced TOT Patch: Adds Off to the normal 15-second through\n");
    printf("  10-minute timeout options without changing existing settings.\n");
    printf("  Off is value 255 (FF), unavailable in factory programming software.\n");
    printf("  Set Off in CHIRP's global timer settings.\n");
    printf("  Stock firmware treats 255 as a 60-second timeout, not Off.\n");
    printf("TK-840 firmware 8355/9306 supports Enhanced TOT. TK-940/941 AB00\n");
    printf("supports both, which are recommended together. Older supported\n");
    printf("TK-940/941 firmware supports Enhanced TX. Guided Firmware Workflow\n");
    printf("can convert recognized older versions to AB00 and add both patches.\n");
    printf("Unknown firmware revisions cannot be patched.\n");
    printf("After enabling Enhanced TX, check all programmed channel TX\n");
    printf("frequencies before transmitting. Old workarounds may need changing.\n");
    printf("The optional KY4WI FW MODS signature can be left out. Keeping\n");
    printf("the patch metadata byte is recommended for consistent reporting.\n");
    printf("TK-840 8355/9306 Enhanced TOT passed conventional-mode hardware testing.\n");
    printf("TK-940/941 AB00 Enhanced TOT has been hardware-tested.\n");
    printf("Recommended: Firmware Menu -> [G] Guided Firmware Workflow.\n");
    printf("\n");
    printf("I don't own a TK-941. I did this work to help hams and make these radios useful.\n");
    printf("A substantial amount of time, effort, hardware testing, firmware analysis,\n");
    printf("reverse-engineering, and experimentation has gone into this project.\n");
    printf("\n");
    printf("EEPROM calibration backup/restore is a possible future feature.\n");
    printf("Development firmware mode can write only required flash pages without first\n");
    printf("reading the installed firmware, and can use a user-supplied baseline for an\n");
    printf("offline sparse diff during rapid custom-firmware testing.\n");
    printf("\n");
    printf("LEGAL / OPERATING RESPONSIBILITY:\n");
    printf("You are responsible for ensuring that the radio and its operation comply with\n");
    printf("all applicable laws, license conditions, frequency allocations, emission\n");
    printf("requirements, and equipment-authorization rules, including FCC Part 90,\n");
    printf("Part 97, or other applicable rules in the United States.\n");
    printf("The ability to program a frequency does not make operation there legal.\n");
    printf("Use this software at your own risk.\n");
    printf("\n");
    printf("FIRMWARE RECOVERY:\n");
    printf("A failed or interrupted normal firmware write cannot hard-brick the radio.\n");
    printf("The permanent loader is in mask ROM, and loader access must already be\n");
    printf("enabled before normal firmware writing. Reflash to recover. Deliberately\n");
    printf("altered custom firmware could disable loader access in configuration memory\n");
    printf("and then become unusable; the README explains this exceptional case.\n");
    printf("CHIRP: https://chirpmyradio.com/  KENWOOD: https://comms.kenwood.com/\n");
    printf("TK-x4x CHIRP driver: https://github.com/KY4WI/Kenwood_TK-x4x_Chirp_Driver\n");
    if (wait_for_return) {
        ui_flush_input();
        printf("Press any key to return to the main menu.\n");
        fflush(stdout);
        (void)ui_read_key();
    } else {
        fflush(stdout);
    }
}

/* --------------------------- Terminal dimensions ------------------------- */

static void check_terminal_size(void)
{
    int cols = 0, rows = 0;
    if (!detect_terminal_size(&cols, &rows) || rows < 30)
        printf("For best results, a terminal size of 80x30 or larger is recommended.\n\n");
}

/* ---------------------------- CLI front end ----------------------------- */

enum cli_exit_code {
    CLI_OK = 0,
    CLI_FW_WRITE_COMM = 11,
    CLI_FW_VERIFY = 12,
    CLI_FW_INVALID = 13,
    CLI_FW_SETUP = 14,
    CLI_FW_RESOURCE = 15,
    CLI_FW_NEEDS_INTERACTION = 16,
    CLI_FW_INTERNAL = 19,
    CLI_CH_WRITE_COMM = 21,
    CLI_CH_VERIFY = 22,
    CLI_CH_INVALID = 23,
    CLI_CH_SETUP = 24,
    CLI_CH_RESOURCE = 25,
    CLI_CH_NEEDS_INTERACTION = 26,
    CLI_CH_INTERNAL = 29,
    CLI_CONN_IO = 31,
    CLI_CONN_PROTOCOL = 32,
    CLI_CONN_IDENT = 33,
    CLI_CONN_MODE = 34,
    CLI_CONN_RESOURCE = 35,
    CLI_CONN_NEEDS_INTERACTION = 36,
    CLI_CONN_INTERNAL = 39,
    CLI_FW_READ_COMM = 41,
    CLI_FW_READ_RESPONSE = 42,
    CLI_FW_READ_MODE = 44,
    CLI_FW_READ_ABORT = 45,
    CLI_FW_READ_NEEDS_INTERACTION = 46,
    CLI_FW_READ_RESOURCE = 47,
    CLI_FW_READ_INTERNAL = 49,
    CLI_CH_READ_COMM = 51,
    CLI_CH_READ_RESPONSE = 52,
    CLI_CH_READ_MODE = 54,
    CLI_CH_READ_ABORT = 55,
    CLI_CH_READ_NEEDS_INTERACTION = 56,
    CLI_CH_READ_RESOURCE = 57,
    CLI_CH_READ_INTERNAL = 59,
    CLI_FILE_READ = 61,
    CLI_FILE_WRITE = 62,
    CLI_FILE_FORMAT = 63,
    CLI_FILE_REFUSED = 64,
    CLI_FILE_ABORT = 65,
    CLI_FILE_NEEDS_INTERACTION = 66,
    CLI_FILE_RESOURCE = 68,
    CLI_FILE_INTERNAL = 69,
    CLI_BACKUP_ERROR = 71,
    CLI_BACKUP_FILE = 72,
    CLI_BACKUP_FORMAT = 73,
    CLI_BACKUP_SETUP = 74,
    CLI_BACKUP_ABORT = 75,
    CLI_BACKUP_NEEDS_INTERACTION = 76,
    CLI_BACKUP_INPUT = 77,
    CLI_BACKUP_RESOURCE = 78,
    CLI_BACKUP_INTERNAL = 79,
    CLI_PATCH_INVALID = 81,
    CLI_PATCH_SUPPORT = 82,
    CLI_PATCH_REFERENCE = 83,
    CLI_PATCH_ABORT = 85,
    CLI_PATCH_NEEDS_INTERACTION = 86,
    CLI_PATCH_INPUT = 87,
    CLI_PATCH_RESOURCE = 88,
    CLI_PATCH_INTERNAL = 89,
    CLI_USER_ABORTED = 91,
    CLI_USER_RETRY_DECLINED = 92,
    CLI_USER_CANCELLED_SELECTION = 93,
    CLI_USER_CANCELLED_FILE = 94,
    CLI_USER_CANCELLED_VERIFY = 95,
    CLI_ARGUMENT_ERROR = 98,
    CLI_INTERNAL_ERROR = 99
};

struct cli_options {
    const char *command;
    char input[MAX_PATH_LEN];
    char output[MAX_PATH_LEN];
    char port[MAX_PATH_LEN];
    char backup_dir[MAX_PATH_LEN];
    int has_input;
    int has_output;
    int has_port;
    int has_backup_dir;
    int verbose;
    int no_sparse;
    int no_ansi;
    int omit_author_signature;
    int omit_chirp_tag;
    unsigned patch_selection;
    int patch_selection_explicit;
    int show_help;
    int show_version;
};

static uint32_t moose_rotl32(uint32_t x, unsigned r)
{
    return (x << r) | (x >> (32u - r));
}

/* Nonlinear phase decorrelation for deterministic byte-stream whitening. */
static uint32_t moose_next(uint32_t *state, size_t i)
{
    static const uint8_t moose_mix[5] = {0x6B, 0x59, 0x34, 0x77, 0x49};
    uint32_t x = *state;
    uint32_t k = moose_mix[i % 5u];

    x ^= moose_rotl32(x, 4u + (k & 3u));
    x += 0x6D5A56A5u ^ (k << ((i & 3u) * 8u));
    x ^= x >> (1u + (k & 3u));
    x = moose_rotl32(x, 22u - (i & 7u));
    x ^= x >> (5u + ((k >> 3) & 3u));
    x += 0x45D9F3Bu * (k | 1u);
    x ^= x >> 16;
    *state = x;
    return x;
}

// moose
static const uint8_t moose_option[] = {
    0xF3,0xF3,0xCC,0x25,0xB3,0xBA,0xF8,0x39,0x98,0x36,0x6C,0xD2,0xF4,
    0x83,0xF0,0xA2,0xB0,0x67,0xC1,0x45,0xAC,0x3D,0x50,0x7C,0x06,0x34,
    0x40,0xA2,0x9B,0xE3,0x72,0x5B,0x67,0xA7,0xF1,0x10,0x2A,0x8D
};

static void decode_moose(char *out, size_t cap)
{
    uint32_t state = 0x44615665u;
    size_t i, n = sizeof(moose_option);
    if (cap == 0) return;
    if (n >= cap) n = cap - 1;
    for (i = 0; i < n; i++) {
        state = moose_next(&state, i);
        out[i] = (char)(moose_option[i] ^ (uint8_t)(state >> 24));
    }
    out[n] = '\0';
}

static int cli_is_radio(const char *s)
{
    return s && !ascii_casecmp(s, "radio");
}

static void cli_set_path(char *dst, size_t cap, const char *src)
{
    copy_string(dst, cap, src ? src : "");
}

static int cli_select_port_if_needed(struct app_state *st)
{
    if (st->port_name[0]) return 0;
    if (g_cli_noninteractive) {
        cli_mark_needs_interaction("no serial port was supplied; use --port");
        return 1;
    }
    return select_port(st) == 0 ? 0 : 1;
}

static void cli_print_connection_failure(const struct app_state *st)
{
    printf("\nConnection failed.\n");
    printf("Check the cable, serial port, and radio power.\n");
    printf("Make sure the radio screen is not flashing \"PROGRAM\" or displaying a 4-digit\n");
    printf("checksum. Non-flashing \"PROGRAM\" or normal radio mode is OK.\n");
    if (st->port_name[0] && !selected_port_is_present(st->port_name))
        printf("Selected serial port is no longer available.\n");
}

static int cli_probe_radio(struct app_state *st)
{
    for (;;) {
        if (cli_select_port_if_needed(st) != 0) return 1;
        if (quick_probe(st) == 0) {
            printf("Radio type: %s\n", model_name(st->model));
            printf("Radio ID: %s\n", st->ident);
            if (st->quick_rev)
                printf("Firmware: %s\n", st->quick_rev->label);
            else
                printf("Firmware: Unknown firmware version\n");
            if (st->model == MODEL_940 || st->model == MODEL_941) {
                char patch_status[80];
                format_patch_status(st,patch_status,sizeof(patch_status),1);
                printf("%s\n",patch_status);
            }
            if (st->quick_rev && st->radio_tx == TX_FIXED_OFFSET && !st->radio_chirp_tag_present)
                printf("Checksum: %04X (known stock firmware)\n", st->quick_rev->checksum);
            else
                printf("Checksum: -- (full firmware read required)\n");
            printf("Password: %s\n",password_display(st));
            print_memory_jogger(st);
            return 0;
        }

        st->connected = 0;
        cli_print_connection_failure(st);
        if (g_cli_noninteractive) {
            cli_mark_needs_interaction("radio connection failed; retry/port-selection decision required");
            return 1;
        }
        {
            int choice = connection_failure_menu("connection");
            if (choice == 'r') continue;
            if (choice == 's') {
                if (select_port(st) != 0) return 1;
                continue;
            }
            if (choice == 'd') {
                int detect = auto_detect_usb_port(st);
                if (detect == 0) continue;
                if (detect == 2 || g_main_menu_requested) return 1;
                continue;
            }
            return 1;
        }
    }
}

static int cli_get_input_file(struct app_state *st, const struct cli_options *o,
                              enum browse_kind kind, const char *title,
                              char out[MAX_PATH_LEN])
{
    if (o->has_input) {
        if (cli_is_radio(o->input)) {
            fprintf(stderr, "Error: this operation requires a file input, not radio.\n");
            return CLI_ARGUMENT_ERROR;
        }
        cli_set_path(out, MAX_PATH_LEN, o->input);
        return 0;
    }
    if (g_cli_noninteractive) {
        cli_mark_needs_interaction("input file was not supplied; use --input");
        return CLI_FILE_NEEDS_INTERACTION;
    }
    {
        int rc = browse_for_file(st, kind, title, out);
        return rc == 0 ? 0 : CLI_USER_CANCELLED_SELECTION;
    }
}

static int cli_get_output_file(struct app_state *st, const struct cli_options *o,
                               enum browse_kind kind, const char *title,
                               const char *def, char out[MAX_PATH_LEN])
{
    if (o->has_output) {
        if (cli_is_radio(o->output)) {
            fprintf(stderr, "Error: this operation requires a file output, not radio.\n");
            return CLI_ARGUMENT_ERROR;
        }
        cli_set_path(out, MAX_PATH_LEN, o->output);
        return 0;
    }
    if (g_cli_noninteractive) {
        cli_mark_needs_interaction("output file was not supplied; use --output");
        return CLI_FILE_NEEDS_INTERACTION;
    }
    {
        int rc = make_save_path(st, kind, title, def, out);
        return rc == 0 ? 0 : CLI_USER_CANCELLED_FILE;
    }
}

static int cli_default_firmware_name(const struct app_state *st, char out[MAX_PATH_LEN])
{
    proposed_firmware_filename(st, "bin", out);
    return 0;
}

static int cli_default_channels_name(const struct app_state *st, char out[MAX_PATH_LEN])
{
    char model[32];
    snprintf(model, sizeof(model), "%s",
             model_name(st->model != MODEL_UNKNOWN ? st->model : st->channels_model));
    sanitize_filename(model);
    snprintf(out, MAX_PATH_LEN, "%s_CH.bin", model);
    return 0;
}

static int cli_default_backup_name(const struct app_state *st, char out[MAX_PATH_LEN])
{
    char model[32];
    snprintf(model, sizeof(model), "%s", model_name(st->model));
    sanitize_filename(model);
    snprintf(out, MAX_PATH_LEN, "%s_FULLBACKUP.bin", model);
    return 0;
}

static int cli_save_firmware(struct app_state *st, const struct cli_options *o)
{
    char path[MAX_PATH_LEN], def[MAX_PATH_LEN];
    cli_default_firmware_name(st, def);
    {
        int rc = cli_get_output_file(st, o, BROWSE_FIRMWARE, "Save firmware as", def, path);
        if (rc != 0) return rc;
    }
    {
        int rc = write_firmware_to_named_file(st, path, 0);
        if (rc == -2) return CLI_USER_CANCELLED_FILE;
        return rc == 0 ? 0 : CLI_FILE_WRITE;
    }
}

static int cli_save_channels(struct app_state *st, const struct cli_options *o)
{
    char path[MAX_PATH_LEN], def[MAX_PATH_LEN];
    cli_default_channels_name(st, def);
    {
        int rc = cli_get_output_file(st, o, BROWSE_CHANNELS, "Save channels as", def, path);
        if (rc != 0) return rc;
    }
    {
        int rc = save_channels_to_path(st, path);
        if (rc == -2) return CLI_USER_CANCELLED_FILE;
        return rc == 0 ? 0 : CLI_FILE_WRITE;
    }
}

static int cli_backup(struct app_state *st, const struct cli_options *o)
{
    uint8_t firmware[PROGRAM_SIZE], channels[CHANNEL_SIZE];
    char ident[8], path[MAX_PATH_LEN], def[MAX_PATH_LEN];
    if (read_full_radio(st, firmware, channels, 0, NULL, ident) < 0) return -1;
    populate_full_backup_state(st, firmware, channels, ident);
    cli_default_backup_name(st, def);
    {
        int rc = cli_get_output_file(st, o, BROWSE_FULL, "Save full backup as", def, path);
        if (rc != 0) return rc;
    }
    {
        int rc = save_full_backup_to_path(st, firmware, channels, ident, path);
        if (rc == -2) return CLI_USER_CANCELLED_FILE;
        return rc == 0 ? 0 : CLI_BACKUP_FILE;
    }
}

static int cli_restore(struct app_state *st, const struct cli_options *o)
{
    char path[MAX_PATH_LEN];
    if (o->has_output && !cli_is_radio(o->output)) {
        fprintf(stderr, "Error: restore output is the radio. A file output is not valid.\n");
        return CLI_ARGUMENT_ERROR;
    }
    if (o->has_input) {
        if (cli_is_radio(o->input)) {
            fprintf(stderr, "Error: restore input must be a backup file.\n");
            return CLI_ARGUMENT_ERROR;
        }
        cli_set_path(path, sizeof(path), o->input);
    } else {
        if (g_cli_noninteractive) {
            cli_mark_needs_interaction("restore input file was not supplied; use --input");
            return CLI_BACKUP_NEEDS_INTERACTION;
        }
        if (browse_for_file(st, BROWSE_FULL, "Select full backup to restore", path) != 0)
            return CLI_USER_CANCELLED_SELECTION;
    }
    return action_full_restore_path(st, path);
}

static int cli_firmware_read(struct app_state *st, const struct cli_options *o)
{
    if (o->has_input && !cli_is_radio(o->input)) {
        fprintf(stderr, "Error: firmware-read input is the radio.\n");
        return CLI_ARGUMENT_ERROR;
    }
    if (o->has_output && cli_is_radio(o->output)) {
        fprintf(stderr, "Error: firmware-read output must be a file.\n");
        return CLI_ARGUMENT_ERROR;
    }
    if (action_read_firmware_radio(st) != 0) return -1;
    return cli_save_firmware(st, o);
}

static int cli_load_firmware_path(struct app_state *st, const char *path)
{
    int rc = load_firmware_file(st, path);
    if (rc == -2) return CLI_FILE_FORMAT;
    if (rc < 0) return CLI_FILE_READ;
    return 0;
}

static int cli_load_channels_path(struct app_state *st, const char *path)
{
    int rc = load_channels_file(st, path);
    if (rc == -2) return CLI_FILE_FORMAT;
    if (rc < 0) return CLI_FILE_READ;
    return 0;
}

static int cli_firmware_write(struct app_state *st, const struct cli_options *o)
{
    char path[MAX_PATH_LEN];
    if (o->has_output && !cli_is_radio(o->output)) {
        fprintf(stderr, "Error: firmware-write output is the radio.\n");
        return CLI_ARGUMENT_ERROR;
    }
    {
        int rc = cli_get_input_file(st, o, BROWSE_FIRMWARE, "Select firmware file", path);
        if (rc != 0) return rc;
    }
    {
        int rc = cli_load_firmware_path(st, path);
        if (rc != 0) return rc;
    }
    return action_write_firmware_radio_mode(st, o->no_sparse, NULL, NULL);
}

static int cli_channels_read(struct app_state *st, const struct cli_options *o)
{
    if (o->has_input && !cli_is_radio(o->input)) {
        fprintf(stderr, "Error: channel-read input is the radio.\n");
        return CLI_ARGUMENT_ERROR;
    }
    if (o->has_output && cli_is_radio(o->output)) {
        fprintf(stderr, "Error: channel-read output must be a file.\n");
        return CLI_ARGUMENT_ERROR;
    }
    if (action_read_channels_radio(st) != 0) return -1;
    return cli_save_channels(st, o);
}

static int cli_channels_write(struct app_state *st, const struct cli_options *o)
{
    char path[MAX_PATH_LEN];
    if (o->has_output && !cli_is_radio(o->output)) {
        fprintf(stderr, "Error: channel-write output is the radio.\n");
        return CLI_ARGUMENT_ERROR;
    }
    {
        int rc = cli_get_input_file(st, o, BROWSE_CHANNELS, "Select channels file", path);
        if (rc != 0) return rc;
    }
    {
        int rc = cli_load_channels_path(st, path);
        if (rc != 0) return rc;
    }
    return action_write_channels_radio(st);
}

static int cli_firmware_patch(struct app_state *st, const struct cli_options *o)
{
    int input_radio = !o->has_input || cli_is_radio(o->input);
    int output_radio = !o->has_output || cli_is_radio(o->output);
    char path[MAX_PATH_LEN];

    if (o->no_sparse) {
        fprintf(stderr, "Error: --no-sparse applies to firmware-write, not firmware-patch.\n");
        return CLI_ARGUMENT_ERROR;
    }
    if (input_radio) {
        if (action_read_firmware_radio(st) != 0) return -1;
    } else {
        {
            int rc = cli_get_input_file(st, o, BROWSE_FIRMWARE, "Select firmware file", path);
            if (rc != 0) return rc;
        }
        {
            int rc = cli_load_firmware_path(st, path);
            if (rc != 0) return rc;
        }
    }

    st->include_author_signature = !o->omit_author_signature;
    st->include_modified_tag = !o->omit_chirp_tag;
    if (action_patch_loaded(st) != 0) return -1;

    if (output_radio) {
        return action_write_firmware_radio(st);
    }
    return cli_save_firmware(st, o);
}

static int cli_firmware_loader_write(struct app_state *st, const struct cli_options *o)
{
    struct cli_options fw = *o;
    fw.no_sparse = 1;
    return cli_firmware_write(st, &fw);
}

static void cli_verbose_description(const char *command)
{
    if (!strcmp(command, "help"))
        printf("Show the project Help / Info page.\n\n");
    else if (!strcmp(command, "guided"))
        printf("Start the same Guided Firmware Workflow available in the Firmware Menu.\n"
               "Choose radio or file input, validate and prepare recommended firmware,\n"
               "then optionally save it or write it to the radio.\n\n");
    else if (!strcmp(command, "probe-radio"))
        printf("Quickly connect to the radio and show basic identification, firmware status,\n"
               "and a channel preview. No full firmware read is performed.\n\n");
    else if (!strcmp(command, "firmware-read"))
        printf("Read the complete firmware from the radio and save it to a firmware file.\n\n");
    else if (!strcmp(command, "firmware-write"))
        printf("Write a firmware file to the radio. Changed blocks are written by default;\n"
               "--no-sparse writes every block.\n\n");
    else if (!strcmp(command, "firmware-patch"))
        printf("Patch recognized TK-940/941 firmware (TX), or 8355/AB00 (TOT) only.\n\n");
    else if (!strcmp(command, "channel-read"))
        printf("Read channel information from the radio and save it to a channel file.\n"
               "This is not a channel editor; use CHIRP to edit channels.\n\n");
    else if (!strcmp(command, "channel-write"))
        printf("Load channel information from a file and write it to the radio.\n"
               "This is not a channel editor; use CHIRP to edit channels.\n\n");
    else if (!strcmp(command, "backup"))
        printf("Read firmware and channel information from the radio and save both to a\n"
               "backup file.\n\n");
    else if (!strcmp(command, "restore"))
        printf("Load firmware and channel information from a backup file and write both\n"
               "to the radio. Firmware loader access is left enabled.\n\n");
    else if (!strcmp(command, "firmware-loader-write"))
        printf("Write the complete firmware without reading the existing firmware first.\n"
               "This uses the resident mask-ROM firmware loader and does not depend on the installed application firmware.\n\n");
}

static int cli_parse_args(int argc, char **argv, struct cli_options *o,
                          const char *hidden_signature_option)
{
    int i;
    memset(o, 0, sizeof(*o));
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { o->show_help = 1; continue; }
        if (!strcmp(a, "--version")) { o->show_version = 1; continue; }
        if (!strcmp(a, "--verbose")) { o->verbose = 1; continue; }
        if (!strcmp(a, "--no-sparse")) { o->no_sparse = 1; continue; }
        if (!strcmp(a, "--no-ansi") || !strcmp(a, "--plain-text") || !strcmp(a, "-p")) { o->no_ansi = 1; continue; }
        if (!strcmp(a, "--omit-author-signature") ||
            (hidden_signature_option && !strcmp(a, hidden_signature_option))) { o->omit_author_signature = 1; continue; }
        if (!strcmp(a, "--omit-chirp-tag")) { o->omit_chirp_tag = 1; continue; }
        if (!strcmp(a, "--patches")) {
            const char *value;
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --patches requires tx, tot, or both.\n");
                return CLI_ARGUMENT_ERROR;
            }
            value = argv[++i];
            if (!strcmp(value, "tx")) o->patch_selection = PATCH_SELECT_TX;
            else if (!strcmp(value, "tot")) o->patch_selection = PATCH_SELECT_TOT;
            else if (!strcmp(value, "both")) o->patch_selection = PATCH_SELECT_BOTH;
            else {
                fprintf(stderr, "Error: --patches must be tx, tot, or both.\n");
                return CLI_ARGUMENT_ERROR;
            }
            o->patch_selection_explicit = 1;
            continue;
        }
        if (!strcmp(a, "--input") || !strcmp(a, "--output") || !strcmp(a, "--port") || !strcmp(a, "--backup-dir")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: %s requires a value.\n", a);
                return CLI_ARGUMENT_ERROR;
            }
            if (!strcmp(a, "--input")) { cli_set_path(o->input, sizeof(o->input), argv[++i]); o->has_input = 1; }
            else if (!strcmp(a, "--output")) { cli_set_path(o->output, sizeof(o->output), argv[++i]); o->has_output = 1; }
            else if (!strcmp(a, "--port")) { cli_set_path(o->port, sizeof(o->port), argv[++i]); o->has_port = 1; }
            else { cli_set_path(o->backup_dir, sizeof(o->backup_dir), argv[++i]); o->has_backup_dir = 1; }
            continue;
        }
        if (a[0] == '-') {
            fprintf(stderr, "Error: unknown option: %s\n", a);
            return CLI_ARGUMENT_ERROR;
        }
        if (o->command) {
            fprintf(stderr, "Error: more than one command was specified.\n");
            return CLI_ARGUMENT_ERROR;
        }
        if (!strcmp(a, "channels-read") || !strcmp(a, "channel-read")) o->command = "channel-read";
        else if (!strcmp(a, "channels-write") || !strcmp(a, "channel-write")) o->command = "channel-write";
        else if (!strcmp(a, "brick-recovery") || !strcmp(a, "firmware-loader-write"))
            o->command = "firmware-loader-write";
        else o->command = a;
    }
    return 0;
}

static int cli_validate_options(const struct cli_options *o)
{
    if (!o->command) return 0;

    /* The guided entry point is intentionally fully interactive.
       Ignore no options silently: source, output and patches are chosen
       inside the exact same workflow used by the Firmware Menu. */
    if (!strcmp(o->command, "guided") &&
        (o->has_input || o->has_output || o->no_sparse ||
         o->patch_selection_explicit || o->omit_author_signature ||
         o->omit_chirp_tag)) {
        fprintf(stderr, "Error: guided selects source, output and patches interactively.\n"
                        "Only --port, --backup-dir, --no-ansi and --verbose apply.\n");
        return CLI_ARGUMENT_ERROR;
    }

    if (o->patch_selection_explicit &&
        strcmp(o->command, "firmware-patch")) {
        fprintf(stderr, "Error: --patches only applies to firmware-patch.\n");
        return CLI_ARGUMENT_ERROR;
    }
    if (o->omit_author_signature || o->omit_chirp_tag) {
        if (strcmp(o->command, "firmware-patch")) {
            fprintf(stderr, "Error: patch-marking options only apply to firmware-patch.\n");
            return CLI_ARGUMENT_ERROR;
        }
    }
    if (o->command && (!strcmp(o->command, "help") || !strcmp(o->command, "probe-radio")) &&
        (o->no_sparse || o->omit_author_signature || o->omit_chirp_tag)) {
        fprintf(stderr, "Error: one or more firmware-write/patch options are not valid with %s.\n", o->command);
        return CLI_ARGUMENT_ERROR;
    }
    if (o->command && !strcmp(o->command, "firmware-read") && (o->no_sparse || o->omit_author_signature || o->omit_chirp_tag)) {
        fprintf(stderr, "Error: firmware-read does not accept those options.\n");
        return CLI_ARGUMENT_ERROR;
    }
    if (o->command && !strcmp(o->command, "channel-read") && (o->no_sparse || o->omit_author_signature || o->omit_chirp_tag)) {
        fprintf(stderr, "Error: channel-read does not accept those options.\n");
        return CLI_ARGUMENT_ERROR;
    }
    if (o->command && !strcmp(o->command, "channel-write") && (o->no_sparse || o->omit_author_signature || o->omit_chirp_tag)) {
        fprintf(stderr, "Error: channel-write does not accept those options.\n");
        return CLI_ARGUMENT_ERROR;
    }
    if (o->command && !strcmp(o->command, "backup") && (o->no_sparse || o->omit_author_signature || o->omit_chirp_tag)) {
        fprintf(stderr, "Error: backup does not accept those options.\n");
        return CLI_ARGUMENT_ERROR;
    }
    if (o->command && !strcmp(o->command, "restore") && (o->no_sparse || o->omit_author_signature || o->omit_chirp_tag)) {
        fprintf(stderr, "Error: restore does not accept those options.\n");
        return CLI_ARGUMENT_ERROR;
    }
    if (o->command && !strcmp(o->command, "firmware-loader-write") &&
        (o->omit_author_signature || o->omit_chirp_tag || o->no_sparse)) {
        fprintf(stderr, "Error: firmware-loader-write does not accept patch options or --no-sparse.\n");
        return CLI_ARGUMENT_ERROR;
    }
    return 0;
}

static int run_cli_command(struct app_state *st, const struct cli_options *o)
{
    if (!o->command) return CLI_OK;
    if (!strcmp(o->command, "help")) {
        help_screen(0);
        return CLI_OK;
    }
    if (!strcmp(o->command, "guided")) return action_guided_firmware(st);
    if (!strcmp(o->command, "probe-radio")) return cli_probe_radio(st);
    if (!strcmp(o->command, "firmware-read")) return cli_firmware_read(st, o);
    if (!strcmp(o->command, "firmware-write")) return cli_firmware_write(st, o);
    if (!strcmp(o->command, "firmware-patch")) return cli_firmware_patch(st, o);
    if (!strcmp(o->command, "channel-read")) return cli_channels_read(st, o);
    if (!strcmp(o->command, "channel-write")) return cli_channels_write(st, o);
    if (!strcmp(o->command, "backup")) return cli_backup(st, o);
    if (!strcmp(o->command, "restore")) return cli_restore(st, o);
    if (!strcmp(o->command, "firmware-loader-write")) return cli_firmware_loader_write(st, o);
    fprintf(stderr, "Error: unknown command: %s\n", o->command);
    return CLI_ARGUMENT_ERROR;
}

static int cli_map_result(const char *command, int rc)
{
    if (g_cli_needs_interaction) {
        if (!command) return CLI_INTERNAL_ERROR;
        if (!strcmp(command, "guided")) return CLI_PATCH_NEEDS_INTERACTION;
        if (!strcmp(command, "probe-radio")) return CLI_CONN_NEEDS_INTERACTION;
        if (!strcmp(command, "firmware-read")) return CLI_FW_READ_NEEDS_INTERACTION;
        if (!strcmp(command, "firmware-write") || !strcmp(command, "firmware-loader-write"))
            return CLI_FW_NEEDS_INTERACTION;
        if (!strcmp(command, "channel-read")) return CLI_CH_READ_NEEDS_INTERACTION;
        if (!strcmp(command, "channel-write")) return CLI_CH_NEEDS_INTERACTION;
        if (!strcmp(command, "backup") || !strcmp(command, "restore"))
            return CLI_BACKUP_NEEDS_INTERACTION;
        if (!strcmp(command, "firmware-patch"))
            return CLI_PATCH_NEEDS_INTERACTION;
        return CLI_FILE_NEEDS_INTERACTION;
    }
    if (rc == 0) return CLI_OK;
    if (rc >= 10 && rc <= 99) return rc;
    if (rc == 1) return CLI_USER_ABORTED;
    if (rc == 2 && command && (!strcmp(command, "firmware-write") ||
                               !strcmp(command, "firmware-patch") ||
                               !strcmp(command, "restore"))) return CLI_FW_VERIFY;
    if (!command) return CLI_INTERNAL_ERROR;
    if (!strcmp(command, "firmware-read")) return CLI_FW_READ_COMM;
    if (!strcmp(command, "firmware-write") || !strcmp(command, "firmware-loader-write")) return CLI_FW_WRITE_COMM;
    if (!strcmp(command, "firmware-patch") || !strcmp(command, "guided"))
        return CLI_PATCH_INVALID;
    if (!strcmp(command, "channel-read")) return CLI_CH_READ_COMM;
    if (!strcmp(command, "channel-write")) return CLI_CH_WRITE_COMM;
    if (!strcmp(command, "backup") || !strcmp(command, "restore")) return CLI_BACKUP_ERROR;
    return CLI_INTERNAL_ERROR;
}

static void usage(const char *prog)
{
    printf("Usage: %s [global options] command [options]\n\n", prog);
    printf("Commands:\n");
    printf("  help                 Show the project Help / Info page.\n");
    printf("  guided               Launch the interactive Guided Firmware Workflow.\n");
    printf("  probe-radio          Quickly identify the connected radio.\n");
    printf("  firmware-read        Read firmware from radio and save it to a file.\n");
    printf("  firmware-write       Write a firmware file to the radio.\n");
    printf("  firmware-patch       Apply TX and/or TOT patches between file/radio endpoints.\n");
    printf("  channel-read         Read channel data from radio and save it to a file.\n");
    printf("  channel-write        Write channel data from a file to the radio.\n");
    printf("  backup               Read firmware and channel information from the radio,\n");
    printf("                       and save them to a backup file.\n");
    printf("  restore              Load firmware and channel information from a backup\n");
    printf("                       file, and write them to the radio.\n");
    printf("  firmware-loader-write\n");
    printf("                       Write the entire firmware through the resident mask-ROM\n");
    printf("                       firmware loader without reading installed firmware first.\n\n");
    printf("Global options:\n");
    printf("  --help               Show this command/option reference.\n");
    printf("  --version            Show program name, version, and copyright.\n");
    printf("  --verbose            Show a short description of what the command does.\n");
    printf("  --no-ansi            Force plain-text terminal output.\n");
    printf("  --port PORT          Use this serial port when radio access is required.\n");
    printf("  --input SOURCE       Input file, or the reserved endpoint 'radio'.\n");
    printf("  --output DEST        Output file, or the reserved endpoint 'radio'.\n");
    printf("  --backup-dir DIR     Directory for firmware and channel backups.\n");
    printf("  --no-sparse          Write the entire firmware, including identical blocks.\n");
    printf("  --patches MODE       Select tx, tot, or both on recognized firmware.\n");
    printf("                       Unknown firmware revisions cannot be patched.\n");
    printf("  --omit-author-signature\n");
    printf("                       Do not write the optional KY4WI FW MODS signature.\n");
    printf("  --omit-chirp-tag     Do not write the recommended firmware feature-options byte\n");
    printf("                       that identifies the patch to the Toolkit and CHIRP.\n\n");
    printf("Endpoint rules:\n");
    printf("  firmware-read, channel-read, and backup read from radio by default.\n");
    printf("  firmware-write, channel-write, restore, and firmware-loader-write write to radio\n");
    printf("  by default.\n");
    printf("  firmware-patch accepts file/file, radio/file, file/radio, or radio/radio;\n");
    printf("  omitted patch endpoints default to radio.\n");
    printf("  Binary firmware, channel files and full backups default to .bin.\n");
    printf("  Binary input is identified by size, regardless of file extension.\n");
    printf("  channel-read/channel-write are the canonical names; plural forms are aliases.\n");
    printf("  TK-940 and TK-941 are identical from a programming standpoint; their\n");
    printf("  differences are hardware-only and determine usable frequencies.\n");
    printf("  This is not a channel editor. Use CHIRP if you want to edit channels.\n\n");
    printf("Command-line operations use the normal guided prompts and radio-handling steps\n");
    printf("when interaction is required.\n");
}

/* --------------------------------- Main ---------------------------------- */

int main(int argc, char **argv)
{
    struct app_state st;
    struct cli_options opt;
    char hidden_signature_option[64];
    int rc;

    /* Help and version must not touch the radio, filesystem state or
       interactive terminal. Handle them before loading development caches. */
    memset(&st, 0, sizeof(st));
    decode_moose(hidden_signature_option, sizeof(hidden_signature_option));
    rc = cli_parse_args(argc, argv, &opt, hidden_signature_option);
    if (rc != 0) return rc;

    if (opt.show_version) {
        printf("%s %s\n", argv[0], tool_display_version());
        printf("Copyright (c) 2026 Dave, KY4WI\n");
        return 0;
    }
    if (opt.show_help) {
        usage(argv[0]);
        return 0;
    }

    snprintf(st.backup_dir, sizeof(st.backup_dir), "backups");
    get_current_dir(st.file_dir);
    st.include_author_signature = 1;
    st.include_modified_tag = 1;
    st.patch_selection = PATCH_SELECT_TX;
    st.compare_before_firmware_write = 1;
    st.development_firmware_mode = 0;
    dev_clear_baseline(&st);
    dev_load_persisted_baseline(&st);
    /* These global settings apply equally to command and menu mode. */
    if (opt.has_port)
        normalize_port_name(st.port_name, sizeof(st.port_name), opt.port);
    if (opt.has_backup_dir)
        cli_set_path(st.backup_dir, sizeof(st.backup_dir), opt.backup_dir);

    if (!opt.command) {
        ui_init(opt.no_ansi);
        check_terminal_size();
        if (opt.verbose)
            printf("Note: --verbose has no additional effect in menu mode.\n");
        {
            char backup[MAX_PATH_LEN];
            size_t selected = 0;
            for (;;) {
                struct ui_item items[10];
                size_t count = 0;
                struct status_screen_ctx ctx = {&st, "MAIN MENU"};
                int choice;

                items[count++] = (struct ui_item){'1', "1. [G]uided Firmware Workflow", NULL,
                    "Prepare recommended firmware and patches, then save to file or write to radio."};
                items[count++] = (struct ui_item){'2', "2. [S]et Programmer Port", NULL,
                    "Select, detect, or manually enter the serial port for the radio programmer."};
                items[count++] = (struct ui_item){'3', "3. [Q]uick Probe Radio", NULL,
                    "Identify the radio and preview firmware, patch status, and channels without a full read."};
                items[count++] = (struct ui_item){'4', "4. [F]irmware Menu", NULL,
                    "Guided Firmware Workflow and individual firmware operations."};
                items[count++] = (struct ui_item){'5', "5. [C]hannels Menu", NULL,
                    "Read, load, save, or write channel information."};
                items[count++] = (struct ui_item){'6', "6. Full [B]ackup", NULL,
                    "Read firmware and channels and save them together in one full backup."};
                items[count++] = (struct ui_item){'7', "7. Full [R]estore", NULL,
                    "Restore firmware and channels from a full backup, overwriting the radio's existing data."};
                items[count++] = (struct ui_item){'8', "8. [A]dvanced / Special Options", NULL,
                    "Configure backup, firmware-writing, patch-tag, and recovery options."};
                items[count++] = (struct ui_item){'9', "9. [H]elp / Info", NULL,
                    "Show project information, supported radios, and usage notes."};
                items[count++] = (struct ui_item){'0', "0. [E]xit", NULL,
                    "Exit TK-x4x Toolkit."};

                choice = ui_menu(items, count, &selected, draw_status_screen, &ctx);
                if (choice == '0') break;
                if (choice == UI_KEY_EOF) break;
                switch (choice) {
                case '1': (void)action_guided_firmware(&st); break;
                case '2': (void)select_port(&st); break;
                case '3': (void)connect_menu(&st); break;
                case '4': firmware_menu(&st); break;
                case '5': channels_menu(&st); break;
                case '6': (void)full_backup(&st, backup); break;
                case '7': (void)action_full_restore(&st); break;
                case '8': advanced_menu(&st); break;
                case '9': help_screen(1); break;
                default: break;
                }
                ui_flush_input();
                if (g_quit_requested) break;
                if (g_main_menu_requested) g_main_menu_requested = 0;
            }
        }
        return 0;
    }

    /* CLI commands use the guided workflow; no unattended mode is exposed. */
    g_cli_noninteractive = 0;
    g_cli_confirm_write = 0;
    g_cli_needs_interaction = 0;

    rc = cli_validate_options(&opt);
    if (rc != 0) return rc;

    if (opt.patch_selection_explicit) {
        st.patch_selection = opt.patch_selection;
        st.patch_selection_explicit = 1;
    }
    if (opt.omit_author_signature)
        st.include_author_signature = 0;
    if (opt.omit_chirp_tag)
        st.include_modified_tag = 0;

    ui_init(opt.no_ansi);
    if (opt.verbose)
        cli_verbose_description(opt.command);

    rc = run_cli_command(&st, &opt);
    return cli_map_result(opt.command, rc);
}
