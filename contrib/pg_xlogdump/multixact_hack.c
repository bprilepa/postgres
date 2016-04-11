#define FRONTEND 1
#include "postgres.h"

#include <dirent.h>
#include <unistd.h>

#include "access/xlog.h"
#include "access/xlogreader.h"
#include "access/multixact.h"
#include "access/slru.h"
#include "access/transam.h"
#include "common/fe_memutils.h"
#include "common/relpath.h"
#include "getopt_long.h"
#include "rmgrdesc.h"


#define MULTIXACT_OFFSETS_PER_PAGE (BLCKSZ / sizeof(MultiXactOffset))

#define MultiXactIdToOffsetPage(xid) \
	((xid) / (MultiXactOffset) MULTIXACT_OFFSETS_PER_PAGE)
#define MultiXactIdToOffsetEntry(xid) \
	((xid) % (MultiXactOffset) MULTIXACT_OFFSETS_PER_PAGE)

#define MXACT_MEMBER_BITS_PER_XACT			8
#define MXACT_MEMBER_FLAGS_PER_BYTE			1
#define MXACT_MEMBER_XACT_BITMASK	((1 << MXACT_MEMBER_BITS_PER_XACT) - 1)

/* how many full bytes of flags are there in a group? */
#define MULTIXACT_FLAGBYTES_PER_GROUP		4
#define MULTIXACT_MEMBERS_PER_MEMBERGROUP	\
	(MULTIXACT_FLAGBYTES_PER_GROUP * MXACT_MEMBER_FLAGS_PER_BYTE)
/* size in bytes of a complete group */
#define MULTIXACT_MEMBERGROUP_SIZE \
	(sizeof(TransactionId) * MULTIXACT_MEMBERS_PER_MEMBERGROUP + MULTIXACT_FLAGBYTES_PER_GROUP)
#define MULTIXACT_MEMBERGROUPS_PER_PAGE (BLCKSZ / MULTIXACT_MEMBERGROUP_SIZE)
#define MULTIXACT_MEMBERS_PER_PAGE	\
	(MULTIXACT_MEMBERGROUPS_PER_PAGE * MULTIXACT_MEMBERS_PER_MEMBERGROUP)

#define MAX_MEMBERS_IN_LAST_MEMBERS_PAGE \
		((uint32) ((0xFFFFFFFF % MULTIXACT_MEMBERS_PER_PAGE) + 1))

/* page in which a member is to be found */
#define MXOffsetToMemberPage(xid) ((xid) / (TransactionId) MULTIXACT_MEMBERS_PER_PAGE)

/* Location (byte offset within page) of flag word for a given member */
#define MXOffsetToFlagsOffset(xid) \
	((((xid) / (TransactionId) MULTIXACT_MEMBERS_PER_MEMBERGROUP) % \
	  (TransactionId) MULTIXACT_MEMBERGROUPS_PER_PAGE) * \
	 (TransactionId) MULTIXACT_MEMBERGROUP_SIZE)
#define MXOffsetToFlagsBitShift(xid) \
	(((xid) % (TransactionId) MULTIXACT_MEMBERS_PER_MEMBERGROUP) * \
	 MXACT_MEMBER_BITS_PER_XACT)

/* Location (byte offset within page) of TransactionId of given member */
#define MXOffsetToMemberOffset(xid) \
	(MXOffsetToFlagsOffset(xid) + MULTIXACT_FLAGBYTES_PER_GROUP + \
	 ((xid) % MULTIXACT_MEMBERS_PER_MEMBERGROUP) * sizeof(TransactionId))

/* Multixact members wraparound thresholds. */
#define MULTIXACT_MEMBER_SAFE_THRESHOLD		(MaxMultiXactOffset / 2)
#define MULTIXACT_MEMBER_DANGER_THRESHOLD	\
	(MaxMultiXactOffset - MaxMultiXactOffset / 4)

#define PreviousMultiXactId(xid) \
	((xid) == FirstMultiXactId ? MaxMultiXactId : (xid) - 1)

void init_multixact_hack(void);
void shutdown_multixact_hack(void);

/*
 * Links to shared-memory data structures for MultiXact control
 */
static SlruCtlData MultiXactOffsetCtlData;
static SlruCtlData MultiXactMemberCtlData;

#define MultiXactOffsetCtl	(&MultiXactOffsetCtlData)
#define MultiXactMemberCtl	(&MultiXactMemberCtlData)

/* TODO: initialize from pg_control? */
TransactionId ShmemVariableCache_nextXid = FirstNormalTransactionId;

typedef struct MultiXactStateData
{
	/* next-to-be-assigned MultiXactId */
	MultiXactId nextMXact;

	/* next-to-be-assigned offset */
	MultiXactOffset nextOffset;

} MultiXactStateData;

/*
 * Last element of OldestMemberMXactID and OldestVisibleMXactId arrays.
 * Valid elements are (1..MaxOldestSlot); element 0 is never used.
 */
