#include <sys/netrollback.h>
#include <sys/netsync.h>

#include <ft/fighter.h>
#include <it/item.h>
#include <wp/weapon.h>
#include <ef/effect.h>
#include <sc/sctypes.h>
#include <sc/scmanager.h>
#include <sys/malloc.h>
#include <sys/netinput.h>
#include <sys/netpeer.h>
#include <sys/netreplay.h>
#include <sys/taskman.h>
#include <sys/utils.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PORT
extern void port_log(const char *fmt, ...);
extern const char *port_symbolize(const void *addr, u32 *offset);
extern void port_exit_process(int code);
extern size_t port_aobj_fixup_snapshot_size(void);
extern size_t port_aobj_fixup_snapshot_save(void *dst, size_t capacity);
extern void port_aobj_fixup_snapshot_load(const void *src, size_t size);
extern char *getenv(const char *name);
extern int atoi(const char *s);
#endif

extern u32 sGCThreadsActive;
extern void *gPortSceneHeap;
extern const size_t gPortSceneHeapSize;

// // // // // // // // // // // //
//                               //
//        SECTION MARKERS        //
//                               //
// // // // // // // // // // // //

/* Bounds of the gameplay-global sections built by port/netrollback_seg.h.
 * The $a/$z subsections sort before/after every $m contribution. */
#if defined(_MSC_VER)
#pragma bss_seg(".ssbb$a")
u8 gSYNetRollbackBssStart;
#pragma bss_seg(".ssbb$z")
u8 gSYNetRollbackBssEnd;
#pragma data_seg(".ssbd$a")
u8 gSYNetRollbackDataStart = 1;
#pragma data_seg(".ssbd$z")
u8 gSYNetRollbackDataEnd = 1;
#pragma data_seg()
#pragma bss_seg()
#define SYNETROLLBACK_HAS_SECTIONS TRUE
#else
#define SYNETROLLBACK_HAS_SECTIONS FALSE
#endif

// // // // // // // // // // // //
//                               //
//   GLOBAL / STATIC VARIABLES   //
//                               //
// // // // // // // // // // // //

/* Slot SYNETROLLBACK_SLOT_NUM is private to SyncTest: the post-update state
 * of the first pass, compared byte-for-byte against the second pass. */
#define SYNETROLLBACK_SLOT_COMPARE SYNETROLLBACK_SLOT_NUM
#define SYNETROLLBACK_DIFF_REPORT_MAX 64

/* Reloc files loaded into the arena during the session. Gameplay never writes
 * file data; only the port's lazy endian fixups do, and those are tracked in
 * host-side "already fixed" sets. Restoring file bytes would desync the bytes
 * from those sets, so file ranges are excluded from snapshots entirely.
 *
 * Fighter animation (figatree) heaps are the exception: gameplay reloads
 * them on status changes, so their bytes ARE state. They stay in the
 * snapshot, and their fixup verdicts (port_aobj_fixup bitmaps) are saved
 * and restored alongside so bytes and verdicts always agree. */
#define SYNETROLLBACK_FILE_RANGE_MAX 512

typedef struct SYNetRollbackState
{
	sb32 is_valid;
	u8 *data;
	u8 *bss;
	u8 *arena;
	size_t arena_size;
	SYMallocRegion heap;
	u32 spawn_serial;
	u8 *aobj_fixup;
	size_t aobj_fixup_size;
	size_t aobj_fixup_capacity;

} SYNetRollbackState;

typedef struct SYNetRollbackRange
{
	u8 *lo;
	u8 *hi;

} SYNetRollbackRange;

static SYNetRollbackState sSYNetRollbackStates[SYNETROLLBACK_SLOT_NUM + 1];

static sb32 sSYNetRollbackIsSyncTest;
static sb32 sSYNetRollbackIsTickTimer;
static sb32 sSYNetRollbackIsTickTimerDefault;
static sb32 sSYNetRollbackIsSessionActive;
static sb32 sSYNetRollbackIsResimulating;
static sb32 sSYNetRollbackIsRigExit;
static u32 sSYNetRollbackTraceTick;
static u8 *sSYNetRollbackArenaWatermark;

static SYNetRollbackRange sSYNetRollbackFileRanges[SYNETROLLBACK_FILE_RANGE_MAX];
static s32 sSYNetRollbackFileRangesNum;
static sb32 sSYNetRollbackIsFileRangeOverflow;

/* Snapshot ring: slot i holds the pre-update state of sSYNetRollbackSlotTicks[i].
 * Ticks before sSYNetRollbackChainStart are not reachable (a gap in the
 * chain: some tick in between could not be snapshotted). */
static u32 sSYNetRollbackSlotTicks[SYNETROLLBACK_SLOT_NUM];
static u32 sSYNetRollbackChainStart;
static u32 sSYNetRollbackSyncTestDistance;
static SYNetSyncTickHash sSYNetRollbackFirstHashes[SYNETROLLBACK_SLOT_NUM];

/* Netplay: hash of each tick's post-update state in the current (possibly
 * corrected) timeline, and the confirmed-state trace written once a tick's
 * inputs are all confirmed (SSB64_NETPLAY_CONFIRMED_TRACE). Two peers'
 * traces must be identical line for line. */
static SYNetSyncTickHash sSYNetRollbackLiveHashes[SYNETROLLBACK_SLOT_NUM];
static u32 sSYNetRollbackLiveHashTicks[SYNETROLLBACK_SLOT_NUM];
static sb32 sSYNetRollbackLiveHashValid[SYNETROLLBACK_SLOT_NUM];
static u32 sSYNetRollbackTraceNextTick;
static FILE *sSYNetRollbackConfirmedTrace;
static const char *sSYNetRollbackConfirmedTracePath;
#ifdef PORT
static sb32 sSYNetRollbackIsDefaultTraceTried;
#endif
static u32 sSYNetRollbackRollbacks;
static u32 sSYNetRollbackResimTicks;
static u32 sSYNetRollbackMaxRollback;
static u32 sSYNetRollbackFailedRollbacks;

/* In-band desync detection: the confirmed full-state hash of recent ticks;
 * each input packet carries the newest one and the receiver compares. */
#define SYNETROLLBACK_CONFIRMED_RING 128
#define SYNETROLLBACK_SYNC_CHECK_LAG 16
static u32 sSYNetRollbackConfirmedTicks[SYNETROLLBACK_CONFIRMED_RING];
static u32 sSYNetRollbackConfirmedHashes[SYNETROLLBACK_CONFIRMED_RING];
#ifdef PORT
/* Every column of each confirmed tick, logged on a desync so the two
 * players' logs show which part of the game state diverged. */
static SYNetSyncTickHash sSYNetRollbackConfirmedColumns[SYNETROLLBACK_CONFIRMED_RING];
#endif
static sb32 sSYNetRollbackConfirmedValid[SYNETROLLBACK_CONFIRMED_RING];
static sb32 sSYNetRollbackHasConfirmed;
static u32 sSYNetRollbackLatestConfirmedTick;
static sb32 sSYNetRollbackIsDesynced;
static u32 sSYNetRollbackDesyncTick;
static u32 sSYNetRollbackSyncChecks;

