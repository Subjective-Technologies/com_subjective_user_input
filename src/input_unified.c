/*
 * input_unified.c - Unified Input Client for Multi-Monitor KVM
 * 
 * A cross-platform KVM client that captures keyboard/mouse input on a "main"
 * computer and sends it via WebSocket to other "player" computers.
 * 
 * Supports two roles:
 * - 'main': Captures and sends input to server, also receives/executes input when active
 * - 'player': Only receives and executes input when active
 * 
 * Compilation:
 *   Linux:   gcc -o input_unified input_unified.c -lwebsockets -lssl -lcrypto -lX11 -lXtst -lXrandr -lpthread -lm
 *   macOS:   clang -o input_unified input_unified.c -lwebsockets -lssl -lcrypto -framework CoreFoundation -framework IOKit -framework ApplicationServices -framework Carbon
 *   Windows: gcc -o input_unified.exe input_unified.c -lwebsockets -lssl -lcrypto -lws2_32 -luser32
 * 
 * Dependencies:
 *   - libwebsockets (https://libwebsockets.org/)
 *   - OpenSSL
 *   - X11/XTest/Xrandr (Linux only)
 * 
 * Author: Generated for subjective_kvm project
 * License: MIT
 */

/* _GNU_SOURCE is defined via compiler flag (-D_GNU_SOURCE) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <math.h>

/* Cross-compiler attribute macros */
#ifdef _MSC_VER
    #define UNUSED(x) ((void)(x))
    #define ATTRIBUTE_UNUSED
#else
    #define UNUSED(x) ((void)(x))
    #define ATTRIBUTE_UNUSED __attribute__((unused))
#endif

/* ============================================================================
 * Platform Detection
 * ============================================================================ */

#if defined(_WIN32) || defined(_WIN64)
    #define PLATFORM_WINDOWS 1
    #define PLATFORM_NAME "Windows"
#elif defined(__APPLE__) && defined(__MACH__)
    #define PLATFORM_MACOS 1
    #define PLATFORM_NAME "macOS"
#elif defined(__linux__)
    #define PLATFORM_LINUX 1
    #define PLATFORM_NAME "Linux"
#else
    #error "Unsupported platform"
#endif

/* ============================================================================
 * Platform-Specific Includes
 * ============================================================================ */

#ifdef PLATFORM_LINUX
    #include <unistd.h>
    #include <fcntl.h>
    #include <dirent.h>
    #include <sys/select.h>
    #include <sys/time.h>
    #include <sys/ioctl.h>
    #include <sys/stat.h>
    #include <linux/input.h>
    #include <linux/uinput.h>
    #include <X11/Xlib.h>
    #include <X11/Xutil.h>
    #include <X11/extensions/XTest.h>
    #include <X11/extensions/Xrandr.h>
    #include <X11/extensions/Xfixes.h>
    #include <X11/extensions/record.h>
    #include <X11/keysym.h>
    #include <X11/cursorfont.h>
    #include <X11/XKBlib.h>
    #include <pthread.h>
    #include <netdb.h>
    #include <arpa/inet.h>
    #include <sys/socket.h>
    #include <sys/wait.h>
#endif

#ifdef PLATFORM_WINDOWS
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <process.h>
    #include <io.h>
    #include <direct.h>
    #include <pthread.h>  /* pthreads4w from vcpkg */
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "user32.lib")
    #pragma comment(lib, "pthreadVC3.lib")  /* pthreads4w library */
#endif

#ifdef PLATFORM_MACOS
    #include <unistd.h>
    #include <pthread.h>
    #include <sys/select.h>
    #include <sys/time.h>
    #include <netdb.h>
    #include <arpa/inet.h>
    #include <sys/socket.h>
    #include <CoreFoundation/CoreFoundation.h>
    #include <CoreGraphics/CoreGraphics.h>
    #include <ApplicationServices/ApplicationServices.h>
    /* Note: IOKit HID requires more complex setup */
#endif

/* libwebsockets for WebSocket support */
#include <libwebsockets.h>

/* ============================================================================
 * Constants and Configuration
 * ============================================================================ */

#define VERSION "1.0.0"
#define MAX_MONITORS 16
#define MAX_DEVICES 32
#define MAX_MESSAGE_SIZE 65536
#define MAX_HOSTNAME 256
#define MAX_URL 512
#define RECONNECT_DELAY_SEC 5
#define MAX_RECONNECT_ATTEMPTS 10

/* Input event buffer size */
#define EVENT_BUFFER_SIZE 256

/* Message queue size for thread-safe sending */
#define MSG_QUEUE_SIZE 256   /* Queue size (reduced since msgs are larger now) */
#define MSG_MAX_LEN 8192     /* 8KB max message length - needed for clipboard! */

/* Log levels */
typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO = 1,
    LOG_WARNING = 2,
    LOG_ERROR = 3
} LogLevel;

/* Logging structure */
typedef struct {
    FILE *log_file;
    LogLevel level;
    char log_filename[1024];
} Logger;

/* ============================================================================
 * Data Structures
 * ============================================================================ */

/* Monitor geometry */
typedef struct {
    char monitor_id[32];
    int x;
    int y;
    int width;
    int height;
} Monitor;

/* Client configuration */
typedef struct {
    char server_url[MAX_URL];
    char computer_id[MAX_HOSTNAME];
    char role[16];  /* "main" or "player" */
    int port;
    bool use_ssl;
    LogLevel log_level;
} ClientConfig;

/* Input event data */
typedef struct {
    char event_type[32];
    char action[16];
    char key[64];
    bool is_special;
    double x;
    double y;
    int dx;
    int dy;
    char button[32];
} InputEventData;

/* Thread-safe message queue for WebSocket sending */
typedef struct {
    char messages[MSG_QUEUE_SIZE][MSG_MAX_LEN];
    int head;
    int tail;
    int count;
#ifdef PLATFORM_LINUX
    pthread_mutex_t mutex;
#endif
#ifdef PLATFORM_WINDOWS
    CRITICAL_SECTION cs;
    bool cs_initialized;
#endif
} MessageQueue;

/* Client state */
typedef struct {
    ClientConfig config;
    
    /* Connection state */
    struct lws_context *ws_context;
    struct lws *ws_connection;
    bool connected;
    bool running;
    int reconnect_attempts;
    
    /* Active state */
    bool is_active;
    char active_monitor_computer[MAX_HOSTNAME];
    
    /* Monitor info */
    Monitor monitors[MAX_MONITORS];
    int monitor_count;
    
    /* Input state */
    bool executing_input;
    int skip_mouse_moves;
    double last_input_time;
    double last_x;
    double last_y;
    
    /* Connection health tracking */
    double last_server_message_time;
    
    /* Message buffer (legacy - for registration and layout messages) */
    char send_buffer[MAX_MESSAGE_SIZE];
    size_t send_buffer_len;
    bool has_pending_message;
    
    /* Thread-safe message queue for input events */
    MessageQueue msg_queue;
    
    /* Platform-specific handles */
#ifdef PLATFORM_LINUX
    Display *x_display;
    int evdev_fds[MAX_DEVICES];
    int evdev_count;
    bool input_grabbed;
    pthread_t input_thread;
    bool input_thread_running;
#endif

    /* Clipboard sharing */
    char last_clipboard[65536];  /* Last known clipboard content (max 64KB) */
    size_t last_clipboard_len;
    pthread_t clipboard_thread;
    bool clipboard_thread_running;
    
#ifdef PLATFORM_WINDOWS
    HHOOK keyboard_hook;
    HHOOK mouse_hook;
    HANDLE hook_thread;
    DWORD hook_thread_id;
    bool hook_thread_running;
#endif
    
#ifdef PLATFORM_MACOS
    CFMachPortRef event_tap;
    CFRunLoopSourceRef run_loop_source;
#endif
    
} ClientState;

/* Forward declarations (used across platforms) */
static void send_input_event(ClientState *client, const char *event_type,
                             const char *json_data);
static void* clipboard_monitor_thread(void *arg);

/* ============================================================================
 * Embedded Server State (for --role main server mode)
 * ============================================================================ */

#define MAX_SERVER_CLIENTS 16

/* Per-client session data for server mode */
typedef struct {
    char computer_id[MAX_HOSTNAME];
    char role[16];  /* "main" or "player" */
    Monitor monitors[MAX_MONITORS];
    int monitor_count;
    bool registered;
    struct lws *wsi;
    /* Receive buffer */
    char rx_buffer[MAX_MESSAGE_SIZE];
    size_t rx_len;
} ServerClient;

/* Per-connection session data (points into g_server.clients) */
typedef struct {
    ServerClient *client;
} ServerClientSession;

/* Server state */
typedef struct {
    bool running;
    bool is_server_mode;  /* true if running as embedded server */
    int port;
    struct lws_context *ws_context;

    /* Connected clients */
    ServerClient clients[MAX_SERVER_CLIENTS];
    int client_count;
    pthread_mutex_t clients_mutex;

    /* Active computer tracking */
    char active_computer_id[MAX_HOSTNAME];
    char active_monitor_id[32];
    double cursor_x;
    double cursor_y;

    /* Edge crossing debounce */
    double last_edge_crossing_time;
    char last_crossed_from_computer[MAX_HOSTNAME];
    char last_crossed_from_monitor[32];

    /* Broadcast message queue (thread-safe for hook callbacks) */
    MessageQueue broadcast_queue;
} ServerState;

/* Global server state */
static ServerState g_server = {0};

/* Global client state (needed for callbacks) */
static ClientState g_client;
static volatile sig_atomic_t g_shutdown = 0;

/* ============================================================================
 * Logging
 * ============================================================================ */

static const char* log_level_str[] = {"DEBUG", "INFO", "WARN", "ERROR"};

static Logger g_logger = {NULL, LOG_INFO, {0}};

static void log_message(LogLevel level, const char *fmt, ...) {
    if (level < g_client.config.log_level) return;
    
    time_t now;
    struct tm *tm_info;
    char timestamp[32];
    
    time(&now);
    tm_info = localtime(&now);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    
    va_list args;
    char message[1024];
    
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    
    /* Log to stderr */
    fprintf(stderr, "%s - %s - %s\n", timestamp, log_level_str[level], message);
    fflush(stderr);
    
    /* Log to file if available */
    if (g_logger.log_file) {
        fprintf(g_logger.log_file, "%s - %s - %s\n", timestamp, log_level_str[level], message);
        fflush(g_logger.log_file);  /* Flush immediately for sync */
    }
}

/* Load .env file from executable directory */
static void load_env_file(void) {
    FILE *env_file = NULL;
    char env_path[512];
    char line[1024];

    /* Try .env in current directory first */
    env_file = fopen(".env", "r");
    if (!env_file) {
        /* Try in executable directory */
#ifdef PLATFORM_WINDOWS
        char exe_path[512];
        if (GetModuleFileNameA(NULL, exe_path, sizeof(exe_path)) > 0) {
            /* Remove executable name to get directory */
            char *last_slash = strrchr(exe_path, '\\');
            if (last_slash) {
                *last_slash = '\0';
                snprintf(env_path, sizeof(env_path), "%s\\.env", exe_path);
                env_file = fopen(env_path, "r");
            }
        }
#else
        /* Try ../. env or ./.env */
        env_file = fopen("../.env", "r");
#endif
    }

    if (!env_file) {
        return;  /* No .env file found, use defaults */
    }

    fprintf(stderr, "Loading .env file...\n");

    while (fgets(line, sizeof(line), env_file)) {
        /* Skip comments and empty lines */
        char *trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
        if (*trimmed == '#' || *trimmed == '\n' || *trimmed == '\0') continue;

        /* Remove trailing newline */
        char *newline = strchr(trimmed, '\n');
        if (newline) *newline = '\0';
        char *cr = strchr(trimmed, '\r');
        if (cr) *cr = '\0';

        /* Parse KEY=VALUE */
        char *equals = strchr(trimmed, '=');
        if (!equals) continue;

        *equals = '\0';
        char *key = trimmed;
        char *value = equals + 1;

        /* Remove quotes from value if present */
        if (*value == '"' || *value == '\'') {
            char quote = *value;
            value++;
            char *end_quote = strrchr(value, quote);
            if (end_quote) *end_quote = '\0';
        }

        /* Set environment variable (don't override existing) */
        if (!getenv(key)) {
#ifdef PLATFORM_WINDOWS
            char env_str[1024];
            snprintf(env_str, sizeof(env_str), "%s=%s", key, value);
            _putenv(env_str);
#else
            setenv(key, value, 0);
#endif
            fprintf(stderr, "  Set %s=%s\n", key, value);
        }
    }

    fclose(env_file);
}

static int init_logging(const char *computer_name, const char *role) {

    /* Get timestamp in format YYYY_MM_DD_HH_MM_SS */
    time_t now;
    struct tm *tm_info;
    char timestamp[32];
    time(&now);
    tm_info = localtime(&now);
    strftime(timestamp, sizeof(timestamp), "%Y_%m_%d_%H_%M_%S", tm_info);
    
    /* Get logs directory from environment variable (can be set via .env file) */
    struct stat st;
    char log_dir[512];
    const char *env_logs_path = getenv("LOGS_FOLDER_PATH");

    if (env_logs_path && strlen(env_logs_path) > 0) {
        /* Use environment variable if set */
        strncpy(log_dir, env_logs_path, sizeof(log_dir) - 1);
        log_dir[sizeof(log_dir) - 1] = '\0';
    } else {
        /* Default to "logs" in current directory */
        strncpy(log_dir, "logs", sizeof(log_dir) - 1);
        log_dir[sizeof(log_dir) - 1] = '\0';
    }
    
    /* Check if logs directory exists (could be symlink) */
    if (stat(log_dir, &st) != 0) {
        /* If using default "logs", try parent directory (if running from cinput/) */
        if (!env_logs_path || strlen(env_logs_path) == 0) {
            char parent_logs[512];
            snprintf(parent_logs, sizeof(parent_logs), "../logs");
            if (stat(parent_logs, &st) == 0) {
                strncpy(log_dir, parent_logs, sizeof(log_dir) - 1);
                log_dir[sizeof(log_dir) - 1] = '\0';  /* Ensure null termination */
            } else {
                /* Create logs directory */
#ifdef PLATFORM_WINDOWS
                _mkdir(log_dir);
#else
                mkdir(log_dir, 0755);
#endif
            }
        } else {
            /* Environment variable path specified, create it if it doesn't exist */
#ifdef PLATFORM_WINDOWS
            _mkdir(log_dir);
#else
            mkdir(log_dir, 0755);
#endif
        }
    }
    
    /* Generate log filename: YYYY_MM_DD_HH_MM_SS_virtualglass_input-[role]-[pc_name].log */
    snprintf(g_logger.log_filename, sizeof(g_logger.log_filename),
             "%s/%s_virtualglass_input-%s-%s.log", log_dir, timestamp, role, computer_name);
    
    /* Open log file */
    g_logger.log_file = fopen(g_logger.log_filename, "a");
    if (!g_logger.log_file) {
        fprintf(stderr, "Warning: Could not open log file: %s (%s)\n", 
                g_logger.log_filename, strerror(errno));
        return -1;
    }
    
    /* Log initial messages (these will go to stderr only since file isn't set yet in log_message) */
    fprintf(stderr, "Logging to: %s\n", g_logger.log_filename);
    fprintf(stderr, "Computer: %s\n", computer_name);
    if (g_logger.log_file) {
        time_t now2;
        struct tm *tm_info2;
        char timestamp2[32];
        time(&now2);
        tm_info2 = localtime(&now2);
        strftime(timestamp2, sizeof(timestamp2), "%Y-%m-%d %H:%M:%S", tm_info2);
        fprintf(g_logger.log_file, "%s - INFO - Logging to: %s\n", timestamp2, g_logger.log_filename);
        fprintf(g_logger.log_file, "%s - INFO - Computer: %s\n", timestamp2, computer_name);
        fflush(g_logger.log_file);
    }
    
    return 0;
}

static void cleanup_logging(void) {
    if (g_logger.log_file) {
        fclose(g_logger.log_file);
        g_logger.log_file = NULL;
    }
}

#define LOG_DEBUG(...) log_message(LOG_DEBUG, __VA_ARGS__)
#define LOG_INFO(...) log_message(LOG_INFO, __VA_ARGS__)
#define LOG_WARN(...) log_message(LOG_WARNING, __VA_ARGS__)
#define LOG_ERROR(...) log_message(LOG_ERROR, __VA_ARGS__)

/* ============================================================================
 * Thread-Safe Message Queue for WebSocket Sending
 * ============================================================================ */

static void msg_queue_init(MessageQueue *q) {
    q->head = 0;
    q->tail = 0;
    q->count = 0;
#ifdef PLATFORM_LINUX
    pthread_mutex_init(&q->mutex, NULL);
#endif
#ifdef PLATFORM_WINDOWS
    InitializeCriticalSection(&q->cs);
    q->cs_initialized = true;
#endif
}

static void msg_queue_destroy(MessageQueue *q) {
#ifdef PLATFORM_LINUX
    pthread_mutex_destroy(&q->mutex);
#endif
#ifdef PLATFORM_WINDOWS
    if (q->cs_initialized) {
        DeleteCriticalSection(&q->cs);
        q->cs_initialized = false;
    }
#endif
}

/* Add message to queue (thread-safe). Returns true on success. */
static bool msg_queue_push(MessageQueue *q, const char *msg) {
    bool success = false;
    size_t len = strlen(msg);

#ifdef PLATFORM_LINUX
    pthread_mutex_lock(&q->mutex);
#endif
#ifdef PLATFORM_WINDOWS
    if (q->cs_initialized) EnterCriticalSection(&q->cs);
#endif

    if (len >= MSG_MAX_LEN) {
        /* Message too large - log and drop */
        static int too_large_count = 0;
        too_large_count++;
        LOG_WARN("Message too large for queue: %zu bytes (max %d) - dropped #%d",
                 len, MSG_MAX_LEN, too_large_count);
    } else if (q->count < MSG_QUEUE_SIZE) {
        strncpy(q->messages[q->tail], msg, MSG_MAX_LEN - 1);
        q->messages[q->tail][MSG_MAX_LEN - 1] = '\0';
        q->tail = (q->tail + 1) % MSG_QUEUE_SIZE;
        q->count++;
        success = true;
    }

#ifdef PLATFORM_LINUX
    pthread_mutex_unlock(&q->mutex);
#endif
#ifdef PLATFORM_WINDOWS
    if (q->cs_initialized) LeaveCriticalSection(&q->cs);
#endif
    return success;
}

/* Pop message from queue (thread-safe). Returns true if message was popped. */
static bool msg_queue_pop(MessageQueue *q, char *out_msg, size_t out_size) {
    bool success = false;
#ifdef PLATFORM_LINUX
    pthread_mutex_lock(&q->mutex);
#endif
#ifdef PLATFORM_WINDOWS
    if (q->cs_initialized) EnterCriticalSection(&q->cs);
#endif

    if (q->count > 0) {
        strncpy(out_msg, q->messages[q->head], out_size - 1);
        out_msg[out_size - 1] = '\0';
        q->head = (q->head + 1) % MSG_QUEUE_SIZE;
        q->count--;
        success = true;
    }

#ifdef PLATFORM_LINUX
    pthread_mutex_unlock(&q->mutex);
#endif
#ifdef PLATFORM_WINDOWS
    if (q->cs_initialized) LeaveCriticalSection(&q->cs);
#endif
    return success;
}

/* Check if queue has messages (thread-safe) */
static bool msg_queue_has_messages(MessageQueue *q) {
    bool has;
#ifdef PLATFORM_LINUX
    pthread_mutex_lock(&q->mutex);
#endif
#ifdef PLATFORM_WINDOWS
    if (q->cs_initialized) EnterCriticalSection(&q->cs);
#endif
    has = (q->count > 0);
#ifdef PLATFORM_LINUX
    pthread_mutex_unlock(&q->mutex);
#endif
#ifdef PLATFORM_WINDOWS
    if (q->cs_initialized) LeaveCriticalSection(&q->cs);
#endif
    return has;
}

/* Get queue count (thread-safe) */
static int msg_queue_count(MessageQueue *q) {
    int count;
#ifdef PLATFORM_LINUX
    pthread_mutex_lock(&q->mutex);
#endif
#ifdef PLATFORM_WINDOWS
    if (q->cs_initialized) EnterCriticalSection(&q->cs);
#endif
    count = q->count;
#ifdef PLATFORM_LINUX
    pthread_mutex_unlock(&q->mutex);
#endif
#ifdef PLATFORM_WINDOWS
    if (q->cs_initialized) LeaveCriticalSection(&q->cs);
#endif
    return count;
}

/* Clear queue (thread-safe) - drops all pending messages */
static void msg_queue_clear(MessageQueue *q) {
#ifdef PLATFORM_LINUX
    pthread_mutex_lock(&q->mutex);
#endif
#ifdef PLATFORM_WINDOWS
    if (q->cs_initialized) EnterCriticalSection(&q->cs);
#endif
    int cleared = q->count;
    q->head = 0;
    q->tail = 0;
    q->count = 0;
#ifdef PLATFORM_LINUX
    pthread_mutex_unlock(&q->mutex);
#endif
#ifdef PLATFORM_WINDOWS
    if (q->cs_initialized) LeaveCriticalSection(&q->cs);
#endif
    if (cleared > 0) {
        LOG_INFO("Cleared %d stale messages from queue", cleared);
    }
}

/* ============================================================================
 * Simple JSON Parser (minimal implementation)
 * ============================================================================ */

/* JSON value types */
typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;
typedef struct JsonPair JsonPair;

struct JsonValue {
    JsonType type;
    union {
        bool boolean;
        double number;
        char *string;
        struct {
            JsonValue **items;
            size_t count;
        } array;
        struct {
            JsonPair *pairs;
            size_t count;
        } object;
    } value;
};

struct JsonPair {
    char *key;
    JsonValue *value;
};

/* Forward declarations */
static JsonValue* json_parse(const char *str);
static void json_free(JsonValue *val);
/* static char* json_stringify(JsonValue *val); - not used */
static JsonValue* json_get(JsonValue *obj, const char *key);
static const char* json_get_string(JsonValue *obj, const char *key, const char *def);
static double json_get_number(JsonValue *obj, const char *key, double def);
static bool json_get_bool(JsonValue *obj, const char *key, bool def);

