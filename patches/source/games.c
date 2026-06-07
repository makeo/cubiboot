

// The concept here is that we have a thread which can be started and performs 3 actions in order
// 1. List all of the games in target_dir and sort them by name (only including certain file extensions)
// 2. Enumerate all of the games and evaluate if they are valid by checking their headers
// 3. Load assets for each approved path and store them in the auxiliarly data RAM using DMA

// Note that the thread is protected by a mutex
// Also note that the cube animation will return WAIT until step 3. starts running

// Given that assets are stored in "external memory" we need to be able to load them from the ARAM
// We will use a thread to handle queue requeust and loading the assets in the background

// This is how data is stored for file entries...

#include <gctypes.h>

#include "pmalloc/pmalloc.h"
#include "picolibc.h"
#include "reloc.h"
#include "attr.h"

#include "dolphin_os.h"
#include "dolphin_arq.h"
#include "dolphin_dvd.h"
#include "flippy_sync.h"
#include "dvd_threaded.h"

#include "metaphrasis.h"

#include "games.h"
#include "grid.h"
#include "menu.h"
#include "time.h"

#include "emu/tweaks.h"

#define PRELOAD_LINE_COUNT 2
#define ASSET_BUFFER_COUNT 128

// Swiss maintains a recent-games list at this path (when its "Recent List" option is
// enabled). cubeboot boots games by chainloading swiss-gc.dol with Autoload=, so every
// launch from the menu lands here -- Recent_0 is the most recent. We only READ it (no
// sidecar to write), so there is no SD-write dependency. Entries look like
// "Recent_0=sdc:/Games/Game.iso"; the "<dev>:" prefix is stripped to match our paths.
#define SWISS_RECENT_PATH "/swiss/settings/recent.ini"
#define RECENT_READ_SIZE 512   // Recent_0 is line 2, always well within the first sector

// Globals
int number_of_lines = 4;
int game_backing_count = 0;

static OSMutex game_enum_mutex_obj;
OSMutex *game_enum_mutex = &game_enum_mutex_obj;

char game_enum_path[128] = {0};
bool game_enum_running = false;

int assets_per_page;
int assets_initial_count;

// TODO: use a log2 malloc copy strategy for this
__attribute_data_lowmem__ static gm_path_entry_t __gm_early_path_list[2000];
__attribute_data_lowmem__ static gm_path_entry_t *__gm_sorted_path_list[2000];
__attribute_data_lowmem__ static gm_file_entry_t *gm_entry_backing[2000];

static u32 gm_entry_count = 0;

// Sliding-window mode for folders with more banners than the pool. While false (<=128
// banners) every banner stays resident in MRAM (the proven cold-boot-safe path, no
// per-scroll work). It flips true once the pool fills during gm_check_files; from then
// banners load/free as lines scroll, re-read straight from disc (bypassing the ARAM
// bnr_cache). Reset per folder scan in gm_check_files.
static bool gm_evict_on_scroll = false;

// Last-played: remember_last_game is defined/patched in main.c. This is the index the
// enum thread asks the menu thread to select (-1 = nothing pending).
int gm_pending_last_played_slot = -1;

// When the cold-boot scan lands in the folder that holds the last-played game, we run that
// scan in sliding-window mode (no resident banner preload) so the menu appears immediately;
// the menu thread then jumps to the game and loads just its window, instead of waiting for
// every banner in the folder to load. gm_last_played_path is the game's full path, read once
// at the start of that scan and reused by gm_match_last_played (no second recent.ini read).
static bool gm_scan_is_last_played = false;
static char gm_last_played_path[MAX_FILE_NAME];

gm_file_entry_t *gm_get_game_entry(int index) {
    if (index >= gm_entry_count) return NULL;
    return gm_entry_backing[index];
}

__attribute_aligned_data_lowmem__ static gm_icon_buf_t gm_icon_pool[ASSET_BUFFER_COUNT] = {};
__attribute_aligned_data_lowmem__ static gm_banner_buf_t gm_banner_pool[ASSET_BUFFER_COUNT] = {};

static inline gm_icon_buf_t *gm_get_icon_buf() {
    for (int i = 0; i < ASSET_BUFFER_COUNT; i++) {
        if (gm_icon_pool[i].used == 0) {
            OSReport("Allocating icon buffer\n");
            gm_icon_pool[i].used = 1;
            return &gm_icon_pool[i];
        }
    }

    return NULL;
}

static inline void gm_free_icon_buf(gm_icon_buf_t *buf) {
    if (buf == NULL) return;
    buf->used = 0;
}

static int gm_count_icon_buf() {
    int count = 0;
    for (int i = 0; i < ASSET_BUFFER_COUNT; i++) {
        if (gm_icon_pool[i].used == 1) count++;
    }

    return count;
}

static inline gm_banner_buf_t *gm_get_banner_buf() {
    for (int i = 0; i < ASSET_BUFFER_COUNT; i++) {
        if (gm_banner_pool[i].used == 0) {
            gm_banner_pool[i].used = 1;
            return &gm_banner_pool[i];
        }
    }

    return NULL;
}

static inline void gm_free_banner_buf(gm_banner_buf_t *buf) {
    if (buf == NULL) return;
    buf->used = 0;
}

static int gm_count_banner_buf() {
    int count = 0;
    for (int i = 0; i < ASSET_BUFFER_COUNT; i++) {
        if (gm_banner_pool[i].used == 1) count++;
    }

    return count;
}

static int gm_count_pending_free() {
    int count = 0;
    for (int i = 0; i < gm_entry_count; i++) {
        gm_file_entry_t *entry = gm_entry_backing[i];
        count += entry->asset.icon.schedule_free;
        count += entry->asset.banner.schedule_free;
    }

    return count;
}

// TODO: swap the icon/banner callback with req->owner checks
static void arq_icon_callback_setup(u32 arq_request_ptr) {
    // OSReport("CALLBACK arq_icon_callback_setup\n");
    ARQRequest *req = (ARQRequest*)arq_request_ptr;
    gm_icon_t *icon = (gm_icon_t*)req;

    icon->state = GM_LOAD_STATE_LOADED;
    DCFlushRange(icon, sizeof(gm_icon_t));
}

static void arq_icon_callback_unload(u32 arq_request_ptr) {
    // OSReport("CALLBACK arq_icon_callback_unload\n");
    ARQRequest *req = (ARQRequest*)arq_request_ptr;
    gm_icon_t *icon = (gm_icon_t*)req;

    icon->state = GM_LOAD_STATE_UNLOADED;
    gm_free_icon_buf(icon->buf);
    DCFlushRange(icon, sizeof(gm_icon_t));
}

static void arq_icon_callback_load(u32 arq_request_ptr) {
    // OSReport("CALLBACK arq_icon_callback_load\n");
    ARQRequest *req = (ARQRequest*)arq_request_ptr;
    gm_icon_t *icon = (gm_icon_t*)req;

    icon->state = GM_LOAD_STATE_LOADED;
    DCFlushRange(icon, sizeof(gm_icon_t));
}