/* Set when the live tick ran in a rollback session; consumed by taskman. */
static sb32 sSYNetRollbackIsTickRan;

static u32 sSYNetRollbackTotalTestedTicks;
static u32 sSYNetRollbackTotalSkippedTicks;
static u32 sSYNetRollbackTotalFailedTicks;
static u32 sSYNetRollbackTestedTicks;
static u32 sSYNetRollbackSkippedTicks;
static u32 sSYNetRollbackFailedTicks;
static u32 sSYNetRollbackColumnReported;
static u32 sSYNetRollbackDiffsReported;
static size_t sSYNetRollbackMaxArenaBytes;

// // // // // // // // // // // //
//                               //
//           FUNCTIONS           //
//                               //
// // // // // // // // // // // //

static size_t syNetRollbackGetDataSize(void)
{
#if SYNETROLLBACK_HAS_SECTIONS
	return (size_t)(&gSYNetRollbackDataEnd - &gSYNetRollbackDataStart);
#else
	return 0;
#endif
}

static size_t syNetRollbackGetBssSize(void)
{
#if SYNETROLLBACK_HAS_SECTIONS
	return (size_t)(&gSYNetRollbackBssEnd - &gSYNetRollbackBssStart);
#else
	return 0;
#endif
}

#ifdef PORT
static s32 syNetRollbackGetEnvInt(const char *name, s32 fallback)
{
	const char *env = getenv(name);

	return (env != NULL) ? atoi(env) : fallback;
}

/* SSB64_ROLLBACK_AUTOVS=1: skip the menus into a 4-CPU (level 9) VS match so
 * SyncTest runs unattended. Optional: SSB64_ROLLBACK_STAGE (gkind),
 * SSB64_ROLLBACK_FIGHTERS ("fkind,fkind,fkind,fkind"), SSB64_ROLLBACK_SEED,
 * SSB64_ROLLBACK_MINUTES (default 1),
 * SSB64_ROLLBACK_PLAYERS (active CPU count 2-4, default 4). */
static void syNetRollbackSetupAutoVS(void)
{
	SCBattleState battle_state = dSCManagerDefaultBattleState;
	SYNetInputReplayMetadata metadata;
	const char *fighters_env = getenv("SSB64_ROLLBACK_FIGHTERS");
	u32 seed = (u32)syNetRollbackGetEnvInt("SSB64_ROLLBACK_SEED", 12345);
	s32 player;

	syNetReplayCaptureBattleMetadata(&battle_state, &metadata);

	metadata.rng_seed = seed;
	metadata.game_type = nSCBattleGameTypeRoyal;
	metadata.game_rules = SCBATTLE_GAMERULE_TIME;
	metadata.time_limit = syNetRollbackGetEnvInt("SSB64_ROLLBACK_MINUTES", 1);
	metadata.stage_kind = syNetRollbackGetEnvInt("SSB64_ROLLBACK_STAGE", nGRKindBattleStart + (seed % (nGRKindBattleEnd - nGRKindBattleStart + 1)));
	metadata.item_toggles = ~0;
	metadata.item_appearance_rate = nSCBattleItemSwitchHigh;

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		s32 fkind = nFTKindPlayableStart + ((seed + player * 3) % (nFTKindPlayableEnd - nFTKindPlayableStart + 1));

		if (fighters_env != NULL)
		{
			fkind = atoi(fighters_env);

			while ((*fighters_env != '\0') && (*fighters_env != ','))
			{
				fighters_env++;
			}
			fighters_env = (*fighters_env == ',') ? fighters_env + 1 : NULL;
		}
		metadata.player_kinds[player] = (player < syNetRollbackGetEnvInt("SSB64_ROLLBACK_PLAYERS", MAXCONTROLLERS)) ? nFTPlayerKindCom : nFTPlayerKindNot;
		metadata.fighter_kinds[player] = fkind;
		metadata.costumes[player] = 0;
		metadata.teams[player] = player;
		metadata.handicaps[player] = 9;
		metadata.levels[player] = 9;
		metadata.shades[player] = 0;
	}
	syNetReplayApplyBattleMetadata(&metadata);
	syUtilsSetRandomSeed(seed);
	gSCManagerSceneData.scene_prev = nSCKindVSMode;
	gSCManagerSceneData.scene_curr = nSCKindVSBattle;

	port_log("SSB64 Rollback: AutoVS stage=%u fighters=%u,%u,%u,%u seed=%u minutes=%u\n",
	         metadata.stage_kind, metadata.fighter_kinds[0], metadata.fighter_kinds[1],
	         metadata.fighter_kinds[2], metadata.fighter_kinds[3], seed, metadata.time_limit);
}
#endif

void syNetRollbackInitDebugEnv(void)
{
#ifdef PORT
	const char *synctest_env = getenv("SSB64_ROLLBACK_SYNCTEST");
	const char *netplay_env = getenv("SSB64_NETPLAY");

	sSYNetRollbackIsSyncTest = ((synctest_env != NULL) && (atoi(synctest_env) > 0)) ? TRUE : FALSE;
	sSYNetRollbackSyncTestDistance = (sSYNetRollbackIsSyncTest != FALSE) ? (u32)atoi(synctest_env) : 0;

	if (sSYNetRollbackSyncTestDistance > SYNETROLLBACK_SLOT_NUM)
	{
		sSYNetRollbackSyncTestDistance = SYNETROLLBACK_SLOT_NUM;
	}

	if ((sSYNetRollbackIsSyncTest != FALSE) && (SYNETROLLBACK_HAS_SECTIONS == FALSE))
	{
		port_log("SSB64 Rollback: SyncTest needs MSVC section grouping; disabled on this toolchain\n");
		sSYNetRollbackIsSyncTest = FALSE;
	}
	/* The original match timer counts VI retraces. Resimulation runs several
	 * ticks inside one VI, so any rollback-capable session counts sim ticks
	 * instead. Offline play keeps the retrace-driven timer (1:1 with N64). */
	sSYNetRollbackIsTickTimer = ((sSYNetRollbackIsSyncTest != FALSE) || ((netplay_env != NULL) && (atoi(netplay_env) != 0))) ? TRUE : FALSE;
	sSYNetRollbackIsTickTimerDefault = sSYNetRollbackIsTickTimer;

	sSYNetRollbackIsRigExit = (syNetRollbackGetEnvInt("SSB64_RIG_EXIT", 0) != 0) ? TRUE : FALSE;
	sSYNetRollbackConfirmedTracePath = getenv("SSB64_NETPLAY_CONFIRMED_TRACE");
	sSYNetRollbackTraceTick = (u32)syNetRollbackGetEnvInt("SSB64_ROLLBACK_TRACE_TICK", 0);

	if (syNetRollbackGetEnvInt("SSB64_ROLLBACK_AUTOVS", 0) != 0)
	{
		syNetRollbackSetupAutoVS();
	}
	if (sSYNetRollbackIsSyncTest != FALSE)
	{
		port_log("SSB64 Rollback: SyncTest enabled distance=%u data=%u bss=%u bytes\n", sSYNetRollbackSyncTestDistance,
		         (unsigned)syNetRollbackGetDataSize(), (unsigned)syNetRollbackGetBssSize());
		{
			u32 sym_offset;
			const char *sym = port_symbolize(&sSYNetRollbackTestedTicks, &sym_offset);

			port_log("SSB64 Rollback: symbolizer probe -> %s\n", (sym != NULL) ? sym : "(unavailable)");
		}
	}
#endif
}