/* Skip whitespace */
static const char* json_skip_ws(const char *str) {
    while (*str && (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r')) str++;
    return str;
}

/* Decode Unicode escape sequence (\\uXXXX) */
static int json_decode_unicode(const char *hex_str, unsigned int *out) {
    unsigned int val = 0;
    for (int i = 0; i < 4; i++) {
        char c = hex_str[i];
        if (c >= '0' && c <= '9') {
            val = (val << 4) | (c - '0');
        } else if (c >= 'a' && c <= 'f') {
            val = (val << 4) | (c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            val = (val << 4) | (c - 'A' + 10);
        } else {
            return -1;
        }
    }
    *out = val;
    return 0;
}

/* Parse string with Unicode escape decoding */
static const char* json_parse_string(const char *str, char **out) {
    if (*str != '"') return NULL;
    str++;
    
    /* Find end of string */
    const char *end = str;
    while (*end && *end != '"') {
        if (*end == '\\' && *(end+1) == '"') {
            end += 2;  /* Skip escaped quote */
        } else {
            end++;
        }
    }
    if (*end != '"') return NULL;
    
    /* Allocate buffer (generous size for UTF-8 expansion) */
    size_t max_len = (end - str) * 4 + 1;  /* Unicode can expand to 4 bytes */
    *out = malloc(max_len);
    if (!*out) return NULL;
    
    char *dst = *out;
    const char *p = str;
    
    while (*p && p < end) {
        if (*p == '\\' && *(p+1)) {
            p++;  /* Skip backslash */
            if (*p == 'u' && p + 4 < end) {
                /* Decode Unicode escape: \uXXXX */
                unsigned int code;
                if (json_decode_unicode(p + 1, &code) == 0) {
                    /* Convert to UTF-8 */
                    if (code < 0x80) {
                        *dst++ = (char)code;
                    } else if (code < 0x800) {
                        *dst++ = (char)(0xC0 | (code >> 6));
                        *dst++ = (char)(0x80 | (code & 0x3F));
                    } else if (code < 0x10000) {
                        *dst++ = (char)(0xE0 | (code >> 12));
                        *dst++ = (char)(0x80 | ((code >> 6) & 0x3F));
                        *dst++ = (char)(0x80 | (code & 0x3F));
                    } else {
                        *dst++ = (char)(0xF0 | (code >> 18));
                        *dst++ = (char)(0x80 | ((code >> 12) & 0x3F));
                        *dst++ = (char)(0x80 | ((code >> 6) & 0x3F));
                        *dst++ = (char)(0x80 | (code & 0x3F));
                    }
                    p += 5;  /* Skip uXXXX */
                } else {
                    /* Invalid Unicode, copy as-is */
                    *dst++ = '\\';
                    *dst++ = *p++;
                }
            } else {
                /* Regular escape */
                switch (*p) {
                    case 'n': *dst++ = '\n'; break;
                    case 't': *dst++ = '\t'; break;
                    case 'r': *dst++ = '\r'; break;
                    case '"': *dst++ = '"'; break;
                    case '\\': *dst++ = '\\'; break;
                    default: *dst++ = '\\'; *dst++ = *p; break;
                }
                p++;
            }
        } else {
            *dst++ = *p++;
        }
    }
    *dst = '\0';
    
    return end + 1;  /* Skip closing quote */
}

/* Parse number */
static const char* json_parse_number(const char *str, double *out) {
    char *end;
    *out = strtod(str, &end);
    return (end > str) ? end : NULL;
}

/* Forward declare parse_value */
static const char* json_parse_value(const char *str, JsonValue **out);

/* Parse array */
static const char* json_parse_array(const char *str, JsonValue **out) {
    if (*str != '[') return NULL;
    str = json_skip_ws(str + 1);
    
    *out = calloc(1, sizeof(JsonValue));
    (*out)->type = JSON_ARRAY;
    
    size_t capacity = 8;
    (*out)->value.array.items = malloc(capacity * sizeof(JsonValue*));
    (*out)->value.array.count = 0;
    
    while (*str && *str != ']') {
        if ((*out)->value.array.count >= capacity) {
            capacity *= 2;
            (*out)->value.array.items = realloc((*out)->value.array.items, 
                                                  capacity * sizeof(JsonValue*));
        }
        
        JsonValue *item;
        str = json_parse_value(str, &item);
        if (!str) { json_free(*out); *out = NULL; return NULL; }
        
        (*out)->value.array.items[(*out)->value.array.count++] = item;
        
        str = json_skip_ws(str);
        if (*str == ',') str = json_skip_ws(str + 1);
    }
    
    return (*str == ']') ? str + 1 : NULL;
}

/* Parse object */
static const char* json_parse_object(const char *str, JsonValue **out) {
    if (*str != '{') return NULL;
    str = json_skip_ws(str + 1);
    
    *out = calloc(1, sizeof(JsonValue));
    (*out)->type = JSON_OBJECT;
    
    size_t capacity = 8;
    (*out)->value.object.pairs = malloc(capacity * sizeof(JsonPair));
    (*out)->value.object.count = 0;
    
    while (*str && *str != '}') {
        if ((*out)->value.object.count >= capacity) {
            capacity *= 2;
            (*out)->value.object.pairs = realloc((*out)->value.object.pairs,
                                                   capacity * sizeof(JsonPair));
        }
        
        JsonPair *pair = &(*out)->value.object.pairs[(*out)->value.object.count];
        
        /* Parse key */
        str = json_skip_ws(str);
        str = json_parse_string(str, &pair->key);
        if (!str) { json_free(*out); *out = NULL; return NULL; }
        
        /* Skip colon */
        str = json_skip_ws(str);
        if (*str != ':') { json_free(*out); *out = NULL; return NULL; }
        str = json_skip_ws(str + 1);
        
        /* Parse value */
        str = json_parse_value(str, &pair->value);
        if (!str) { json_free(*out); *out = NULL; return NULL; }
        
        (*out)->value.object.count++;
        
        str = json_skip_ws(str);
        if (*str == ',') str = json_skip_ws(str + 1);
    }
    
    return (*str == '}') ? str + 1 : NULL;
}

/* Parse any value */
static const char* json_parse_value(const char *str, JsonValue **out) {
    str = json_skip_ws(str);
    
    if (*str == '"') {
        *out = calloc(1, sizeof(JsonValue));
        (*out)->type = JSON_STRING;
        return json_parse_string(str, &(*out)->value.string);
    }
    
    if (*str == '{') {
        return json_parse_object(str, out);
    }
    
    if (*str == '[') {
        return json_parse_array(str, out);
    }
    
    if (*str == 't' && strncmp(str, "true", 4) == 0) {
        *out = calloc(1, sizeof(JsonValue));
        (*out)->type = JSON_BOOL;
        (*out)->value.boolean = true;
        return str + 4;
    }
    
    if (*str == 'f' && strncmp(str, "false", 5) == 0) {
        *out = calloc(1, sizeof(JsonValue));
        (*out)->type = JSON_BOOL;
        (*out)->value.boolean = false;
        return str + 5;
    }
    
    if (*str == 'n' && strncmp(str, "null", 4) == 0) {
        *out = calloc(1, sizeof(JsonValue));
        (*out)->type = JSON_NULL;
        return str + 4;
    }
    
    if (*str == '-' || (*str >= '0' && *str <= '9')) {
        *out = calloc(1, sizeof(JsonValue));
        (*out)->type = JSON_NUMBER;
        return json_parse_number(str, &(*out)->value.number);
    }
    
    return NULL;
}

/* Parse JSON string */
static JsonValue* json_parse(const char *str) {
    JsonValue *val = NULL;
    json_parse_value(str, &val);
    return val;
}

/* Free JSON value */
static void json_free(JsonValue *val) {
    if (!val) return;
    
    switch (val->type) {
        case JSON_STRING:
            free(val->value.string);
            break;
        case JSON_ARRAY:
            for (size_t i = 0; i < val->value.array.count; i++) {
                json_free(val->value.array.items[i]);
            }
            free(val->value.array.items);
            break;
        case JSON_OBJECT:
            for (size_t i = 0; i < val->value.object.count; i++) {
                free(val->value.object.pairs[i].key);
                json_free(val->value.object.pairs[i].value);
            }
            free(val->value.object.pairs);
            break;
        default:
            break;
    }
    free(val);
}

/* Get object property */
static JsonValue* json_get(JsonValue *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    
    for (size_t i = 0; i < obj->value.object.count; i++) {
        if (strcmp(obj->value.object.pairs[i].key, key) == 0) {
            return obj->value.object.pairs[i].value;
        }
    }
    return NULL;
}

/* Get string property with default */
static const char* json_get_string(JsonValue *obj, const char *key, const char *def) {
    JsonValue *val = json_get(obj, key);
    if (val && val->type == JSON_STRING) return val->value.string;
    return def;
}

/* Get number property with default */
static double json_get_number(JsonValue *obj, const char *key, double def) {
    JsonValue *val = json_get(obj, key);
    if (val && val->type == JSON_NUMBER) return val->value.number;
    return def;
}

/* Get bool property with default */
static bool json_get_bool(JsonValue *obj, const char *key, bool def) {
    JsonValue *val = json_get(obj, key);
    if (val && val->type == JSON_BOOL) return val->value.boolean;
    return def;
}

/* String builder for JSON stringify */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} StringBuilder;

static void sb_init(StringBuilder *sb) {
    sb->cap = 256;
    sb->buf = malloc(sb->cap);
    sb->buf[0] = '\0';
    sb->len = 0;
}

static void sb_append(StringBuilder *sb, const char *str) {
    size_t slen = strlen(str);
    while (sb->len + slen + 1 > sb->cap) {
        sb->cap *= 2;
        sb->buf = realloc(sb->buf, sb->cap);
    }
    memcpy(sb->buf + sb->len, str, slen + 1);
    sb->len += slen;
}

static void sb_append_escaped(StringBuilder *sb, const char *str) {
    sb_append(sb, "\"");
    while (*str) {
        switch (*str) {
            case '"': sb_append(sb, "\\\""); break;
            case '\\': sb_append(sb, "\\\\"); break;
            case '\n': sb_append(sb, "\\n"); break;
            case '\r': sb_append(sb, "\\r"); break;
            case '\t': sb_append(sb, "\\t"); break;
            default: {
                char c[2] = {*str, '\0'};
                sb_append(sb, c);
            }
        }
        str++;
    }
    sb_append(sb, "\"");
}

/* json_stringify functions are not currently used but kept for potential future use */
#if 0
static void json_stringify_internal(StringBuilder *sb, JsonValue *val);

static void json_stringify_internal(StringBuilder *sb, JsonValue *val) {
    if (!val) { sb_append(sb, "null"); return; }
    
    char numbuf[64];
    
    switch (val->type) {
        case JSON_NULL:
            sb_append(sb, "null");
            break;
        case JSON_BOOL:
            sb_append(sb, val->value.boolean ? "true" : "false");
            break;
        case JSON_NUMBER:
            snprintf(numbuf, sizeof(numbuf), "%.2f", val->value.number);
            sb_append(sb, numbuf);
            break;
        case JSON_STRING:
            sb_append_escaped(sb, val->value.string);
            break;
        case JSON_ARRAY:
            sb_append(sb, "[");
            for (size_t i = 0; i < val->value.array.count; i++) {
                if (i > 0) sb_append(sb, ",");
                json_stringify_internal(sb, val->value.array.items[i]);
            }
            sb_append(sb, "]");
            break;
        case JSON_OBJECT:
            sb_append(sb, "{");
            for (size_t i = 0; i < val->value.object.count; i++) {
                if (i > 0) sb_append(sb, ",");
                sb_append_escaped(sb, val->value.object.pairs[i].key);
                sb_append(sb, ":");
                json_stringify_internal(sb, val->value.object.pairs[i].value);
            }
            sb_append(sb, "}");
            break;
    }
}
#endif  /* End of #if 0 for json_stringify_internal */

/* json_stringify function removed - not used in current implementation */
#if 0
static char* json_stringify(JsonValue *val) {
    StringBuilder sb;
    sb_init(&sb);
    json_stringify_internal(&sb, val);
    return sb.buf;
}
#endif

/* Helper to create JSON objects easily */
static char* json_create_message(const char *type, ...) {
    StringBuilder sb;
    sb_init(&sb);
    
    sb_append(&sb, "{\"type\":\"");
    sb_append(&sb, type);
    sb_append(&sb, "\"");
    
    va_list args;
    va_start(args, type);
    
    const char *key;
    while ((key = va_arg(args, const char*)) != NULL) {
        sb_append(&sb, ",\"");
        sb_append(&sb, key);
        sb_append(&sb, "\":");
        
        const char *val = va_arg(args, const char*);
        /* Check if value looks like a number, bool, or needs quotes */
        if (val[0] == '{' || val[0] == '[' || 
            strcmp(val, "true") == 0 || strcmp(val, "false") == 0 ||
            strcmp(val, "null") == 0 ||
            (val[0] >= '0' && val[0] <= '9') || val[0] == '-') {
            sb_append(&sb, val);
        } else {
            sb_append_escaped(&sb, val);
        }
    }
    
    va_end(args);
    
    sb_append(&sb, "}");
    return sb.buf;
}

/* ============================================================================
 * Time Utilities
 * ============================================================================ */

static double get_time_ms(void) {
#ifdef PLATFORM_WINDOWS
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
#endif
}

static void sleep_ms(int ms) {
#ifdef PLATFORM_WINDOWS
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

/* ============================================================================
 * Monitor Detection
 * ============================================================================ */

#ifdef PLATFORM_LINUX
static int detect_monitors_linux(Monitor *monitors, int max_monitors) {
    Display *display = XOpenDisplay(NULL);
    if (!display) {
        LOG_WARN("Could not open X display for monitor detection");
        /* Return default monitor */
        strcpy(monitors[0].monitor_id, "m0");
        monitors[0].x = 0;
        monitors[0].y = 0;
        monitors[0].width = 1920;
        monitors[0].height = 1080;
        return 1;
    }
    
    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);
    
    int count = 0;
    
    /* Try XRandR */
    int event_base, error_base;
    if (XRRQueryExtension(display, &event_base, &error_base)) {
        XRRScreenResources *res = XRRGetScreenResources(display, root);
        if (res) {
            for (int i = 0; i < res->noutput && count < max_monitors; i++) {
                XRROutputInfo *output = XRRGetOutputInfo(display, res, res->outputs[i]);
                if (output && output->connection == RR_Connected && output->crtc) {
                    XRRCrtcInfo *crtc = XRRGetCrtcInfo(display, res, output->crtc);
                    if (crtc) {
                        snprintf(monitors[count].monitor_id, sizeof(monitors[count].monitor_id), 
                                 "m%d", count);
                        monitors[count].x = crtc->x;
                        monitors[count].y = crtc->y;
                        monitors[count].width = crtc->width;
                        monitors[count].height = crtc->height;
                        
                        LOG_INFO("Detected monitor %s: %dx%d at (%d,%d)",
                                 monitors[count].monitor_id,
                                 monitors[count].width, monitors[count].height,
                                 monitors[count].x, monitors[count].y);
                        
                        count++;
                        XRRFreeCrtcInfo(crtc);
                    }
                }
                if (output) XRRFreeOutputInfo(output);
            }
            XRRFreeScreenResources(res);
        }
    }
    
    /* Fallback to single screen */
    if (count == 0) {
        strcpy(monitors[0].monitor_id, "m0");
        monitors[0].x = 0;
        monitors[0].y = 0;
        monitors[0].width = DisplayWidth(display, screen);
        monitors[0].height = DisplayHeight(display, screen);
        count = 1;
        
        LOG_INFO("Using default screen: %dx%d", 
                 monitors[0].width, monitors[0].height);
    }
    
    XCloseDisplay(display);
    return count;
}
#endif

#ifdef PLATFORM_WINDOWS
typedef struct {
    Monitor *monitors;
    int max_monitors;
    int count;
} MonitorEnumCtx;

static BOOL CALLBACK monitor_enum_proc(HMONITOR hMonitor, HDC hdc, LPRECT rect, LPARAM data) {
    UNUSED(hdc);
    UNUSED(rect);
    MonitorEnumCtx *ctx = (MonitorEnumCtx *)data;
    if (ctx->count >= ctx->max_monitors) {
        return FALSE;
    }

    MONITORINFOEX info;
    memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info);
    if (GetMonitorInfo(hMonitor, (MONITORINFO *)&info)) {
        RECT rc = info.rcMonitor;
        int idx = ctx->count;
        snprintf(ctx->monitors[idx].monitor_id,
                 sizeof(ctx->monitors[idx].monitor_id), "m%d", idx);
        ctx->monitors[idx].x = rc.left;
        ctx->monitors[idx].y = rc.top;
        ctx->monitors[idx].width = rc.right - rc.left;
        ctx->monitors[idx].height = rc.bottom - rc.top;

        LOG_INFO("Detected monitor %s: %dx%d at (%d,%d)",
                 ctx->monitors[idx].monitor_id,
                 ctx->monitors[idx].width,
                 ctx->monitors[idx].height,
                 ctx->monitors[idx].x,
                 ctx->monitors[idx].y);
        ctx->count++;
    }

    return TRUE;
}

static int detect_monitors_windows(Monitor *monitors, int max_monitors) {
    MonitorEnumCtx ctx = {monitors, max_monitors, 0};

    EnumDisplayMonitors(NULL, NULL, monitor_enum_proc, (LPARAM)&ctx);

    if (ctx.count == 0) {
        strcpy(monitors[0].monitor_id, "m0");
        monitors[0].x = 0;
        monitors[0].y = 0;
        monitors[0].width = GetSystemMetrics(SM_CXSCREEN);
        monitors[0].height = GetSystemMetrics(SM_CYSCREEN);
        LOG_INFO("Detected primary monitor: %dx%d", monitors[0].width, monitors[0].height);
        return 1;
    }

    return ctx.count;
}
#endif

#ifdef PLATFORM_MACOS
static int detect_monitors_macos(Monitor *monitors, int max_monitors) {
    /* Use CoreGraphics to detect displays */
    uint32_t display_count = 0;
    CGGetActiveDisplayList(0, NULL, &display_count);
    
    if (display_count == 0) {
        strcpy(monitors[0].monitor_id, "m0");
        monitors[0].x = 0;
        monitors[0].y = 0;
        monitors[0].width = 1920;
        monitors[0].height = 1080;
        return 1;
    }
    
    CGDirectDisplayID *displays = malloc(display_count * sizeof(CGDirectDisplayID));
    CGGetActiveDisplayList(display_count, displays, &display_count);
    
    int count = 0;
    for (uint32_t i = 0; i < display_count && count < max_monitors; i++) {
        CGRect bounds = CGDisplayBounds(displays[i]);
        
        snprintf(monitors[count].monitor_id, sizeof(monitors[count].monitor_id), "m%d", count);
        monitors[count].x = (int)bounds.origin.x;
        monitors[count].y = (int)bounds.origin.y;
        monitors[count].width = (int)bounds.size.width;
        monitors[count].height = (int)bounds.size.height;
        
        LOG_INFO("Detected monitor %s: %dx%d at (%d,%d)",
                 monitors[count].monitor_id,
                 monitors[count].width, monitors[count].height,
                 monitors[count].x, monitors[count].y);
        
        count++;
    }
    
    free(displays);
    return count;
}
#endif

static int detect_monitors(Monitor *monitors, int max_monitors) {
#ifdef PLATFORM_LINUX
    return detect_monitors_linux(monitors, max_monitors);
#elif defined(PLATFORM_WINDOWS)
    return detect_monitors_windows(monitors, max_monitors);
#elif defined(PLATFORM_MACOS)
    return detect_monitors_macos(monitors, max_monitors);
#else
    strcpy(monitors[0].monitor_id, "m0");
    monitors[0].x = 0;
    monitors[0].y = 0;
    monitors[0].width = 1920;
    monitors[0].height = 1080;
    return 1;
#endif
}

/* ============================================================================
 * Input Capture - Linux (evdev)
 * ============================================================================ */

#ifdef PLATFORM_LINUX

/* Key code to name mapping (matches Python version) */
typedef struct {
    int code;
    const char *name;
    bool is_special;
} KeyMapping;

static const KeyMapping key_mappings[] = {
    {KEY_ESC, "Key.esc", true},
    {KEY_1, "1", false}, {KEY_2, "2", false}, {KEY_3, "3", false},
    {KEY_4, "4", false}, {KEY_5, "5", false}, {KEY_6, "6", false},
    {KEY_7, "7", false}, {KEY_8, "8", false}, {KEY_9, "9", false},
    {KEY_0, "0", false},
    {KEY_MINUS, "-", false}, {KEY_EQUAL, "=", false},
    {KEY_BACKSPACE, "Key.backspace", true}, {KEY_TAB, "Key.tab", true},
    {KEY_Q, "q", false}, {KEY_W, "w", false}, {KEY_E, "e", false},
    {KEY_R, "r", false}, {KEY_T, "t", false}, {KEY_Y, "y", false},
    {KEY_U, "u", false}, {KEY_I, "i", false}, {KEY_O, "o", false},
    {KEY_P, "p", false},
    {KEY_LEFTBRACE, "[", false}, {KEY_RIGHTBRACE, "]", false},
    {KEY_ENTER, "Key.enter", true},
    {KEY_LEFTCTRL, "Key.ctrl", true}, {KEY_RIGHTCTRL, "Key.ctrl_r", true},
    {KEY_A, "a", false}, {KEY_S, "s", false}, {KEY_D, "d", false},
    {KEY_F, "f", false}, {KEY_G, "g", false}, {KEY_H, "h", false},
    {KEY_J, "j", false}, {KEY_K, "k", false}, {KEY_L, "l", false},
    {KEY_SEMICOLON, ";", false}, {KEY_APOSTROPHE, "'", false},
    {KEY_GRAVE, "`", false},
    {KEY_LEFTSHIFT, "Key.shift", true}, {KEY_RIGHTSHIFT, "Key.shift_r", true},
    {KEY_BACKSLASH, "\\", false},
    {KEY_Z, "z", false}, {KEY_X, "x", false}, {KEY_C, "c", false},
    {KEY_V, "v", false}, {KEY_B, "b", false}, {KEY_N, "n", false},
    {KEY_M, "m", false},
    {KEY_COMMA, ",", false}, {KEY_DOT, ".", false}, {KEY_SLASH, "/", false},
    {KEY_LEFTALT, "Key.alt", true}, {KEY_RIGHTALT, "Key.alt_r", true},
    {KEY_SPACE, "Key.space", true},
    {KEY_CAPSLOCK, "Key.caps_lock", true},
    {KEY_F1, "Key.f1", true}, {KEY_F2, "Key.f2", true},
    {KEY_F3, "Key.f3", true}, {KEY_F4, "Key.f4", true},
    {KEY_F5, "Key.f5", true}, {KEY_F6, "Key.f6", true},
    {KEY_F7, "Key.f7", true}, {KEY_F8, "Key.f8", true},
    {KEY_F9, "Key.f9", true}, {KEY_F10, "Key.f10", true},
    {KEY_F11, "Key.f11", true}, {KEY_F12, "Key.f12", true},
    {KEY_HOME, "Key.home", true}, {KEY_UP, "Key.up", true},
    {KEY_PAGEUP, "Key.page_up", true}, {KEY_LEFT, "Key.left", true},
    {KEY_RIGHT, "Key.right", true}, {KEY_END, "Key.end", true},
    {KEY_DOWN, "Key.down", true}, {KEY_PAGEDOWN, "Key.page_down", true},
    {KEY_INSERT, "Key.insert", true}, {KEY_DELETE, "Key.delete", true},
    {KEY_LEFTMETA, "Key.cmd", true}, {KEY_RIGHTMETA, "Key.cmd_r", true},
    {0, NULL, false}
};

static const char* get_key_name(int code, bool *is_special) {
    for (int i = 0; key_mappings[i].name != NULL; i++) {
        if (key_mappings[i].code == code) {
            *is_special = key_mappings[i].is_special;
            return key_mappings[i].name;
        }
    }
    *is_special = false;
    return NULL;
}

static const char* get_button_name(int code) {
    switch (code) {
        case BTN_LEFT: return "Button.left";
        case BTN_RIGHT: return "Button.right";
        case BTN_MIDDLE: return "Button.middle";
        default: return NULL;
    }
}

/* Open evdev devices */
static int open_evdev_devices(int *fds, int max_devices) {
    DIR *dir = opendir("/dev/input");
    if (!dir) {
        LOG_ERROR("Cannot open /dev/input directory");
        return 0;
    }
    
    int count = 0;
    struct dirent *entry;
    
    while ((entry = readdir(dir)) != NULL && count < max_devices) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;
        
        char path[512];
        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
        
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            LOG_DEBUG("Cannot open %s: %s", path, strerror(errno));
            continue;
        }
        
        /* Check device capabilities */
        unsigned long evbit[EV_MAX/8/sizeof(unsigned long) + 1] = {0};
        unsigned long keybit[KEY_MAX/8/sizeof(unsigned long) + 1] = {0};
        
        if (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit) < 0) {
            close(fd);
            continue;
        }
        
        /* Check for EV_KEY capability */
        if (!(evbit[0] & (1 << EV_KEY))) {
            close(fd);
            continue;
        }
        
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit);
        
        /* Check if it's a keyboard (has KEY_A) or mouse (has BTN_LEFT) */
        bool is_keyboard = (keybit[KEY_A/8/sizeof(unsigned long)] & 
                           (1UL << (KEY_A % (8*sizeof(unsigned long)))));
        bool is_mouse = (keybit[BTN_LEFT/8/sizeof(unsigned long)] & 
                        (1UL << (BTN_LEFT % (8*sizeof(unsigned long)))));
        
        if (is_keyboard || is_mouse) {
            char name[256] = "Unknown";
            ioctl(fd, EVIOCGNAME(sizeof(name)), name);
            LOG_INFO("Opened input device: %s (%s) - %s", 
                     entry->d_name, name, 
                     is_keyboard ? "keyboard" : "mouse");
            fds[count++] = fd;
        } else {
            close(fd);
        }
    }
    
    closedir(dir);
    return count;
}

/* Grab evdev devices (exclusive access) */
static void grab_evdev_devices(int *fds, int count) {
    for (int i = 0; i < count; i++) {
        if (ioctl(fds[i], EVIOCGRAB, 1) < 0) {
            LOG_WARN("Failed to grab device %d: %s", fds[i], strerror(errno));
        }
    }
}

/* Ungrab evdev devices */
static void ungrab_evdev_devices(int *fds, int count) {
    for (int i = 0; i < count; i++) {
        ioctl(fds[i], EVIOCGRAB, 0);
    }
}

/* Close evdev devices */
static void close_evdev_devices(int *fds, int count) {
    for (int i = 0; i < count; i++) {
        if (fds[i] >= 0) {
            close(fds[i]);
            fds[i] = -1;
        }
    }
}

#endif /* PLATFORM_LINUX */

/* ============================================================================
 * Input Injection
 * ============================================================================ */

#ifdef PLATFORM_LINUX

