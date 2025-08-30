// Controls:
//  Arrow keys or mouse drag to pan; +/- to zoom; Esc to quit.
//  1..5 -> change map tile provider  [osm, google, arcgis, carto_light, carto_dark]

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_thread.h>
#include <curl/curl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define PATH_SEP '\\'
#else
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0755)
#define PATH_SEP '/'
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ----------------------------------------------------------------------------
// Config
// ----------------------------------------------------------------------------
#define WIN_W 1024
#define WIN_H 768
#define TILE_SIZE 256
#define MIN_ZOOM 1
#define MAX_ZOOM 22
#define INITIAL_ZOOM 1

// bage coord
#define INITIAL_LONGITUDE -54.10793
#define INITIAL_LATITUDE -31.33244

static const char *APP_TITLE = "Minimal Map Viewer (SDL2)";
static const char *CURL_USER_AGENT = "Minimal SDL2 Map Viewer";
static const char *CACHE_ROOT = "tilecache";

struct provider_t {
    char *type;
    char *url;
    char swap_xy;
};

// respect usage policies for real projects
static const struct provider_t PROVIDERS[] = {
    {"osm", "https://tile.openstreetmap.org/%d/%d/%d.png", 0},
    {"google", "https://khms2.google.com/kh/v=1000?z=%d&x=%d&y=%d", 0},
    {"arcgis", "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/%d/%d/%d", 1},
    {"carto_light", "https://d.basemaps.cartocdn.com/light_nolabels/%d/%d/%d.png", 0},
    {"carto_dark", "https://d.basemaps.cartocdn.com/dark_nolabels/%d/%d/%d.png", 0},
};
#define SIZE_PROVIDER (sizeof(PROVIDERS) / sizeof(PROVIDERS[0]))
// static const size_t SIZE_PROVIDER = sizeof(PROVIDERS) / sizeof(PROVIDERS[0]);

// ----------------------------------------------------------------------------
// Utils
// ----------------------------------------------------------------------------
#define UNUSED(x) (void)(x)

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

// Make "dir/subdir/subsubdir"
static void mkdir_recursive(const char *path_dir) {
    char buffer[1024];
    size_t len = strlen(path_dir);
    if (len >= sizeof(buffer)) {
        return;
    }

    strcpy(buffer, path_dir);
    for (size_t i = 1; i < len; ++i) {
        if (buffer[i] == '/' || buffer[i] == '\\') {
            char c = buffer[i];
            buffer[i] = '\0';
            MKDIR(buffer);
            buffer[i] = c;
        }
    }
    MKDIR(buffer);
}

static void ensure_cache_dir(int provider, int zoom, int x) {
    // Ensure directory exists
    // Cache path: tilecache/provider/z/x
    char dir[512];
    snprintf(dir, sizeof(dir), "%s%c%s%c%d%c%d", CACHE_ROOT, PATH_SEP, PROVIDERS[provider].type, PATH_SEP, zoom, PATH_SEP, x);
    mkdir_recursive(dir);
}

// libcurl write callback: write to FILE*
static size_t curl_write(void *ptr, size_t size, size_t nmemb, void *userdata) {
    FILE *file = (FILE *)userdata;
    return fwrite(ptr, size, nmemb, file);
}

static int download_tile(int provider, int z, int x, int y, const char *out_path) {
    CURL *curl = curl_easy_init();
    if (!curl)
        return -1;

    char url[256];
    int a, b;
    if (PROVIDERS[provider].swap_xy) {
        a = y;
        b = x;
    } else {
        a = x;
        b = y;
    }
    snprintf(url, sizeof(url), PROVIDERS[provider].url, z, a, b);

    ensure_cache_dir(provider, z, x);

    FILE *file = fopen(out_path, "wb");
    if (!file) {
        curl_easy_cleanup(curl);
        return -2;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, CURL_USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
    // A short timeout keeps things snappy in demos
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    fclose(file);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        // Remove possibly empty/partial file
        remove(out_path);
        return -3;
    }
    return 0;
}

// ----------------------------------------------------------------------------
// Web Mercator helpers
// ----------------------------------------------------------------------------

