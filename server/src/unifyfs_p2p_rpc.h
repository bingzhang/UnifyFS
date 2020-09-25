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

#ifndef _UNIFYFS_P2P_RPC_H
#define _UNIFYFS_P2P_RPC_H

/* Point-to-point Server RPCs */


/* determine server responsible for maintaining target file's metadata */
int hash_gfid_to_server(int gfid);


/**
 * @brief Add new extents to target file
 *
 * @param gfid     target file
 * @param len      length of file extents array
 * @param extents  array of extents to add
 *
 * @return success|failure
 */
int unifyfs_invoke_add_extents_rpc(int gfid,
                                   unsigned len,
                                   struct extent_tree_node* extents);

/**
 * @brief Get file size for the target file
 *
 * @param gfid      target file
 * @param filesize  pointer to size variable
 *
 * @return success|failure
 */
int unifyfs_invoke_filesize_rpc(int gfid,
                                size_t* filesize);

/**
 * @brief Laminate the target file
 *
 * @param gfid  target file
 *
 * @return success|failure
 */
int unifyfs_invoke_laminate_rpc(int gfid);

/**
 * @brief Get metadata for target file
 *
 * @param gfid    target file
 * @param create  flag indicating if this is a newly created file
 * @param attr    file attributes to update
 *
 * @return success|failure
 */
int unifyfs_invoke_metaget_rpc(int gfid,
                               unifyfs_file_attr_t* attr);

/**
 * @brief Update metadata for target file
 *
 * @param gfid     target file
 * @param attr_op  metadata operation that triggered update
 * @param attr     file attributes to update
 *
 * @return success|failure
 */
int unifyfs_invoke_metaset_rpc(int gfid, int attr_op,
                               unifyfs_file_attr_t* attr);

/**
 * @brief Truncate target file
 *
 * @param gfid      target file
 * @param filesize  truncated file size
 *
 * @return success|failure
 */
int unifyfs_invoke_truncate_rpc(int gfid, size_t filesize);


#endif // UNIFYFS_P2P_RPC_H