/* Called first thing in the VS task start function: everything the arena
 * holds below this point is the scene's render buffers, never rolled back. */
/* One VS scene can hold two sessions (timed tie -> sudden death); each is
 * reported on its own and folded into the scene total used by the verdict. */
static void syNetRollbackReportSession(void)
{
#ifdef PORT
	if ((sSYNetRollbackIsSyncTest == FALSE) || (sSYNetRollbackIsSessionActive == FALSE))
	{
		return;
	}
	port_log("SSB64 Rollback: session end status=%d time_passed=%u time_remain=%u is_reset=%d stocks=%d,%d,%d,%d\n",
	         gSCManagerBattleState->game_status, gSCManagerBattleState->time_passed, gSCManagerBattleState->time_remain,
	         gSCManagerSceneData.is_reset, gSCManagerBattleState->players[0].stock_count, gSCManagerBattleState->players[1].stock_count,
	         gSCManagerBattleState->players[2].stock_count, gSCManagerBattleState->players[3].stock_count);
	port_log("SSB64 Rollback: session tested=%u failed=%u skipped=%u max_arena=%u bytes\n",
	         sSYNetRollbackTestedTicks, sSYNetRollbackFailedTicks, sSYNetRollbackSkippedTicks,
	         (unsigned)sSYNetRollbackMaxArenaBytes);
#endif
	sSYNetRollbackTotalTestedTicks += sSYNetRollbackTestedTicks;
	sSYNetRollbackTotalSkippedTicks += sSYNetRollbackSkippedTicks;
	sSYNetRollbackTotalFailedTicks += sSYNetRollbackFailedTicks;
}

void syNetRollbackStartVSSession(void)
{
	s32 i;

	syNetRollbackReportSession();

	for (i = 0; i < ARRAY_COUNT(sSYNetRollbackStates); i++)
	{
		sSYNetRollbackStates[i].is_valid = FALSE;
	}
	sSYNetRollbackArenaWatermark = (u8*)gSYTaskmanGeneralHeap.ptr;
	sSYNetRollbackChainStart = 0;
	sSYNetRollbackTraceNextTick = 0;
#ifdef PORT
	sSYNetRollbackIsDefaultTraceTried = FALSE;
#endif
	sSYNetRollbackHasConfirmed = FALSE;
	sSYNetRollbackIsDesynced = FALSE;
	sSYNetRollbackSyncChecks = 0;

	for (i = 0; i < SYNETROLLBACK_CONFIRMED_RING; i++)
	{
		sSYNetRollbackConfirmedValid[i] = FALSE;
	}
	sSYNetRollbackRollbacks = 0;
	sSYNetRollbackResimTicks = 0;
	sSYNetRollbackMaxRollback = 0;
	sSYNetRollbackFailedRollbacks = 0;

	for (i = 0; i < SYNETROLLBACK_SLOT_NUM; i++)
	{
		sSYNetRollbackLiveHashValid[i] = FALSE;
	}
#ifdef PORT
	if (sSYNetRollbackConfirmedTrace == NULL)
	{
		if (sSYNetRollbackConfirmedTracePath != NULL)
		{
			sSYNetRollbackConfirmedTrace = fopen(sSYNetRollbackConfirmedTracePath, "w");
		}
	}
#endif
	sSYNetRollbackFileRangesNum = 0;
	sSYNetRollbackIsFileRangeOverflow = FALSE;
	sSYNetRollbackIsSessionActive = TRUE;
	sSYNetRollbackIsResimulating = FALSE;
	sSYNetRollbackTestedTicks = 0;
	sSYNetRollbackSkippedTicks = 0;
	sSYNetRollbackFailedTicks = 0;
	sSYNetRollbackColumnReported = 0;
	sSYNetRollbackDiffsReported = 0;
	sSYNetRollbackMaxArenaBytes = 0;
}

/* Sudden death restarts the session in the middle of a match: the tick counter goes on, so the
 * confirmed trace must continue at the current tick instead of starting over at 0. */
void syNetRollbackContinueTraceAt(u32 tick)
{
	sSYNetRollbackTraceNextTick = tick;
}

void syNetRollbackFinishVSSession(void)
{
#ifdef PORT
	if ((syNetPeerIsRollbackActive() != FALSE) && (sSYNetRollbackIsSessionActive != FALSE))
	{
		port_log("SSB64 Rollback: netplay session rollbacks=%u resim_ticks=%u max_rollback=%u failed=%u traced_ticks=%u sync_checks=%u desync=%d\n",
		         sSYNetRollbackRollbacks, sSYNetRollbackResimTicks, sSYNetRollbackMaxRollback,
		         sSYNetRollbackFailedRollbacks, sSYNetRollbackTraceNextTick, sSYNetRollbackSyncChecks, sSYNetRollbackIsDesynced);
	}
	if (sSYNetRollbackConfirmedTrace != NULL)
	{
		fflush(sSYNetRollbackConfirmedTrace);
	}
	if ((sSYNetRollbackIsSyncTest != FALSE) && (sSYNetRollbackIsSessionActive != FALSE))
	{
		syNetRollbackReportSession();

		port_log("SSB64 Rollback: SyncTest %s tested=%u failed=%u skipped=%u\n",
		         (sSYNetRollbackTotalFailedTicks == 0) ? "PASS" : "FAIL",
		         sSYNetRollbackTotalTestedTicks, sSYNetRollbackTotalFailedTicks, sSYNetRollbackTotalSkippedTicks);

		if (sSYNetRollbackIsRigExit != FALSE)
		{
			port_exit_process((sSYNetRollbackTotalFailedTicks == 0) ? 0 : 1);
		}
		sSYNetRollbackTotalTestedTicks = 0;
		sSYNetRollbackTotalSkippedTicks = 0;
		sSYNetRollbackTotalFailedTicks = 0;
	}
#endif
	sSYNetRollbackIsSessionActive = FALSE;
}

sb32 syNetRollbackIsTickTimer(void)
{
	return sSYNetRollbackIsTickTimer;
}

/* Online matches started from the lobby switch the timer to sim ticks for
 * the match only; RestoreTickTimer returns to the env-derived default. */
void syNetRollbackSetTickTimer(sb32 is_tick_timer)
{
	sSYNetRollbackIsTickTimer = is_tick_timer;
}

void syNetRollbackRestoreTickTimer(void)
{
	sSYNetRollbackIsTickTimer = sSYNetRollbackIsTickTimerDefault;
}

sb32 syNetRollbackIsResimulating(void)
{
	return sSYNetRollbackIsResimulating;
}

/* Called by the reloc loader for every file it copies into memory. Ranges are
 * kept sorted and merged; only arena memory above the setup watermark is
 * tracked (everything below it is never snapshotted anyway). */
