#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"

typedef struct { int x, y; } Point;
#include "config.h"

/* Headers recorded before position modes existed retain relative behavior. */
#ifndef POSITION_ABSOLUTE
#define POSITION_ABSOLUTE 0
#endif

static struct sockaddr_un endpoint;

static void fail(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    fputs("click-helper: ", stderr);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
    exit(EXIT_FAILURE);
}

static void *allocate(size_t size)
{
    void *memory = malloc(size);
    if (!memory)
        fail("out of memory");
    return memory;
}

static int64_t now_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        fail("clock_gettime: %s", strerror(errno));
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void pause_ms(int milliseconds)
{
    struct timespec interval = {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
    while (nanosleep(&interval, &interval) != 0)
        if (errno != EINTR)
            fail("nanosleep: %s", strerror(errno));
}

static void initialize_socket(void)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    const char *signature = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!runtime || !*runtime || !signature || !*signature)
        fail("run this executable inside Hyprland, without sudo");
    endpoint.sun_family = AF_UNIX;
    int length = snprintf(endpoint.sun_path, sizeof(endpoint.sun_path),
                          "%s/hypr/%s/.socket.sock", runtime, signature);
    if (length < 0 || (size_t)length >= sizeof(endpoint.sun_path))
        fail("Hyprland socket path is too long");
}

static char *request(const char *command)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        fail("socket: %s", strerror(errno));
    struct timeval timeout = {3, 0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0)
        fail("socket timeout: %s", strerror(errno));
    if (connect(fd, (struct sockaddr *)&endpoint, sizeof(endpoint)) != 0)
        fail("cannot connect to Hyprland: %s", strerror(errno));
    size_t length = strlen(command);
    for (size_t sent = 0; sent < length;) {
        ssize_t n = send(fd, command + sent, length - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            fail("sending to Hyprland: %s", strerror(errno));
        sent += (size_t)n;
    }
    size_t used = 0, capacity = 4096;
    const size_t maximum = 4 * 1024 * 1024;
    char *reply = allocate(capacity + 1);
    for (;;) {
        if (used == capacity) {
            if (capacity == maximum)
                fail("Hyprland response is too large");
            capacity *= 2;
            char *larger = realloc(reply, capacity + 1);
            if (!larger)
                fail("out of memory");
            reply = larger;
        }
        ssize_t n = recv(fd, reply + used, capacity - used, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            fail("reading Hyprland response: %s", strerror(errno));
        if (n == 0)
            break;
        used += (size_t)n;
    }
    close(fd);
    reply[used] = '\0';
    return reply;
}

static void dispatch(const char *format, ...)
{
    char command[512] = "/dispatch ";
    size_t prefix = strlen(command);
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(command + prefix, sizeof(command) - prefix, format, arguments);
    va_end(arguments);
    if (length < 0 || (size_t)length >= sizeof(command) - prefix)
        fail("dispatcher command is too long");
    char *reply = request(command);
    reply[strcspn(reply, "\r\n")] = '\0';
    if (strcmp(reply, "ok") != 0)
        fail("Hyprland rejected %s: %s", command + prefix, reply);
    free(reply);
}

static cJSON *query(const char *command)
{
    char *reply = request(command);
    cJSON *json = cJSON_Parse(reply);
    free(reply);
    if (!json)
        fail("invalid JSON from Hyprland");
    return json;
}

static const char *string_field(const cJSON *object, const char *name)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(value) ? value->valuestring : "";
}

static const char *window_class(const cJSON *window)
{
    const char *initial = string_field(window, "initialClass");
    return *initial ? initial : string_field(window, "class");
}

static bool usable(const cJSON *window)
{
    return !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(window, "mapped")) &&
           !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(window, "hidden"));
}

static cJSON *get_clients(void)
{
    cJSON *list = query("j/clients");
    if (!cJSON_IsArray(list))
        fail("Hyprland returned an invalid client list");
    return list;
}

