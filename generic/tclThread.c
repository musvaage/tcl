/*
 * tclThread.c --
 *
 *	This file implements Platform independent thread operations. Most of
 *	the real work is done in the platform dependent files.
 *
 * Copyright © 1998 Sun Microsystems, Inc.
 * Copyright © 2008 George Peter Staplin
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tclInt.h"

/*
 * There are three classes of synchronization objects: mutexes, thread data
 * keys, and condition variables. The following are used to record the memory
 * used for these objects so they can be finalized.
 *
 * These statics are guarded by the mutex in the caller of
 * TclRememberThreadData, e.g., TclpThreadDataKeyInit
 */

typedef struct {
    int num;			/* Number of objects remembered */
    int max;			/* Max size of the array */
    void **list;		/* List of pointers */
} SyncObjRecord;

static SyncObjRecord keyRecord = {0, 0, NULL};
static SyncObjRecord mutexRecord = {0, 0, NULL};
static SyncObjRecord condRecord = {0, 0, NULL};

/*
 * Prototypes of functions used only in this file.
 */

static void		ForgetSyncObject(void *objPtr, SyncObjRecord *recPtr);
static void		RememberSyncObject(void *objPtr,
			    SyncObjRecord *recPtr);

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetThreadData --
 *
 *	This function allocates and initializes a chunk of thread local
 *	storage.
 *
 * Results:
 *	A thread-specific pointer to the data structure.
 *
 * Side effects:
 *	Will allocate memory the first time this thread calls for this chunk
 *	of storage.
 *
 *----------------------------------------------------------------------
 */