void syNetRollbackNoteFileLoad(void *dst, size_t size, sb32 is_figatree)
{
	u8 *lo = (u8*)dst;
	u8 *hi = lo + size;
	u8 *arena_end;
	s32 i;
	s32 j;

	if ((sSYNetRollbackIsSessionActive == FALSE) || (is_figatree != FALSE) || (size == 0) || (gPortSceneHeap == NULL))
	{
		return;
	}
	arena_end = (u8*)gPortSceneHeap + gPortSceneHeapSize;

	if ((hi <= sSYNetRollbackArenaWatermark) || (lo >= arena_end))
	{
		return;
	}
	if (lo < sSYNetRollbackArenaWatermark)
	{
		lo = sSYNetRollbackArenaWatermark;
	}
	if (hi > arena_end)
	{
		hi = arena_end;
	}
	/* Absorb every range overlapping or touching [lo, hi), then insert. */
	for (i = 0; i < sSYNetRollbackFileRangesNum; )
	{
		SYNetRollbackRange *range = &sSYNetRollbackFileRanges[i];

		if ((range->hi >= lo) && (range->lo <= hi))
		{
			if (range->lo < lo)
			{
				lo = range->lo;
			}
			if (range->hi > hi)
			{
				hi = range->hi;
			}
			for (j = i; j < sSYNetRollbackFileRangesNum - 1; j++)
			{
				sSYNetRollbackFileRanges[j] = sSYNetRollbackFileRanges[j + 1];
			}
			sSYNetRollbackFileRangesNum--;
		}
		else i++;
	}
	if (sSYNetRollbackFileRangesNum >= SYNETROLLBACK_FILE_RANGE_MAX)
	{
		if (sSYNetRollbackIsFileRangeOverflow == FALSE)
		{
#ifdef PORT
			port_log("SSB64 Rollback: file range table full; rollback disabled for this session\n");
#endif
			sSYNetRollbackIsFileRangeOverflow = TRUE;
		}
		return;
	}
	for (i = sSYNetRollbackFileRangesNum; (i > 0) && (sSYNetRollbackFileRanges[i - 1].lo > lo); i--)
	{
		sSYNetRollbackFileRanges[i] = sSYNetRollbackFileRanges[i - 1];
	}
	sSYNetRollbackFileRanges[i].lo = lo;
	sSYNetRollbackFileRanges[i].hi = hi;
	sSYNetRollbackFileRangesNum++;
}

/* Copies [watermark, watermark + size) between the live arena and a snapshot
 * image laid out at the same offsets, skipping the file ranges. */
static void syNetRollbackCopyArena(u8 *image, size_t size, sb32 is_save)
{
	u8 *live = sSYNetRollbackArenaWatermark;
	size_t cursor = 0;
	s32 i;

	for (i = 0; (i < sSYNetRollbackFileRangesNum) && (cursor < size); i++)
	{
		size_t lo = (size_t)(sSYNetRollbackFileRanges[i].lo - live);
		size_t hi = (size_t)(sSYNetRollbackFileRanges[i].hi - live);

		if (lo > size)
		{
			lo = size;
		}
		if (lo > cursor)
		{
			if (is_save != FALSE)
			{
				memcpy(image + cursor, live + cursor, lo - cursor);
			}
			else memcpy(live + cursor, image + cursor, lo - cursor);
		}
		if (hi > cursor)
		{
			cursor = hi;
		}
	}
	if (cursor < size)
	{
		if (is_save != FALSE)
		{
			memcpy(image + cursor, live + cursor, size - cursor);
		}
		else memcpy(live + cursor, image + cursor, size - cursor);
	}
}

/* GObj thread processes keep live state on host coroutine stacks, which a
 * snapshot cannot capture; only snapshot while none exist (they run during
 * the countdown, entry, announcer and sudden-death sequences). */
sb32 syNetRollbackCheckCanSave(void)
{
	if ((SYNETROLLBACK_HAS_SECTIONS == FALSE) || (sSYNetRollbackIsSessionActive == FALSE))
	{
		return FALSE;
	}
	if ((gSCManagerBattleState == NULL) || (gSCManagerBattleState->game_status != nSCBattleGameStatusGo))
	{
		return FALSE;
	}
	if (sGCThreadsActive != 0)
	{
		return FALSE;
	}
	if (sSYNetRollbackIsFileRangeOverflow != FALSE)
	{
		return FALSE;
	}
	return TRUE;
}

static sb32 syNetRollbackAllocState(SYNetRollbackState *state)
{
	if (state->data == NULL)
	{
		state->data = malloc(syNetRollbackGetDataSize() + 1);
		state->bss = malloc(syNetRollbackGetBssSize() + 1);
		state->arena = malloc(gPortSceneHeapSize);
	}
	return ((state->data != NULL) && (state->bss != NULL) && (state->arena != NULL)) ? TRUE : FALSE;
}

static sb32 syNetRollbackSaveState(SYNetRollbackState *state)
{
#if SYNETROLLBACK_HAS_SECTIONS
	size_t arena_size = (size_t)((u8*)gSYTaskmanGeneralHeap.ptr - sSYNetRollbackArenaWatermark);
	s32 i;

	if (syNetRollbackAllocState(state) == FALSE)
	{
		return FALSE;
	}
	memcpy(state->data, &gSYNetRollbackDataStart, syNetRollbackGetDataSize());
	memcpy(state->bss, &gSYNetRollbackBssStart, syNetRollbackGetBssSize());
	syNetRollbackCopyArena(state->arena, arena_size, TRUE);

#ifdef PORT
	{
		size_t aobj_size = port_aobj_fixup_snapshot_size();

		if (aobj_size > state->aobj_fixup_capacity)
		{
			u8 *grown = realloc(state->aobj_fixup, aobj_size);

			if (grown == NULL)
			{
				return FALSE;
			}
			state->aobj_fixup = grown;
			state->aobj_fixup_capacity = aobj_size;
		}
		state->aobj_fixup_size = port_aobj_fixup_snapshot_save(state->aobj_fixup, state->aobj_fixup_capacity);
	}
#endif
	state->arena_size = arena_size;
	state->heap = gSYTaskmanGeneralHeap;
	state->spawn_serial = syNetSyncGetSpawnSerial();
	state->is_valid = TRUE;

	if (arena_size > sSYNetRollbackMaxArenaBytes)
	{
		sSYNetRollbackMaxArenaBytes = arena_size;
	}
	return TRUE;
#else
	return FALSE;
#endif
}

sb32 syNetRollbackSave(s32 slot)
{
	if ((slot < 0) || (slot > SYNETROLLBACK_SLOT_NUM) || (syNetRollbackCheckCanSave() == FALSE))
	{
		return FALSE;
	}
	return syNetRollbackSaveState(&sSYNetRollbackStates[slot]);
}

void syNetRollbackLoad(s32 slot)
{
#if SYNETROLLBACK_HAS_SECTIONS
	SYNetRollbackState *state;
	s32 i;

	if ((slot < 0) || (slot > SYNETROLLBACK_SLOT_NUM))
	{
		return;
	}
	state = &sSYNetRollbackStates[slot];

	if (state->is_valid == FALSE)
	{
		return;
	}
	memcpy(&gSYNetRollbackDataStart, state->data, syNetRollbackGetDataSize());
	memcpy(&gSYNetRollbackBssStart, state->bss, syNetRollbackGetBssSize());
	syNetRollbackCopyArena(state->arena, state->arena_size, FALSE);

	gSYTaskmanGeneralHeap = state->heap;
	syNetSyncSetSpawnSerial(state->spawn_serial);

#ifdef PORT
	port_aobj_fixup_snapshot_load(state->aobj_fixup, state->aobj_fixup_size);
#endif
#endif
}