static void arq_banner_callback_setup(u32 arq_request_ptr) {
    // OSReport("CALLBACK arq_banner_callback_setup\n");
    ARQRequest *req = (ARQRequest*)arq_request_ptr;
    gm_banner_t *banner = (gm_banner_t*)req;

    banner->state = GM_LOAD_STATE_LOADED;
    DCFlushRange(banner, sizeof(gm_banner_t));
}

static void arq_banner_callback_unload(u32 arq_request_ptr) {
    // OSReport("CALLBACK arq_banner_callback_unload\n");
    ARQRequest *req = (ARQRequest*)arq_request_ptr;
    gm_banner_t *banner = (gm_banner_t*)req;

    banner->state = GM_LOAD_STATE_UNLOADED;
    gm_free_banner_buf(banner->buf);
    DCFlushRange(banner, sizeof(gm_banner_t));
}

static void arq_banner_callback_load(u32 arq_request_ptr) {
    // OSReport("CALLBACK arq_banner_callback_load\n");
    ARQRequest *req = (ARQRequest*)arq_request_ptr;
    gm_banner_t *banner = (gm_banner_t*)req;

    banner->state = GM_LOAD_STATE_LOADED;
    DCFlushRange(banner, sizeof(gm_banner_t));
}

// asset offload helpers
void gm_icon_setup(gm_icon_t *icon, u32 aram_offset) {
    OSReport("Setting up icon\n");
    icon->aram_offset = aram_offset;
    icon->state = GM_LOAD_STATE_SETUP;

    ARQRequest *req = &icon->req;
    u32 owner = make_type('I', 'X', 'X', 'S');
    u32 type = ARAM_DIR_MRAM_TO_ARAM;
    u32 priority = ARQ_PRIORITY_LOW;
    u32 source = (u32)icon->buf->data;
    u32 dest = aram_offset;
    u32 length = ICON_PIXELDATA_LEN;

    dolphin_ARQPostRequest(req, owner, type, priority, source, dest, length, &arq_icon_callback_setup);
}

void gm_icon_setup_unload(gm_icon_t *icon, u32 aram_offset) {
    icon->aram_offset = aram_offset;
    icon->state = GM_LOAD_STATE_UNLOADING;

    ARQRequest *req = &icon->req;
    u32 owner = make_type('I', 'C', 'O', 'U');
    u32 type = ARAM_DIR_MRAM_TO_ARAM;
    u32 priority = ARQ_PRIORITY_LOW;
    u32 source = (u32)icon->buf->data;
    u32 dest = aram_offset;
    u32 length = ICON_PIXELDATA_LEN;

    dolphin_ARQPostRequest(req, owner, type, priority, source, dest, length, &arq_icon_callback_unload);
}

void gm_icon_load(gm_icon_t *icon) {
    // OSReport("Loading icon %d\n", icon->state);
    if (icon->state == GM_LOAD_STATE_SETUP) {
        OSReport("ERROR: banner is still in setup\n");
    }
    if (icon->state == GM_LOAD_STATE_UNLOADING) {
        OSReport("ERROR: banner is still in unload\n");
    }
    if (icon->state == GM_LOAD_STATE_NONE || icon->state == GM_LOAD_STATE_LOADED) return;
    icon->state = GM_LOAD_STATE_LOADING;

    gm_icon_buf_t *icon_ptr = gm_get_icon_buf();
    if (icon_ptr == NULL) {
        OSReport("ERROR: could not allocate memory\n");
        return;
    }
    icon->buf = icon_ptr;
    DCFlushRange(icon, sizeof(gm_icon_t));

    ARQRequest *req = &icon->req;
    u32 owner = make_type('I', 'C', 'O', 'L');
    u32 type = ARAM_DIR_ARAM_TO_MRAM;
    u32 priority = ARQ_PRIORITY_LOW;
    u32 source = icon->aram_offset;
    u32 dest = (u32)icon->buf->data;
    u32 length = ICON_PIXELDATA_LEN;

    dolphin_ARQPostRequest(req, owner, type, priority, source, dest, length, &arq_icon_callback_load);
}

void gm_icon_free(gm_icon_t *icon) {
    if (icon->state == GM_LOAD_STATE_NONE) return;
    
    if (icon->state == GM_LOAD_STATE_LOADING || icon->state == GM_LOAD_STATE_SETUP) {
        OSReport("ERROR: icon is still loading\n");
        icon->schedule_free = true;
        return;
    }

    icon->state = GM_LOAD_STATE_UNLOADING;
    // memset(icon->buf->data, 0, ICON_PIXELDATA_LEN); // test only
    gm_free_icon_buf(icon->buf);
    icon->buf = NULL;
    icon->state = GM_LOAD_STATE_UNLOADED;
}

// Banners are kept resident in MRAM (no ARAM streaming). The original async
// MRAM<->ARAM unload/reload path corrupts banners on cold boot; with up to
// ASSET_BUFFER_COUNT buffers the visible set fits in MRAM, so ARAM is never used.
// The pixels are already in banner->buf->data (memcpy'd + flushed by gm_load_banner),
// so we just mark the banner LOADED.
void gm_banner_setup(gm_banner_t *banner, u32 aram_offset) {
    banner->aram_offset = aram_offset;
    banner->state = GM_LOAD_STATE_LOADED;
}

void gm_banner_setup_unload(gm_banner_t *banner, u32 aram_offset) {
    // No ARAM unload — keep the banner resident in MRAM (see gm_banner_setup).
    banner->aram_offset = aram_offset;
    banner->state = GM_LOAD_STATE_LOADED;
}

void gm_banner_load(gm_banner_t *banner) {
    // No ARAM reload: banners are loaded straight into MRAM by gm_load_banner and kept
    // resident, so on scroll they are already LOADED and there is nothing to reload.
    (void)banner;
}

void gm_banner_free(gm_banner_t *banner) {
    if (banner->state == GM_LOAD_STATE_NONE || banner->state == GM_LOAD_STATE_UNLOADING) {
        if (banner->state == GM_LOAD_STATE_UNLOADING) OSReport("ERROR: banner is unloading??\n");
        return;
    }
    
    if (banner->state == GM_LOAD_STATE_LOADING || banner->state == GM_LOAD_STATE_SETUP) {
        OSReport("ERROR: banner is still loading\n");
        banner->schedule_free = true;
        return;
    }

    banner->state = GM_LOAD_STATE_UNLOADING;
    // memset(banner->buf->data, 0, BNR_PIXELDATA_LEN); // test only
    gm_free_banner_buf(banner->buf);
    banner->buf = NULL;
    banner->state = GM_LOAD_STATE_UNLOADED;
}

