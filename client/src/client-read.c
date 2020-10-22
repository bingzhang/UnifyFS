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

#include "client-read.h"


static void debug_print_read_req(read_req_t* req)
{
    if (NULL != req) {
        LOGDBG("read_req[%p] gfid=%d, file offset=%zu, length=%zu, buf=%p"
               " - nread=%zu, errcode=%d (%s), byte coverage=[%zu,%zu]",
               req, req->gfid, req->offset, req->length, req->buf, req->nread,
               req->errcode, unifyfs_rc_enum_description(req->errcode),
               req->cover_begin_offset, req->cover_end_offset);
    }
}

/* an arraylist to maintain the active mread requests for the client */
arraylist_t* active_mreads; // = NULL

/* use to generated unique ids for each new mread */
static int id_generator; // = 0

/* compute the arraylist index for the given request id. we use
 * modulo operator to reuse slots in the list */
static inline
int id_to_list_index(int id)
{
    int list_capacity = arraylist_capacity(active_mreads);
    return id % list_capacity;
}

/* Create a new mread request containing the n_reads requests provided
 * in read_reqs array */
client_mread_status* client_create_mread_request(int n_reads,
                                                 read_req_t* read_reqs)
{
    if (NULL == active_mreads) {
        LOGERR("active_mreads is NULL");
        return NULL;
    }

    int active_count = arraylist_size(active_mreads);
    if (active_count == arraylist_capacity(active_mreads)) {
        /* already at full capacity for outstanding reads */
        LOGWARN("too many outstanding client reads");
        return NULL;
    }

    /* generate an id that doesn't conflict with another active mread */
    int mread_id, req_ndx;
    void* existing;
    do {
        mread_id = ++id_generator;
        req_ndx = id_to_list_index(mread_id);
        existing = arraylist_get(active_mreads, req_ndx);
    } while (existing != NULL);

    client_mread_status* mread = calloc(1, sizeof(client_mread_status));
    if (NULL == mread) {
        LOGERR("failed to allocate client mread status");
        return NULL;
    }

    int rc = arraylist_insert(active_mreads, req_ndx, (void*)mread);
    if (rc != 0) {
        free(mread);
        return NULL;
    }

    mread->id = mread_id;
    mread->reqs = read_reqs;
    mread->n_reads = n_reads;
    ABT_mutex_create(&(mread->sync));

    rc = pthread_mutex_init(&(mread->mutex), NULL);
    if (rc != 0) {
        LOGERR("client mread status pthread mutex init failed");
        free(mread);
        return NULL;
    }
    rc = pthread_cond_init(&(mread->completed), NULL);
    if (rc != 0) {
        LOGERR("client mread status pthread condition init failed");
        free(mread);
        return NULL;
    }

    return mread;
}

/* Retrieve the mread request corresponding to the given mread_id. */
client_mread_status* client_get_mread_status(int mread_id)
{
    if (NULL == active_mreads) {
        LOGERR("active_mreads is NULL");
        return NULL;
    }

    int list_index = id_to_list_index(mread_id);
    void* list_item = arraylist_get(active_mreads, list_index);
    client_mread_status* status = (client_mread_status*)list_item;
    if (NULL != status) {
        if (status->id != mread_id) {
            LOGERR("mismatch on mread id=%d - status at index %d has id=%d",
                   mread_id, list_index, status->id);
            status = NULL;
        }
    } else {
        LOGERR("lookup of mread status for id=%d failed", mread_id);
    }
    return status;
}

/* Update the mread status for the request at the given req_index.
 * If the request is now complete, update the request's completion state
 * (i.e., errcode and nread) */
int client_update_mread_request(client_mread_status* mread,
                                int req_index,
                                int req_complete,
                                int req_error)
{
    int ret = UNIFYFS_SUCCESS;

    LOGDBG("updating mread[%d] status for request %d", mread->id, req_index);
    ABT_mutex_lock(mread->sync);
    if (req_index < mread->n_reads) {
        read_req_t* rdreq = mread->reqs + req_index;
        if (req_complete) {
            mread->n_complete++;
            if (req_error != 0) {
                mread->n_error++;
                rdreq->nread = 0;
                rdreq->errcode = req_error;
            } else {
                rdreq->nread = rdreq->cover_end_offset + 1;
            }
        }
    } else {
        LOGERR("invalid read request index %d (mread[%d] has %d reqs)",
               req_index, mread->id, mread->n_reads);
        ret = EINVAL;
    }

    int complete = (mread->n_complete == mread->n_reads);
    ABT_mutex_unlock(mread->sync);

    if (complete) {
        /* Signal client thread waiting on mread completion */
        LOGDBG("mread[%d] signalling completion of %d requests",
               mread->id, mread->n_reads);
        pthread_cond_signal(&(mread->completed));
    }

    return ret;
}