static void inject_key_linux(Display *display, const char *key, bool is_special, bool press) {
    if (!display) return;
    
    KeySym keysym = 0;
    
    if (is_special) {
        /* Parse special key name */
        if (strcmp(key, "Key.enter") == 0) keysym = XK_Return;
        else if (strcmp(key, "Key.space") == 0) keysym = XK_space;
        else if (strcmp(key, "Key.backspace") == 0) keysym = XK_BackSpace;
        else if (strcmp(key, "Key.tab") == 0) keysym = XK_Tab;
        else if (strcmp(key, "Key.esc") == 0) keysym = XK_Escape;
        else if (strcmp(key, "Key.shift") == 0) keysym = XK_Shift_L;
        else if (strcmp(key, "Key.shift_r") == 0) keysym = XK_Shift_R;
        else if (strcmp(key, "Key.ctrl") == 0) keysym = XK_Control_L;
        else if (strcmp(key, "Key.ctrl_r") == 0) keysym = XK_Control_R;
        else if (strcmp(key, "Key.alt") == 0) keysym = XK_Alt_L;
        else if (strcmp(key, "Key.alt_r") == 0) keysym = XK_Alt_R;
        else if (strcmp(key, "Key.cmd") == 0) keysym = XK_Super_L;
        else if (strcmp(key, "Key.cmd_r") == 0) keysym = XK_Super_R;
        else if (strcmp(key, "Key.caps_lock") == 0) keysym = XK_Caps_Lock;
        else if (strcmp(key, "Key.up") == 0) keysym = XK_Up;
        else if (strcmp(key, "Key.down") == 0) keysym = XK_Down;
        else if (strcmp(key, "Key.left") == 0) keysym = XK_Left;
        else if (strcmp(key, "Key.right") == 0) keysym = XK_Right;
        else if (strcmp(key, "Key.home") == 0) keysym = XK_Home;
        else if (strcmp(key, "Key.end") == 0) keysym = XK_End;
        else if (strcmp(key, "Key.page_up") == 0) keysym = XK_Page_Up;
        else if (strcmp(key, "Key.page_down") == 0) keysym = XK_Page_Down;
        else if (strcmp(key, "Key.insert") == 0) keysym = XK_Insert;
        else if (strcmp(key, "Key.delete") == 0) keysym = XK_Delete;
        else if (strcmp(key, "Key.f1") == 0) keysym = XK_F1;
        else if (strcmp(key, "Key.f2") == 0) keysym = XK_F2;
        else if (strcmp(key, "Key.f3") == 0) keysym = XK_F3;
        else if (strcmp(key, "Key.f4") == 0) keysym = XK_F4;
        else if (strcmp(key, "Key.f5") == 0) keysym = XK_F5;
        else if (strcmp(key, "Key.f6") == 0) keysym = XK_F6;
        else if (strcmp(key, "Key.f7") == 0) keysym = XK_F7;
        else if (strcmp(key, "Key.f8") == 0) keysym = XK_F8;
        else if (strcmp(key, "Key.f9") == 0) keysym = XK_F9;
        else if (strcmp(key, "Key.f10") == 0) keysym = XK_F10;
        else if (strcmp(key, "Key.f11") == 0) keysym = XK_F11;
        else if (strcmp(key, "Key.f12") == 0) keysym = XK_F12;
        else {
            LOG_WARN("Unknown special key: %s", key);
            return;
        }
    } else {
        /* Regular character key */
        if (strlen(key) == 1) {
            keysym = XStringToKeysym(key);
            if (keysym == NoSymbol) {
                /* Try as character */
                keysym = key[0];
            }
        } else {
            keysym = XStringToKeysym(key);
        }
    }
    
    if (keysym == 0 || keysym == NoSymbol) {
        LOG_WARN("Could not map key: %s", key);
        return;
    }
    
    KeyCode keycode = XKeysymToKeycode(display, keysym);
    if (keycode == 0) {
        LOG_WARN("No keycode for keysym: %s", key);
        return;
    }
    
    XTestFakeKeyEvent(display, keycode, press ? True : False, CurrentTime);
    XFlush(display);
}

static void inject_mouse_move_linux(Display *display, int x, int y) {
    if (!display) return;
    XTestFakeMotionEvent(display, -1, x, y, CurrentTime);
    XFlush(display);
}

static void inject_mouse_button_linux(Display *display, const char *button, bool press) {
    if (!display) return;
    
    unsigned int btn = 1;  /* Default to left */
    if (strcmp(button, "Button.left") == 0) btn = 1;
    else if (strcmp(button, "Button.middle") == 0) btn = 2;
    else if (strcmp(button, "Button.right") == 0) btn = 3;
    
    XTestFakeButtonEvent(display, btn, press ? True : False, CurrentTime);
    XFlush(display);
}

static void inject_mouse_scroll_linux(Display *display, int dx, int dy) {
    if (!display) return;
    
    /* Vertical scroll: button 4 (up) or 5 (down) */
    if (dy != 0) {
        unsigned int btn = (dy > 0) ? 4 : 5;
        int clicks = abs(dy);
        for (int i = 0; i < clicks; i++) {
            XTestFakeButtonEvent(display, btn, True, CurrentTime);
            XTestFakeButtonEvent(display, btn, False, CurrentTime);
        }
    }
    
    /* Horizontal scroll: button 6 (left) or 7 (right) */
    if (dx != 0) {
        unsigned int btn = (dx > 0) ? 7 : 6;
        int clicks = abs(dx);
        for (int i = 0; i < clicks; i++) {
            XTestFakeButtonEvent(display, btn, True, CurrentTime);
            XTestFakeButtonEvent(display, btn, False, CurrentTime);
        }
    }
    
    XFlush(display);
}

#endif /* PLATFORM_LINUX */

/* ============================================================================
 * Clipboard Sharing
 * ============================================================================ */

#define CLIPBOARD_MAX_SIZE 65536  /* Max 64KB clipboard */
#define CLIPBOARD_CHECK_INTERVAL_MS 500  /* Check every 500ms */

#ifdef PLATFORM_LINUX
/* Get clipboard content using xclip */
static char* clipboard_get_linux(void) {
    /* Use popen to run xclip and capture output */
    FILE *fp = popen("xclip -selection clipboard -o 2>/dev/null", "r");
    if (!fp) {
        return NULL;
    }
    
    char *content = (char*)malloc(CLIPBOARD_MAX_SIZE);
    if (!content) {
        pclose(fp);
        return NULL;
    }
    
    size_t total_read = 0;
    size_t bytes_read;
    while ((bytes_read = fread(content + total_read, 1, 
                                CLIPBOARD_MAX_SIZE - total_read - 1, fp)) > 0) {
        total_read += bytes_read;
        if (total_read >= CLIPBOARD_MAX_SIZE - 1) break;
    }
    content[total_read] = '\0';
    
    int status = pclose(fp);
    if (status != 0 || total_read == 0) {
        free(content);
        return NULL;
    }
    
    return content;
}

/* Set clipboard content using xclip */
static bool clipboard_set_linux(const char *content, size_t len) {
    if (!content || len == 0) return false;
    
    /* Use popen to write to xclip */
    FILE *fp = popen("xclip -selection clipboard -i 2>/dev/null", "w");
    if (!fp) {
        LOG_WARN("📋 Failed to open xclip for writing");
        return false;
    }
    
    size_t written = fwrite(content, 1, len, fp);
    int status = pclose(fp);
    
    if (status != 0 || written != len) {
        LOG_WARN("📋 xclip write failed (wrote %zu/%zu, status %d)", written, len, status);
        return false;
    }
    
    return true;
}
#endif

#ifdef PLATFORM_WINDOWS
static bool clipboard_open_with_retry(void) {
    for (int i = 0; i < 10; i++) {
        if (OpenClipboard(NULL)) {
            return true;
        }
        Sleep(5);
    }
    return false;
}

static char* clipboard_get_windows(void) {
    if (!clipboard_open_with_retry()) {
        return NULL;
    }

    bool is_unicode = true;
    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (!handle) {
        is_unicode = false;
        handle = GetClipboardData(CF_TEXT);
    }
    if (!handle) {
        CloseClipboard();
        return NULL;
    }

    char *result = NULL;
    if (is_unicode) {
        LPCWSTR wtext = (LPCWSTR)GlobalLock(handle);
        if (wtext) {
            int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wtext, -1, NULL, 0, NULL, NULL);
            if (utf8_len > 0) {
                result = (char *)malloc((size_t)utf8_len);
                if (result) {
                    WideCharToMultiByte(CP_UTF8, 0, wtext, -1, result, utf8_len, NULL, NULL);
                }
            }
            GlobalUnlock(handle);
        }
    } else {
        const char *text = (const char *)GlobalLock(handle);
        if (text) {
            size_t len = strlen(text);
            result = (char *)malloc(len + 1);
            if (result) {
                memcpy(result, text, len + 1);
            }
            GlobalUnlock(handle);
        }
    }

    CloseClipboard();
    return result;
}

static bool clipboard_set_windows(const char *content, size_t len) {
    if (!content || len == 0) return false;
    if (!clipboard_open_with_retry()) return false;

    if (!EmptyClipboard()) {
        CloseClipboard();
        return false;
    }

    int wide_len = MultiByteToWideChar(CP_UTF8, 0, content, (int)len, NULL, 0);
    if (wide_len <= 0) {
        CloseClipboard();
        return false;
    }

    HGLOBAL hmem = GlobalAlloc(GMEM_MOVEABLE, (size_t)(wide_len + 1) * sizeof(WCHAR));
    if (!hmem) {
        CloseClipboard();
        return false;
    }

    WCHAR *wbuf = (WCHAR *)GlobalLock(hmem);
    if (!wbuf) {
        GlobalFree(hmem);
        CloseClipboard();
        return false;
    }

    MultiByteToWideChar(CP_UTF8, 0, content, (int)len, wbuf, wide_len);
    wbuf[wide_len] = L'\0';
    GlobalUnlock(hmem);

    if (!SetClipboardData(CF_UNICODETEXT, hmem)) {
        GlobalFree(hmem);
        CloseClipboard();
        return false;
    }

    CloseClipboard();
    return true;
}
#endif

static char* clipboard_get(void) {
#ifdef PLATFORM_LINUX
    return clipboard_get_linux();
#elif defined(PLATFORM_WINDOWS)
    return clipboard_get_windows();
#else
    return NULL;
#endif
}

static bool clipboard_set(const char *content, size_t len) {
#ifdef PLATFORM_LINUX
    return clipboard_set_linux(content, len);
#elif defined(PLATFORM_WINDOWS)
    return clipboard_set_windows(content, len);
#else
    UNUSED(content);
    UNUSED(len);
    return false;
#endif
}

/* Forward declaration */
static void send_input_event(ClientState *client, const char *event_type, 
                             const char *json_data);
static void* clipboard_monitor_thread(void *arg);

/* Clipboard monitor thread */
static void* clipboard_monitor_thread(void *arg) {
    ClientState *client = (ClientState*)arg;
    
    LOG_INFO("📋 Clipboard monitor started");
    
    /* Initialize with current clipboard */
    char *initial = clipboard_get();
    if (initial) {
        size_t len = strlen(initial);
        if (len < sizeof(client->last_clipboard)) {
            memcpy(client->last_clipboard, initial, len + 1);
            client->last_clipboard_len = len;
        }
        free(initial);
        LOG_INFO("📋 Initial clipboard: %zu chars", client->last_clipboard_len);
    }
    
    while (client->clipboard_thread_running && client->running && !g_shutdown) {
        sleep_ms(CLIPBOARD_CHECK_INTERVAL_MS);
        
        /* Only check clipboard when this computer is ACTIVE */
        if (!client->is_active) {
            continue;
        }
        
        /* Get current clipboard */
        char *current = clipboard_get();
        if (!current) continue;
        
        size_t current_len = strlen(current);
        
        /* Check if clipboard changed */
        if (current_len != client->last_clipboard_len ||
            memcmp(current, client->last_clipboard, current_len) != 0) {
            
            /* Update last known clipboard */
            if (current_len < sizeof(client->last_clipboard)) {
                memcpy(client->last_clipboard, current, current_len + 1);
                client->last_clipboard_len = current_len;
                
                /* Send clipboard update to server */
                if (current_len > 0 && current_len < CLIPBOARD_MAX_SIZE) {
                    /* Escape JSON special characters (simple escaping) */
                    char *escaped = (char*)malloc(current_len * 2 + 100);
                    if (escaped) {
                        char *dst = escaped;
                        *dst++ = '{';
                        *dst++ = '"';
                        *dst++ = 'c'; *dst++ = 'o'; *dst++ = 'n'; *dst++ = 't';
                        *dst++ = 'e'; *dst++ = 'n'; *dst++ = 't';
                        *dst++ = '"';
                        *dst++ = ':';
                        *dst++ = '"';
                        
                        for (size_t i = 0; i < current_len && (size_t)(dst - escaped) < current_len * 2 + 50; i++) {
                            char c = current[i];
                            if (c == '"' || c == '\\') {
                                *dst++ = '\\';
                                *dst++ = c;
                            } else if (c == '\n') {
                                *dst++ = '\\';
                                *dst++ = 'n';
                            } else if (c == '\r') {
                                *dst++ = '\\';
                                *dst++ = 'r';
                            } else if (c == '\t') {
                                *dst++ = '\\';
                                *dst++ = 't';
                            } else if ((unsigned char)c >= 32) {
                                *dst++ = c;
                            }
                        }
                        
                        *dst++ = '"';
                        *dst++ = ',';
                        sprintf(dst, "\"source\":\"%s\"}", client->config.computer_id);
                        
                        LOG_INFO("📋 Sending clipboard_update: %.100s...", escaped);
                        send_input_event(client, "clipboard_update", escaped);
                        LOG_INFO("📋 Clipboard changed (%zu chars), synced", current_len);
                        
                        free(escaped);
                    }
                }
            }
        }
        
        free(current);
    }
    
    LOG_INFO("📋 Clipboard monitor stopped");
    return NULL;
}

/* Handle clipboard update from server */
static void handle_clipboard_update(ClientState *client, JsonValue *msg) {
    LOG_INFO("📋 handle_clipboard_update called");
    
    JsonValue *data = json_get(msg, "data");
    if (!data) {
        LOG_WARN("📋 No 'data' field in clipboard message");
        return;
    }
    
    const char *content = json_get_string(data, "content", "");
    const char *source = json_get_string(data, "source", "");
    
    LOG_INFO("📋 Clipboard from source='%s', content_len=%zu", source, strlen(content));
    
    /* Don't update if this is our own clipboard update echoed back */
    if (strcmp(source, client->config.computer_id) == 0) {
        LOG_DEBUG("📋 Ignoring own clipboard echo");
        return;
    }
    
    size_t content_len = strlen(content);
    if (content_len == 0 || content_len >= CLIPBOARD_MAX_SIZE) {
        LOG_WARN("📋 Invalid content length: %zu", content_len);
        return;
    }
    
    /* Set local clipboard */
    if (clipboard_set(content, content_len)) {
        /* Update our tracking to prevent re-sending */
        if (content_len < sizeof(client->last_clipboard)) {
            memcpy(client->last_clipboard, content, content_len + 1);
            client->last_clipboard_len = content_len;
        }
        LOG_INFO("📋 Received clipboard from %s (%zu chars) - SET OK", source, content_len);
    } else {
        LOG_WARN("📋 Failed to set clipboard");
    }
}

#ifdef PLATFORM_LINUX
static void warp_cursor_to_center_linux(Display *display, Monitor *monitor) {
    if (!display || !monitor) return;
    
    int center_x = monitor->x + monitor->width / 2;
    int center_y = monitor->y + monitor->height / 2;
    
    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);
    
    XWarpPointer(display, None, root, 0, 0, 0, 0, center_x, center_y);
    XFlush(display);
    
    LOG_INFO("Warped cursor to center (%d, %d)", center_x, center_y);
}

/* Hide cursor using XFixes (when this computer is INACTIVE) */
static bool g_cursor_hidden = false;
static Cursor g_invisible_cursor = None;
static Display *g_cursor_display = NULL;

/* Cleanup function to restore cursor on exit */
static void cleanup_cursor_on_exit(void) {
    if (g_cursor_display && g_cursor_hidden) {
        Window root = DefaultRootWindow(g_cursor_display);
        /* Try XFixes first */
        int ev, err;
        if (XFixesQueryExtension(g_cursor_display, &ev, &err)) {
            XFixesShowCursor(g_cursor_display, root);
        }
        /* Also undefine any custom cursor */
        XUndefineCursor(g_cursor_display, root);
        XFlush(g_cursor_display);
        g_cursor_hidden = false;
    }
}

/* Register cleanup at program start */
static void register_cursor_cleanup(void) {
    static bool registered = false;
    if (!registered) {
        atexit(cleanup_cursor_on_exit);
        registered = true;
    }
}

static Cursor create_invisible_cursor(Display *display) {
    if (!display) return None;
    
    /* Create an invisible cursor using XFixes or a blank pixmap */
    Pixmap blank;
    XColor dummy;
    Cursor cursor;
    Window root = DefaultRootWindow(display);
    
    blank = XCreatePixmap(display, root, 1, 1, 1);
    if (blank == None) return None;
    
    memset(&dummy, 0, sizeof(dummy));
    cursor = XCreatePixmapCursor(display, blank, blank, &dummy, &dummy, 0, 0);
    XFreePixmap(display, blank);
    
    return cursor;
}

static void hide_cursor_linux(Display *display) {
    if (!display || g_cursor_hidden) return;
    
    /* Register cleanup handler on first use */
    register_cursor_cleanup();
    
    g_cursor_display = display;
    Window root = DefaultRootWindow(display);
    
    /* Use XFixes extension to hide cursor globally (most reliable method) */
    int event_base, error_base;
    if (XFixesQueryExtension(display, &event_base, &error_base)) {
        XFixesHideCursor(display, root);
        XFlush(display);
        g_cursor_hidden = true;
        LOG_INFO("🔲 Cursor hidden (XFixes)");
    } else {
        /* Fallback: invisible cursor pixmap */
        if (g_invisible_cursor == None) {
            g_invisible_cursor = create_invisible_cursor(display);
        }
        
        if (g_invisible_cursor != None) {
            XDefineCursor(display, root, g_invisible_cursor);
            XFlush(display);
            g_cursor_hidden = true;
            LOG_INFO("🔲 Cursor hidden (pixmap fallback)");
        } else {
            LOG_WARN("Could not hide cursor - no XFixes or pixmap cursor");
        }
    }
}

static void show_cursor_linux(Display *display) {
    if (!display) display = g_cursor_display;
    if (!display || !g_cursor_hidden) return;
    
    Window root = DefaultRootWindow(display);
    
    /* Use XFixes to show cursor */
    int event_base, error_base;
    if (XFixesQueryExtension(display, &event_base, &error_base)) {
        XFixesShowCursor(display, root);
    }
    
    /* Also undefine any custom cursor (for fallback case) */
    XUndefineCursor(display, root);
    XFlush(display);
    g_cursor_hidden = false;
    LOG_INFO("🔳 Cursor shown");
}

/* X11 keyboard/pointer grab for blocking local input when INACTIVE */
static bool g_x11_grabbed = false;

static void x11_grab_input(Display *display) {
    if (!display || g_x11_grabbed) return;
    
    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);
    
    /* Grab keyboard */
    int kb_status = XGrabKeyboard(display, root, True,
                                   GrabModeAsync, GrabModeAsync, CurrentTime);
    
    /* Grab pointer */
    int ptr_status = XGrabPointer(display, root, True,
                                   PointerMotionMask | ButtonPressMask | ButtonReleaseMask,
                                   GrabModeAsync, GrabModeAsync,
                                   None, None, CurrentTime);
    
    XFlush(display);
    
    if (kb_status == GrabSuccess && ptr_status == GrabSuccess) {
        g_x11_grabbed = true;
        LOG_INFO("🔒 X11 input grabbed (keyboard + pointer blocked locally)");
    } else {
        /* Ungrab if partial success */
        if (kb_status == GrabSuccess) XUngrabKeyboard(display, CurrentTime);
        if (ptr_status == GrabSuccess) XUngrabPointer(display, CurrentTime);
        LOG_WARN("X11 grab failed (kb=%d, ptr=%d)", kb_status, ptr_status);
    }
}

static void x11_ungrab_input(Display *display) {
    if (!display || !g_x11_grabbed) return;
    
    XUngrabKeyboard(display, CurrentTime);
    XUngrabPointer(display, CurrentTime);
    XFlush(display);
    
    g_x11_grabbed = false;
    LOG_INFO("🔓 X11 input ungrabbed (keyboard + pointer enabled locally)");
}

/* XRecord-based keyboard capture for X11 fallback mode */

/* Forward declaration */
static void send_input_event(ClientState *client, const char *event_type, 
                             const char *json_data);

/* X error handler to catch XRecord errors gracefully */
static bool g_xrecord_error = false;
static int xrecord_error_handler(Display *display, XErrorEvent *event) {
    (void)display;
    if (event->request_code == 146) {  /* RECORD extension */
        g_xrecord_error = true;
        LOG_WARN("XRecord error caught (code=%d, minor=%d) - keyboard capture disabled", 
                 event->error_code, event->minor_code);
        return 0;  /* Don't crash */
    }
    /* For other errors, log but don't crash */
    LOG_WARN("X error: code=%d, request=%d, minor=%d", 
             event->error_code, event->request_code, event->minor_code);
    return 0;
}

typedef struct {
    ClientState *client;
    Display *display;
    int key_count;
} XRecordContext_t;

static XRecordContext_t *g_xrecord_ctx = NULL;
static XRecordContext g_record_ctx = 0;
static bool g_xrecord_running = false;

/* Convert X11 keycode to key name string */
static const char* x11_keycode_to_name(Display *display, unsigned int keycode, bool *is_special) {
    static char key_buf[32];
    *is_special = false;
    
    KeySym keysym = XkbKeycodeToKeysym(display, keycode, 0, 0);
    
    /* Map common special keys */
    switch (keysym) {
        case XK_Escape: *is_special = true; return "Key.esc";
        case XK_Tab: *is_special = true; return "Key.tab";
        case XK_BackSpace: *is_special = true; return "Key.backspace";
        case XK_Return: case XK_KP_Enter: *is_special = true; return "Key.enter";
        case XK_space: return " ";
        case XK_Shift_L: *is_special = true; return "Key.shift";
        case XK_Shift_R: *is_special = true; return "Key.shift_r";
        case XK_Control_L: *is_special = true; return "Key.ctrl";
        case XK_Control_R: *is_special = true; return "Key.ctrl_r";
        case XK_Alt_L: *is_special = true; return "Key.alt";
        case XK_Alt_R: *is_special = true; return "Key.alt_r";
        case XK_Super_L: *is_special = true; return "Key.cmd";
        case XK_Super_R: *is_special = true; return "Key.cmd_r";
        case XK_Caps_Lock: *is_special = true; return "Key.caps_lock";
        case XK_Delete: *is_special = true; return "Key.delete";
        case XK_Home: *is_special = true; return "Key.home";
        case XK_End: *is_special = true; return "Key.end";
        case XK_Page_Up: *is_special = true; return "Key.page_up";
        case XK_Page_Down: *is_special = true; return "Key.page_down";
        case XK_Up: *is_special = true; return "Key.up";
        case XK_Down: *is_special = true; return "Key.down";
        case XK_Left: *is_special = true; return "Key.left";
        case XK_Right: *is_special = true; return "Key.right";
        case XK_Insert: *is_special = true; return "Key.insert";
        case XK_Print: *is_special = true; return "Key.print_screen";
        case XK_Scroll_Lock: *is_special = true; return "Key.scroll_lock";
        case XK_Pause: *is_special = true; return "Key.pause";
        case XK_Num_Lock: *is_special = true; return "Key.num_lock";
        case XK_F1: *is_special = true; return "Key.f1";
        case XK_F2: *is_special = true; return "Key.f2";
        case XK_F3: *is_special = true; return "Key.f3";
        case XK_F4: *is_special = true; return "Key.f4";
        case XK_F5: *is_special = true; return "Key.f5";
        case XK_F6: *is_special = true; return "Key.f6";
        case XK_F7: *is_special = true; return "Key.f7";
        case XK_F8: *is_special = true; return "Key.f8";
        case XK_F9: *is_special = true; return "Key.f9";
        case XK_F10: *is_special = true; return "Key.f10";
        case XK_F11: *is_special = true; return "Key.f11";
        case XK_F12: *is_special = true; return "Key.f12";
        default:
            break;
    }
    
    /* For printable characters, get the string representation */
    if (keysym >= XK_space && keysym <= XK_asciitilde) {
        key_buf[0] = (char)keysym;
        key_buf[1] = '\0';
        return key_buf;
    }
    
    /* Fallback: return keysym name */
    const char *name = XKeysymToString(keysym);
    if (name) {
        *is_special = true;
        snprintf(key_buf, sizeof(key_buf), "Key.%s", name);
        return key_buf;
    }
    
    return NULL;
}

