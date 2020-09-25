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

#ifndef __UNIFYFS_INODE_H
#define __UNIFYFS_INODE_H

#include <pthread.h>
#include "tree.h"
#include "extent_tree.h"
#include "unifyfs_meta.h"
#include "unifyfs_global.h"

/**
 * @brief file and directory inode structure. this holds:
 */
struct unifyfs_inode {
    /* tree entry for global inode tree */
    RB_ENTRY(unifyfs_inode) inode_tree_entry;

    int gfid;                     /* global file identifier */
    int laminated;                /* is this file laminated? */
    unifyfs_file_attr_t attr;     /* file attributes */
    pthread_rwlock_t rwlock;      /* rwlock for accessing this structure */
    ABT_mutex abt_sync;

    struct extent_tree* extents;  /* extent information */
};

/**
 * @brief create a new inode with given parameters. The newly created inode
 * will be inserted to the global inode tree (global_inode_tree).
 *
 * @param gfid global file identifier.
 * @param attr attributes of the new file.
 *
 * @return 0 on success, errno otherwise
 */
int unifyfs_inode_create(int gfid, unifyfs_file_attr_t* attr);

/**
 * @brief update the attributes of file with @gfid. The attributes are
 * selectively updated with unifyfs_file_attr_update() function (see
 * common/unifyfs_meta.h).
 *
 * @param gfid global file identifier
 * @param attr new attributes
 *
 * @return 0 on success, errno otherwise
 */
int unifyfs_inode_update_attr(int gfid, int attrop, unifyfs_file_attr_t* attr);

/**
 * @brief create a new or update an existing inode.
 *
 * @param gfid global file identifier
 * @param create try to create a new inode if set
 * @param attr file attributes
 *
 * @return 0 on success, errno otherwise
 */
static inline
int unifyfs_inode_metaset(int gfid, int attrop, unifyfs_file_attr_t* attr)
{
    int ret = 0;

    if (attrop == UNIFYFS_FILE_ATTR_OP_CREATE) {
        ret = unifyfs_inode_create(gfid, attr);
    } else {
        ret = unifyfs_inode_update_attr(gfid, attrop, attr);
    }

    return ret;
}

/**
 * @brief read attributes for file with @gfid.
 *
 * @param gfid global file identifier
 * @param attr [out] file attributes to be filled
 *
 * @return 0 on success, errno otherwise
 */
int unifyfs_inode_metaget(int gfid, unifyfs_file_attr_t* attr);

/**
 * @brief unlink file with @gfid. this will remove the target file inode from
 * the global inode tree.
 *
 * @param gfid global file identifier
 *
 * @return 0 on success, errno otherwise
 */
int unifyfs_inode_unlink(int gfid);

/**
 * @brief truncate size of file with @gfid to @size.
 *
 * @param gfid global file identifier
 * @param size new file size
 *
 * @return 0 on success, errno otherwise
 */
int unifyfs_inode_truncate(int gfid, unsigned long size);

/**
 * @brief get the local extent array from the target inode
 *
 * @param gfid the global file identifier
 * @param n the number of extents, set by this function
 * @param nodes the pointer to the array of extents, caller should free this
 *
 * @return 0 on success, errno otherwise
 */
int unifyfs_inode_get_extents(int gfid, size_t* n,
                              struct extent_tree_node** nodes);

/**
 * @brief add new extents to the inode
 *
 * @param gfid the global file identifier
 * @param n the number of new extents in @nodes
 * @param nodes an array of extents to be added
 *
 * @return
 */
int unifyfs_inode_add_extents(int gfid, int n, struct extent_tree_node* nodes);

/**
 * @brief get the maximum file size from the local extent tree of given file
 *
 * @param gfid global file identifier
 * @param offset [out] file offset to be filled by this function
 *
 * @return 0 on success, errno otherwise
 */
int unifyfs_inode_get_filesize(int gfid, size_t* offset);

/**
 * @brief set the given file as laminated
 *
 * @param gfid global file identifier
 *
 * @return 0 on success, errno otherwise
 */
int unifyfs_inode_laminate(int gfid);

struct unifyfs_inode_chunk {
    int gfid;
    unsigned long offset;
    unsigned long length;
};
typedef struct unifyfs_inode_chunk unifyfs_inode_chunk_t;

/**
 * @brief Get chunks for given file extent
 *
 * @param gfid
 * @param offset
 * @param len
 * @param[out] n_chunks
 * @param[out] chunks
 *
 * @return UNIFYFS_SUCCESS, or error code
 */
int unifyfs_inode_get_chunk_list(
    int gfid,
    unsigned long offset,
    unsigned long len,
    unsigned int* n_chunks,
    chunk_read_req_t** chunks);

/**
 * @brief Get chunks for given file extent request
 *
 * @param chunk_request
 * @param[out] n_chunks
 * @param[out] chunks
 *
 * @return UNIFYFS_SUCCESS, or error code
 */
static inline int unifyfs_inode_resolve_chunk_request(
    unifyfs_inode_chunk_t* chunk_request,
    unsigned int* n_chunks,
    chunk_read_req_t** chunks)
{
    int ret = EINVAL;

    if (chunk_request) {
        int gfid = chunk_request->gfid;
        unsigned long offset = chunk_request->offset;
        unsigned long length = chunk_request->length;

        ret = unifyfs_inode_get_chunk_list(gfid, offset, length,
                                           n_chunks, chunks);
    }

    return ret;
}

/**
 * @brief calls extents_tree_span, which will do:
 *
 * given an extent tree and starting and ending logical offsets, fill in
 * key/value entries that overlap that range, returns at most max entries
 * starting from lowest starting offset, sets outnum with actual number of
 * entries returned
 *
 * @param gfid global file id
 * @param start starting logical offset
 * @param end ending logical offset
 * @param max maximum number of key/vals to return
 * @param keys array of length max for output keys
 * @param vals array of length max for output values
 * @param outnum number of entries returned
 *
 * @return
 */
int unifyfs_inode_span_extents(
    int gfid,               /* global file id we're looking in */
    unsigned long start,    /* starting logical offset */
    unsigned long end,      /* ending logical offset */
    int max,                /* maximum number of key/vals to return */
    void* keys,             /* array of length max for output keys */
    void* vals,             /* array of length max for output values */
    int* outnum);           /* number of entries returned */

/**
 * @brief prints the inode information to the log stream
 *
 * @param gfid global file identifier
 *
 * @return 0 on success, errno otherwise
 */
int unifyfs_inode_dump(int gfid);

#endif /* __UNIFYFS_INODE_H */

