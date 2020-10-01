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

#include "unifyfs_inode_tree.h"
#include "unifyfs_inode.h"
#include "unifyfs_group_rpc.h"
#include "unifyfs_p2p_rpc.h"
#include "unifyfs_request_manager.h"


static
int rpc_init(unifyfs_cfg_t* cfg)
{
    int ret = 0;
    long range_sz = 0;

    LOGDBG("initializing file operations..");

    ret = configurator_int_val(cfg->meta_range_size, &range_sz);
    if (ret != 0) {
        LOGERR("failed to read configuration (meta_range_size)");
    }
    meta_slice_sz = (size_t) range_sz;

    return ret;
}

static
int rpc_metaget(unifyfs_fops_ctx_t* ctx,
                int gfid,
                unifyfs_file_attr_t* attr)
{
    return unifyfs_invoke_metaget_rpc(gfid, attr);
}

static
int rpc_metaset(unifyfs_fops_ctx_t* ctx,
                int gfid,
                int attr_op,
                unifyfs_file_attr_t* attr)
{
    return unifyfs_invoke_metaset_rpc(gfid, attr_op, attr);
}

/*
 * sync rpc from client contains extents for a single gfid (file).
 */
static
int rpc_fsync(unifyfs_fops_ctx_t* ctx,
              int gfid)
{
    size_t i;

    /* assume we'll succeed */
    int ret = UNIFYFS_SUCCESS;

    /* get memory page size on this machine */
    int page_sz = getpagesize();

    /* get application client */
    app_client* client = get_app_client(ctx->app_id, ctx->client_id);
    if (NULL == client) {
        return EINVAL;
    }

    /* get pointer to superblock for this client and app */
    shm_context* super_ctx = client->shmem_super;
    if (NULL == super_ctx) {
        LOGERR("missing client superblock");
        return UNIFYFS_FAILURE;
    }
    char* superblk = (char*)(super_ctx->addr);

    /* get pointer to start of key/value region in superblock */
    char* meta = superblk + client->super_meta_offset;

    /* get number of file extent index values client has for us,
     * stored as a size_t value in meta region of shared memory */
    size_t num_extents = *(size_t*)(meta);

    /* indices are stored in the superblock shared memory
     * created by the client, these are stored as index_t
     * structs starting one page size offset into meta region
     *
     * Is it safe to assume that the index information in this superblock is
     * not going to be modified by the client while we perform this operation?
     */
    char* ptr_extents = meta + page_sz;

    if (num_extents == 0) {
        return UNIFYFS_SUCCESS;  /* Nothing to do */
    }

    unifyfs_index_t* meta_payload = (unifyfs_index_t*)(ptr_extents);

    struct extent_tree_node* extents = calloc(num_extents, sizeof(*extents));
    if (!extents) {
        LOGERR("failed to allocate memory for local_extents");
        return ENOMEM;
    }

    /* the sync rpc now contains extents from a single file/gfid */
    assert(gfid == meta_payload[0].gfid);

    for (i = 0; i < num_extents; i++) {
        struct extent_tree_node* extent = &extents[i];
        unifyfs_index_t* meta = &meta_payload[i];

        extent->start = meta->file_pos;
        extent->end = (meta->file_pos + meta->length) - 1;
        extent->svr_rank = glb_pmi_rank;
        extent->app_id = ctx->app_id;
        extent->cli_id = ctx->client_id;
        extent->pos = meta->log_pos;
    }

    /* update local inode state first */
    ret = unifyfs_inode_add_extents(gfid, num_extents, extents);
    if (ret) {
        LOGERR("failed to add local extents (gfid=%d, ret=%d)", gfid, ret);
        return ret;
    }

    /* then update owner inode state */
    ret = unifyfs_invoke_add_extents_rpc(gfid, num_extents, extents);
    if (ret) {
        LOGERR("failed to add extents (gfid=%d, ret=%d)", gfid, ret);
    }

    return ret;
}

static
int rpc_filesize(unifyfs_fops_ctx_t* ctx,
                 int gfid,
                 size_t* filesize)
{
    return unifyfs_invoke_filesize_rpc(gfid, filesize);
}

static
int rpc_truncate(unifyfs_fops_ctx_t* ctx,
                 int gfid,
                 off_t len)
{
    return unifyfs_invoke_truncate_rpc(gfid, len);
}

static
int rpc_laminate(unifyfs_fops_ctx_t* ctx,
                 int gfid)
{
    return unifyfs_invoke_laminate_rpc(gfid);
}

static
int rpc_unlink(unifyfs_fops_ctx_t* ctx,
               int gfid)
{
    return unifyfs_invoke_broadcast_unlink(gfid);
}