/* XRecord callback for keyboard events */
static void xrecord_callback(XPointer priv, XRecordInterceptData *data) {
    if (data->category != XRecordFromServer) {
        XRecordFreeData(data);
        return;
    }
    
    XRecordContext_t *ctx = (XRecordContext_t *)priv;
    if (!ctx || !ctx->client || !ctx->client->running) {
        XRecordFreeData(data);
        return;
    }
    
    /* Skip if not main computer or if executing received input */
    if (strcmp(ctx->client->config.role, "main") != 0 ||
        ctx->client->executing_input ||
        !ctx->client->connected) {
        XRecordFreeData(data);
        return;
    }
    
    /* NOTE: Main computer ALWAYS captures keyboard events regardless of is_active!
     * The physical keyboard is attached to main, so we must capture and send
     * events to the server which will forward them to the active computer. */
    
    /* Parse the X event from the intercept data */
    unsigned char *event_data = (unsigned char *)data->data;
    int event_type = event_data[0] & 0x7F;  /* Event type is in first byte */
    
    if (event_type == KeyPress || event_type == KeyRelease) {
        /* Extract keycode from event structure (xEvent format) */
        unsigned int keycode = event_data[1];
        bool is_press = (event_type == KeyPress);
        bool is_special = false;
        
        const char *key_name = x11_keycode_to_name(ctx->display, keycode, &is_special);
        if (key_name) {
            ctx->key_count++;
            
            char json[256];
            snprintf(json, sizeof(json),
                "{\"action\":\"%s\",\"key\":\"%s\",\"is_special\":%s}",
                is_press ? "press" : "release",
                key_name,
                is_special ? "true" : "false");
            send_input_event(ctx->client, "keyboard", json);
            
            /* Log first 5 keys and then every 50 */
            if (ctx->key_count <= 5 || ctx->key_count % 50 == 0) {
                LOG_INFO("⌨️ KEY: %s %s [#%d]", 
                         is_press ? "press" : "release", key_name, ctx->key_count);
            }
            
            /* Check for ESC to exit */
            KeySym keysym = XkbKeycodeToKeysym(ctx->display, keycode, 0, 0);
            if (keysym == XK_Escape && is_press) {
                LOG_INFO("ESC pressed, stopping...");
                ctx->client->running = false;
            }
        }
    }
    else if (event_type == ButtonPress) {
        /* Handle scroll wheel via XRecord - Button 4=up, 5=down, 6=left, 7=right */
        unsigned int button = event_data[1];  /* Button number in detail field */
        
        /* Only handle scroll wheel buttons (4, 5, 6, 7), not regular clicks */
        if (button >= 4 && button <= 7) {
            static int scroll_count = 0;
            scroll_count++;
            
            /* Get current pointer position for scroll event */
            Window root_return, child_return;
            int root_x = 0, root_y = 0, win_x, win_y;
            unsigned int mask;
            
            if (XQueryPointer(ctx->display, DefaultRootWindow(ctx->display),
                              &root_return, &child_return,
                              &root_x, &root_y, &win_x, &win_y, &mask)) {
                
                int dx = 0, dy = 0;
                if (button == 4) dy = 1;       /* Scroll up */
                else if (button == 5) dy = -1; /* Scroll down */
                else if (button == 6) dx = -1; /* Scroll left */
                else if (button == 7) dx = 1;  /* Scroll right */
                
                char json[128];
                snprintf(json, sizeof(json),
                    "{\"x\":%d,\"y\":%d,\"dx\":%d,\"dy\":%d}",
                    root_x, root_y, dx, dy);
                send_input_event(ctx->client, "mouse_scroll", json);
                
                /* Log first 3 scrolls and then every 20 */
                if (scroll_count <= 3 || scroll_count % 20 == 0) {
                    LOG_INFO("🖱️ SCROLL: %s at (%d,%d) [#%d]",
                             button == 4 ? "up" : button == 5 ? "down" : 
                             button == 6 ? "left" : "right",
                             root_x, root_y, scroll_count);
                }
            }
        }
    }
    
    XRecordFreeData(data);
}

/* Setup XRecord for keyboard capture */
static bool xrecord_setup(ClientState *client, Display *ctrl_display, Display *data_display) {
    (void)data_display;  /* Not used - thread opens its own connection */
    
    if (g_xrecord_running) return true;
    
    /* Check if XRecord extension is available */
    int major, minor;
    if (!XRecordQueryVersion(ctrl_display, &major, &minor)) {
        LOG_WARN("XRecord extension not available - keyboard capture disabled");
        return false;
    }
    LOG_INFO("XRecord extension version %d.%d", major, minor);
    
    /* Create context */
    g_xrecord_ctx = (XRecordContext_t *)calloc(1, sizeof(XRecordContext_t));
    if (!g_xrecord_ctx) return false;
    
    g_xrecord_ctx->client = client;
    g_xrecord_ctx->display = ctrl_display;
    g_xrecord_ctx->key_count = 0;
    
    /* Setup record range for keyboard events */
    XRecordRange *range = XRecordAllocRange();
    if (!range) {
        free(g_xrecord_ctx);
        g_xrecord_ctx = NULL;
        return false;
    }
    
    /* Clear the range structure and set only what we need */
    memset(range, 0, sizeof(XRecordRange));
    /* Capture keyboard AND button events (for scroll wheel) */
    range->device_events.first = KeyPress;
    range->device_events.last = ButtonRelease;  /* Includes KeyPress, KeyRelease, ButtonPress, ButtonRelease */
    
    /* Create record context on control display */
    XRecordClientSpec client_spec = XRecordAllClients;
    g_record_ctx = XRecordCreateContext(ctrl_display, 0, &client_spec, 1, &range, 1);
    XFree(range);
    
    if (!g_record_ctx) {
        LOG_WARN("Failed to create XRecord context");
        free(g_xrecord_ctx);
        g_xrecord_ctx = NULL;
        return false;
    }
    
    /* Sync to ensure context is created before thread uses it */
    XSync(ctrl_display, False);
    
    LOG_INFO("⌨️ XRecord context created (keyboard capture ready)");
    return true;
}

/* XRecord thread - runs XRecordEnableContext in blocking mode */
static Display *g_xrecord_data_display = NULL;

static void* xrecord_thread_func(void *arg) {
    XRecordContext_t *ctx = (XRecordContext_t *)arg;
    
    /* Open dedicated display for XRecord data */
    g_xrecord_data_display = XOpenDisplay(NULL);
    if (!g_xrecord_data_display) {
        LOG_ERROR("Cannot open display for XRecord thread");
        g_xrecord_running = false;
        return NULL;
    }
    
    LOG_INFO("⌨️ XRecord thread started");
    
    /* Set error handler */
    XSetErrorHandler(xrecord_error_handler);
    g_xrecord_error = false;
    
    /* This BLOCKS and calls xrecord_callback for each event */
    if (!XRecordEnableContext(g_xrecord_data_display, g_record_ctx, 
                               xrecord_callback, (XPointer)ctx)) {
        LOG_WARN("XRecordEnableContext failed");
        g_xrecord_running = false;
    }
    
    XCloseDisplay(g_xrecord_data_display);
    g_xrecord_data_display = NULL;
    LOG_INFO("⌨️ XRecord thread exiting");
    return NULL;
}

static void xrecord_cleanup(Display *ctrl_display, Display *data_display) {
    if (!g_xrecord_running) return;
    
    if (data_display && g_record_ctx) {
        XRecordDisableContext(data_display, g_record_ctx);
    }
    
    if (ctrl_display && g_record_ctx) {
        XRecordFreeContext(ctrl_display, g_record_ctx);
    }
    
    if (g_xrecord_ctx) {
        free(g_xrecord_ctx);
        g_xrecord_ctx = NULL;
    }
    
    g_record_ctx = 0;
    g_xrecord_running = false;
    LOG_INFO("⌨️ XRecord keyboard capture disabled");
}

#endif /* PLATFORM_LINUX */

#ifdef PLATFORM_WINDOWS
static bool is_extended_vk(WORD vk) {
    switch (vk) {
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_INSERT:
        case VK_DELETE:
        case VK_RCONTROL:
        case VK_RMENU:
        case VK_LWIN:
        case VK_RWIN:
            return true;
        default:
            return false;
    }
}

static bool map_key_name_to_vk(const char *key, WORD *vk_out) {
    if (!key || !vk_out) return false;

    if (strncmp(key, "Key.vk", 6) == 0) {
        int code = atoi(key + 6);
        if (code > 0 && code < 256) {
            *vk_out = (WORD)code;
            return true;
        }
    }

    if (strcmp(key, "Key.enter") == 0) *vk_out = VK_RETURN;
    else if (strcmp(key, "Key.space") == 0) *vk_out = VK_SPACE;
    else if (strcmp(key, "Key.backspace") == 0) *vk_out = VK_BACK;
    else if (strcmp(key, "Key.tab") == 0) *vk_out = VK_TAB;
    else if (strcmp(key, "Key.esc") == 0) *vk_out = VK_ESCAPE;
    else if (strcmp(key, "Key.shift") == 0) *vk_out = VK_LSHIFT;
    else if (strcmp(key, "Key.shift_r") == 0) *vk_out = VK_RSHIFT;
    else if (strcmp(key, "Key.ctrl") == 0) *vk_out = VK_LCONTROL;
    else if (strcmp(key, "Key.ctrl_r") == 0) *vk_out = VK_RCONTROL;
    else if (strcmp(key, "Key.alt") == 0) *vk_out = VK_LMENU;
    else if (strcmp(key, "Key.alt_r") == 0) *vk_out = VK_RMENU;
    else if (strcmp(key, "Key.cmd") == 0) *vk_out = VK_LWIN;
    else if (strcmp(key, "Key.cmd_r") == 0) *vk_out = VK_RWIN;
    else if (strcmp(key, "Key.caps_lock") == 0) *vk_out = VK_CAPITAL;
    else if (strcmp(key, "Key.up") == 0) *vk_out = VK_UP;
    else if (strcmp(key, "Key.down") == 0) *vk_out = VK_DOWN;
    else if (strcmp(key, "Key.left") == 0) *vk_out = VK_LEFT;
    else if (strcmp(key, "Key.right") == 0) *vk_out = VK_RIGHT;
    else if (strcmp(key, "Key.home") == 0) *vk_out = VK_HOME;
    else if (strcmp(key, "Key.end") == 0) *vk_out = VK_END;
    else if (strcmp(key, "Key.page_up") == 0) *vk_out = VK_PRIOR;
    else if (strcmp(key, "Key.page_down") == 0) *vk_out = VK_NEXT;
    else if (strcmp(key, "Key.insert") == 0) *vk_out = VK_INSERT;
    else if (strcmp(key, "Key.delete") == 0) *vk_out = VK_DELETE;
    else if (strcmp(key, "Key.print_screen") == 0) *vk_out = VK_SNAPSHOT;
    else if (strcmp(key, "Key.scroll_lock") == 0) *vk_out = VK_SCROLL;
    else if (strcmp(key, "Key.pause") == 0) *vk_out = VK_PAUSE;
    else if (strcmp(key, "Key.num_lock") == 0) *vk_out = VK_NUMLOCK;
    else if (strcmp(key, "Key.f1") == 0) *vk_out = VK_F1;
    else if (strcmp(key, "Key.f2") == 0) *vk_out = VK_F2;
    else if (strcmp(key, "Key.f3") == 0) *vk_out = VK_F3;
    else if (strcmp(key, "Key.f4") == 0) *vk_out = VK_F4;
    else if (strcmp(key, "Key.f5") == 0) *vk_out = VK_F5;
    else if (strcmp(key, "Key.f6") == 0) *vk_out = VK_F6;
    else if (strcmp(key, "Key.f7") == 0) *vk_out = VK_F7;
    else if (strcmp(key, "Key.f8") == 0) *vk_out = VK_F8;
    else if (strcmp(key, "Key.f9") == 0) *vk_out = VK_F9;
    else if (strcmp(key, "Key.f10") == 0) *vk_out = VK_F10;
    else if (strcmp(key, "Key.f11") == 0) *vk_out = VK_F11;
    else if (strcmp(key, "Key.f12") == 0) *vk_out = VK_F12;
    else return false;

    return true;
}

static void inject_key_windows(const char *key, bool is_special, bool press) {
    if (!key || !*key) return;

    INPUT input;
    memset(&input, 0, sizeof(input));
    input.type = INPUT_KEYBOARD;

    bool treat_as_special = is_special || strncmp(key, "Key.", 4) == 0;

    if (!treat_as_special && strlen(key) == 1) {
        input.ki.wVk = 0;
        input.ki.wScan = (WORD)(unsigned char)key[0];
        input.ki.dwFlags = KEYEVENTF_UNICODE | (press ? 0 : KEYEVENTF_KEYUP);
        SendInput(1, &input, sizeof(INPUT));
        return;
    }

    WORD vk = 0;
    if (!map_key_name_to_vk(key, &vk)) {
        if (strlen(key) == 1) {
            input.ki.wVk = 0;
            input.ki.wScan = (WORD)(unsigned char)key[0];
            input.ki.dwFlags = KEYEVENTF_UNICODE | (press ? 0 : KEYEVENTF_KEYUP);
            SendInput(1, &input, sizeof(INPUT));
            return;
        }
        LOG_WARN("Unknown Windows key: %s", key);
        return;
    }

    input.ki.wVk = vk;
    input.ki.wScan = (WORD)MapVirtualKey(vk, MAPVK_VK_TO_VSC);
    input.ki.dwFlags = (press ? 0 : KEYEVENTF_KEYUP);
    if (is_extended_vk(vk)) {
        input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }

    SendInput(1, &input, sizeof(INPUT));
}

static void inject_mouse_move_windows(int x, int y) {
    SetCursorPos(x, y);
}

static void inject_mouse_button_windows(const char *button, bool press) {
    INPUT input = {0};
    input.type = INPUT_MOUSE;
    
    if (strcmp(button, "Button.left") == 0) {
        input.mi.dwFlags = press ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    } else if (strcmp(button, "Button.right") == 0) {
        input.mi.dwFlags = press ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    } else if (strcmp(button, "Button.middle") == 0) {
        input.mi.dwFlags = press ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    }
    
    SendInput(1, &input, sizeof(INPUT));
}

static void inject_mouse_scroll_windows(int dx, int dy) {
    INPUT input = {0};
    input.type = INPUT_MOUSE;
    
    if (dy != 0) {
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = (DWORD)(dy * WHEEL_DELTA);
    } else if (dx != 0) {
        input.mi.dwFlags = MOUSEEVENTF_HWHEEL;
        input.mi.mouseData = (DWORD)(dx * WHEEL_DELTA);
    }
    
    SendInput(1, &input, sizeof(INPUT));
}

/* ============================================================================
 * Windows Input Capture (Low-Level Hooks)
 * ============================================================================ */

/* Forward declarations */
static void send_input_event(ClientState *client, const char *event_type,
                             const char *json_data);
static void send_edge_crossing_request(ClientState *client, const char *edge,
                                       float position, int cursor_x, int cursor_y);
static void send_mouse_move_fast(ClientState *client, int x, int y, double now_ms);

/* Windows low-level keyboard hook */
static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && g_client.running && strcmp(g_client.config.role, "main") == 0) {
        if (!g_client.is_active) {
            static int kb_block_count = 0;
            if (kb_block_count < 5) {
                kb_block_count++;
                LOG_INFO("HOOK: blocking local keyboard input (main inactive)");
            }
            return 1; /* Block local input when inactive */
        }
        KBDLLHOOKSTRUCT *kb = (KBDLLHOOKSTRUCT *)lParam;
        bool press = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        
        /* Map virtual key code to key name */
        char key_name[64];
        bool is_special = false;
        
        if (kb->vkCode >= 'A' && kb->vkCode <= 'Z') {
            snprintf(key_name, sizeof(key_name), "%c", (char)(kb->vkCode + 32));  /* Lowercase */
        } else if (kb->vkCode >= '0' && kb->vkCode <= '9') {
            snprintf(key_name, sizeof(key_name), "%c", (char)kb->vkCode);
        } else {
            /* Map special keys */
            is_special = true;
            switch (kb->vkCode) {
                case VK_RETURN: strcpy(key_name, "Key.enter"); break;
                case VK_SPACE: strcpy(key_name, "Key.space"); break;
                case VK_BACK: strcpy(key_name, "Key.backspace"); break;
                case VK_TAB: strcpy(key_name, "Key.tab"); break;
                case VK_ESCAPE: strcpy(key_name, "Key.esc"); break;
                case VK_SHIFT: strcpy(key_name, "Key.shift"); break;
                case VK_CONTROL: strcpy(key_name, "Key.ctrl"); break;
                case VK_MENU: strcpy(key_name, "Key.alt"); break;
                case VK_LWIN: case VK_RWIN: strcpy(key_name, "Key.cmd"); break;
                default: snprintf(key_name, sizeof(key_name), "Key.vk%d", kb->vkCode); break;
            }
        }
        
        char json[128];
        snprintf(json, sizeof(json), "{\"action\":\"%s\",\"key\":\"%s\",\"is_special\":%s}",
                 press ? "press" : "release", key_name, is_special ? "true" : "false");
        send_input_event(&g_client, "keyboard", json);
    }
    
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

/* Windows low-level mouse hook */
static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    /* Debug: Log first few hook calls */
    static int debug_count = 0;
    if (debug_count < 5) {
        debug_count++;
        LOG_INFO("HOOK DEBUG #%d: nCode=%d, running=%d, role=%s, server_mode=%d",
                 debug_count, nCode, g_client.running, g_client.config.role, g_server.is_server_mode);
    }

    /* CRITICAL: Always call CallNextHookEx first for non-negative codes to avoid blocking */
    if (nCode < 0) {
        return CallNextHookEx(NULL, nCode, wParam, lParam);
    }

    /* Fast path: Return immediately if not in main role or not running */
    if (!g_client.running || strcmp(g_client.config.role, "main") != 0) {
        return CallNextHookEx(NULL, nCode, wParam, lParam);
    }

    /* Skip if executing received input (to avoid capturing our own injected events) */
    if (g_client.executing_input) {
        return CallNextHookEx(NULL, nCode, wParam, lParam);
    }

    /* Block local input when main is inactive (exclusive control on active computer) */
    if (!g_client.is_active) {
        static int inactive_block_count = 0;
        if (inactive_block_count < 5) {
            inactive_block_count++;
            LOG_INFO("HOOK: blocking local mouse input (main inactive)");
        }
        return 1; /* Block local input when inactive */
    }

    MSLLHOOKSTRUCT *ms = (MSLLHOOKSTRUCT *)lParam;
    POINT pt = ms->pt;
    
    switch (wParam) {
        case WM_MOUSEMOVE: {
            /* Skip if we're ignoring mouse moves (after injecting cursor position) */
            if (g_client.skip_mouse_moves > 0) {
                g_client.skip_mouse_moves--;
                return CallNextHookEx(NULL, nCode, wParam, lParam);
            }
            
            /* Throttle mouse moves - increased to 16ms to reduce hook overhead */
            static double last_move_time = 0;
            static int last_x = -1, last_y = -1;
            static int hook_call_count = 0;
            static int hook_send_count = 0;
            static double hook_total_time = 0;
            static double last_perf_log = 0;
            
            double hook_start = get_time_ms();
            hook_call_count++;
            
            double now = hook_start;
            bool should_send = false;
            
            /* Only process if enough time has passed AND position changed */
            if (now - last_move_time >= 16 && (pt.x != last_x || pt.y != last_y)) {
                should_send = true;
            }
            
            double hook_end = get_time_ms();
            double hook_duration = hook_end - hook_start;
            hook_total_time += hook_duration;
            
            /* Log performance every 5 seconds (avoid calling msg_queue_count in hook for performance) */
            if (now - last_perf_log >= 5000) {
                double avg_time = hook_call_count > 0 ? hook_total_time / hook_call_count : 0;
                double send_rate = hook_send_count > 0 ? (hook_send_count * 1000.0) / (now - last_perf_log) : 0;
                LOG_INFO("🖱️ Mouse hook perf: calls=%d, sent=%d, avg_time=%.3fms, send_rate=%.1f/s",
                         hook_call_count, hook_send_count, avg_time, send_rate);
                hook_call_count = 0;
                hook_send_count = 0;
                hook_total_time = 0;
                last_perf_log = now;
            }
            
            if (should_send) {
                send_mouse_move_fast(&g_client, pt.x, pt.y, now);
                last_move_time = now;
                last_x = pt.x;
                last_y = pt.y;
                hook_send_count++;

                /* Edge detection for screen boundary crossing */
                int screen_w = (g_client.monitor_count > 0) ? g_client.monitors[0].width : 1920;
                int screen_h = (g_client.monitor_count > 0) ? g_client.monitors[0].height : 1080;
                bool at_edge = (pt.x <= 5 || pt.x >= screen_w - 5 ||
                               pt.y <= 5 || pt.y >= screen_h - 5);

                /* Detect which edge and send edge crossing request if at edge and active */
                if (at_edge && g_client.is_active) {
                    const char *edge = NULL;
                    float position = 0.0f;

                    if (pt.x <= 5) {
                        edge = "left";
                        position = (float)pt.y / screen_h;
                    } else if (pt.x >= screen_w - 5) {
                        edge = "right";
                        position = (float)pt.y / screen_h;
                    } else if (pt.y <= 5) {
                        edge = "top";
                        position = (float)pt.x / screen_w;
                    } else if (pt.y >= screen_h - 5) {
                        edge = "bottom";
                        position = (float)pt.x / screen_w;
                    }

                    if (edge) {
                        /* Throttle edge crossing requests to max once per 200ms */
                        static double last_edge_request_time = 0;
                        if (now - last_edge_request_time >= 200) {
                            LOG_INFO("Edge detected: %s at (%.2f) cursor=(%d,%d) screen=%dx%d",
                                     edge, position, pt.x, pt.y, screen_w, screen_h);
                            send_edge_crossing_request(&g_client, edge, position, pt.x, pt.y);
                            last_edge_request_time = now;
                        }
                    }
                }

                /* Warn if hook is taking too long (could cause mouse lag) */
                double total_time = get_time_ms() - hook_start;
                if (total_time > 5.0) {
                    LOG_WARN("⚠️ Mouse hook took %.2fms - this may cause cursor lag!", total_time);
                }
            }
            break;
        }
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP: {
            char json[128];
            snprintf(json, sizeof(json), "{\"action\":\"%s\",\"button\":\"Button.left\",\"x\":%d,\"y\":%d}",
                     wParam == WM_LBUTTONDOWN ? "press" : "release", pt.x, pt.y);
            send_input_event(&g_client, "mouse_click", json);
            break;
        }
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP: {
            char json[128];
            snprintf(json, sizeof(json), "{\"action\":\"%s\",\"button\":\"Button.right\",\"x\":%d,\"y\":%d}",
                     wParam == WM_RBUTTONDOWN ? "press" : "release", pt.x, pt.y);
            send_input_event(&g_client, "mouse_click", json);
            break;
        }
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP: {
            char json[128];
            snprintf(json, sizeof(json), "{\"action\":\"%s\",\"button\":\"Button.middle\",\"x\":%d,\"y\":%d}",
                     wParam == WM_MBUTTONDOWN ? "press" : "release", pt.x, pt.y);
            send_input_event(&g_client, "mouse_click", json);
            break;
        }
        case WM_MOUSEWHEEL: {
            short delta = HIWORD(ms->mouseData);
            int dy = delta > 0 ? 1 : -1;
            char json[128];
            snprintf(json, sizeof(json), "{\"dx\":0,\"dy\":%d}", dy);
            send_input_event(&g_client, "mouse_scroll", json);
            break;
        }
        case WM_MOUSEHWHEEL: {
            short delta = HIWORD(ms->mouseData);
            int dx = delta > 0 ? 1 : -1;
            char json[128];
            snprintf(json, sizeof(json), "{\"dx\":%d,\"dy\":0}", dx);
            send_input_event(&g_client, "mouse_scroll", json);
            break;
        }
    }

    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