/* For the given read request and extent (file offset, length), calculate
 * the coverage including offsets from the beginning of the request and extent
 * and the coverage length. Return a pointer to the segment within the request
 * buffer where read data should be placed. */
char* get_extent_coverage(read_req_t* req,
                          size_t extent_file_offset,
                          size_t extent_length,
                          size_t* out_req_offset,
                          size_t* out_ext_offset,
                          size_t* out_length)
{
    assert(NULL != req);

    /* start and end file offset of this request */
    size_t req_start = req->offset;
    size_t req_end   = (req->offset + req->length) - 1;

    /* start and end file offset of the extent */
    size_t ext_start = extent_file_offset;
    size_t ext_end   = (ext_start + extent_length) - 1;

    if ((ext_end < req_start) || (ext_start > req_end)) {
        /* no overlap between request and extent */
        if (NULL != out_length) {
            *out_length = 0;
        }
        return NULL;
    }

    /* starting file offset of covered segment is maximum of extent and request
     * start offsets */
    size_t start = ext_start;
    if (req_start > start) {
        start = req_start;
    }

    /* ending file offset of covered segment is mimimum of extent and request
     * end offsets */
    size_t end = ext_end;
    if (req_end < end) {
        end = req_end;
    }

    /* compute length of covered segment */
    size_t cover_length = (end - start) + 1;

    /* compute byte offsets of covered segment start from request
     * and extent */
    size_t req_byte_offset = start - req_start;
    size_t ext_byte_offset = start - ext_start;

    /* fill output values for request and extent byte offsets for covered
     * segment and covered length */
    if (NULL != out_req_offset) {
        *out_req_offset = req_byte_offset;
    }
    if (NULL != out_ext_offset) {
        *out_ext_offset = ext_byte_offset;
    }
    if (NULL != out_length) {
        *out_length = cover_length;
    }

    /* return pointer to request buffer where extent data should be placed */
    assert((req_byte_offset + cover_length) <= req->length);
    return (req->buf + req_byte_offset);
}

void update_read_req_coverage(read_req_t* req,
                              size_t extent_byte_offset,
                              size_t extent_length)
{
    size_t end_byte_offset = (extent_byte_offset + extent_length) - 1;

    /* update bytes we have filled in the request buffer */
    if ((req->cover_begin_offset == (size_t)-1) ||
        (extent_byte_offset < req->cover_begin_offset)) {
        req->cover_begin_offset = extent_byte_offset;
    }

    if ((req->cover_end_offset == (size_t)-1) ||
        (end_byte_offset > req->cover_end_offset)) {
        req->cover_end_offset = end_byte_offset;
    }
}


/* This uses information in the extent map for a file on the client to
 * complete any read requests.  It only complets a request if it contains
 * all of the data.  Otherwise the request is copied to the list of
 * requests to be handled by the server. */
