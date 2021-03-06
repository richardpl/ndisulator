/*-
 * Copyright (c) 2003
 *	Bill Paul <wpaul@windriver.com>.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by Bill Paul.
 * 4. Neither the name of the author nor the names of any co-contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY Bill Paul AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL Bill Paul OR THE VOICES IN HIS HEAD
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/ctype.h>
#include <sys/unistd.h>
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mutex.h>

#include <sys/kdb.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/condvar.h>
#include <sys/kthread.h>
#include <sys/module.h>
#include <sys/smp.h>
#include <sys/sched.h>
#include <sys/queue.h>
#include <sys/taskqueue.h>

#include <machine/_inttypes.h>
#include <machine/atomic.h>
#include <machine/bus.h>
#include <machine/stdarg.h>
#include <machine/resource.h>

#include <sys/bus.h>
#include <sys/rman.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_param.h>
#include <vm/pmap.h>
#include <vm/uma.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>

#include "pe_var.h"
#include "resource_var.h"
#include "ntoskrnl_var.h"
#include "hal_var.h"
#include "ndis_var.h"

struct kdpc_queue {
	struct list_entry	disp;
	struct thread		*td;
	int			exit;
	unsigned long		lock;
	struct nt_kevent	proc;
	struct nt_kevent	done;
};

struct wb_ext {
	struct cv		we_cv;
	struct thread		*we_td;
};

struct ndis_work_item_task {
	struct ndis_work_item *work;
	struct task tq_item;
};

static struct list_entry nt_intlist;

#ifdef __amd64__
struct kuser_shared_data kuser_data;
struct callout update_kuser;
static void ntoskrnl_update_kuser(void *);
#endif

static int32_t RtlAppendUnicodeStringToString(struct unicode_string *,
    const struct unicode_string *);
static int32_t RtlAppendUnicodeToString(struct unicode_string *,
    const uint16_t *);
static uint8_t RtlEqualString(const struct ansi_string *,
    const struct ansi_string *, uint8_t);
static uint8_t RtlEqualUnicodeString(const struct unicode_string *,
    const struct unicode_string *, uint8_t);
static void RtlCopyString(struct ansi_string *, const struct ansi_string *);
static void RtlCopyUnicodeString(struct unicode_string *,
    const struct unicode_string *);
static struct irp *IoBuildSynchronousFsdRequest(uint32_t,
    struct device_object *, void *, uint32_t, uint64_t *, struct nt_kevent *,
    struct io_status_block *);
static struct irp *IoBuildAsynchronousFsdRequest(uint32_t,
    struct device_object *, void *, uint32_t, uint64_t *,
    struct io_status_block *);
static struct irp *IoBuildDeviceIoControlRequest(uint32_t,
    struct device_object *, void *, uint32_t, void *, uint32_t, uint8_t,
    struct nt_kevent *, struct io_status_block *);
static struct irp *IoAllocateIrp(uint8_t, uint8_t);
static void IoReuseIrp(struct irp *, uint32_t);
static uint8_t IoCancelIrp(struct irp *);
static void IoFreeIrp(struct irp *);
static void IoInitializeIrp(struct irp *, uint16_t, uint8_t);
static struct irp *IoMakeAssociatedIrp(struct irp *, uint8_t);
static int32_t KeWaitForMultipleObjects(uint32_t,
    struct nt_dispatcher_header **, uint32_t, uint32_t, uint32_t, uint8_t,
    int64_t *, struct wait_block *);
static void ntoskrnl_waittest(struct nt_dispatcher_header *, uint32_t);
static void ntoskrnl_satisfy_wait(struct nt_dispatcher_header *,
    struct thread *);
static void ntoskrnl_satisfy_multiple_waits(struct wait_block *);
static int ntoskrnl_is_signalled(struct nt_dispatcher_header *,
    struct thread *);
static void ntoskrnl_ascii_to_unicode(char *, uint16_t *, int);
static void ntoskrnl_destroy_dpc_thread(void);
static void ntoskrnl_dpc_thread(void *);
static void ntoskrnl_timercall(void *);
static void ntoskrnl_unicode_to_ascii(uint16_t *, char *, int);
static void run_ndis_work_item(struct ndis_work_item_task *, int);
static void IORunWorkItem(struct io_workitem *iw, int pending);
static uint8_t ntoskrnl_insert_dpc(struct list_entry *, struct nt_kdpc *);
static void WRITE_REGISTER_USHORT(uint16_t *, uint16_t);
static uint16_t READ_REGISTER_USHORT(uint16_t *);
static void WRITE_REGISTER_ULONG(uint32_t *, uint32_t);
static uint32_t READ_REGISTER_ULONG(uint32_t *);
static void WRITE_REGISTER_UCHAR(uint8_t *, uint8_t);
static uint8_t READ_REGISTER_UCHAR(uint8_t *);
static int64_t _allmul(int64_t, int64_t);
static int64_t _alldiv(int64_t, int64_t);
static int64_t _allrem(int64_t, int64_t);
static int64_t _allshr(int64_t, uint8_t);
static int64_t _allshl(int64_t, uint8_t);
static uint64_t _aullmul(uint64_t, uint64_t);
static uint64_t _aulldiv(uint64_t, uint64_t);
static uint64_t _aullrem(uint64_t, uint64_t);
static uint64_t _aullshr(uint64_t, uint8_t);
static uint64_t _aullshl(uint64_t, uint8_t);
static struct slist_entry *ntoskrnl_pushsl(union slist_header *,
    struct slist_entry *);
static struct slist_entry *ntoskrnl_popsl(union slist_header *);
static void *ExAllocatePoolWithTag(uint32_t, size_t, uint32_t);
static void ExFreePoolWithTag(void *, uint32_t);
static void ExInitializeNPagedLookasideList(struct npaged_lookaside_list *,
    lookaside_alloc_func *, lookaside_free_func *, uint32_t, size_t, uint32_t,
    uint16_t);
static void ExDeleteNPagedLookasideList(struct npaged_lookaside_list *);
static struct slist_entry *ExInterlockedPushEntrySList(union slist_header *,
    struct slist_entry *, unsigned long *);
static struct slist_entry *ExInterlockedPopEntrySList(union slist_header *,
    unsigned long *);
static void InitializeSListHead(union slist_header *);
static int32_t InterlockedIncrement(volatile int32_t *);
static int32_t InterlockedDecrement(volatile int32_t *);
static void ExInterlockedAddLargeStatistic(uint64_t *, uint32_t);
static void *MmAllocateContiguousMemory(uint32_t, uint64_t);
static void *MmAllocateContiguousMemorySpecifyCache(uint32_t, uint64_t,
    uint64_t, uint64_t, enum memory_caching_type);
static void MmFreeContiguousMemory(void *);
static void MmFreeContiguousMemorySpecifyCache(void *, uint32_t,
    enum memory_caching_type);
static uint64_t MmGetPhysicalAddress(void *);
static void *MmGetSystemRoutineAddress(struct unicode_string *);
static uint8_t MmIsAddressValid(void *);
static uint32_t MmSizeOfMdl(void *, size_t);
static void *MmMapLockedPages(struct mdl *, uint8_t);
static void *MmMapLockedPagesSpecifyCache(struct mdl *, uint8_t,
    enum memory_caching_type, void *, uint32_t, uint32_t);
static void * MmMapIoSpace(uint64_t, uint32_t, enum memory_caching_type);
static void MmUnmapIoSpace(void *, size_t);
static void MmUnmapLockedPages(void *, struct mdl *);
static device_t ntoskrnl_finddev(device_t, uint64_t, struct resource **);
static uint32_t RtlxAnsiStringToUnicodeSize(const struct ansi_string *);
static uint32_t RtlxUnicodeStringToAnsiSize(const struct unicode_string *);
static void RtlZeroMemory(void *, size_t);
static void RtlSecureZeroMemory(void *, size_t);
static void RtlFillMemory(void *, size_t, uint8_t);
static void RtlMoveMemory(void *, const void *, size_t);
static int32_t RtlCharToInteger(const char *, uint32_t, uint32_t *);
static void RtlCopyMemory(void *, const void *, size_t);
static size_t RtlCompareMemory(const void *, const void *, size_t);
static int32_t RtlCompareString(const struct ansi_string *,
    const struct ansi_string *, uint8_t);
static int32_t RtlCompareUnicodeString(const struct unicode_string *,
    const struct unicode_string *, uint8_t);
static int32_t RtlUnicodeStringToInteger(const struct unicode_string *,
    uint32_t, uint32_t *);
static int atoi(const char *);
static long atol(const char *);
static int rand(void);
static void srand(unsigned int);
static uint16_t *wcscat(uint16_t *, const uint16_t *);
static int wcscmp(const uint16_t *, const uint16_t *);
static uint16_t *wcscpy(uint16_t *, const uint16_t *);
static int wcsicmp(const uint16_t *, const uint16_t *);
static size_t wcslen(const uint16_t *);
static uint16_t *wcsncpy(uint16_t *, const uint16_t *, size_t);
static unsigned long KeQueryActiveProcessors(void);
static uint64_t KeQueryInterruptTime(void);
static void KeQuerySystemTime(int64_t *);
static void KeQueryTickCount(int64_t *);
static uint32_t KeQueryTimeIncrement(void);
static uint32_t KeTickCount(void);
static uint8_t IoIsWdmVersionAvailable(uint8_t, uint8_t);
static int32_t IoOpenDeviceRegistryKey(struct device_object *, uint32_t,
    uint32_t, void **);
static void ntoskrnl_thrfunc(void *);
static int32_t PsCreateSystemThread(void **, uint32_t, void *, void *, void *,
    void *, void *);
static int32_t PsTerminateSystemThread(int32_t);
static int32_t IoGetDeviceObjectPointer(struct unicode_string *, uint32_t,
    void *, struct device_object *);
static int32_t IoGetDeviceProperty(struct device_object *,
    enum device_registry_property, uint32_t, void *, uint32_t *);
static void KeInitializeMutex(struct nt_kmutex *, uint32_t);
static int32_t KeReleaseMutex(struct nt_kmutex *, uint8_t);
static int32_t KeReadStateMutex(struct nt_kmutex *);
static int32_t ObReferenceObjectByHandle(void *, uint32_t, void *, uint8_t,
    void **, void **);
static void ObfDereferenceObject(void *);
static int32_t ZwClose(void *);
static int32_t ZwCreateFile(void **, uint32_t, struct object_attributes *,
    struct io_status_block *, int64_t *, uint32_t, uint32_t, uint32_t,
    uint32_t, void *, uint32_t);
static int32_t ZwCreateKey(void **, uint32_t, struct object_attributes *,
    uint32_t, struct unicode_string *, uint32_t, uint32_t *);
static int32_t ZwDeleteKey(void *);
static int32_t ZwOpenFile(void **, uint32_t, struct object_attributes *,
    struct io_status_block *, uint32_t, uint32_t);
static int32_t ZwOpenKey(void **, uint32_t, struct object_attributes *);
static int32_t ZwReadFile(void *, struct nt_kevent *, void *, void *,
    struct io_status_block *, void *, uint32_t, int64_t *, uint32_t *);
static int32_t ZwWriteFile(void *, struct nt_kevent *, void *, void *,
    struct io_status_block *, void *, uint32_t, int64_t *, uint32_t *);
static int32_t WmiQueryTraceInformation(uint32_t, void *, uint32_t, uint32_t,
    void *);
static int32_t WmiTraceMessage(uint64_t, uint32_t, void *, uint16_t, ...);
static int32_t IoWMIRegistrationControl(struct device_object *, uint32_t);
static int32_t IoWMIQueryAllData(void *, uint32_t *, void *);
static int32_t IoWMIOpenBlock(void *, uint32_t, void **);
static int32_t IoUnregisterPlugPlayNotification(void *);
static void *ntoskrnl_memchr(void *, unsigned char, size_t);
static char *ntoskrnl_strncat(char *, const char *, size_t);
static int ntoskrnl_toupper(int);
static int ntoskrnl_tolower(int);
static funcptr ntoskrnl_findwrap(void *);
static int32_t DbgPrint(const char *, ...);
static void DbgBreakPoint(void);
static void KeClearEvent(struct nt_kevent *);
static int32_t KeReadStateEvent(struct nt_kevent *);
static void KeBugCheck(uint32_t);
static void KeBugCheckEx(uint32_t, unsigned long, unsigned long, unsigned long,
    unsigned long);
static uint32_t KeGetCurrentProcessorNumber(void);
static struct thread * KeGetCurrentThread(void);
static uint8_t KeReadStateTimer(struct nt_ktimer *);
static int32_t KeDelayExecutionThread(uint8_t, uint8_t, int64_t *);
static int32_t KeSetPriorityThread(struct thread *, int32_t);
static int32_t KeQueryPriorityThread(struct thread *);
static void KeInitializeSemaphore(struct nt_ksemaphore *, int32_t, int32_t);
static int32_t KeReleaseSemaphore(struct nt_ksemaphore *, int32_t, int32_t,
    uint8_t);
static int32_t KeReadStateSemaphore(struct nt_ksemaphore *);
static void dummy(void);

static funcptr ExFreePool_wrap;
static funcptr ExAllocatePoolWithTag_wrap;
static struct proc *ndisproc;
static struct mtx nt_dispatchlock;
static struct mtx nt_interlock;
static unsigned long nt_cancellock;
static unsigned long nt_intlock;
static uint8_t ntoskrnl_kth;
static struct nt_objref_head nt_reflist;
static uma_zone_t mdl_zone;
static uma_zone_t iw_zone;
static struct kdpc_queue *kq_queue;
static struct taskqueue *nq_queue;
static struct taskqueue *wq_queue;

MALLOC_DEFINE(M_NDIS_NTOSKRNL, "ndis_ntoskrnl", "ndis_ntoskrnl buffers");

void
ntoskrnl_libinit(void)
{
	struct thread *t;

	mtx_init(&nt_dispatchlock, "dispatchlock", NULL, MTX_DEF | MTX_RECURSE);
	mtx_init(&nt_interlock, "interlock", NULL, MTX_SPIN);
	KeInitializeSpinLock(&nt_cancellock);
	KeInitializeSpinLock(&nt_intlock);
	TAILQ_INIT(&nt_reflist);

	InitializeListHead(&nt_intlist);

	kq_queue = ExAllocatePool(sizeof(struct kdpc_queue));
	if (kq_queue == NULL)
		panic("failed to allocate kq_queue");

	InitializeListHead(&kq_queue->disp);
	KeInitializeSpinLock(&kq_queue->lock);
	KeInitializeEvent(&kq_queue->proc, SYNCHRONIZATION_EVENT, FALSE);
	KeInitializeEvent(&kq_queue->done, SYNCHRONIZATION_EVENT, FALSE);
	if (kproc_kthread_add(ntoskrnl_dpc_thread, kq_queue, &ndisproc,
	    &t, RFHIGHPID, NDIS_KSTACK_PAGES, "ndis", "dpc"))
		panic("failed to launch dpc thread");

	if ((nq_queue = taskqueue_create("ndis queue", M_WAITOK,
	    taskqueue_thread_enqueue, &nq_queue)) == NULL)
		panic("failed to allocate taskqueue for nq_queue");
	taskqueue_start_threads(&nq_queue, 1, PRI_MIN_KERN + 20,
	    "ndis nq_queue");

	if ((wq_queue = taskqueue_create("work queue", M_WAITOK,
	    taskqueue_thread_enqueue, &wq_queue)) == NULL)
		panic("failed to allocate taskqueue for wq_queue");
	taskqueue_start_threads(&wq_queue, 1, PRI_MIN_KERN + 20,
	    "ndis wq_queue");

	windrv_wrap_table(ntoskrnl_functbl);
	ExAllocatePoolWithTag_wrap = ntoskrnl_findwrap(ExAllocatePoolWithTag);
	ExFreePool_wrap = ntoskrnl_findwrap(ExFreePool);

	/*
	 * MDLs are supposed to be variable size (they describe
	 * buffers containing some number of pages, but we don't
	 * know ahead of time how many pages that will be). But
	 * always allocating them off the heap is very slow. As
	 * a compromise, we create an MDL UMA zone big enough to
	 * handle any buffer requiring up to 16 pages, and we
	 * use those for any MDLs for buffers of 16 pages or less
	 * in size. For buffers larger than that (which we assume
	 * will be few and far between, we allocate the MDLs off
	 * the heap.
	 */
	mdl_zone = uma_zcreate("Windows MDL", MDL_ZONE_SIZE,
	    NULL, NULL, NULL, NULL, UMA_ALIGN_PTR, 0);

	iw_zone = uma_zcreate("Windows WorkItem", sizeof(struct io_workitem),
	    NULL, NULL, NULL, NULL, UMA_ALIGN_PTR, 0);

#ifdef notdef
	callout_init(&update_kuser, CALLOUT_MPSAFE);
	callout_reset(&update_kuser, hz / 40, ntoskrnl_update_kuser, 0);
#endif
}