/* Setup Windows input hooks */
static bool setup_windows_hooks(ClientState *client) {
    if (client->keyboard_hook || client->mouse_hook) {
        return true;  /* Already set up */
    }
    
    HINSTANCE hInstance = GetModuleHandle(NULL);
    
    client->keyboard_hook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInstance, 0);
    client->mouse_hook = SetWindowsHookEx(WH_MOUSE_LL, LowLevelMouseProc, hInstance, 0);
    
    if (!client->keyboard_hook || !client->mouse_hook) {
        LOG_ERROR("Failed to install Windows hooks (error: %lu)", GetLastError());
        if (client->keyboard_hook) UnhookWindowsHookEx(client->keyboard_hook);
        if (client->mouse_hook) UnhookWindowsHookEx(client->mouse_hook);
        client->keyboard_hook = NULL;
        client->mouse_hook = NULL;
        return false;
    }
    
    LOG_INFO("✓ Windows input hooks installed (keyboard + mouse)");
    return true;
}

/* Remove Windows input hooks */
static void remove_windows_hooks(ClientState *client) {
    if (client->keyboard_hook) {
        UnhookWindowsHookEx(client->keyboard_hook);
        client->keyboard_hook = NULL;
    }
    if (client->mouse_hook) {
        UnhookWindowsHookEx(client->mouse_hook);
        client->mouse_hook = NULL;
    }
}

/* Run Windows hooks on a dedicated thread with its own message loop. */
static DWORD WINAPI hook_thread_proc(LPVOID param) {
    ClientState *client = (ClientState *)param;
    MSG msg;

    /* Ensure the thread has a message queue before we post to it. */
    PeekMessage(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);

    if (!setup_windows_hooks(client)) {
        LOG_WARN("Windows input hooks not available - input capture disabled");
        return 1;
    }

    client->hook_thread_running = true;

    while (client->running && GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    client->hook_thread_running = false;
    remove_windows_hooks(client);
    return 0;
}

static bool start_windows_hook_thread(ClientState *client) {
    if (client->hook_thread) {
        return true;
    }

    client->hook_thread_running = false;
    client->hook_thread = CreateThread(NULL, 0, hook_thread_proc, client, 0,
                                       &client->hook_thread_id);
    if (!client->hook_thread) {
        LOG_ERROR("Failed to create Windows hook thread (error: %lu)", GetLastError());
        return false;
    }

    return true;
}

static void stop_windows_hook_thread(ClientState *client) {
    if (!client->hook_thread) {
        return;
    }

    /* Signal the hook thread to exit its message loop. */
    PostThreadMessage(client->hook_thread_id, WM_QUIT, 0, 0);
    WaitForSingleObject(client->hook_thread, 2000);
    CloseHandle(client->hook_thread);
    client->hook_thread = NULL;
    client->hook_thread_id = 0;
}
#endif

#ifdef PLATFORM_MACOS
static void inject_key_macos(const char *key, bool is_special, bool press) {
    /* TODO: Implement macOS key injection using CGEventPost */
    CGEventRef event = CGEventCreateKeyboardEvent(NULL, 0, press);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
}

static void inject_mouse_move_macos(int x, int y) {
    CGPoint point = CGPointMake(x, y);
    CGEventRef event = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved, point, kCGMouseButtonLeft);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
}

static void inject_mouse_button_macos(const char *button, bool press) {
    CGPoint point;
    CGEventRef moveEvent = CGEventCreate(NULL);
    point = CGEventGetLocation(moveEvent);
    CFRelease(moveEvent);
    
    CGMouseButton btn = kCGMouseButtonLeft;
    CGEventType eventType;
    
    if (strcmp(button, "Button.left") == 0) {
        btn = kCGMouseButtonLeft;
        eventType = press ? kCGEventLeftMouseDown : kCGEventLeftMouseUp;
    } else if (strcmp(button, "Button.right") == 0) {
        btn = kCGMouseButtonRight;
        eventType = press ? kCGEventRightMouseDown : kCGEventRightMouseUp;
    } else {
        btn = kCGMouseButtonCenter;
        eventType = press ? kCGEventOtherMouseDown : kCGEventOtherMouseUp;
    }
    
    CGEventRef event = CGEventCreateMouseEvent(NULL, eventType, point, btn);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
}
#endif

/* ============================================================================
 * WebSocket Client
 * ============================================================================ */

/* Protocol handler data */
struct ws_session_data {
    char rx_buffer[MAX_MESSAGE_SIZE];
    size_t rx_len;
};

/* Forward declarations */
static void handle_server_message(ClientState *client, const char *msg, size_t len);
static void send_input_event(ClientState *client, const char *event_type, 
                             const char *json_data);
static void apply_active_monitor_changed(ClientState *client,
                                         const char *new_computer,
                                         const char *new_monitor,
                                         double cursor_x,
                                         double cursor_y);

/* WebSocket callback */
static int ws_callback(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len) {
    struct ws_session_data *session = (struct ws_session_data *)user;
    
    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            LOG_INFO("WebSocket connected");
            g_client.connected = true;
            g_client.reconnect_attempts = 0;
            g_client.last_server_message_time = get_time_ms();  /* Initialize connection health */
            
#ifdef PLATFORM_LINUX
            /* For player computers: hide cursor immediately on connect
             * (main computer controls, so non-main should have hidden cursor by default) */
            if (strcmp(g_client.config.role, "player") == 0 && g_client.x_display) {
                LOG_INFO("Player connected - hiding cursor until becomes active");
                x11_grab_input(g_client.x_display);
                hide_cursor_linux(g_client.x_display);
            }
#endif
            
            /* Request write callback to send registration */
            lws_callback_on_writable(wsi);
            break;
            
        case LWS_CALLBACK_CLIENT_RECEIVE:
            if (len > 0 && in) {
                /* Accumulate message (could be fragmented) */
                if (session->rx_len + len < MAX_MESSAGE_SIZE) {
                    memcpy(session->rx_buffer + session->rx_len, in, len);
                    session->rx_len += len;
                }
                
                /* Check if message is complete (simple check for JSON) */
                if (lws_is_final_fragment(wsi)) {
                    session->rx_buffer[session->rx_len] = '\0';
                    /* Update connection health timestamp */
                    g_client.last_server_message_time = get_time_ms();
                    
                    /* Debug: log clipboard messages specifically */
                    if (strstr(session->rx_buffer, "clipboard") != NULL) {
                        LOG_INFO("📋 RAW WS MSG (clipboard): %.200s%s", 
                                 session->rx_buffer, 
                                 session->rx_len > 200 ? "..." : "");
                    }
                    
                    handle_server_message(&g_client, session->rx_buffer, session->rx_len);
                    session->rx_len = 0;
                }
            }
            break;
            
        case LWS_CALLBACK_CLIENT_WRITEABLE:
            /* First, send any pending large message (registration, layout) */
            if (g_client.has_pending_message && g_client.send_buffer_len > 0) {
                /* LWS requires LWS_PRE bytes before the data */
                unsigned char buf[LWS_PRE + MAX_MESSAGE_SIZE];
                memcpy(buf + LWS_PRE, g_client.send_buffer, g_client.send_buffer_len);
                
                int written = lws_write(wsi, buf + LWS_PRE, g_client.send_buffer_len, 
                                         LWS_WRITE_TEXT);
                if (written < 0) {
                    LOG_ERROR("WebSocket write failed");
                    return -1;
                }
                
                g_client.has_pending_message = false;
                g_client.send_buffer_len = 0;
                
                /* If there are queued messages, request another write callback */
                if (msg_queue_has_messages(&g_client.msg_queue)) {
                    lws_callback_on_writable(wsi);
                }
            } else {
                /* Drain multiple messages from queue per callback for better throughput */
                /* Send up to 64 messages per callback to keep up with capture rate */
                int batch_count = 0;
                const int MAX_BATCH = 64;
                char msg[MSG_MAX_LEN];
                
                static int total_sent = 0;
                int queue_before = msg_queue_count(&g_client.msg_queue);
                
                while (batch_count < MAX_BATCH && 
                       msg_queue_pop(&g_client.msg_queue, msg, sizeof(msg))) {
                    size_t msg_len = strlen(msg);
                    unsigned char buf[LWS_PRE + MSG_MAX_LEN];
                    memcpy(buf + LWS_PRE, msg, msg_len);
                    
                    /* Debug: log clipboard messages being sent */
                    if (strstr(msg, "clipboard") != NULL) {
                        LOG_INFO("📋 WS SENDING clipboard: %.200s%s", msg, msg_len > 200 ? "..." : "");
                    }
                    
                    /* Use LWS_WRITE_TEXT for first, LWS_WRITE_CONTINUATION for rest */
                    int flags = LWS_WRITE_TEXT;
                    int written = lws_write(wsi, buf + LWS_PRE, msg_len, flags);
                    if (written < 0) {
                        LOG_ERROR("WebSocket write failed for queued message");
                        return -1;
                    }
                    batch_count++;
                    total_sent++;
                }
                
                /* Log send stats periodically (reduced frequency to reduce log size) */
                if (batch_count > 0 && (total_sent <= 10 || total_sent % 500 == 0)) {
                    LOG_INFO("📤 WS SENT: %d msgs (total=%d), queue was %d now %d", 
                             batch_count, total_sent, queue_before, 
                             msg_queue_count(&g_client.msg_queue));
                }
                
                /* If more messages in queue, request another write callback */
                if (msg_queue_has_messages(&g_client.msg_queue)) {
                    lws_callback_on_writable(wsi);
                }
            }
            break;
            
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            LOG_ERROR("WebSocket connection error: %s", in ? (char*)in : "unknown");
            LOG_ERROR("Failed to connect to server. Check:");
            LOG_ERROR("  - Server is running and accessible");
            LOG_ERROR("  - Server URL and port are correct");
            LOG_ERROR("  - Firewall allows connection");
            LOG_ERROR("  - Network connectivity is working");
            LOG_INFO("Server connection lost. Exiting gracefully...");
#ifdef PLATFORM_LINUX
            /* Immediately restore cursor and ungrab input */
            if (g_client.x_display) {
                show_cursor_linux(g_client.x_display);
                x11_ungrab_input(g_client.x_display);
            }
#endif
            g_client.connected = false;
            g_client.running = false;
            g_shutdown = 1;  /* Trigger graceful shutdown */
            return -1;
            
        case LWS_CALLBACK_CLIENT_CLOSED:
            LOG_INFO("WebSocket closed by server");
            LOG_INFO("Server connection lost. Exiting gracefully...");
#ifdef PLATFORM_LINUX
            /* Immediately restore cursor and ungrab input */
            if (g_client.x_display) {
                show_cursor_linux(g_client.x_display);
                x11_ungrab_input(g_client.x_display);
            }
#endif
            g_client.connected = false;
            g_client.running = false;
            g_shutdown = 1;  /* Trigger graceful shutdown */
            break;
            
        default:
            break;
    }
    
    return 0;
}

/* ============================================================================
 * Embedded WebSocket Server (for --role main without external server)
 * ============================================================================ */

/* Find a client slot by WSI */
static ServerClient* server_find_client_by_wsi(struct lws *wsi) {
    for (int i = 0; i < MAX_SERVER_CLIENTS; i++) {
        if (g_server.clients[i].wsi == wsi) {
            return &g_server.clients[i];
        }
    }
    return NULL;
}

/* Find a client by computer_id */
static ServerClient* server_find_client_by_id(const char *computer_id) {
    for (int i = 0; i < MAX_SERVER_CLIENTS; i++) {
        if (g_server.clients[i].registered &&
            strcmp(g_server.clients[i].computer_id, computer_id) == 0) {
            return &g_server.clients[i];
        }
    }
    return NULL;
}

/* Allocate a new client slot */
static ServerClient* server_allocate_client(struct lws *wsi) {
    for (int i = 0; i < MAX_SERVER_CLIENTS; i++) {
        if (!g_server.clients[i].registered && g_server.clients[i].wsi == NULL) {
            memset(&g_server.clients[i], 0, sizeof(ServerClient));
            g_server.clients[i].wsi = wsi;
            g_server.client_count++;
            return &g_server.clients[i];
        }
    }
    return NULL;
}

/* Free a client slot */
static void server_free_client(ServerClient *client) {
    if (client) {
        memset(client, 0, sizeof(ServerClient));
        g_server.client_count--;
    }
}

/* Send message to a specific client */
static void server_send_to_client(ServerClient *client, const char *msg) {
    if (!client || !client->wsi) return;

    size_t len = strlen(msg);
    unsigned char *buf = malloc(LWS_PRE + len + 1);
    if (!buf) return;

    memcpy(buf + LWS_PRE, msg, len);
    lws_write(client->wsi, buf + LWS_PRE, len, LWS_WRITE_TEXT);
    free(buf);
}

/* Broadcast message to all registered clients */
static void server_broadcast(const char *msg) {
    for (int i = 0; i < MAX_SERVER_CLIENTS; i++) {
        if (g_server.clients[i].registered && g_server.clients[i].wsi) {
            server_send_to_client(&g_server.clients[i], msg);
        }
    }
}

/* Broadcast message to all clients except one */
static void server_broadcast_except(const char *msg, const char *except_computer_id) {
    for (int i = 0; i < MAX_SERVER_CLIENTS; i++) {
        if (g_server.clients[i].registered && g_server.clients[i].wsi) {
            if (strcmp(g_server.clients[i].computer_id, except_computer_id) != 0) {
                server_send_to_client(&g_server.clients[i], msg);
            }
        }
    }
}

/* Handle registration message from client */
static void server_handle_registration(ServerClient *client, const char *json_data) {
    /* Parse computer_id */
    const char *id_start = strstr(json_data, "\"computer_id\"");
    if (id_start) {
        id_start = strchr(id_start + 13, '"');
        if (id_start) {
            id_start++;
            const char *id_end = strchr(id_start, '"');
            if (id_end) {
                size_t len = id_end - id_start;
                if (len < MAX_HOSTNAME) {
                    strncpy(client->computer_id, id_start, len);
                    client->computer_id[len] = '\0';
                }
            }
        }
    }

    /* Parse role (is_main) */
    if (strstr(json_data, "\"is_main\":true") || strstr(json_data, "\"is_main\": true")) {
        strcpy(client->role, "main");
    } else if (strstr(json_data, "\"role\":\"main\"") || strstr(json_data, "\"role\": \"main\"")) {
        strcpy(client->role, "main");
    } else {
        strcpy(client->role, "player");
    }

    /* Parse monitors */
    const char *monitors_start = strstr(json_data, "\"monitors\"");
    if (monitors_start) {
        monitors_start = strchr(monitors_start, '[');
        if (monitors_start) {
            client->monitor_count = 0;
            const char *mon_ptr = monitors_start;
            while ((mon_ptr = strstr(mon_ptr, "\"monitor_id\"")) && client->monitor_count < MAX_MONITORS) {
                Monitor *mon = &client->monitors[client->monitor_count];

                /* Parse monitor_id */
                const char *mid_start = strchr(mon_ptr + 12, '"');
                if (mid_start) {
                    mid_start++;
                    const char *mid_end = strchr(mid_start, '"');
                    if (mid_end) {
                        size_t len = mid_end - mid_start;
                        if (len < sizeof(mon->monitor_id)) {
                            strncpy(mon->monitor_id, mid_start, len);
                            mon->monitor_id[len] = '\0';
                        }
                    }
                }

                /* Parse width */
                const char *w_start = strstr(mon_ptr, "\"width\"");
                if (w_start) {
                    w_start = strchr(w_start + 7, ':');
                    if (w_start) mon->width = atoi(w_start + 1);
                }

                /* Parse height */
                const char *h_start = strstr(mon_ptr, "\"height\"");
                if (h_start) {
                    h_start = strchr(h_start + 8, ':');
                    if (h_start) mon->height = atoi(h_start + 1);
                }

                mon->x = 0;
                mon->y = 0;
                client->monitor_count++;
                mon_ptr = mid_start ? mid_start : mon_ptr + 1;
            }
        }
    }

    client->registered = true;

    /* If this is the first main client or first client overall, set as active */
    if (g_server.active_computer_id[0] == '\0' || strcmp(client->role, "main") == 0) {
        strncpy(g_server.active_computer_id, client->computer_id, MAX_HOSTNAME - 1);
        strcpy(g_server.active_monitor_id, client->monitor_count > 0 ? client->monitors[0].monitor_id : "m0");
        g_server.cursor_x = client->monitor_count > 0 ? client->monitors[0].width / 2.0 : 960;
        g_server.cursor_y = client->monitor_count > 0 ? client->monitors[0].height / 2.0 : 540;
    }

    LOG_INFO("Server: Registered %s '%s' with %d monitors (active: %s:%s)",
             client->role, client->computer_id, client->monitor_count,
             g_server.active_computer_id, g_server.active_monitor_id);

    /* Send registration acknowledgment */
    char response[2048];
    snprintf(response, sizeof(response),
             "{\"type\":\"registration_success\",\"computer_id\":\"%s\",\"status\":\"success\","
             "\"active_monitor\":{\"computer_id\":\"%s\",\"monitor_id\":\"%s\","
             "\"cursor_x\":%.1f,\"cursor_y\":%.1f}}",
             client->computer_id, g_server.active_computer_id, g_server.active_monitor_id,
             g_server.cursor_x, g_server.cursor_y);
    server_send_to_client(client, response);

    /* Notify all clients of the active monitor */
    char active_msg[512];
    snprintf(active_msg, sizeof(active_msg),
             "{\"type\":\"active_monitor_changed\",\"computer_id\":\"%s\",\"monitor_id\":\"%s\","
             "\"cursor_x\":%.1f,\"cursor_y\":%.1f}",
             g_server.active_computer_id, g_server.active_monitor_id,
             g_server.cursor_x, g_server.cursor_y);
    server_broadcast(active_msg);

    /* Apply active change locally in embedded server mode */
    if (g_server.is_server_mode && strcmp(g_client.config.role, "main") == 0) {
        apply_active_monitor_changed(&g_client, g_server.active_computer_id,
                                     g_server.active_monitor_id,
                                     g_server.cursor_x, g_server.cursor_y);
    }
}

/* Handle input event from client and route to active computer */
static void server_handle_input_event(ServerClient *sender, const char *json_data) {
    /* Parse event_type */
    char event_type[32] = "";
    const char *et_start = strstr(json_data, "\"event_type\"");
    if (et_start) {
        et_start = strchr(et_start + 12, '"');
        if (et_start) {
            et_start++;
            const char *et_end = strchr(et_start, '"');
            if (et_end && et_end - et_start < (int)sizeof(event_type)) {
                strncpy(event_type, et_start, et_end - et_start);
                event_type[et_end - et_start] = '\0';
            }
        }
    }

    /* Build forwarding message with active computer info */
    char fwd_msg[MAX_MESSAGE_SIZE];
    size_t json_len = strlen(json_data);

    /* Find the last } in the JSON */
    const char *last_brace = strrchr(json_data, '}');
    if (last_brace && json_len < MAX_MESSAGE_SIZE - 200) {
        size_t prefix_len = last_brace - json_data;
        memcpy(fwd_msg, json_data, prefix_len);
        snprintf(fwd_msg + prefix_len, sizeof(fwd_msg) - prefix_len,
                 ",\"active_computer_id\":\"%s\",\"active_monitor_id\":\"%s\"}",
                 g_server.active_computer_id, g_server.active_monitor_id);
    } else {
        strncpy(fwd_msg, json_data, sizeof(fwd_msg) - 1);
    }

    /* Broadcast to all clients - each client decides if it should execute */
    server_broadcast(fwd_msg);
}

/* Handle edge crossing request */
static void server_handle_edge_crossing(const char *json_data) {
    /* Parse edge crossing parameters */
    char computer_id[MAX_HOSTNAME] = "";
    char monitor_id[32] = "";
    char edge[16] = "";
    double position = 0.5;

    const char *cid = strstr(json_data, "\"computer_id\"");
    if (cid) {
        cid = strchr(cid + 13, '"');
        if (cid) {
            cid++;
            const char *end = strchr(cid, '"');
            if (end && end - cid < MAX_HOSTNAME) {
                strncpy(computer_id, cid, end - cid);
            }
        }
    }

    const char *mid = strstr(json_data, "\"monitor_id\"");
    if (mid) {
        mid = strchr(mid + 12, '"');
        if (mid) {
            mid++;
            const char *end = strchr(mid, '"');
            if (end && end - mid < 32) {
                strncpy(monitor_id, mid, end - mid);
            }
        }
    }

    const char *edg = strstr(json_data, "\"edge\"");
    if (edg) {
        edg = strchr(edg + 6, '"');
        if (edg) {
            edg++;
            const char *end = strchr(edg, '"');
            if (end && end - edg < 16) {
                strncpy(edge, edg, end - edg);
            }
        }
    }

    const char *pos = strstr(json_data, "\"position\"");
    if (pos) {
        pos = strchr(pos + 10, ':');
        if (pos) position = atof(pos + 1);
    }

    LOG_INFO("Server: Edge crossing request from %s:%s edge=%s pos=%.2f",
             computer_id, monitor_id, edge, position);

    /* Ignore edge requests from inactive computers to prevent flip-flop */
    if (g_server.active_computer_id[0] &&
        strcmp(computer_id, g_server.active_computer_id) != 0) {
        static int ignore_count = 0;
        ignore_count++;
        if (ignore_count <= 5 || ignore_count % 50 == 0) {
            LOG_INFO("Server: Ignoring edge crossing from inactive %s (active=%s)",
                     computer_id, g_server.active_computer_id);
        }
        return;
    }

    /* Debounce rapid edge crossings */
    double now_ms = get_time_ms();
    if (g_server.last_edge_crossing_time > 0 &&
        now_ms - g_server.last_edge_crossing_time < 200) {
        return;
    }
    g_server.last_edge_crossing_time = now_ms;

    /* Find target computer - simple logic: cycle through registered players */
    ServerClient *target = NULL;

    /* Find a different registered client to switch to */
    for (int i = 0; i < MAX_SERVER_CLIENTS; i++) {
        if (g_server.clients[i].registered &&
            strcmp(g_server.clients[i].computer_id, g_server.active_computer_id) != 0) {
            target = &g_server.clients[i];
            break;
        }
    }

    if (!target) {
        LOG_INFO("Server: No other computer to switch to");
        return;
    }

    /* Update active computer */
    strncpy(g_server.active_computer_id, target->computer_id, MAX_HOSTNAME - 1);
    strcpy(g_server.active_monitor_id, target->monitor_count > 0 ? target->monitors[0].monitor_id : "m0");

    /* Calculate cursor position on target */
    double new_x = 100, new_y = 100;  /* Default offset from edge */
    if (target->monitor_count > 0) {
        Monitor *mon = &target->monitors[0];
        if (strcmp(edge, "right") == 0) {
            new_x = 50;  /* Enter from left */
            new_y = position * mon->height;
        } else if (strcmp(edge, "left") == 0) {
            new_x = mon->width - 50;  /* Enter from right */
            new_y = position * mon->height;
        } else if (strcmp(edge, "bottom") == 0) {
            new_x = position * mon->width;
            new_y = 50;  /* Enter from top */
        } else if (strcmp(edge, "top") == 0) {
            new_x = position * mon->width;
            new_y = mon->height - 50;  /* Enter from bottom */
        }
    }

    g_server.cursor_x = new_x;
    g_server.cursor_y = new_y;

    LOG_INFO("Server: Switched active to %s:%s cursor=(%.1f, %.1f)",
             g_server.active_computer_id, g_server.active_monitor_id, new_x, new_y);

    /* Broadcast active monitor change to all clients */
    char msg[512];
    snprintf(msg, sizeof(msg),
             "{\"type\":\"active_monitor_changed\",\"computer_id\":\"%s\",\"monitor_id\":\"%s\","
             "\"cursor_x\":%.1f,\"cursor_y\":%.1f}",
             g_server.active_computer_id, g_server.active_monitor_id, new_x, new_y);
    server_broadcast(msg);

    /* Apply active change locally in embedded server mode */
    if (g_server.is_server_mode && strcmp(g_client.config.role, "main") == 0) {
        apply_active_monitor_changed(&g_client, g_server.active_computer_id,
                                     g_server.active_monitor_id, new_x, new_y);
    }
}

/* Handle message from client */
static void server_handle_message(ServerClient *client, const char *json_data, size_t len) {
    UNUSED(len);

    /* Determine message type */
    if (strstr(json_data, "\"type\":\"register_device\"") ||
        strstr(json_data, "\"type\": \"register_device\"")) {
        server_handle_registration(client, json_data);
    } else if (strstr(json_data, "\"type\":\"input_event\"") ||
               strstr(json_data, "\"type\": \"input_event\"")) {
        server_handle_input_event(client, json_data);
    } else if (strstr(json_data, "\"type\":\"edge_crossing_request\"") ||
               strstr(json_data, "\"type\": \"edge_crossing_request\"")) {
        server_handle_edge_crossing(json_data);
    } else if (strstr(json_data, "\"type\":\"ping\"")) {
        server_send_to_client(client, "{\"type\":\"pong\"}");
    } else {
        LOG_DEBUG("Server: Unknown message type: %.100s", json_data);
    }
}