static
void service_local_reqs(
    read_req_t* read_reqs,   /* list of input read requests */
    int count,               /* number of input read requests */
    read_req_t* local_reqs,  /* output list of requests completed by client */
    read_req_t* server_reqs, /* output list of requests to forward to server */
    int* out_count)          /* number of items copied to server list */
{
    /* this will track the total number of requests we're passing
     * on to the server */
    int local_count  = 0;
    int server_count = 0;

    /* iterate over each input read request, satisfy it locally if we can
     * otherwise copy request into output list that the server will handle
     * for us */
    int i;
    for (i = 0; i < count; i++) {
        /* get current read request */
        read_req_t* req = &read_reqs[i];
        int gfid = req->gfid;

        /* lookup local extents if we have them */
        int fid = unifyfs_fid_from_gfid(gfid);

        /* move to next request if we can't find the matching fid */
        if (fid < 0) {
            /* copy current request into list of requests
             * that we'll ask server for */
            memcpy(&server_reqs[server_count], req, sizeof(read_req_t));
            server_count++;
            continue;
        }

        /* start and length of this request */
        size_t req_start = req->offset;
        size_t req_end   = req->offset + req->length;

        /* get pointer to extents for this file */
        unifyfs_filemeta_t* meta = unifyfs_get_meta_from_fid(fid);
        assert(meta != NULL);
        struct seg_tree* extents = &meta->extents;

        /* lock the extent tree for reading */
        seg_tree_rdlock(extents);

        /* can we fully satisfy this request? assume we can */
        int have_local = 1;

        /* this will point to the offset of the next byte we
         * need to account for */
        size_t expected_start = req_start;

        /* iterate over extents we have for this file,
         * and check that there are no holes in coverage.
         * we search for a starting extent using a range
         * of just the very first byte that we need */
        struct seg_tree_node* first;
        first = seg_tree_find_nolock(extents, req_start, req_start);
        struct seg_tree_node* next = first;
        while (next != NULL && next->start < req_end) {
            if (expected_start >= next->start) {
                /* this extent has the next byte we expect,
                 * bump up to the first byte past the end
                 * of this extent */
                expected_start = next->end + 1;
            } else {
                /* there is a gap between extents so we're missing
                 * some bytes */
                have_local = 0;
                break;
            }

            /* get the next element in the tree */
            next = seg_tree_iter(extents, next);
        }

        /* check that we account for the full request
         * up until the last byte */
        if (expected_start < req_end) {
            /* missing some bytes at the end of the request */
            have_local = 0;
        }

        /* if we can't fully satisfy the request, copy request to
         * output array, so it can be passed on to server */
        if (!have_local) {
            /* copy current request into list of requests
             * that we'll ask server for */
            memcpy(&server_reqs[server_count], req, sizeof(read_req_t));
            server_count++;

            /* release lock before we go to next request */
            seg_tree_unlock(extents);

            continue;
        }

        /* otherwise we can copy the data locally, iterate
         * over the extents and copy data into request buffer.
         * again search for a starting extent using a range
         * of just the very first byte that we need */
        next = first;
        while ((next != NULL) && (next->start < req_end)) {
            /* get start and length of this extent */
            size_t ext_start = next->start;
            size_t ext_length = (next->end + 1) - ext_start;

            /* get the offset into the log */
            size_t ext_log_pos = next->ptr;

            /* get number of bytes from start of extent and request
             * buffers to the start of the overlap region */
            size_t ext_byte_offset, req_byte_offset, cover_length;
            char* req_ptr = get_extent_coverage(req, ext_start, ext_length,
                                                &req_byte_offset,
                                                &ext_byte_offset,
                                                &cover_length);
            assert(req_ptr != NULL);

            /* copy data from local write log into user buffer */
            off_t log_offset = ext_log_pos + ext_byte_offset;
            size_t nread = 0;
            int rc = unifyfs_logio_read(logio_ctx, log_offset, cover_length,
                                        req_ptr, &nread);
            if (rc == UNIFYFS_SUCCESS) {
                /* update bytes we have filled in the request buffer */
                update_read_req_coverage(req, req_byte_offset, nread);
            } else {
                LOGERR("local log read failed for offset=%zu size=%zu",
                       (size_t)log_offset, cover_length);
                req->errcode = rc;
            }

            /* get the next element in the tree */
            next = seg_tree_iter(extents, next);
        }

        /* copy request data to list we completed locally */
        memcpy(&local_reqs[local_count], req, sizeof(read_req_t));
        local_count++;

        /* done reading the tree */
        seg_tree_unlock(extents);
    }

    /* return to user the number of key/values we set */
    *out_count = server_count;

    return;
}

/* order by file id then by offset */
static
int compare_read_req(const void* a, const void* b)
{
    const read_req_t* rra = a;
    const read_req_t* rrb = b;

    if (rra->gfid != rrb->gfid) {
        if (rra->gfid < rrb->gfid) {
            return -1;
        } else {
            return 1;
        }
    }

    if (rra->offset == rrb->offset) {
        return 0;
    } else if (rra->offset < rrb->offset) {
        return -1;
    } else {
        return 1;
    }
}

