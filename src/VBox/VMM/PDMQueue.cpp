/* $Id$ */
/** @file
 * PDM Queue - Transport data and tasks to EMT and R3.
 */

/*
 * Copyright (C) 2006-2007 Sun Microsystems, Inc.
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa
 * Clara, CA 95054 USA or visit http://www.sun.com if you need
 * additional information or have any questions.
 */


/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#define LOG_GROUP LOG_GROUP_PDM_QUEUE
#include "PDMInternal.h"
#include <VBox/pdm.h>
#include <VBox/mm.h>
#include <VBox/rem.h>
#include <VBox/vm.h>
#include <VBox/err.h>

#include <VBox/log.h>
#include <iprt/asm.h>
#include <iprt/assert.h>
#include <iprt/thread.h>


/*******************************************************************************
*   Internal Functions                                                         *
*******************************************************************************/
DECLINLINE(void)            pdmR3QueueFree(PPDMQUEUE pQueue, PPDMQUEUEITEMCORE pItem);
static bool                 pdmR3QueueFlush(PPDMQUEUE pQueue);
static DECLCALLBACK(void)   pdmR3QueueTimer(PVM pVM, PTMTIMER pTimer, void *pvUser);



/**
 * Internal worker for the queue creation apis.
 *
 * @returns VBox status.
 * @param   pVM                 VM handle.
 * @param   cbItem              Item size.
 * @param   cItems              Number of items.
 * @param   cMilliesInterval    Number of milliseconds between polling the queue.
 *                              If 0 then the emulation thread will be notified whenever an item arrives.
 * @param   fRZEnabled          Set if the queue will be used from RC/R0 and need to be allocated from the hyper heap.
 * @param   pszName             The queue name. Unique. Not copied.
 * @param   ppQueue             Where to store the queue handle.
 */