void
ntoskrnl_libfini(void)
{
	windrv_unwrap_table(ntoskrnl_functbl);

	ntoskrnl_destroy_dpc_thread();

	taskqueue_free(wq_queue);
	taskqueue_free(nq_queue);
	ExFreePool(kq_queue);

	uma_zdestroy(mdl_zone);
	uma_zdestroy(iw_zone);

	mtx_destroy(&nt_dispatchlock);
	mtx_destroy(&nt_interlock);
#ifdef notdef
	callout_drain(&update_kuser);
#endif
}

#ifdef __amd64__
static void
ntoskrnl_update_kuser(void *unused)
{
	ntoskrnl_time(&kuser_data.system_time);
	KeQueryTickCount(&kuser_data.tick.tick_count_quad);
	*((uint64_t *)&kuser_data.interrupt_time) = KeQueryInterruptTime();
	callout_reset(&update_kuser, hz / 40, ntoskrnl_update_kuser, 0);
}
#endif

static void *
ntoskrnl_memchr(void *buf, unsigned char ch, size_t len)
{
	if (len != 0) {
		unsigned char *p = buf;

		do {
			if (*p++ == ch)
				return (p - 1);
		} while (--len != 0);
	}

	return (NULL);
}

/* Taken from libc */
static char *
ntoskrnl_strncat(char *dst, const char *src, size_t n)
{
	if (n != 0) {
		char *d = dst;
		const char *s = src;

		while (*d != 0)
			d++;
		do {
			if ((*d = *s++) == 0)
				break;
			d++;
		} while (--n != 0);
		*d = 0;
	}

	return (dst);
}

static int
ntoskrnl_toupper(int c)
{
	return (toupper(c));
}

static int
ntoskrnl_tolower(int c)
{
	return (tolower(c));
}

static uint8_t
RtlEqualString(const struct ansi_string *str1, const struct ansi_string *str2,
    uint8_t case_in_sensitive)
{
	if (str1->len != str2->len)
		return (FALSE);
	return (!RtlCompareString(str1, str2, case_in_sensitive));
}

static uint8_t
RtlEqualUnicodeString(const struct unicode_string *str1,
    const struct unicode_string *str2, uint8_t case_in_sensitive)
{
	if (str1->len != str2->len)
		return (FALSE);
	return (!RtlCompareUnicodeString(str1, str2, case_in_sensitive));
}

static void
RtlCopyString(struct ansi_string *dst, const struct ansi_string *src)
{
	TRACE(NDBG_RTL, "dst %p src %p\n", dst, src);
	if (src != NULL && src->buf != NULL && dst->buf != NULL) {
		dst->len = min(src->len, dst->maxlen);
		memcpy(dst->buf, src->buf, dst->len);
		if (dst->len < dst->maxlen)
			dst->buf[dst->len] = 0;
	} else
		dst->len = 0;
}

static void
RtlCopyUnicodeString(struct unicode_string *dst,
    const struct unicode_string *src)
{
	TRACE(NDBG_RTL, "dst %p src %p\n", dst, src);
	if (src != NULL && src->buf != NULL && dst->buf != NULL) {
		dst->len = min(src->len, dst->maxlen);
		memcpy(dst->buf, src->buf, dst->len);
		if (dst->len < dst->maxlen)
			dst->buf[dst->len / sizeof(dst->buf[0])] = 0;
	} else
		dst->len = 0;
}

static void
ntoskrnl_ascii_to_unicode(char *ascii, uint16_t *unicode, int len)
{
	int i;
	uint16_t *ustr;

	ustr = unicode;
	for (i = 0; i < len; i++) {
		*ustr = (uint16_t)ascii[i];
		ustr++;
	}
}

static void
ntoskrnl_unicode_to_ascii(uint16_t *unicode, char *ascii, int len)
{
	int i;
	uint8_t *astr;

	astr = ascii;
	for (i = 0; i < len / 2; i++) {
		*astr = (uint8_t)unicode[i];
		astr++;
	}
}

int32_t
RtlUnicodeStringToAnsiString(struct ansi_string *dst,
    const struct unicode_string *src, uint8_t allocate)
{
	TRACE(NDBG_RTL, "dst %p src %p allocate %u\n", dst, src, allocate);
	if (dst == NULL || src == NULL)
		return (NDIS_STATUS_INVALID_PARAMETER);

	dst->len = src->len / 2;
	if (dst->maxlen < dst->len)
		dst->len = dst->maxlen;

	if (allocate == TRUE) {
		dst->buf = ExAllocatePool((src->len / 2) + 1);
		if (dst->buf == NULL)
			return (NDIS_STATUS_RESOURCES);
		dst->len = dst->maxlen = src->len / 2;
	} else {
		dst->len = src->len / 2; /* XXX */
		if (dst->maxlen < dst->len)
			dst->len = dst->maxlen;
	}
	ntoskrnl_unicode_to_ascii(src->buf, dst->buf, dst->len * 2);

	return (NDIS_STATUS_SUCCESS);
}

int32_t
RtlAnsiStringToUnicodeString(struct unicode_string *dst,
    const struct ansi_string *src, uint8_t allocate)
{
	TRACE(NDBG_RTL, "dst %p src %p allocate %u\n", dst, src, allocate);
	if (dst == NULL || src == NULL)
		return (NDIS_STATUS_INVALID_PARAMETER);

	if (allocate == TRUE) {
		dst->buf = ExAllocatePool(src->len * 2);
		if (dst->buf == NULL)
			return (NDIS_STATUS_RESOURCES);
		dst->len = dst->maxlen = strlen(src->buf) * 2;
	} else {
		dst->len = src->len * 2; /* XXX */
		if (dst->maxlen < dst->len)
			dst->len = dst->maxlen;
	}
	ntoskrnl_ascii_to_unicode(src->buf, dst->buf, dst->len / 2);

	return (NDIS_STATUS_SUCCESS);
}

void *
ExAllocatePool(size_t len)
{
	void *buf;

	buf = malloc(len, M_NDIS_NTOSKRNL, M_NOWAIT|M_ZERO);

	return (buf);
}

static void *
ExAllocatePoolWithTag(enum pool_type pooltype, size_t len, uint32_t tag)
{
	return (ExAllocatePool(len));
}

void
ExFreePool(void *buf)
{
	free(buf, M_NDIS_NTOSKRNL);
}

static void
ExFreePoolWithTag(void *buf, uint32_t tag)
{
	ExFreePool(buf);
}

int32_t
IoAllocateDriverObjectExtension(struct driver_object *drv, void *clid,
    uint32_t extlen, void **ext)
{
	struct custom_extension *ce;

	ce = ExAllocatePool(sizeof(struct custom_extension) + extlen);
	if (ce == NULL)
		return (NDIS_STATUS_RESOURCES);

	ce->ce_clid = clid;
	InsertTailList(&drv->driver_extension->usrext, &ce->ce_list);

	*ext = (void *)(ce + 1);

	return (NDIS_STATUS_SUCCESS);
}

void *
IoGetDriverObjectExtension(struct driver_object *drv, void *clid)
{
	struct list_entry *e;
	struct custom_extension *ce;

	/*
	 * Sanity check. Our dummy bus drivers don't have
	 * any driver extentions.
	 */
	if (drv->driver_extension == NULL)
		return (NULL);

	e = drv->driver_extension->usrext.flink;
	while (e != &drv->driver_extension->usrext) {
		ce = (struct custom_extension *)e;
		if (ce->ce_clid == clid)
			return ((void *)(ce + 1));
		e = e->flink;
	}

	return (NULL);
}


int32_t
IoCreateDevice(struct driver_object *drv, uint32_t devextlen,
    struct unicode_string *devname, enum device_type devtype,
    uint32_t devchars, uint8_t exclusive, struct device_object **newdev)
{
	struct device_object *dev;

	TRACE(NDBG_IO, "drv %p devtype %d\n", drv, devtype);

	dev = ExAllocatePool(sizeof(struct device_object));
	if (dev == NULL)
		return (NDIS_STATUS_RESOURCES);

	dev->type = devtype;
	dev->drvobj = drv;
	dev->currirp = NULL;
	dev->flags = 0;

	if (devextlen) {
		dev->devext = ExAllocatePool(devextlen);
		if (dev->devext == NULL) {
			ExFreePool(dev);
			return (NDIS_STATUS_RESOURCES);
		}
	} else
		dev->devext = NULL;

	dev->size = sizeof(struct device_object) + devextlen;
	dev->refcnt = 1;
	dev->attacheddev = NULL;
	dev->nextdev = NULL;
	dev->stacksize = 1;
	dev->alignreq = 1;
	dev->characteristics = devchars;
	dev->iotimer = NULL;
	KeInitializeEvent(&dev->devlock, SYNCHRONIZATION_EVENT, TRUE);

	/*
	 * Vpd is used for disk/tape devices,
	 * but we don't support those. (Yet.)
	 */
	dev->vpb = NULL;

	dev->devobj_ext = ExAllocatePool(sizeof(struct devobj_extension));
	if (dev->devobj_ext == NULL) {
		if (dev->devext != NULL)
			ExFreePool(dev->devext);
		ExFreePool(dev);
		return (NDIS_STATUS_RESOURCES);
	}

	dev->devobj_ext->type = 0;
	dev->devobj_ext->size = sizeof(struct devobj_extension);
	dev->devobj_ext->devobj = dev;

	/*
	 * Attach this device to the driver object's list
	 * of devices. Note: this is not the same as attaching
	 * the device to the device stack. The driver's AddDevice
	 * routine must explicitly call IoAddDeviceToDeviceStack()
	 * to do that.
	 */
	dev->nextdev = drv->device_object;
	drv->device_object = dev;
	*newdev = dev;

	return (NDIS_STATUS_SUCCESS);
}

void
IoDeleteDevice(struct device_object *dev)
{
	struct device_object *prev;

	if (dev == NULL)
		return;
	if (dev->devobj_ext != NULL)
		ExFreePool(dev->devobj_ext);
	if (dev->devext != NULL)
		ExFreePool(dev->devext);

	/* Unlink the device from the driver's device list. */
	prev = dev->drvobj->device_object;
	if (prev == dev)
		dev->drvobj->device_object = dev->nextdev;
	else {
		while (prev->nextdev != dev)
			prev = prev->nextdev;
		prev->nextdev = dev->nextdev;
	}

	ExFreePool(dev);
}

struct device_object *
IoGetAttachedDevice(struct device_object *dev)
{
	struct device_object *d = dev;

	if (d == NULL)
		return (NULL);
	for (; d->attacheddev != NULL; d = d->attacheddev);
	return (d);
}

static struct irp *
IoBuildSynchronousFsdRequest(uint32_t func, struct device_object *dobj,
    void *buf, uint32_t len, uint64_t *off, struct nt_kevent *event,
    struct io_status_block *status)
{
	struct irp *ip;

	ip = IoBuildAsynchronousFsdRequest(func, dobj, buf, len, off, status);
	if (ip == NULL)
		return (NULL);
	ip->usrevent = event;

	return (ip);
}

static struct irp *
IoBuildAsynchronousFsdRequest(uint32_t func, struct device_object *dobj,
    void *buf, uint32_t len, uint64_t *off, struct io_status_block *status)
{
	struct irp *ip;
	struct io_stack_location *sl;

	ip = IoAllocateIrp(dobj->stacksize, TRUE);
	if (ip == NULL)
		return (NULL);

	ip->usriostat = status;
	ip->tail.overlay.thread = NULL;

	sl = IoGetNextIrpStackLocation(ip);
	sl->major = func;
	sl->minor = 0;
	sl->flags = 0;
	sl->ctl = 0;
	sl->devobj = dobj;
	sl->fileobj = NULL;
	sl->completionfunc = NULL;

	ip->userbuf = buf;

	if (dobj->flags & DO_BUFFERED_IO) {
		ip->assoc.sysbuf = ExAllocatePool(len);
		if (ip->assoc.sysbuf == NULL) {
			IoFreeIrp(ip);
			return (NULL);
		}
		bcopy(buf, ip->assoc.sysbuf, len);
	}

	if (dobj->flags & DO_DIRECT_IO) {
		ip->mdl = IoAllocateMdl(buf, len, FALSE, FALSE, ip);
		if (ip->mdl == NULL) {
			if (ip->assoc.sysbuf != NULL)
				ExFreePool(ip->assoc.sysbuf);
			IoFreeIrp(ip);
			return (NULL);
		}
		ip->userbuf = NULL;
		ip->assoc.sysbuf = NULL;
	}

	if (func == IRP_MJ_READ) {
		sl->parameters.read.len = len;
		if (off != NULL)
			sl->parameters.read.byteoff = *off;
		else
			sl->parameters.read.byteoff = 0;
	}

	if (func == IRP_MJ_WRITE) {
		sl->parameters.write.len = len;
		if (off != NULL)
			sl->parameters.write.byteoff = *off;
		else
			sl->parameters.write.byteoff = 0;
	}

	return (ip);
}

static struct irp *
IoBuildDeviceIoControlRequest(uint32_t iocode, struct device_object *dobj,
    void *ibuf, uint32_t ilen, void *obuf, uint32_t olen, uint8_t isinternal,
    struct nt_kevent *event, struct io_status_block *status)
{
	struct irp *ip;
	struct io_stack_location *sl;
	uint32_t buflen;

	ip = IoAllocateIrp(dobj->stacksize, TRUE);
	if (ip == NULL)
		return (NULL);
	ip->usrevent = event;
	ip->usriostat = status;
	ip->tail.overlay.thread = NULL;

	sl = IoGetNextIrpStackLocation(ip);
	sl->major = isinternal == TRUE ?
	    IRP_MJ_INTERNAL_DEVICE_CONTROL : IRP_MJ_DEVICE_CONTROL;
	sl->minor = 0;
	sl->flags = 0;
	sl->ctl = 0;
	sl->devobj = dobj;
	sl->fileobj = NULL;
	sl->completionfunc = NULL;
	sl->parameters.ioctl.iocode = iocode;
	sl->parameters.ioctl.ibuflen = ilen;
	sl->parameters.ioctl.obuflen = olen;

	switch (IO_METHOD(iocode)) {
	case METHOD_BUFFERED:
		if (ilen > olen)
			buflen = ilen;
		else
			buflen = olen;
		if (buflen) {
			ip->assoc.sysbuf = ExAllocatePool(buflen);
			if (ip->assoc.sysbuf == NULL) {
				IoFreeIrp(ip);
				return (NULL);
			}
		}
		if (ilen && ibuf != NULL)
			bcopy(ibuf, ip->assoc.sysbuf, ilen);
		ip->userbuf = obuf;
		break;
	case METHOD_IN_DIRECT:
	case METHOD_OUT_DIRECT:
		if (ilen && ibuf != NULL) {
			ip->assoc.sysbuf = ExAllocatePool(ilen);
			if (ip->assoc.sysbuf == NULL) {
				IoFreeIrp(ip);
				return (NULL);
			}
			bcopy(ibuf, ip->assoc.sysbuf, ilen);
		}
		if (olen && obuf != NULL) {
			ip->mdl = IoAllocateMdl(obuf, olen, FALSE, FALSE, ip);
			/*
			 * Normally we would MmProbeAndLockPages()
			 * here, but we don't have to in our
			 * imlementation.
			 */
		}
		break;
	case METHOD_NEITHER:
		ip->userbuf = obuf;
		sl->parameters.ioctl.type3ibuf = ibuf;
		break;
	default:
		break;
	}
	/*
	 * Ideally, we should associate this IRP with the calling
	 * thread here.
	 */
	return (ip);
}

static struct irp *
IoAllocateIrp(uint8_t stsize, uint8_t chargequota)
{
	struct irp *i;

	i = ExAllocatePool(IoSizeOfIrp(stsize));
	if (i == NULL)
		return (NULL);
	IoInitializeIrp(i, IoSizeOfIrp(stsize), stsize);

	return (i);
}

static struct irp *
IoMakeAssociatedIrp(struct irp *ip, uint8_t stsize)
{
	struct irp *associrp;

	associrp = IoAllocateIrp(stsize, FALSE);
	if (associrp == NULL)
		return (NULL);

	mtx_lock(&nt_dispatchlock);
	associrp->flags |= IRP_ASSOCIATED_IRP;
	associrp->tail.overlay.thread = ip->tail.overlay.thread;
	associrp->assoc.master = ip;
	mtx_unlock(&nt_dispatchlock);

	return (associrp);
}

static void
IoFreeIrp(struct irp *ip)
{
	ExFreePool(ip);
}

static void
IoInitializeIrp(struct irp *io, uint16_t psize, uint8_t ssize)
{
	bzero((char *)io, IoSizeOfIrp(ssize));
	io->size = psize;
	io->stackcnt = ssize;
	io->currentstackloc = ssize;
	InitializeListHead(&io->thlist);
	io->tail.overlay.s2.u2.csl =
	    (struct io_stack_location *)(io + 1) + ssize;
}

