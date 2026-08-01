/*
 * qos_shmem_slot.c - Shared-memory slot lookup shared by the QoS subsystems
 *
 * The rate limiter and the statistics collector both need the same thing: a
 * fixed-size array in shared memory, keyed by (database, role[, kind]), with
 * entries created on first use and never released.  This file holds that
 * logic once instead of once per subsystem.
 *
 * Publication protocol
 * --------------------
 * Readers scan without a lock.  An entry's key fields are written exactly
 * once, under a lock, and database_oid is stored LAST behind a write barrier.
 * A reader therefore loads database_oid first and only inspects the rest of
 * the key after a read barrier.  Without that pairing, a weakly ordered
 * architecture (aarch64, ppc64) could expose a published database_oid next to
 * a stale role_oid or kind, and the reader would either miss the entry or
 * match the wrong one.
 *
 * Author:  M.Atif Ceylan
 * Company: AppstoniA OÜ
 * Created: October 28, 2025
 * Version: 1.1
 * License: See LICENSE file in the project root
 *
 * Copyright (c) 2025 AppstoniA OÜ
 * All rights reserved.
 */

#include "postgres.h"
#include "qos.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/lwlock.h"

/*
 * Common prefix of every slot entry, so the generic code can read the key
 * without knowing the concrete entry type.
 *
 * Only arrays that are keyed by kind (callers passing kind >= 0) actually
 * have a `kind` member.  For arrays keyed by (db, role) alone the third word
 * belongs to the entry's own payload, so it must never be touched - writing
 * it corrupts whatever field the concrete struct puts there.
 */
typedef struct QoSSlotKey
{
    Oid     database_oid;
    Oid     role_oid;
    int     kind;           /* only valid when the caller passes kind >= 0 */
} QoSSlotKey;

/* Warn once per backend per array when it fills up */
static const char *warned_arrays[4];
static int  num_warned_arrays = 0;

static bool
qos_slot_warn_once(const char *what)
{
    int i;

    for (i = 0; i < num_warned_arrays; i++)
    {
        if (warned_arrays[i] == what)
            return false;
    }

    if (num_warned_arrays < (int) lengthof(warned_arrays))
        warned_arrays[num_warned_arrays++] = what;

    return true;
}

int
qos_find_or_create_slot(void *base, Size stride, int nslots,
                        Oid database_oid, Oid role_oid, int kind,
                        LWLockPadded *locks, int nstripes,
                        QoSSlotInitCallback init_cb, void *init_arg,
                        QoSSlotCache *cache, const char *what)
{
    int i;
    int cache_idx = (kind < 0) ? 0 : kind;

    if (base == NULL || locks == NULL || nslots <= 0)
        return -1;

    /*
     * Session identity changed - drop EVERY memoised index, not just the one
     * being looked up.  Dropping only this kind's entry would leave the other
     * kinds pointing at the previous role's slots.
     */
    if (cache->db != database_oid || cache->role != role_oid)
    {
        for (i = 0; i < QOS_RATE_NKINDS; i++)
            cache->slot[i] = -1;
        cache->db = database_oid;
        cache->role = role_oid;
    }
    else if (cache->slot[cache_idx] >= 0)
    {
        /*
         * Fast path: slots are never released once claimed, so a memoised
         * index stays valid for the life of the backend.
         */
        return cache->slot[cache_idx];
    }

    for (;;)
    {
        int free_slot = -1;
        LWLock *lock;
        QoSSlotKey *key;
        bool claimed = false;

        for (i = 0; i < nslots; i++)
        {
            Oid slot_db;

            key = (QoSSlotKey *) ((char *) base + (Size) i * stride);
            slot_db = key->database_oid;

            if (slot_db == InvalidOid)
            {
                if (free_slot < 0)
                    free_slot = i;
                continue;
            }

            if (slot_db != database_oid)
                continue;

            pg_read_barrier();

            if (key->role_oid == role_oid && (kind < 0 || key->kind == kind))
            {
                cache->slot[cache_idx] = i;
                return i;
            }
        }

        if (free_slot < 0)
        {
            if (qos_slot_warn_once(what))
                ereport(WARNING,
                        (errmsg("qos: %s slot table is full (%d entries)",
                                what, nslots),
                         errdetail("Further activity for db=%u role=%u is not tracked.",
                                   database_oid, role_oid)));
            return -1;
        }

        lock = &locks[free_slot % nstripes].lock;
        key = (QoSSlotKey *) ((char *) base + (Size) free_slot * stride);

        LWLockAcquire(lock, LW_EXCLUSIVE);

        if (key->database_oid == InvalidOid)
        {
            if (init_cb)
                init_cb((void *) key, init_arg);

            /*
             * Only kind-keyed arrays have this member; for the others the
             * word belongs to the entry's payload (see QoSSlotKey).
             */
            if (kind >= 0)
                key->kind = kind;

            key->role_oid = role_oid;

            /*
             * Publish last, behind a write barrier: lock-free readers key off
             * database_oid, so everything else must be visible before it is.
             */
            pg_write_barrier();
            key->database_oid = database_oid;
            claimed = true;
        }
        else if (key->database_oid == database_oid && key->role_oid == role_oid &&
                 (kind < 0 || key->kind == kind))
        {
            claimed = true;     /* another backend built the same entry */
        }

        LWLockRelease(lock);

        if (claimed)
        {
            cache->slot[cache_idx] = free_slot;
            return free_slot;
        }

        /*
         * Lost the race to a different key.  Rescan from the start rather
         * than resuming past this slot: a concurrent backend may have
         * published our key in a slot we already walked, and a duplicate
         * entry would permanently split the counters.
         */
    }
}