static int pdmR3QueueCreate(PVM pVM, RTUINT cbItem, RTUINT cItems, uint32_t cMilliesInterval, bool fRZEnabled,
                            const char *pszName, PPDMQUEUE *ppQueue)
{
    /*
     * Validate input.
     */
    if (cbItem < sizeof(PDMQUEUEITEMCORE))
    {
        AssertMsgFailed(("cbItem=%d\n", cbItem));
        return VERR_INVALID_PARAMETER;
    }
    if (cItems < 1 || cItems >= 0x10000)
    {
        AssertMsgFailed(("cItems=%d valid:[1-65535]\n", cItems));
        return VERR_INVALID_PARAMETER;
    }

    /*
     * Align the item size and calculate the structure size.
     */
    cbItem = RT_ALIGN(cbItem, sizeof(RTUINTPTR));
    size_t cb = cbItem * cItems + RT_ALIGN_Z(RT_OFFSETOF(PDMQUEUE, aFreeItems[cItems + PDMQUEUE_FREE_SLACK]), 16);
    PPDMQUEUE pQueue;
    int rc;
    if (fRZEnabled)
        rc = MMHyperAlloc(pVM, cb, 0, MM_TAG_PDM_QUEUE, (void **)&pQueue );
    else
        rc = MMR3HeapAllocZEx(pVM, MM_TAG_PDM_QUEUE, cb, (void **)&pQueue);
    if (RT_FAILURE(rc))
        return rc;

    /*
     * Initialize the data fields.
     */
    pQueue->pVMR3 = pVM;
    pQueue->pVMR0 = fRZEnabled ? pVM->pVMR0 : NIL_RTR0PTR;
    pQueue->pVMRC = fRZEnabled ? pVM->pVMRC : NIL_RTRCPTR;
    pQueue->pszName = pszName;
    pQueue->cMilliesInterval = cMilliesInterval;
    //pQueue->pTimer = NULL;
    pQueue->cbItem = cbItem;
    pQueue->cItems = cItems;
    //pQueue->pPendingR3 = NULL;
    //pQueue->pPendingR0 = NULL;
    //pQueue->pPendingRC = NULL;
    pQueue->iFreeHead = cItems;
    //pQueue->iFreeTail = 0;
    PPDMQUEUEITEMCORE pItem = (PPDMQUEUEITEMCORE)((char *)pQueue + RT_ALIGN_Z(RT_OFFSETOF(PDMQUEUE, aFreeItems[cItems + PDMQUEUE_FREE_SLACK]), 16));
    for (unsigned i = 0; i < cItems; i++, pItem = (PPDMQUEUEITEMCORE)((char *)pItem + cbItem))
    {
        pQueue->aFreeItems[i].pItemR3 = pItem;
        if (fRZEnabled)
        {
            pQueue->aFreeItems[i].pItemR0 = MMHyperR3ToR0(pVM, pItem);
            pQueue->aFreeItems[i].pItemRC = MMHyperR3ToRC(pVM, pItem);
        }
    }

    /*
     * Create timer?
     */
    if (cMilliesInterval)
    {
        int rc = TMR3TimerCreateInternal(pVM, TMCLOCK_REAL, pdmR3QueueTimer, pQueue, "Queue timer", &pQueue->pTimer);
        if (RT_SUCCESS(rc))
        {
            rc = TMTimerSetMillies(pQueue->pTimer, cMilliesInterval);
            if (RT_FAILURE(rc))
            {
                AssertMsgFailed(("TMTimerSetMillies failed rc=%Rrc\n", rc));
                int rc2 = TMR3TimerDestroy(pQueue->pTimer); AssertRC(rc2);
            }
        }
        else
            AssertMsgFailed(("TMR3TimerCreateInternal failed rc=%Rrc\n", rc));
        if (RT_FAILURE(rc))
        {
            if (fRZEnabled)
                MMHyperFree(pVM, pQueue);
            else
                MMR3HeapFree(pQueue);
            return rc;
        }

        /*
         * Insert into the queue list for timer driven queues.
         */
        pdmLock(pVM);
        pQueue->pNext = pVM->pdm.s.pQueuesTimer;
        pVM->pdm.s.pQueuesTimer = pQueue;
        pdmUnlock(pVM);
    }
    else
    {
        /*
         * Insert into the queue list for forced action driven queues.
         * This is a FIFO, so insert at the end.
         */
        /** @todo we should add a priority priority to the queues so we don't have to rely on
         * the initialization order to deal with problems like #1605 (pgm/pcnet deadlock
         * caused by the critsect queue to be last in the chain).
         * - Update, the critical sections are no longer using queues, so this isn't a real
         *   problem any longer. The priority might be a nice feature for later though.
         */
        pdmLock(pVM);
        if (!pVM->pdm.s.pQueuesForced)
            pVM->pdm.s.pQueuesForced = pQueue;
        else
        {
            PPDMQUEUE pPrev = pVM->pdm.s.pQueuesForced;
            while (pPrev->pNext)
                pPrev = pPrev->pNext;
            pPrev->pNext = pQueue;
        }
        pdmUnlock(pVM);
    }

    /*
     * Register the statistics.
     */
    STAMR3RegisterF(pVM, &pQueue->cbItem,               STAMTYPE_U32,     STAMVISIBILITY_ALWAYS, STAMUNIT_BYTES,        "Item size.",                       "/PDM/Queue/%s/cbItem",         pQueue->pszName);
    STAMR3RegisterF(pVM, &pQueue->cItems,               STAMTYPE_U32,     STAMVISIBILITY_ALWAYS, STAMUNIT_COUNT,        "Queue size.",                      "/PDM/Queue/%s/cItems",         pQueue->pszName);
    STAMR3RegisterF(pVM, &pQueue->StatAllocFailures,    STAMTYPE_COUNTER, STAMVISIBILITY_ALWAYS, STAMUNIT_OCCURENCES,   "PDMQueueAlloc failures.",          "/PDM/Queue/%s/AllocFailures",  pQueue->pszName);
    STAMR3RegisterF(pVM, &pQueue->StatInsert,           STAMTYPE_COUNTER, STAMVISIBILITY_ALWAYS, STAMUNIT_CALLS,        "Calls to PDMQueueInsert.",         "/PDM/Queue/%s/Insert",         pQueue->pszName);
    STAMR3RegisterF(pVM, &pQueue->StatFlush,            STAMTYPE_COUNTER, STAMVISIBILITY_ALWAYS, STAMUNIT_CALLS,        "Calls to pdmR3QueueFlush.",        "/PDM/Queue/%s/Flush",          pQueue->pszName);
    STAMR3RegisterF(pVM, &pQueue->StatFlushLeftovers,   STAMTYPE_COUNTER, STAMVISIBILITY_ALWAYS, STAMUNIT_OCCURENCES,   "Left over items after flush.",     "/PDM/Queue/%s/FlushLeftovers", pQueue->pszName);
#ifdef VBOX_WITH_STATISTICS
    STAMR3RegisterF(pVM, &pQueue->StatFlushPrf,         STAMTYPE_PROFILE, STAMVISIBILITY_ALWAYS, STAMUNIT_CALLS,        "Profiling pdmR3QueueFlush.",       "/PDM/Queue/%s/FlushPrf",       pQueue->pszName);
    STAMR3RegisterF(pVM, (void *)&pQueue->cStatPending, STAMTYPE_U32,     STAMVISIBILITY_ALWAYS, STAMUNIT_COUNT,        "Pending items.",                   "/PDM/Queue/%s/Pending",        pQueue->pszName);
#endif

    *ppQueue = pQueue;
    return VINF_SUCCESS;
}