#if 0
/* notify our reqmgr that the shared memory buffer
 * is now clear and ready to hold more read data */
static
void delegator_signal(void)
{
    LOGDBG("receive buffer now empty");

    /* set shm flag to signal reqmgr we're done */
    shm_data_header* hdr = (shm_data_header*)(shm_recv_ctx->addr);
    hdr->state = SHMEM_REGION_EMPTY;

    /* TODO: MEM_FLUSH */
}

/* wait for reqmgr to inform us that shared memory buffer
 * is filled with read data */
static
int delegator_wait(void)
{
    int rc = (int)UNIFYFS_SUCCESS;

    /* specify time to sleep between checking flag in shared
     * memory indicating server has produced */
    struct timespec shm_wait_tm;
    shm_wait_tm.tv_sec  = 0;
    shm_wait_tm.tv_nsec = SHM_WAIT_INTERVAL;

    /* get pointer to flag in shared memory */
    shm_data_header* hdr = (shm_data_header*)(shm_recv_ctx->addr);

    /* wait for server to set flag to non-zero */
    int max_sleep = 5000000; // 5s
    volatile int* vip = (volatile int*)&(hdr->state);
    while (*vip == SHMEM_REGION_EMPTY) {
        /* not there yet, sleep for a while */
        nanosleep(&shm_wait_tm, NULL);
        /* TODO: MEM_FETCH */
        max_sleep--;
        if (0 == max_sleep) {
            LOGERR("timed out waiting for non-empty");
            rc = (int)UNIFYFS_ERROR_SHMEM;
            break;
        }
    }

    return rc;
}

/* copy read data from shared memory buffer to user buffers from read
 * calls, sets done=1 on return when reqmgr informs us it has no
 * more data */
static
int process_read_data(read_req_t* read_reqs, int count, int* done)
{
    /* assume we'll succeed */
    int rc = UNIFYFS_SUCCESS;

    /* get pointer to start of shared memory buffer */
    shm_data_header* shm_hdr = (shm_data_header*)(shm_recv_ctx->addr);
    char* shmptr = ((char*)shm_hdr) + sizeof(shm_data_header);

    /* get number of read replies in shared memory */
    size_t num = shm_hdr->meta_cnt;

    /* process each of our read replies */
    size_t i;
    for (i = 0; i < num; i++) {
        /* get pointer to current read reply header */
        shm_data_meta* rep = (shm_data_meta*)shmptr;
        shmptr += sizeof(shm_data_meta);

        /* get pointer to data */
        char* rep_buf = shmptr;
        shmptr += rep->length;

        LOGDBG("processing data response from server: "
               "[%zu] (gfid=%d, offset=%lu, length=%lu, errcode=%d)",
               i, rep->gfid, rep->offset, rep->length, rep->errcode);

        /* iterate over each of our read requests */
        size_t j;
        for (j = 0; j < count; j++) {
            /* get pointer to read request */
            read_req_t* req = &read_reqs[j];

            /* skip if this request if not the same file */
            if (rep->gfid != req->gfid) {
                /* request and reply are for different files */
                continue;
            }

            size_t rep_byte_offset, req_byte_offset, cover_length;
            char* req_ptr = get_extent_coverage(req, rep->offset, rep->length,
                                                &req_byte_offset,
                                                &rep_byte_offset,
                                                &cover_length);
            if (NULL == req_ptr) {
                /* no overlap, continue checking read_reqs */
                continue;
            }

            /* this reply overlaps with the request, check that
             * we didn't get an error */
            if (rep->errcode != UNIFYFS_SUCCESS) {
                /* TODO: should we look for the reply with an errcode
                 * with the lowest start offset? */

                /* read reply has an error, mark the read request
                 * as also having an error, then quit processing */
                req->errcode = rep->errcode;
                continue;
            }

            /* otherwise, we have an error-free, overlapping reply
             * for this request, copy data into request buffer */
            char* rep_ptr = rep_buf + rep_byte_offset;
            memcpy(req_ptr, rep_ptr, cover_length);
            LOGDBG("copied data to application buffer (%zu bytes)",
                   cover_length);

            /* update bytes we have filled in the request buffer */
            update_read_req_coverage(req, req_byte_offset, cover_length);
        }
    }

    /* set done flag if there is no more data */
    if (shm_hdr->state == SHMEM_REGION_DATA_COMPLETE) {
        *done = 1;
    }

    return rc;
}
#endif

