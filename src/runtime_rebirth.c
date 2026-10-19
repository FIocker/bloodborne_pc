/* Rebirth at the Doll (scripts/rebirth.py, game 1.09): the Doll's new "Rebirth" choice puts a
 * character back to its origin's attributes and level for REBIRTH_INSIGHT Insight, and the levels
 * it had are then bought again for free in the level-up screen, as in Dark Souls III's rebirth.
 * No Blood Echoes are given back: a level costs nothing only below the level the character had
 * (its target), and no further.
 *
 * The patched game calls in through sceSystemServiceLaunchWebBrowser (runtime_services.c):
 * "bb:b" when the Doll's script asks for the rebirth, "bb:f" with a level when the level-up
 * screen prices that level. The player's data is GameDataMan (a global of the game image)
 * -> +0x8; its layout is the save's (launcher/bbport_respec.py) 0x40 bytes further in. The
 * targets are kept per character (origin and name) in <user>/rebirth.txt, so levels not spent
 * yet stay free after quitting. */
#include "runtime.h"
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAME_DATA_MAN 0x553B130u
enum { VITALITY = 0x40, INSIGHT = 0x84, LEVEL = 0x90, NAME = 0xA8, NAME_CHARS = 16, ORIGIN = 0xCE };
#define REBIRTH_INSIGHT 5
/* Each attribute's offset, in CharaInitParam's order: Vitality, Endurance, Strength, Skill,
 * Bloodtinge, Arcane (the slot at 0x50 is unused). */
static const int ATTRIBUTE[6] = {0x40, 0x48, 0x58, 0x60, 0x68, 0x70};
/* CharaInitParam rows 3000-3008: each origin's level and attributes. */
static const uint8_t ORIGINS[9][7] = {
    {10, 11, 10, 12, 10, 9, 8},  /* Milquetoast */
    {10, 14, 11, 11, 10, 7, 7},  /* Lone Survivor */
    {10, 9, 14, 9, 13, 6, 9},    /* Troubled Childhood */
    {10, 12, 11, 15, 9, 6, 7},   /* Violent Past */
    {10, 9, 12, 9, 15, 7, 8},    /* Professional */
    {10, 10, 10, 14, 13, 7, 6},  /* Military Veteran */
    {10, 7, 8, 9, 13, 14, 9},    /* Noble Scion */
    {10, 10, 12, 10, 9, 5, 14},  /* Cruel Fate */
    {4, 10, 9, 10, 9, 7, 9},     /* Waste of Skin */
};

static unsigned char *image;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void runtime_rebirth_set_image(unsigned char *base) { image = base; }

static uint32_t get32(const unsigned char *p, int at) { uint32_t v; memcpy(&v, p + at, 4); return v; }
static void put32(unsigned char *p, int at, uint32_t v) { memcpy(p + at, &v, 4); }

/* The player's data, or NULL outside a game. */
static unsigned char *player(void) {
    if (!image) return NULL;
    uintptr_t manager, data;
    memcpy(&manager, image + GAME_DATA_MAN, sizeof(manager));
    if (!manager) return NULL;
    memcpy(&data, (const void *)(manager + 8), sizeof(data));
    return (unsigned char *)data;
}

/* "<origin>:<name as UTF-16 hex>": who the target belongs to. */
static void character_key(const unsigned char *p, char *out, size_t size) {
    size_t used = (size_t)snprintf(out, size, "%u:", p[ORIGIN]);
    for (int i = 0; i < NAME_CHARS * 2 && used + 3 < size; i += 2) {
        if (!p[NAME + i] && !p[NAME + i + 1]) break;
        used += (size_t)snprintf(out + used, size - used, "%02x%02x", p[NAME + i], p[NAME + i + 1]);
    }
}

static void targets_path(char *out, size_t size) {
    snprintf(out, size, "%s/rebirth.txt", runtime_file_user_dir());
}

/* The target level for `key` (0: none). */
static unsigned read_target(const char *key) {
    char path[4096], line[256];
    targets_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    unsigned target = 0;
    const size_t length = strlen(key);
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, key, length) && line[length] == ' ') target = (unsigned)strtoul(line + length + 1, NULL, 10);
    }
    fclose(f);
    return target;
}

/* Sets (or with 0 removes) `key`'s target; the other characters' lines are kept. */
static void write_target(const char *key, unsigned target) {
    char path[4096], temp[4200], line[256];
    targets_path(path, sizeof(path));
    snprintf(temp, sizeof(temp), "%s.new", path);
    FILE *in = fopen(path, "r"), *out = fopen(temp, "w");
    if (!out) { if (in) fclose(in); return; }
    const size_t length = strlen(key);
    while (in && fgets(line, sizeof(line), in)) {
        if (!(strncmp(line, key, length) == 0 && line[length] == ' ')) fputs(line, out);
    }
    if (in) fclose(in);
    if (target) fprintf(out, "%s %u\n", key, target);
    fclose(out);
    remove(path);
    rename(temp, path);
}

int runtime_rebirth(void) {
    pthread_mutex_lock(&lock);
    unsigned char *p = player();
    int result = 0;
    if (p && p[ORIGIN] < 9) {
        const uint8_t *origin = ORIGINS[p[ORIGIN]];
        const uint32_t insight = get32(p, INSIGHT), level = get32(p, LEVEL);
        if (insight >= REBIRTH_INSIGHT && level > origin[0]) {
            char key[96];
            character_key(p, key, sizeof(key));
            const unsigned target = read_target(key);
            write_target(key, target > level ? target : level);
            for (int i = 0; i < 6; ++i) put32(p, ATTRIBUTE[i], origin[i + 1]);
            put32(p, LEVEL, origin[0]);
            put32(p, INSIGHT, insight - REBIRTH_INSIGHT);
            printf("Runtime: rebirth: level %" PRIu32 " -> %u, levels up to %u free, Insight %" PRIu32 " -> %" PRIu32 "\n",
                   level, origin[0], target > level ? target : level, insight, insight - REBIRTH_INSIGHT);
            result = 1;
        } else {
            printf("Runtime: rebirth refused: Insight %" PRIu32 " (needs %d), level %" PRIu32 " (origin %u)\n",
                   insight, REBIRTH_INSIGHT, level, origin[0]);
        }
    }
    pthread_mutex_unlock(&lock);
    fflush(stdout);
    return result;
}

int runtime_rebirth_free_level(int level) {
    pthread_mutex_lock(&lock);
    unsigned char *p = player();
    int free_level = 0;
    if (p && p[ORIGIN] < 9 && level >= 0) {
        char key[96];
        character_key(p, key, sizeof(key));
        const unsigned target = read_target(key);
        free_level = (unsigned)level < target;
        /* Every level is bought again: the target is no longer needed. */
        if (target && get32(p, LEVEL) >= target) write_target(key, 0);
    }
    pthread_mutex_unlock(&lock);
    return free_level;
}