void *
Tcl_GetThreadData(
    Tcl_ThreadDataKey *keyPtr,	/* Identifier for the data chunk */
    Tcl_Size size)		/* Size of storage block */
{
    void *result;
#if TCL_THREADS
    /*
     * Initialize the key for this thread.
     */

    result = TclThreadStorageKeyGet(keyPtr);

    if (result == NULL) {
	result = Tcl_Alloc(size);
	memset(result, 0, size);
	TclThreadStorageKeySet(keyPtr, result);
    }
#else /* TCL_THREADS */
    if (*keyPtr == NULL) {
	result = Tcl_Alloc(size);
	memset(result, 0, size);
	*keyPtr = (Tcl_ThreadDataKey)result;
	RememberSyncObject(keyPtr, &keyRecord);
    } else {
	result = *keyPtr;
    }
#endif /* TCL_THREADS */
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * TclThreadDataKeyGet --
 *
 *	This function returns a pointer to a block of thread local storage.
 *
 * Results:
 *	A thread-specific pointer to the data structure, or NULL if the memory
 *	has not been assigned to this key for this thread.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

void *
TclThreadDataKeyGet(
    Tcl_ThreadDataKey *keyPtr)	/* Identifier for the data chunk. */
{
#if TCL_THREADS
    return TclThreadStorageKeyGet(keyPtr);
#else /* TCL_THREADS */
    return *keyPtr;
#endif /* TCL_THREADS */
}

/*
 *----------------------------------------------------------------------
 *
 * RememberSyncObject
 *
 *	Keep a list of (mutexes/condition variable/data key) used during
 *	finalization.
 *
 *	Assume global lock is held.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Add to the appropriate list.
 *
 *----------------------------------------------------------------------
 */

static void
RememberSyncObject(
    void *objPtr,		/* Pointer to sync object */
    SyncObjRecord *recPtr)	/* Record of sync objects */
{
    void **newList;
    int i, j;

    /*
     * Reuse any free slot in the list.
     */

    for (i=0 ; i < recPtr->num ; ++i) {
	if (recPtr->list[i] == NULL) {
	    recPtr->list[i] = objPtr;
	    return;
	}
    }

    /*
     * Grow the list of pointers if necessary, copying only non-NULL
     * pointers to the new list.
     */

    if (recPtr->num >= recPtr->max) {
	recPtr->max += 8;
	newList = (void **)Tcl_Alloc(recPtr->max * sizeof(void *));
	for (i=0,j=0 ; i<recPtr->num ; i++) {
	    if (recPtr->list[i] != NULL) {
		newList[j++] = recPtr->list[i];
	    }
	}
	if (recPtr->list != NULL) {
	    Tcl_Free(recPtr->list);
	}
	recPtr->list = newList;
	recPtr->num = j;
    }

    recPtr->list[recPtr->num] = objPtr;
    recPtr->num++;
}

/*
 *----------------------------------------------------------------------
 *
 * ForgetSyncObject
 *
 *	Remove a single object from the list.
 *	Assume global lock is held.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Remove from the appropriate list.
 *
 *----------------------------------------------------------------------
 */

static void
ForgetSyncObject(
    void *objPtr,		/* Pointer to sync object */
    SyncObjRecord *recPtr)	/* Record of sync objects */
{
    int i;

    for (i=0 ; i<recPtr->num ; i++) {
	if (objPtr == recPtr->list[i]) {
	    recPtr->list[i] = NULL;
	    return;
	}
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TclRememberMutex
 *
 *	Keep a list of mutexes used during finalization.
 *	Assume global lock is held.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Add to the mutex list.
 *
 *----------------------------------------------------------------------
 */

void
TclRememberMutex(
    Tcl_Mutex *mutexPtr)
{
    RememberSyncObject(mutexPtr, &mutexRecord);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_MutexFinalize --
 *
 *	Finalize a single mutex and remove it from the list of remembered
 *	objects.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Remove the mutex from the list.
 *
 *----------------------------------------------------------------------
 */

#undef Tcl_MutexFinalize
void
Tcl_MutexFinalize(
    Tcl_Mutex *mutexPtr)
{
#if TCL_THREADS
    TclpFinalizeMutex(mutexPtr);
#endif
    TclpGlobalLock();
    ForgetSyncObject(mutexPtr, &mutexRecord);
    TclpGlobalUnlock();
}

/*
 *----------------------------------------------------------------------
 *
 * TclRememberCondition
 *
 *	Keep a list of condition variables used during finalization.
 *	Assume global lock is held.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Add to the condition variable list.
 *
 *----------------------------------------------------------------------
 */

void
TclRememberCondition(
    Tcl_Condition *condPtr)
{
    RememberSyncObject(condPtr, &condRecord);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ConditionFinalize --
 *
 *	Finalize a single condition variable and remove it from the list of
 *	remembered objects.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Remove the condition variable from the list.
 *
 *----------------------------------------------------------------------
 */

#undef Tcl_ConditionFinalize
void
Tcl_ConditionFinalize(
    Tcl_Condition *condPtr)
{
#if TCL_THREADS
    TclpFinalizeCondition(condPtr);
#endif
    TclpGlobalLock();
    ForgetSyncObject(condPtr, &condRecord);
    TclpGlobalUnlock();
}

/*
 *----------------------------------------------------------------------
 *
 * TclFinalizeThreadData --
 *
 *	This function cleans up the thread-local storage. Secondary, it cleans
 *	thread alloc cache.
 *	This is called once for each thread before thread exits.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Frees up all thread local storage.
 *
 *----------------------------------------------------------------------
 */

void
TclFinalizeThreadData(int quick)
{
    TclFinalizeThreadDataThread();
#if TCL_THREADS && defined(USE_THREAD_ALLOC)
    if (!quick) {
	/*
	 * Quick exit principle makes it useless to terminate allocators
	 */
	TclFinalizeThreadAllocThread();
    }
#else
    (void)quick;
#endif
}

/*
 *----------------------------------------------------------------------
 *
 * TclFinalizeSynchronization --
 *
 *	This function cleans up all synchronization objects: mutexes,
 *	condition variables, and thread-local storage.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Frees up the memory.
 *
 *----------------------------------------------------------------------
 */

void
TclFinalizeSynchronization(void)
{
    int i;
    void *blockPtr;
    Tcl_ThreadDataKey *keyPtr;
#if TCL_THREADS
    Tcl_Mutex *mutexPtr;
    Tcl_Condition *condPtr;

    TclpGlobalLock();
#endif

    /*
     * If we're running unthreaded, the TSD blocks are simply stored inside
     * their thread data keys. Free them here.
     */

    if (keyRecord.list != NULL) {
	for (i=0 ; i<keyRecord.num ; i++) {
	    keyPtr = (Tcl_ThreadDataKey *) keyRecord.list[i];
	    blockPtr = *keyPtr;
	    Tcl_Free(blockPtr);
	}
	Tcl_Free(keyRecord.list);
	keyRecord.list = NULL;
    }
    keyRecord.max = 0;
    keyRecord.num = 0;

#if TCL_THREADS
    /*
     * Call thread storage global cleanup.
     */

    TclFinalizeThreadStorage();

    for (i=0 ; i<mutexRecord.num ; i++) {
	mutexPtr = (Tcl_Mutex *)mutexRecord.list[i];
	if (mutexPtr != NULL) {
	    TclpFinalizeMutex(mutexPtr);
	}
    }
    if (mutexRecord.list != NULL) {
	Tcl_Free(mutexRecord.list);
	mutexRecord.list = NULL;
    }
    mutexRecord.max = 0;
    mutexRecord.num = 0;

    for (i=0 ; i<condRecord.num ; i++) {
	condPtr = (Tcl_Condition *) condRecord.list[i];
	if (condPtr != NULL) {
	    TclpFinalizeCondition(condPtr);
	}
    }
    if (condRecord.list != NULL) {
	Tcl_Free(condRecord.list);
	condRecord.list = NULL;
    }
    condRecord.max = 0;
    condRecord.num = 0;

    TclpGlobalUnlock();
#endif /* TCL_THREADS */
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ExitThread --
 *
 *	This function is called to terminate the current thread. This should
 *	be used by extensions that create threads with additional interpreters
 *	in them.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	All thread exit handlers are invoked, then the thread dies.
 *
 *----------------------------------------------------------------------
 */

TCL_NORETURN void
Tcl_ExitThread(
    int status)
{
    Tcl_FinalizeThread();
    TclpThreadExit(status);
}

#if !TCL_THREADS

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ConditionWait, et al. --
 *
 *	These noop functions are provided so the stub table does not have to
 *	be conditionalized for threads. The real implementations of these
 *	functions live in the platform specific files.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

#undef Tcl_ConditionWait
void
Tcl_ConditionWait(
    TCL_UNUSED(Tcl_Condition *),	/* Really (pthread_cond_t **) */
    TCL_UNUSED(Tcl_Mutex *),	/* Really (pthread_mutex_t **) */
    TCL_UNUSED(const Tcl_Time *)) /* Timeout on waiting period */
{
}

#undef Tcl_ConditionNotify
void
Tcl_ConditionNotify(
    TCL_UNUSED(Tcl_Condition *))
{
}

#undef Tcl_MutexLock
void
Tcl_MutexLock(
    TCL_UNUSED(Tcl_Mutex *))
{
}

#undef Tcl_MutexUnlock
void
Tcl_MutexUnlock(
    TCL_UNUSED(Tcl_Mutex *))
{
}
#endif /* !TCL_THREADS */

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