static cJSON *find_address(cJSON *list, const char *address)
{
    cJSON *item;
    cJSON_ArrayForEach(item, list)
        if (strcmp(string_field(item, "address"), address) == 0)
            return item;
    return NULL;
}

static void copy_address(char output[64], const char *input)
{
    size_t length = strlen(input);
    if (length < 3 || length >= 64 || strncmp(input, "0x", 2) != 0 ||
        strspn(input + 2, "0123456789abcdefABCDEF") != length - 2)
        fail("invalid window address from Hyprland");
    memcpy(output, input, length + 1);
}

static cJSON *current_window(const char *address)
{
    cJSON *list = get_clients();
    cJSON *window = find_address(list, address);
    if (!window || !usable(window) || strcmp(window_class(window), APP_CLASS) != 0)
        fail("target window closed or changed identity");
    cJSON *copy = cJSON_Duplicate(window, true);
    cJSON_Delete(list);
    if (!copy)
        fail("out of memory");
    return copy;
}

static int coordinate(const cJSON *window, const char *field, int index)
{
    const cJSON *array = cJSON_GetObjectItemCaseSensitive(window, field);
    const cJSON *number = cJSON_GetArrayItem(array, index);
    if (!cJSON_IsNumber(number) || number->valuedouble < -1000000 ||
        number->valuedouble > 1000000 || number->valuedouble != number->valueint)
        fail("invalid window geometry from Hyprland");
    return number->valueint;
}

static bool correct_size(const cJSON *window)
{
    return coordinate(window, "size", 0) == WINDOW_WIDTH &&
           coordinate(window, "size", 1) == WINDOW_HEIGHT;
}

static bool correct_position(const cJSON *window)
{
    return !POSITION_ABSOLUTE ||
           (coordinate(window, "at", 0) == 0 && coordinate(window, "at", 1) == 0);
}

static pid_t launch(void)
{
    int error_pipe[2];
    if (pipe(error_pipe) != 0 || fcntl(error_pipe[1], F_SETFD, FD_CLOEXEC) < 0)
        fail("launch pipe: %s", strerror(errno));
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0)
        fail("fork: %s", strerror(errno));
    if (pid == 0) {
        close(error_pipe[0]);
        (void)setsid();
        execv(APP_PATH, APP_ARGV);
        int reason = errno;
        (void)write(error_pipe[1], &reason, sizeof(reason));
        _exit(127);
    }
    close(error_pipe[1]);
    int reason = 0;
    ssize_t n;
    do {
        n = read(error_pipe[0], &reason, sizeof(reason));
    } while (n < 0 && errno == EINTR);
    close(error_pipe[0]);
    if (n < 0)
        fail("reading launch status: %s", strerror(errno));
    if (n > 0)
        fail("cannot launch %s: %s", APP_PATH, strerror(reason));
    return pid;
}

static void wait_for_window(cJSON *before, pid_t pid, char target[64])
{
    int64_t start = now_ms(), first_seen = -1;
    char previous[64] = "";
    while (now_ms() - start < LAUNCH_TIMEOUT_MS) {
        cJSON *list = get_clients();
        cJSON *window, *candidate = NULL;
        int matches = 0;
        cJSON_ArrayForEach(window, list) {
            if (usable(window) && strcmp(window_class(window), APP_CLASS) == 0 &&
                !find_address(before, string_field(window, "address"))) {
                candidate = window;
                ++matches;
            }
        }
        if (matches == 0 && now_ms() - start >= STARTUP_DELAY_MS) {
            cJSON *active = query("j/activewindow");
            window = find_address(list, string_field(active, "address"));
            if (window && usable(window) && strcmp(window_class(window), APP_CLASS) == 0) {
                candidate = window;
                matches = 1;
            }
            cJSON_Delete(active);
        }
        if (matches > 1)
            fail("several new windows match the recorded app ID; use a unique app ID/new-window option when recording");
        if (candidate) {
            const char *address = string_field(candidate, "address");
            if (strcmp(previous, address) != 0) {
                copy_address(previous, address);
                first_seen = now_ms();
            }
            if (now_ms() - first_seen >= STARTUP_DELAY_MS) {
                copy_address(target, address);
                cJSON_Delete(list);
                return;
            }
        } else {
            previous[0] = '\0';
            first_seen = -1;
        }
        bool had_candidate = candidate != NULL;
        cJSON_Delete(list);
        int status;
        if (pid > 0 && waitpid(pid, &status, WNOHANG) == pid) {
            pid = -1;
            if (!had_candidate && (!WIFEXITED(status) || WEXITSTATUS(status) != 0))
                fail("program exited before opening a matching window");
        }
        pause_ms(50);
    }
    fail("timed out waiting for the recorded app ID; check the program, window class, and startup timeout");
}

