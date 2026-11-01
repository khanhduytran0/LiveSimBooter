#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

static unsigned calls;
static int failure;
static char observedName[33];
static int nameSysctl(int *mib, u_int count, void *old, size_t *oldLength, void *value, size_t length) {
    assert(count == 2 && mib[0] == CTL_KERN && mib[1] == KERN_PROCNAME);
    assert(!old && !oldLength && value && length > 0 && length < sizeof(observedName));
    assert(!memchr(value, 0, length));
    memcpy(observedName, value, length);
    observedName[length] = 0; // Match XNU sysctl_io_string: the supplied length excludes the NUL it adds.
    ++calls;
    errno = failure;
    return failure ? -1 : 0;
}
#define sysctl nameSysctl
#include "../LiveProcess/ProcessDisplayName.c"
#undef sysctl

static void labelsAndBounds(void) {
    const struct { const char *path; pid_t pid; const char *expected; } labels[] = {
        {"Metal.backboardd", 0, "LSBHelper: Metal.backboardd"}, {"Metal.SpringBoard", 0, "LSBHelper: Metal.SpringBoard"},
        {"Metal.public", 0, "LSBHelper: Metal.public"}, {"Signer", 0, "LSBHelper: Signer"}, {"Metal.probe", 0, "LSBHelper: Metal.probe"},
        {"/sbin/launchd", 1, "LSB:1 launchd"}, {"/Applications/Contacts.app/Contacts", 1234, "LSB:1234 Contacts"}
    };
    for (unsigned i = 0; i < sizeof(labels) / sizeof(labels[0]); ++i) {
        struct { char before[8], argv0[128], after[32]; } storage;
        memset(&storage, 0xa5, sizeof(storage));
        strcpy(storage.argv0, "/private/var/containers/Bundle/Application/Test/LiveProcess.appex/LiveProcess");
        size_t capacity = strlen(storage.argv0) + 1;
        char original[sizeof(storage)];
        memcpy(original, &storage, sizeof(storage));
        errno = EDOM;
        assert(!LSBSetProcessDisplayName(labels[i].path, labels[i].pid, storage.argv0, capacity) && errno == EDOM);
        const char *expected = labels[i].expected;
        assert(!strcmp(storage.argv0, expected) && !strcmp(observedName, expected));
        for (size_t n = strlen(expected); n < capacity; ++n) assert(!storage.argv0[n]);
        assert(!memcmp(&storage, original, sizeof(storage.before)));
        size_t untouched = sizeof(storage.before) + capacity;
        assert(!memcmp((char *)&storage + untouched, original + untouched, sizeof(storage) - untouched));
    }
    char small[14]; memset(small, 'X', sizeof(small));
    assert(!LSBSetProcessDisplayName("Metal.public", 0, small, sizeof(small) - 1));
    assert(!strcmp(small, "LSBHelper: M") && small[13] == 'X' && !strcmp(observedName, "LSBHelper: Metal.public"));
}

static void prefixesAndIndependentLimits(void) {
    const char *longName = "/Applications/ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    char storage[96];
    assert(!LSBSetProcessDisplayName(longName, 123, storage, sizeof(storage)));
    assert(!strcmp(storage, "LSB:123 ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"));
    assert(!strcmp(observedName, "LSB:123 ABCDEFGHIJKLMNOPQRSTUVWX"));
    assert(!LSBSetProcessDisplayName(longName, INT_MAX, storage, sizeof(storage)));
    assert(!strcmp(storage, "LSB:2147483647 ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"));
    assert(!strcmp(observedName, "LSB:2147483647 ABCDEFGHIJKLMNOPQ"));
    assert(!LSBSetProcessDisplayName(longName, 0, storage, sizeof(storage)));
    assert(!strcmp(storage, "LSBHelper: ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"));
    assert(!strcmp(observedName, "LSBHelper: ABCDEFGHIJKLMNOPQRSTU"));
    char small[9];
    memset(small, 'X', sizeof(small));
    assert(!LSBSetProcessDisplayName("Contacts", 7, small, sizeof(small) - 1));
    assert(!strcmp(small, "LSB:7 C") && small[8] == 'X');
    assert(!strcmp(observedName, "LSB:7 Contacts"));
    assert(!LSBSetProcessDisplayName("Contacts", INT_MAX, NULL, 0));
    assert(!strcmp(observedName, "LSB:2147483647 Contacts"));
    assert(!LSBSetProcessDisplayName("Signer", 0, NULL, 0));
    assert(!strcmp(observedName, "LSBHelper: Signer"));
}