#ifdef PORT
static char sSYNetRollbackDescribeBuf[96];

static sb32 syNetRollbackDescribeIn(const u8 *addr, const void *base, size_t size, const char *what, s32 link)
{
	if ((base != NULL) && (addr >= (const u8*)base) && (addr < (const u8*)base + size))
	{
		snprintf(sSYNetRollbackDescribeBuf, sizeof(sSYNetRollbackDescribeBuf), "%s[link%d]+0x%X",
		         what, link, (unsigned)(addr - (const u8*)base));
		return TRUE;
	}
	return FALSE;
}

static sb32 syNetRollbackDescribeDObj(const u8 *addr, DObj *dobj, s32 link, s32 depth)
{
	for (; (dobj != NULL) && (depth < 64); dobj = dobj->sib_next)
	{
		AObj *aobj;

		if (syNetRollbackDescribeIn(addr, dobj, sizeof(DObj), "DObj", link) != FALSE)
		{
			return TRUE;
		}
		for (aobj = dobj->aobj; aobj != NULL; aobj = aobj->next)
		{
			if (syNetRollbackDescribeIn(addr, aobj, sizeof(AObj), "AObj", link) != FALSE)
			{
				return TRUE;
			}
		}
		if (syNetRollbackDescribeDObj(addr, dobj->child, link, depth + 1) != FALSE)
		{
			return TRUE;
		}
	}
	return FALSE;
}

/* Names the live game object an arena byte belongs to (GObj, its user struct,
 * or a DObj/AObj in its tree). Returns NULL when nothing live owns it. */
static const char *syNetRollbackDescribeArena(const u8 *addr)
{
	s32 link;

	for (link = 0; link < GC_COMMON_MAX_LINKS; link++)
	{
		GObj *gobj;

		for (gobj = gGCCommonLinks[link]; gobj != NULL; gobj = gobj->link_next)
		{
			size_t user_size = 0;
			const char *user_name = NULL;

			if (syNetRollbackDescribeIn(addr, gobj, sizeof(GObj), "GObj", link) != FALSE)
			{
				return sSYNetRollbackDescribeBuf;
			}
			switch (link)
			{
			case nGCCommonLinkIDFighter: user_size = sizeof(FTStruct); user_name = "FTStruct"; break;
			case nGCCommonLinkIDItem: user_size = sizeof(ITStruct); user_name = "ITStruct"; break;
			case nGCCommonLinkIDWeapon: user_size = sizeof(WPStruct); user_name = "WPStruct"; break;
			case nGCCommonLinkIDEffect:
			case nGCCommonLinkIDSpecialEffect: user_size = sizeof(EFStruct); user_name = "EFStruct"; break;
			default: break;
			}
			if ((user_name != NULL) && (syNetRollbackDescribeIn(addr, gobj->user_data.p, user_size, user_name, link) != FALSE))
			{
				return sSYNetRollbackDescribeBuf;
			}
			if ((gobj->obj_kind == 1) && (syNetRollbackDescribeDObj(addr, (DObj*)gobj->obj, link, 0) != FALSE))
			{
				return sSYNetRollbackDescribeBuf;
			}
		}
	}
	return NULL;
}

/* Globals written only while drawing (or audio bookkeeping). Resimulated
 * ticks are never drawn, so these legitimately differ from the first pass
 * after a multi-tick rollback; the next draw recomputes them. */
static const char *dSYNetRollbackPresentationSymbols[] =
{
	"gGCFrameQueueGfxLinks",
	"sIFCommonTimerDigitsInterface",
	"dLBParticleCurrentTransformID",
	"sFTPublicCommonOrder",
	"sFTPublicCommonALSound"
};

static sb32 syNetRollbackCheckPresentationSymbol(const char *sym)
{
	s32 i;

	if (sym == NULL)
	{
		return FALSE;
	}
	for (i = 0; i < ARRAY_COUNT(dSYNetRollbackPresentationSymbols); i++)
	{
		if (strcmp(sym, dSYNetRollbackPresentationSymbols[i]) == 0)
		{
			return TRUE;
		}
	}
	return FALSE;
}

static void syNetRollbackReportDiff(const char *region, size_t region_offset, const u8 *live, const u8 *saved, size_t size)
{
	size_t i;

	for (i = 0; (i < size) && (sSYNetRollbackDiffsReported < SYNETROLLBACK_DIFF_REPORT_MAX); i++)
	{
		if (live[i] != saved[i])
		{
			u32 sym_offset = 0;
			const char *sym = (region[0] == 'a') ? syNetRollbackDescribeArena(&live[i]) : port_symbolize(&live[i], &sym_offset);
			s32 range_id;

			if (syNetRollbackCheckPresentationSymbol(sym) != FALSE)
			{
				continue;
			}

			for (range_id = 0; range_id < sSYNetRollbackFileRangesNum; range_id++)
			{
				if ((&live[i] >= sSYNetRollbackFileRanges[range_id].lo) && (&live[i] < sSYNetRollbackFileRanges[range_id].hi))
				{
					break;
				}
			}
			port_log("SSB64 Rollback:   tick=%u %s+0x%X %s+0x%X%s pass1=%02X pass2=%02X\n",
			         sSYNetRollbackTestedTicks, region, (unsigned)(region_offset + i), (sym != NULL) ? sym : "?", sym_offset,
			         (range_id < sSYNetRollbackFileRangesNum) ? " (file)" : "", saved[i], live[i]);
			sSYNetRollbackDiffsReported++;

			/* Skip the rest of this word/struct run so one field is one line. */
			while ((i + 1 < size) && (live[i + 1] != saved[i + 1]))
			{
				i++;
			}
		}
	}
}

/* Byte-compare the live state (second pass) against the first pass's
 * post-update state to name the exact globals/arena bytes that diverged. */
static void syNetRollbackReportMemoryDiffs(void)
{
#if SYNETROLLBACK_HAS_SECTIONS
	SYNetRollbackState *first = &sSYNetRollbackStates[SYNETROLLBACK_SLOT_COMPARE];
	size_t arena_size = (size_t)((u8*)gSYTaskmanGeneralHeap.ptr - sSYNetRollbackArenaWatermark);
	size_t cursor = 0;
	s32 i;

	if (first->is_valid == FALSE)
	{
		return;
	}
	if ((arena_size != first->arena_size) && (sSYNetRollbackDiffsReported < SYNETROLLBACK_DIFF_REPORT_MAX))
	{
		sSYNetRollbackDiffsReported++;
		port_log("SSB64 Rollback:   arena bump differs pass1=%u pass2=%u\n",
		         (unsigned)first->arena_size, (unsigned)arena_size);
	}
	syNetRollbackReportDiff("data", 0, &gSYNetRollbackDataStart, first->data, syNetRollbackGetDataSize());
	syNetRollbackReportDiff("bss", 0, &gSYNetRollbackBssStart, first->bss, syNetRollbackGetBssSize());

	/* Same spans as syNetRollbackCopyArena: file ranges are not in the image. */
	if (arena_size > first->arena_size)
	{
		arena_size = first->arena_size;
	}
	for (i = 0; (i <= sSYNetRollbackFileRangesNum) && (cursor < arena_size); i++)
	{
		size_t lo = arena_size;
		size_t hi = arena_size;

		if (i < sSYNetRollbackFileRangesNum)
		{
			lo = (size_t)(sSYNetRollbackFileRanges[i].lo - sSYNetRollbackArenaWatermark);
			hi = (size_t)(sSYNetRollbackFileRanges[i].hi - sSYNetRollbackArenaWatermark);

			if (lo > arena_size)
			{
				lo = arena_size;
			}
		}
		if (lo > cursor)
		{
			syNetRollbackReportDiff("arena", cursor, sSYNetRollbackArenaWatermark + cursor, first->arena + cursor, lo - cursor);
		}
		if (hi > cursor)
		{
			cursor = hi;
		}
	}
#endif
}
#endif