#define MaxOldestSlot	(MaxBackends + max_prepared_xacts)

/* Pointers to the state data in shared memory */
static MultiXactStateData *MultiXactState;

/*
 * Write stuff to files
 */
static void
updateMinMulti(MultiXactId minMulti)
{
	fprintf(stderr, "not implemented");
	exit(1);
}

static void
updateMinMultiOffset(MultiXactOffset off)
{
	fprintf(stderr, "not implemented");
	exit(1);
}

bool
MultiXactIdPrecedes(MultiXactId multi1, MultiXactId multi2)
{
	int32		diff = (int32) (multi1 - multi2);

	return (diff < 0);
}

bool
MultiXactIdPrecedesOrEquals(MultiXactId multi1, MultiXactId multi2)
{
	int32		diff = (int32) (multi1 - multi2);

	return (diff <= 0);
}


/*
 * Decide which of two offsets is earlier.
 */
static bool
MultiXactOffsetPrecedes(MultiXactOffset offset1, MultiXactOffset offset2)
{
	int32		diff = (int32) (offset1 - offset2);

	return (diff < 0);
}

void
MultiXactAdvanceNextMXact(MultiXactId minMulti,
						  MultiXactOffset minMultiOffset)
{
	if (MultiXactIdPrecedes(MultiXactState->nextMXact, minMulti))
	{
		updateMinMulti(minMulti);
	}
	if (MultiXactOffsetPrecedes(MultiXactState->nextOffset, minMultiOffset))
	{
		updateMinMultiOffset(minMultiOffset);
	}
}

static void
WriteMZeroPageXlogRec(int pageno, uint8 info)
{
	XLogRecData rdata;

	rdata.data = (char *) (&pageno);
	rdata.len = sizeof(int);
	rdata.buffer = InvalidBuffer;
	rdata.next = NULL;
	(void) XLogInsert(RM_MULTIXACT_ID, info, &rdata);
}


static int
ZeroMultiXactOffsetPage(int pageno, bool writeXlog)
{
	int			slotno;

	slotno = SimpleLruZeroPage(MultiXactOffsetCtl, pageno);

	if (writeXlog)
		WriteMZeroPageXlogRec(pageno, XLOG_MULTIXACT_ZERO_OFF_PAGE);

	return slotno;
}

/*
 * Ditto, for MultiXactMember
 */
static int
ZeroMultiXactMemberPage(int pageno, bool writeXlog)
{
	int			slotno;

	slotno = SimpleLruZeroPage(MultiXactMemberCtl, pageno);

	if (writeXlog)
		WriteMZeroPageXlogRec(pageno, XLOG_MULTIXACT_ZERO_MEM_PAGE);

	return slotno;
}

static void
RecordNewMultiXact(MultiXactId multi, MultiXactOffset offset,
		   int nmembers, MultiXactMember *members)
{
	int			pageno;
	int			prev_pageno;
	int			entryno;
	int			slotno;
	MultiXactOffset *offptr;
	int			i;

	pageno = MultiXactIdToOffsetPage(multi);
	entryno = MultiXactIdToOffsetEntry(multi);

	/*
	 * Note: we pass the MultiXactId to SimpleLruReadPage as the "transaction"
	 * to complain about if there's any I/O error.  This is kinda bogus, but
	 * since the errors will always give the full pathname, it should be clear
	 * enough that a MultiXactId is really involved.  Perhaps someday we'll
	 * take the trouble to generalize the slru.c error reporting code.
	 */
	slotno = SimpleLruReadPage(MultiXactOffsetCtl, pageno, true, multi);
	offptr = (MultiXactOffset *) MultiXactOffsetCtl->shared->page_buffer[slotno];
	offptr += entryno;

	*offptr = offset;

	MultiXactOffsetCtl->shared->page_dirty[slotno] = true;

	prev_pageno = -1;

	for (i = 0; i < nmembers; i++, offset++)
	{
		TransactionId *memberptr;
		uint32	   *flagsptr;
		uint32		flagsval;
		int			bshift;
		int			flagsoff;
		int			memberoff;

		Assert(members[i].status <= MultiXactStatusUpdate);

		pageno = MXOffsetToMemberPage(offset);
		memberoff = MXOffsetToMemberOffset(offset);
		flagsoff = MXOffsetToFlagsOffset(offset);
		bshift = MXOffsetToFlagsBitShift(offset);

		if (pageno != prev_pageno)
		{
			slotno = SimpleLruReadPage(MultiXactMemberCtl, pageno, true, multi);
			prev_pageno = pageno;
		}

		memberptr = (TransactionId *)
			(MultiXactMemberCtl->shared->page_buffer[slotno] + memberoff);

		*memberptr = members[i].xid;

		flagsptr = (uint32 *)
			(MultiXactMemberCtl->shared->page_buffer[slotno] + flagsoff);

		flagsval = *flagsptr;
		flagsval &= ~(((1 << MXACT_MEMBER_BITS_PER_XACT) - 1) << bshift);
		flagsval |= (members[i].status << bshift);
		*flagsptr = flagsval;

		MultiXactMemberCtl->shared->page_dirty[slotno] = true;
	}
}