// HEAP
pmalloc_t pmblock;
pmalloc_t *pm = &pmblock;
__attribute_aligned_data_lowmem__ static u8 gm_heap_buffer[2 * 1024 * 1024];

// MACROS
#define gm_malloc(x) pmalloc_memalign(pm, x, 32);
#define gm_free(x) pmalloc_freealign(pm, x);

void gm_init_heap() {
    OSReport("Initializing heap [%x]\n", sizeof(gm_heap_buffer));

    // The asset pools live in the custom .data_lowmem section, which is NOT
    // zero-cleared on a cold boot (PicoBoot/gekko doesn't clear it). Their .used
    // flags therefore come up as random garbage -> gm_get_*_buf either hands out a
    // slot already in use (banner aliasing/corruption) or finds none free (blank
    // banners), and it gets worse the colder the RAM. Zero them explicitly before
    // any asset is loaded.
    memset(gm_banner_pool, 0, sizeof(gm_banner_pool));
    memset(gm_icon_pool, 0, sizeof(gm_icon_pool));

    // Initialise our pmalloc
	pmalloc_init(pm);
	pmalloc_addblock(pm, &gm_heap_buffer[0], sizeof(gm_heap_buffer));
}

#if 0
// png
static void *ok_gm_alloc(void *user_data, size_t size) {
    (void)user_data;
    return pmalloc_malloc(pm, size);
}

static void ok_gm_free(void *user_data, void *memory) {
    (void)user_data;
    pmalloc_free(pm, memory);
}

const ok_png_allocator OK_PNG_GM_ALLOCATOR = {
    .alloc = ok_gm_alloc,
    .free = ok_gm_free,
    .image_alloc = NULL,
};
#endif

typedef struct {
    uint8_t *data;
    size_t size;
    size_t position;
} ok_mem_state_t;
static ok_mem_state_t ok_mem_state;

static size_t ok_mem_read(void *user_data, uint8_t *buffer, size_t length) {
    (void)user_data;
    ok_mem_state_t *state = &ok_mem_state;
    if (state->position + length > state->size) {
        length = state->size - state->position;  // Adjust length to prevent overflow
    }
    memcpy(buffer, state->data + state->position, length);
    state->position += length;
    return length;
}

static bool ok_mem_seek(void *user_data, long count) {
    (void)user_data;
    ok_mem_state_t *state = &ok_mem_state;
    if (state->position + count > state->size || state->position + count < 0) {
        return false;  // Out of bounds
    }
    state->position += count;
    return true;
}

#if 0
static const ok_png_input OK_PNG_MEM_INPUT = {
    .read = ok_mem_read,
    .seek = ok_mem_seek,
};

ok_png gm_png_decode(void *file_buf, size_t file_size) {
    ok_mem_state.data = file_buf;
    ok_mem_state.size = file_size;
    ok_mem_state.position = 0;

    ok_png png = ok_png_read_from_input(OK_PNG_COLOR_FORMAT_RGBA, OK_PNG_MEM_INPUT, file_buf, OK_PNG_GM_ALLOCATOR, NULL);
    return png;
}
#endif

// HELPERS
static char *valid_game_exts[] = {".gcm", ".iso", ".fdi"};
static char *valid_prog_exts[] = {".dol", ".dol+cli"}; // TODO: add .elf
static gm_file_type_t gm_get_file_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (ext == NULL) return GM_FILE_TYPE_UNKNOWN;

    for (int i = 0; i < sizeof(valid_game_exts) / sizeof(char*); i++) {
        if (strcasecmp(ext, valid_game_exts[i]) == 0) return GM_FILE_TYPE_GAME;
    }

    for (int i = 0; i < sizeof(valid_prog_exts) / sizeof(char*); i++) {
        if (strcasecmp(ext, valid_prog_exts[i]) == 0) return GM_FILE_TYPE_PROGRAM;
    }

    return GM_FILE_TYPE_UNKNOWN;
}

static void ipl_panic() {
    // TODO: set XFB
    OSReport("PANIC: unknown\n");
    while(1);
}

static char *hidden_file_names[] = {
    // Windows
    "$RECYCLE.BIN",
    "System Volume Information",
    "Recovery",
    // GameCube
    "swiss",
    "MCBACKUP",
};

static bool check_file_hidden(const char *name) {
    if (name[0] == '.') return true;
    if (name[0] == '~') return true;
    
    int length = strlen(name);
    if (name[length - 1] == '~') return true;

    for (int i = 0; i < countof(hidden_file_names); i++) {
        if (strcasecmp(name, hidden_file_names[i]) == 0) return true;
    }

    return false;
}

int gm_cmp_path_entry(const void* ptr_a, const void* ptr_b){
    const gm_path_entry_t *obj_a = *(gm_path_entry_t**)ptr_a;
    const gm_path_entry_t *obj_b = *(gm_path_entry_t**)ptr_b;

    return strcasecmp(obj_a->path, obj_b->path);
}

// DEFS
typedef struct {
    int num_paths;
} gm_list_info;

// 1. func named gm_list_files returns {pointer to path array, number of paths}
// 2. func named gm_check_headers returns {pointer to valid path array, number of valid paths}
// 3. func named gm_load_assets returns {void}

// Before the next go a dealloc function is called to free all of the memory before the thread is started again

gm_list_info gm_list_files(const char *target_dir) {
    // List all of the games in target_dir and sort them by name (only including certain file extensions)
    OSReport("Listing files in %s\n", target_dir);
    u64 start_time = gettime();

    int res = dvd_custom_open(target_dir, FILE_ENTRY_TYPE_DIR, 0);
    if (res != 0) {
        OSReport("PANIC: SD Card could not be opened\n");
        while(1);
    }

    // TODO: if the SD card is not inserted this would be a good place to bail out 
    file_status_t *status = dvd_custom_status();
    if (status->result != 0) {
        OSReport("PANIC: SD Card could not be opened\n");
        while(1);
    }

    uint8_t dir_fd = status->fd;
    OSReport("found readdir fd=%u\n", dir_fd);

    // now list everything
    static GCN_ALIGNED(file_entry_t) ent;
    int path_entry_count = 0;
    char file_full_path_buf[128] = {0};

    // TODO: switch to using DVD Mutex (this is all happening in a thread)
    while(1) {
        int ret = dvd_custom_readdir(&ent, dir_fd);
        if (ret != 0) ipl_panic();
        if (ent.name[0] == 0) break; // end of directory
        if (ent.attrib & FILE_ATTRIB_FLAG_HIDDEN) continue; // skip hidden files
        if (check_file_hidden(ent.name)) continue; // skip hidden files

        // only check file ext for now
        gm_file_type_t file_type = GM_FILE_TYPE_UNKNOWN;
        if (ent.type == FILE_ENTRY_TYPE_DIR) {
            file_type = GM_FILE_TYPE_DIRECTORY;
        } else {
            file_type = gm_get_file_type(ent.name);
        }

        if (file_type == GM_FILE_TYPE_UNKNOWN) continue; // check if the file is valid

#ifdef PRINT_READDIR_NAMES
        // logging
        OSReport("READDIR ent(%u): %s [len=%d]\n", ent.type, ent.name, strlen(ent.name));
#endif

        // combine the path
        strcpy(file_full_path_buf, target_dir);
        strcat(file_full_path_buf, ent.name);
#ifdef PRINT_READDIR_NAMES
        // logging
        OSReport("PATH ent(%u): %s\n", ent.type, file_full_path_buf);
#endif

        // store the path
        gm_path_entry_t *entry = &__gm_early_path_list[path_entry_count];
        strcpy(entry->path, file_full_path_buf);
        entry->type = file_type;

        // setup sort list
        __gm_sorted_path_list[path_entry_count] = entry;
        path_entry_count++;

        if (path_entry_count >= 1920) {
            OSReport("WARNING: Too many files in directory\n");
            break;
        }
    }

    dvd_custom_close(dir_fd);

    f32 runtime = (f32)diff_usec(start_time, gettime()) / 1000.0;
    OSReport("File enum completed! took=%f (%d)\n", runtime, game_backing_count);
    (void)runtime;

    return (gm_list_info){path_entry_count};
}

