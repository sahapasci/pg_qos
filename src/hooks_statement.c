/*
 * hooks_statement.c - Statement-level QoS tracking and enforcement
 *
 * This file implements concurrent statement tracking for SELECT, UPDATE,
 * DELETE, and INSERT operations.
 *
 * Author:  M.Atif Ceylan
 * Company: AppstoniA OÜ
 * Created: October 28, 2025
 * Version: 1.0
 * License: See LICENSE file in the project root
 *
 * Copyright (c) 2025 AppstoniA OÜ
 * All rights reserved.
 */

#include "postgres.h"
#include "qos.h"
#include "hooks_internal.h"
#include "storage/lwlock.h"
#include "nodes/nodes.h"
#include "miscadmin.h"
#include "storage/proc.h"

/* Per-backend statement tracking */
static CmdType current_statement_type = CMD_UNKNOWN;
static bool statement_tracked = false;

/*
 * Track statement start - for SELECT, UPDATE, DELETE, INSERT concurrency limits
 */
void
qos_track_statement_start(CmdType operation)
{
    QoSLimits limits;
    int count = 0;
    int i;
    int limit_val = -1;
    int rate_kind = -1;
    int rate_count;
    int rate_window;
    int retry_after_ms = 0;
#ifndef MyBackendId
    int my_slot = -1;
#endif

    if (!qos_enabled || statement_tracked)
        return;

    limits = qos_get_cached_limits();

    /* Determine which limits apply */
    switch (operation)
    {
        case CMD_SELECT:
            limit_val = limits.max_concurrent_select;
            rate_kind = QOS_RATE_SELECT;
            break;
        case CMD_UPDATE:
            limit_val = limits.max_concurrent_update;
            rate_kind = QOS_RATE_UPDATE;
            break;
        case CMD_DELETE:
            limit_val = limits.max_concurrent_delete;
            rate_kind = QOS_RATE_DELETE;
            break;
        case CMD_INSERT:
            limit_val = limits.max_concurrent_insert;
            rate_kind = QOS_RATE_INSERT;
            break;
        default: return;
    }

    /* A rate count without an explicit window falls back to the default */
    rate_count = limits.max_rate[rate_kind];
    rate_window = limits.max_rate_window_ms[rate_kind];
    if (rate_count > 0 && rate_window <= 0)
        rate_window = QOS_RATE_WINDOW_DEFAULT_MS;

    if (qos_shared_state)
    {
#ifndef MyBackendId
    my_slot = qos_get_backend_slot(true);
#endif
        LWLockAcquire(qos_shared_state->lock, LW_EXCLUSIVE);
        
        /* Scan active backends to count current usage */
        for (i = 0; i < qos_shared_state->max_backends; i++)
        {
            /* Skip empty slots */
            if (qos_shared_state->backend_status[i].pid == 0)
                continue;
                
            /* Skip myself */
#ifndef MyBackendId
            if (i == my_slot)
                continue;
#else
            if (i == MyBackendId - 1)
                continue;
#endif
            
            /* Count if matches my role, db, and operation */
            if (qos_shared_state->backend_status[i].role_oid == GetUserId() &&
                qos_shared_state->backend_status[i].database_oid == MyDatabaseId &&
                qos_shared_state->backend_status[i].cmd_type == operation)
            {
                count++;
            }
        }
        
        /* Check limit */
        if (limit_val > 0 && count >= limit_val)
        {
            /* Update stats */
            switch (operation)
            {
                case CMD_SELECT: qos_shared_state->stats.concurrent_select_violations++; break;
                case CMD_UPDATE: qos_shared_state->stats.concurrent_update_violations++; break;
                case CMD_DELETE: qos_shared_state->stats.concurrent_delete_violations++; break;
                case CMD_INSERT: qos_shared_state->stats.concurrent_insert_violations++; break;
                default: break;
            }
            qos_shared_state->stats.rejected_queries++;
            
            LWLockRelease(qos_shared_state->lock);
            
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("qos: maximum concurrent %s statements exceeded", 
                            operation == CMD_SELECT ? "SELECT" :
                            operation == CMD_UPDATE ? "UPDATE" :
                            operation == CMD_DELETE ? "DELETE" : "INSERT"),
                     errdetail("Current: %d, Maximum: %d", count, limit_val),
                     errhint("Wait for other queries to complete")));
        }
        
        /* Register myself */
    #ifndef MyBackendId
        if (my_slot >= 0)
        {
            qos_shared_state->backend_status[my_slot].role_oid = GetUserId();
            qos_shared_state->backend_status[my_slot].database_oid = MyDatabaseId;
            qos_shared_state->backend_status[my_slot].cmd_type = operation;
        }
    #else
        qos_shared_state->backend_status[MyBackendId - 1].pid = MyProcPid;
        qos_shared_state->backend_status[MyBackendId - 1].role_oid = GetUserId();
        qos_shared_state->backend_status[MyBackendId - 1].database_oid = MyDatabaseId;
        qos_shared_state->backend_status[MyBackendId - 1].cmd_type = operation;
    #endif
        /* Preserve in_transaction state */
        
        LWLockRelease(qos_shared_state->lock);
        
        /* Only set tracking flags after successful registration */
        current_statement_type = operation;
        statement_tracked = true;

        /*
         * Rate check runs after the concurrency check so that a statement
         * already rejected for concurrency does not burn a token.  On
         * rejection we undo our registration first, otherwise the slot would
         * stay marked as running this command type.
         */
        if (rate_count > 0 &&
            !qos_rate_check(rate_kind, rate_count, rate_window, &retry_after_ms))
        {
            qos_track_statement_end();

            LWLockAcquire(qos_shared_state->lock, LW_EXCLUSIVE);
            qos_shared_state->stats.rate_violations[rate_kind]++;
            qos_shared_state->stats.rejected_queries++;
            LWLockRelease(qos_shared_state->lock);

            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("qos: %s rate limit exceeded",
                            qos_rate_kind_name(rate_kind)),
                     errdetail("Maximum %d %s statements per %d ms.",
                               rate_count, qos_rate_kind_name(rate_kind),
                               rate_window),
                     errhint("Retry after approximately %d ms.", retry_after_ms)));
        }
    }
}

/*
 * Track statement end - decrement statement-specific counters
 */
void
qos_track_statement_end(void)
{
#ifndef MyBackendId
    int my_slot = -1;
#endif
    if (!qos_enabled || !statement_tracked)
        return;
    
    if (qos_shared_state)
    {
#ifndef MyBackendId
    my_slot = qos_get_backend_slot(false);
#endif
        LWLockAcquire(qos_shared_state->lock, LW_EXCLUSIVE);
        
        /* Clear my command type */
#ifndef MyBackendId
        if (my_slot >= 0 && qos_shared_state->backend_status[my_slot].pid == MyProcPid)
            qos_shared_state->backend_status[my_slot].cmd_type = CMD_UNKNOWN;
#else
        if (qos_shared_state->backend_status[MyBackendId - 1].pid == MyProcPid)
            qos_shared_state->backend_status[MyBackendId - 1].cmd_type = CMD_UNKNOWN;
#endif
        
        LWLockRelease(qos_shared_state->lock);
    }
    
    statement_tracked = false;
    current_statement_type = CMD_UNKNOWN;
}