/* Server WebSocket callback */
static int server_ws_callback(struct lws *wsi, enum lws_callback_reasons reason,
                               void *user, void *in, size_t len) {
    ServerClientSession *session = (ServerClientSession *)user;
    ServerClient *client = session ? session->client : NULL;

    switch (reason) {
        case LWS_CALLBACK_ESTABLISHED:
            LOG_INFO("Server: New connection");
            if (session && !session->client) {
                session->client = server_allocate_client(wsi);
                if (!session->client) {
                    LOG_ERROR("Server: Max clients reached");
                    return -1;
                }
            }
            client = session ? session->client : NULL;
            if (client) client->wsi = wsi;
            break;

        case LWS_CALLBACK_RECEIVE:
            if (!client) client = server_find_client_by_wsi(wsi);
            if (client && len > 0 && in) {
                if (client->rx_len + len < MAX_MESSAGE_SIZE) {
                    memcpy(client->rx_buffer + client->rx_len, in, len);
                    client->rx_len += len;
                }
                if (lws_is_final_fragment(wsi)) {
                    client->rx_buffer[client->rx_len] = '\0';
                    server_handle_message(client, client->rx_buffer, client->rx_len);
                    client->rx_len = 0;
                }
            }
            break;

        case LWS_CALLBACK_CLOSED:
            LOG_INFO("Server: Connection closed");
            if (!client) client = server_find_client_by_wsi(wsi);
            if (client) {
                if (client->registered) {
                    LOG_INFO("Server: Client '%s' disconnected", client->computer_id);

                    /* If active computer disconnected, switch to another */
                    if (strcmp(client->computer_id, g_server.active_computer_id) == 0) {
                        g_server.active_computer_id[0] = '\0';
                        for (int i = 0; i < MAX_SERVER_CLIENTS; i++) {
                            if (g_server.clients[i].registered &&
                                g_server.clients[i].wsi != wsi) {
                                strncpy(g_server.active_computer_id,
                                        g_server.clients[i].computer_id, MAX_HOSTNAME - 1);
                                strcpy(g_server.active_monitor_id,
                                       g_server.clients[i].monitor_count > 0 ?
                                       g_server.clients[i].monitors[0].monitor_id : "m0");
                                LOG_INFO("Server: Switched active to %s", g_server.active_computer_id);
                                break;
                            }
                        }
                    }
                }
                server_free_client(client);
            }
            if (session) session->client = NULL;
            break;

        case LWS_CALLBACK_SERVER_WRITEABLE:
            /* Drain broadcast queue and send to this client */
            if (client && client->registered) {
                char msg[MSG_MAX_LEN];
                int sent = 0;
                while (msg_queue_pop(&g_server.broadcast_queue, msg, sizeof(msg))) {
                    /* Broadcast this message to ALL registered clients */
                    for (int i = 0; i < MAX_SERVER_CLIENTS; i++) {
                        if (g_server.clients[i].registered && g_server.clients[i].wsi) {
                            size_t len = strlen(msg);
                            unsigned char *buf = malloc(LWS_PRE + len + 1);
                            if (buf) {
                                memcpy(buf + LWS_PRE, msg, len);
                                lws_write(g_server.clients[i].wsi, buf + LWS_PRE, len, LWS_WRITE_TEXT);
                                free(buf);
                            }
                        }
                    }
                    sent++;
                    if (sent >= 64) break;  /* Limit per callback */
                }
                /* If more messages, request another writable callback */
                if (msg_queue_has_messages(&g_server.broadcast_queue)) {
                    lws_callback_on_writable(wsi);
                }
            }
            break;

        case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
            /* Triggered by lws_cancel_service - request writable for all clients */
            if (msg_queue_has_messages(&g_server.broadcast_queue)) {
                for (int i = 0; i < MAX_SERVER_CLIENTS; i++) {
                    if (g_server.clients[i].registered && g_server.clients[i].wsi) {
                        lws_callback_on_writable(g_server.clients[i].wsi);
                    }
                }
            }
            break;

        default:
            break;
    }

    return 0;
}

/* Server protocols */
static struct lws_protocols server_protocols[] = {
    {
        "kvm-protocol",
        server_ws_callback,
        sizeof(ServerClientSession),
        MAX_MESSAGE_SIZE,
        0, NULL, 0
    },
    { NULL, NULL, 0, 0, 0, NULL, 0 }
};

/* Start embedded WebSocket server */
static int start_embedded_server(int port) {
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));

    /* Initialize broadcast queue for thread-safe messaging from hooks */
    msg_queue_init(&g_server.broadcast_queue);

    info.port = port;
    info.protocols = server_protocols;
    info.gid = -1;
    info.uid = -1;
    info.options = LWS_SERVER_OPTION_VALIDATE_UTF8;

    g_server.ws_context = lws_create_context(&info);
    if (!g_server.ws_context) {
        LOG_ERROR("Failed to create server context");
        return -1;
    }

    g_server.port = port;
    g_server.running = true;
    g_server.is_server_mode = true;

    LOG_INFO("Embedded WebSocket server started on port %d", port);
    return 0;
}

/* Protocol list */
static struct lws_protocols protocols[] = {
    {
        "kvm-protocol",
        ws_callback,
        sizeof(struct ws_session_data),
        MAX_MESSAGE_SIZE,
        0, NULL, 0
    },
    { NULL, NULL, 0, 0, 0, NULL, 0 }
};

/* Queue message for sending */
static void queue_ws_message(ClientState *client, const char *msg) {
    size_t len = strlen(msg);
    if (len >= MAX_MESSAGE_SIZE) {
        LOG_ERROR("Message too large: %zu bytes", len);
        return;
    }
    
    memcpy(client->send_buffer, msg, len);
    client->send_buffer_len = len;
    client->has_pending_message = true;
    
    if (client->ws_connection) {
        lws_callback_on_writable(client->ws_connection);
    }
}

/* Queue message to client send queue (thread-safe). */
static void queue_client_message(ClientState *client, const char *msg, const char *tag) {
    if (!msg_queue_push(&client->msg_queue, msg)) {
        static int drop_count = 0;
        drop_count++;
        if (drop_count <= 3 || drop_count % 100 == 0) {
            LOG_WARN("Message queue full, dropping %s (%d total)", tag, drop_count);
        }
        return;
    }

    if (client->ws_context) {
        lws_cancel_service(client->ws_context);
    }
}

/* Queue message to server broadcast queue (thread-safe). */
static void queue_server_broadcast(const char *msg, const char *tag) {
    if (!msg_queue_push(&g_server.broadcast_queue, msg)) {
        static int drop_count = 0;
        drop_count++;
        if (drop_count <= 3 || drop_count % 100 == 0) {
            LOG_WARN("Server broadcast queue full, dropping %s (%d total)", tag, drop_count);
        }
        return;
    }

    if (g_server.ws_context) {
        lws_cancel_service(g_server.ws_context);
    }
}

/* Send edge crossing request to server */
static void send_edge_crossing_request(ClientState *client, const char *edge,
                                       float position, int cursor_x, int cursor_y) {
    if (!client->running) return;
    if (!g_server.is_server_mode && !client->connected) return;
    
    /* Only send from main computer when active */
    if (strcmp(client->config.role, "main") != 0 || !client->is_active) return;
    
    /* Get monitor ID (default to m0) */
    const char *monitor_id = (client->monitor_count > 0) ? client->monitors[0].monitor_id : "m0";
    
    /* Build JSON message directly */
    char json_msg[512];
    snprintf(json_msg, sizeof(json_msg),
        "{\"type\":\"edge_crossing_request\","
        "\"computer_id\":\"%s\","
        "\"monitor_id\":\"%s\","
        "\"edge\":\"%s\","
        "\"position\":%.3f,"
        "\"cursor_x\":%d,"
        "\"cursor_y\":%d}",
        client->config.computer_id, monitor_id, edge, position, cursor_x, cursor_y);
    
    /* In server mode, handle edge crossing directly */
    if (g_server.is_server_mode) {
        server_handle_edge_crossing(json_msg);
        return;
    }
    queue_client_message(client, json_msg, "edge_crossing_request");
    LOG_INFO("Queued edge_crossing_request: edge=%s pos=%.2f cursor=(%d,%d)",
             edge, position, cursor_x, cursor_y);
}

/* Fast path for high-frequency mouse moves (avoid heap allocations). */
static void send_mouse_move_fast(ClientState *client, int x, int y, double now_ms) {
    if (!client->running) return;
    if (!g_server.is_server_mode && !client->connected) return;

    double delta_ms = (client->last_input_time > 0) ? (now_ms - client->last_input_time) : 0;
    client->last_input_time = now_ms;

    if (g_server.is_server_mode) {
        char full_msg[512];
        int n = snprintf(full_msg, sizeof(full_msg),
            "{\"type\":\"input_event\",\"event_type\":\"mouse_move\","
            "\"data\":{\"x\":%d,\"y\":%d},\"device_id\":\"%s\",\"delta_ms\":%.2f,"
            "\"active_computer_id\":\"%s\",\"active_monitor_id\":\"%s\"}",
            x, y, client->config.computer_id, delta_ms,
            g_server.active_computer_id, g_server.active_monitor_id);
        if (n < 0 || n >= (int)sizeof(full_msg)) {
            LOG_WARN("mouse_move message too large, dropping");
            return;
        }
        queue_server_broadcast(full_msg, "mouse_move");
        return;
    }

    char msg[256];
    int n = snprintf(msg, sizeof(msg),
        "{\"type\":\"input_event\",\"event_type\":\"mouse_move\","
        "\"data\":{\"x\":%d,\"y\":%d},\"device_id\":\"%s\",\"delta_ms\":%.2f}",
        x, y, client->config.computer_id, delta_ms);
    if (n < 0 || n >= (int)sizeof(msg)) {
        LOG_WARN("mouse_move message too large, dropping");
        return;
    }
    queue_client_message(client, msg, "mouse_move");
}

/* Send input event to server (uses thread-safe message queue) */
static void send_input_event(ClientState *client, const char *event_type,
                             const char *json_data) {
    /* Debug: Log first few calls */
    static int send_debug_count = 0;
    if (send_debug_count < 10) {
        send_debug_count++;
        LOG_INFO("SEND DEBUG #%d: type=%s, running=%d, server_mode=%d, connected=%d",
                 send_debug_count, event_type, client->running, g_server.is_server_mode, client->connected);
    }

    /* Fast path: Return immediately if not ready */
    /* In server mode, we don't need client->connected */
    if (!client->running) return;
    if (!g_server.is_server_mode && !client->connected) return;
    
    /* Track performance for mouse moves (most frequent event) */
    static double last_perf_log_send = 0;
    static int send_count = 0;
    static double send_total_time = 0;
    double send_start = 0;
    bool is_mouse_move = (strcmp(event_type, "mouse_move") == 0);
    
    if (is_mouse_move) {
        send_start = get_time_ms();
    }
    
    double current_time = get_time_ms();
    double delta_ms = (client->last_input_time > 0) ? 
                      (current_time - client->last_input_time) : 0;
    client->last_input_time = current_time;
    
    char delta_str[32];
    snprintf(delta_str, sizeof(delta_str), "%.2f", delta_ms);
    
    char *msg = json_create_message("input_event",
        "event_type", event_type,
        "data", json_data,
        "device_id", client->config.computer_id,
        "delta_ms", delta_str,
        NULL);
    
    if (!msg) {
        LOG_ERROR("Failed to create JSON message for %s", event_type);
        return;
    }
    
    /* In server mode, queue message for async broadcast (avoid blocking hook) */
    if (g_server.is_server_mode) {
        static int server_send_debug = 0;
        if (server_send_debug < 5) {
            server_send_debug++;
            LOG_INFO("SEND DEBUG server: active=%s:%s event=%s",
                     g_server.active_computer_id, g_server.active_monitor_id, event_type);
        }
        /* Build full message with active computer info */
        char full_msg[MAX_MESSAGE_SIZE];
        size_t msg_len = strlen(msg);
        /* Find last } to insert active computer info */
        const char *last_brace = strrchr(msg, '}');
        if (last_brace && msg_len < MAX_MESSAGE_SIZE - 200) {
            size_t prefix_len = last_brace - msg;
            memcpy(full_msg, msg, prefix_len);
            snprintf(full_msg + prefix_len, sizeof(full_msg) - prefix_len,
                     ",\"active_computer_id\":\"%s\",\"active_monitor_id\":\"%s\"}",
                     g_server.active_computer_id, g_server.active_monitor_id);
        } else {
            strncpy(full_msg, msg, sizeof(full_msg) - 1);
            full_msg[sizeof(full_msg) - 1] = '\0';
        }

        /* Queue message instead of direct broadcast (thread-safe for hooks) */
        queue_server_broadcast(full_msg, event_type);

        free(msg);
        return;
    }

    /* Client mode: Use thread-safe queue for input events */
    if (msg_queue_push(&client->msg_queue, msg)) {
        /* Log send performance for mouse moves */
        if (is_mouse_move) {
            send_count++;
            double send_duration = get_time_ms() - send_start;
            send_total_time += send_duration;

            if (send_duration > 2.0) {
                LOG_WARN("⚠️ send_input_event took %.2fms for mouse_move", send_duration);
            }

            if (current_time - last_perf_log_send >= 10000) {
                double avg_time = send_count > 0 ? send_total_time / send_count : 0;
                LOG_INFO("📤 send_input_event perf: count=%d, avg=%.3fms",
                         send_count, avg_time);
                send_count = 0;
                send_total_time = 0;
                last_perf_log_send = current_time;
            }
        }

        /* CRITICAL: Use lws_cancel_service to wake up main loop from capture thread
         * lws_callback_on_writable is NOT thread-safe and causes delays! */
        if (client->ws_context) {
            lws_cancel_service(client->ws_context);
        }
        free(msg);
    } else {
        static int overflow_count = 0;
        overflow_count++;
        if (overflow_count <= 3 || overflow_count % 100 == 0) {
            LOG_WARN("Message queue full, dropping %s (%d total)", event_type, overflow_count);
        }
        free(msg);
    }
}

/* ============================================================================
 * Message Handlers
 * ============================================================================ */

static void handle_input_event(ClientState *client, JsonValue *msg) {
    const char *event_type = json_get_string(msg, "event_type", "");
    JsonValue *data = json_get(msg, "data");
    const char *active_computer = json_get_string(msg, "active_computer_id", "");
    
    /* CLIPBOARD: Handle BEFORE is_for_me check - clipboard goes to ALL computers */
#ifdef PLATFORM_LINUX
    if (strcmp(event_type, "clipboard_update") == 0) {
        const char *source = json_get_string(data, "source", "");
        const char *content = json_get_string(data, "content", "");
        
        /* Don't update if this is our own clipboard update echoed back */
        if (strcmp(source, client->config.computer_id) == 0) {
            LOG_DEBUG("📋 Ignoring own clipboard echo");
            return;
        }
        
        size_t content_len = strlen(content);
        if (content_len > 0 && content_len < CLIPBOARD_MAX_SIZE) {
            if (clipboard_set(content, content_len)) {
                /* Update our tracking to prevent re-sending */
                if (content_len < sizeof(client->last_clipboard)) {
                    memcpy(client->last_clipboard, content, content_len + 1);
                    client->last_clipboard_len = content_len;
                }
                LOG_INFO("📋 Received clipboard from %s (%zu chars)", source, content_len);
            }
        }
        return;  /* Clipboard handled, don't process as regular input */
    }
#endif
    
    /* Only execute if this computer is active */
    bool is_for_me = (strcmp(active_computer, client->config.computer_id) == 0);
    
    /* Log input events (reduced frequency to prevent log bloat) */
    static int mouse_move_count = 0;
    static int non_move_count = 0;
    if (strcmp(event_type, "mouse_move") != 0) {
        /* Only log non-move events occasionally (every 50th) to reduce log size */
        non_move_count++;
        if (non_move_count <= 10 || non_move_count % 50 == 0) {
            LOG_INFO("📥 Received %s event, active=%s, for_me=%d", 
                      event_type, active_computer, is_for_me);
        }
    } else {
        mouse_move_count++;
        if (mouse_move_count % 500 == 1) {  /* Log every 500th mouse move (was 100) */
            LOG_INFO("📥 Received mouse_move (count=%d), active=%s, for_me=%d", 
                      mouse_move_count, active_computer, is_for_me);
        }
    }
    
    if (!is_for_me) return;
    
    /* Main role never executes received input (physical input works natively) */
    if (strcmp(client->config.role, "main") == 0) {
        LOG_DEBUG("Skipping execution (main role)");
        return;
    }
    
    client->executing_input = true;
    
#ifdef PLATFORM_LINUX
    Display *display = client->x_display;
    
    if (strcmp(event_type, "keyboard") == 0) {
        const char *action = json_get_string(data, "action", "");
        const char *key = json_get_string(data, "key", "");
        bool is_special = json_get_bool(data, "is_special", false);
        
        LOG_INFO("⌨️  Executing key: %s %s", action, key);
        inject_key_linux(display, key, is_special, strcmp(action, "press") == 0);
        
    } else if (strcmp(event_type, "mouse_move") == 0) {
        double x = json_get_number(data, "x", 0);
        double y = json_get_number(data, "y", 0);
        
        /* Log first few mouse moves when active to confirm injection works */
        static int exec_count = 0;
        exec_count++;
        if (exec_count <= 5 || exec_count % 100 == 0) {
            LOG_INFO("🖱️  Executing mouse_move: (%.0f, %.0f) [exec #%d]", x, y, exec_count);
        }
        
        inject_mouse_move_linux(display, (int)x, (int)y);
        
    } else if (strcmp(event_type, "mouse_click") == 0) {
        const char *button = json_get_string(data, "button", "Button.left");
        const char *action = json_get_string(data, "action", "");
        double x = json_get_number(data, "x", 0);
        double y = json_get_number(data, "y", 0);
        
        inject_mouse_move_linux(display, (int)x, (int)y);
        inject_mouse_button_linux(display, button, strcmp(action, "press") == 0);
        
    } else if (strcmp(event_type, "mouse_scroll") == 0) {
        int dx = (int)json_get_number(data, "dx", 0);
        int dy = (int)json_get_number(data, "dy", 0);
        
        inject_mouse_scroll_linux(display, dx, dy);
    }
#endif

#ifdef PLATFORM_WINDOWS
    if (strcmp(event_type, "keyboard") == 0) {
        const char *action = json_get_string(data, "action", "");
        const char *key = json_get_string(data, "key", "");
        bool is_special = json_get_bool(data, "is_special", false);
        inject_key_windows(key, is_special, strcmp(action, "press") == 0);
    } else if (strcmp(event_type, "mouse_move") == 0) {
        inject_mouse_move_windows((int)json_get_number(data, "x", 0),
                                   (int)json_get_number(data, "y", 0));
    } else if (strcmp(event_type, "mouse_click") == 0) {
        inject_mouse_button_windows(json_get_string(data, "button", "Button.left"),
                                     strcmp(json_get_string(data, "action", ""), "press") == 0);
    } else if (strcmp(event_type, "mouse_scroll") == 0) {
        int dx = (int)json_get_number(data, "dx", 0);
        int dy = (int)json_get_number(data, "dy", 0);
        inject_mouse_scroll_windows(dx, dy);
    }
#endif

#ifdef PLATFORM_MACOS
    if (strcmp(event_type, "keyboard") == 0) {
        const char *action = json_get_string(data, "action", "");
        const char *key = json_get_string(data, "key", "");
        bool is_special = json_get_bool(data, "is_special", false);
        inject_key_macos(key, is_special, strcmp(action, "press") == 0);
    } else if (strcmp(event_type, "mouse_move") == 0) {
        inject_mouse_move_macos((int)json_get_number(data, "x", 0),
                                 (int)json_get_number(data, "y", 0));
    } else if (strcmp(event_type, "mouse_click") == 0) {
        inject_mouse_button_macos(json_get_string(data, "button", "Button.left"),
                                   strcmp(json_get_string(data, "action", ""), "press") == 0);
    }
#endif
    
    client->executing_input = false;
}

static void apply_active_monitor_changed(ClientState *client,
                                         const char *new_computer,
                                         const char *new_monitor,
                                         double cursor_x,
                                         double cursor_y) {
    LOG_INFO("Active monitor changed to %s:%s", new_computer, new_monitor);

    /* CRITICAL: Clear stale events from queue on ANY edge crossing
     * Old positions would cause erratic cursor jumps on the new active computer */
    msg_queue_clear(&client->msg_queue);

    bool was_active = client->is_active;
    client->is_active = (strcmp(new_computer, client->config.computer_id) == 0);
    strncpy(client->active_monitor_computer, new_computer,
            sizeof(client->active_monitor_computer) - 1);
    LOG_INFO("Active state: was_active=%d now_active=%d role=%s",
             was_active, client->is_active, client->config.role);

    if (client->is_active) {
        /* This computer is now ACTIVE */
#ifdef PLATFORM_LINUX
        /* Show cursor */
        show_cursor_linux(client->x_display);

        /* Ungrab input so local keyboard/mouse works */
        x11_ungrab_input(client->x_display);

        if (strcmp(client->config.role, "main") == 0) {
            ungrab_evdev_devices(client->evdev_fds, client->evdev_count);
            client->input_grabbed = false;
        }

        /* Set cursor to server position */
        if (cursor_x >= 0 && cursor_y >= 0 && client->x_display) {
            client->skip_mouse_moves = 10;
            inject_mouse_move_linux(client->x_display, (int)cursor_x, (int)cursor_y);
            LOG_INFO("Set cursor to (%.0f, %.0f)", cursor_x, cursor_y);
        }
#endif
#ifdef PLATFORM_WINDOWS
        /* Set cursor to server position */
        if (cursor_x >= 0 && cursor_y >= 0) {
            client->skip_mouse_moves = 10;
            SetCursorPos((int)cursor_x, (int)cursor_y);
            LOG_INFO("Set cursor to (%.0f, %.0f)", cursor_x, cursor_y);
        }
        /* Show cursor */
        while (ShowCursor(TRUE) < 0);  /* Ensure cursor is visible */
#endif
        LOG_INFO("This computer is now ACTIVE - local input enabled");

    } else {
        /* This computer is now INACTIVE */
#ifdef PLATFORM_LINUX
        if (strcmp(client->config.role, "main") == 0) {
            if (was_active && client->monitor_count > 0) {
                warp_cursor_to_center_linux(client->x_display, &client->monitors[0]);

                /* CRITICAL: Notify server that cursor was warped
                 * This resets the server's delta tracking to prevent wrong positions */
                Monitor *mon = &client->monitors[0];
                int center_x = mon->x + mon->width / 2;
                int center_y = mon->y + mon->height / 2;
                char json[128];
                snprintf(json, sizeof(json), "{\"x\":%d,\"y\":%d}", center_x, center_y);
                send_input_event(client, "cursor_reset", json);
                LOG_INFO("Sent cursor_reset to server (%d, %d)", center_x, center_y);
            }
            grab_evdev_devices(client->evdev_fds, client->evdev_count);
            client->input_grabbed = true;
        }

        /* Grab X11 input to block local keyboard/mouse */
        x11_grab_input(client->x_display);

        /* Hide cursor */
        hide_cursor_linux(client->x_display);
#endif
#ifdef PLATFORM_WINDOWS
        if (strcmp(client->config.role, "main") == 0) {
            LOG_INFO("Windows main inactive: local input blocked by hooks");
        }
        /* Hide cursor */
        while (ShowCursor(FALSE) >= 0);  /* Hide cursor */
#endif
        LOG_INFO("This computer is INACTIVE - local input blocked");
    }
}

static void handle_active_monitor_changed(ClientState *client, JsonValue *msg) {
    const char *new_computer = json_get_string(msg, "computer_id", "");
    const char *new_monitor = json_get_string(msg, "monitor_id", "");
    double cursor_x = json_get_number(msg, "cursor_x", -1);
    double cursor_y = json_get_number(msg, "cursor_y", -1);

    apply_active_monitor_changed(client, new_computer, new_monitor, cursor_x, cursor_y);
}

