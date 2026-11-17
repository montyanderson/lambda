#include "preferences.h"
#include "config.h"
#include "reasoning.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int model_path(int gateway, char *out, size_t cap)
{
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    int n;
    const char *name = gateway ? "default-model" : "default-model-anthropic";
    if (xdg && *xdg)
        n = snprintf(out, cap, "%s/lambda/%s", xdg, name);
    else if (home && *home)
        n = snprintf(out, cap, "%s/.config/lambda/%s", home, name);
    else { errno = ENOENT; return -1; }
    if (n < 0 || (size_t)n >= cap) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

static int valid_model(const char *model, size_t len)
{
    if (!len || len >= LAMBDA_MODEL_ID_MAX) return 0;
    for (size_t i = 0; i < len; i++)
        if ((unsigned char)model[i] < 32 || model[i] == 127) return 0;
    return 1;
}

int preferences_load(int gateway, model_preference *out)
{
    memset(out, 0, sizeof *out);
    char path[LAMBDA_PATH_MAX], data[LAMBDA_MODEL_ID_MAX + 64];
    if (model_path(gateway, path, sizeof path) != 0)
        return errno == ENOENT ? 0 : -1;
    FILE *f = fopen(path, "r");
    if (!f) return errno == ENOENT ? 0 : -1;
    size_t len = fread(data, 1, sizeof data - 1, f);
    int error = ferror(f) ? EIO : 0;
    fclose(f);
    if (error) { errno = error; return -1; }
    if (len == sizeof data - 1) { errno = EINVAL; return -1; }
    if (len && data[len - 1] == '\n') len--;
    data[len] = '\0';
    char *line = memchr(data, '\n', len);
    size_t model_len = line ? (size_t)(line - data) : len;
    if (!valid_model(data, model_len)) { errno = EINVAL; return -1; }
    if (line) {
        const char *value = line + 1;
        if (strncmp(value, "effort=", 7) || strlen(data) != len ||
            reasoning_index(value + 7) < 0) { errno = EINVAL; return -1; }
        snprintf(out->effort, sizeof out->effort, "%s", reasoning_name(reasoning_index(value + 7)));
        out->effort_set = 1;
    }
    memcpy(out->model, data, model_len);
    out->model[model_len] = '\0';
    return 1;
}

int preferences_save(int gateway, const char *model, const char *effort)
{
    char path[LAMBDA_PATH_MAX], temp[LAMBDA_PATH_MAX + 16];
    size_t len = strlen(model);
    int level = reasoning_index(effort);
    if (!valid_model(model, len) || level < 0) { errno = EINVAL; return -1; }
    if (model_path(gateway, path, sizeof path) != 0) return -1;
    /* Create missing config parents without touching an existing config file. */
    for (char *p = path + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        int rc = mkdir(path, 0700), error = errno;
        *p = '/';
        if (rc != 0 && error != EEXIST) { errno = error; return -1; }
    }
    snprintf(temp, sizeof temp, "%s.XXXXXX", path);
    int fd = mkstemp(temp);
    if (fd < 0) return -1;
    char data[LAMBDA_MODEL_ID_MAX + 64];
    len = (size_t)snprintf(data, sizeof data, "%s\neffort=%s\n", model,
                          level ? reasoning_name(level) : "default");
    int error = 0;
    for (size_t off = 0; off < len;) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { error = n < 0 ? errno : EIO; break; }
        off += (size_t)n;
    }
    if (!error && fsync(fd) != 0) error = errno;
    if (close(fd) != 0 && !error) error = errno;
    if (!error && rename(temp, path) != 0) error = errno;
    if (error) { unlink(temp); errno = error; return -1; }
    return 0;
}
