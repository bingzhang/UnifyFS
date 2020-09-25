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

#include "unifyfs_global.h"
#include "margo_server.h"
#include "unifyfs_server_rpcs.h"

/*************************************************************************
 * Peer-to-peer RPC helper methods
 *************************************************************************/

/* determine server responsible for maintaining target file's metadata */
int hash_gfid_to_server(int gfid)
{
    return gfid % glb_pmi_size;
}

/* server peer-to-peer (p2p) margo request structure */
typedef struct {
    margo_request request;
    hg_handle_t   handle;
} p2p_request;

/* helper method to initialize peer request rpc handle */
static int get_request_handle(hg_id_t request_hgid,
                              int peer_rank,
                              p2p_request* req)
{
    int rc = UNIFYFS_SUCCESS;

    /* get address for specified server rank */
    hg_addr_t addr = glb_servers[peer_rank].margo_svr_addr;

    /* get handle to rpc function */
    hg_return_t hret = margo_create(unifyfsd_rpc_context->svr_mid, addr,
                                    request_hgid, &(req->handle));
    if (hret != HG_SUCCESS) {
        LOGERR("failed to get handle for p2p request(%p) to server %d",
               req, peer_rank);
        rc = UNIFYFS_ERROR_MARGO;
    }

    return rc;
}

/* helper method to forward collective rpc request */
static int forward_request(void* input_ptr,
                           p2p_request* req)
{
    int rc = UNIFYFS_SUCCESS;

    /* call rpc function */
    hg_return_t hret = margo_iforward(req->handle, input_ptr,
                                      &(req->request));
    if (hret != HG_SUCCESS) {
        LOGERR("failed to forward p2p request(%p)", req);
        rc = UNIFYFS_ERROR_MARGO;
    }

    return rc;
}

/* helper method to wait for collective rpc request completion */
static int wait_for_request(p2p_request* req)
{
    int rc = UNIFYFS_SUCCESS;

    /* call rpc function */
    hg_return_t hret = margo_wait(req->request);
    if (hret != HG_SUCCESS) {
        LOGERR("wait on p2p request(%p) failed", req);
        rc = UNIFYFS_ERROR_MARGO;
    }

    return rc;
}

/*************************************************************************
 * File extents metadata update request
 *************************************************************************/

/* Add extents rpc handler */
static void add_extents_rpc(hg_handle_t handle)
{
    LOGDBG("add_extents rpc handler");

    /* assume we'll succeed */
    int32_t ret = UNIFYFS_SUCCESS;

    /* get input params */
    add_extents_in_t in;
    hg_return_t hret = margo_get_input(handle, &in);
    if (hret != HG_SUCCESS) {
        LOGERR("margo_get_input() failed");
        ret = UNIFYFS_ERROR_MARGO;
    } else {
        // TODO - unifyfs_inode_get_filesize(in->gfid, &filesize);
        margo_free_input(handle, &in);
    }

    /* build our output values */
    add_extents_out_t out;
    out.ret = ret;

    /* send output back to caller */
    hret = margo_respond(handle, &out);
    if (hret != HG_SUCCESS) {
        LOGERR("margo_respond() failed");
    }

    /* free margo resources */
    margo_destroy(handle);
}
DEFINE_MARGO_RPC_HANDLER(add_extents_rpc)

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
                                   struct extent_tree_node* extents)
{
    int server_rank = hash_gfid_to_server(gfid);
    return UNIFYFS_ERROR_NYI;
}

/*************************************************************************
 * File attributes request
 *************************************************************************/

/* Metaget rpc handler */
static void metaget_rpc(hg_handle_t handle)
{
    LOGDBG("metaget rpc handler");

    /* assume we'll succeed */
    int32_t ret = UNIFYFS_SUCCESS;

    /* get input params */
    metaget_in_t in;
    hg_return_t hret = margo_get_input(handle, &in);
    if (hret != HG_SUCCESS) {
        LOGERR("margo_get_input() failed");
        ret = UNIFYFS_ERROR_MARGO;
    } else {
        // TODO - unifyfs_inode_get_filesize(in->gfid, &filesize);
        margo_free_input(handle, &in);
    }

    /* build our output values */
    metaget_out_t out;
    out.ret = ret;
    // TODO - out.attr = ??;

    /* send output back to caller */
    hret = margo_respond(handle, &out);
    if (hret != HG_SUCCESS) {
        LOGERR("margo_respond() failed");
    }

    /* free margo resources */
    margo_destroy(handle);
}
DEFINE_MARGO_RPC_HANDLER(metaget_rpc)