static void handle_registration_response(ClientState *client, JsonValue *msg) {
    const char *status = json_get_string(msg, "status", "");
    LOG_INFO("Registration confirmed: %s", status);
    
    /* Handle initial active monitor state */
    JsonValue *active_mon = json_get(msg, "active_monitor");
    if (active_mon) {
        handle_active_monitor_changed(client, active_mon);
    }
    
    /* IMPORTANT: Ensure cursor is properly hidden for non-active computers after registration
     * This catches the case where player computers connect while main is active */
#ifdef PLATFORM_LINUX
    if (!client->is_active && client->x_display) {
        LOG_INFO("Post-registration: NOT active - ensuring cursor hidden");
        x11_grab_input(client->x_display);
        hide_cursor_linux(client->x_display);
    }
#endif
    
    /* Display layout visualization */
    JsonValue *layout = json_get(msg, "layout");
    if (layout) {
        const char *viz = json_get_string(layout, "visualization", "");
        if (viz[0]) {
            /* Print visualization directly (includes Unicode box-drawing characters) */
            fprintf(stderr, "\n%s\n\n", viz);
            if (g_logger.log_file) {
                fprintf(g_logger.log_file, "\n%s\n\n", viz);
                fflush(g_logger.log_file);
            }
        }
    }
}

static void handle_server_message(ClientState *client, const char *msg, size_t len ATTRIBUTE_UNUSED) {
    JsonValue *json = json_parse(msg);
    if (!json) {
        LOG_ERROR("Failed to parse JSON: %.100s...", msg);
        return;
    }
    
    const char *type = json_get_string(json, "type", "");
    
    if (strcmp(type, "input_event") == 0) {
        handle_input_event(client, json);
    } else if (strcmp(type, "active_monitor_changed") == 0) {
        handle_active_monitor_changed(client, json);
    } else if (strcmp(type, "registration_success") == 0 || 
               strcmp(type, "device_registered") == 0) {
        handle_registration_response(client, json);
    } else if (strcmp(type, "clipboard_update") == 0) {
        LOG_INFO("📋 Received clipboard_update message from server");
#ifdef PLATFORM_LINUX
        handle_clipboard_update(client, json);
#endif
    } else {
        LOG_DEBUG("Unknown message type: %s", type);
    }
    
    json_free(json);
}

/* ============================================================================
 * Input Capture Thread (Linux)
 * ============================================================================ */

#ifdef PLATFORM_LINUX

static void* input_capture_thread(void *arg) {
    ClientState *client = (ClientState *)arg;
    
    struct input_event ev;
    fd_set fds;
    struct timeval tv;
    
    double last_x = client->monitors[0].width / 2.0;
    double last_y = client->monitors[0].height / 2.0;
    double last_move_time = 0;
    const double move_throttle_ms = 8;  /* ~120 Hz */
    
    LOG_INFO("Input capture thread started");
    
    while (client->input_thread_running && client->running) {
        FD_ZERO(&fds);
        int max_fd = 0;
        
        for (int i = 0; i < client->evdev_count; i++) {
            if (client->evdev_fds[i] >= 0) {
                FD_SET(client->evdev_fds[i], &fds);
                if (client->evdev_fds[i] > max_fd) {
                    max_fd = client->evdev_fds[i];
                }
            }
        }
        
        tv.tv_sec = 0;
        tv.tv_usec = 10000;  /* 10ms timeout */
        
        int ret = select(max_fd + 1, &fds, NULL, NULL, &tv);
        if (ret <= 0) continue;
        
        for (int i = 0; i < client->evdev_count; i++) {
            int fd = client->evdev_fds[i];
            if (fd < 0 || !FD_ISSET(fd, &fds)) continue;
            
            while (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
                /* Skip if executing received input */
                if (client->executing_input) continue;
                
                if (ev.type == EV_KEY) {
                    /* Keyboard or mouse button */
                    bool is_special = false;
                    const char *key_name = get_key_name(ev.code, &is_special);
                    const char *btn_name = get_button_name(ev.code);
                    
                    if (btn_name) {
                        /* Mouse button */
                        if (ev.value == 1 || ev.value == 0) {
                            char json[256];
                            snprintf(json, sizeof(json),
                                "{\"x\":%.0f,\"y\":%.0f,\"button\":\"%s\",\"action\":\"%s\"}",
                                last_x, last_y, btn_name,
                                ev.value == 1 ? "press" : "release");
                            send_input_event(client, "mouse_click", json);
                        }
                    } else if (key_name) {
                        /* Keyboard */
                        if (ev.value == 1 || ev.value == 0) {
                            char json[256];
                            snprintf(json, sizeof(json),
                                "{\"action\":\"%s\",\"key\":\"%s\",\"is_special\":%s}",
                                ev.value == 1 ? "press" : "release",
                                key_name,
                                is_special ? "true" : "false");
                            send_input_event(client, "keyboard", json);
                            
                            /* Check for ESC to exit */
                            if (ev.code == KEY_ESC && ev.value == 1) {
                                LOG_INFO("ESC pressed, stopping...");
                                client->running = false;
                            }
                        }
                    }
                    
                } else if (ev.type == EV_REL) {
                    /* Relative mouse movement */
                    if (ev.code == REL_X) {
                        last_x += ev.value;
                        /* Clamp to screen bounds */
                        if (last_x < 0) last_x = 0;
                        if (last_x > 4000) last_x = 4000;
                    } else if (ev.code == REL_Y) {
                        last_y += ev.value;
                        if (last_y < 0) last_y = 0;
                        if (last_y > 3000) last_y = 3000;
                    } else if (ev.code == REL_WHEEL) {
                        char json[128];
                        snprintf(json, sizeof(json),
                            "{\"x\":%.0f,\"y\":%.0f,\"dx\":0,\"dy\":%d}",
                            last_x, last_y, ev.value);
                        send_input_event(client, "mouse_scroll", json);
                    }
                    
                    /* Send mouse move (throttled) */
                    if (ev.code == REL_X || ev.code == REL_Y) {
                        double now = get_time_ms();
                        if (now - last_move_time >= move_throttle_ms) {
                            /* Skip if requested */
                            if (client->skip_mouse_moves > 0) {
                                client->skip_mouse_moves--;
                            } else {
                                char json[128];
                                snprintf(json, sizeof(json),
                                    "{\"x\":%.0f,\"y\":%.0f}", last_x, last_y);
                                send_input_event(client, "mouse_move", json);
                            }
                            last_move_time = now;
                        }
                    }
                    
                } else if (ev.type == EV_ABS) {
                    /* Absolute mouse position (touchpad, touchscreen) */
                    if (ev.code == ABS_X) {
                        last_x = ev.value;
                    } else if (ev.code == ABS_Y) {
                        last_y = ev.value;
                    }
                }
            }
        }
    }
    
    LOG_INFO("Input capture thread stopped");
    return NULL;
}

/* X11-based input capture (fallback when evdev not available) */
static void* x11_input_capture_thread(void *arg) {
    ClientState *client = (ClientState *)arg;
    
    /* Open separate display connection for thread safety */
    Display *display = XOpenDisplay(NULL);
    if (!display) {
        LOG_ERROR("Cannot open X11 display for input capture thread");
        return NULL;
    }
    
    /* Start XRecord thread for keyboard capture (main computer only) */
    pthread_t xrecord_thread = 0;
    bool keyboard_capture = false;
    
    if (strcmp(client->config.role, "main") == 0) {
        if (xrecord_setup(client, display, NULL)) {  /* Setup context on this display */
            /* Start dedicated thread for XRecord blocking call */
            if (pthread_create(&xrecord_thread, NULL, xrecord_thread_func, g_xrecord_ctx) == 0) {
                keyboard_capture = true;
            } else {
                LOG_WARN("Failed to create XRecord thread");
                g_xrecord_running = false;
            }
        }
    }
    
    Window root = DefaultRootWindow(display);
    
    /* Track previous state */
    int last_sent_x = -1, last_sent_y = -1;  /* Last position actually sent */
    unsigned int prev_mask = 0;
    double last_move_time = 0;
    const double move_throttle_ms = 16;  /* 16ms = ~60 Hz base rate (like Python) */
    const int min_move_distance = 5;     /* Minimum 5 pixels to count as movement */
    int poll_count = 0;
    int move_count = 0;
    int query_fail_count = 0;
    double start_time = get_time_ms();
    
    LOG_INFO("X11 input capture thread started (XQueryPointer polling, ~60Hz send rate%s)",
             keyboard_capture ? ", XRecord keyboard" : "");
    
    /* Get initial mouse position */
    Window root_ret, child_ret;
    int root_x, root_y, win_x, win_y;
    unsigned int mask;
    if (XQueryPointer(display, root, &root_ret, &child_ret, 
                      &root_x, &root_y, &win_x, &win_y, &mask)) {
        last_sent_x = root_x;
        last_sent_y = root_y;
        prev_mask = mask;
        LOG_INFO("Initial mouse position: (%d, %d)", root_x, root_y);
    } else {
        LOG_WARN("XQueryPointer failed on initial call");
    }
    
    while (client->input_thread_running && client->running) {
        poll_count++;
        
        /* Poll mouse position using XQueryPointer */
        if (XQueryPointer(display, root, &root_ret, &child_ret, 
                          &root_x, &root_y, &win_x, &win_y, &mask)) {
            
            double now = get_time_ms();
            double elapsed = now - start_time;
            
            /* Debug: Log polling status every 5 seconds for first 30 seconds */
            if (elapsed < 30000 && poll_count % 1250 == 0) {  /* Every ~5 sec at 250Hz */
                LOG_INFO("X11 poll stats: polls=%d, moves=%d, pos=(%d,%d), connected=%d, executing=%d, queue=%d",
                         poll_count, move_count, root_x, root_y, 
                         client->connected, client->executing_input,
                         msg_queue_count(&client->msg_queue));
            }
            
            /* Skip if executing received input or not connected */
            if (!client->executing_input && client->connected) {
                
                /* Adaptive throttling based on queue depth */
                int queue_depth = msg_queue_count(&client->msg_queue);
                double adaptive_throttle = move_throttle_ms;
                if (queue_depth > 100) {
                    adaptive_throttle = 100;  /* STOP sending when queue > 100 */
                } else if (queue_depth > 50) {
                    adaptive_throttle = 50;  /* Heavy throttle */
                } else if (queue_depth > 20) {
                    adaptive_throttle = 32;  /* Moderate throttle */
                }
                
                /* Calculate distance from last SENT position */
                int dx = root_x - last_sent_x;
                int dy = root_y - last_sent_y;
                int distance_sq = dx * dx + dy * dy;
                int min_dist_sq = min_move_distance * min_move_distance;
                
                /* Only send if moved enough pixels from last sent position */
                bool moved_enough = (distance_sq >= min_dist_sq) || 
                                   (last_sent_x < 0);  /* Always send first position */
                bool time_ok = (now - last_move_time >= adaptive_throttle);
                
                /* Also send immediately if at screen edge (critical for edge detection) */
                int screen_w = (client->monitor_count > 0) ? client->monitors[0].width : 1920;
                int screen_h = (client->monitor_count > 0) ? client->monitors[0].height : 1080;
                bool at_edge = (root_x <= 5 || root_x >= screen_w - 5 ||
                               root_y <= 5 || root_y >= screen_h - 5);
                
                /* Detect which edge and send edge crossing request if at edge and active */
                if (at_edge && client->is_active && strcmp(client->config.role, "main") == 0) {
                    const char *edge = NULL;
                    float position = 0.0f;
                    
                    if (root_x <= 5) {
                        edge = "left";
                        position = (float)root_y / screen_h;  /* 0.0 at top, 1.0 at bottom */
                    } else if (root_x >= screen_w - 5) {
                        edge = "right";
                        position = (float)root_y / screen_h;
                    } else if (root_y <= 5) {
                        edge = "top";
                        position = (float)root_x / screen_w;  /* 0.0 at left, 1.0 at right */
                    } else if (root_y >= screen_h - 5) {
                        edge = "bottom";
                        position = (float)root_x / screen_w;
                    }
                    
                    if (edge) {
                        /* Throttle edge crossing requests to max once per 200ms */
                        static double last_edge_request_time = 0;
                        if (now - last_edge_request_time >= 200) {
                            send_edge_crossing_request(client, edge, position, root_x, root_y);
                            last_edge_request_time = now;
                        }
                    }
                }
                
                if ((moved_enough && time_ok) || at_edge) {
                    if (client->skip_mouse_moves > 0) {
                        client->skip_mouse_moves--;
                    } else {
                        char json[128];
                        snprintf(json, sizeof(json), "{\"x\":%d,\"y\":%d}", root_x, root_y);
                        send_input_event(client, "mouse_move", json);
                        move_count++;
                        
                        /* Update last sent position */
                        last_sent_x = root_x;
                        last_sent_y = root_y;
                        
                        /* Debug: Log first 5 moves and then every 200 */
                        if (move_count <= 5 || move_count % 200 == 0) {
                            LOG_INFO("X11 mouse_move #%d: (%d,%d), queue=%d%s", 
                                     move_count, root_x, root_y, queue_depth,
                                     at_edge ? " [EDGE]" : "");
                        }
                        
                        /* CRITICAL: When main is INACTIVE and cursor is near edge,
                         * re-center to allow continued movement in all directions.
                         * This prevents cursor from getting "stuck" at screen edge. */
                        if (!client->is_active && at_edge) {
                            /* Throttle re-centering to max once per 100ms */
                            static double last_recenter_time = 0;
                            if (now - last_recenter_time >= 100) {
                                int center_x = screen_w / 2;
                                int center_y = screen_h / 2;
                                
                                /* Warp cursor to center */
                                XWarpPointer(display, None, root, 0, 0, 0, 0, center_x, center_y);
                                XFlush(display);
                                
                                /* Send cursor_reset to server so delta tracking resets */
                                char reset_json[128];
                                snprintf(reset_json, sizeof(reset_json), 
                                         "{\"x\":%d,\"y\":%d}", center_x, center_y);
                                send_input_event(client, "cursor_reset", reset_json);
                                
                                /* Skip next mouse move to avoid sending warp position */
                                client->skip_mouse_moves = 1;
                                
                                /* Update tracking */
                                last_sent_x = center_x;
                                last_sent_y = center_y;
                                last_recenter_time = now;
                                
                                LOG_DEBUG("Re-centered cursor (was at edge: %d,%d)", root_x, root_y);
                            }
                        }
                    }
                    
                    last_move_time = now;
                }
                
                /* Mouse button changes */
                unsigned int btn_changed = mask ^ prev_mask;
                if (btn_changed & (Button1Mask | Button2Mask | Button3Mask)) {
                    /* Left button */
                    if (btn_changed & Button1Mask) {
                        char json[256];
                        bool pressed = (mask & Button1Mask) != 0;
                        snprintf(json, sizeof(json),
                            "{\"x\":%d,\"y\":%d,\"button\":\"Button.left\",\"action\":\"%s\"}",
                            root_x, root_y, pressed ? "press" : "release");
                        send_input_event(client, "mouse_click", json);
                        LOG_INFO("🖱️ CLICK: left %s at (%d,%d), queue=%d", 
                                 pressed ? "press" : "release", root_x, root_y,
                                 msg_queue_count(&client->msg_queue));
                    }
                    /* Middle button */
                    if (btn_changed & Button2Mask) {
                        char json[256];
                        bool pressed = (mask & Button2Mask) != 0;
                        snprintf(json, sizeof(json),
                            "{\"x\":%d,\"y\":%d,\"button\":\"Button.middle\",\"action\":\"%s\"}",
                            root_x, root_y, pressed ? "press" : "release");
                        send_input_event(client, "mouse_click", json);
                        LOG_INFO("🖱️ CLICK: middle %s at (%d,%d)", pressed ? "press" : "release", root_x, root_y);
                    }
                    /* Right button */
                    if (btn_changed & Button3Mask) {
                        char json[256];
                        bool pressed = (mask & Button3Mask) != 0;
                        snprintf(json, sizeof(json),
                            "{\"x\":%d,\"y\":%d,\"button\":\"Button.right\",\"action\":\"%s\"}",
                            root_x, root_y, pressed ? "press" : "release");
                        send_input_event(client, "mouse_click", json);
                        LOG_INFO("🖱️ CLICK: right %s at (%d,%d)", pressed ? "press" : "release", root_x, root_y);
                    }
                    
                    /* Scroll wheel (Button4=up, Button5=down) */
                    /* Note: These are edge-triggered, so we need to detect the press only */
                    if ((btn_changed & Button4Mask) && (mask & Button4Mask)) {
                        char json[128];
                        snprintf(json, sizeof(json),
                            "{\"x\":%d,\"y\":%d,\"dx\":0,\"dy\":1}", root_x, root_y);
                        send_input_event(client, "mouse_scroll", json);
                        LOG_INFO("🖱️ SCROLL: up at (%d,%d)", root_x, root_y);
                    }
                    if ((btn_changed & Button5Mask) && (mask & Button5Mask)) {
                        char json[128];
                        snprintf(json, sizeof(json),
                            "{\"x\":%d,\"y\":%d,\"dx\":0,\"dy\":-1}", root_x, root_y);
                        send_input_event(client, "mouse_scroll", json);
                        LOG_INFO("🖱️ SCROLL: down at (%d,%d)", root_x, root_y);
                    }
                    
                    prev_mask = mask;
                }
            }
        } else {
            query_fail_count++;
            if (query_fail_count <= 3 || query_fail_count % 100 == 0) {
                LOG_WARN("XQueryPointer failed at poll %d (total failures: %d)", 
                         poll_count, query_fail_count);
            }
        }
        
        /* Check for keyboard events using XPending/XNextEvent */
        /* This works when X11 grab is active */
        while (XPending(display) > 0) {
            XEvent event;
            XNextEvent(display, &event);
            
            if (event.type == KeyPress || event.type == KeyRelease) {
                XKeyEvent *key_event = (XKeyEvent *)&event;
                KeySym keysym = XLookupKeysym(key_event, 0);
                
                /* Convert keysym to key name */
                const char *key_name = XKeysymToString(keysym);
                if (key_name) {
                    bool is_press = (event.type == KeyPress);
                    bool is_special = (keysym >= XK_BackSpace && keysym <= XK_Hyper_R) ||
                                     (keysym >= XK_F1 && keysym <= XK_F35);
                    
                    /* Format key name similar to pynput */
                    char formatted_key[64];
                    if (is_special) {
                        /* Special keys get Key. prefix */
                        snprintf(formatted_key, sizeof(formatted_key), "Key.%s", key_name);
                    } else if (strlen(key_name) == 1) {
                        /* Single character keys */
                        snprintf(formatted_key, sizeof(formatted_key), "%s", key_name);
                    } else {
                        snprintf(formatted_key, sizeof(formatted_key), "Key.%s", key_name);
                    }
                    
                    char json[256];
                    snprintf(json, sizeof(json),
                        "{\"action\":\"%s\",\"key\":\"%s\",\"is_special\":%s}",
                        is_press ? "press" : "release",
                        formatted_key,
                        is_special ? "true" : "false");
                    send_input_event(client, "keyboard", json);
                    
                    static int key_count = 0;
                    key_count++;
                    if (key_count <= 5 || key_count % 50 == 0) {
                        LOG_INFO("⌨️ KEY: %s %s [#%d]", 
                                 is_press ? "press" : "release", formatted_key, key_count);
                    }
                }
            }
        }
        
        /* Sleep 4ms for ~250Hz polling - we filter what we actually send */
        usleep(4000);
    }
    
    /* Stop XRecord thread */
    if (keyboard_capture && xrecord_thread) {
        g_xrecord_running = false;
        /* Disable the context to unblock the thread */
        if (g_record_ctx && display) {
            XRecordDisableContext(display, g_record_ctx);
            XFlush(display);
        }
        pthread_join(xrecord_thread, NULL);
        xrecord_cleanup(display, NULL);
    }
    
    /* Log final stats */
    LOG_INFO("X11 input capture thread stopped (polls: %d, moves: %d, query_fails: %d)", 
             poll_count, move_count, query_fail_count);
    
    XCloseDisplay(display);
    
    return NULL;
}

#endif /* PLATFORM_LINUX */

/* ============================================================================
 * Client Initialization and Main Loop
 * ============================================================================ */

static void send_registration(ClientState *client) {
    /* Build monitors array JSON */
    StringBuilder sb;
    sb_init(&sb);
    sb_append(&sb, "[");
    
    for (int i = 0; i < client->monitor_count; i++) {
        if (i > 0) sb_append(&sb, ",");
        char mon_json[256];
        snprintf(mon_json, sizeof(mon_json),
            "{\"monitor_id\":\"%s\",\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d}",
            client->monitors[i].monitor_id,
            client->monitors[i].x, client->monitors[i].y,
            client->monitors[i].width, client->monitors[i].height);
        sb_append(&sb, mon_json);
    }
    sb_append(&sb, "]");
    
    bool is_main = (strcmp(client->config.role, "main") == 0);
    
    char *msg = json_create_message("register_device",
        "computer_id", client->config.computer_id,
        "is_main", is_main ? "true" : "false",
        "monitors", sb.buf,
        NULL);
    
    queue_ws_message(client, msg);
    
    LOG_INFO("Sent registration: %s as %s", 
             client->config.computer_id,
             is_main ? "main (has keyboard/mouse)" : "player (receives input)");
    
    free(sb.buf);
    free(msg);
}

static int client_init(ClientState *client) {
    memset(client, 0, sizeof(ClientState));
    
    /* Initialize message queue */
    msg_queue_init(&client->msg_queue);
    
    /* Default config */
    client->config.log_level = LOG_INFO;
    client->config.port = 8765;
    client->config.use_ssl = false;
    strcpy(client->config.role, "player");

#ifdef PLATFORM_WINDOWS
    client->keyboard_hook = NULL;
    client->mouse_hook = NULL;
#endif
    
    /* Get hostname for computer_id */
#ifdef PLATFORM_WINDOWS
    {
        DWORD size = sizeof(client->config.computer_id);
        if (!GetComputerNameA(client->config.computer_id, &size)) {
            strcpy(client->config.computer_id, "unknown");
        }
    }
#else
    if (gethostname(client->config.computer_id, sizeof(client->config.computer_id)) != 0) {
        strcpy(client->config.computer_id, "unknown");
    }
#endif
    
    /* Detect monitors */
    client->monitor_count = detect_monitors(client->monitors, MAX_MONITORS);
    
#ifdef PLATFORM_LINUX
    /* Initialize evdev */
    for (int i = 0; i < MAX_DEVICES; i++) {
        client->evdev_fds[i] = -1;
    }
    
    /* Open X display for injection */
    client->x_display = XOpenDisplay(NULL);
    if (!client->x_display) {
        LOG_ERROR("Cannot open X display");
        return -1;
    }
    
    /* Check for XTest extension */
    int event_base, error_base, major, minor;
    if (!XTestQueryExtension(client->x_display, &event_base, &error_base, 
                              &major, &minor)) {
        LOG_ERROR("XTest extension not available");
        XCloseDisplay(client->x_display);
        return -1;
    }
    LOG_INFO("XTest extension version %d.%d", major, minor);
#endif
    
    return 0;
}

static void client_cleanup(ClientState *client) {
#ifdef PLATFORM_LINUX
    /* Stop input thread */
    if (client->input_thread_running) {
        client->input_thread_running = false;
        pthread_join(client->input_thread, NULL);
    }
    
    /* Stop clipboard monitor thread */
    if (client->clipboard_thread_running) {
        client->clipboard_thread_running = false;
        pthread_join(client->clipboard_thread, NULL);
    }
    
    /* Restore cursor visibility */
    if (client->x_display) {
        show_cursor_linux(client->x_display);
    }
    
    /* Cleanup XRecord keyboard capture */
    if (client->x_display) {
        xrecord_cleanup(client->x_display, NULL);
    }
    
    /* Ungrab X11 input (keyboard + pointer) */
    if (client->x_display) {
        x11_ungrab_input(client->x_display);
    }
    
    /* Ungrab and close evdev devices */
    if (client->input_grabbed) {
        ungrab_evdev_devices(client->evdev_fds, client->evdev_count);
    }
    close_evdev_devices(client->evdev_fds, client->evdev_count);
    
    /* Close X display */
    if (client->x_display) {
        XCloseDisplay(client->x_display);
        client->x_display = NULL;
    }
#endif
#ifdef PLATFORM_WINDOWS
    /* Stop Windows hook thread (removes hooks internally) */
    stop_windows_hook_thread(client);
    
    /* Show cursor */
    while (ShowCursor(TRUE) < 0);
#endif
    
    /* Cleanup message queue */
    msg_queue_destroy(&client->msg_queue);
    
    /* Destroy WebSocket context */
    if (client->ws_context) {
        lws_context_destroy(client->ws_context);
        client->ws_context = NULL;
    }
}