static void
IoReuseIrp(struct irp *ip, uint32_t status)
{
	uint8_t allocflags;

	allocflags = ip->allocflags;
	IoInitializeIrp(ip, ip->size, ip->stackcnt);
	ip->iostat.u.status = status;
	ip->allocflags = allocflags;
}

void
IoAcquireCancelSpinLock(uint8_t *irql)
{
	KeAcquireSpinLock(&nt_cancellock, irql);
}

void
IoReleaseCancelSpinLock(uint8_t irql)
{
	KeReleaseSpinLock(&nt_cancellock, irql);
}

static uint8_t
IoCancelIrp(struct irp *ip)
{
	cancel_func cfunc;
	uint8_t cancelirql;

	IoAcquireCancelSpinLock(&cancelirql);
	cfunc = IoSetCancelRoutine(ip, NULL);
	ip->cancel = TRUE;
	if (cfunc == NULL) {
		IoReleaseCancelSpinLock(cancelirql);
		return (FALSE);
	}
	ip->cancelirql = cancelirql;
	MSCALL2(cfunc, IoGetCurrentIrpStackLocation(ip)->devobj, ip);

	return (uint8_t)IoSetCancelValue(ip, TRUE);
}

int32_t
IofCallDriver(struct device_object *dobj, struct irp *ip)
{
	struct io_stack_location *sl;
	int32_t status;
	driver_dispatch disp;

	KASSERT(ip != NULL, ("no irp"));
	KASSERT(dobj != NULL, ("no device object"));
	KASSERT(dobj->drvobj != NULL, ("no driver object"));

	if (ip->currentstackloc <= 0)
		return (NDIS_STATUS_INVALID_PARAMETER);

	IoSetNextIrpStackLocation(ip);
	sl = IoGetCurrentIrpStackLocation(ip);

	sl->devobj = dobj;

	disp = dobj->drvobj->dispatch[sl->major];
	status = MSCALL2(disp, dobj, ip);

	return (status);
}

void
IofCompleteRequest(struct irp *ip, uint8_t prioboost)
{
	struct device_object *dobj;
	struct io_stack_location *sl;
	completion_func cf;

	KASSERT(ip->iostat.u.status != NDIS_STATUS_PENDING,
	    ("incorrect IRP(%p) status (NDIS_STATUS_PENDING)", ip));

	sl = IoGetCurrentIrpStackLocation(ip);
	IoSkipCurrentIrpStackLocation(ip);

	do {
		if (sl->ctl & SL_PENDING_RETURNED)
			ip->pendingreturned = TRUE;

		if (ip->currentstackloc != (ip->stackcnt + 1))
			dobj = IoGetCurrentIrpStackLocation(ip)->devobj;
		else
			dobj = NULL;

		if (sl->completionfunc != NULL &&
		    ((ip->iostat.u.status == NDIS_STATUS_SUCCESS &&
		    sl->ctl & SL_INVOKE_ON_SUCCESS) ||
		    (ip->iostat.u.status != NDIS_STATUS_SUCCESS &&
		    sl->ctl & SL_INVOKE_ON_ERROR) ||
		    (ip->cancel == TRUE &&
		    sl->ctl & SL_INVOKE_ON_CANCEL))) {
			cf = sl->completionfunc;
			if (MSCALL3(cf, dobj, ip, sl->completionctx) ==
			    NDIS_STATUS_MORE_PROCESSING_REQUIRED)
				return;
		} else {
			if ((ip->currentstackloc <= ip->stackcnt) &&
			    (ip->pendingreturned == TRUE))
				IoMarkIrpPending(ip);
		}
		/* move to the next.  */
		IoSkipCurrentIrpStackLocation(ip);
		sl++;
	} while (ip->currentstackloc <= (ip->stackcnt + 1));

	if (ip->usriostat != NULL)
		*ip->usriostat = ip->iostat;
	if (ip->usrevent != NULL)
		KeSetEvent(ip->usrevent, prioboost, FALSE);

	/* Handle any associated IRPs. */
	if (ip->flags & IRP_ASSOCIATED_IRP) {
		uint32_t masterirpcnt;
		struct irp *masterirp;
		struct mdl *m;

		masterirp = ip->assoc.master;
		masterirpcnt = InterlockedDecrement(&masterirp->assoc.irpcnt);

		while ((m = ip->mdl) != NULL) {
			ip->mdl = m->next;
			IoFreeMdl(m);
		}
		IoFreeIrp(ip);
		if (masterirpcnt == 0)
			IoCompleteRequest(masterirp, IO_NO_INCREMENT);
		return;
	}
	/* With any luck, these conditions will never arise. */
	if (ip->flags & IRP_PAGING_IO) {
		if (ip->mdl != NULL)
			IoFreeMdl(ip->mdl);
		IoFreeIrp(ip);
	}
}

void
ntoskrnl_intr(void *arg)
{
	struct nt_kinterrupt *iobj;
	uint8_t irql;
	uint8_t claimed;
	struct list_entry *l;

	KeAcquireSpinLock(&nt_intlock, &irql);
	for (l = nt_intlist.flink; l != &nt_intlist; l = l->flink) {
		iobj = CONTAINING_RECORD(l, struct nt_kinterrupt, list);
		claimed = MSCALL2(iobj->func, iobj, iobj->ctx);
		if (claimed == TRUE)
			break;
	}
	KeReleaseSpinLock(&nt_intlock, irql);
}

uint8_t
KeAcquireInterruptSpinLock(struct nt_kinterrupt *iobj)
{
	uint8_t irql;

	KeAcquireSpinLock(iobj->lock, &irql);

	return (irql);
}

void
KeReleaseInterruptSpinLock(struct nt_kinterrupt *iobj, uint8_t irql)
{
	KeReleaseSpinLock(iobj->lock, irql);
}

uint8_t
KeSynchronizeExecution(struct nt_kinterrupt *iobj, synchronize_func func,
    void *ctx)
{
	uint8_t irql, rval;

	KASSERT(iobj != NULL, ("no iobj"));
	KASSERT(iobj->lock != NULL, ("no lock"));
	KASSERT(func != NULL, ("no func"));
	KASSERT(ctx != NULL, ("no ctx"));
	KeAcquireSpinLock(iobj->lock, &irql);
	rval = MSCALL1(func, ctx);
	KeReleaseSpinLock(iobj->lock, irql);

	return (rval);
}

/*
 * IoConnectInterrupt() is passed only the interrupt vector and
 * irql that a device wants to use, but no device-specific tag
 * of any kind. This conflicts rather badly with FreeBSD's
 * bus_setup_intr(), which needs the device_t for the device
 * requesting interrupt delivery. In order to bypass this
 * inconsistency, we implement a second level of interrupt
 * dispatching on top of bus_setup_intr(). All devices use
 * ntoskrnl_intr() as their ISR, and any device requesting
 * interrupts will be registered with ntoskrnl_intr()'s interrupt
 * dispatch list. When an interrupt arrives, we walk the list
 * and invoke all the registered ISRs. This effectively makes all
 * interrupts shared, but it's the only way to duplicate the
 * semantics of IoConnectInterrupt() and IoDisconnectInterrupt() properly.
 */
int32_t
IoConnectInterrupt(struct nt_kinterrupt **iobj, void *func, void *ctx,
    unsigned long *lock, uint32_t vector, uint8_t irql, uint8_t syncirql,
    uint8_t imode, uint8_t shared, uint32_t affinity, uint8_t savefloat)
{
	uint8_t curirql;

	*iobj = ExAllocatePool(sizeof(struct nt_kinterrupt));
	if (*iobj == NULL)
		return (NDIS_STATUS_RESOURCES);

	(*iobj)->func = func;
	(*iobj)->ctx = ctx;

	if (lock == NULL) {
		KeInitializeSpinLock(&(*iobj)->lock_priv);
		(*iobj)->lock = &(*iobj)->lock_priv;
	} else
		(*iobj)->lock = lock;

	KeAcquireSpinLock(&nt_intlock, &curirql);
	InsertHeadList(&nt_intlist, &(*iobj)->list);
	KeReleaseSpinLock(&nt_intlock, curirql);

	return (NDIS_STATUS_SUCCESS);
}

void
IoDisconnectInterrupt(struct nt_kinterrupt *iobj)
{
	uint8_t irql;

	if (iobj == NULL)
		return;

	KeAcquireSpinLock(&nt_intlock, &irql);
	RemoveEntryList(&iobj->list);
	KeReleaseSpinLock(&nt_intlock, irql);

	ExFreePool(iobj);
}

struct device_object *
IoAttachDeviceToDeviceStack(struct device_object *src,
    struct device_object *dst)
{
	struct device_object *attached;

	mtx_lock(&nt_dispatchlock);
	attached = IoGetAttachedDevice(dst);
	attached->attacheddev = src;
	src->attacheddev = NULL;
	src->stacksize = attached->stacksize + 1;
	mtx_unlock(&nt_dispatchlock);

	return (attached);
}

void
IoDetachDevice(struct device_object *topdev)
{
	struct device_object *tail;

	mtx_lock(&nt_dispatchlock);

	/* First, break the chain. */
	tail = topdev->attacheddev;
	if (tail == NULL) {
		mtx_unlock(&nt_dispatchlock);
		return;
	}
	topdev->attacheddev = tail->attacheddev;
	topdev->refcnt--;

	for (tail = topdev->attacheddev; tail != NULL; tail = tail->attacheddev)
		tail->stacksize--;

	mtx_unlock(&nt_dispatchlock);
}

/*
 * For the most part, an object is considered signalled if
 * signal_state == TRUE. The exception is for mutant objects
 * (mutexes), where the logic works like this:
 *
 * - If the thread already owns the object and signal_state is
 *   less than or equal to 0, then the object is considered
 *   signalled (recursive acquisition).
 * - If signal_state == 1, the object is also considered signalled.
 */
static int
ntoskrnl_is_signalled(struct nt_dispatcher_header *obj, struct thread *td)
{
	struct nt_kmutex *km;

	if (obj->type == MUTANT_OBJECT) {
		km = (struct nt_kmutex *)obj;
		if ((obj->signal_state <= 0 && km->owner_thread == td) ||
		    obj->signal_state == 1)
			return (TRUE);
		return (FALSE);
	}
	if (obj->signal_state > 0)
		return (TRUE);

	return (FALSE);
}

static void
ntoskrnl_satisfy_wait(struct nt_dispatcher_header *obj, struct thread *td)
{
	struct nt_kmutex *km;

	switch (obj->type) {
	case MUTANT_OBJECT:
		km = (struct nt_kmutex *)obj;
		obj->signal_state--;
		/*
		 * If signal_state reaches 0, the mutex is now
		 * non-signalled (the new thread owns it).
		 */
		if (obj->signal_state == 0) {
			km->owner_thread = td;
			if (km->abandoned == TRUE)
				km->abandoned = FALSE;
		}
		break;
	/* Synchronization objects get reset to unsignalled. */
	case SYNCHRONIZATION_EVENT_OBJECT:
	case SYNCHRONIZATION_TIMER_OBJECT:
		obj->signal_state = FALSE;
		break;
	case SEMAPHORE_OBJECT:
		obj->signal_state--;
		break;
	default:
		break;
	}
}

static void
ntoskrnl_satisfy_multiple_waits(struct wait_block *wb)
{
	struct wait_block *cur = wb;
	struct thread *td;

	td = wb->wb_kthread;

	do {
		ntoskrnl_satisfy_wait(wb->wb_object, td);
		cur->wb_awakened = TRUE;
		cur = cur->wb_next;
	} while (cur != wb);
}

/*
 * Always called with dispatcher lock held.
 */
static void
ntoskrnl_waittest(struct nt_dispatcher_header *obj, uint32_t increment)
{
	struct wait_block *w, *next;
	struct list_entry *e;
	struct thread *td;
	struct wb_ext *we;
	int satisfied;

	/*
	 * Once an object has been signalled, we walk its list of
	 * wait blocks. If a wait block can be awakened, then satisfy
	 * waits as necessary and wake the thread.
	 *
	 * The rules work like this:
	 *
	 * If a wait block is marked as WAIT_ANY, then
	 * we can satisfy the wait conditions on the current
	 * object and wake the thread right away. Satisfying
	 * the wait also has the effect of breaking us out
	 * of the search loop.
	 *
	 * If the object is marked as WAITTYLE_ALL, then the
	 * wait block will be part of a circularly linked
	 * list of wait blocks belonging to a waiting thread
	 * that's sleeping in KeWaitForMultipleObjects(). In
	 * order to wake the thread, all the objects in the
	 * wait list must be in the signalled state. If they
	 * are, we then satisfy all of them and wake the
	 * thread.
	 *
	 */

	e = obj->wait_list_head.flink;
	while (e != &obj->wait_list_head && obj->signal_state > 0) {
		w = CONTAINING_RECORD(e, struct wait_block, wb_waitlist);
		we = w->wb_ext;
		td = we->we_td;
		satisfied = TRUE;
		if (w->wb_waittype == WAIT_ANY) {
			/*
			 * Thread can be awakened if
			 * any wait is satisfied.
			 */
			ntoskrnl_satisfy_wait(obj, td);
			w->wb_awakened = TRUE;
		} else {
			/*
			 * Thread can only be woken up
			 * if all waits are satisfied.
			 * If the thread is waiting on multiple
			 * objects, they should all be linked
			 * through the wb_next pointers in the
			 * wait blocks.
			 */
			next = w->wb_next;
			while (next != w) {
				if (ntoskrnl_is_signalled(obj, td) == FALSE) {
					satisfied = FALSE;
					break;
				}
				next = next->wb_next;
			}
			ntoskrnl_satisfy_multiple_waits(w);
		}

		if (satisfied == TRUE)
			cv_broadcastpri(&we->we_cv,
			    (w->wb_oldpri - (increment * 4)) > PRI_MIN_KERN ?
			    w->wb_oldpri - (increment * 4) : PRI_MIN_KERN);

		e = e->flink;
	}
}

/*
 * Return the number of 100 nanosecond intervals since
 * January 1, 1601. (?!?!)
 */
void
ntoskrnl_time(uint64_t *tval)
{
	struct timespec ts;

	nanotime(&ts);
	*tval = (uint64_t)ts.tv_nsec / 100 + (uint64_t)ts.tv_sec * 10000000 +
	    11644473600 * 10000000; /* 100ns ticks from 1601 to 1970 */
}

static void
KeQuerySystemTime(int64_t *current_time)
{
	ntoskrnl_time(current_time);
}

static void
KeQueryTickCount(int64_t *count)
{
	*count = ticks;
}

static uint32_t
KeQueryTimeIncrement(void)
{
	return (tick * 10);
}

static uint32_t
KeTickCount(void)
{
	return (ticks);
}

/*
 * KeWaitForSingleObject() is a tricky beast, because it can be used
 * with several different object types: semaphores, timers, events,
 * mutexes and threads. Semaphores don't appear very often, but the
 * other object types are quite common. KeWaitForSingleObject() is
 * what's normally used to acquire a mutex, and it can be used to
 * wait for a thread termination.
 *
 * The Windows NDIS API is implemented in terms of Windows kernel
 * primitives, and some of the object manipulation is duplicated in
 * NDIS. For example, NDIS has timers and events, which are actually
 * Windows kevents and ktimers. Now, you're supposed to only use the
 * NDIS variants of these objects within the confines of the NDIS API,
 * but there are some naughty developers out there who will use
 * KeWaitForSingleObject() on NDIS timer and event objects, so we
 * have to support that as well. Conseqently, our NDIS timer and event
 * code has to be closely tied into our ntoskrnl timer and event code,
 * just as it is in Windows.
 *
 * KeWaitForSingleObject() may do different things for different kinds
 * of objects:
 *
 * - For events, we check if the event has been signalled. If the
 *   event is already in the signalled state, we just return immediately,
 *   otherwise we wait for it to be set to the signalled state by someone
 *   else calling KeSetEvent(). Events can be either synchronization or
 *   notification events.
 *
 * - For timers, if the timer has already fired and the timer is in
 *   the signalled state, we just return, otherwise we wait on the
 *   timer. Unlike an event, timers get signalled automatically when
 *   they expire rather than someone having to trip them manually.
 *   Timers initialized with KeInitializeTimer() are always notification
 *   events: KeInitializeTimerEx() lets you initialize a timer as
 *   either a notification or synchronization event.
 *
 * - For mutexes, we try to acquire the mutex and if we can't, we wait
 *   on the mutex until it's available and then grab it. When a mutex is
 *   released, it enters the signalled state, which wakes up one of the
 *   threads waiting to acquire it. Mutexes are always synchronization
 *   events.
 *
 * - For threads, the only thing we do is wait until the thread object
 *   enters a signalled state, which occurs when the thread terminates.
 *   Threads are always notification events.
 *
 * A notification event wakes up all threads waiting on an object. A
 * synchronization event wakes up just one. Also, a synchronization event
 * is auto-clearing, which means we automatically set the event back to
 * the non-signalled state once the wakeup is done.
 */