// lon/lat to global pixel coords at zoom z
static void lonlat_to_pixels(double lon_deg, double lat_deg, int z, double *px, double *py) {
    double lat = fmax(fmin(lat_deg, 85.05112878), -85.05112878); // clamp to Web Mercator
    double n = (double)(1 << z);
    double x = (lon_deg + 180.0) / 360.0;
    double s = sin(lat * M_PI / 180.0);
    double y = 0.5 - log((1 + s) / (1 - s)) / (4 * M_PI);
    *px = x * n * TILE_SIZE;
    *py = y * n * TILE_SIZE;
}

static void pixels_to_lonlat(double px, double py, int z, double *lon_deg, double *lat_deg) {
    double n = (double)(1 << z);
    double x = px / (n * TILE_SIZE);
    double y = py / (n * TILE_SIZE);
    *lon_deg = x * 360.0 - 180.0;
    double a = M_PI * (1 - 2 * y);
    *lat_deg = 180.0 / M_PI * atan(0.5 * (exp(a) - exp(-a)));
}

// positive modulo
static int imod(int a, int m) {
    int r = a % m;
    return r < 0 ? r + m : r;
}

// ----------------------------------------------------------------------------
// Async Download Task struct
// ----------------------------------------------------------------------------
typedef struct TileJob {
    int provider, z, x, y;
    char path[512];
    struct TileJob *next;
} TileJob;

static TileJob *job_head = NULL, *job_tail = NULL;
static SDL_mutex *job_mutex;
static SDL_cond *job_cond;
static int downloader_running = 1;

static void enqueue_job(int provider, int z, int x, int y, const char *path) {
    SDL_LockMutex(job_mutex);
    for (TileJob *t = job_head; t; t = t->next) {
        if (t->z == z && t->x == x && t->y == y) {
            SDL_UnlockMutex(job_mutex);
            return;
        }
    }

    TileJob *job = malloc(sizeof(TileJob));
    job->provider = provider;
    job->z = z;
    job->x = x;
    job->y = y;
    strncpy(job->path, path, sizeof(job->path));
    job->next = NULL;

    if (!job_tail) {
        job_head = job_tail = job;
    } else {
        job_tail->next = job;
        job_tail = job;
    }

    SDL_CondSignal(job_cond);
    SDL_UnlockMutex(job_mutex);
}

static TileJob *dequeue_job(void) {
    SDL_LockMutex(job_mutex);
    while (!job_head && downloader_running) {
        SDL_CondWait(job_cond, job_mutex);
    }

    TileJob *job = NULL;
    if (job_head) {
        job = job_head;
        job_head = job_head->next;
        if (!job_head) {
            job_tail = NULL;
        }
    }
    SDL_UnlockMutex(job_mutex);

    return job;
}

static int downloader_thread(void *arg) {
    UNUSED(arg);

    while (downloader_running) {
        TileJob *job = dequeue_job();
        if (!job) {
            continue;
        }
        if (!file_exists(job->path)) {
            download_tile(job->provider, job->z, job->x, job->y, job->path);
        }
        free(job);
    }

    return 0;
}

// ----------------------------------------------------------------------------
// Texture cache
// ----------------------------------------------------------------------------

typedef struct TileCache {
    int z, x, y;
    SDL_Texture *texture;
    struct TileCache *next;
} TileCache;

static TileCache *TILE_CACHE[SIZE_PROVIDER];

static void init_tile_cache() {
    for (int i = 0; i < SIZE_PROVIDER; i++) {
        TILE_CACHE[i] = NULL;
    }
}

static void cleanup_tile_cache() {
    for (int i = 0; i < SIZE_PROVIDER; i++) {
        TileCache *p = TILE_CACHE[i];

        while (p != NULL) {
            TileCache *n = p->next;
            p->next = NULL;
            SDL_DestroyTexture(p->texture);
            free(p);
            p = n;
        }

        TILE_CACHE[i] = NULL;
    }
}