/**
 * Service a list of client read requests using either local
 * data or forwarding requests to the server.
 *
 * @param in_reqs     a list of read requests
 * @param in_count    number of read requests
 *
 * @return error code
 */
int process_gfid_reads(read_req_t* in_reqs, int in_count)
{
    int i;
    int read_rc;

    /* assume we'll succeed */
    int ret = UNIFYFS_SUCCESS;

    /* assume we'll service all requests from the server */
    int server_count = in_count;
    read_req_t* server_reqs = in_reqs;
    read_req_t* local_reqs = NULL;

    /* TODO: if the file is laminated so that we know the file size,
     * we can adjust read requests to not read past the EOF */

    /* mark all read requests as in-progress */
    for (i = 0; i < in_count; i++) {
        in_reqs[i].errcode = EINPROGRESS;
    }

    /* if the option is enabled to service requests locally, try it,
     * in this case we'll allocate a large array which we split into
     * two, the first half will record requests we completed locally
     * and the second half will store requests to be sent to the server */

    /* this records the pointer to the temp request array if
     * we allocate one, we should free this later if not NULL */
    read_req_t* reqs = NULL;

    /* attempt to complete requests locally if enabled */
    if (unifyfs_local_extents) {
        /* allocate space to make local and server copies of the requests,
         * each list will be at most in_count long */
        size_t reqs_size = 2 * in_count;
        reqs = (read_req_t*) calloc(reqs_size, sizeof(read_req_t));
        if (reqs == NULL) {
            return ENOMEM;
        }

        /* define pointers to space where we can build our list
         * of requests handled on the client and those left
         * for the server */
        local_reqs = reqs;
        server_reqs = reqs + in_count;

        /* service reads from local extent info if we can, this copies
         * completed requests from in_reqs into local_reqs, and it copies
         * any requests that can't be completed locally into the server_reqs
         * to be processed by the server */
        service_local_reqs(in_reqs, in_count,
                           local_reqs, server_reqs, &server_count);

        /* return early if we satisfied all requests locally */
        if (server_count == 0) {
            /* copy completed requests back into user's array */
            memcpy(in_reqs, local_reqs, in_count * sizeof(read_req_t));

            /* free the temporary array */
            free(reqs);
            return ret;
        }
    }

    /* TODO: When the number of read requests exceed the
     * request buffer, split list io into multiple bulk
     * sends and transfer in bulks */

    /* check that we have enough slots for all read requests */
    if (server_count > UNIFYFS_MAX_READ_CNT) {
        LOGERR("Too many requests to pass to server");
        if (reqs != NULL) {
            free(reqs);
        }
        return ENOSPC;
    }

    /* order read request by increasing file id, then increasing offset */
    qsort(server_reqs, server_count, sizeof(read_req_t), compare_read_req);

    /* create mread status for tracking completion */
    client_mread_status* mread = client_create_mread_request(server_count,
                                                             server_reqs);
    if (NULL == mread) {
        return ENOMEM;
    }
    unsigned mread_id = mread->id;

    /* create buffer of extent requests */
    size_t size = (size_t)server_count * sizeof(unifyfs_extent_t);
    void* buffer = malloc(size);
    if (NULL == buffer) {
        return ENOMEM;
    }
    unifyfs_extent_t* extents = (unifyfs_extent_t*)buffer;
    for (i = 0; i < server_count; i++) {
        unifyfs_extent_t* ext = extents + i;
        read_req_t* req = server_reqs + i;
        ext->gfid = req->gfid;
        ext->offset = req->offset;
        ext->length = req->length;
    }

    LOGDBG("mread[%d]: n_reqs=%d, reqs(%p)",
           mread_id, server_count, server_reqs);

    /* invoke multi-read rpc on server */
    read_rc = invoke_client_mread_rpc(mread_id, server_count, size, buffer);
    free(buffer);

    if (read_rc != UNIFYFS_SUCCESS) {
        /* mark requests as failed if we couldn't even start the read(s) */
        LOGERR("failed to issue read RPC to server");
        for (i = 0; i < server_count; i++) {
            server_reqs[i].errcode = read_rc;
        }
        ret = read_rc;
    } else {
        /* wait for all requests to finish by blocking on mread
         * completion condition (with a reasonable timeout) */
        LOGDBG("waiting for completion of mread[%d]", mread->id);
        pthread_mutex_lock(&(mread->mutex));
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += UNIFYFS_CLIENT_READ_TIMEOUT_SECONDS;
        int wait_rc = pthread_cond_timedwait(&(mread->completed),
                                             &(mread->mutex), &timeout);
        if (wait_rc) {
            if (ETIMEDOUT == wait_rc) {
                LOGERR("mread[%d] timed out", mread->id);
                for (i = 0; i < server_count; i++) {
                    if (EINPROGRESS == server_reqs[i].errcode) {
                        server_reqs[i].errcode = wait_rc;
                        mread->n_error++;
                    }
                }
            }
            ret = wait_rc;
        }
        LOGDBG("mread[%d] wait completed (rc=%d) - %d requests, %d errors",
               mread->id, wait_rc, mread->n_reads, mread->n_error);
        pthread_mutex_unlock(&(mread->mutex));
    }

    /* got all of the data we'll get from the server, check for short reads
     * and whether those short reads are from errors, holes, or end of file */
    for (i = 0; i < server_count; i++) {
        /* get pointer to next read request */
        read_req_t* req = server_reqs + i;
        LOGDBG("mread[%d] server request %d:", mread->id, i);
        debug_print_read_req(req);

        /* no error message was received from server, assume success */
        if (req->errcode == EINPROGRESS) {
            req->errcode = UNIFYFS_SUCCESS;
        }

        /* if we hit an error on our read, nothing else to do */
        if (req->errcode != UNIFYFS_SUCCESS) {
            continue;
        }

        /* if we read all of the bytes, request is satisfied */
        if (req->nread == req->length) {
            /* check for read hole at beginning of request */
            if (req->cover_begin_offset != 0) {
                /* fill read hole at beginning of request */
                LOGDBG("zero-filling hole at offset %zu of length %zu",
                       req->offset, req->cover_begin_offset);
                memset(req->buf, 0, req->cover_begin_offset);
            }
            continue;
        }

        /* otherwise, we have a short read, check whether there
         * would be a hole after us, in which case we fill the
         * request buffer with zeros */

        /* get file size for this file */
        off_t filesize_offt = unifyfs_gfid_filesize(req->gfid);
        if (filesize_offt == (off_t)-1) {
            /* failed to get file size */
            req->errcode = ENOENT;
            continue;
        }
        size_t filesize = (size_t)filesize_offt;

        /* get offset of where hole starts */
        size_t gap_start = req->offset + req->nread;

        /* get last offset of the read request */
        size_t req_end = req->offset + req->length;

        /* if file size is larger than last offset we wrote to in
         * read request, then there is a hole we can fill */
        if (filesize > gap_start) {
            /* assume we can fill the full request with zero */
            size_t gap_length = req_end - gap_start;
            if (req_end > filesize) {
                /* request is trying to read past end of file,
                 * so only fill zeros up to end of file */
                gap_length = filesize - gap_start;
            }

            /* copy zeros into request buffer */
            LOGDBG("zero-filling hole at offset %zu of length %zu",
                   gap_start, gap_length);
            char* req_ptr = req->buf + req->nread;
            memset(req_ptr, 0, gap_length);

            /* update number of bytes read */
            req->nread += gap_length;
        }
    }

    /* if we attempted to service requests from our local extent map,
     * then we need to copy the resulting read requests from the local
     * and server arrays back into the user's original array */
    if (unifyfs_local_extents) {
        /* TODO: would be nice to copy these back into the same order
         * in which we received them. */

        /* copy locally completed requests back into user's array */
        int local_count = in_count - server_count;
        if (local_count > 0) {
            memcpy(in_reqs, local_reqs, local_count * sizeof(read_req_t));
        }

        /* copy sever completed requests back into user's array */
        if (server_count > 0) {
            /* skip past any items we copied in from the local requests */
            read_req_t* in_ptr = in_reqs + local_count;
            memcpy(in_ptr, server_reqs, server_count * sizeof(read_req_t));
        }

        /* free storage we used for copies of requests */
        if (reqs != NULL) {
            free(reqs);
            reqs = NULL;
        }
    }

    return ret;
}