int32_t
KeWaitForSingleObject(void *arg, uint32_t reason, uint32_t mode,
    uint8_t alertable, int64_t *duetime)
{
	struct wait_block w;
	struct thread *td = curthread;
	struct timeval tv;
	struct wb_ext we;
	struct nt_dispatcher_header *obj = arg;
	uint64_t curtime;
	int error = 0;

	if (obj == NULL)
		return (NDIS_STATUS_INVALID_PARAMETER);

	mtx_lock(&nt_dispatchlock);

	cv_init(&we.we_cv, "KeWFS");
	we.we_td = td;

	/*
	 * Check to see if this object is already signalled,
	 * and just return without waiting if it is.
	 */
	if (ntoskrnl_is_signalled(obj, td) == TRUE) {
		/* Sanity check the signal state value. */
		if (obj->signal_state != INT32_MIN) {
			ntoskrnl_satisfy_wait(obj, curthread);
			mtx_unlock(&nt_dispatchlock);
			return (NDIS_STATUS_SUCCESS);
		} else {
			/*
			 * There's a limit to how many times we can
			 * recursively acquire a mutant. If we hit
			 * the limit, something is very wrong.
			 */
			if (obj->type == MUTANT_OBJECT) {
				mtx_unlock(&nt_dispatchlock);
				panic("mutant limit exceeded");
			}
		}
	}

	bzero((char *)&w, sizeof(struct wait_block));
	w.wb_object = obj;
	w.wb_ext = &we;
	w.wb_waittype = WAIT_ANY;
	w.wb_next = &w;
	w.wb_waitkey = 0;
	w.wb_awakened = FALSE;
	w.wb_oldpri = td->td_priority;

	InsertTailList(&obj->wait_list_head, &w.wb_waitlist);

	/*
	 * The timeout value is specified in 100 nanosecond units
	 * and can be a positive or negative number. If it's positive,
	 * then the duetime is absolute, and we need to convert it
	 * to an absolute offset relative to now in order to use it.
	 * If it's negative, then the duetime is relative and we
	 * just have to convert the units.
	 */
	if (duetime != NULL) {
		if (*duetime < 0) {
			tv.tv_sec = - (*duetime) / 10000000;
			tv.tv_usec = (- (*duetime) / 10) -
			    (tv.tv_sec * 1000000);
		} else {
			ntoskrnl_time(&curtime);
			if (*duetime < curtime)
				tv.tv_sec = tv.tv_usec = 0;
			else {
				tv.tv_sec = ((*duetime) - curtime) / 10000000;
				tv.tv_usec = ((*duetime) - curtime) / 10 -
				    (tv.tv_sec * 1000000);
			}
		}
	}

	if (duetime == NULL)
		cv_wait(&we.we_cv, &nt_dispatchlock);
	else
		error = cv_timedwait(&we.we_cv, &nt_dispatchlock, tvtohz(&tv));

	RemoveEntryList(&w.wb_waitlist);

	cv_destroy(&we.we_cv);

	/* We timed out. Leave the object alone and return status. */
	if (error == EWOULDBLOCK) {
		mtx_unlock(&nt_dispatchlock);
		return (NDIS_STATUS_TIMEOUT);
	}
	mtx_unlock(&nt_dispatchlock);

	return (NDIS_STATUS_SUCCESS);
/*
	return (KeWaitForMultipleObjects(1, &obj, WAIT_ALL, reason,
	    mode, alertable, duetime, &w));
*/
}

static int32_t
KeWaitForMultipleObjects(uint32_t cnt, struct nt_dispatcher_header *obj[],
    uint32_t wtype, uint32_t reason, uint32_t mode, uint8_t alertable,
    int64_t *duetime, struct wait_block *wb_array)
{
	struct thread *td = curthread;
	struct wait_block *whead, *w, _wb_array[MAX_WAIT_OBJECTS];
	struct nt_dispatcher_header *cur;
	struct timeval tv;
	int i, wcnt = 0, error = 0;
	uint64_t curtime;
	struct timespec t1, t2;
	int32_t status = NDIS_STATUS_SUCCESS;
	struct wb_ext we;

	if (cnt > MAX_WAIT_OBJECTS ||
	    (cnt > THREAD_WAIT_OBJECTS && wb_array == NULL))
		return (NDIS_STATUS_INVALID_PARAMETER);

	mtx_lock(&nt_dispatchlock);

	cv_init(&we.we_cv, "KeWFM");
	we.we_td = td;

	if (wb_array == NULL)
		whead = _wb_array;
	else
		whead = wb_array;

	bzero((char *)whead, sizeof(struct wait_block) * cnt);

	/* First pass: see if we can satisfy any waits immediately. */
	wcnt = 0;
	w = whead;

	for (i = 0; i < cnt; i++) {
		InsertTailList(&obj[i]->wait_list_head, &w->wb_waitlist);
		w->wb_ext = &we;
		w->wb_object = obj[i];
		w->wb_waittype = wtype;
		w->wb_waitkey = i;
		w->wb_awakened = FALSE;
		w->wb_oldpri = td->td_priority;
		w->wb_next = w + 1;
		w++;
		wcnt++;
		if (ntoskrnl_is_signalled(obj[i], td)) {
			/*
			 * There's a limit to how many times
			 * we can recursively acquire a mutant.
			 * If we hit the limit, something
			 * is very wrong.
			 */
			if (obj[i]->signal_state == INT32_MIN &&
			    obj[i]->type == MUTANT_OBJECT) {
				mtx_unlock(&nt_dispatchlock);
				panic("mutant limit exceeded");
			}

			/*
			 * If this is a WAIT_ANY wait, then
			 * satisfy the waited object and exit
			 * right now.
			 */
			if (wtype == WAIT_ANY) {
				ntoskrnl_satisfy_wait(obj[i], td);
				status = NDIS_STATUS_WAIT_0 + i;
				goto wait_done;
			} else {
				w--;
				wcnt--;
				w->wb_object = NULL;
				RemoveEntryList(&w->wb_waitlist);
			}
		}
	}

	/*
	 * If this is a WAIT_ALL wait and all objects are
	 * already signalled, satisfy the waits and exit now.
	 */
	if (wtype == WAIT_ALL && wcnt == 0) {
		for (i = 0; i < cnt; i++)
			ntoskrnl_satisfy_wait(obj[i], td);
		status = NDIS_STATUS_SUCCESS;
		goto wait_done;
	}

	/*
	 * Create a circular waitblock list. The waitcount
	 * must always be non-zero when we get here.
	 */
	(w - 1)->wb_next = whead;

	/* Wait on any objects that aren't yet signalled. */

	/* Calculate timeout, if any. */
	if (duetime != NULL) {
		if (*duetime < 0) {
			tv.tv_sec = - (*duetime) / 10000000;
			tv.tv_usec = (- (*duetime) / 10) -
			    (tv.tv_sec * 1000000);
		} else {
			ntoskrnl_time(&curtime);
			if (*duetime < curtime)
				tv.tv_sec = tv.tv_usec = 0;
			else {
				tv.tv_sec = ((*duetime) - curtime) / 10000000;
				tv.tv_usec = ((*duetime) - curtime) / 10 -
				    (tv.tv_sec * 1000000);
			}
		}
	}

	while (wcnt) {
		nanotime(&t1);
		if (duetime == NULL)
			cv_wait(&we.we_cv, &nt_dispatchlock);
		else
			error = cv_timedwait(&we.we_cv,
			    &nt_dispatchlock, tvtohz(&tv));

		/* Wait with timeout expired. */
		if (error) {
			status = NDIS_STATUS_TIMEOUT;
			goto wait_done;
		}

		nanotime(&t2);

		/* See what's been signalled. */
		w = whead;
		do {
			cur = w->wb_object;
			if (ntoskrnl_is_signalled(cur, td) == TRUE ||
			    w->wb_awakened == TRUE) {
				/* Sanity check the signal state value. */
				if (cur->signal_state == INT32_MIN &&
				    cur->type == MUTANT_OBJECT) {
					mtx_unlock(&nt_dispatchlock);
					panic("mutant limit exceeded");
				}
				wcnt--;
				if (wtype == WAIT_ANY) {
					status = w->wb_waitkey &
					    NDIS_STATUS_WAIT_0;
					goto wait_done;
				}
			}
			w = w->wb_next;
		} while (w != whead);

		/*
		 * If all objects have been signalled, or if this
		 * is a WAIT_ANY wait and we were woke up by
		 * someone, we can bail.
		 */
		if (wcnt == 0) {
			status = NDIS_STATUS_SUCCESS;
			goto wait_done;
		}

		/*
		 * If this is WAIT_ALL wait, and there's still
		 * objects that haven't been signalled, deduct the
		 * time that's elapsed so far from the timeout and
		 * wait again (or continue waiting indefinitely if
		 * there's no timeout).
		 */
		if (duetime != NULL) {
			tv.tv_sec -= (t2.tv_sec - t1.tv_sec);
			tv.tv_usec -= (t2.tv_nsec - t1.tv_nsec) / 1000;
		}
	}
wait_done:
	cv_destroy(&we.we_cv);

	for (i = 0; i < cnt; i++) {
		if (whead[i].wb_object != NULL)
			RemoveEntryList(&whead[i].wb_waitlist);

	}
	mtx_unlock(&nt_dispatchlock);

	return (status);
}

static void
WRITE_REGISTER_USHORT(uint16_t *reg, uint16_t val)
{
	bus_space_write_2(X86_BUS_SPACE_MEM, 0x0, (bus_size_t)reg, val);
}

static uint16_t
READ_REGISTER_USHORT(uint16_t *reg)
{
	return (bus_space_read_2(X86_BUS_SPACE_MEM, 0x0, (bus_size_t)reg));
}

static void
WRITE_REGISTER_ULONG(uint32_t *reg, uint32_t val)
{
	bus_space_write_4(X86_BUS_SPACE_MEM, 0x0, (bus_size_t)reg, val);
}

static uint32_t
READ_REGISTER_ULONG(uint32_t *reg)
{
	return (bus_space_read_4(X86_BUS_SPACE_MEM, 0x0, (bus_size_t)reg));
}

static uint8_t
READ_REGISTER_UCHAR(uint8_t *reg)
{
	return (bus_space_read_1(X86_BUS_SPACE_MEM, 0x0, (bus_size_t)reg));
}

static void
WRITE_REGISTER_UCHAR(uint8_t *reg, uint8_t val)
{
	bus_space_write_1(X86_BUS_SPACE_MEM, 0x0, (bus_size_t)reg, val);
}

static int64_t
_allmul(int64_t a, int64_t b)
{
	return (a * b);
}

static int64_t
_alldiv(int64_t a, int64_t b)
{
	return (a / b);
}

static int64_t
_allrem(int64_t a, int64_t b)
{
	return (a % b);
}

static uint64_t
_aullmul(uint64_t a, uint64_t b)
{
	return (a * b);
}

static uint64_t
_aulldiv(uint64_t a, uint64_t b)
{
	return (a / b);
}

static uint64_t
_aullrem(uint64_t a, uint64_t b)
{
	return (a % b);
}

static int64_t
_allshl(int64_t a, uint8_t b)
{
	return (a << b);
}

static uint64_t
_aullshl(uint64_t a, uint8_t b)
{
	return (a << b);
}

static int64_t
_allshr(int64_t a, uint8_t b)
{
	return (a >> b);
}

static uint64_t
_aullshr(uint64_t a, uint8_t b)
{
	return (a >> b);
}

static struct slist_entry *
ntoskrnl_pushsl(union slist_header *head, struct slist_entry *entry)
{
	struct slist_entry *oldhead;

	oldhead = head->slh_list.slh_next;
	entry->sl_next = head->slh_list.slh_next;
	head->slh_list.slh_next = entry;
	head->slh_list.slh_depth++;

	return (oldhead);
}

static struct slist_entry *
ntoskrnl_popsl(union slist_header *head)
{
	struct slist_entry *first;

	first = head->slh_list.slh_next;
	if (first != NULL) {
		head->slh_list.slh_next = first->sl_next;
		head->slh_list.slh_depth--;
	}

	return (first);
}

/*
 * We need this to make lookaside lists work for amd64.
 * We pass a pointer to ExAllocatePoolWithTag() the lookaside
 * list structure. For amd64 to work right, this has to be a
 * pointer to the wrapped version of the routine, not the
 * original. Letting the Windows driver invoke the original
 * function directly will result in a convention calling
 * mismatch and a pretty crash. On x86, this effectively
 * becomes a no-op since func and wrap are the same.
 */
static funcptr
ntoskrnl_findwrap(void *func)
{
	struct image_patch_table *p;

	for (p = ntoskrnl_functbl; p->func != NULL; p++)
		if ((funcptr)p->func == func)
			return ((funcptr)p->wrap);
	return (NULL);
}

static void
ExInitializeNPagedLookasideList(struct npaged_lookaside_list *lookaside,
    lookaside_alloc_func *allocfunc, lookaside_free_func *freefunc,
    uint32_t flags, size_t size, uint32_t tag, uint16_t depth)
{
	bzero((char *)lookaside, sizeof(struct npaged_lookaside_list));

	if (size < sizeof(struct slist_entry))
		lookaside->nll_l.size = sizeof(struct slist_entry);
	else
		lookaside->nll_l.size = size;
	lookaside->nll_l.tag = tag;
	if (allocfunc == NULL)
		lookaside->nll_l.allocfunc = ExAllocatePoolWithTag_wrap;
	else
		lookaside->nll_l.allocfunc = allocfunc;

	if (freefunc == NULL)
		lookaside->nll_l.freefunc = ExFreePool_wrap;
	else
		lookaside->nll_l.freefunc = freefunc;
#ifdef __i386__
	KeInitializeSpinLock(&lookaside->nll_obsoletelock);
#endif
	lookaside->nll_l.type = NON_PAGED_POOL;
	lookaside->nll_l.depth = depth;
	lookaside->nll_l.maximum_depth = LOOKASIDE_DEPTH;
}

static void
ExDeleteNPagedLookasideList(struct npaged_lookaside_list *lookaside)
{
	void *buf;
	void (*freefunc)(void *);

	freefunc = lookaside->nll_l.freefunc;
	while ((buf = ntoskrnl_popsl(&lookaside->nll_l.list_head)) != NULL)
		MSCALL1(freefunc, buf);
}

struct slist_entry *
InterlockedPushEntrySList(union slist_header *head, struct slist_entry *entry)
{
	struct slist_entry *oldhead;

	mtx_lock_spin(&nt_interlock);
	oldhead = ntoskrnl_pushsl(head, entry);
	mtx_unlock_spin(&nt_interlock);

	return (oldhead);
}

struct slist_entry *
InterlockedPopEntrySList(union slist_header *head)
{
	struct slist_entry *first;

	mtx_lock_spin(&nt_interlock);
	first = ntoskrnl_popsl(head);
	mtx_unlock_spin(&nt_interlock);

	return (first);
}

static void
InitializeSListHead(union slist_header *head)
{
	memset(head, 0, sizeof(*head));
}

static struct slist_entry *
ExInterlockedPushEntrySList(union slist_header *head, struct slist_entry *entry,
    unsigned long *lock)
{
	return (InterlockedPushEntrySList(head, entry));
}

static struct slist_entry *
ExInterlockedPopEntrySList(union slist_header *head, unsigned long *lock)
{
	return (InterlockedPopEntrySList(head));
}

uint16_t
ExQueryDepthSList(union slist_header *head)
{
	uint16_t depth;

	mtx_lock_spin(&nt_interlock);
	depth = head->slh_list.slh_depth;
	mtx_unlock_spin(&nt_interlock);

	return (depth);
}

void
KeInitializeSpinLock(unsigned long *lock)
{
	*lock = 0;
}

#ifdef __i386__
void
KefAcquireSpinLockAtDpcLevel(unsigned long *lock)
{
	while (atomic_cmpset_acq_int((volatile unsigned int *)lock, 0, 1) == 0)
		/* sit and spin */;
	thread_lock(curthread);
	sched_prio(curthread, PRI_MIN_KERN);
	thread_unlock(curthread);
}

void
KefReleaseSpinLockFromDpcLevel(unsigned long *lock)
{
	atomic_store_rel_int((volatile unsigned int *)lock, 0);
	thread_lock(curthread);
	sched_prio(curthread, PRI_MIN_KERN + 20);
	thread_unlock(curthread);
}

uint8_t
KeAcquireSpinLockRaiseToDpc(unsigned long *lock)
{
	uint8_t oldirql;

	KASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL, ("irql not <="));
	KeRaiseIrql(DISPATCH_LEVEL, &oldirql);
	KeAcquireSpinLockAtDpcLevel(lock);
	return (oldirql);
}
#else
void
KeAcquireSpinLockAtDpcLevel(unsigned long *lock)
{
	while (atomic_cmpset_acq_int((volatile unsigned int *)lock, 0, 1) == 0)
		/* sit and spin */;
	thread_lock(curthread);
	sched_prio(curthread, PRI_MIN_KERN);
	thread_unlock(curthread);
}