static SDL_Texture *get_tile_texture(SDL_Renderer *renderer, int provider, int zoom, int x, int y) {
    TileCache *p = TILE_CACHE[provider];

    do {
        if (p == NULL) {
            break;
        } else if (p->z == zoom && p->x == x && p->y == y) {
            // printf("[cache hit] %d (%d, %d), %x, >%x\n", zoom, x, y, p->texture, p->next);
            return p->texture;
        }
    } while ((p = p->next) != NULL);

    // Cache path: tilecache/provider/z/x/y.png
    char tile_path[512];
    snprintf(tile_path, sizeof(tile_path), "%s%c%s%c%d%c%d%c%d.png",
             CACHE_ROOT, PATH_SEP, PROVIDERS[provider].type, PATH_SEP, zoom, PATH_SEP, x, PATH_SEP, y);

    if (!file_exists(tile_path)) {
        ensure_cache_dir(provider, zoom, x);
        enqueue_job(provider, zoom, x, y, tile_path);
    }

    SDL_Texture *texture = IMG_LoadTexture(renderer, tile_path);
    if (!texture) {
        // printf("[missing] %d (%d, %d)\n", zoom, x, y);
        return NULL;
    }

    TileCache *entry = (TileCache *)malloc(sizeof(TileCache));
    entry->z = zoom;
    entry->x = x;
    entry->y = y;
    entry->texture = texture;
    entry->next = NULL;

    if (TILE_CACHE[provider] == NULL) {
        TILE_CACHE[provider] = entry;
    } else {
        TileCache *c = TILE_CACHE[provider];
        while (c->next != NULL) {
            c = c->next;
        }
        c->next = entry;
    }
    // printf("[cache miss] %d (%d, %d)\n", zoom, x, y);

    return texture;
}

// ----------------------------------------------------------------------------
// Main
// ----------------------------------------------------------------------------