void gm_sort_files(int path_count) {
    // Sort the paths by name
    u64 start_time = gettime();
    qsort(__gm_sorted_path_list, path_count, sizeof(gm_path_entry_t*), &gm_cmp_path_entry);

    f32 runtime = (f32)diff_usec(start_time, gettime()) / 1000.0;
    OSReport("Sort took=%f\n", runtime);
    (void)runtime;
}
// Title is the .iso/.gcm filename with the extension stripped (matching Swiss), so
// multi-disc games are told apart by the "Disc N" in their filename. Names are expected
// to fit the title box (~28 chars); a longer one is just clipped by the box at draw time
// (no in-code truncation) -- keep filenames short.
static void gm_set_title_from_path(gm_file_entry_t *entry) {
    char *out = entry->desc.fullGameName; // BNR_FULL_TEXT_LEN bytes

    const char *base = strrchr(entry->path, '/');
    base = base ? base + 1 : entry->path;

    const char *dot = strrchr(base, '.');
    int len = dot ? (int)(dot - base) : (int)strlen(base);
    if (len >= BNR_FULL_TEXT_LEN) len = BNR_FULL_TEXT_LEN - 1; // buffer safety only

    memcpy(out, base, len);
    out[len] = '\0';
}

// returns amount of space used in aram
static int gm_load_banner(gm_file_entry_t *entry, u32 aram_offset, bool force_unload, bool use_cache) {
    if (entry->extra.dvd_bnr_offset == 0) return false;
    // Already resident: never re-read or re-allocate. Guards the scroll re-read path so
    // a still-loaded banner can't leak its pool buffer.
    if (entry->asset.banner.state == GM_LOAD_STATE_LOADED) return true;

    __attribute_aligned_data_lowmem__ static BNR banner_buffer;
    // use_cache keeps makeo's bnr_cache for the resident (<=128) path; the >128 scroll
    // re-reads pass use_cache=false so they read straight from disc and never touch ARAM.
    if (use_cache && bnr_cache_get(entry->extra.game_id, entry->extra.disc_num, entry->extra.disc_ver, &banner_buffer))
        goto cached;

    // load the banner
    dvd_custom_open(entry->path, FILE_ENTRY_TYPE_FILE, IPC_FILE_FLAG_DISABLECACHE | IPC_FILE_FLAG_DISABLESPEEDEMU);
    file_status_t *status = dvd_custom_status();
    if (status == NULL || status->result != 0) {
        OSReport("ERROR: could not open file\n");
        return false;
    }

    //__attribute_aligned_data_lowmem__ static BNR banner_buffer;
    dvd_threaded_read(&banner_buffer, sizeof(BNR), entry->extra.dvd_bnr_offset, status->fd);
    dvd_custom_close(status->fd);

    if (use_cache) bnr_cache_put(entry->extra.game_id, entry->extra.disc_num, entry->extra.disc_ver, &banner_buffer);
    cached:

    entry->asset.banner.state = GM_LOAD_STATE_LOADING;
    gm_banner_buf_t *banner_ptr = gm_get_banner_buf();
    if (banner_ptr == NULL) {
        OSReport("ERROR: could not allocate memory\n");
        return false;
    }

    memcpy(&banner_ptr->data[0], &banner_buffer.pixelData[0], BNR_PIXELDATA_LEN);
    DCFlushRange(&banner_ptr->data[0], BNR_PIXELDATA_LEN);
    entry->asset.banner.buf = banner_ptr;
    if (force_unload) {
        gm_banner_setup_unload(&entry->asset.banner, aram_offset);
    } else {
        gm_banner_setup(&entry->asset.banner, aram_offset);
    }

    // TODO: check current language using extra.dvd_bnr_type
    memcpy(&entry->desc, &banner_buffer.desc[0], sizeof(BNRDesc));

    // Override the banner's internal name with the .iso filename (the banner name is
    // identical across discs of the same game). Description/company stay for the info line.
    gm_set_title_from_path(entry);

    return true;
}

#if 0

// returns amount of space used in aram
static bool gm_load_icon(gm_file_entry_t *entry, u32 aram_offset, bool force_unload) {
    // split path and add png extension
    char icon_path[128];
    strcpy(icon_path, entry->path);
    char *ext = strrchr(icon_path, '.');
    if (ext == NULL) strcat(icon_path, ".png");
    else strcpy(ext, ".png");

    // load the icon
    dvd_custom_open(icon_path, FILE_ENTRY_TYPE_FILE, IPC_FILE_FLAG_DISABLECACHE | IPC_FILE_FLAG_DISABLEFASTSEEK);
    file_status_t *status = dvd_custom_status();
    if (status == NULL || status->result != 0) {
        OSReport("ERROR: could not open icon file: %s\n", icon_path);
        return false;
    }

    // allocate file buffer
    u32 file_size = (u32)__builtin_bswap64(*(u64*)(&status->fsize));
    file_size += 31;
    file_size &= 0xffffffe0;
    void *file_buf = gm_malloc(file_size);

    // read
    dvd_threaded_read(file_buf, file_size, 0, status->fd);
    dvd_custom_close(status->fd);

    ok_png png = gm_png_decode(file_buf, file_size);
    if (png.error_code != OK_PNG_SUCCESS) {
        OSReport("ERROR: could not decode icon file\n");
        return false;
    }

    OSReport("PNG: %d x %d\n", png.width, png.height);
    gm_free(file_buf);

    if (png_width != 32 || png_height != 32) {
        OSReport("ERROR: invalid png size\n");
        return false;
    }

    entry->asset.icon.state = GM_LOAD_STATE_LOADING;
    gm_icon_buf_t *icon_ptr = gm_get_icon_buf();
    if (icon_ptr == NULL) {
        OSReport("ERROR: could not allocate memory\n");
        return false;
    }

    Metaphrasis_convertBufferToRGB5A3((uint32_t*)png.data, (uint32_t*)&icon_ptr->data[0], png.width, png.height);
    DCFlushRange(&icon_ptr->data[0], ICON_PIXELDATA_LEN);
    pmalloc_free(pm, png.data);

    entry->asset.icon.buf = icon_ptr;
    if (force_unload) {
        gm_icon_setup_unload(&entry->asset.icon, aram_offset);
    } else {
        gm_icon_setup(&entry->asset.icon, aram_offset);
    }

    return true;
}