void
KeReleaseSpinLockFromDpcLevel(unsigned long *lock)
{
	atomic_store_rel_int((volatile unsigned int *)lock, 0);
	thread_lock(curthread);
	sched_prio(curthread, PRI_MIN_KERN + 20);
	thread_unlock(curthread);
}
#endif /* __i386__ */

uintptr_t
InterlockedExchange(volatile uint32_t *dst, uintptr_t val)
{
	uintptr_t r;

	mtx_lock_spin(&nt_interlock);
	r = *dst;
	*dst = val;
	mtx_unlock_spin(&nt_interlock);

	return (r);
}

static int32_t
InterlockedIncrement(volatile int32_t *addend)
{
	atomic_add_int(addend, 1);

	return (*addend);
}

static int32_t
InterlockedDecrement(volatile int32_t *addend)
{
	atomic_subtract_int(addend, 1);

	return (*addend);
}

static void
ExInterlockedAddLargeStatistic(uint64_t *addend, uint32_t inc)
{
	mtx_lock_spin(&nt_interlock);
	*addend += inc;
	mtx_unlock_spin(&nt_interlock);
}

struct mdl *
IoAllocateMdl(void *vaddr, uint32_t len, uint8_t secondarybuf,
    uint8_t chargequota, struct irp *iopkt)
{
	struct mdl *m;
	int zone = 0;

	if (MmSizeOfMdl(vaddr, len) > MDL_ZONE_SIZE)
		m = ExAllocatePool(MmSizeOfMdl(vaddr, len));
	else {
		m = uma_zalloc(mdl_zone, M_NOWAIT|M_ZERO);
		zone++;
	}
	if (m == NULL)
		return (NULL);
	MmInitializeMdl(m, vaddr, len);

	/*
	 * MmInitializMdl() clears the flags field, so we
	 * have to set this here. If the MDL came from the
	 * MDL UMA zone, tag it so we can release it to
	 * the right place later.
	 */
	if (zone)
		m->flags = MDL_ZONE_ALLOCED;

	if (iopkt != NULL) {
		if (secondarybuf == TRUE) {
			struct mdl *last;
			last = iopkt->mdl;
			while (last->next != NULL)
				last = last->next;
			last->next = m;
		} else {
			if (iopkt->mdl != NULL)
				panic("leaking an MDL");
			iopkt->mdl = m;
		}
	}

	return (m);
}

void
IoFreeMdl(struct mdl *m)
{
	if (m == NULL)
		return;

	if (m->flags & MDL_ZONE_ALLOCED)
		uma_zfree(mdl_zone, m);
	else
		ExFreePool(m);
}

static void *
MmAllocateContiguousMemory(uint32_t size, uint64_t highest)
{
	TRACE(NDBG_MM, "size %u highest %"PRIu64"\n", size, highest);
	return (ExAllocatePool(roundup(size, PAGE_SIZE)));
}

static vm_memattr_t
cnvmct(enum memory_caching_type type)
{
	switch (type) {
	case MM_WRITE_COMBINED:		return (VM_MEMATTR_WRITE_COMBINING);
	case MM_NON_CACHED:
	case MM_NON_CACHED_UNORDERED:	return (VM_MEMATTR_UNCACHEABLE);
	default:			return (VM_MEMATTR_DEFAULT);
	}
	return (VM_MEMATTR_DEFAULT);
}

static void *
MmAllocateContiguousMemorySpecifyCache(uint32_t size, uint64_t lowest,
    uint64_t highest, uint64_t boundary, enum memory_caching_type type)
{
	void *ret;

	TRACE(NDBG_MM, "size %u lowest %"PRIu64" highest %"PRIu64" boundary "
	    "%"PRIu64" type %d\n", size, lowest, highest, boundary, type);
	ret = (void *)kmem_alloc_contig(kernel_arena, size, M_ZERO|M_NOWAIT,
	    lowest, highest, PAGE_SIZE, boundary, cnvmct(type));
	if (ret != NULL)
		malloc_type_allocated(M_NDIS_NTOSKRNL, round_page(size));
	return (ret);
}

static void
MmFreeContiguousMemory(void *base)
{
	TRACE(NDBG_MM, "base %p\n", base);
	ExFreePool(base);
}

static void
MmFreeContiguousMemorySpecifyCache(void *base, uint32_t size,
    enum memory_caching_type type)
{
	TRACE(NDBG_MM, "base %p size %u type %d\n", base, size, type);
	if (base == NULL)
		return;
	kmem_free(kernel_arena, (vm_offset_t)base, size);
	malloc_type_freed(M_NDIS_NTOSKRNL, round_page(size));
}

static uint32_t
MmSizeOfMdl(void *vaddr, size_t len)
{
	uint32_t l;

	l = sizeof(struct mdl) +
	    (sizeof(vm_offset_t *) * SPAN_PAGES(vaddr, len));

	return (l);
}

/*
 * The Microsoft documentation says this routine fills in the
 * page array of an MDL with the _physical_ page addresses that
 * comprise the buffer, but we don't really want to do that here.
 * Instead, we just fill in the page array with the kernel virtual
 * addresses of the buffers.
 */
void
MmBuildMdlForNonPagedPool(struct mdl *m)
{
	vm_offset_t *mdl_pages;
	int pagecnt, i;

	pagecnt = SPAN_PAGES(m->byteoffset, m->bytecount);
	if (pagecnt > (m->size - sizeof(struct mdl)) / sizeof(vm_offset_t *))
		panic("not enough pages in MDL to describe buffer");

	mdl_pages = MmGetMdlPfnArray(m);

	for (i = 0; i < pagecnt; i++)
		*mdl_pages = (vm_offset_t)m->startva + (i * PAGE_SIZE);

	m->flags |= MDL_SOURCE_IS_NONPAGED_POOL;
	m->mappedsystemva = MmGetMdlVirtualAddress(m);
}

static void *
MmMapLockedPages(struct mdl *buf, uint8_t accessmode)
{
	buf->flags |= MDL_MAPPED_TO_SYSTEM_VA;

	return (MmGetMdlVirtualAddress(buf));
}

static void *
MmMapLockedPagesSpecifyCache(struct mdl *buf, uint8_t accessmode,
    enum memory_caching_type type,
    void *vaddr, uint32_t bugcheck, uint32_t prio)
{
	TRACE(NDBG_MM, "buf %p type %d vaddr %p\n", buf, type, vaddr);
	return (MmMapLockedPages(buf, accessmode));
}

static void
MmUnmapLockedPages(void *vaddr, struct mdl *buf)
{
	TRACE(NDBG_MM, "vaddr %p buf %p\n", vaddr, buf);
	buf->flags &= ~MDL_MAPPED_TO_SYSTEM_VA;
}

static uint64_t
MmGetPhysicalAddress(void *base)
{
	TRACE(NDBG_MM, "base %p\n", base);
	return (pmap_extract(kernel_map->pmap, (vm_offset_t)base));
}

static void *
MmGetSystemRoutineAddress(struct unicode_string *ustr)
{
	struct ansi_string astr;

	if (RtlUnicodeStringToAnsiString(&astr, ustr, TRUE))
		return (NULL);
	TRACE(NDBG_INIT, "routine %s\n", astr.buf);
	return (ndis_get_routine_address(ntoskrnl_functbl, astr.buf));
}

static uint8_t
MmIsAddressValid(void *vaddr)
{
	TRACE(NDBG_MM, "vaddr %p\n", vaddr);
	if (pmap_extract(kernel_map->pmap, (vm_offset_t)vaddr))
		return (TRUE);
	else
		return (FALSE);
}

static void *
MmMapIoSpace(uint64_t paddr, uint32_t len, enum memory_caching_type type)
{
	devclass_t nexus_class;
	device_t *nexus_devs, devp;
	int nexus_count = 0;
	device_t matching_dev = NULL;
	struct resource *res;
	int i;
	vm_offset_t v;

	/* There will always be at least one nexus. */
	nexus_class = devclass_find("nexus");
	devclass_get_devices(nexus_class, &nexus_devs, &nexus_count);

	for (i = 0; i < nexus_count; i++) {
		devp = nexus_devs[i];
		matching_dev = ntoskrnl_finddev(devp, paddr, &res);
		if (matching_dev)
			break;
	}

	free(nexus_devs, M_TEMP);

	if (matching_dev == NULL)
		return (NULL);

	v = (vm_offset_t)rman_get_virtual(res);
	if (paddr > rman_get_start(res))
		v += paddr - rman_get_start(res);

	return ((void *)v);
}

static void
MmUnmapIoSpace(void *vaddr, size_t len)
{
}

static device_t
ntoskrnl_finddev(device_t dev, uint64_t paddr, struct resource **res)
{
	device_t *children = NULL;
	device_t matching_dev;
	int childcnt;
	struct resource *r;
	struct resource_list *rl;
	struct resource_list_entry *rle;
	uint32_t flags;
	int i;

	/* We only want devices that have been successfully probed. */
	if (device_is_alive(dev) == FALSE)
		return (NULL);

	rl = BUS_GET_RESOURCE_LIST(device_get_parent(dev), dev);
	if (rl != NULL) {
		STAILQ_FOREACH(rle, rl, link) {
			r = rle->res;
			if (r == NULL)
				continue;

			flags = rman_get_flags(r);

			if (rle->type == SYS_RES_MEMORY &&
			    paddr >= rman_get_start(r) &&
			    paddr <= rman_get_end(r)) {
				if (!(flags & RF_ACTIVE))
					bus_activate_resource(dev,
					    SYS_RES_MEMORY, 0, r);
				*res = r;
				return (dev);
			}
		}
	}
	/*
	 * If this device has children, do another
	 * level of recursion to inspect them.
	 */
	device_get_children(dev, &children, &childcnt);
	for (i = 0; i < childcnt; i++) {
		matching_dev = ntoskrnl_finddev(children[i], paddr, res);
		if (matching_dev != NULL) {
			free(children, M_TEMP);
			return (matching_dev);
		}
	}
	/* Won't somebody please think of the children! */
	if (children != NULL)
		free(children, M_TEMP);

	return (NULL);
}

static void
run_ndis_work_item(struct ndis_work_item_task *arg, int pending)
{
	struct ndis_work_item *work = arg->work;

	if (work->func == NULL)
		return;
	MSCALL2(work->func, work, work->ctx);
	free(arg, M_NDIS_NTOSKRNL);
}

void
schedule_ndis_work_item(void *arg)
{
	struct ndis_work_item_task *task;

	task = malloc(sizeof(struct ndis_work_item_task), M_NDIS_NTOSKRNL,
	    M_NOWAIT | M_ZERO);
	if (task == NULL) {
		printf("schedule_ndis_work_item: malloc failed\n");
		return;
	}
	TASK_INIT(&task->tq_item, 0, (task_fn_t *)run_ndis_work_item, task);
	task->work = arg;

	taskqueue_enqueue(nq_queue, &task->tq_item);
}

struct io_workitem *
IoAllocateWorkItem(struct device_object *dobj)
{
	struct io_workitem *iw;

	TRACE(NDBG_WORK, "dobj %p\n", dobj);
	iw = uma_zalloc(iw_zone, M_NOWAIT|M_ZERO);
	if (iw == NULL)
		return (NULL);

	InitializeListHead(&iw->list);
	iw->dobj = dobj;

	return (iw);
}

void
IoFreeWorkItem(struct io_workitem *iw)
{
	TRACE(NDBG_WORK, "iw %p\n", iw);
	uma_zfree(iw_zone, iw);
}

static void
IORunWorkItem(struct io_workitem *iw, int pending)
{
	if (iw->func == NULL)
		return;
	MSCALL2(iw->func, iw->dobj, iw->ctx);
}

void
IoQueueWorkItem(struct io_workitem *iw, io_workitem_func func,
    enum work_queue_type type, void *ctx)
{
	TRACE(NDBG_WORK, "iw %p func %p type %d ctx %p\n", iw, func, type, ctx);

	if (func == NULL)
		return;

	/* don't readd task which is already queued */
	if (iw->tq_item.ta_pending)
		return;

	if (iw->func && iw->func != func)
		printf("IoQueueWorkItem: warning iw->func got redefined\n");
	if (iw->ctx && iw->ctx != ctx)
		printf("IoQueueWorkItem: warning iw->ctx got redefined\n");

	iw->func = func;
	iw->ctx = ctx;
	iw->tq_item.ta_priority = type;
	iw->tq_item.ta_func = (task_fn_t *)IORunWorkItem;
	iw->tq_item.ta_context = iw;

	taskqueue_enqueue(wq_queue, &iw->tq_item);
}

static int32_t
RtlAppendUnicodeStringToString(struct unicode_string *dst,
    const struct unicode_string *src)
{
	TRACE(NDBG_RTL, "dst %p src %p\n", dst, src);
	if (dst->maxlen < src->len + dst->len)
		return (NDIS_STATUS_BUFFER_TOO_SMALL);
	if (src->len) {
		memcpy(&dst->buf[dst->len], src->buf, src->len);
		dst->len += src->len;
		if (dst->maxlen > dst->len)
			dst->buf[dst->len / sizeof(dst->buf[0])] = 0;
	}
	return (NDIS_STATUS_SUCCESS);
}

static int32_t
RtlAppendUnicodeToString(struct unicode_string *dst, const uint16_t *src)
{
	TRACE(NDBG_RTL, "dst %p src %p\n", dst, src);
	if (src != NULL) {
		int len;
		for (len = 0; src[len]; len++);
		if (dst->len +
		    (len * sizeof(dst->buf[0])) > dst->maxlen)
			return NDIS_STATUS_BUFFER_TOO_SMALL;
		memcpy(&dst->buf[dst->len], src,
		    len * sizeof(dst->buf[0]));
		dst->len += len * sizeof(dst->buf[0]);
		if (dst->maxlen > dst->len)
			dst->buf[dst->len / sizeof(dst->buf[0])] = 0;
	}
	return (NDIS_STATUS_SUCCESS);
}

static uint32_t
RtlxAnsiStringToUnicodeSize(const struct ansi_string *str)
{
	int i;

	for (i = 0; i < str->maxlen && str->buf[i]; i++);
	return (i * sizeof(uint16_t));
}

static uint32_t
RtlxUnicodeStringToAnsiSize(const struct unicode_string *str)
{
	int i;

	for (i = 0; i < str->maxlen && str->buf[i]; i++);
	return (i * sizeof(uint8_t));
}

static void
RtlZeroMemory(void *dst, size_t len)
{
	memset(dst, 0, len);
}

static void
RtlSecureZeroMemory(void *dst, size_t len)
{
	memset(dst, 0, len);
}

static void
RtlFillMemory(void *dst, size_t len, uint8_t c)
{
	memset(dst, c, len);
}

static void
RtlMoveMemory(void *dst, const void *src, size_t len)
{
	memmove(dst, src, len);
}

static int32_t
RtlCharToInteger(const char *src, uint32_t base, uint32_t *val)
{
	uint32_t res;
	int negative = 0;

	TRACE(NDBG_RTL, "src %p base %u val %p\n", src, base, val);
	if (src == NULL || val == NULL)
		return (NDIS_STATUS_ACCESS_VIOLATION);
	while (*src != '\0' && *src <= ' ')
		src++;
	if (*src == '+')
		src++;
	else if (*src == '-') {
		src++;
		negative = 1;
	}
	if (base == 0) {
		base = 10;
		if (*src == '0') {
			src++;
			if (*src == 'b') {
				base = 2;
				src++;
			} else if (*src == 'o') {
				base = 8;
				src++;
			} else if (*src == 'x') {
				base = 16;
				src++;
			}
		}
	} else if (!(base == 2 || base == 8 || base == 10 || base == 16))
		return (NDIS_STATUS_INVALID_PARAMETER);

	for (res = 0; *src; src++) {
		int v;
		if (isdigit(*src))
			v = *src - '0';
		else if (isxdigit(*src))
			v = tolower(*src) - 'a' + 10;
		else
			v = base;
		if (v >= base)
			return (NDIS_STATUS_INVALID_PARAMETER);
		res = res * base + v;
	}
	*val = negative ? -res : res;
	return (NDIS_STATUS_SUCCESS);
}

static void
RtlCopyMemory(void *dst, const void *src, size_t len)
{
	memcpy(dst, src, len);
}

static size_t
RtlCompareMemory(const void *s1, const void *s2, size_t len)
{
	size_t i;

	for (i = 0; (i < len) && (((const char*)s1)[i] == ((const char*)s2)[i]); i++);
	return (i);
}

static int32_t
RtlCompareString(const struct ansi_string *str1, const struct ansi_string *str2,
    uint8_t case_in_sensitive)
{
	int32_t ret = 0;
	uint16_t len;
	const char *p1 = str1->buf, *p2 = str2->buf;

	TRACE(NDBG_RTL, "str1 %p str2 %p case_in_sensitive %u\n",
	    str1, str2, case_in_sensitive);
	len = min(str1->len, str2->len) / sizeof(char);
	if (case_in_sensitive)
		while (!ret && len--)
			ret = ntoskrnl_toupper(*p1++) - ntoskrnl_toupper(*p2++);
	else
		while (!ret && len--)
			ret = *p1++ - *p2++;
	if (!ret)
		ret = str1->len - str2->len;
	return (ret);
}

