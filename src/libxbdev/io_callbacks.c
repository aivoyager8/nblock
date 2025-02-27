/**
 * @file io_callbacks.c
 * @brief Implements IO callbacks and retry mechanisms
 *
 * This file implements callback functions and retry mechanisms for IO operations,
 * handling SPDK asynchronous IO completion events.
 */

#include "xbdev.h"
#include "xbdev_internal.h"
#include <spdk/env.h>
#include <spdk/thread.h>
#include <spdk/bdev.h>
#include <spdk/log.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/**
 * Synchronous IO completion callback
 *
 * @param bdev_io BDEV IO object
 * @param success Whether IO was successful
 * @param cb_arg Callback parameter (IO completion structure)
 */
void xbdev_sync_io_completion_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    struct xbdev_sync_io_completion *completion = cb_arg;
    
    // Set result status
    if (success) {
        completion->status = SPDK_BDEV_IO_STATUS_SUCCESS;
    } else {
        completion->status = SPDK_BDEV_IO_STATUS_FAILED;
    }
    
    // Free BDEV IO
    spdk_bdev_free_io(bdev_io);
    
    // Mark as completed
    completion->done = true;
}

/**
 * Wait for synchronous operation to complete
 * 
 * @param done Pointer to completion flag
 * @param timeout_us Timeout in microseconds, 0 means no timeout
 * @return 0 on success, -ETIMEDOUT on timeout
 */
int _xbdev_wait_for_completion(bool *done, uint64_t timeout_us) {
    uint64_t start_ticks, current_ticks;
    
    // Record start time
    start_ticks = spdk_get_ticks();
    
    // Wait for completion or timeout
    while (!*done) {
        // Check for timeout
        if (timeout_us > 0) {
            current_ticks = spdk_get_ticks();
            uint64_t elapsed_us = (current_ticks - start_ticks) * 1000000 / spdk_get_ticks_hz();
            
            if (elapsed_us >= timeout_us) {
                XBDEV_ERRLOG("操作超时: %lu us\n", timeout_us);
                return -ETIMEDOUT;
            }
        }
        
        // Poll in SPDK thread context
        spdk_thread_poll(spdk_get_thread(), 0, 0);
    }
    
    return 0;
}

/**
 * Read operation retry function
 * 
 * When a read operation fails due to resource constraints,
 * SPDK adds this function to the wait queue to retry when resources are available.
 * 
 * @param arg Callback parameter
 */
void xbdev_sync_io_retry_read(void *arg) {
    struct xbdev_sync_io_completion *completion = arg;
    struct {
        struct spdk_bdev *bdev;
        struct spdk_bdev_desc *desc;
        struct spdk_io_channel *channel;
        void *buf;
        uint64_t offset_blocks;
        uint64_t num_blocks;
    } *retry_ctx = completion + 1;  // Context info immediately follows completion struct
    
    int rc;
    
    // Retry read operation
    rc = spdk_bdev_read_blocks(retry_ctx->desc, retry_ctx->channel, retry_ctx->buf,
                             retry_ctx->offset_blocks, retry_ctx->num_blocks,
                             xbdev_sync_io_completion_cb, completion);
    
    if (rc == -ENOMEM) {
        // Still out of resources, register wait again
        struct spdk_bdev_io_wait_entry *bdev_io_wait = (struct spdk_bdev_io_wait_entry *)
                                                     ((uint8_t *)completion + sizeof(*completion) + sizeof(*retry_ctx));
        
        bdev_io_wait->bdev = retry_ctx->bdev;
        bdev_io_wait->cb_fn = xbdev_sync_io_retry_read;
        bdev_io_wait->cb_arg = completion;
        
        spdk_bdev_queue_io_wait(retry_ctx->bdev, retry_ctx->channel, bdev_io_wait);
    } else if (rc != 0) {
        // Other error, mark IO as failed
        XBDEV_ERRLOG("Read retry failed: %d\n", rc);
        completion->status = SPDK_BDEV_IO_STATUS_FAILED;
        completion->done = true;
    }
}

/**
 * Write operation retry function
 * 
 * When a write operation fails due to resource constraints,
 * SPDK adds this function to the wait queue to retry when resources are available.
 * 
 * @param arg Callback parameter
 */