#endif

void gm_check_files(int path_count) {
    // Here we will also check for override assets and matching save icons
    u32 aram_offset = (1 * 1024 * 1024); // 1MB mark

    // Re-evaluate resident-vs-sliding per folder scan (pool is empty here after dealloc).
    gm_evict_on_scroll = false;

    // Enumerate all of the games
    u64 start_time = gettime();
    for (int i = 0; i < path_count; i++) {
        gm_path_entry_t *entry = __gm_sorted_path_list[i];
        // OSReport("Checking header %s [%d]\n", entry->path, entry->type);

        bool force_unload = false;
        if (gm_entry_count - (top_line_num * columns_per_line) > assets_initial_count) {
            force_unload = true;
        }

        if (!OSTryLockMutex(game_enum_mutex)) {
            OSReport("STOPPING GAME LOADING\n");
            break;
        }
        OSUnlockMutex(game_enum_mutex);

        // Load assets and store them in the auxiliarly data RAM using DMA
        if (entry->type == GM_FILE_TYPE_GAME) {
            // OSReport("DEBUG: Game Check %d\n", i);
            // check if the banner file exists
            dolphin_game_into_t info = get_game_info(entry->path);
            if (!info.valid) continue;
            OSReport("Found game %s (%d)\n", entry->path, force_unload); // lets do this!

            // create a new entry
            gm_file_entry_t *backing = gm_malloc(sizeof(gm_file_entry_t));
            memset(backing, 0, sizeof(gm_file_entry_t));
            memcpy(backing->path, entry->path, sizeof(backing->path));
            backing->type = GM_FILE_TYPE_GAME;
            gm_set_title_from_path(backing); // title even before the banner loads (>128 sliding window)

            // copy the extra info
            memcpy(backing->extra.game_id, info.game_id, sizeof(backing->extra.game_id));
            backing->extra.disc_num = info.disc_num;
            backing->extra.disc_ver = info.disc_ver;
            backing->extra.dvd_bnr_offset = info.bnr_offset;
            backing->extra.dvd_bnr_type = info.bnr_type;
            backing->extra.dvd_dol_offset = info.dol_offset;
            backing->extra.dvd_fst_offset = info.fst_offset;
            backing->extra.dvd_fst_size = info.fst_size;
            backing->extra.dvd_max_fst_size = info.max_fst_size;

            // load the banner, keeping it resident in MRAM. Once the pool fills (folder
            // has more than ASSET_BUFFER_COUNT games) switch to the sliding window: leave
            // the rest unloaded and let gm_line_load read them from disc on scroll.
            // <=128 games => all resident (the proven, cold-boot-safe path, unchanged).
            if (!gm_evict_on_scroll && gm_count_banner_buf() >= ASSET_BUFFER_COUNT) {
                gm_evict_on_scroll = true;
            }
            // Last-played scan: defer ALL banner loading to the background loader
            // (gm_bg_load_last_played) so the menu can appear and jump to the game first,
            // instead of blocking here until every banner in the folder is read.
            // Banner is normally already cached by get_game_info (read during validation),
            // so this is a cheap cache hit with no extra file open.
            if (!gm_evict_on_scroll && !gm_scan_is_last_played) {
                bool bnr_loaded = gm_load_banner(backing, aram_offset, force_unload, true);
                if (!bnr_loaded) {
                    OSReport("Failed to load banner %s\n", entry->path);
                }
            }
            aram_offset += BNR_PIXELDATA_LEN;

            // load the icon
            backing->asset.use_banner = true;

            // bool icon_loaded = gm_load_icon(backing, aram_offset, force_unload);
            // if (!icon_loaded) {
            //     // OSReport("Failed to load icon %s\n", entry->path);
            //     backing->asset.use_banner = true;
            // } else {
            //     backing->asset.use_banner = false;
            // }
            backing->asset.use_banner = true;
            aram_offset += ICON_PIXELDATA_LEN;

            // set heap pointer
            gm_entry_backing[gm_entry_count] = backing;
            gm_entry_count++;
        } else if (entry->type == GM_FILE_TYPE_PROGRAM || entry->type == GM_FILE_TYPE_DIRECTORY) {
            OSReport("Found other %s\n", entry->path); // lets do this!

            // create a new entry
            gm_file_entry_t *backing = gm_malloc(sizeof(gm_file_entry_t));
            memset(backing, 0, sizeof(gm_file_entry_t));
            memcpy(backing->path, entry->path, sizeof(backing->path));
            backing->type = entry->type;

            // get the basename (bounded copy: fullGameName is BNR_FULL_TEXT_LEN bytes)
            char *base = strrchr(entry->path, '/');
            const char *name = base ? base + 1 : entry->path;
            int nlen = (int)strlen(name);
            if (nlen >= BNR_FULL_TEXT_LEN) nlen = BNR_FULL_TEXT_LEN - 1;
            memcpy(backing->desc.fullGameName, name, nlen);
            backing->desc.fullGameName[nlen] = '\0';
            if (entry->type == GM_FILE_TYPE_PROGRAM) {
                strcpy(backing->desc.description, "Homebrew Program");
            } else {
                strcpy(backing->desc.description, "Directory");
            }

            // // load the icon
            // bool icon_loaded = gm_load_icon(backing, aram_offset, force_unload);
            // if (!icon_loaded) {
            //     // OSReport("Failed to load icon %s\n", entry->path);
            // }
            backing->asset.use_banner = false;
            aram_offset += ICON_PIXELDATA_LEN;

            // set heap pointer
            gm_entry_backing[gm_entry_count] = backing;
            gm_entry_count++;
        }

        // udelay_threaded(100 * 1000); // test only
        game_backing_count = gm_entry_count;
    }

    OSReport("Total entries = %d\n", gm_entry_count);

    // find multi-disc games
    for (int i = 0; i < gm_entry_count; i++) {
        gm_file_entry_t *entry = gm_entry_backing[i];
        if (entry->type != GM_FILE_TYPE_GAME) continue;
        if (entry->extra.disc_num == 0) continue; // check Disc 2 only

        OSReport("Checking multi-disc %s[%d] (%d)\n", entry->path, i, entry->extra.disc_num);

        for (int j = 0; j < gm_entry_count; j++) {
            gm_file_entry_t *entry2 = gm_entry_backing[j];
            if (entry2->type != GM_FILE_TYPE_GAME) continue;
            if (entry2 == entry) continue;

            // check if the game is multi-disc
            bool is_same_game = memcmp(entry->extra.game_id, entry2->extra.game_id, 6) == 0;
            bool is_different_disc = entry->extra.disc_num != entry2->extra.disc_num;
            if (is_same_game && is_different_disc) {
                OSReport("Found multi-disc %s[%d] (%d)\n", entry2->path, j, entry2->extra.disc_num);
                entry->second = entry2;
                entry2->second = entry;
            }
        }
    }

    f32 runtime = (f32)diff_usec(start_time, gettime()) / 1000.0;
    OSReport("Header check took=%f\n", runtime);
    (void)runtime;
}

