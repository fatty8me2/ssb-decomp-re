#ifndef _SYNETROLLBACK_H_
#define _SYNETROLLBACK_H_

#include <PR/ultratypes.h>
#include <ssb_types.h>
#include <stddef.h>
#include <sys/netsync.h>

/* Rollback snapshot/restore for VS battles. See docs/rollback_design.md.
 *
 * A snapshot is: every global of the gameplay modules (grouped into one PE
 * section by port/netrollback_seg.h, which CMake force-includes into those
 * TUs), the general heap allocator, and the scene arena from the VS setup
 * watermark up to the allocator's bump pointer. */

#define SYNETROLLBACK_SLOT_NUM 8

/* What the taskman loop must do about drawing after an update. */
enum SYNetRollbackDraw
{
	nSYNetRollbackDrawNormal,       // not a rollback session: draw as the scene always has
	nSYNetRollbackDrawRequired,     // a tick ran: draw it (shadow draw if no context is free)
	nSYNetRollbackDrawSkip          // no tick ran (stalled peer): drawing would repeat side effects
};

extern void syNetRollbackInitDebugEnv(void);
extern void syNetRollbackStartVSSession(void);
extern void syNetRollbackContinueTraceAt(u32 tick);
extern void syNetRollbackFinishVSSession(void);

extern void syNetRollbackNoteFileLoad(void *dst, size_t size, sb32 is_figatree);

extern sb32 syNetRollbackIsTickTimer(void);
extern void syNetRollbackSetTickTimer(sb32 is_tick_timer);
extern void syNetRollbackRestoreTickTimer(void);
extern sb32 syNetRollbackIsResimulating(void);
extern sb32 syNetRollbackCheckCanSave(void);
extern sb32 syNetRollbackSave(s32 slot);
extern void syNetRollbackLoad(s32 slot);

extern sb32 syNetRollbackResimulate(u32 first_tick, u32 last_tick, void (*update)(void), SYNetSyncTickHash *hashes);
extern void syNetRollbackRunBattleUpdate(void (*update)(void));
extern s32 syNetRollbackTakeDrawRequest(void);
/* Sync check: every hash column (SYNETSYNC_COLUMN_NUM u32s, full first) of
 * a confirmed tick, so a mismatch names the part of the state that differs. */
extern sb32 syNetRollbackGetLatestConfirmedHash(u32 *out_tick, u32 *out_columns);
extern void syNetRollbackCheckRemoteConfirmedHash(s32 player, u32 tick, const u32 *columns);
extern sb32 syNetRollbackIsDesynced(void);
extern void syTaskmanShadowDraw(void);

#endif /* _SYNETROLLBACK_H_ */