static void aliases(void) {
    char alias[64] = "/Applications/Contacts.app/Contacts";
    assert(!LSBSetProcessDisplayName(alias, 1234, alias, strlen(alias) + 1));
    assert(!strcmp(alias, "LSB:1234 Contacts") && !strcmp(observedName, "LSB:1234 Contacts"));
    strcpy(alias, "/ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
    assert(!LSBSetProcessDisplayName(alias, INT_MAX, alias, strlen(alias) + 1));
    assert(!strcmp(alias, "LSB:2147483647 ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijkl"));
    assert(!strcmp(observedName, "LSB:2147483647 ABCDEFGHIJKLMNOPQ"));
    strcpy(alias, "/Applications/Contacts.app/Contacts");
    assert(!LSBSetProcessDisplayName(alias, 0, alias, strlen(alias) + 1) && !strcmp(alias, "LSBHelper: Contacts"));
    assert(!strcmp(observedName, "LSBHelper: Contacts"));
}

static void shortAlias(void) {
    char alias[] = "Contacts";
    assert(!LSBSetProcessDisplayName(alias, 7, alias, sizeof(alias)));
    assert(!strcmp(alias, "LSB:7 Co") && !strcmp(observedName, "LSB:7 Contacts"));
    char helperAlias[] = "Metal.public";
    assert(!LSBSetProcessDisplayName(helperAlias, 0, helperAlias, sizeof(helperAlias)));
    assert(!strcmp(helperAlias, "LSBHelper: M") && !strcmp(observedName, "LSBHelper: Metal.public"));
}

static void errorsDoNotChangeArguments(void) {
    char storage[] = "native argv";
    unsigned before = calls;
    errno = ERANGE;
    assert(LSBSetProcessDisplayName(NULL, 0, storage, sizeof(storage)) == EINVAL && errno == ERANGE);
    assert(LSBSetProcessDisplayName("", 0, storage, sizeof(storage)) == EINVAL && errno == ERANGE);
    assert(LSBSetProcessDisplayName("/", 0, storage, sizeof(storage)) == EINVAL && errno == ERANGE);
    assert(LSBSetProcessDisplayName("/trailing/", 0, storage, sizeof(storage)) == EINVAL && errno == ERANGE);
    assert(LSBSetProcessDisplayName("valid", -1, storage, sizeof(storage)) == EINVAL && errno == ERANGE);
    assert(LSBSetProcessDisplayName("valid", 0, NULL, 1) == EINVAL && errno == ERANGE);
    assert(LSBSetProcessDisplayName("Contacts", 1234, storage, 10) == ENOSPC && errno == ERANGE);
    assert(LSBSetProcessDisplayName("Contacts", 7, storage, 7) == ENOSPC && errno == ERANGE);
    assert(LSBSetProcessDisplayName("Contacts", 7, storage, 1) == ENOSPC && errno == ERANGE);
    assert(LSBSetProcessDisplayName("Metal.public", 0, storage, sizeof(storage)) == ENOSPC && errno == ERANGE);
    assert(LSBSetProcessDisplayName("Signer", 0, storage, 1) == ENOSPC && errno == ERANGE);
    assert(calls == before && !strcmp(storage, "native argv"));
}

int main(void) {
    labelsAndBounds();
    prefixesAndIndependentLimits();
    aliases();
    shortAlias();
    errorsDoNotChangeArguments();
    char storage[16] = "native argv";
    failure = EPERM;
    errno = ERANGE;
    assert(LSBSetProcessDisplayName("Metal.public", 0, storage, sizeof(storage)) == EPERM && errno == ERANGE);
    assert(!strcmp(storage, "LSBHelper: Meta") && !strcmp(observedName, "LSBHelper: Metal.public"));
    char guestStorage[32];
    assert(LSBSetProcessDisplayName("Contacts", 42, guestStorage, sizeof(guestStorage)) == EPERM && errno == ERANGE);
    assert(!strcmp(guestStorage, "LSB:42 Contacts"));
    puts("Process display name tests passed");
    return 0;
}