void gm_line_load(int line_num) {
    // OSReport("Line load %d\n", line_num);

    for (int i = 0; i < columns_per_line; i++) {
        int index = (line_num * columns_per_line) + i;
        if (index >= gm_entry_count) break;

        gm_file_entry_t *entry = gm_entry_backing[index];
        if (entry->type == GM_FILE_TYPE_GAME) {
            gm_icon_load(&entry->asset.icon);
            if (gm_evict_on_scroll) {
                // sliding window (>128): re-read from disc, no bnr_cache => no ARAM.
                // gm_load_banner guards against re-loading an already-resident banner.
                gm_load_banner(entry, 0, false, false);
            } else {
                gm_banner_load(&entry->asset.banner); // no-op: <=128 stays resident
            }
        } else {
            gm_icon_load(&entry->asset.icon);
        }
    }
}

void gm_line_free(int line_num) {
    // <=128: banners stay resident in MRAM (no per-scroll free + ARAM reload, which
    // corrupted banners on cold boot). Buffers are released by the folder-change dealloc.
    if (!gm_evict_on_scroll) return;

    // >128 sliding window: free this off-screen line so its buffers return to the pool
    // for the lines scrolling into view (gm_line_load re-reads those from disc).
    for (int i = 0; i < columns_per_line; i++) {
        int index = (line_num * columns_per_line) + i;
        if (index >= gm_entry_count) break;

        gm_file_entry_t *entry = gm_entry_backing[index];
        if (entry->type == GM_FILE_TYPE_GAME) {
            gm_banner_free(&entry->asset.banner);
        }
    }
}

void gm_line_changed(int delta) {
    // OSReport("Line changed %+d\n", delta);

    // int previous_line_num = top_line_num;
    int new_line_num = top_line_num + delta;

    // two slots should be processed, one to load and one to unload

    // get number of lines to process. Free BEFORE load so the >128 sliding window always
    // has a free pool buffer for the line scrolling in (no-op order for the <=128 path).
    if (delta < 0) {
        // unload from the bottom
        int unload_line = new_line_num + DRAW_TOTAL_ROWS + PRELOAD_LINE_COUNT;
        if (unload_line < number_of_lines) {
            // OSReport("DEBUG: Unload line %d\n", unload_line);
            gm_line_free(unload_line);
        }

        int load_line = new_line_num - PRELOAD_LINE_COUNT + 1;
        if (load_line >= 0) {
            // OSReport("DEBUG: Load line %d\n", load_line);
            gm_line_load(load_line);
        }
    } else if (delta > 0) {
        // unload from the top
        int unload_line = new_line_num - PRELOAD_LINE_COUNT;
        if (unload_line >= 0) {
            // OSReport("DEBUG: Unload line %d\n", unload_line);
            gm_line_free(unload_line);
        }

        int load_line = new_line_num + DRAW_TOTAL_ROWS + PRELOAD_LINE_COUNT - 1;
        if (load_line < number_of_lines) {
            // OSReport("DEBUG: Load line %d\n", load_line);
            gm_line_load(load_line);
        }
    }

    // int icon_buf_count = gm_count_icon_buf();
    // int banner_buf_count = gm_count_banner_buf();

    // OSReport("icon buf count = %d\n", icon_buf_count);
    // OSReport("banner buf count = %d\n", banner_buf_count);

    // int pending_free = gm_count_pending_free();
    // OSReport("pending free = %d\n", pending_free);
}

// so grid can check if load/unload is possible
bool gm_can_move() {
    return true;
}

// ===== Last played =========================================================

// Strip the "<dev>:" device prefix Swiss writes (e.g. "sdc:/Games/x.iso" -> "/Games/x.iso")
// so the path matches cubeboot's internal entry paths. Returns the value unchanged if there
// is no prefix.
static const char *gm_strip_device(const char *value) {
    const char *colon = strchr(value, ':');
    return colon != NULL ? colon + 1 : value;
}

// Read Swiss's recent-games list and return the most-recently-played GAME path into `out`.
// Swiss writes entries newest-first ("Recent_0=...", "Recent_1=..."), so we walk the lines
// top-to-bottom and return the first whose extension is a game image -- this skips a
// last-launched utility (.dol) so it doesn't shadow the real last game. Uses the synchronous
// DI read so it is safe both pre-thread (startup folder selection) and on the enum thread
// (matching). Returns false if the option is off, the file is missing/empty, or it holds no
// game entry (e.g. Swiss's "Recent List" setting is disabled).
static bool gm_read_last_played(char *out, int out_len) {
    static GCN_ALIGNED(char) rbuf[RECENT_READ_SIZE];
    memset(rbuf, 0, sizeof(rbuf));

    if (dvd_custom_open(SWISS_RECENT_PATH, FILE_ENTRY_TYPE_FILE, IPC_FILE_FLAG_DISABLECACHE) != 0) return false;
    file_status_t *status = dvd_custom_status();
    if (status == NULL || status->result != 0) return false;
    int r = dvd_read(rbuf, sizeof(rbuf) - 1, 0, status->fd);
    dvd_custom_close(status->fd);
    if (r != 0) return false;
    rbuf[sizeof(rbuf) - 1] = '\0';

    char *p = rbuf;
    while (*p != '\0') {
        // terminate the current line (paths may contain spaces, so only stop at CR/LF)
        char *eol = p;
        while (*eol != '\0' && *eol != '\r' && *eol != '\n') eol++;
        char saved = *eol;
        *eol = '\0';

        if (strncmp(p, "Recent_", 7) == 0) {
            char *eq = strchr(p, '=');
            if (eq != NULL) {
                const char *path = gm_strip_device(eq + 1);
                if (path[0] != '\0' && gm_get_file_type(path) == GM_FILE_TYPE_GAME) {
                    strncpy(out, path, out_len - 1);
                    out[out_len - 1] = '\0';
                    return true;
                }
            }
        }

        if (saved == '\0') break;               // last line, no trailing newline
        p = eol + 1;
        while (*p == '\r' || *p == '\n') p++;   // skip the line break(s)
    }
    return false;
}