static int connect_websocket(ClientState *client) {
    /* Parse server URL */
    char protocol[16] = "ws";
    char host[256] = "localhost";
    int port = client->config.port;
    char path[256] = "/";
    
    /* Simple URL parsing */
    const char *url = client->config.server_url;
    if (strncmp(url, "wss://", 6) == 0) {
        strcpy(protocol, "wss");
        url += 6;
        client->config.use_ssl = true;
    } else if (strncmp(url, "ws://", 5) == 0) {
        url += 5;
    }
    
    /* Parse host:port/path */
    const char *colon = strchr(url, ':');
    const char *slash = strchr(url, '/');
    
    if (colon && (!slash || colon < slash)) {
        size_t host_len = colon - url;
        if (host_len < sizeof(host)) {
            strncpy(host, url, host_len);
            host[host_len] = '\0';
        }
        port = atoi(colon + 1);
    } else if (slash) {
        size_t host_len = slash - url;
        if (host_len < sizeof(host)) {
            strncpy(host, url, host_len);
            host[host_len] = '\0';
        } else {
            /* Host too long, truncate */
            memcpy(host, url, sizeof(host) - 1);
            host[sizeof(host) - 1] = '\0';
        }
    } else {
        /* Copy entire URL as host (up to buffer size) */
        size_t url_len = strlen(url);
        size_t copy_len = (url_len < sizeof(host) - 1) ? url_len : (sizeof(host) - 1);
        memcpy(host, url, copy_len);
        host[copy_len] = '\0';
    }
    
    if (slash) {
        size_t path_len = strlen(slash);
        size_t copy_len = (path_len < sizeof(path) - 1) ? path_len : (sizeof(path) - 1);
        memcpy(path, slash, copy_len);
        path[copy_len] = '\0';
    } else {
        path[0] = '/';
        path[1] = '\0';
    }
    
    LOG_INFO("Connecting to %s://%s:%d%s", protocol, host, port, path);
    
    /* Create WebSocket context */
    struct lws_context_creation_info ctx_info;
    memset(&ctx_info, 0, sizeof(ctx_info));
    ctx_info.port = CONTEXT_PORT_NO_LISTEN;
    ctx_info.protocols = protocols;
    ctx_info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    
    /* Enable TCP keepalive for faster disconnection detection */
    ctx_info.ka_time = 5;       /* Start keepalive after 5 seconds of idle */
    ctx_info.ka_probes = 3;     /* Send 3 probes before declaring dead */
    ctx_info.ka_interval = 1;   /* 1 second between probes */
    
    client->ws_context = lws_create_context(&ctx_info);
    if (!client->ws_context) {
        LOG_ERROR("Failed to create WebSocket context");
        return -1;
    }
    
    /* Connect */
    struct lws_client_connect_info conn_info;
    memset(&conn_info, 0, sizeof(conn_info));
    conn_info.context = client->ws_context;
    conn_info.address = host;
    conn_info.port = port;
    conn_info.path = path;
    conn_info.host = host;
    conn_info.origin = host;
    conn_info.protocol = protocols[0].name;
    
    if (client->config.use_ssl) {
        conn_info.ssl_connection = LCCSCF_USE_SSL | 
                                    LCCSCF_ALLOW_SELFSIGNED |
                                    LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
    }
    
    client->ws_connection = lws_client_connect_via_info(&conn_info);
    if (!client->ws_connection) {
        LOG_ERROR("Failed to initiate WebSocket connection to %s://%s:%d%s", 
                  protocol, host, port, path);
        LOG_ERROR("Possible causes:");
        LOG_ERROR("  - Server is not running");
        LOG_ERROR("  - Incorrect server URL or port");
        LOG_ERROR("  - Firewall blocking connection");
        LOG_ERROR("  - Network unreachable");
        if (client->config.use_ssl) {
            LOG_ERROR("  - SSL/TLS handshake failure");
        }
        return -1;
    }
    
    return 0;
}

/* ============================================================================
 * Base64 Encoding (for connection code)
 * ============================================================================ */

static const char base64_chars[] = 
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char* base64_encode(const char *input) {
    size_t input_len = strlen(input);
    size_t output_len = 4 * ((input_len + 2) / 3);
    char *output = malloc(output_len + 1);
    if (!output) return NULL;
    
    size_t i, j;
    for (i = 0, j = 0; i < input_len;) {
        uint32_t octet_a = i < input_len ? (unsigned char)input[i++] : 0;
        uint32_t octet_b = i < input_len ? (unsigned char)input[i++] : 0;
        uint32_t octet_c = i < input_len ? (unsigned char)input[i++] : 0;
        
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;
        
        output[j++] = base64_chars[(triple >> 18) & 0x3F];
        output[j++] = base64_chars[(triple >> 12) & 0x3F];
        output[j++] = base64_chars[(triple >> 6) & 0x3F];
        output[j++] = base64_chars[triple & 0x3F];
    }
    
    /* Add padding */
    int mod = input_len % 3;
    if (mod > 0) {
        output[output_len - 1] = '=';
        if (mod == 1) {
            output[output_len - 2] = '=';
        }
    }
    
    output[output_len] = '\0';
    return output;
}

static char* generate_connection_code(const char *ip, int port) {
    char data[128];
    snprintf(data, sizeof(data), "%s:%d", ip, port);
    return base64_encode(data);
}

/* ============================================================================
 * Local Server Support (start Python server as subprocess)
 * ============================================================================ */

#ifdef PLATFORM_LINUX
static char* get_local_ip(void) {
    static char ip[64] = "127.0.0.1";
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return ip;
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("8.8.8.8");
    addr.sin_port = htons(80);
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        struct sockaddr_in local;
        socklen_t len = sizeof(local);
        if (getsockname(sock, (struct sockaddr*)&local, &len) == 0) {
            strncpy(ip, inet_ntoa(local.sin_addr), sizeof(ip) - 1);
        }
    }
    
    close(sock);
    return ip;
}
#endif

#ifdef PLATFORM_WINDOWS
static char* get_local_ip(void) {
    static char ip[64] = "127.0.0.1";
    WSADATA wsaData;
    SOCKET sock = INVALID_SOCKET;
    
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return ip;
    }
    
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) {
        WSACleanup();
        return ip;
    }
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("8.8.8.8");
    addr.sin_port = htons(80);
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        struct sockaddr_in local;
        int len = sizeof(local);
        if (getsockname(sock, (struct sockaddr*)&local, &len) == 0) {
            strncpy(ip, inet_ntoa(local.sin_addr), sizeof(ip) - 1);
        }
    }
    
    closesocket(sock);
    WSACleanup();
    return ip;
}
#endif

static void signal_handler(int sig) {
    LOG_INFO("Signal %d received, shutting down...", sig);
    g_shutdown = 1;
    g_client.running = false;
    g_server.running = false;
}

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("\nOptions:\n");
    printf("  --server URL       WebSocket server URL (for connecting to external server)\n");
    printf("  --computer-id ID   Unique computer identifier (default: hostname)\n");
    printf("  --role ROLE        Role: 'main' or 'player' (default: player)\n");
    printf("  --port PORT        Server port (default: 8765)\n");
    printf("  --ssl              Use SSL/TLS (wss://)\n");
    printf("  --debug            Enable debug logging\n");
    printf("  --help             Show this help\n");
    printf("\nModes:\n");
    printf("  Server mode:  %s --role main --port 8765\n", prog);
    printf("                Starts embedded server, other computers connect to this one.\n");
    printf("  Client mode:  %s --role player --server ws://192.168.1.100:8765\n", prog);
    printf("                Connects to an existing server.\n");
    printf("\nExamples:\n");
    printf("  %s --role main --port 8765           # Start as server on port 8765\n", prog);
    printf("  %s --role player --server ws://192.168.1.100:8765  # Connect to server\n", prog);
}

int main(int argc, char *argv[]) {
    /* Load .env file first (before anything else) */
    load_env_file();

    /* Get computer name early (for logging) */
    char computer_name[MAX_HOSTNAME];
    const char *env_id = getenv("COMPUTER_ID");
    if (env_id && env_id[0]) {
        strncpy(computer_name, env_id, sizeof(computer_name) - 1);
        computer_name[sizeof(computer_name) - 1] = '\0';
    } else {
#ifdef PLATFORM_WINDOWS
        /* Use GetComputerNameA on Windows (doesn't require Winsock) */
        DWORD size = sizeof(computer_name);
        if (!GetComputerNameA(computer_name, &size)) {
            strcpy(computer_name, "unknown");
        }
#else
        if (gethostname(computer_name, sizeof(computer_name)) != 0) {
            strcpy(computer_name, "unknown");
        }
#endif
    }
    
    /* Parse computer-id and role from command line early (for logging) */
    char role[16] = "player";  /* Default role */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--computer-id") == 0 && i + 1 < argc) {
            strncpy(computer_name, argv[i + 1], sizeof(computer_name) - 1);
            computer_name[sizeof(computer_name) - 1] = '\0';
        } else if (strcmp(argv[i], "--role") == 0 && i + 1 < argc) {
            strncpy(role, argv[i + 1], sizeof(role) - 1);
            role[sizeof(role) - 1] = '\0';
        }
    }

    /* Initialize logging first (before any LOG_* calls) */
    if (init_logging(computer_name, role) != 0) {
        fprintf(stderr, "Warning: Logging initialization failed, continuing with console only\n");
    }
    
    /* Initialize client */
    if (client_init(&g_client) != 0) {
        LOG_ERROR("Failed to initialize client");
        cleanup_logging();
        return 1;
    }
    
    /* Store computer name in config */
    strncpy(g_client.config.computer_id, computer_name, sizeof(g_client.config.computer_id) - 1);
    g_client.config.computer_id[sizeof(g_client.config.computer_id) - 1] = '\0';  /* Ensure null termination */
    
    /* Parse command line arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--server") == 0 && i + 1 < argc) {
            strncpy(g_client.config.server_url, argv[++i], 
                    sizeof(g_client.config.server_url) - 1);
        } else if (strcmp(argv[i], "--computer-id") == 0 && i + 1 < argc) {
            strncpy(g_client.config.computer_id, argv[++i],
                    sizeof(g_client.config.computer_id) - 1);
            g_client.config.computer_id[sizeof(g_client.config.computer_id) - 1] = '\0';
            strncpy(computer_name, g_client.config.computer_id, sizeof(computer_name) - 1);
            computer_name[sizeof(computer_name) - 1] = '\0';
        } else if (strcmp(argv[i], "--role") == 0 && i + 1 < argc) {
            strncpy(g_client.config.role, argv[++i],
                    sizeof(g_client.config.role) - 1);
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            g_client.config.port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--ssl") == 0) {
            g_client.config.use_ssl = true;
        } else if (strcmp(argv[i], "--debug") == 0) {
            g_client.config.log_level = LOG_DEBUG;
        }
    }

    /* Handle --role main without --server: Run embedded server mode */
    if (strcmp(g_client.config.role, "main") == 0 && g_client.config.server_url[0] == '\0') {
        /* --role main without --server: Run embedded server mode */
        const char *local_ip = get_local_ip();
        int port = g_client.config.port;
        char *connection_code = generate_connection_code(local_ip, port);

        printf("\n");
        printf("╔═══════════════════════════════════════════════════════════════╗\n");
        printf("║         EMBEDDED SERVER MODE (No External Server)             ║\n");
        printf("╚═══════════════════════════════════════════════════════════════╝\n");
        printf("\n");
        printf("  Server IP:   %s\n", local_ip);
        printf("  Server Port: %d\n", port);
        printf("\n");
        printf("  ┌─────────────────────────────────────────────────────────────┐\n");
        printf("  │  CONNECTION CODE: %-40s │\n", connection_code ? connection_code : "ERROR");
        printf("  └─────────────────────────────────────────────────────────────┘\n");
        printf("\n");
        printf("  On other computers, run:\n");
#ifdef PLATFORM_WINDOWS
        printf("    .\\input_unified.exe --role player --server ws://%s:%d\n", local_ip, port);
#else
        printf("    ./input_unified --role player --server ws://%s:%d\n", local_ip, port);
#endif
        printf("\n");
        fflush(stdout);

        if (connection_code) free(connection_code);

        /* Start embedded WebSocket server */
        if (start_embedded_server(port) != 0) {
            LOG_ERROR("Failed to start embedded server");
            cleanup_logging();
            return 1;
        }

        /* Setup signal handlers */
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);
#ifndef PLATFORM_WINDOWS
        signal(SIGPIPE, SIG_IGN);
#endif

        LOG_INFO("=== Unified Input Server v%s ===", VERSION);
        LOG_INFO("Platform: %s", PLATFORM_NAME);
        LOG_INFO("Computer ID: %s", g_client.config.computer_id);
        LOG_INFO("Role: main (embedded server)");
        LOG_INFO("Listening on port: %d", port);
        LOG_INFO("Monitors: %d", g_client.monitor_count);

        /* Register local machine as first client */
        ServerClient *local_client = &g_server.clients[0];
        strncpy(local_client->computer_id, g_client.config.computer_id, MAX_HOSTNAME - 1);
        strcpy(local_client->role, "main");
        local_client->monitor_count = g_client.monitor_count;
        for (int i = 0; i < g_client.monitor_count && i < MAX_MONITORS; i++) {
            memcpy(&local_client->monitors[i], &g_client.monitors[i], sizeof(Monitor));
        }
        local_client->registered = true;
        local_client->wsi = NULL;  /* Local client has no WSI */
        g_server.client_count = 1;

        /* Set this machine as active */
        strncpy(g_server.active_computer_id, g_client.config.computer_id, MAX_HOSTNAME - 1);
        strcpy(g_server.active_monitor_id, g_client.monitor_count > 0 ? g_client.monitors[0].monitor_id : "m0");
        g_server.cursor_x = g_client.monitor_count > 0 ? g_client.monitors[0].width / 2.0 : 960;
        g_server.cursor_y = g_client.monitor_count > 0 ? g_client.monitors[0].height / 2.0 : 540;

        g_client.is_active = true;  /* Main is active by default */
        g_client.running = true;

#ifdef PLATFORM_WINDOWS
        /* Start Windows input hooks on a dedicated thread */
        if (!start_windows_hook_thread(&g_client)) {
            LOG_WARN("Windows input hooks not available - input capture disabled");
        }
#endif

        LOG_INFO("Server running. Waiting for player connections...");
        LOG_INFO("Press Ctrl+C to stop.");

        /* Server main loop */
        while (g_server.running && !g_shutdown) {
            /* Service WebSocket server */
            int n = lws_service(g_server.ws_context, 10);
            if (n < 0) {
                LOG_ERROR("WebSocket service error");
                break;
            }

            /* Process broadcast queue (drain messages queued by hooks) */
            char queued_msg[MSG_MAX_LEN];
            int broadcast_sent = 0;
            static int total_broadcast = 0;
            while (msg_queue_pop(&g_server.broadcast_queue, queued_msg, sizeof(queued_msg))) {
                total_broadcast++;
                if (total_broadcast <= 5) {
                    LOG_INFO("QUEUE DEBUG #%d: sending msg to clients, client_count=%d",
                             total_broadcast, g_server.client_count);
                }
                /* Send to all registered clients */
                for (int i = 0; i < MAX_SERVER_CLIENTS; i++) {
                    if (g_server.clients[i].registered && g_server.clients[i].wsi) {
                        size_t len = strlen(queued_msg);
                        unsigned char *buf = malloc(LWS_PRE + len + 1);
                        if (buf) {
                            memcpy(buf + LWS_PRE, queued_msg, len);
                            lws_write(g_server.clients[i].wsi, buf + LWS_PRE, len, LWS_WRITE_TEXT);
                            free(buf);
                        }
                    }
                }
                broadcast_sent++;
                if (broadcast_sent >= 128) break;  /* Limit per iteration */
            }
        }

        /* Cleanup server */
        LOG_INFO("Shutting down server...");
#ifdef PLATFORM_WINDOWS
        stop_windows_hook_thread(&g_client);
#endif
        if (g_server.ws_context) {
            lws_context_destroy(g_server.ws_context);
            g_server.ws_context = NULL;
        }
        msg_queue_destroy(&g_server.broadcast_queue);

        client_cleanup(&g_client);
        cleanup_logging();
        return 0;

    } else {
        /* Build server URL if not specified */
        if (g_client.config.server_url[0] == '\0') {
            snprintf(g_client.config.server_url, sizeof(g_client.config.server_url),
                     "%s://localhost:%d",
                     g_client.config.use_ssl ? "wss" : "ws",
                     g_client.config.port);
        }
    }

    /* Setup signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#ifndef PLATFORM_WINDOWS
    signal(SIGPIPE, SIG_IGN);
#endif
    
    LOG_INFO("=== Unified Input Client v%s ===", VERSION);
    LOG_INFO("Platform: %s", PLATFORM_NAME);
    LOG_INFO("Computer ID: %s", g_client.config.computer_id);
    LOG_INFO("Role: %s", g_client.config.role);
    LOG_INFO("Server: %s", g_client.config.server_url);
    LOG_INFO("Monitors: %d", g_client.monitor_count);
    
#ifdef PLATFORM_LINUX
    /* Open evdev devices for input capture (main role) */
    if (strcmp(g_client.config.role, "main") == 0) {
        g_client.evdev_count = open_evdev_devices(g_client.evdev_fds, MAX_DEVICES);
        if (g_client.evdev_count == 0) {
            LOG_INFO("No evdev devices (run as root or add to 'input' group for better performance)");
            LOG_INFO("Using X11 input capture fallback mode");
        } else {
            LOG_INFO("Opened %d evdev input devices", g_client.evdev_count);
        }
    }
#endif
    
    g_client.running = true;
    
    /* Main reconnection loop */
    while (g_client.running && !g_shutdown) {
        /* Connect to WebSocket server */
        if (connect_websocket(&g_client) != 0) {
            g_client.reconnect_attempts++;
            if (g_client.reconnect_attempts >= MAX_RECONNECT_ATTEMPTS) {
                LOG_ERROR("Max reconnection attempts reached. Giving up.");
                break;
            }
            LOG_INFO("Reconnecting in %d seconds... (attempt %d/%d)",
                     RECONNECT_DELAY_SEC, g_client.reconnect_attempts,
                     MAX_RECONNECT_ATTEMPTS);
            sleep_ms(RECONNECT_DELAY_SEC * 1000);
            continue;
        }
        
        /* Wait for connection to establish */
        int connect_timeout = 100;  /* 10 seconds */
        int timeout_seconds = connect_timeout / 10;
        LOG_INFO("Waiting up to %d seconds for WebSocket connection...", timeout_seconds);
        
        int waited = 0;
        while (!g_client.connected && connect_timeout > 0 && g_client.running) {
            lws_service(g_client.ws_context, 100);
            connect_timeout--;
            waited++;
            
            /* Log progress every 2 seconds */
            if (waited % 20 == 0 && connect_timeout > 0) {
                LOG_INFO("Still waiting for connection... (%d seconds remaining)", connect_timeout / 10);
            }
        }
        
        if (!g_client.connected) {
            LOG_ERROR("Connection timeout after %d seconds - failed to establish WebSocket connection", timeout_seconds);
            LOG_ERROR("Server may be unreachable or not responding");
            if (g_client.ws_context) {
                lws_context_destroy(g_client.ws_context);
                g_client.ws_context = NULL;
            }
            g_client.reconnect_attempts++;
            if (g_client.reconnect_attempts >= MAX_RECONNECT_ATTEMPTS) {
                LOG_ERROR("Max reconnection attempts reached. Giving up.");
                break;
            }
            LOG_INFO("Reconnecting in %d seconds... (attempt %d/%d)",
                     RECONNECT_DELAY_SEC, g_client.reconnect_attempts,
                     MAX_RECONNECT_ATTEMPTS);
            sleep_ms(RECONNECT_DELAY_SEC * 1000);
            continue;
        }
        
        /* Send registration */
        send_registration(&g_client);
        
#ifdef PLATFORM_LINUX
        /* Start input capture thread */
        if (strcmp(g_client.config.role, "main") == 0) {
            g_client.input_thread_running = true;
            
            if (g_client.evdev_count > 0) {
                /* Use evdev for input capture (requires permissions) */
                LOG_INFO("Starting evdev input capture (%d devices)", g_client.evdev_count);
                if (pthread_create(&g_client.input_thread, NULL, 
                                   input_capture_thread, &g_client) != 0) {
                    LOG_ERROR("Failed to create evdev input capture thread");
                    g_client.input_thread_running = false;
                }
            } else {
                /* Fallback to X11 input capture (works without root) */
                LOG_INFO("Starting X11 input capture (fallback mode)");
                if (pthread_create(&g_client.input_thread, NULL, 
                                   x11_input_capture_thread, &g_client) != 0) {
                    LOG_ERROR("Failed to create X11 input capture thread");
                    g_client.input_thread_running = false;
                }
            }
        }
        
        /* Start clipboard monitor thread for all roles */
        g_client.clipboard_thread_running = true;
        if (pthread_create(&g_client.clipboard_thread, NULL, 
                           clipboard_monitor_thread, &g_client) != 0) {
            LOG_ERROR("Failed to create clipboard monitor thread");
            g_client.clipboard_thread_running = false;
        }
#endif
#ifdef PLATFORM_WINDOWS
        /* Setup Windows input hooks for main role */
        if (strcmp(g_client.config.role, "main") == 0) {
            if (!setup_windows_hooks(&g_client)) {
                LOG_WARN("Windows input hooks not available - input capture disabled");
            }
        }
        
        /* Start clipboard monitor thread for all roles */
        g_client.clipboard_thread_running = true;
        if (pthread_create(&g_client.clipboard_thread, NULL,
                           clipboard_monitor_thread, &g_client) != 0) {
            LOG_ERROR("Failed to create clipboard monitor thread");
            g_client.clipboard_thread_running = false;
        }
#endif
        
        /* Main event loop */
        while (g_client.connected && g_client.running && !g_shutdown) {
#ifdef PLATFORM_WINDOWS
            /* Process Windows messages (required for low-level hooks to work) */
            MSG msg;
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
#endif
            /* Service WebSocket more frequently (1ms timeout for better throughput) */
            int n = lws_service(g_client.ws_context, 1);
            if (n < 0) {
                LOG_ERROR("WebSocket service error. Server connection lost.");
                LOG_INFO("Exiting gracefully...");
                g_client.connected = false;
                g_client.running = false;
                g_shutdown = 1;
                break;
            }
            
            /* Periodic heartbeat log (every 60 seconds) */
            static double last_heartbeat = 0;
            double now = get_time_ms();
            if (last_heartbeat == 0) last_heartbeat = now;
            if (now - last_heartbeat > 60000) {  /* Every 60 seconds */
                LOG_INFO("💓 Heartbeat: running, connected=%d, queue=%d", 
                         g_client.connected, msg_queue_count(&g_client.msg_queue));
                last_heartbeat = now;
            }
            
            /* Timeout check disabled - connection will stay alive indefinitely */
            /* The WebSocket library and TCP keepalive will handle actual connection failures */
            /* Uncomment below to re-enable timeout checking if needed */
            /*
            double server_silent_ms = now - g_client.last_server_message_time;
            if (server_silent_ms > 300000) {  
                LOG_WARN("No server message for %.1f seconds - connection may be dead", 
                         server_silent_ms / 1000.0);
                LOG_INFO("Server connection timeout. Exiting gracefully...");
#ifdef PLATFORM_LINUX
                if (g_client.x_display) {
                    show_cursor_linux(g_client.x_display);
                    x11_ungrab_input(g_client.x_display);
                }
#endif
                g_client.connected = false;
                g_client.running = false;
                g_shutdown = 1;
                break;
            }
            */
            
            /* Proactively request write callback if queue has messages */
            if (msg_queue_has_messages(&g_client.msg_queue) && g_client.ws_connection) {
                lws_callback_on_writable(g_client.ws_connection);
            }
        }
        
#ifdef PLATFORM_LINUX
        /* Stop input thread */
        if (g_client.input_thread_running) {
            g_client.input_thread_running = false;
            pthread_join(g_client.input_thread, NULL);
        }
        
        /* Stop clipboard monitor thread */
        if (g_client.clipboard_thread_running) {
            g_client.clipboard_thread_running = false;
            pthread_join(g_client.clipboard_thread, NULL);
        }
#endif
#ifdef PLATFORM_WINDOWS
        /* Stop clipboard monitor thread */
        if (g_client.clipboard_thread_running) {
            g_client.clipboard_thread_running = false;
            pthread_join(g_client.clipboard_thread, NULL);
        }
#endif
        
        /* Cleanup WebSocket */
        if (g_client.ws_context) {
            lws_context_destroy(g_client.ws_context);
            g_client.ws_context = NULL;
        }
        g_client.ws_connection = NULL;
        g_client.connected = false;
        
        /* Exit gracefully if connection lost (server died) */
        if (!g_shutdown) {
            LOG_INFO("Server connection lost. Exiting gracefully...");
            g_shutdown = 1;
            g_client.running = false;
            break;  /* Exit the outer connection loop */
        }
    }
    
    /* Cleanup */
    client_cleanup(&g_client);

    LOG_INFO("Client stopped");
    cleanup_logging();
    return 0;
}