/* Get file attributes for target file */
int unifyfs_invoke_metaget_rpc(int gfid,
                               unifyfs_file_attr_t* attr)
{
    int server_rank = hash_gfid_to_server(gfid);
    return UNIFYFS_ERROR_NYI;
}

/*************************************************************************
 * File size request
 *************************************************************************/

/* Filesize rpc handler */
static void filesize_rpc(hg_handle_t handle)
{
    LOGDBG("filesize rpc handler");

    /* assume we'll succeed */
    int32_t ret = UNIFYFS_SUCCESS;
    hg_size_t filesize = 0;

    /* get input params */
    filesize_in_t in;
    hg_return_t hret = margo_get_input(handle, &in);
    if (hret != HG_SUCCESS) {
        LOGERR("margo_get_input() failed");
        ret = UNIFYFS_ERROR_MARGO;
    } else {
        unifyfs_inode_get_filesize(in->gfid, &filesize);
        margo_free_input(handle, &in);
    }

    /* build our output values */
    filesize_out_t out;
    out.ret = ret;
    out.filesize = filesize;

    /* send output back to caller */
    hret = margo_respond(handle, &out);
    if (hret != HG_SUCCESS) {
        LOGERR("margo_respond() failed");
    }

    /* free margo resources */
    margo_destroy(handle);
}
DEFINE_MARGO_RPC_HANDLER(filesize_rpc)

/*  Get current global size for the target file */
int unifyfs_invoke_filesize_rpc(int gfid,
                                size_t* filesize)
{
    p2p_request preq;
    int owner_rank = hash_gfid_to_server(gfid);
    hg_id_t req_hgid = unifyfsd_rpc_context->rpcs.filesize_id;
    int rc = get_request_handle(req_hgid, owner_rank, &preq);
    if (rc != UNIFYFS_SUCCESS) {
        return rc;
    }

    filesize_in_t in;
    in.gfid = (int32_t)gfid;
    rc = forward_request((void*)&in, &preq);
    if (rc != UNIFYFS_SUCCESS) {
        return rc;
    }

    rc = wait_for_request(&preq);
    if (rc != UNIFYFS_SUCCESS) {
        return rc;
    }

    /* get the output of the rpc */
    int ret;
    filesize_out_t out;
    hg_return_t hret = margo_get_output(preq.handle, &out);
    if (hret != HG_SUCCESS) {
        LOGERR("margo_get_output() failed");
        ret = UNIFYFS_ERROR_MARGO;
    } else {
        /* set return value */
        ret = out.ret;
        if (ret == UNIFYFS_SUCCESS) {
            *filesize = (size_t) out.filesize;
        }
        margo_free_output(preq.handle, &out);
    }
    margo_destroy(preq.handle);

    return ret;
}

/*************************************************************************
 * File attributes update request
 *************************************************************************/

/* Metaset rpc handler */
static void metaset_rpc(hg_handle_t handle)
{
    LOGDBG("metaset rpc handler");

    /* assume we'll succeed */
    int32_t ret = UNIFYFS_SUCCESS;

    /* get input params */
    metaset_in_t in;
    hg_return_t hret = margo_get_input(handle, &in);
    if (hret != HG_SUCCESS) {
        LOGERR("margo_get_input() failed");
        ret = UNIFYFS_ERROR_MARGO;
    } else {
        // TODO - unifyfs_inode_get_filesize(in->gfid, &filesize);
        margo_free_input(handle, &in);
    }

    /* build our output values */
    metaset_out_t out;
    out.ret = ret;

    /* send output back to caller */
    hret = margo_respond(handle, &out);
    if (hret != HG_SUCCESS) {
        LOGERR("margo_respond() failed");
    }

    /* free margo resources */
    margo_destroy(handle);
}
DEFINE_MARGO_RPC_HANDLER(metaset_rpc)