/* SyncTest: save, run the tick, hash; restore, run the same tick again, hash.
 * Any divergence means the snapshot misses state (or the tick is not a pure
 * function of snapshot + published input). Inputs for the tick are already
 * published into gSYControllerDevices, which lives outside the snapshot. */
#ifdef PORT
static void syNetRollbackTraceDObjs(DObj *dobj, s32 depth, const char *phase)
{
	for (; (dobj != NULL) && (depth < 64); dobj = dobj->sib_next)
	{
		if (dobj->anim_joint.event16 != NULL)
		{
			u16 *ev = (u16*)dobj->anim_joint.event16;

			port_log("SSB64 RollbackTrace: %s dobj=+0x%X ev=%p wait=%f speed=%f frame=%f words=%04X %04X %04X %04X\n",
			         phase, (unsigned)((u8*)dobj - sSYNetRollbackArenaWatermark), (void*)ev,
			         dobj->anim_wait, dobj->anim_speed, dobj->anim_frame, ev[0], ev[1], ev[2], ev[3]);
		}
		syNetRollbackTraceDObjs(dobj->child, depth + 1, phase);
	}
}

/* SSB64_ROLLBACK_TRACE_TICK=N: dump every fighter joint's animation cursor
 * around SyncTest tick N, to localize a pass1/pass2 divergence. */
static void syNetRollbackTraceTick(const char *phase)
{
	GObj *gobj;

	if ((sSYNetRollbackTraceTick == 0) || (sSYNetRollbackTestedTicks + 1 != sSYNetRollbackTraceTick))
	{
		return;
	}
	for (gobj = gGCCommonLinks[nGCCommonLinkIDFighter]; gobj != NULL; gobj = gobj->link_next)
	{
		if (gobj->obj_kind == 1)
		{
			syNetRollbackTraceDObjs((DObj*)gobj->obj, 0, phase);
		}
	}
}
#else
#define syNetRollbackTraceTick(phase)
#endif

/* Hash every gated/diag column and fold the mismatches into the session
 * counters. Returns TRUE when a gated column diverged. */
static sb32 syNetRollbackCompareTick(u32 tick, const SYNetSyncTickHash *expected, const SYNetSyncTickHash *actual)
{
	u32 diff_mask = 0;
	s32 column;

	for (column = 0; column < SYNETSYNC_COLUMN_NUM; column++)
	{
		if (expected->column[column] != actual->column[column])
		{
			diff_mask |= (1U << column);
		}
	}
#ifdef PORT
	if (diff_mask & ~sSYNetRollbackColumnReported)
	{
		port_log("SSB64 Rollback: SyncTest divergence sim_tick=%u tested_tick=%u game_tic=%u mask=0x%X\n",
		         tick, sSYNetRollbackTestedTicks, (unsigned)gSCManagerBattleState->time_passed, diff_mask);

		for (column = 0; column < SYNETSYNC_COLUMN_NUM; column++)
		{
			if ((diff_mask & ~sSYNetRollbackColumnReported) & (1U << column))
			{
				port_log("SSB64 Rollback:   column %s first=%08X resim=%08X\n",
				         syNetSyncGetColumnName(column), expected->column[column], actual->column[column]);
			}
		}
		sSYNetRollbackColumnReported |= diff_mask;
	}
#endif
	return (diff_mask & SYNETSYNC_GATED_COLUMN_MASK) ? TRUE : FALSE;
}

/* One simulated tick, first run or resimulated. Players who dropped out of the
 * match are knocked out at their agreed tick (netpeer.c): here, after the
 * snapshot of the tick was taken, so a rollback to or before it replays the
 * knock-out on every peer alike. */
static void syNetRollbackRunTick(void (*update)(void), u32 tick)
{
	syNetPeerApplyDrops(tick);
	update();
}

/* Saves the pre-update state of sim tick `tick` into its ring slot. */
static sb32 syNetRollbackSaveTick(u32 tick)
{
	s32 slot = tick % SYNETROLLBACK_SLOT_NUM;

	if (syNetRollbackSave(slot) == FALSE)
	{
		return FALSE;
	}
	sSYNetRollbackSlotTicks[slot] = tick;

	return TRUE;
}

/* Rewinds to the snapshot of `first_tick` and re-runs every tick through
 * `last_tick` with its inputs re-published, re-saving each tick's snapshot
 * on the way (as live rollback must, since resimulated states replace the
 * predicted ones). `hashes`, when set, receives each resimulated tick's
 * hash indexed by tick % SYNETROLLBACK_SLOT_NUM. Returns FALSE (and leaves
 * the state untouched) if the snapshot chain does not cover the range. */
sb32 syNetRollbackResimulate(u32 first_tick, u32 last_tick, void (*update)(void), SYNetSyncTickHash *hashes)
{
	u32 tick;

	if ((last_tick < first_tick) || (last_tick - first_tick >= SYNETROLLBACK_SLOT_NUM) ||
	    (first_tick < sSYNetRollbackChainStart))
	{
		return FALSE;
	}
	if ((sSYNetRollbackStates[first_tick % SYNETROLLBACK_SLOT_NUM].is_valid == FALSE) ||
	    (sSYNetRollbackSlotTicks[first_tick % SYNETROLLBACK_SLOT_NUM] != first_tick))
	{
		return FALSE;
	}
	syNetRollbackLoad(first_tick % SYNETROLLBACK_SLOT_NUM);

	sSYNetRollbackIsResimulating = TRUE;

	for (tick = first_tick; tick <= last_tick; tick++)
	{
		if (tick != first_tick)
		{
			syNetRollbackSaveTick(tick);
		}
		syNetInputRepublishTick(tick);
		syNetRollbackRunTick(update, tick);

		/* Stand in for the draw that followed this tick originally: draw
		 * passes produce state the next tick's gameplay reads. The last tick
		 * is drawn normally by the taskman loop afterwards. */
		if (tick != last_tick)
		{
			syTaskmanShadowDraw();
		}

		if (hashes != NULL)
		{
			syNetSyncHashTick(&hashes[tick % SYNETROLLBACK_SLOT_NUM]);
		}
	}
	sSYNetRollbackIsResimulating = FALSE;

	return TRUE;
}