// Cold-boot startup folder: when a last-played game is saved, return the folder that
// CONTAINS it, so the menu opens wherever that game lives -- including letter/genre
// subfolders, not just default_folder. Returns NULL (caller falls back to default_folder)
// when the option is off, nothing is saved, or the folder can't be opened.
char *gm_last_played_folder(void) {
    static char folder_buf[MAX_FILE_NAME];
    if (!remember_last_game) return NULL;
    if (!gm_read_last_played(folder_buf, sizeof(folder_buf))) return NULL;

    // strip the filename to get the containing folder
    char *last_slash = strrchr(folder_buf, '/');
    if (last_slash == NULL) return NULL;        // malformed (no slash)
    if (last_slash == folder_buf) {
        folder_buf[1] = '\0';                   // game at root -> "/"
    } else {
        *last_slash = '\0';                     // "/a/b/Game.iso" -> "/a/b"
    }

    // verify the folder still exists; otherwise fall back to the default
    if (dvd_custom_open(folder_buf, FILE_ENTRY_TYPE_DIR, 0) != 0) return NULL;
    dvd_custom_close(dvd_custom_status()->fd);
    return folder_buf;
}

// Run once, at the very start of the cold-boot scan (before gm_check_files), to decide
// whether this folder is the one holding the last-played game. If so it arms
// gm_scan_is_last_played (so gm_check_files skips the resident banner preload and the menu
// appears immediately) and stashes the game's path for gm_match_last_played. One-shot: only
// the first cold-boot folder pre-selects; later navigation is unaffected. `folder` is the
// folder being scanned, with a trailing slash (game_enum_path).
static void gm_arm_last_played(const char *folder) {
    static bool consumed = false;
    gm_scan_is_last_played = false;
    if (!remember_last_game || consumed) return;
    consumed = true;

    if (!gm_read_last_played(gm_last_played_path, sizeof(gm_last_played_path))) return;

    // Only arm if we actually landed in the game's folder (pre-thread normally ensures this).
    // Derive the game's folder (keep the trailing slash) and compare to the scanned folder.
    char game_folder[MAX_FILE_NAME];
    strcpy(game_folder, gm_last_played_path);
    char *slash = strrchr(game_folder, '/');
    if (slash == NULL) return;
    slash[1] = '\0';                                // "/Games/x.iso" -> "/Games/"
    if (strcasecmp(game_folder, folder) == 0) {
        gm_scan_is_last_played = true;
    }
}

// After gm_check_files, if this scan was armed, find the last-played game among the scanned
// entries and stash its index for the menu thread to select. Runs on the enum thread.
static void gm_match_last_played() {
    if (!gm_scan_is_last_played) return;

    for (int i = 0; i < gm_entry_count; i++) {
        gm_file_entry_t *e = gm_entry_backing[i];
        if (e == NULL || e->type != GM_FILE_TYPE_GAME) continue;
        // Case-INSENSITIVE: the folder is opened via FAT (case-insensitive), so Swiss can
        // record a path whose casing differs from the on-disc directory entry. A
        // case-sensitive compare would open the right folder yet fail to highlight.
        if (strcasecmp(e->path, gm_last_played_path) == 0) {
            gm_pending_last_played_slot = i;
            return;
        }
    }

    // Armed but the game wasn't found (e.g. it failed validation): we ran in sliding-window
    // mode, so nothing is resident. Fall back to slot 0 so gm_apply still loads the top
    // window and the menu isn't left with blank banners until the user scrolls.
    gm_pending_last_played_slot = 0;
}

// Load every game banner on one line directly (used by the background loader). use_cache
// keeps the ARAM bnr_cache for the resident path; the >ASSET_BUFFER_COUNT sliding path passes
// false. gm_load_banner is a no-op for an already-resident banner, so re-calls are cheap.
// Icons are intentionally left to the menu thread (gm_line_load on scroll), so the only pool
// the background loader touches is the banner pool -- no contention with the live menu.
static void gm_bg_load_line(int line_num, bool use_cache) {
    for (int i = 0; i < columns_per_line; i++) {
        int index = (line_num * columns_per_line) + i;
        if (index >= gm_entry_count) break;
        gm_file_entry_t *entry = gm_entry_backing[index];
        if (entry->type == GM_FILE_TYPE_GAME) {
            gm_load_banner(entry, 0, false, use_cache);
        }
    }
}

// Background banner loader for the last-played folder. Runs on the enum thread AFTER the (fast,
// banner-less) scan. It loads the selected game's on-screen window FIRST so that game's banner
// appears almost immediately, then fills the rest of the folder while the menu is already
// interactive (the cursor is already on the game via gm_apply_pending_last_played).
// game_enum_running stays true throughout so a boot (Start -> gm_deinit_thread) still joins this
// thread; navigation/boot interrupt it via game_enum_mutex.
static void gm_bg_load_last_played(int target_slot) {
    // Count games: only banners use the pool, and only <=ASSET_BUFFER_COUNT fit resident.
    int game_count = 0;
    for (int i = 0; i < gm_entry_count; i++) {
        if (gm_entry_backing[i]->type == GM_FILE_TYPE_GAME) game_count++;
    }
    bool resident = (game_count <= ASSET_BUFFER_COUNT);
    // >pool: must use the sliding window (banners load/free on scroll). <=pool: stay resident
    // so scrolling never re-reads once filled.
    gm_evict_on_scroll = !resident;

    if (target_slot < 0) target_slot = 0;

    // Mirror grid_jump_to_slot's top-line clamp so the window we load matches what's shown.
    int start = target_slot / columns_per_line;
    int max_start = number_of_lines - DRAW_TOTAL_ROWS;
    if (max_start < 0) max_start = 0;
    if (start > max_start) start = max_start;
    if (start < 0) start = 0;

    int first = start - PRELOAD_LINE_COUNT;
    if (first < 0) first = 0;
    int last = start + DRAW_TOTAL_ROWS + PRELOAD_LINE_COUNT;
    if (last >= number_of_lines) last = number_of_lines - 1;

    // 1) Selected game's window first -> its banner is ready almost immediately.
    for (int l = first; l <= last; l++) gm_bg_load_line(l, resident);

    // >pool: the rest stream in on scroll (sliding window); nothing more to preload.
    if (!resident) return;

    // 2) Fill the rest of the folder resident, so later scrolling is instant. Poll the enum
    // mutex each line so gm_deinit_thread (boot/navigation) can stop us promptly.
    for (int l = 0; l < number_of_lines; l++) {
        if (l >= first && l <= last) continue;            // window already loaded
        if (!OSTryLockMutex(game_enum_mutex)) return;     // stop requested
        OSUnlockMutex(game_enum_mutex);
        gm_bg_load_line(l, resident);
    }
}