/**
 * Create a queue with a device owner.
 *
 * @returns VBox status code.
 * @param   pVM                 VM handle.
 * @param   pDevIns             Device instance.
 * @param   cbItem              Size a queue item.
 * @param   cItems              Number of items in the queue.
 * @param   cMilliesInterval    Number of milliseconds between polling the queue.
 *                              If 0 then the emulation thread will be notified whenever an item arrives.
 * @param   pfnCallback         The consumer function.
 * @param   fRZEnabled          Set if the queue must be usable from RC/R0.
 * @param   pszName             The queue name. Unique. Not copied.
 * @param   ppQueue             Where to store the queue handle on success.
 * @thread  Emulation thread only.
 */
VMMR3DECL(int) PDMR3QueueCreateDevice(PVM pVM, PPDMDEVINS pDevIns, RTUINT cbItem, RTUINT cItems, uint32_t cMilliesInterval,
                                      PFNPDMQUEUEDEV pfnCallback, bool fRZEnabled, const char *pszName, PPDMQUEUE *ppQueue)
{
    LogFlow(("PDMR3QueueCreateDevice: pDevIns=%p cbItem=%d cItems=%d cMilliesInterval=%d pfnCallback=%p fRZEnabled=%RTbool pszName=%s\n",
             pDevIns, cbItem, cItems, cMilliesInterval, pfnCallback, fRZEnabled));

    /*
     * Validate input.
     */
    VMCPU_ASSERT_EMT(&pVM->aCpus[0]);
    if (!pfnCallback)
    {
        AssertMsgFailed(("No consumer callback!\n"));
        return VERR_INVALID_PARAMETER;
    }

    /*
     * Create the queue.
     */
    PPDMQUEUE pQueue;
    int rc = pdmR3QueueCreate(pVM, cbItem, cItems, cMilliesInterval, fRZEnabled, pszName, &pQueue);
    if (RT_SUCCESS(rc))
    {
        pQueue->enmType = PDMQUEUETYPE_DEV;
        pQueue->u.Dev.pDevIns = pDevIns;
        pQueue->u.Dev.pfnCallback = pfnCallback;

        *ppQueue = pQueue;
        Log(("PDM: Created device queue %p; cbItem=%d cItems=%d cMillies=%d pfnCallback=%p pDevIns=%p\n",
             cbItem, cItems, cMilliesInterval, pfnCallback, pDevIns));
    }
    return rc;
}


/**
 * Create a queue with a driver owner.
 *
 * @returns VBox status code.
 * @param   pVM                 VM handle.
 * @param   pDrvIns             Driver instance.
 * @param   cbItem              Size a queue item.
 * @param   cItems              Number of items in the queue.
 * @param   cMilliesInterval    Number of milliseconds between polling the queue.
 *                              If 0 then the emulation thread will be notified whenever an item arrives.
 * @param   pfnCallback         The consumer function.
 * @param   pszName             The queue name. Unique. Not copied.
 * @param   ppQueue             Where to store the queue handle on success.
 * @thread  Emulation thread only.
 */
VMMR3DECL(int) PDMR3QueueCreateDriver(PVM pVM, PPDMDRVINS pDrvIns, RTUINT cbItem, RTUINT cItems, uint32_t cMilliesInterval,
                                      PFNPDMQUEUEDRV pfnCallback, const char *pszName, PPDMQUEUE *ppQueue)
{
    LogFlow(("PDMR3QueueCreateDriver: pDrvIns=%p cbItem=%d cItems=%d cMilliesInterval=%d pfnCallback=%p pszName=%s\n",
             pDrvIns, cbItem, cItems, cMilliesInterval, pfnCallback, pszName));

    /*
     * Validate input.
     */
    VMCPU_ASSERT_EMT(&pVM->aCpus[0]);
    if (!pfnCallback)
    {
        AssertMsgFailed(("No consumer callback!\n"));
        return VERR_INVALID_PARAMETER;
    }

    /*
     * Create the queue.
     */
    PPDMQUEUE pQueue;
    int rc = pdmR3QueueCreate(pVM, cbItem, cItems, cMilliesInterval, false, pszName, &pQueue);
    if (RT_SUCCESS(rc))
    {
        pQueue->enmType = PDMQUEUETYPE_DRV;
        pQueue->u.Drv.pDrvIns = pDrvIns;
        pQueue->u.Drv.pfnCallback = pfnCallback;

        *ppQueue = pQueue;
        Log(("PDM: Created driver queue %p; cbItem=%d cItems=%d cMillies=%d pfnCallback=%p pDrvIns=%p\n",
             cbItem, cItems, cMilliesInterval, pfnCallback, pDrvIns));
    }
    return rc;
}