static void syNetRollbackStoreLiveHash(u32 tick)
{
	s32 slot = tick % SYNETROLLBACK_SLOT_NUM;

	syNetSyncHashTick(&sSYNetRollbackLiveHashes[slot]);
	sSYNetRollbackLiveHashTicks[slot] = tick;
	sSYNetRollbackLiveHashValid[slot] = TRUE;
}

/* Writes the hash of every tick that can no longer change: simulated, and
 * every remote input up to it confirmed. */
static void syNetRollbackEmitConfirmedTrace(u32 last_simulated_tick)
{
	u32 confirmed = syNetPeerGetConfirmedTick();

#ifdef PORT
	if ((sSYNetRollbackConfirmedTrace == NULL) && (sSYNetRollbackConfirmedTracePath == NULL) &&
	    (sSYNetRollbackIsDefaultTraceTried == FALSE))
	{
		/* Online matches always keep a trace next to the game, so a desync
		 * between two players' PCs can be diffed afterwards. */
		sSYNetRollbackIsDefaultTraceTried = TRUE;
		sSYNetRollbackConfirmedTrace = fopen("logs/netplay_trace.txt", "w");
	}
#endif
	while ((sSYNetRollbackTraceNextTick <= last_simulated_tick) && (sSYNetRollbackTraceNextTick < confirmed))
	{
		u32 tick = sSYNetRollbackTraceNextTick;
		s32 slot = tick % SYNETROLLBACK_SLOT_NUM;

		if ((sSYNetRollbackLiveHashValid[slot] != FALSE) && (sSYNetRollbackLiveHashTicks[slot] == tick))
		{
			s32 ring = tick % SYNETROLLBACK_CONFIRMED_RING;

			sSYNetRollbackConfirmedTicks[ring] = tick;
			sSYNetRollbackConfirmedHashes[ring] = sSYNetRollbackLiveHashes[slot].column[nSYNetSyncColumnFull];
#ifdef PORT
			sSYNetRollbackConfirmedColumns[ring] = sSYNetRollbackLiveHashes[slot];
#endif
			sSYNetRollbackConfirmedValid[ring] = TRUE;
			sSYNetRollbackLatestConfirmedTick = tick;
			sSYNetRollbackHasConfirmed = TRUE;
		}

#ifdef PORT
		if (sSYNetRollbackConfirmedTrace != NULL)
		{
			if ((sSYNetRollbackLiveHashValid[slot] != FALSE) && (sSYNetRollbackLiveHashTicks[slot] == tick))
			{
				SYNetSyncTickHash *hash = &sSYNetRollbackLiveHashes[slot];

				fprintf(sSYNetRollbackConfirmedTrace, "%u %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X\n", tick,
				        hash->column[nSYNetSyncColumnFull], hash->column[nSYNetSyncColumnRNG],
				        hash->column[nSYNetSyncColumnBattle], hash->column[nSYNetSyncColumnFighters],
				        hash->column[nSYNetSyncColumnItems], hash->column[nSYNetSyncColumnWeapons],
				        hash->column[nSYNetSyncColumnStage], hash->column[nSYNetSyncColumnObjMan],
				        hash->column[nSYNetSyncColumnInput], hash->column[nSYNetSyncColumnJoints],
				        hash->column[nSYNetSyncColumnCamera], hash->column[nSYNetSyncColumnVars]);
			}
			else fprintf(sSYNetRollbackConfirmedTrace, "%u missing\n", tick);

			if ((tick % 60) == 0)
			{
				fflush(sSYNetRollbackConfirmedTrace);
			}
		}
#endif
		sSYNetRollbackTraceNextTick++;
	}
}

/* Live rollback netplay. Before running tick T: if a confirmed remote input
 * contradicted a prediction at tick E < T, rewind to E's snapshot and
 * resimulate E..T-1 with the corrected inputs; then re-resolve T's inputs
 * (its prediction may have improved), snapshot T and run it. */
static void syNetRollbackRunNetplayUpdate(void (*update)(void))
{
	u32 tick = syNetInputGetTick() - 1;
	u32 mispredict_tick;

	if (syNetInputTakeMispredictTick(&mispredict_tick) != FALSE)
	{
		if (mispredict_tick < tick)
		{
			if (syNetRollbackResimulate(mispredict_tick, tick - 1, update, sSYNetRollbackLiveHashes) != FALSE)
			{
				u32 resim;

				for (resim = mispredict_tick; resim < tick; resim++)
				{
					sSYNetRollbackLiveHashTicks[resim % SYNETROLLBACK_SLOT_NUM] = resim;
					sSYNetRollbackLiveHashValid[resim % SYNETROLLBACK_SLOT_NUM] = TRUE;
				}
				sSYNetRollbackRollbacks++;
				sSYNetRollbackResimTicks += tick - mispredict_tick;

				if ((tick - mispredict_tick) > sSYNetRollbackMaxRollback)
				{
					sSYNetRollbackMaxRollback = tick - mispredict_tick;
				}
			}
			else
			{
				/* Pacing keeps every mispredicted tick inside the snapshot
				 * window, so this is a bug: the peers will desync. */
				sSYNetRollbackFailedRollbacks++;
#ifdef PORT
				port_log("SSB64 Rollback: netplay rollback FAILED tick=%u mispredict=%u chain_start=%u\n",
				         tick, mispredict_tick, sSYNetRollbackChainStart);
#endif
				if (syNetPeerHasDrops() != FALSE)
				{
					syNetPeerAbortMatch("Lost sync with the other players");
				}
			}
		}
		syNetInputRepublishTick(tick);
	}
	if (syNetRollbackSaveTick(tick) == FALSE)
	{
		sSYNetRollbackChainStart = tick + 1;
	}
#ifdef PORT
	/* SSB64_NETPLAY_TEST_DESYNC=<tick>: corrupt this peer's RNG once, to
	 * prove the in-band desync detector fires. Test harness only. */
	{
		static s32 sInjectTick = -2;

		if (sInjectTick == -2)
		{
			sInjectTick = syNetRollbackGetEnvInt("SSB64_NETPLAY_TEST_DESYNC", -1);
		}
		if ((sInjectTick >= 0) && (tick == (u32)sInjectTick))
		{
			port_log("SSB64 Rollback: TEST injecting desync at tick=%u\n", tick);
			syUtilsSetRandomSeed(syUtilsRandSeed() ^ 0x5A5A5A5AU);
			sInjectTick = -1;
		}
	}
#endif
	syNetRollbackRunTick(update, tick);
	syNetRollbackStoreLiveHash(tick);
#ifdef PORT
	/* Readable state at a few early ticks: both players' logs can be diffed
	 * line by line when a match desyncs from the start. */
	if ((tick == 0) || (tick == 1) || (tick == 60) || (tick == 300))
	{
		syNetSyncLogState(tick);
	}
#endif
	syNetRollbackEmitConfirmedTrace(tick);

#ifdef PORT
	if ((tick % 600) == 0)
	{
		port_log("SSB64 Rollback: netplay tick=%u rollbacks=%u resim_ticks=%u max_rollback=%u failed=%u confirmed=%u sync_checks=%u desync=%d\n",
		         tick, sSYNetRollbackRollbacks, sSYNetRollbackResimTicks, sSYNetRollbackMaxRollback,
		         sSYNetRollbackFailedRollbacks, syNetPeerGetConfirmedTick(),
		         sSYNetRollbackSyncChecks, sSYNetRollbackIsDesynced);
	}
#endif
}