// Menu-thread side of the jump. The pending slot is set only after the grid is fully built
// (gm_match_last_played), so this fires immediately -- the cursor lands on the last game right
// away (even while its banner is still loading in the background), so pressing Start boots the
// right game. Banners are loaded by the background loader on the enum thread; the jump only
// touches menu-thread state (cursor/scroll), so the two never contend.
void gm_apply_pending_last_played() {
    if (gm_pending_last_played_slot < 0) return;

    int slot = gm_pending_last_played_slot;
    gm_pending_last_played_slot = -1;
    if (slot >= gm_entry_count) return;

    grid_jump_to_slot(slot);
}

#if 0
void gm_debug_func() {
    int icon_buf_count = gm_count_icon_buf();
    int banner_buf_count = gm_count_banner_buf();

    OSReport("icon buf count = %d\n", icon_buf_count);
    OSReport("banner buf count = %d\n", banner_buf_count);

    for (int i = 0; i < gm_entry_count; i++) {
        gm_file_entry_t *entry = gm_entry_backing[i];
        OSReport("Entry %d: %s\n", i, entry->path);

        if (entry->type == GM_FILE_TYPE_GAME) {
            // banner buf
            OSReport("Banner: %p (%d)\n", entry->asset.banner.buf, entry->asset.banner.state);
        }
    }
}
#endif

void gm_setup_grid(int line_count, bool initial) {
    grid_setup_columns_per_line();

    number_of_lines = (line_count + (columns_per_line - 1)) / columns_per_line;
    if (number_of_lines < 4) {
        number_of_lines = 4;
    }

    assets_per_page = columns_per_line * DRAW_TOTAL_ROWS;
    assets_initial_count = assets_per_page + (PRELOAD_LINE_COUNT * columns_per_line); // assuming we start at the top

    if (initial) {
        // Setup the grid
        grid_setup_func();
    }
}

void *gm_thread_worker(void* param) {
    const char *target = &game_enum_path[0];
    if (target == NULL || strlen(target) == 0) {
        OSReport("ERROR: target is NULL\n");
        return NULL;
    }

    // Scan ONLY the folder we land in, then draw its list. The menu draws from
    // game_backing_count (populated by gm_check_files), so the default_folder appears
    // immediately -- that is the instant cold boot. Other folders are read on demand when
    // opened (banners load on first visit, then stay cached for the session).
    //
    // There is deliberately NO whole-card background warm. Warming every folder up front
    // needs its own dedicated banner buffer (so it can't race the live menu's banner_buffer),
    // and that extra 8KB buffer is exactly what grew the loader image and broke launching
    // swiss-gc.dol from a folder (the boot hard-reset to the console IPL). The instant boot
    // comes from scanning just this folder -- not from the warm -- so dropping the warm keeps
    // the speed and fixes the regression.
    gm_list_info list_info = gm_list_files(target);
    gm_setup_grid(list_info.num_paths, true);
    gm_sort_files(list_info.num_paths);

    // Decide BEFORE loading banners whether this is the last-played folder: if so gm_check_files
    // skips the resident preload (sliding-window mode) so the menu appears immediately and only
    // the selected game's window is loaded afterwards.
    gm_arm_last_played(target);
    gm_check_files(list_info.num_paths);
    gm_setup_grid(gm_entry_count, false);

    // Pre-select the last-played game and, for that scan, load its banner window in the
    // background: the menu jumps to the game first (cursor + its banner), then the rest of
    // the folder's banners fill in here while the menu is already interactive.
    gm_match_last_played();
    if (gm_scan_is_last_played) {
        gm_bg_load_last_played(gm_pending_last_played_slot);
    }

    game_enum_running = false;
    // DCBlockStore((void*)OSRoundDown32B((u32)&game_enum_running));
    DCFlushRange((void*)OSRoundDown32B((u32)&game_enum_running), 4);

    // pmalloc_dump_stats(pm);
    return NULL;
}

void gm_init_thread() {
    OSInitMutex(game_enum_mutex);
}

// match https://github.com/projectPiki/pikmin2/blob/snakecrowstate-work/include/Dolphin/OS/OSThread.h#L55-L74
static OSThread thread_obj;
static u8 thread_stack[32 * 1024]; // TODO: move to lowmem slab?
void gm_start_thread(const char *target) {
    if (game_enum_running) {
        OSReport("ERROR: game enum thread is already running\n");
        return;
    }

    // pop one path element
    if (strcmp(target, "..") == 0) {
        OSReport("Previous path...\n");
        // zero the last char
        game_enum_path[strlen(game_enum_path) - 1] = 0;

        // find the last slash
        char *last_slash = strrchr(game_enum_path, '/');
        if (last_slash != NULL) {
            *last_slash++ = 0;
        }

        if (strlen(game_enum_path) == 0) {
            OSReport("ERROR: cannot go back any further\n");
            game_enum_path[0] = '/';
            game_enum_path[1] = 0;
        }

        target = game_enum_path;
    }

    char path[128];
    strcpy(path, target);
    if (path[strlen(path) - 1] != '/') {
        strcat(path, "/");
    }

    OSReport("Starting game thread %s\n", path);
    strcpy(game_enum_path, path);

    game_enum_running = true;
    DCBlockStore((void*)OSRoundDown32B((u32)&game_enum_running));
    
    // OSUnlockMutex(game_enum_mutex);

    if (gm_entry_count > 0) {
        for (int i = 0; i < gm_entry_count; i++) {
            gm_file_entry_t *entry = gm_entry_backing[i];
            if (entry->type == GM_FILE_TYPE_GAME) {
                gm_icon_free(&entry->asset.icon);
                gm_banner_free(&entry->asset.banner);
            } else {
                gm_icon_free(&entry->asset.icon);
            }
            gm_free(entry);
        }
        gm_entry_count = 0;
    }

    number_of_lines = 0;
    DCBlockStore((void*)OSRoundDown32B((u32)&number_of_lines));

    game_backing_count = 0;
    DCBlockStore((void*)OSRoundDown32B((u32)&game_backing_count));

    // Start the thread
    u32 thread_stack_size = sizeof(thread_stack);
    void *thread_stack_top = thread_stack + thread_stack_size;
    s32 thread_priority = DEFAULT_THREAD_PRIO + 3;

    dolphin_OSCreateThread(&thread_obj, gm_thread_worker, NULL, thread_stack_top, thread_stack_size, thread_priority, 0);
    dolphin_OSResumeThread(&thread_obj);
}


void gm_deinit_thread() {
    if (game_enum_running) {
        OSReport("Stopping file enum\n");
        OSLockMutex(game_enum_mutex);
        OSReport("Waiting for thread to exit, %d\n", game_enum_running);
        OSJoinThread(&thread_obj, NULL);
        OSReport("File enum done\n");
        OSUnlockMutex(game_enum_mutex);
    }
}