void xbdev_sync_io_retry_write(void *arg) {
    struct xbdev_sync_io_completion *completion = arg;
    struct {
        struct spdk_bdev *bdev;
        struct spdk_bdev_desc *desc;
        struct spdk_io_channel *channel;
        void *buf;
        uint64_t offset_blocks;
        uint64_t num_blocks;
    } *retry_ctx = completion + 1;  // Context info immediately follows completion struct
    
    int rc;
    
    // Retry write operation
    rc = spdk_bdev_write_blocks(retry_ctx->desc, retry_ctx->channel, retry_ctx->buf,
                              retry_ctx->offset_blocks, retry_ctx->num_blocks,
                              xbdev_sync_io_completion_cb, completion);
    
    if (rc == -ENOMEM) {
        // Still out of resources, register wait again
        struct spdk_bdev_io_wait_entry *bdev_io_wait = (struct spdk_bdev_io_wait_entry *)
                                                     ((uint8_t *)completion + sizeof(*completion) + sizeof(*retry_ctx));
        
        bdev_io_wait->bdev = retry_ctx->bdev;
        bdev_io_wait->cb_fn = xbdev_sync_io_retry_write;
        bdev_io_wait->cb_arg = completion;
        
        spdk_bdev_queue_io_wait(retry_ctx->bdev, retry_ctx->channel, bdev_io_wait);
    } else if (rc != 0) {
        // Other error, mark IO as failed
        XBDEV_ERRLOG("写入重试失败: %d\n", rc);
        completion->status = SPDK_BDEV_IO_STATUS_FAILED;
        completion->done = true;
    }
}

/**
 * Flush operation retry function
 * 
 * When a flush operation fails due to resource constraints,
 * SPDK adds this function to the wait queue to retry when resources are available.
 * 
 * @param arg Callback parameter
 */
void xbdev_sync_io_retry_flush(void *arg) {
    struct xbdev_sync_io_completion *completion = arg;
    struct {
        struct spdk_bdev *bdev;
        struct spdk_bdev_desc *desc;
        struct spdk_io_channel *channel;
    } *retry_ctx = completion + 1;  // Context info immediately follows completion struct
    
    int rc;
    
    // Retry flush operation
    rc = spdk_bdev_flush_blocks(retry_ctx->desc, retry_ctx->channel, 0, 
                               spdk_bdev_get_num_blocks(retry_ctx->bdev),
                               xbdev_sync_io_completion_cb, completion);
    
    if (rc == -ENOMEM) {
        // Still out of resources, register wait again
        struct spdk_bdev_io_wait_entry *bdev_io_wait = (struct spdk_bdev_io_wait_entry *)
                                                     ((uint8_t *)completion + sizeof(*completion) + sizeof(*retry_ctx));
        
        bdev_io_wait->bdev = retry_ctx->bdev;
        bdev_io_wait->cb_fn = xbdev_sync_io_retry_flush;
        bdev_io_wait->cb_arg = completion;
        
        spdk_bdev_queue_io_wait(retry_ctx->bdev, retry_ctx->channel, bdev_io_wait);
    } else if (rc != 0) {
        // Other error, mark IO as failed
        XBDEV_ERRLOG("刷新重试失败: %d\n", rc);
        completion->status = SPDK_BDEV_IO_STATUS_FAILED;
        completion->done = true;
    }
}

/**
 * UNMAP operation retry function
 * 
 * When an UNMAP operation fails due to resource constraints,
 * SPDK adds this function to the wait queue to retry when resources are available.
 * 
 * @param arg Callback parameter
 */
void xbdev_sync_io_retry_unmap(void *arg) {
    struct xbdev_sync_io_completion *completion = arg;
    struct {
        struct spdk_bdev *bdev;
        struct spdk_bdev_desc *desc;
        struct spdk_io_channel *channel;
        uint64_t offset_blocks;
        uint64_t num_blocks;
    } *retry_ctx = completion + 1;  // Context info immediately follows completion struct
    
    int rc;
    
    // Retry UNMAP operation
    rc = spdk_bdev_unmap_blocks(retry_ctx->desc, retry_ctx->channel, 
                              retry_ctx->offset_blocks, retry_ctx->num_blocks,
                              xbdev_sync_io_completion_cb, completion);
    
    if (rc == -ENOMEM) {
        // Still out of resources, register wait again
        struct spdk_bdev_io_wait_entry *bdev_io_wait = (struct spdk_bdev_io_wait_entry *)
                                                     ((uint8_t *)completion + sizeof(*completion) + sizeof(*retry_ctx));
        
        bdev_io_wait->bdev = retry_ctx->bdev;
        bdev_io_wait->cb_fn = xbdev_sync_io_retry_unmap;
        bdev_io_wait->cb_arg = completion;
        
        spdk_bdev_queue_io_wait(retry_ctx->bdev, retry_ctx->channel, bdev_io_wait);
    } else if (rc != 0) {
        // Other error, mark IO as failed
        XBDEV_ERRLOG("UNMAP重试失败: %d\n", rc);
        completion->status = SPDK_BDEV_IO_STATUS_FAILED;
        completion->done = true;
    }
}