/**
 * Create a queue with an internal owner.
 *
 * @returns VBox status code.
 * @param   pVM                 VM handle.
 * @param   cbItem              Size a queue item.
 * @param   cItems              Number of items in the queue.
 * @param   cMilliesInterval    Number of milliseconds between polling the queue.
 *                              If 0 then the emulation thread will be notified whenever an item arrives.
 * @param   pfnCallback         The consumer function.
 * @param   fRZEnabled          Set if the queue must be usable from RC/R0.
 * @param   pszName             The queue name. Unique. Not copied.
 * @param   ppQueue             Where to store the queue handle on success.
 * @thread  Emulation thread only.
 */
VMMR3DECL(int) PDMR3QueueCreateInternal(PVM pVM, RTUINT cbItem, RTUINT cItems, uint32_t cMilliesInterval,
                                        PFNPDMQUEUEINT pfnCallback, bool fRZEnabled, const char *pszName, PPDMQUEUE *ppQueue)
{
    LogFlow(("PDMR3QueueCreateInternal: cbItem=%d cItems=%d cMilliesInterval=%d pfnCallback=%p fRZEnabled=%RTbool pszName=%s\n",
             cbItem, cItems, cMilliesInterval, pfnCallback, fRZEnabled, pszName));

    /*
     * Validate input.
     */
    VMCPU_ASSERT_EMT(&pVM->aCpus[0]);
    if (!pfnCallback)
    {
        AssertMsgFailed(("No consumer callback!\n"));
        return VERR_INVALID_PARAMETER;
    }

    /*
     * Create the queue.
     */
    PPDMQUEUE pQueue;
    int rc = pdmR3QueueCreate(pVM, cbItem, cItems, cMilliesInterval, fRZEnabled, pszName, &pQueue);
    if (RT_SUCCESS(rc))
    {
        pQueue->enmType = PDMQUEUETYPE_INTERNAL;
        pQueue->u.Int.pfnCallback = pfnCallback;

        *ppQueue = pQueue;
        Log(("PDM: Created internal queue %p; cbItem=%d cItems=%d cMillies=%d pfnCallback=%p\n",
             cbItem, cItems, cMilliesInterval, pfnCallback));
    }
    return rc;
}


/**
 * Create a queue with an external owner.
 *
 * @returns VBox status code.
 * @param   pVM                 VM handle.
 * @param   cbItem              Size a queue item.
 * @param   cItems              Number of items in the queue.
 * @param   cMilliesInterval    Number of milliseconds between polling the queue.
 *                              If 0 then the emulation thread will be notified whenever an item arrives.
 * @param   pfnCallback         The consumer function.
 * @param   pvUser              The user argument to the consumer function.
 * @param   pszName             The queue name. Unique. Not copied.
 * @param   ppQueue             Where to store the queue handle on success.
 * @thread  Emulation thread only.
 */
VMMR3DECL(int) PDMR3QueueCreateExternal(PVM pVM, RTUINT cbItem, RTUINT cItems, uint32_t cMilliesInterval, PFNPDMQUEUEEXT pfnCallback, void *pvUser, const char *pszName, PPDMQUEUE *ppQueue)
{
    LogFlow(("PDMR3QueueCreateExternal: cbItem=%d cItems=%d cMilliesInterval=%d pfnCallback=%p pszName=%s\n", cbItem, cItems, cMilliesInterval, pfnCallback, pszName));

    /*
     * Validate input.
     */
    VMCPU_ASSERT_EMT(&pVM->aCpus[0]);
    if (!pfnCallback)
    {
        AssertMsgFailed(("No consumer callback!\n"));
        return VERR_INVALID_PARAMETER;
    }

    /*
     * Create the queue.
     */
    PPDMQUEUE pQueue;
    int rc = pdmR3QueueCreate(pVM, cbItem, cItems, cMilliesInterval, false, pszName, &pQueue);
    if (RT_SUCCESS(rc))
    {
        pQueue->enmType = PDMQUEUETYPE_EXTERNAL;
        pQueue->u.Ext.pvUser = pvUser;
        pQueue->u.Ext.pfnCallback = pfnCallback;

        *ppQueue = pQueue;
        Log(("PDM: Created external queue %p; cbItem=%d cItems=%d cMillies=%d pfnCallback=%p pvUser=%p\n",
             cbItem, cItems, cMilliesInterval, pfnCallback, pvUser));
    }
    return rc;
}


/**
 * Destroy a queue.
 *
 * @returns VBox status code.
 * @param   pQueue      Queue to destroy.
 * @thread  Emulation thread only.
 */
