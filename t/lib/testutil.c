/*
 * Copyright (c) 2018, Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 *
 * Copyright 2018, UT-Battelle, LLC.
 *
 * LLNL-CODE-741539
 * All rights reserved.
 *
 * This is the license for UnifyFS.
 * For details, see https://github.com/LLNL/UnifyFS.
 * Please read https://github.com/LLNL/UnifyFS/LICENSE for full license text.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/stat.h>

static unsigned long seed;

/*
 * Seed the pseudo random number generator if it hasn't already been
 * seeded. Call this before calling rand() if you want a unique
 * pseudo random sequence.
 *
 * Test suites currently each run off their own main function so that they can
 * be run individually if need be. If they run too fast, seeding srand() with
 * time(NULL) can happen more than once in a second, causing the pseudo random
 * sequence to repeat which causes each suite to create the same random files.
 * Using gettimeofday() allows us to increase the granularity to microseconds.
 */
static void test_util_srand(void)
{
    if (seed == 0) {
        struct timeval tv;
        gettimeofday(&tv, NULL);

        /* Convert seconds since Epoch to microseconds and add the microseconds
         * in order to prevent the seed from rolling over and repeating. */
        seed = (tv.tv_sec * 1000000) + tv.tv_usec;
        srand(seed);
    }
}

/*
 * Store a random string of length @len into buffer @buf.
 */
void testutil_rand_string(char* buf, size_t len)
{
    const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                           "abcdefghijklmnopqrstuvwxyz"
                           "0123456789"
                           ".,_-+@";
    int idx;
    int i;

    test_util_srand();

    for (i = 0; i < len - 1; i++) {
        idx = rand() % (sizeof(charset) - 1);
        buf[i] = charset[idx];
    }
    buf[i] = '\0';
}

/*
 * Generate a path of length @len and store it into buffer @buf. The
 * path will begin with the NUL-terminated string pointed to by @pfx,
 * followed by a slash (/), followed by a random sequence of characters.
 */
void testutil_rand_path(char* buf, size_t len, const char* pfx)
{
    int rc;

    memset(buf, 0, len);
    rc = snprintf(buf, len, "%s/", pfx);
    testutil_rand_string(buf + rc, len - rc);
}

/*
 * Return a pointer to the path name of the UnifyFS mount point. Use the
 * value of the environment variable UNIFYFS_MOUNTPOINT if it exists,
 * otherwise use P_tmpdir which is defined in stdio.h and is typically
 * /tmp.
 */
char* testutil_get_mount_point(void)
{
    char* path;
    char* env = getenv("UNIFYFS_MOUNTPOINT");

    if (env != NULL) {
        path = env;
    } else {
        path = P_tmpdir;
    }

    return path;
}

/* Stat the file associated to by path and store the global size of the
 * file at path in the address of the global pointer passed in. */
void testutil_get_size(char* path, size_t* global)
{
    struct stat sb = {0};
    int rc;

    rc = stat(path, &sb);
    if (rc != 0) {
        printf("Error: %s\n", strerror(errno));
        exit(1);
    }
    if (global) {
        *global = sb.st_size;
    }
}