/* SyncTest (SSB64_ROLLBACK_SYNCTEST=N): every tick runs normally, then the
 * last N ticks (this one included) are rolled back and resimulated from the
 * snapshot ring with the same inputs; each resimulated tick must hash the
 * same as its first run. Any divergence means the snapshot misses state or
 * the tick is not a pure function of snapshot + published input. */
sb32 syNetRollbackGetLatestConfirmedHash(u32 *out_tick, u32 *out_columns)
{
	/* Report a tick a little behind our newest confirmed one: the peer
	 * confirms ticks on its own schedule, and a hash for a tick it hasn't
	 * confirmed yet can't be checked. Both keep 128 ticks of history. */
	u32 tick;
	s32 ring;

	if ((sSYNetRollbackHasConfirmed == FALSE) || (sSYNetRollbackLatestConfirmedTick < SYNETROLLBACK_SYNC_CHECK_LAG))
	{
		return FALSE;
	}
	tick = sSYNetRollbackLatestConfirmedTick - SYNETROLLBACK_SYNC_CHECK_LAG;
	ring = tick % SYNETROLLBACK_CONFIRMED_RING;

	if ((sSYNetRollbackConfirmedValid[ring] == FALSE) || (sSYNetRollbackConfirmedTicks[ring] != tick))
	{
		return FALSE;
	}
	*out_tick = tick;
	{
		s32 c;

		for (c = 0; c < SYNETSYNC_COLUMN_NUM; c++)
		{
#ifdef PORT
			out_columns[c] = sSYNetRollbackConfirmedColumns[ring].column[c];
#else
			out_columns[c] = (c == nSYNetSyncColumnFull) ? sSYNetRollbackConfirmedHashes[ring] : 0;
#endif
		}
	}

	return TRUE;
}

/* The peer's full-state hash for a tick it has confirmed. Ours must match;
 * a mismatch means the simulations diverged (a determinism bug). */
void syNetRollbackCheckRemoteConfirmedHash(s32 player, u32 tick, const u32 *columns)
{
	u32 hash = columns[nSYNetSyncColumnFull];
	s32 ring = tick % SYNETROLLBACK_CONFIRMED_RING;

	if ((sSYNetRollbackConfirmedValid[ring] == FALSE) || (sSYNetRollbackConfirmedTicks[ring] != tick))
	{
		return;     // not confirmed here yet, or already rotated out
	}
	sSYNetRollbackSyncChecks++;

	if ((sSYNetRollbackConfirmedHashes[ring] != hash) && (sSYNetRollbackIsDesynced == FALSE))
	{
		sSYNetRollbackIsDesynced = TRUE;
		sSYNetRollbackDesyncTick = tick;
#ifdef PORT
		port_log("SSB64 Rollback: DESYNC detected at tick=%u local=%08X remote=%08X (player %d)\n",
		         tick, sSYNetRollbackConfirmedHashes[ring], hash, player);
		{
			const SYNetSyncTickHash *cols = &sSYNetRollbackConfirmedColumns[ring];
			s32 c;

			for (c = 0; c < SYNETSYNC_COLUMN_NUM; c++)
			{
				port_log("SSB64 Rollback: DESYNC tick=%u column %-8s local=%08X remote=%08X%s\n", tick,
				         syNetSyncGetColumnName(c), cols->column[c], columns[c],
				         (cols->column[c] != columns[c]) ? "  <-- DIFFERS" : "");
			}
		}
#endif
	}
}

sb32 syNetRollbackIsDesynced(void)
{
	return sSYNetRollbackIsDesynced;
}

s32 syNetRollbackTakeDrawRequest(void)
{
	s32 request;

	if ((sSYNetRollbackIsSessionActive == FALSE) ||
	    ((sSYNetRollbackIsSyncTest == FALSE) && (syNetPeerIsRollbackActive() == FALSE)))
	{
		return nSYNetRollbackDrawNormal;
	}
	request = (sSYNetRollbackIsTickRan != FALSE) ? nSYNetRollbackDrawRequired : nSYNetRollbackDrawSkip;
	sSYNetRollbackIsTickRan = FALSE;

	return request;
}

void syNetRollbackRunBattleUpdate(void (*update)(void))
{
	SYNetSyncTickHash resim_hashes[SYNETROLLBACK_SLOT_NUM];

	sSYNetRollbackIsTickRan = TRUE;
	u32 tick;
	u32 first_tick;
	u32 resim_tick;
	sb32 is_failed;

	if (syNetPeerIsRollbackActive() != FALSE)
	{
		syNetRollbackRunNetplayUpdate(update);
		return;
	}
	if (sSYNetRollbackIsSyncTest == FALSE)
	{
		update();
		return;
	}
	tick = syNetInputGetTick() - 1;    // syNetInputFuncRead already advanced past this tick

	if (syNetRollbackSaveTick(tick) == FALSE)
	{
		/* Unsnapshottable tick (thread process alive, not in Go): the chain of
		 * contiguous snapshots restarts after it. */
		sSYNetRollbackChainStart = tick + 1;
		sSYNetRollbackSkippedTicks++;
		update();
		return;
	}
	syNetRollbackTraceTick("pre1");
	update();
	syNetRollbackTraceTick("post1");
	syNetSyncHashTick(&sSYNetRollbackFirstHashes[tick % SYNETROLLBACK_SLOT_NUM]);
	syNetRollbackSaveState(&sSYNetRollbackStates[SYNETROLLBACK_SLOT_COMPARE]);

	first_tick = ((tick + 1) >= sSYNetRollbackSyncTestDistance) ? (tick + 1 - sSYNetRollbackSyncTestDistance) : 0;

	if (first_tick < sSYNetRollbackChainStart)
	{
		first_tick = sSYNetRollbackChainStart;
	}
	if (syNetRollbackResimulate(first_tick, tick, update, resim_hashes) == FALSE)
	{
		sSYNetRollbackSkippedTicks++;
		return;
	}
	syNetRollbackTraceTick("post2");
	sSYNetRollbackTestedTicks++;

	is_failed = FALSE;

	for (resim_tick = first_tick; resim_tick <= tick; resim_tick++)
	{
		if (syNetRollbackCompareTick(resim_tick, &sSYNetRollbackFirstHashes[resim_tick % SYNETROLLBACK_SLOT_NUM],
		                             &resim_hashes[resim_tick % SYNETROLLBACK_SLOT_NUM]) != FALSE)
		{
			is_failed = TRUE;
		}
	}
	if (is_failed != FALSE)
	{
		sSYNetRollbackFailedTicks++;
	}
#ifdef PORT
	/* The hashes cover a subset of state; the raw compare catches the rest.
	 * After a multi-tick rollback, draw-written state (matrices, frame
	 * queues) differs legitimately, so then only report failing ticks. */
	if ((sSYNetRollbackDiffsReported < SYNETROLLBACK_DIFF_REPORT_MAX) && ((first_tick == tick) || (is_failed != FALSE)))
	{
		syNetRollbackReportMemoryDiffs();
	}
#endif
}
