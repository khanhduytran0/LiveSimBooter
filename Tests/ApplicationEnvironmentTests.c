#include "../LiveProcess/ApplicationEnvironment.h"
#include <assert.h>
#include <fcntl.h>
#include <fts.h>
#include <unistd.h>

static void join(char output[PATH_MAX], const char *base, const char *suffix) {
    int length = snprintf(output, PATH_MAX, "%s/%s", base, suffix);
    assert(length > 0 && length < PATH_MAX);
}
static void directories(const char *path) {
    char copy[PATH_MAX]; strcpy(copy, path);
    for (char *cursor = copy + 1; *cursor; ++cursor) if (*cursor == '/') {
        *cursor = 0; assert(!mkdir(copy, 0700) || errno == EEXIST); *cursor = '/';
    }
    assert(!mkdir(copy, 0700) || errno == EEXIST);
}
static void file(const char *path) {
    int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(descriptor >= 0 && !close(descriptor));
}
static void prepare(const char *root, const char *image, const char *home, bool applies, LSBApplicationEnvironment *result) {
    char homeEntry[PATH_MAX + sizeof("HOME=")];
    assert(snprintf(homeEntry, sizeof(homeEntry), "HOME=%s", home) > 0);
    const char *environment[] = {homeEntry, "CFFIXED_USER_HOME=/old", "TMPDIR=/old/tmp", NULL};
    assert(!LSBPrepareApplicationEnvironment(root, image, environment, result) && result->applies == applies);
    if (!applies) assert(!result->fixedHome[0] && !result->temporaryDirectory[0]);
}
static void unchanged(const LSBApplicationEnvironment *prepared) {
    const char *environment[] = {"HOME=/shared", "CFFIXED_USER_HOME=/shared", "TMPDIR=/shared/tmp", NULL};
    const char *original[4]; memcpy(original, environment, sizeof(original));
    unsigned count = 3;
    assert(!LSBApplyApplicationEnvironment(prepared, environment, &count, 4) && count == 3);
    assert(!memcmp(original, environment, sizeof(original)));
}
static void noChange(const char *root, const char *image, const char *home) {
    LSBApplicationEnvironment result;
    prepare(root, image, home, false, &result);
    unchanged(&result);
}
static void testApplicationEnvironment(const char *base, const char *aliasBase) {
    const char *bundleRelative = "data/Containers/Bundle/Application/11111111-1111-1111-1111-111111111111/Example.app";
    const char *homeRelative = "data/Containers/Data/Application/22222222-2222-2222-2222-222222222222";
    char root[PATH_MAX], image[PATH_MAX], app[PATH_MAX], home[PATH_MAX], aliasRoot[PATH_MAX], aliasImage[PATH_MAX], aliasHome[PATH_MAX];
    join(root, base, "RuntimeRoot"); directories(root);
    join(app, base, bundleRelative); directories(app);
    join(image, app, "Example"); file(image);
    join(home, base, homeRelative); directories(home);
    join(aliasRoot, aliasBase, "RuntimeRoot");
    join(aliasImage, aliasBase, bundleRelative); strcat(aliasImage, "/Example");
    join(aliasHome, aliasBase, homeRelative);
    LSBApplicationEnvironment result;
    prepare(root, image, home, true, &result);
    assert(!strcmp(result.fixedHome + strlen("CFFIXED_USER_HOME="), home));
    char expectedTemporary[PATH_MAX + 8]; snprintf(expectedTemporary, sizeof(expectedTemporary), "TMPDIR=%s/tmp/", home);
    assert(!strcmp(result.temporaryDirectory, expectedTemporary));
    // /tmp -> /private/tmp models the device's /var -> /private/var physical aliases for all three paths.
    prepare(aliasRoot, aliasImage, aliasHome, true, &result);
    assert(!strcmp(result.fixedHome + strlen("CFFIXED_USER_HOME="), home));
    char aliasEntry[PATH_MAX + sizeof("HOME=")]; snprintf(aliasEntry, sizeof(aliasEntry), "HOME=%s", aliasHome);
    const char *environment[10] = {aliasEntry, "KEEP=value", "CFFIXED_USER_HOME=/old", "TMPDIR=/old/tmp", "TMPDIR=/duplicate",
        "CFFIXED_USER_HOME=/duplicate", "SIMULATOR_SHARED_RESOURCES_DIRECTORY=/shared", "HOME=/ignored-duplicate", NULL};
    unsigned count = 8;
    assert(!LSBApplyApplicationEnvironment(&result, environment, &count, 10) && count == 6 && !environment[count]);
    assert(environment[0] == aliasEntry && !strcmp(environment[1], "KEEP=value"));
    assert(!strcmp(environment[2], "SIMULATOR_SHARED_RESOURCES_DIRECTORY=/shared") && !strcmp(environment[3], "HOME=/ignored-duplicate"));
    assert(!strcmp(environment[4], result.fixedHome) && !strcmp(environment[5], result.temporaryDirectory));
    assert(!strcmp(LSBRuntimeEnvironmentValue(environment, "HOME"), aliasHome));
    const char *firstWins[] = {"HOME=/shared", aliasEntry, NULL};
    assert(!LSBPrepareApplicationEnvironment(root, image, firstWins, &result) && !result.applies);

    char path[PATH_MAX], unrelated[PATH_MAX];
    join(path, base, "data"); noChange(root, image, path);
    noChange(root, image, "/var/mobile");
    noChange(root, image, "relative");
    snprintf(path, sizeof(path), "%s/../22222222-2222-2222-2222-222222222222", home); noChange(root, image, path);
    snprintf(path, sizeof(path), "%s/", home); noChange(root, image, path);
    snprintf(path, sizeof(path), "%s//tmp", home); noChange(root, image, path);
    join(path, base, "data/Containers/Data/Application/not-a-uuid"); directories(path); noChange(root, image, path);
    join(path, home, "Documents"); directories(path); noChange(root, image, path);
    join(path, base, "data/Containers/Data/Application/33333333-3333-3333-3333-333333333333"); noChange(root, image, path);
    file(path); noChange(root, image, path); assert(!unlink(path));
    join(unrelated, base, "unrelated"); directories(unrelated);
    assert(!symlink(unrelated, path)); noChange(root, image, path);
    join(path, base, "home-alias"); assert(!symlink(home, path)); prepare(root, image, path, true, &result);
    join(path, root, "bin"); directories(path); join(path, root, "bin/df"); file(path); noChange(root, path, home);
    join(path, app, "PlugIns/Widget.appex"); directories(path);
    join(path, app, "PlugIns/Widget.appex/Widget"); file(path); noChange(root, path, home);
    join(path, app, "Helper.xpc"); directories(path); join(path, app, "Helper.xpc/Helper"); file(path); noChange(root, path, home);
    noChange(root, app, home);
    join(path, app, "escape"); assert(!symlink(image, path)); prepare(root, path, home, true, &result);
    assert(!unlink(path) && !symlink(unrelated, path)); noChange(root, path, home);
    join(path, base, "other/RuntimeRoot"); directories(path); noChange(path, image, home);
    char oversized[PATH_MAX + 1]; memset(oversized, 'a', sizeof(oversized)); oversized[0] = '/'; oversized[PATH_MAX] = 0;
    noChange(root, image, oversized);
    assert(!LSBPrepareApplicationEnvironment(root, image, NULL, &result) && !result.applies);
    assert(LSBPrepareApplicationEnvironment(NULL, image, NULL, &result) == EINVAL && !result.applies);
    assert(LSBPrepareApplicationEnvironment(root, image, NULL, NULL) == EINVAL);

    prepare(root, image, home, true, &result);
    const char *tight[4] = {aliasEntry, "KEEP=value", NULL, "sentinel"}, *original[4];
    memcpy(original, tight, sizeof(original)); count = 2;
    assert(LSBApplyApplicationEnvironment(&result, tight, &count, 4) == E2BIG && count == 2);
    assert(!memcmp(tight, original, sizeof(original)));
    const char *exact[5] = {aliasEntry, "KEEP=value", NULL}; count = 2;
    assert(!LSBApplyApplicationEnvironment(&result, exact, &count, 5) && count == 4 && !exact[4]);
    const char *invalid[4] = {aliasEntry, NULL, "not-null", NULL}; count = 2;
    assert(LSBApplyApplicationEnvironment(&result, invalid, &count, 4) == EINVAL);
    invalid[2] = NULL;
    assert(LSBApplyApplicationEnvironment(&result, invalid, &count, 4) == EINVAL);
    count = 4;
    assert(LSBApplyApplicationEnvironment(&result, invalid, &count, 4) == EINVAL);
}
int main(void) {
    char temporary[] = "/tmp/LSB-application-environment.XXXXXX", base[PATH_MAX];
    assert(mkdtemp(temporary) && realpath(temporary, base));
    testApplicationEnvironment(base, temporary);
    char *paths[] = {base, NULL};
    FTS *tree = fts_open(paths, FTS_PHYSICAL | FTS_NOCHDIR, NULL); assert(tree);
    FTSENT *entry;
    while ((entry = fts_read(tree))) {
        if (entry->fts_info == FTS_D) continue;
        assert(!(entry->fts_info == FTS_DP ? rmdir(entry->fts_accpath) : unlink(entry->fts_accpath)));
    }
    assert(!fts_close(tree));
    puts("ApplicationEnvironment: installed main scope, existing UUID homes, aliases, unchanged shared roots and bounded replacement passed");
    return 0;
}