static int32_t
RtlCompareUnicodeString(const struct unicode_string *str1,
    const struct unicode_string *str2, uint8_t case_in_sensitive)
{
	int32_t ret = 0;
	uint16_t len;
	const uint16_t *p1 = str1->buf, *p2 = str2->buf;

	TRACE(NDBG_RTL, "str1 %p str2 %p case_in_sensitive %u\n",
	    str1, str2, case_in_sensitive);
	len = min(str1->len, str2->len) / sizeof(uint16_t);
	if (case_in_sensitive)
		while (!ret && len--)
			ret = ntoskrnl_toupper(*p1++) - ntoskrnl_toupper(*p2++);
	else
		while (!ret && len--)
			ret = *p1++ - *p2++;
	if (!ret)
		ret = str1->len - str2->len;
	return (ret);
}

void
RtlInitAnsiString(struct ansi_string *dst, const char *src)
{
	TRACE(NDBG_RTL, "dst %p src %p\n", dst, src);
	if (dst == NULL)
		return;
	if (src == NULL) {
		dst->len = dst->maxlen = 0;
		dst->buf = NULL;
	} else {
		int i;
		for (i = 0; src[i]; i++);
		dst->buf = (char *)src;
		dst->len = i;
		dst->maxlen = i + 1;
	}
}

void
RtlInitUnicodeString(struct unicode_string *dst, const uint16_t *src)
{
	TRACE(NDBG_RTL, "dst %p src %p\n", dst, src);
	if (dst == NULL)
		return;
	if (src == NULL) {
		dst->len = dst->maxlen = 0;
		dst->buf = NULL;
	} else {
		int i;
		for (i = 0; src[i]; i++);
		dst->buf = (uint16_t *)src;
		dst->len = i * sizeof(dst->buf[0]);
		dst->maxlen = (i + 1) * sizeof(dst->buf[0]);
	}
}

static int32_t
RtlUnicodeStringToInteger(const struct unicode_string *src, uint32_t base,
    uint32_t *val)
{
	uint32_t res;
	uint16_t *uchr;
	int i = 0, negative = 0;

	TRACE(NDBG_RTL, "src %p base %u val %p\n", src, base, val);
	if (src == NULL || val == NULL)
		return (NDIS_STATUS_ACCESS_VIOLATION);

	uchr = src->buf;
	while (i < (src->len / sizeof(*uchr)) && uchr[i] == ' ')
		i++;
	if (uchr[i] == '+')
		i++;
	else if (uchr[i] == '-') {
		i++;
		negative = 1;
	}
	if (base == 0) {
		base = 10;
		if (i <= ((src->len / sizeof(*uchr)) - 2) && uchr[i] == '0') {
			i++;
			if (uchr[i] == 'b') {
				base = 2;
				i++;
			} else if (uchr[i] == 'o') {
				base = 8;
				i++;
			} else if (uchr[i] == 'x') {
				base = 16;
				i++;
			}
		}
	}
	if (!(base == 2 || base == 8 || base == 10 || base == 16))
		return (NDIS_STATUS_INVALID_PARAMETER);

	for (res = 0; i < (src->len / sizeof(*uchr)); i++) {
		int v;
		if (isdigit((char)uchr[i]))
			v = uchr[i] - '0';
		else if (isxdigit((char)uchr[i]))
			v = tolower((char)uchr[i]) - 'a' + 10;
		else
			v = base;
		if (v >= base)
			return (NDIS_STATUS_INVALID_PARAMETER);
		res = res * base + v;
	}
	*val = negative ? -res : res;
	return (NDIS_STATUS_SUCCESS);
}

int32_t
RtlUpcaseUnicodeString(struct unicode_string *dst, struct unicode_string *src,
    uint8_t alloc)
{
	uint16_t i, n;

	TRACE(NDBG_RTL, "dst %p src %p alloc %u\n", dst, src, alloc);
	if (alloc) {
		dst->buf = ExAllocatePool(src->len);
		if (dst->buf)
			dst->maxlen = src->len;
		else
			return (NDIS_STATUS_NO_MEMORY);
	} else {
		if (dst->maxlen < src->len)
			return (NDIS_STATUS_BUFFER_OVERFLOW);
	}

	n = src->len / sizeof(src->buf[0]);
	for (i = 0; i < n; i++)
		dst->buf[i] = toupper(src->buf[i]);

	dst->len = src->len;
	return (NDIS_STATUS_SUCCESS);
}

void
RtlFreeUnicodeString(struct unicode_string *str)
{
	TRACE(NDBG_RTL, "str %p\n", str);
	ExFreePool(str->buf);
	str->buf = NULL;
}

void
RtlFreeAnsiString(struct ansi_string *str)
{
	TRACE(NDBG_RTL, "str %p\n", str);
	ExFreePool(str->buf);
	str->buf = NULL;
}

static int32_t
RtlGUIDFromString(struct unicode_string *guid_string, struct guid *guid)
{
	struct ansi_string ansi;
	int32_t ret;
	int i, j, k, l, m;

	ret = RtlUnicodeStringToAnsiString(&ansi, guid_string, TRUE);
	if (ret != NDIS_STATUS_SUCCESS)
		return (ret);
	if (ansi.len != 37 || ansi.buf[0] != '{' ||
	    ansi.buf[36] != '}' || ansi.buf[9] != '-' ||
	    ansi.buf[14] != '-' || ansi.buf[19] != '-' ||
	    ansi.buf[24] != '-') {
		RtlFreeAnsiString(&ansi);
		return (NDIS_STATUS_INVALID_PARAMETER);
	}
	memcpy(&guid->data4, &ansi.buf[29], sizeof(guid->data3));
	ansi.buf[29] = 0;
	if (sscanf(&ansi.buf[1], "%x", &i) == 1 &&
	    sscanf(&ansi.buf[10], "%x", &j) == 1 &&
	    sscanf(&ansi.buf[15], "%x", &k) == 1 &&
	    sscanf(&ansi.buf[20], "%x", &l) == 1 &&
	    sscanf(&ansi.buf[25], "%x", &m) == 1) {
		guid->data1 = (i << 16) | (j < 8) | k;
		guid->data2 = l;
		guid->data3 = m;
		ret = NDIS_STATUS_SUCCESS;
	} else
		ret = NDIS_STATUS_INVALID_PARAMETER;
	RtlFreeAnsiString(&ansi);
	return (ret);
}

static int
atoi(const char *str)
{
	return (int)strtol(str, (char **)NULL, 10);
}

static long
atol(const char *str)
{
	return (strtol(str, (char **)NULL, 10));
}

static int
rand(void)
{
	struct timeval tv;

	microtime(&tv);
	srandom(tv.tv_usec);

	return ((int)random());
}

static void
srand(unsigned int seed)
{
	srandom(seed);
}

static uint16_t *
wcscat(uint16_t *s, const uint16_t *append)
{
	uint16_t *save = s;

	for (; *s; ++s);
	while ((*s++ = *append++) != 0);
	return (save);
}

static int
wcscmp(const uint16_t *s1, const uint16_t *s2)
{
	while (*s1 && *s1 == *s2) {
		s1++;
		s2++;
	}
	return (*s1 - *s2);
}

static uint16_t *
wcscpy(uint16_t *to, const uint16_t *from)
{
	uint16_t *save = to;

	for (; (*to = *from) != 0; ++from, ++to);
	return (save);
}

static int
wcsicmp(const uint16_t *s1, const uint16_t *s2)
{
	while (*s1 && tolower((char)*s1) == tolower((char)*s2)) {
		s1++;
		s2++;
	}
	return (tolower((char)*s1) - tolower((char)*s2));
}

static size_t
wcslen(const uint16_t *str)
{
	register const uint16_t *s;

	for (s = str; *s; ++s);
	return (s - str);
}

static uint16_t *
wcsncpy(uint16_t *dst, const uint16_t *src, size_t n)
{
	if (n != 0) {
		register uint16_t *d = dst;
		register const uint16_t *s = src;

		do {
			if ((*d++ = *s++) == 0) {
				while (--n != 0)
					*d++ = 0;
				break;
			}
		} while (--n != 0);
	}
	return (dst);
}

static uint8_t
IoIsWdmVersionAvailable(uint8_t major, uint8_t minor)
{
	if (major == WDM_MAJOR && minor == WDM_MINOR_WINXP)
		return (TRUE);

	return (FALSE);
}
static int32_t
IoOpenDeviceRegistryKey(struct device_object *devobj, uint32_t type,
    uint32_t mask, void **key)
{
	return (NDIS_STATUS_INVALID_DEVICE_REQUEST);
}

static int32_t
IoGetDeviceObjectPointer(struct unicode_string *name, uint32_t reqaccess,
    void *fileobj, struct device_object *devobj)
{
	/* TODO */
	devobj = NULL;
	fileobj = NULL;
	return (NDIS_STATUS_RESOURCES);
}

static int32_t
IoGetDeviceProperty(struct device_object *devobj,
    enum device_registry_property regprop,
    uint32_t buflen, void *prop, uint32_t *reslen)
{
	struct driver_object *drv;
	uint16_t **name;

	drv = devobj->drvobj;

	switch (regprop) {
	case DEVICE_PROPERTY_DRIVER_KEY_NAME:
		name = prop;
		*name = drv->driver_name.buf;
		*reslen = drv->driver_name.len;
		break;
	default:
		return (NDIS_STATUS_INVALID_PARAMETER_2);
		break;
	}

	return (NDIS_STATUS_SUCCESS);
}

static void
KeInitializeMutex(struct nt_kmutex *kmutex, uint32_t level)
{
	InitializeListHead(&kmutex->header.wait_list_head);
	kmutex->owner_thread = NULL;
	kmutex->apc_disable = TRUE;
	kmutex->abandoned = FALSE;
	kmutex->header.signal_state = TRUE;
	kmutex->header.type = MUTANT_OBJECT;
	kmutex->header.size = sizeof(struct nt_kmutex);
}

static int32_t
KeReleaseMutex(struct nt_kmutex *kmutex, uint8_t kwait)
{
	int32_t prevstate;

	mtx_lock(&nt_dispatchlock);
	prevstate = kmutex->header.signal_state;
	if (kmutex->owner_thread != curthread) {
		mtx_unlock(&nt_dispatchlock);
		return (NDIS_STATUS_MUTANT_NOT_OWNED);
	}

	kmutex->header.signal_state++;
	kmutex->abandoned = FALSE;

	if (kmutex->header.signal_state == 1) {
		kmutex->owner_thread = NULL;
		ntoskrnl_waittest(&kmutex->header, IO_NO_INCREMENT);
	}

	mtx_unlock(&nt_dispatchlock);

	return (prevstate);
}

static int32_t
KeReadStateMutex(struct nt_kmutex *kmutex)
{
	return (kmutex->header.signal_state);
}

void
KeInitializeEvent(struct nt_kevent *kevent, enum event_type type, uint8_t state)
{
	InitializeListHead(&kevent->header.wait_list_head);
	kevent->header.signal_state = state;
	kevent->header.type = NOTIFICATION_EVENT_OBJECT + type;
	kevent->header.size = sizeof(struct nt_kevent);
}

int32_t
KeResetEvent(struct nt_kevent *kevent)
{
	int32_t prevstate;

	mtx_lock(&nt_dispatchlock);
	prevstate = kevent->header.signal_state;
	kevent->header.signal_state = FALSE;
	mtx_unlock(&nt_dispatchlock);
	return (prevstate);
}

int32_t
KeSetEvent(struct nt_kevent *kevent, int32_t increment, uint8_t kwait)
{
	int32_t prevstate;
	struct wait_block *w;
	struct nt_dispatcher_header *dh;
	struct wb_ext *we;

	mtx_lock(&nt_dispatchlock);
	prevstate = kevent->header.signal_state;
	dh = &kevent->header;
	if (IsListEmpty(&dh->wait_list_head))
		/*
		 * If there's nobody in the waitlist, just set
		 * the state to signalled.
		 */
		dh->signal_state = TRUE;
	else {
		/*
		 * Get the first waiter. If this is a synchronization
		 * event, just wake up that one thread (don't bother
		 * setting the state to signalled since we're supposed
		 * to automatically clear synchronization events anyway).
		 *
		 * If it's a notification event, or the first
		 * waiter is doing a WAIT_ALL wait, go through
		 * the full wait satisfaction process.
		 */
		w = CONTAINING_RECORD(dh->wait_list_head.flink,
		    struct wait_block, wb_waitlist);
		we = w->wb_ext;
		if (kevent->header.type == NOTIFICATION_EVENT_OBJECT ||
		    w->wb_waittype == WAIT_ALL) {
			if (prevstate == FALSE) {
				dh->signal_state = TRUE;
				ntoskrnl_waittest(dh, increment);
			}
		} else {
			w->wb_awakened |= TRUE;
			cv_broadcastpri(&we->we_cv,
			    (w->wb_oldpri - (increment * 4)) > PRI_MIN_KERN ?
			    w->wb_oldpri - (increment * 4) : PRI_MIN_KERN);
		}
	}
	mtx_unlock(&nt_dispatchlock);
	return (prevstate);
}

static void
KeClearEvent(struct nt_kevent *kevent)
{
	kevent->header.signal_state = FALSE;
}

static int32_t
KeReadStateEvent(struct nt_kevent *kevent)
{
	return (kevent->header.signal_state);
}

/*
 * The object manager in Windows is responsible for managing
 * references and access to various types of objects, including
 * device_objects, events, threads, timers and so on. However,
 * there's a difference in the way objects are handled in user
 * mode versus kernel mode.
 *
 * In user mode (i.e. Win32 applications), all objects are
 * managed by the object manager. For example, when you create
 * a timer or event object, you actually end up with an
 * object_header (for the object manager's bookkeeping
 * purposes) and an object body (which contains the actual object
 * structure, e.g. ktimer, kevent, etc...). This allows Windows
 * to manage resource quotas and to enforce access restrictions
 * on basically every kind of system object handled by the kernel.
 *
 * However, in kernel mode, you only end up using the object
 * manager some of the time. For example, in a driver, you create
 * a timer object by simply allocating the memory for a ktimer
 * structure and initializing it with KeInitializeTimer(). Hence,
 * the timer has no object_header and no reference counting or
 * security/resource checks are done on it. The assumption in
 * this case is that if you're running in kernel mode, you know
 * what you're doing, and you're already at an elevated privilege
 * anyway.
 *
 * There are some exceptions to this. The two most important ones
 * for our purposes are device_objects and threads. We need to use
 * the object manager to do reference counting on device_objects,
 * and for threads, you can only get a pointer to a thread's
 * dispatch header by using ObReferenceObjectByHandle() on the
 * handle returned by PsCreateSystemThread().
 */
static int32_t
ObReferenceObjectByHandle(void *handle, uint32_t reqaccess, void *otype,
    uint8_t accessmode, void **object, void **handleinfo)
{
	struct nt_objref *nr;

	nr = malloc(sizeof(struct nt_objref), M_NDIS_NTOSKRNL, M_NOWAIT|M_ZERO);
	if (nr == NULL)
		return (NDIS_STATUS_RESOURCES);

	InitializeListHead(&nr->header.wait_list_head);
	nr->obj = handle;
	nr->header.type = THREAD_OBJECT;
	nr->header.signal_state = 0;
	nr->header.size = sizeof(struct thread) / 8;
	TAILQ_INSERT_TAIL(&nt_reflist, nr, link);
	*object = nr;

	return (NDIS_STATUS_SUCCESS);
}

static void
ObfDereferenceObject(void *object)
{
	struct nt_objref *nr = object;

	TAILQ_REMOVE(&nt_reflist, nr, link);
	free(nr, M_NDIS_NTOSKRNL);
}

static int32_t
ZwClose(void *handle)
{
	TRACE(NDBG_ZW, "handle %p\n", handle);
	return (NDIS_STATUS_SUCCESS);
}

static int32_t
ZwCreateFile(void **handle, uint32_t access, struct object_attributes *attr,
    struct io_status_block *iosb, int64_t *size, uint32_t file_attr,
    uint32_t share_access, uint32_t create_disposition,
    uint32_t create_options, void *ea_buf, uint32_t ea_len)
{
	TRACE(NDBG_ZW, "handle %p\n", handle);
	return (NDIS_STATUS_FAILURE);
}

static int32_t
ZwCreateKey(void **handle, uint32_t access, struct object_attributes *attr,
    uint32_t title_index, struct unicode_string *class, uint32_t create_options,
    uint32_t *create_disposition)
{
	TRACE(NDBG_ZW, "handle %p\n", handle);
	return (NDIS_STATUS_FAILURE);
}