/**
 * Reset device retry function
 * 
 * When a reset operation fails due to resource constraints,
 * SPDK adds this function to the wait queue to retry when resources are available.
 * 
 * @param arg Callback parameter
 */
void xbdev_sync_io_retry_reset(void *arg) {
    struct xbdev_sync_io_completion *completion = arg;
    struct {
        struct spdk_bdev *bdev;
        struct spdk_bdev_desc *desc;
        struct spdk_io_channel *channel;
    } *retry_ctx = completion + 1;  // Context info immediately follows completion struct
    
    int rc;
    
    // Retry reset operation
    rc = spdk_bdev_reset(retry_ctx->desc, retry_ctx->channel, 
                        xbdev_sync_io_completion_cb, completion);
    
    if (rc == -ENOMEM) {
        // Still out of resources, register wait again
        struct spdk_bdev_io_wait_entry *bdev_io_wait = (struct spdk_bdev_io_wait_entry *)
                                                     ((uint8_t *)completion + sizeof(*completion) + sizeof(*retry_ctx));
        
        bdev_io_wait->bdev = retry_ctx->bdev;
        bdev_io_wait->cb_fn = xbdev_sync_io_retry_reset;
        bdev_io_wait->cb_arg = completion;
        
        spdk_bdev_queue_io_wait(retry_ctx->bdev, retry_ctx->channel, bdev_io_wait);
    } else if (rc != 0) {
        // Other error, mark IO as failed
        XBDEV_ERRLOG("设备复位重试失败: %d\n", rc);
        completion->status = SPDK_BDEV_IO_STATUS_FAILED;
        completion->done = true;
    }
}

/**
 * Write zeroes operation retry function
 * 
 * When a write zeroes operation fails due to resource constraints,
 * SPDK adds this function to the wait queue to retry when resources are available.
 * 
 * @param arg Callback parameter
 */
void xbdev_sync_io_retry_write_zeroes(void *arg) {
    struct xbdev_sync_io_completion *completion = arg;
    struct {
        struct spdk_bdev *bdev;
        struct spdk_bdev_desc *desc;
        struct spdk_io_channel *channel;
        uint64_t offset_blocks;
        uint64_t num_blocks;
    } *retry_ctx = completion + 1;  // Context info immediately follows completion struct
    
    int rc;
    
    // Retry write zeroes operation
    rc = spdk_bdev_write_zeroes_blocks(retry_ctx->desc, retry_ctx->channel, 
                                     retry_ctx->offset_blocks, retry_ctx->num_blocks,
                                     xbdev_sync_io_completion_cb, completion);
    
    if (rc == -ENOMEM) {
        // Still out of resources, register wait again
        struct spdk_bdev_io_wait_entry *bdev_io_wait = (struct spdk_bdev_io_wait_entry *)
                                                     ((uint8_t *)completion + sizeof(*completion) + sizeof(*retry_ctx));
        
        bdev_io_wait->bdev = retry_ctx->bdev;
        bdev_io_wait->cb_fn = xbdev_sync_io_retry_write_zeroes;
        bdev_io_wait->cb_arg = completion;
        
        spdk_bdev_queue_io_wait(retry_ctx->bdev, retry_ctx->channel, bdev_io_wait);
    } else if (rc != 0) {
        // Other error, mark IO as failed
        XBDEV_ERRLOG("写入零重试失败: %d\n", rc);
        completion->status = SPDK_BDEV_IO_STATUS_FAILED;
        completion->done = true;
    }
}

/**
 * Compare operation retry function
 * 
 * When a compare operation fails due to resource constraints,
 * SPDK adds this function to the wait queue to retry when resources are available.
 * 
 * @param arg Callback parameter
 */
