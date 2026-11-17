#include "preferences.h"
#include "config.h"

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

int preferences_model_load(int gateway, char *out, size_t cap)
{
    char path[LAMBDA_PATH_MAX], data[LAMBDA_MODEL_ID_MAX + 1];
    if (model_path(gateway, path, sizeof path) != 0)
        return errno == ENOENT ? 0 : -1;
    FILE *f = fopen(path, "r");
    if (!f) return errno == ENOENT ? 0 : -1;
    size_t len = fread(data, 1, sizeof data, f);
    int error = ferror(f) ? EIO : 0;
    fclose(f);
    if (error) { errno = error; return -1; }
    if (len && data[len - 1] == '\n') len--;
    if (len >= cap || !valid_model(data, len)) { errno = EINVAL; return -1; }
    memcpy(out, data, len);
    out[len] = '\0';
    return 1;
}

int preferences_model_save(int gateway, const char *model)
{
    char path[LAMBDA_PATH_MAX], temp[LAMBDA_PATH_MAX + 16];
    size_t len = strlen(model);
    if (!valid_model(model, len)) { errno = EINVAL; return -1; }
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
    char data[LAMBDA_MODEL_ID_MAX + 1];
    memcpy(data, model, len);
    data[len++] = '\n';
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
