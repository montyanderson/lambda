#include "../src/preferences.c"
#include <assert.h>

int main(void)
{
    char dir[] = "/tmp/lambda-preferences-XXXXXX";
    assert(mkdtemp(dir));
    assert(setenv("XDG_CONFIG_HOME", dir, 1) == 0);
    model_preference preference;
    char path[LAMBDA_PATH_MAX];
    assert(preferences_load(1, &preference) == 0);
    assert(preferences_save(1, "provider/first", "high") == 0);
    assert(preferences_load(1, &preference) == 1);
    assert(!strcmp(preference.model, "provider/first"));
    assert(preference.effort_set && !strcmp(preference.effort, "high"));
    assert(preferences_load(0, &preference) == 0);
    assert(preferences_save(0, "direct-model", "high") == 0);
    assert(preferences_save(1, "provider/second", "high") == 0);
    assert(preferences_load(0, &preference) == 1);
    assert(!strcmp(preference.model, "direct-model"));
    assert(preferences_save(1, "invalid\nmodel", "high") == -1);
    assert(preferences_load(1, &preference) == 1);
    assert(!strcmp(preference.model, "provider/second"));
    assert(model_path(1, path, sizeof path) == 0);
    struct stat st;
    assert(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
    FILE *f = fopen(path, "w");
    assert(f);
    fputs("legacy-model\n", f);
    fclose(f);
    assert(preferences_load(1, &preference) == 1);
    assert(!preference.effort_set && !strcmp(preference.model, "legacy-model"));
    f = fopen(path, "w");
    assert(f);
    fputs("bad\nmultiple\n", f);
    fclose(f);
    assert(preferences_load(1, &preference) == -1 && errno == EINVAL);
    assert(unlink(path) == 0);
    /* A failed atomic replacement leaves the existing destination intact. */
    assert(mkdir(path, 0700) == 0);
    assert(preferences_save(1, "cannot-replace-directory", "high") == -1);
    assert(stat(path, &st) == 0 && S_ISDIR(st.st_mode));
    assert(rmdir(path) == 0);
    assert(model_path(0, path, sizeof path) == 0 && unlink(path) == 0);
    snprintf(path, sizeof path, "%s/lambda", dir);
    assert(rmdir(path) == 0); /* also proves failed saves cleaned up temp files */
    assert(unsetenv("XDG_CONFIG_HOME") == 0 && setenv("HOME", dir, 1) == 0);
    assert(preferences_save(0, "home-default", "high") == 0);
    assert(preferences_load(0, &preference) == 1);
    assert(!strcmp(preference.model, "home-default"));
    assert(model_path(0, path, sizeof path) == 0 && unlink(path) == 0);
    snprintf(path, sizeof path, "%s/.config/lambda", dir);
    assert(rmdir(path) == 0);
    snprintf(path, sizeof path, "%s/.config", dir);
    assert(rmdir(path) == 0 && rmdir(dir) == 0);
    puts("preferences: defaults, provider isolation, atomic replacement, errors, permissions and HOME fallback: ok");
    return 0;
}