VMMR3DECL(int) PDMR3QueueDestroy(PPDMQUEUE pQueue)
{
    LogFlow(("PDMR3QueueDestroy: pQueue=%p\n", pQueue));

    /*
     * Validate input.
     */
    if (!pQueue)
        return VERR_INVALID_PARAMETER;
    Assert(pQueue && pQueue->pVMR3);
    PVM pVM = pQueue->pVMR3;

    pdmLock(pVM);

    /*
     * Unlink it.
     */
    if (pQueue->pTimer)
    {
        if (pVM->pdm.s.pQueuesTimer != pQueue)
        {
            PPDMQUEUE pCur = pVM->pdm.s.pQueuesTimer;
            while (pCur)
            {
                if (pCur->pNext == pQueue)
                {
                    pCur->pNext = pQueue->pNext;
                    break;
                }
                pCur = pCur->pNext;
            }
            AssertMsg(pCur, ("Didn't find the queue!\n"));
        }
        else
            pVM->pdm.s.pQueuesTimer = pQueue->pNext;
    }
    else
    {
        if (pVM->pdm.s.pQueuesForced != pQueue)
        {
            PPDMQUEUE pCur = pVM->pdm.s.pQueuesForced;
            while (pCur)
            {
                if (pCur->pNext == pQueue)
                {
                    pCur->pNext = pQueue->pNext;
                    break;
                }
                pCur = pCur->pNext;
            }
            AssertMsg(pCur, ("Didn't find the queue!\n"));
        }
        else
            pVM->pdm.s.pQueuesForced = pQueue->pNext;
    }
    pQueue->pNext = NULL;
    pQueue->pVMR3 = NULL;
    pdmUnlock(pVM);

    /*
     * Deregister statistics.
     */
    STAMR3Deregister(pVM, &pQueue->cbItem);
    STAMR3Deregister(pVM, &pQueue->cbItem);
    STAMR3Deregister(pVM, &pQueue->StatInsert);
    STAMR3Deregister(pVM, &pQueue->StatFlush);
    STAMR3Deregister(pVM, &pQueue->StatFlushLeftovers);
#ifdef VBOX_WITH_STATISTICS
    STAMR3Deregister(pVM, &pQueue->StatFlushPrf);
#endif

    /*
     * Destroy the timer and free it.
     */
    if (pQueue->pTimer)
    {
        TMR3TimerDestroy(pQueue->pTimer);
        pQueue->pTimer = NULL;
    }
    if (pQueue->pVMRC)
    {
        pQueue->pVMRC = NIL_RTRCPTR;
        pQueue->pVMR0 = NIL_RTR0PTR;
        MMHyperFree(pVM, pQueue);
    }
    else
        MMR3HeapFree(pQueue);

    return VINF_SUCCESS;
}


/**
 * Destroy a all queues owned by the specified device.
 *
 * @returns VBox status code.
 * @param   pVM         VM handle.
 * @param   pDevIns     Device instance.
 * @thread  Emulation thread only.
 */
VMMR3DECL(int) PDMR3QueueDestroyDevice(PVM pVM, PPDMDEVINS pDevIns)
{
    LogFlow(("PDMR3QueueDestroyDevice: pDevIns=%p\n", pDevIns));

    /*
     * Validate input.
     */
    if (!pDevIns)
        return VERR_INVALID_PARAMETER;

    pdmLock(pVM);

    /*
     * Unlink it.
     */
    PPDMQUEUE pQueueNext = pVM->pdm.s.pQueuesTimer;
    PPDMQUEUE pQueue = pVM->pdm.s.pQueuesForced;
    do
    {
        while (pQueue)
        {
            if (    pQueue->enmType == PDMQUEUETYPE_DEV
                &&  pQueue->u.Dev.pDevIns == pDevIns)
            {
                PPDMQUEUE pQueueDestroy = pQueue;
                pQueue = pQueue->pNext;
                int rc = PDMR3QueueDestroy(pQueueDestroy);
                AssertRC(rc);
            }
            else
                pQueue = pQueue->pNext;
        }

        /* next queue list */
        pQueue = pQueueNext;
        pQueueNext = NULL;
    } while (pQueue);

    pdmUnlock(pVM);
    return VINF_SUCCESS;
}


/**
 * Destroy a all queues owned by the specified driver.
 *
 * @returns VBox status code.
 * @param   pVM         VM handle.
 * @param   pDrvIns     Driver instance.
 * @thread  Emulation thread only.
 */