/* Set metadata for target file */
int unifyfs_invoke_metaset_rpc(int gfid,
                               int create,
                               unifyfs_file_attr_t* attr)
{
    int server_rank = hash_gfid_to_server(gfid);
    return UNIFYFS_ERROR_NYI;
}

/*************************************************************************
 * File lamination request
 *************************************************************************/

/* Laminate rpc handler */
static void laminate_rpc(hg_handle_t handle)
{
    LOGDBG("laminate rpc handler");

    /* assume we'll succeed */
    int32_t ret = UNIFYFS_SUCCESS;

    /* get input params */
    laminate_in_t in;
    hg_return_t hret = margo_get_input(handle, &in);
    if (hret != HG_SUCCESS) {
        LOGERR("margo_get_input() failed");
        ret = UNIFYFS_ERROR_MARGO;
    } else {
        // TODO - unifyfs_inode_get_filesize(in->gfid, &filesize);
        margo_free_input(handle, &in);
    }

    /* build our output values */
    laminate_out_t out;
    out.ret = ret;

    /* send output back to caller */
    hret = margo_respond(handle, &out);
    if (hret != HG_SUCCESS) {
        LOGERR("margo_respond() failed");
    }

    /* free margo resources */
    margo_destroy(handle);
}
DEFINE_MARGO_RPC_HANDLER(laminate_rpc)

/*  Laminate the target file */
int unifyfs_invoke_laminate_rpc(int gfid)
{
    p2p_request preq;
    int owner_rank = hash_gfid_to_server(gfid);
    hg_id_t req_hgid = unifyfsd_rpc_context->rpcs.laminate_id;
    int rc = get_request_handle(req_hgid, owner_rank, &preq);
    if (rc != UNIFYFS_SUCCESS) {
        return rc;
    }

    laminate_in_t in;
    in.gfid = (int32_t)gfid;
    rc = forward_request((void*)&in, &preq);
    if (rc != UNIFYFS_SUCCESS) {
        return rc;
    }

    rc = wait_for_request(&preq);
    if (rc != UNIFYFS_SUCCESS) {
        return rc;
    }

    /* get the output of the rpc */
    int ret;
    laminate_out_t out;
    hg_return_t hret = margo_get_output(preq.handle, &out);
    if (hret != HG_SUCCESS) {
        LOGERR("margo_get_output() failed");
        ret = UNIFYFS_ERROR_MARGO;
    } else {
        /* set return value */
        ret = out.ret;
        margo_free_output(preq.handle, &out);
    }
    margo_destroy(preq.handle);

    return ret;
}

/*************************************************************************
 * File truncation request
 *************************************************************************/

/* Truncate rpc handler */
static void truncate_rpc(hg_handle_t handle)
{
    LOGDBG("truncate rpc handler");

    /* assume we'll succeed */
    int32_t ret = UNIFYFS_SUCCESS;

    /* get input params */
    truncate_in_t in;
    hg_return_t hret = margo_get_input(handle, &in);
    if (hret != HG_SUCCESS) {
        LOGERR("margo_get_input() failed");
        ret = UNIFYFS_ERROR_MARGO;
    } else {
        // TODO - unifyfs_inode_get_filesize(in->gfid, &filesize);
        margo_free_input(handle, &in);
    }

    /* build our output values */
    truncate_out_t out;
    out.ret = ret;

    /* send output back to caller */
    hret = margo_respond(handle, &out);
    if (hret != HG_SUCCESS) {
        LOGERR("margo_respond() failed");
    }

    /* free margo resources */
    margo_destroy(handle);
}
DEFINE_MARGO_RPC_HANDLER(truncate_rpc)

/* Truncate target file */
int unifyfs_invoke_truncate_rpc(int gfid, size_t filesize)
{
    int server_rank = hash_gfid_to_server(gfid);
    return UNIFYFS_ERROR_NYI;
}