static int32_t
ZwDeleteKey(void *handle)
{
	TRACE(NDBG_ZW, "handle %p\n", handle);
	return (NDIS_STATUS_FAILURE);
}

static int32_t
ZwOpenFile(void **handle, uint32_t access, struct object_attributes *attr,
    struct io_status_block *iosb, uint32_t share_access, uint32_t options)
{
	TRACE(NDBG_ZW, "handle %p\n", handle);
	return (NDIS_STATUS_FAILURE);
}

static int32_t
ZwOpenKey(void **handle, uint32_t access, struct object_attributes *attr)
{
	TRACE(NDBG_ZW, "handle %p\n", handle);
	return (NDIS_STATUS_FAILURE);
}

static int32_t
ZwReadFile(void *handle, struct nt_kevent *event, void *apc_func,
    void *apc_ctx, struct io_status_block *iosb, void *buffer, uint32_t len,
    int64_t *byte_offset, uint32_t *key)
{
	TRACE(NDBG_ZW, "handle %p\n", handle);
	return (NDIS_STATUS_FAILURE);
}

static int32_t
ZwWriteFile(void *handle, struct nt_kevent *event, void *apc_func,
    void *apc_ctx, struct io_status_block *iosb, void *buffer, uint32_t len,
    int64_t *byte_offset, uint32_t *key)
{
	TRACE(NDBG_ZW, "handle %p\n", handle);
	return (NDIS_STATUS_FAILURE);
}

static int32_t
WmiQueryTraceInformation(uint32_t traceclass, void *traceinfo,
    uint32_t infolen, uint32_t reqlen, void *buf)
{
	return (NDIS_STATUS_NOT_FOUND);
}

static int32_t
WmiTraceMessage(uint64_t loghandle, uint32_t messageflags,
    void *guid, uint16_t messagenum, ...)
{
	return (NDIS_STATUS_SUCCESS);
}

static int32_t
IoWMIRegistrationControl(struct device_object *dobj, uint32_t action)
{
	return (NDIS_STATUS_NOT_IMPLEMENTED);
}

static int32_t
IoWMIQueryAllData(void *data_block_object, uint32_t *buf_size, void *buf)
{
	return (NDIS_STATUS_NOT_IMPLEMENTED);
}

static int32_t
IoWMIOpenBlock(void *data_block_guid, uint32_t access, void **data_block_object)
{
	return (NDIS_STATUS_NOT_IMPLEMENTED);
}

static int32_t
IoUnregisterPlugPlayNotification(void *entry)
{
	return (NDIS_STATUS_SUCCESS);
}

/*
 * This is here just in case the thread returns without calling
 * PsTerminateSystemThread().
 */
static void
ntoskrnl_thrfunc(void *arg)
{
	struct thread_context *thrctx = arg;
	uint32_t (*tfunc)(void *);
	void *tctx;
	int32_t rval;

	tfunc = thrctx->tc_thrfunc;
	tctx = thrctx->tc_thrctx;
	free(thrctx, M_NDIS_NTOSKRNL);

	rval = MSCALL1(tfunc, tctx);

	PsTerminateSystemThread(rval);
	/* notreached */
}

static int32_t
PsCreateSystemThread(void **handle, uint32_t access, void *objattr,
    void *process, void *clientid, void *thrfunc, void *thrctx)
{
	struct thread_context *tc;
	struct thread *t;

	TRACE(NDBG_THREAD, "handle %p access %u objattr %p process %p "
	    "clientid %p thrfunc %p thrctx %p\n",
	    handle, access, objattr, process, clientid, thrfunc, thrctx);
	tc = malloc(sizeof(struct thread_context), M_NDIS_NTOSKRNL, M_NOWAIT);
	if (tc == NULL)
		return (NDIS_STATUS_RESOURCES);
	tc->tc_thrctx = thrctx;
	tc->tc_thrfunc = thrfunc;

	if (kproc_kthread_add(ntoskrnl_thrfunc, tc, &ndisproc, &t, RFHIGHPID,
	    NDIS_KSTACK_PAGES, "ndis", "thread%d", ntoskrnl_kth)) {
		free(tc, M_NDIS_NTOSKRNL);
		return (NDIS_STATUS_FAILURE);
	}

	*handle = t;
	ntoskrnl_kth++;

	return (NDIS_STATUS_SUCCESS);
}

static int32_t
PsTerminateSystemThread(int32_t status)
{

	TRACE(NDBG_THREAD, "status %d\n", status);

	kthread_exit();

	return (NDIS_STATUS_FAILURE);	/* notreached */
}

static int32_t
DbgPrint(const char *fmt, ...)
{
	va_list ap;

	if (bootverbose) {
		va_start(ap, fmt);
		vprintf(fmt, ap);
		va_end(ap);
	}

	return (NDIS_STATUS_SUCCESS);
}

static void
DbgBreakPoint(void)
{
	kdb_enter(KDB_WHY_NDIS, "DbgBreakPoint(): breakpoint");
}

static void
KeBugCheck(uint32_t code)
{
	panic("%s: STOP 0x%X", __func__, code);
}

static void
KeBugCheckEx(uint32_t code, unsigned long prm1, unsigned long prm2,
    unsigned long prm3, unsigned long prm4)
{
	panic("%s: STOP 0x%X, prm1 0x%lX, prm2 0x%lX, prm3 0x%lX, prm4 0x%lX",
	    __func__, code, prm1, prm2, prm3, prm4);
}

static void
ntoskrnl_timercall(void *arg)
{
	struct timeval tv;
	struct nt_ktimer *timer = arg;
	struct nt_kdpc *dpc;

	timer->header.signal_state = TRUE;
	/*
	 * If this is a periodic timer, re-arm it
	 * so it will fire again. We do this before
	 * calling any deferred procedure calls because
	 * it's possible the DPC might cancel the timer,
	 * in which case it would be wrong for us to
	 * re-arm it again afterwards.
	 */
	if (timer->period) {
		tv.tv_sec = 0;
		tv.tv_usec = timer->period * 1000;
		callout_reset(timer->u.callout, tvtohz(&tv),
		    ntoskrnl_timercall, timer);
	}

	dpc = timer->dpc;

	/* If there's a DPC associated with the timer, queue it up. */
	if (dpc != NULL)
		KeInsertQueueDpc(dpc, NULL, NULL);
}

void
KeInitializeTimer(struct nt_ktimer *timer)
{
	KASSERT(timer != NULL, ("no timer"));
	KeInitializeTimerEx(timer,  NOTIFICATION_TIMER);
}

void
KeInitializeTimerEx(struct nt_ktimer *timer, enum timer_type type)
{
	KASSERT(timer != NULL, ("no timer"));
	InitializeListHead(&timer->header.wait_list_head);
	timer->header.signal_state = FALSE;
	timer->header.type = NOTIFICATION_TIMER_OBJECT + type;
	timer->header.size = sizeof(struct nt_ktimer);
	timer->u.callout = ExAllocatePool(sizeof(struct callout));
	callout_init(timer->u.callout, CALLOUT_MPSAFE);
}

/*
 * DPC subsystem. A Windows Defered Procedure Call has the following
 * properties:
 * - It runs at DISPATCH_LEVEL.
 * - It can have one of 3 importance values that control when it
 *   runs relative to other DPCs in the queue.
 * - On SMP systems, it can be set to run on a specific processor.
 * In order to satisfy the last property, we create a DPC thread for
 * each CPU in the system and bind it to that CPU. Each thread
 * maintains three queues with different importance levels, which
 * will be processed in order from lowest to highest.
 *
 * In Windows, interrupt handlers run as DPCs. (Not to be confused
 * with ISRs, which run in interrupt context and can preempt DPCs.)
 * ISRs are given the highest importance so that they'll take
 * precedence over timers and other things.
 */
static void
ntoskrnl_dpc_thread(void *arg)
{
	struct kdpc_queue *kq = arg;
	struct nt_kdpc *d;
	struct list_entry *l;
	uint8_t irql;

	kq->td = curthread;
	kq->exit = FALSE;
	/*
	 * Elevate our priority. DPCs are used to run interrupt
	 * handlers, and they should trigger as soon as possible
	 * once scheduled by an ISR.
	 */
	thread_lock(curthread);
	sched_prio(curthread, PRI_MIN_KERN + 20);
	thread_unlock(curthread);

	for (;;) {
		KeWaitForSingleObject(&kq->proc, 0, 0, TRUE, NULL);
		KeAcquireSpinLock(&kq->lock, &irql);

		if (kq->exit) {
			kq->exit = FALSE;
			KeReleaseSpinLock(&kq->lock, irql);
			break;
		}

		while (!IsListEmpty(&kq->disp)) {
			l = RemoveHeadList(&kq->disp);
			d = CONTAINING_RECORD(l, struct nt_kdpc, dpclistentry);
			InitializeListHead(&d->dpclistentry);
			KeReleaseSpinLockFromDpcLevel(&kq->lock);
			MSCALL4(d->deferedfunc, d, d->deferredctx,
			    d->sysarg1, d->sysarg2);
			KeAcquireSpinLockAtDpcLevel(&kq->lock);
		}
		KeReleaseSpinLock(&kq->lock, irql);

		KeSetEvent(&kq->done, IO_NO_INCREMENT, FALSE);
	}
	kthread_exit();
	/* notreached */
}

static void
ntoskrnl_destroy_dpc_thread(void)
{
	struct nt_kdpc dpc;

	kq_queue->exit = TRUE;
	KeInitializeDpc(&dpc, NULL, NULL);
	KeSetTargetProcessorDpc(&dpc, 0);
	KeInsertQueueDpc(&dpc, NULL, NULL);
	while (kq_queue->exit)
		tsleep(kq_queue->td->td_proc, PWAIT, "dpcw", hz/10);
}

static uint8_t
ntoskrnl_insert_dpc(struct list_entry *head, struct nt_kdpc *dpc)
{
	struct list_entry *l;

	for (l = head->flink; l != head; l = l->flink)
		if (CONTAINING_RECORD(l, struct nt_kdpc, dpclistentry) == dpc)
			return (FALSE);

	if (dpc->importance == IMPORTANCE_LOW)
		InsertTailList(head, &dpc->dpclistentry);
	else
		InsertHeadList(head, &dpc->dpclistentry);

	return (TRUE);
}

void
KeInitializeDpc(struct nt_kdpc *dpc, void *dpcfunc, void *dpcctx)
{
	if (dpc == NULL)
		return;

	dpc->deferedfunc = dpcfunc;
	dpc->deferredctx = dpcctx;
	dpc->num = KDPC_CPU_DEFAULT;
	dpc->importance = IMPORTANCE_MEDIUM;
	InitializeListHead(&dpc->dpclistentry);
}

uint8_t
KeInsertQueueDpc(struct nt_kdpc *dpc, void *sysarg1, void *sysarg2)
{
	uint8_t r;
	uint8_t irql;

	KASSERT(dpc != NULL, ("no dpc"));

	KeAcquireSpinLock(&kq_queue->lock, &irql);
	r = ntoskrnl_insert_dpc(&kq_queue->disp, dpc);
	if (r == TRUE) {
		dpc->sysarg1 = sysarg1;
		dpc->sysarg2 = sysarg2;
	}
	KeReleaseSpinLock(&kq_queue->lock, irql);

	if (r == FALSE)
		return (r);

	KeSetEvent(&kq_queue->proc, IO_NO_INCREMENT, FALSE);

	return (r);
}

uint8_t
KeRemoveQueueDpc(struct nt_kdpc *dpc)
{
	struct kdpc_queue *kq = kq_queue;
	uint8_t irql;

	if (dpc == NULL)
		return (FALSE);

	KeAcquireSpinLock(&kq->lock, &irql);
	if (dpc->dpclistentry.flink == &dpc->dpclistentry) {
		KeReleaseSpinLock(&kq->lock, irql);
		return (FALSE);
	}

	RemoveEntryList(&dpc->dpclistentry);
	InitializeListHead(&dpc->dpclistentry);

	KeReleaseSpinLock(&kq->lock, irql);

	return (TRUE);
}

void
KeSetImportanceDpc(struct nt_kdpc *dpc, enum kdpc_importance imp)
{
	dpc->importance = imp;
}

void
KeSetTargetProcessorDpc(struct nt_kdpc *dpc, uint8_t cpu)
{
	if (cpu > mp_ncpus)
		return;

	dpc->num = cpu;
}

static void
do_nothing_task(void *ptr, int pending)
{
}

void
flush_queue(void)
{
	struct task t_item;
	bzero(&t_item, sizeof(struct task));
	t_item.ta_func = (task_fn_t *)do_nothing_task;
	t_item.ta_context = NULL;
	taskqueue_enqueue(wq_queue, &t_item);
	taskqueue_drain(wq_queue, &t_item);
	taskqueue_enqueue(nq_queue, &t_item);
	taskqueue_drain(nq_queue, &t_item);
	KeSetEvent(&kq_queue->proc, IO_NO_INCREMENT, FALSE);
	KeWaitForSingleObject(&kq_queue->done, 0, 0, TRUE, NULL);
}

static uint32_t
KeGetCurrentProcessorNumber(void)
{
	return (curthread->td_oncpu);
}

uint8_t
KeSetTimerEx(struct nt_ktimer *timer, int64_t duetime, uint32_t period,
    struct nt_kdpc *dpc)
{
	struct timeval tv;
	uint64_t curtime;

	KASSERT(timer != NULL, ("no timer"));

	timer->header.signal_state = FALSE;
	timer->duetime = duetime;
	timer->period = period;
	timer->dpc = dpc;

	if (duetime < 0) {
		tv.tv_sec = - (duetime) / 10000000;
		tv.tv_usec = (- (duetime) / 10) -
		    (tv.tv_sec * 1000000);
	} else {
		ntoskrnl_time(&curtime);
		if (duetime < curtime)
			tv.tv_sec = tv.tv_usec = 0;
		else {
			tv.tv_sec = ((duetime) - curtime) / 10000000;
			tv.tv_usec = ((duetime) - curtime) / 10 -
			    (tv.tv_sec * 1000000);
		}
	}
	return (callout_reset(timer->u.callout, tvtohz(&tv),
	    ntoskrnl_timercall, timer));
}

uint8_t
KeSetTimer(struct nt_ktimer *timer, int64_t duetime, struct nt_kdpc *dpc)
{
	return (KeSetTimerEx(timer, duetime, 0, dpc));
}

uint8_t
KeCancelTimer(struct nt_ktimer *timer)
{
	KASSERT(timer != NULL, ("no timer"));

	timer->period = 0;
	timer->header.signal_state = FALSE;
	return (callout_stop(timer->u.callout));
}

static uint8_t
KeReadStateTimer(struct nt_ktimer *timer)
{
	return (timer->header.signal_state);
}

static int32_t
KeDelayExecutionThread(uint8_t wait_mode, uint8_t alertable, int64_t *interval)
{
	struct timeval tv;
	uint64_t curtime;

	TRACE(NDBG_THREAD, "wait_mode %u alertable %u interval %p\n",
	    wait_mode, alertable, interval);
	if (*interval < 0) {
		tv.tv_sec = - (*interval) / 10000000;
		tv.tv_usec = (- (*interval) / 10) -
		    (tv.tv_sec * 1000000);
	} else {
		ntoskrnl_time(&curtime);
		if (*interval < curtime)
			tv.tv_sec = tv.tv_usec = 0;
		else {
			tv.tv_sec = ((*interval) - curtime) / 10000000;
			tv.tv_usec = ((*interval) - curtime) / 10 -
			    (tv.tv_sec * 1000000);
		}
	}
	pause("delayx", tvtohz(&tv));
	return (NDIS_STATUS_SUCCESS);
}

static unsigned long
KeQueryActiveProcessors(void)
{
	return (1);
}

static uint64_t
KeQueryInterruptTime(void)
{
	return (ticks * tick * 10);
}

static struct thread *
KeGetCurrentThread(void)
{
	return (curthread);
}

static int32_t
KeSetPriorityThread(struct thread *thread, int32_t priority)
{
	int32_t old;

	TRACE(NDBG_THREAD, "thread %p priority %u\n", thread, priority);
	old = KeQueryPriorityThread(thread);

	thread_lock(thread);
	if (priority == HIGH_PRIORITY)
		sched_prio(thread, PRI_MIN_KERN);
	if (priority == LOW_REALTIME_PRIORITY)
		sched_prio(thread, PRI_MIN_KERN +
		    (PRI_MAX_KERN - PRI_MIN_KERN) / 2);
	if (priority == LOW_PRIORITY)
		sched_prio(thread, PRI_MAX_KERN);
	thread_unlock(thread);

	return (old);
}

static int32_t
KeQueryPriorityThread(struct thread *thread)
{
	int32_t priority;

	TRACE(NDBG_THREAD, "thread %p\n", thread);
	if (thread == NULL)
		return (LOW_REALTIME_PRIORITY);
	if (thread->td_priority <= PRI_MIN_KERN)
		priority = HIGH_PRIORITY;
	else if (thread->td_priority >= PRI_MAX_KERN)
		priority = LOW_PRIORITY;
	else
		priority = LOW_REALTIME_PRIORITY;
	return (priority);
}