VMMR3DECL(int) PDMR3QueueDestroyDriver(PVM pVM, PPDMDRVINS pDrvIns)
{
    LogFlow(("PDMR3QueueDestroyDriver: pDrvIns=%p\n", pDrvIns));

    /*
     * Validate input.
     */
    if (!pDrvIns)
        return VERR_INVALID_PARAMETER;

    pdmLock(pVM);

    /*
     * Unlink it.
     */
    PPDMQUEUE pQueueNext = pVM->pdm.s.pQueuesTimer;
    PPDMQUEUE pQueue = pVM->pdm.s.pQueuesForced;
    do
    {
        while (pQueue)
        {
            if (    pQueue->enmType == PDMQUEUETYPE_DRV
                &&  pQueue->u.Drv.pDrvIns == pDrvIns)
            {
                PPDMQUEUE pQueueDestroy = pQueue;
                pQueue = pQueue->pNext;
                int rc = PDMR3QueueDestroy(pQueueDestroy);
                AssertRC(rc);
            }
            else
                pQueue = pQueue->pNext;
        }

        /* next queue list */
        pQueue = pQueueNext;
        pQueueNext = NULL;
    } while (pQueue);

    pdmUnlock(pVM);
    return VINF_SUCCESS;
}


/**
 * Relocate the queues.
 *
 * @param   pVM             The VM handle.
 * @param   offDelta        The relocation delta.
 */
void pdmR3QueueRelocate(PVM pVM, RTGCINTPTR offDelta)
{
    /*
     * Process the queues.
     */
    PPDMQUEUE pQueueNext = pVM->pdm.s.pQueuesTimer;
    PPDMQUEUE pQueue = pVM->pdm.s.pQueuesForced;
    do
    {
        while (pQueue)
        {
            if (pQueue->pVMRC)
            {
                pQueue->pVMRC = pVM->pVMRC;

                /* Pending RC items. */
                if (pQueue->pPendingRC)
                {
                    pQueue->pPendingRC += offDelta;
                    PPDMQUEUEITEMCORE pCur = (PPDMQUEUEITEMCORE)MMHyperRCToR3(pVM, pQueue->pPendingRC);
                    while (pCur->pNextRC)
                    {
                        pCur->pNextRC += offDelta;
                        pCur = (PPDMQUEUEITEMCORE)MMHyperRCToR3(pVM, pCur->pNextRC);
                    }
                }

                /* The free items. */
                uint32_t i = pQueue->iFreeTail;
                while (i != pQueue->iFreeHead)
                {
                    pQueue->aFreeItems[i].pItemRC = MMHyperR3ToRC(pVM, pQueue->aFreeItems[i].pItemR3);
                    i = (i + 1) % (pQueue->cItems + PDMQUEUE_FREE_SLACK);
                }
            }

            /* next queue */
            pQueue = pQueue->pNext;
        }

        /* next queue list */
        pQueue = pQueueNext;
        pQueueNext = NULL;
    } while (pQueue);
}


/**
 * Flush pending queues.
 * This is a forced action callback.
 *
 * @param   pVM     VM handle.
 * @thread  Emulation thread only.
 */
VMMR3DECL(void) PDMR3QueueFlushAll(PVM pVM)
{
    VM_ASSERT_EMT(pVM);
    LogFlow(("PDMR3QueuesFlush:\n"));

    /*
     * Only let one EMT flushing queues at any one time to queue preserve order
     * and to avoid wasting time. The FF is always cleared here, because it's
     * only used to get someones attention. Queue inserts occuring during the
     * flush are caught using the pending bit.
     *
     * Note. The order in which the FF and pending bit are set and cleared is important.
     */
    VM_FF_CLEAR(pVM, VM_FF_PDM_QUEUES);
    if (!ASMAtomicBitTestAndSet(&pVM->pdm.s.fQueueFlushing, PDM_QUEUE_FLUSH_FLAG_ACTIVE_BIT))
    {
        ASMAtomicBitClear(&pVM->pdm.s.fQueueFlushing, PDM_QUEUE_FLUSH_FLAG_PENDING_BIT);
        do
        {
            VM_FF_CLEAR(pVM, VM_FF_PDM_QUEUES);
            for (PPDMQUEUE pCur = pVM->pdm.s.pQueuesForced; pCur; pCur = pCur->pNext)
                if (    pCur->pPendingR3
                    ||  pCur->pPendingR0
                    ||  pCur->pPendingRC)
                    pdmR3QueueFlush(pCur);
        } while (   ASMAtomicBitTestAndClear(&pVM->pdm.s.fQueueFlushing, PDM_QUEUE_FLUSH_FLAG_PENDING_BIT)
                 || VM_FF_ISPENDING(pVM, VM_FF_PDM_QUEUES));

        ASMAtomicBitClear(&pVM->pdm.s.fQueueFlushing, PDM_QUEUE_FLUSH_FLAG_ACTIVE_BIT);
    }
}


/**
 * Process pending items in one queue.
 *
 * @returns Success indicator.
 *          If false the item the consumer said "enough!".
 * @param   pQueue  The queue.
 */