static void prepare_window(const char *address)
{
    dispatch("focuswindow address:%s", address);
    dispatch("fullscreenstate 0 0");
    dispatch("setfloating address:%s", address);
    dispatch("resizewindowpixel exact %d %d,address:%s", WINDOW_WIDTH, WINDOW_HEIGHT, address);
    if (POSITION_ABSOLUTE)
        dispatch("movewindowpixel exact 0 0,address:%s", address);
    int64_t start = now_ms();
    while (now_ms() - start < LAUNCH_TIMEOUT_MS) {
        cJSON *window = current_window(address);
        bool ready = correct_size(window) && correct_position(window) &&
                     cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(window, "floating"));
        cJSON_Delete(window);
        if (ready) {
            pause_ms(300);
            window = current_window(address);
            ready = correct_size(window) && correct_position(window) &&
                    cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(window, "floating"));
            cJSON_Delete(window);
            if (ready)
                return;
        }
        pause_ms(50);
    }
    fail("app did not accept the recorded window size or position; check window rules and monitor layout");
}

int main(int argc, char **argv)
{
    (void)argv;
    if (argc != 1)
        fail("this executable takes no arguments");
    initialize_socket();
    char *probe = request("/dispatch sendshortcut ,mouse:271,");
    if (!strstr(probe, "invalid mouse button"))
        fail("this Hyprland version lacks the expected native mouse dispatcher: %s", probe);
    free(probe);
    cJSON *before = get_clients();
    pid_t pid = launch();
    char address[64];
    wait_for_window(before, pid, address);
    cJSON_Delete(before);
    prepare_window(address);
    for (size_t i = 0; i < sizeof(CLICKS) / sizeof(CLICKS[0]); ++i) {
        if (CLICKS[i].x < 0 || CLICKS[i].x >= WINDOW_WIDTH ||
            CLICKS[i].y < 0 || CLICKS[i].y >= WINDOW_HEIGHT)
            fail("embedded click lies outside the recorded window");
        cJSON *window = current_window(address);
        if (!correct_size(window))
            fail("window size changed during playback");
        if (!correct_position(window))
            fail("window moved away from desktop (0, 0); playback stopped");
        int x = coordinate(window, "at", 0), y = coordinate(window, "at", 1);
        cJSON_Delete(window);
        dispatch("focuswindow address:%s", address);
        dispatch("movecursor %d %d", x + CLICKS[i].x, y + CLICKS[i].y);
        pause_ms(100);
        cJSON *active = query("j/activewindow");
        if (strcmp(string_field(active, "address"), address) != 0)
            fail("target lost focus; playback stopped");
        cJSON_Delete(active);
        window = current_window(address);
        if (!correct_size(window) || coordinate(window, "at", 0) != x || coordinate(window, "at", 1) != y)
            fail("window moved or resized before the click; playback stopped");
        cJSON_Delete(window);
        /* No selector: preserve the actual pointer position and popup focus.
           Supplying address: here can reset the pointer focus to local (1,1). */
        dispatch("sendshortcut ,mouse:272,");
        pause_ms(CLICK_DELAY_MS);
    }
    return EXIT_SUCCESS;
}