int main(int argc, char **argv) {
    UNUSED(argc);
    UNUSED(argv);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    int img_flags = IMG_INIT_PNG;
    if ((IMG_Init(img_flags) & img_flags) != img_flags) {
        fprintf(stderr, "IMG_Init: %s\n", IMG_GetError());
        return 1;
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        fprintf(stderr, "curl_global_init failed\n");
        return 1;
    }

    init_tile_cache();

    // downloader thread
    job_mutex = SDL_CreateMutex();
    job_cond = SDL_CreateCond();
    SDL_Thread *worker = SDL_CreateThread(downloader_thread, "tile_downloader", NULL);

    SDL_Window *window = SDL_CreateWindow(APP_TITLE,
                                          SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          WIN_W, WIN_H, SDL_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return 1;
    }

    double center_lon = INITIAL_LONGITUDE;
    double center_lat = INITIAL_LATITUDE;
    int zoom = INITIAL_ZOOM;

    double center_px, center_py;
    lonlat_to_pixels(center_lon, center_lat, zoom, &center_px, &center_py);

    int provider = 0;
    int running = 1;
    int dragging = 0;
    int win_w = WIN_W, win_h = WIN_H;
    int last_x = 0, last_y = 0;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT)
                running = 0;
            else if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.sym == SDLK_ESCAPE)
                    running = 0;
                else if (e.key.keysym.sym == SDLK_EQUALS || e.key.keysym.sym == SDLK_PLUS) {
                    if (zoom < MAX_ZOOM) {
                        // zoom toward center of window
                        double lon, lat;
                        pixels_to_lonlat(center_px, center_py, zoom, &lon, &lat);
                        zoom++;
                        lonlat_to_pixels(lon, lat, zoom, &center_px, &center_py);
                    }
                } else if (e.key.keysym.sym == SDLK_MINUS) {
                    if (zoom > MIN_ZOOM) {
                        double lon, lat;
                        pixels_to_lonlat(center_px, center_py, zoom, &lon, &lat);
                        zoom--;
                        lonlat_to_pixels(lon, lat, zoom, &center_px, &center_py);
                    }
                } else if (e.key.keysym.sym == SDLK_LEFT) {
                    center_px -= 200;
                } else if (e.key.keysym.sym == SDLK_RIGHT) {
                    center_px += 200;
                } else if (e.key.keysym.sym == SDLK_UP) {
                    center_py -= 200;
                } else if (e.key.keysym.sym == SDLK_DOWN) {
                    center_py += 200;
                } else if (e.key.keysym.sym == SDLK_1) {
                    provider = 0;
                } else if (e.key.keysym.sym == SDLK_2) {
                    provider = 1;
                } else if (e.key.keysym.sym == SDLK_3) {
                    provider = 2;
                } else if (e.key.keysym.sym == SDLK_4) {
                    provider = 3;
                } else if (e.key.keysym.sym == SDLK_5) {
                    provider = 4;
                }
            } else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                win_w = e.window.data1;
                win_h = e.window.data2;
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                dragging = 1;
                last_x = e.button.x;
                last_y = e.button.y;
            } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
                dragging = 0;
            } else if (e.type == SDL_MOUSEMOTION && dragging) {
                int dx = e.motion.x - last_x;
                int dy = e.motion.y - last_y;
                center_px -= dx;
                center_py -= dy;
                last_x = e.motion.x;
                last_y = e.motion.y;
            } else if (e.type == SDL_MOUSEWHEEL) {
                int zoom_dir = (e.wheel.y > 0) ? 1 : (e.wheel.y < 0 ? -1 : 0);
                if (zoom_dir != 0) {
                    int new_zoom = zoom + zoom_dir;
                    if (new_zoom >= MIN_ZOOM && new_zoom <= MAX_ZOOM) {
                        // zoom around mouse position
                        int mx, my;
                        SDL_GetMouseState(&mx, &my);
                        double world_px = center_px - win_w / 2.0 + mx;
                        double world_py = center_py - win_h / 2.0 + my;

                        double lon, lat;
                        pixels_to_lonlat(world_px, world_py, zoom, &lon, &lat);
                        zoom = new_zoom;
                        lonlat_to_pixels(lon, lat, zoom, &world_px, &world_py);

                        // keep the mouse pointing at same world point
                        center_px = world_px - mx + win_w / 2.0;
                        center_py = world_py - my + win_h / 2.0;
                    }
                }
            }
        }

        // Clear
        SDL_SetRenderDrawColor(renderer, 230, 230, 230, 255);
        SDL_RenderClear(renderer);

        // Draw tiles covering the window
        int n = (1 << zoom);
        double world_w = (double)n * TILE_SIZE;
        // Wrap X so we can pan infinitely horizontally
        if (center_px < 0) {
            center_px += world_w;
        }
        if (center_px >= world_w) {
            center_px -= world_w;
        }

        double left_px = center_px - win_w / 2.0;
        double top_px = center_py - win_h / 2.0;

        int first_tx = (int)floor(left_px / TILE_SIZE);
        int first_ty = (int)floor(top_px / TILE_SIZE);
        int tiles_x = win_w / TILE_SIZE + 3;
        int tiles_y = win_h / TILE_SIZE + 3;

        for (int row = 0; row < tiles_y; ++row) {
            for (int col = 0; col < tiles_x; ++col) {
                int tx = first_tx + col;
                int ty = first_ty + row;

                // Pixel position on screen
                double tile_origin_x = tx * TILE_SIZE - left_px;
                double tile_origin_y = ty * TILE_SIZE - top_px;

                // World wrap X
                int wx = imod(tx, n);
                // Clamp Y (no wrap at poles)
                if (ty < 0 || ty >= n)
                    continue;
                int wy = ty;

                SDL_Texture *texture = get_tile_texture(renderer, provider, zoom, wx, wy);
                if (!texture) {
                    // Draw a placeholder if missing
                    SDL_Rect r = {(int)tile_origin_x, (int)tile_origin_y, TILE_SIZE, TILE_SIZE};
                    SDL_SetRenderDrawColor(renderer, 200, 200, 200, 255);
                    SDL_RenderFillRect(renderer, &r);
                    SDL_SetRenderDrawColor(renderer, 150, 150, 150, 255);
                    SDL_RenderDrawRect(renderer, &r);
                    continue;
                } else {
                    SDL_Rect dst = {(int)tile_origin_x, (int)tile_origin_y, TILE_SIZE, TILE_SIZE};
                    SDL_RenderCopy(renderer, texture, NULL, &dst);
                }
            }
        }

        // HUD: zoom and center lat/lon
        double hud_lon, hud_lat;
        pixels_to_lonlat(center_px, center_py, zoom, &hud_lon, &hud_lat);
        // (No font dependency: just print to stdout occasionally)
        static Uint32 lastPrint = 0;
        Uint32 now = SDL_GetTicks();
        if (now - lastPrint > 1000) {
            fprintf(stdout, "Zoom %d | Center: lon %.5f lat %.5f | Provider %s\n", zoom, hud_lon, hud_lat, PROVIDERS[provider].type);
            lastPrint = now;
        }

        SDL_RenderPresent(renderer);
    }

    // turn off downloader thread
    SDL_LockMutex(job_mutex);
    downloader_running = 0;
    SDL_CondSignal(job_cond);
    SDL_UnlockMutex(job_mutex);
    int status;
    SDL_WaitThread(worker, &status);
    SDL_DestroyMutex(job_mutex);
    SDL_DestroyCond(job_cond);

    // cleanup rest
    cleanup_tile_cache();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    IMG_Quit();
    curl_global_cleanup();
    SDL_Quit();

    return 0;
}