static bool pdmR3QueueFlush(PPDMQUEUE pQueue)
{
    STAM_PROFILE_START(&pQueue->StatFlushPrf,p);

    /*
     * Get the lists.
     */
    PPDMQUEUEITEMCORE pItems = (PPDMQUEUEITEMCORE)ASMAtomicXchgPtr((void * volatile *)&pQueue->pPendingR3, NULL);
    RTRCPTR           pItemsRC = ASMAtomicXchgRCPtr(&pQueue->pPendingRC, NIL_RTRCPTR);
    RTR0PTR           pItemsR0 = ASMAtomicXchgR0Ptr(&pQueue->pPendingR0, NIL_RTR0PTR);

    if (    !pItems
        &&  !pItemsRC
        &&  !pItemsR0)
        /* Somebody may be racing us ... never mind. */
        return true;

    /*
     * Reverse the list (it's inserted in LIFO order to avoid semaphores, remember).
     */
    PPDMQUEUEITEMCORE pCur = pItems;
    pItems = NULL;
    while (pCur)
    {
        PPDMQUEUEITEMCORE pInsert = pCur;
        pCur = pCur->pNextR3;
        pInsert->pNextR3 = pItems;
        pItems = pInsert;
    }

    /*
     * Do the same for any pending RC items.
     */
    while (pItemsRC)
    {
        PPDMQUEUEITEMCORE pInsert = (PPDMQUEUEITEMCORE)MMHyperRCToR3(pQueue->pVMR3, pItemsRC);
        pItemsRC = pInsert->pNextRC;
        pInsert->pNextRC = NIL_RTRCPTR;
        pInsert->pNextR3 = pItems;
        pItems = pInsert;
    }

    /*
     * Do the same for any pending R0 items.
     */
    while (pItemsR0)
    {
        PPDMQUEUEITEMCORE pInsert = (PPDMQUEUEITEMCORE)MMHyperR0ToR3(pQueue->pVMR3, pItemsR0);
        pItemsR0 = pInsert->pNextR0;
        pInsert->pNextR0 = NIL_RTR0PTR;
        pInsert->pNextR3 = pItems;
        pItems = pInsert;
    }

    /*
     * Feed the items to the consumer function.
     */
    Log2(("pdmR3QueueFlush: pQueue=%p enmType=%d pItems=%p\n", pQueue, pQueue->enmType, pItems));
    switch (pQueue->enmType)
    {
        case PDMQUEUETYPE_DEV:
            while (pItems)
            {
                if (!pQueue->u.Dev.pfnCallback(pQueue->u.Dev.pDevIns, pItems))
                    break;
                pCur = pItems;
                pItems = pItems->pNextR3;
                pdmR3QueueFree(pQueue, pCur);
            }
            break;

        case PDMQUEUETYPE_DRV:
            while (pItems)
            {
                if (!pQueue->u.Drv.pfnCallback(pQueue->u.Drv.pDrvIns, pItems))
                    break;
                pCur = pItems;
                pItems = pItems->pNextR3;
                pdmR3QueueFree(pQueue, pCur);
            }
            break;

        case PDMQUEUETYPE_INTERNAL:
            while (pItems)
            {
                if (!pQueue->u.Int.pfnCallback(pQueue->pVMR3, pItems))
                    break;
                pCur = pItems;
                pItems = pItems->pNextR3;
                pdmR3QueueFree(pQueue, pCur);
            }
            break;

        case PDMQUEUETYPE_EXTERNAL:
            while (pItems)
            {
                if (!pQueue->u.Ext.pfnCallback(pQueue->u.Ext.pvUser, pItems))
                    break;
                pCur = pItems;
                pItems = pItems->pNextR3;
                pdmR3QueueFree(pQueue, pCur);
            }
            break;

        default:
            AssertMsgFailed(("Invalid queue type %d\n", pQueue->enmType));
            break;
    }

    /*
     * Success?
     */
    if (pItems)
    {
        /*
         * Reverse the list.
         */
        pCur = pItems;
        pItems = NULL;
        while (pCur)
        {
            PPDMQUEUEITEMCORE pInsert = pCur;
            pCur = pInsert->pNextR3;
            pInsert->pNextR3 = pItems;
            pItems = pInsert;
        }

        /*
         * Insert the list at the tail of the pending list.
         */
        for (;;)
        {
            if (ASMAtomicCmpXchgPtr((void * volatile *)&pQueue->pPendingR3, pItems, NULL))
                break;
            PPDMQUEUEITEMCORE pPending = (PPDMQUEUEITEMCORE)ASMAtomicXchgPtr((void * volatile *)&pQueue->pPendingR3, NULL);
            if (pPending)
            {
                pCur = pPending;
                while (pCur->pNextR3)
                    pCur = pCur->pNextR3;
                pCur->pNextR3 = pItems;
                pItems = pPending;
            }
        }

        STAM_REL_COUNTER_INC(&pQueue->StatFlushLeftovers);
        STAM_PROFILE_STOP(&pQueue->StatFlushPrf,p);
        return false;
    }

    STAM_PROFILE_STOP(&pQueue->StatFlushPrf,p);
    return true;
}