static
int create_remote_read_requests(unsigned n_chunks,
                                chunk_read_req_t* chunks,
                                unsigned* outlen,
                                remote_chunk_reads_t** out)
{
    int prev_rank = -1;
    unsigned num_remote_reads = 0;
    unsigned i = 0;
    remote_chunk_reads_t* remote_reads = NULL;
    remote_chunk_reads_t* current = NULL;
    chunk_read_req_t* pos = NULL;

    /* count how many delegators we need to contact */
    for (i = 0; i < n_chunks; i++) {
        chunk_read_req_t* curr_chunk = &chunks[i];
        int curr_rank = curr_chunk->rank;
        if (curr_rank != prev_rank) {
            num_remote_reads++;
        }
        prev_rank = curr_rank;
    }

    /* allocate and fill the per-delegator request data structure */
    remote_reads = (remote_chunk_reads_t*) calloc(num_remote_reads,
                                                  sizeof(*remote_reads));
    if (!remote_reads) {
        LOGERR("failed to allocate memory for remote_reads");
        return ENOMEM;
    }

    pos = chunks;
    unsigned int processed = 0;

    LOGDBG("preparing remote read request for %u chunks (%d delegators)",
           n_chunks, num_remote_reads);

    for (i = 0; i < num_remote_reads; i++) {
        int rank = pos->rank;

        current = &remote_reads[i];
        current->rank = rank;
        current->reqs = pos;

        for ( ; pos->rank == rank && processed < n_chunks; pos++) {
            current->total_sz += pos->nbytes;

            current->num_chunks += 1;
            processed++;
        }

        LOGDBG("%u/%u chunks processed: delegator %d (%u chunks, %zu bytes)",
               processed, n_chunks,
               rank, current->num_chunks, current->total_sz);
    }

    *outlen = num_remote_reads;
    *out = remote_reads;
    return UNIFYFS_SUCCESS;
}

static
int submit_read_request(unifyfs_fops_ctx_t* ctx,
                        size_t count,
                        unifyfs_inode_extent_t* extents)
{
    int ret = UNIFYFS_SUCCESS;
    unsigned n_chunks = 0;
    chunk_read_req_t* chunks = NULL;
    unsigned n_remote_reads = 0;
    remote_chunk_reads_t* remote_reads = NULL;
    server_read_req_t rdreq = { 0, };

    if ((count == 0) || (NULL == extents)) {
        return EINVAL;
    }

    LOGDBG("handling read request (%zu chunk requests)", count);

    /* see if we have a valid app information */
    int app_id = ctx->app_id;
    int client_id = ctx->client_id;

    /* get application client */
    app_client* client = get_app_client(app_id, client_id);
    if (NULL == client) {
        return (int) UNIFYFS_FAILURE;
    }

    /* resolve chunk locations from inode tree */
    ret = unifyfs_inode_resolve_extent_chunks((unsigned)count, extents,
                                              &n_chunks, &chunks);
    if (ret) {
        LOGERR("failed to resolve extent locations");
        goto out_fail;
    }
    if (n_chunks > 0) {
        /* prepare the read request requests */
        ret = create_remote_read_requests(n_chunks, chunks,
                                          &n_remote_reads, &remote_reads);
        if (ret) {
            LOGERR("failed to prepare the remote read requests");
            goto out_fail;
        }

        /* fill the information of server_read_req_t and submit */
        rdreq.app_id = app_id;
        rdreq.client_id = client_id;
        rdreq.chunks = chunks;
        rdreq.num_remote_reads = (int) n_remote_reads;
        rdreq.remote_reads = remote_reads;

        ret = rm_submit_read_request(&rdreq);
    } else {
        ret = ENODATA;
    }

out_fail:
    if (ret != UNIFYFS_SUCCESS) {
        if (remote_reads) {
            free(remote_reads);
            remote_reads = NULL;
        }

        if (chunks) {
            free(chunks);
            chunks = NULL;
        }
    }

    return ret;
}

static
int rpc_read(unifyfs_fops_ctx_t* ctx,
             int gfid,
             off_t offset,
             size_t length)
{
    unifyfs_inode_extent_t chunk = { 0, };

    chunk.gfid = gfid;
    chunk.offset = offset;
    chunk.length = length;

    return submit_read_request(ctx, 1, &chunk);
}

static
int rpc_mread(unifyfs_fops_ctx_t* ctx,
              size_t n_req,
              void* read_reqs)
{
    int ret = UNIFYFS_SUCCESS;
    int i = 0;
    unifyfs_inode_extent_t* chunks = NULL;
    unifyfs_extent_t* reqs = (unifyfs_extent_t*) read_reqs;

    chunks = calloc(n_req, sizeof(*chunks));
    if (!chunks) {
        LOGERR("failed to allocate the chunk request");
        return ENOMEM;
    }

    for (i = 0; i < (int)n_req; i++) {
        unifyfs_inode_extent_t* ch = chunks + i;
        unifyfs_extent_t* req = reqs + i;
        ch->gfid = req->gfid;
        ch->offset = req->offset;
        ch->length = req->length;
    }

    ret = submit_read_request(ctx, n_req, chunks);

    if (chunks) {
        free(chunks);
        chunks = NULL;
    }

    return ret;
}

static struct unifyfs_fops _fops_rpc = {
    .name = "rpc",
    .init = rpc_init,
    .metaget = rpc_metaget,
    .metaset = rpc_metaset,
    .fsync = rpc_fsync,
    .filesize = rpc_filesize,
    .truncate = rpc_truncate,
    .laminate = rpc_laminate,
    .unlink = rpc_unlink,
    .read = rpc_read,
    .mread = rpc_mread,
};

struct unifyfs_fops* unifyfs_fops_impl = &_fops_rpc;