void xbdev_sync_io_retry_compare(void *arg) {
    struct xbdev_sync_io_completion *completion = arg;
    struct {
        struct spdk_bdev *bdev;
        struct spdk_bdev_desc *desc;
        struct spdk_io_channel *channel;
        void *buf;
        uint64_t offset_blocks;
        uint64_t num_blocks;
    } *retry_ctx = completion + 1;  // Context info immediately follows completion struct
    
    int rc;
    
    // Retry compare operation
    rc = spdk_bdev_compare_blocks(retry_ctx->desc, retry_ctx->channel, retry_ctx->buf,
                                retry_ctx->offset_blocks, retry_ctx->num_blocks,
                                xbdev_sync_io_completion_cb, completion);
    
    if (rc == -ENOMEM) {
        // Still out of resources, register wait again
        struct spdk_bdev_io_wait_entry *bdev_io_wait = (struct spdk_bdev_io_wait_entry *)
                                                     ((uint8_t *)completion + sizeof(*completion) + sizeof(*retry_ctx));
        
        bdev_io_wait->bdev = retry_ctx->bdev;
        bdev_io_wait->cb_fn = xbdev_sync_io_retry_compare;
        bdev_io_wait->cb_arg = completion;
        
        spdk_bdev_queue_io_wait(retry_ctx->bdev, retry_ctx->channel, bdev_io_wait);
    } else if (rc != 0) {
        // Other error, mark IO as failed
        XBDEV_ERRLOG("比较操作重试失败: %d\n", rc);
        completion->status = SPDK_BDEV_IO_STATUS_FAILED;
        completion->done = true;
    }
}

/**
 * Compare and write operation retry function
 * 
 * When a compare and write operation fails due to resource constraints,
 * SPDK adds this function to the wait queue to retry when resources are available.
 * 
 * @param arg Callback parameter
 */
void xbdev_sync_io_retry_compare_and_write(void *arg) {
    struct xbdev_sync_io_completion *completion = arg;
    struct {
        struct spdk_bdev *bdev;
        struct spdk_bdev_desc *desc;
        struct spdk_io_channel *channel;
        void *compare_buf;
        void *write_buf;
        uint64_t offset_blocks;
        uint64_t num_blocks;
    } *retry_ctx = completion + 1;  // Context info immediately follows completion struct
    
    int rc;
    
    // Retry compare and write operation
    rc = spdk_bdev_comparev_and_writev_blocks(retry_ctx->desc, retry_ctx->channel, 
                                            &retry_ctx->compare_buf, 1,
                                            &retry_ctx->write_buf, 1,
                                            retry_ctx->offset_blocks, retry_ctx->num_blocks,
                                            xbdev_sync_io_completion_cb, completion);
    
    if (rc == -ENOMEM) {
        // Still out of resources, register wait again
        struct spdk_bdev_io_wait_entry *bdev_io_wait = (struct spdk_bdev_io_wait_entry *)
                                                     ((uint8_t *)completion + sizeof(*completion) + sizeof(*retry_ctx));
        
        bdev_io_wait->bdev = retry_ctx->bdev;
        bdev_io_wait->cb_fn = xbdev_sync_io_retry_compare_and_write;
        bdev_io_wait->cb_arg = completion;
        
        spdk_bdev_queue_io_wait(retry_ctx->bdev, retry_ctx->channel, bdev_io_wait);
    } else if (rc != 0) {
        // Other error, mark IO as failed
        XBDEV_ERRLOG("比较并写入重试失败: %d\n", rc);
        completion->status = SPDK_BDEV_IO_STATUS_FAILED;
        completion->done = true;
    }
}

/**
 * Asynchronous IO callback function
 * 
 * Handles asynchronous IO completion events, calls user callback function.
 * 
 * @param bdev_io BDEV IO handle
 * @param success Whether operation was successful
 * @param cb_arg Callback parameter
 */
void xbdev_async_io_completion_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg) {
    xbdev_request_t *req = cb_arg;
    
    // Update request status
    req->status = success ? XBDEV_REQ_STATUS_COMPLETED : XBDEV_REQ_STATUS_ERROR;
    req->result = success ? 0 : -EIO;
    
    // Record completion timestamp
    req->complete_tsc = spdk_get_ticks();
    
    // Free BDEV IO
    spdk_bdev_free_io(bdev_io);
    
    // Call user callback function
    if (req->cb) {
        req->cb(req->cb_arg, req->result);
    }
}