/**
 * This is a worker function used by PDMQueueFlush to perform the
 * flush in ring-3.
 *
 * The queue which should be flushed is pointed to by either pQueueFlushRC,
 * pQueueFlushR0, or pQueue. This function will flush that queue and recalc
 * the queue FF.
 *
 * @param   pVM     The VM handle.
 * @param   pQueue  The queue to flush. Only used in Ring-3.
 */
VMMR3DECL(void) PDMR3QueueFlushWorker(PVM pVM, PPDMQUEUE pQueue)
{
    Assert(pVM->pdm.s.pQueueFlushR0 || pVM->pdm.s.pQueueFlushRC || pQueue);
    VM_ASSERT_EMT(pVM);

    /** @todo This will clash with PDMR3QueueFlushAll (guest SMP)! */
    Assert(!(pVM->pdm.s.fQueueFlushing & PDM_QUEUE_FLUSH_FLAG_ACTIVE));

    /*
     * Flush the queue.
     */
    if (!pQueue && pVM->pdm.s.pQueueFlushRC)
    {
        pQueue = (PPDMQUEUE)MMHyperRCToR3(pVM, pVM->pdm.s.pQueueFlushRC);
        pVM->pdm.s.pQueueFlushRC = NIL_RTRCPTR;
    }
    else if (!pQueue && pVM->pdm.s.pQueueFlushR0)
    {
        pQueue = (PPDMQUEUE)MMHyperR0ToR3(pVM, pVM->pdm.s.pQueueFlushR0);
        pVM->pdm.s.pQueueFlushR0 = NIL_RTR0PTR;
    }
    Assert(!pVM->pdm.s.pQueueFlushR0 && !pVM->pdm.s.pQueueFlushRC);

    if (    !pQueue
        ||  pdmR3QueueFlush(pQueue))
    {
        /*
         * Recalc the FF (for the queues using force action).
         */
        VM_FF_CLEAR(pVM, VM_FF_PDM_QUEUES);
        for (pQueue = pVM->pdm.s.pQueuesForced; pQueue; pQueue = pQueue->pNext)
            if (    pQueue->pPendingRC
                ||  pQueue->pPendingR0
                ||  pQueue->pPendingR3)
            {
                VM_FF_SET(pVM, VM_FF_PDM_QUEUES);
                break;
            }
    }
}


/**
 * Free an item.
 *
 * @param   pQueue  The queue.
 * @param   pItem   The item.
 */
DECLINLINE(void) pdmR3QueueFree(PPDMQUEUE pQueue, PPDMQUEUEITEMCORE pItem)
{
    VM_ASSERT_EMT(pQueue->pVMR3);

    int i = pQueue->iFreeHead;
    int iNext = (i + 1) % (pQueue->cItems + PDMQUEUE_FREE_SLACK);

    pQueue->aFreeItems[i].pItemR3 = pItem;
    if (pQueue->pVMRC)
    {
        pQueue->aFreeItems[i].pItemRC = MMHyperR3ToRC(pQueue->pVMR3, pItem);
        pQueue->aFreeItems[i].pItemR0 = MMHyperR3ToR0(pQueue->pVMR3, pItem);
    }

    if (!ASMAtomicCmpXchgU32(&pQueue->iFreeHead, iNext, i))
        AssertMsgFailed(("huh? i=%d iNext=%d iFreeHead=%d iFreeTail=%d\n", i, iNext, pQueue->iFreeHead, pQueue->iFreeTail));
    STAM_STATS({ ASMAtomicDecU32(&pQueue->cStatPending); });
}


/**
 * Timer handler for PDM queues.
 * This is called by for a single queue.
 *
 * @param   pVM     VM handle.
 * @param   pTimer  Pointer to timer.
 * @param   pvUser  Pointer to the queue.
 */
static DECLCALLBACK(void) pdmR3QueueTimer(PVM pVM, PTMTIMER pTimer, void *pvUser)
{
    PPDMQUEUE pQueue = (PPDMQUEUE)pvUser;
    Assert(pTimer == pQueue->pTimer); NOREF(pTimer);

    if (    pQueue->pPendingR3
        ||  pQueue->pPendingR0
        ||  pQueue->pPendingRC)
        pdmR3QueueFlush(pQueue);
    int rc = TMTimerSetMillies(pQueue->pTimer, pQueue->cMilliesInterval);
    AssertRC(rc);
}