bool
TransactionIdPrecedes(TransactionId id1, TransactionId id2)
{
	/*
	 * If either ID is a permanent XID then we can just do unsigned
	 * comparison.  If both are normal, do a modulo-2^32 comparison.
	 */
	int32		diff;

	if (!TransactionIdIsNormal(id1) || !TransactionIdIsNormal(id2))
		return (id1 < id2);

	diff = (int32) (id1 - id2);
	return (diff < 0);
}

bool
TransactionIdFollowsOrEquals(TransactionId id1, TransactionId id2)
{
	int32		diff;

	if (!TransactionIdIsNormal(id1) || !TransactionIdIsNormal(id2))
		return (id1 >= id2);

	diff = (int32) (id1 - id2);
	return (diff >= 0);
}

void
multixact_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;

	/* Backup blocks are not used in multixact records */
	Assert(!(record->xl_info & XLR_BKP_BLOCK_MASK));

	if (info == XLOG_MULTIXACT_ZERO_OFF_PAGE)
	{
		int			pageno;
		int			slotno;

		memcpy(&pageno, XLogRecGetData(record), sizeof(int));

		slotno = ZeroMultiXactOffsetPage(pageno, false);
		SimpleLruWritePage(MultiXactOffsetCtl, slotno);
		Assert(!MultiXactOffsetCtl->shared->page_dirty[slotno]);
	}
	else if (info == XLOG_MULTIXACT_ZERO_MEM_PAGE)
	{
		int			pageno;
		int			slotno;

		memcpy(&pageno, XLogRecGetData(record), sizeof(int));

		slotno = ZeroMultiXactMemberPage(pageno, false);
		SimpleLruWritePage(MultiXactMemberCtl, slotno);
		Assert(!MultiXactMemberCtl->shared->page_dirty[slotno]);
	}
	else if (info == XLOG_MULTIXACT_CREATE_ID)
	{
		xl_multixact_create *xlrec =
		(xl_multixact_create *) XLogRecGetData(record);
		TransactionId max_xid;
		int			i;

		/* Store the data back into the SLRU files */
		RecordNewMultiXact(xlrec->mid, xlrec->moff, xlrec->nmembers,
						   xlrec->members);

		/* Make sure nextMXact/nextOffset are beyond what this record has */
		MultiXactAdvanceNextMXact(xlrec->mid + 1,
								  xlrec->moff + xlrec->nmembers);

		/*
		 * Make sure nextXid is beyond any XID mentioned in the record. This
		 * should be unnecessary, since any XID found here ought to have other
		 * evidence in the XLOG, but let's be safe.
		 */
		max_xid = record->xl_xid;
		for (i = 0; i < xlrec->nmembers; i++)
		{
			if (TransactionIdPrecedes(max_xid, xlrec->members[i].xid))
				max_xid = xlrec->members[i].xid;
		}

		/*
		 * We don't expect anyone else to modify nextXid, hence startup
		 * process doesn't need to hold a lock while checking this. We still
		 * acquire the lock to modify it, though.
		 */
		if (TransactionIdFollowsOrEquals(max_xid,
										 ShmemVariableCache_nextXid))
		{
			ShmemVariableCache_nextXid = max_xid;
			TransactionIdAdvance(ShmemVariableCache_nextXid);
		}
	}
	else
	{
		fprintf(stderr, "multixact_redo: unknown op code %u", info);
		exit(1);
	}
}

void
MultiXactShmemInit(void)
{
	MultiXactOffsetCtl->PagePrecedes = NULL; /* XXX was: MultiXactOffsetPagePrecedes; */
	MultiXactMemberCtl->PagePrecedes = NULL; /* XXX was: MultiXactMemberPagePrecedes; */

	SimpleLruInit(MultiXactOffsetCtl,
				  "MultiXactOffset Ctl", NUM_MXACTOFFSET_BUFFERS, 0,
				  MultiXactOffsetControlLock, "pg_multixact/offsets");
	SimpleLruInit(MultiXactMemberCtl,
				  "MultiXactMember Ctl", NUM_MXACTMEMBER_BUFFERS, 0,
				  MultiXactMemberControlLock, "pg_multixact/members");
}

void
init_multixact_hack()
{
	ShmemVariableCache_nextXid = 0;

	MultiXactShmemInit();

	fprintf(stderr, "Initialized multixact hack\n");
}

void
shutdown_multixact_hack()
{
	SimpleLruFlush(MultiXactOffsetCtl, false);
	SimpleLruFlush(MultiXactMemberCtl, false);
}
