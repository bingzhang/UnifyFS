/*
 * Copyright (c) 2020, Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 *
 * Copyright 2020, UT-Battelle, LLC.
 *
 * LLNL-CODE-741539
 * All rights reserved.
 *
 * This is the license for UnifyFS.
 * For details, see https://github.com/LLNL/UnifyFS.
 * Please read https://github.com/LLNL/UnifyFS/LICENSE for full license text.
 */

/*
 * Copyright (c) 2017, Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 * Copyright (c) 2017, Florida State University. Contributions from
 * the Computer Architecture and Systems Research Laboratory (CASTL)
 * at the Department of Computer Science.
 *
 * Written by: Teng Wang, Adam Moody, Weikuan Yu, Kento Sato, Kathryn Mohror
 * LLNL-CODE-728877. All rights reserved.
 *
 * This file is part of burstfs.
 * For details, see https://github.com/llnl/burstfs
 * Please read https://github.com/llnl/burstfs/LICENSE for full license text.
 */

#include "unifyfs_const.h"
#include "unifyfs_log.h"

#include <abt.h>
//#include <pthread.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>

/* one of the loglevel values */
unifyfs_log_level_t unifyfs_log_level = LOG_ERR;

/* pointer to log file stream */
FILE* unifyfs_log_stream; // = NULL

/* used within LOG macro to build a timestamp */
time_t unifyfs_log_time;
struct tm* unifyfs_log_ltime;
char unifyfs_log_timestamp[256];

/* used to reduce source file pathname length */
int unifyfs_log_source_base_len; // = 0
static const char* this_file = __FILE__;

/* logbuf accumulates log messages until full, then
 * we flush it to log stream */
#ifndef LOGBUF_SIZE
# define LOGBUF_SIZE 4096
#endif

static char logbuf[LOGBUF_SIZE];
static size_t logbuf_offset; // = 0
//pthread_mutex_t logsync = PTHREAD_MUTEX_INITIALIZER;
ABT_mutex logsync;

/* open specified file as log file stream,
 * or stderr if no file given.
 * returns UNIFYFS_SUCCESS on success */
int unifyfs_log_open(const char* file)
{
    if (0 == unifyfs_log_source_base_len) {
        // NOTE: if you change the source location of this file, update string
        char* srcdir = strstr(this_file, "common/src/unifyfs_log.c");
        if (NULL != srcdir) {
            unifyfs_log_source_base_len = srcdir - this_file;
        }
    }

    if (NULL == unifyfs_log_stream) {
        /* stderr is the default log stream */
        unifyfs_log_stream = stderr;

        ABT_mutex_create(&logsync);
    }

    if (NULL != file) {
        FILE* logf = fopen(file, "a");
        if (logf == NULL) {
            return ENOENT;
        } else {
            unifyfs_log_stream = logf;
        }
    }

    return (int)UNIFYFS_SUCCESS;
}


/* use log buffer page and pthread mutex to synchronize print statements */
void unifyfs_log_print(time_t now,
                       const char* srcfile,
                       int lineno,
                       const char* function,
                       char* msg)
{
    int print_to_buf = 1;

    char timestamp[64];
    struct tm* log_ltime = localtime(&now);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", log_ltime);

    char line_prefix[256];
    char* file = (char*)srcfile + unifyfs_log_source_base_len;
    size_t strings_len = strlen(file);
    strings_len += strlen(timestamp);
    strings_len += strlen(function);
    assert(strings_len < 256);
    size_t prefix_len = snprintf(line_prefix, sizeof(line_prefix),
                                 "%s tid=%ld @ %s() [%s:%d] ",
                                 timestamp, (long)unifyfs_gettid(),
                                 function, srcfile, lineno);
    size_t full_len = prefix_len + strlen(msg) + 2; /* +2 for '\n\0' */
    if ((full_len + logbuf_offset) >= LOGBUF_SIZE) {
        if (full_len >= LOGBUF_SIZE) {
            /* full message length exceeds buffer size, print directly */
            fprintf(unifyfs_log_stream, "%s%s\n", line_prefix, msg);
            print_to_buf = 0;
        } else {
            /* flush log buffer contents to log file stream */
            //pthread_mutex_lock(&logsync);
            //ABT_mutex_lock(logsync);
            fwrite(logbuf, logbuf_offset, 1, unifyfs_log_stream);
            logbuf_offset = 0;
            memset(logbuf, 0, LOGBUF_SIZE);
            //pthread_mutex_unlock(&logsync);
            //ABT_mutex_unlock(logsync);
        }
        fflush(unifyfs_log_stream);
    }
    if (print_to_buf) {
        //pthread_mutex_lock(&logsync);
        //ABT_mutex_lock(logsync);
        logbuf_offset += sprintf((logbuf + logbuf_offset), "%s%s\n",
                                 line_prefix, msg);
        //pthread_mutex_unlock(&logsync);
        //ABT_mutex_unlock(logsync);
    }
}

/* close our log file stream.
 * returns UNIFYFS_SUCCESS on success */
int unifyfs_log_close(void)
{
    /* if stream is open, and its not stderr, close it */
    if (NULL != unifyfs_log_stream) {
        if (unifyfs_log_stream != stderr) {
            fclose(unifyfs_log_stream);

            /* revert to stderr for any future log messages */
            unifyfs_log_stream = stderr;
        }
        ABT_mutex_free(&logsync);
    }

    return (int)UNIFYFS_SUCCESS;
}

/* set log level */
void unifyfs_set_log_level(unifyfs_log_level_t lvl)
{
    if (lvl < LOG_LEVEL_MAX) {
        unifyfs_log_level = lvl;
    }
}

pid_t unifyfs_gettid(void)
{
#if defined(gettid)
    return gettid();
#elif defined(SYS_gettid)
    return syscall(SYS_gettid);
#else
#error no gettid()
#endif
    return 0;
}