static void
KeInitializeSemaphore(struct nt_ksemaphore *semaphore, int32_t count,
    int32_t limit)
{
	InitializeListHead(&semaphore->header.wait_list_head);
	semaphore->limit = limit;
	semaphore->header.signal_state = count;
	semaphore->header.type = SEMAPHORE_OBJECT;
	semaphore->header.size = sizeof(struct nt_ksemaphore);
}

static int32_t
KeReleaseSemaphore(struct nt_ksemaphore *semaphore, int32_t priority,
    int32_t adjustment, uint8_t wait)
{
	int32_t ret;

	mtx_lock(&nt_dispatchlock);
	ret = semaphore->header.signal_state;
	if (semaphore->header.signal_state + adjustment <= semaphore->limit)
		semaphore->header.signal_state += adjustment;
	else
		semaphore->header.signal_state = semaphore->limit;
	if (semaphore->header.signal_state > 0)
		ntoskrnl_waittest(&semaphore->header, IO_NO_INCREMENT);
	mtx_unlock(&nt_dispatchlock);
	return (ret);
}

static int32_t
KeReadStateSemaphore(struct nt_ksemaphore *semaphore)
{
	return (semaphore->header.signal_state);
}

static void
dummy(void)
{
	printf("ntoskrnl dummy called...\n");
}

struct image_patch_table ntoskrnl_functbl[] = {
	IMPORT_CFUNC(DbgPrint, 0),
	IMPORT_CFUNC(KeTickCount, 0),
	IMPORT_CFUNC(WmiTraceMessage, 0),
	IMPORT_CFUNC(atoi, 0),
	IMPORT_CFUNC(atol, 0),
	IMPORT_CFUNC(memcmp, 0),
	IMPORT_CFUNC(memcpy, 0),
	IMPORT_CFUNC(memmove, 0),
	IMPORT_CFUNC(memset, 0),
	IMPORT_CFUNC(rand, 0),
	IMPORT_CFUNC(sprintf, 0),
	IMPORT_CFUNC(srand, 0),
	IMPORT_CFUNC(strcmp, 0),
	IMPORT_CFUNC(strcpy, 0),
	IMPORT_CFUNC(strlen, 0),
	IMPORT_CFUNC(strncmp, 0),
	IMPORT_CFUNC(strncpy, 0),
	IMPORT_CFUNC(strstr, 0),
	IMPORT_CFUNC(vsprintf, 0),
	IMPORT_CFUNC(wcscat, 0),
	IMPORT_CFUNC(wcscmp, 0),
	IMPORT_CFUNC(wcscpy, 0),
	IMPORT_CFUNC(wcsicmp, 0),
	IMPORT_CFUNC(wcslen, 0),
	IMPORT_CFUNC(wcsncpy, 0),
	IMPORT_CFUNC_MAP(_snprintf, snprintf, 0),
	IMPORT_CFUNC_MAP(_vsnprintf, vsnprintf, 0),
	IMPORT_CFUNC_MAP(memchr, ntoskrnl_memchr, 0),
	IMPORT_CFUNC_MAP(strchr, index, 0),
	IMPORT_CFUNC_MAP(stricmp, strcasecmp, 0),
	IMPORT_CFUNC_MAP(strncat, ntoskrnl_strncat, 0),
	IMPORT_CFUNC_MAP(strrchr, rindex, 0),
	IMPORT_CFUNC_MAP(tolower, ntoskrnl_tolower, 0),
	IMPORT_CFUNC_MAP(toupper, ntoskrnl_toupper, 0),
	IMPORT_FFUNC(ExInterlockedAddLargeStatistic, 2),
	IMPORT_FFUNC(ExInterlockedPopEntrySList, 2),
	IMPORT_FFUNC(ExInterlockedPushEntrySList, 3),
	IMPORT_FFUNC(InitializeSListHead, 1),
	IMPORT_FFUNC(InterlockedDecrement, 1),
	IMPORT_FFUNC(InterlockedExchange, 2),
	IMPORT_FFUNC(InterlockedIncrement, 1),
	IMPORT_FFUNC(InterlockedPopEntrySList, 1),
	IMPORT_FFUNC(InterlockedPushEntrySList, 2),
	IMPORT_FFUNC(IofCallDriver, 2),
	IMPORT_FFUNC(IofCompleteRequest, 2),
	IMPORT_FFUNC(ObfDereferenceObject, 1),
#ifdef __i386__
	IMPORT_FFUNC(KeAcquireSpinLockRaiseToDpc, 1),
	IMPORT_FFUNC(KefAcquireSpinLockAtDpcLevel, 1),
	IMPORT_FFUNC(KefReleaseSpinLockFromDpcLevel, 1),
#endif /* __i386__ */
#ifdef __amd64__
	/*
	 * For AMD64, we can get away with just mapping
	 * KeAcquireSpinLockRaiseToDpc() directly to KfAcquireSpinLock()
	 * because the calling conventions end up being the same.
	 * On i386, we have to be careful because KfAcquireSpinLock()
	 * is _fastcall but KeAcquireSpinLockRaiseToDpc() isn't.
	 */
	IMPORT_SFUNC(KeAcquireSpinLockAtDpcLevel, 1),
	IMPORT_SFUNC(KeReleaseSpinLockFromDpcLevel, 1),
	IMPORT_SFUNC_MAP(KeAcquireSpinLockRaiseToDpc, KfAcquireSpinLock, 1),
#endif /* __amd64__*/
	IMPORT_FFUNC_MAP(ExpInterlockedPopEntrySList,
	    InterlockedPopEntrySList, 1),
	IMPORT_FFUNC_MAP(ExpInterlockedPushEntrySList,
	    InterlockedPushEntrySList, 2),
	IMPORT_RFUNC(_allshl, 0),
	IMPORT_RFUNC(_allshr, 0),
	IMPORT_RFUNC(_aullshl, 0),
	IMPORT_RFUNC(_aullshr, 0),
	IMPORT_SFUNC(DbgBreakPoint, 0),
	IMPORT_SFUNC(ExAllocatePoolWithTag, 3),
	IMPORT_SFUNC(ExDeleteNPagedLookasideList, 1),
	IMPORT_SFUNC(ExFreePool, 1),
	IMPORT_SFUNC(ExFreePoolWithTag, 2),
	IMPORT_SFUNC(ExInitializeNPagedLookasideList, 7),
	IMPORT_SFUNC(ExQueryDepthSList, 1),
	IMPORT_SFUNC(IoAcquireCancelSpinLock, 1),
	IMPORT_SFUNC(IoAllocateDriverObjectExtension, 4),
	IMPORT_SFUNC(IoAllocateIrp, 2),
	IMPORT_SFUNC(IoAllocateMdl, 5),
	IMPORT_SFUNC(IoAllocateWorkItem, 1),
	IMPORT_SFUNC(IoAttachDeviceToDeviceStack, 2),
	IMPORT_SFUNC(IoBuildAsynchronousFsdRequest, 6),
	IMPORT_SFUNC(IoBuildDeviceIoControlRequest, 9),
	IMPORT_SFUNC(IoBuildSynchronousFsdRequest, 7),
	IMPORT_SFUNC(IoCancelIrp, 1),
	IMPORT_SFUNC(IoConnectInterrupt, 11),
	IMPORT_SFUNC(IoCreateDevice, 7),
	IMPORT_SFUNC(IoDeleteDevice, 1),
	IMPORT_SFUNC(IoDetachDevice, 1),
	IMPORT_SFUNC(IoDisconnectInterrupt, 1),
	IMPORT_SFUNC(IoFreeIrp, 1),
	IMPORT_SFUNC(IoFreeMdl, 1),
	IMPORT_SFUNC(IoFreeWorkItem, 1),
	IMPORT_SFUNC(IoGetAttachedDevice, 1),
	IMPORT_SFUNC(IoGetDeviceObjectPointer, 4),
	IMPORT_SFUNC(IoGetDeviceProperty, 5),
	IMPORT_SFUNC(IoGetDriverObjectExtension, 2),
	IMPORT_SFUNC(IoInitializeIrp, 3),
	IMPORT_SFUNC(IoIsWdmVersionAvailable, 2),
	IMPORT_SFUNC(IoMakeAssociatedIrp, 2),
	IMPORT_SFUNC(IoOpenDeviceRegistryKey, 4),
	IMPORT_SFUNC(IoQueueWorkItem, 4),
	IMPORT_SFUNC(IoReleaseCancelSpinLock, 1),
	IMPORT_SFUNC(IoReuseIrp, 2),
	IMPORT_SFUNC(IoWMIOpenBlock, 3),
	IMPORT_SFUNC(IoWMIQueryAllData, 3),
	IMPORT_SFUNC(IoWMIRegistrationControl, 2),
	IMPORT_SFUNC(IoUnregisterPlugPlayNotification, 1),
	IMPORT_SFUNC(KeAcquireInterruptSpinLock, 1),
	IMPORT_SFUNC(KeBugCheck, 1),
	IMPORT_SFUNC(KeBugCheckEx, 5),
	IMPORT_SFUNC(KeCancelTimer, 1),
	IMPORT_SFUNC(KeClearEvent, 1),
	IMPORT_SFUNC(KeDelayExecutionThread, 3),
	IMPORT_SFUNC(KeGetCurrentProcessorNumber, 1),
	IMPORT_SFUNC(KeGetCurrentThread, 0),
	IMPORT_SFUNC(KeInitializeDpc, 3),
	IMPORT_SFUNC(KeInitializeEvent, 3),
	IMPORT_SFUNC(KeInitializeMutex, 2),
	IMPORT_SFUNC(KeInitializeSemaphore, 3),
	IMPORT_SFUNC(KeInitializeSpinLock, 1),
	IMPORT_SFUNC(KeInitializeTimer, 1),
	IMPORT_SFUNC(KeInitializeTimerEx, 2),
	IMPORT_SFUNC(KeInsertQueueDpc, 3),
	IMPORT_SFUNC(KeQueryActiveProcessors, 0),
	IMPORT_SFUNC(KeQueryInterruptTime, 0),
	IMPORT_SFUNC(KeQueryPriorityThread, 1),
	IMPORT_SFUNC(KeQuerySystemTime, 1),
	IMPORT_SFUNC(KeQueryTickCount, 1),
	IMPORT_SFUNC(KeQueryTimeIncrement, 0),
	IMPORT_SFUNC(KeReadStateEvent, 1),
	IMPORT_SFUNC(KeReadStateMutex, 1),
	IMPORT_SFUNC(KeReadStateSemaphore, 1),
	IMPORT_SFUNC(KeReadStateTimer, 1),
	IMPORT_SFUNC(KeReleaseInterruptSpinLock, 2),
	IMPORT_SFUNC(KeReleaseMutex, 2),
	IMPORT_SFUNC(KeReleaseSemaphore, 4),
	IMPORT_SFUNC(KeRemoveQueueDpc, 1),
	IMPORT_SFUNC(KeResetEvent, 1),
	IMPORT_SFUNC(KeSetEvent, 3),
	IMPORT_SFUNC(KeSetImportanceDpc, 2),
	IMPORT_SFUNC(KeSetPriorityThread, 2),
	IMPORT_SFUNC(KeSetTargetProcessorDpc, 2),
	IMPORT_SFUNC(KeSetTimer, 3),
	IMPORT_SFUNC(KeSetTimerEx, 4),
	IMPORT_SFUNC(KeSynchronizeExecution, 3),
	IMPORT_SFUNC(KeWaitForMultipleObjects, 8),
	IMPORT_SFUNC(KeWaitForSingleObject, 5),
	IMPORT_SFUNC(MmAllocateContiguousMemory, 2 + 1),
	IMPORT_SFUNC(MmAllocateContiguousMemorySpecifyCache, 5 + 3),
	IMPORT_SFUNC(MmBuildMdlForNonPagedPool, 1),
	IMPORT_SFUNC(MmFreeContiguousMemory, 1),
	IMPORT_SFUNC(MmFreeContiguousMemorySpecifyCache, 3),
	IMPORT_SFUNC(MmGetPhysicalAddress, 1),
	IMPORT_SFUNC(MmGetSystemRoutineAddress, 1),
	IMPORT_SFUNC(MmIsAddressValid, 1),
	IMPORT_SFUNC(MmMapIoSpace, 3 + 1),
	IMPORT_SFUNC(MmMapLockedPages, 2),
	IMPORT_SFUNC(MmMapLockedPagesSpecifyCache, 6),
	IMPORT_SFUNC(MmSizeOfMdl, 1),
	IMPORT_SFUNC(MmUnmapIoSpace, 2),
	IMPORT_SFUNC(MmUnmapLockedPages, 2),
	IMPORT_SFUNC(ObReferenceObjectByHandle, 6),
	IMPORT_SFUNC(PsCreateSystemThread, 7),
	IMPORT_SFUNC(PsTerminateSystemThread, 1),
	IMPORT_SFUNC(READ_REGISTER_UCHAR, 1),
	IMPORT_SFUNC(READ_REGISTER_ULONG, 1),
	IMPORT_SFUNC(READ_REGISTER_USHORT, 1),
	IMPORT_SFUNC(RtlAnsiStringToUnicodeString, 3),
	IMPORT_SFUNC(RtlAppendUnicodeStringToString, 2),
	IMPORT_SFUNC(RtlAppendUnicodeToString, 2),
	IMPORT_SFUNC(RtlCharToInteger, 3),
	IMPORT_SFUNC(RtlCompareMemory, 3),
	IMPORT_SFUNC(RtlCompareString, 3),
	IMPORT_SFUNC(RtlCompareUnicodeString, 3),
	IMPORT_SFUNC(RtlCopyMemory, 3),
	IMPORT_SFUNC(RtlCopyString, 2),
	IMPORT_SFUNC(RtlCopyUnicodeString, 2),
	IMPORT_SFUNC(RtlEqualString, 3),
	IMPORT_SFUNC(RtlEqualUnicodeString, 3),
	IMPORT_SFUNC(RtlFillMemory, 3),
	IMPORT_SFUNC(RtlFreeAnsiString, 1),
	IMPORT_SFUNC(RtlFreeUnicodeString, 1),
	IMPORT_SFUNC(RtlGUIDFromString, 2),
	IMPORT_SFUNC(RtlInitAnsiString, 2),
	IMPORT_SFUNC(RtlInitUnicodeString, 2),
	IMPORT_SFUNC(RtlMoveMemory, 3),
	IMPORT_SFUNC(RtlSecureZeroMemory, 2),
	IMPORT_SFUNC(RtlUnicodeStringToAnsiString, 3),
	IMPORT_SFUNC(RtlUnicodeStringToInteger, 3),
	IMPORT_SFUNC(RtlUpcaseUnicodeString, 3),
	IMPORT_SFUNC(RtlZeroMemory, 2),
	IMPORT_SFUNC(RtlxAnsiStringToUnicodeSize, 1),
	IMPORT_SFUNC(RtlxUnicodeStringToAnsiSize, 1),
	IMPORT_SFUNC(WRITE_REGISTER_UCHAR, 2),
	IMPORT_SFUNC(WRITE_REGISTER_ULONG, 2),
	IMPORT_SFUNC(WRITE_REGISTER_USHORT, 2),
	IMPORT_SFUNC(WmiQueryTraceInformation, 5),
	IMPORT_SFUNC(ZwClose, 1),
	IMPORT_SFUNC(ZwCreateFile, 11),
	IMPORT_SFUNC(ZwCreateKey, 7),
	IMPORT_SFUNC(ZwDeleteKey, 1),
	IMPORT_SFUNC(ZwOpenFile, 6),
	IMPORT_SFUNC(ZwOpenKey, 3),
	IMPORT_SFUNC(ZwReadFile, 9),
	IMPORT_SFUNC(ZwWriteFile, 9),
	IMPORT_SFUNC(_alldiv, 2 + 2),
	IMPORT_SFUNC(_allmul, 2 + 2),
	IMPORT_SFUNC(_allrem, 2 + 2),
	IMPORT_SFUNC(_aulldiv, 2 + 2),
	IMPORT_SFUNC(_aullmul, 2 + 2),
	IMPORT_SFUNC(_aullrem, 2 + 2),
	IMPORT_SFUNC_MAP(ExDeletePagedLookasideList,
	    ExDeleteNPagedLookasideList, 1),
	IMPORT_SFUNC_MAP(ExInitializePagedLookasideList,
	    ExInitializeNPagedLookasideList, 7),
	IMPORT_SFUNC_MAP(KeReleaseSpinLock, KfReleaseSpinLock, 1),
	IMPORT_SFUNC_MAP(RtlInitString, RtlInitAnsiString, 2),
	{ NULL, (FUNC)dummy, NULL, 0, STDCALL },
	{ NULL, NULL, NULL }
};
