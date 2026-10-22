#ifndef _SYNETPEER_H_
#define _SYNETPEER_H_

#include <PR/ultratypes.h>
#include <ssb_types.h>
#include <sys/netinput.h>

extern void syNetPeerInitDebugEnv(void);
extern void syNetPeerStartVSSession(void);
extern void syNetPeerBeginFrame(void);
extern sb32 syNetPeerCheckBattleExecutionReady(void);
extern sb32 syNetPeerIsRollbackActive(void);
extern u32 syNetPeerGetConfirmedTick(void);
extern s32 syNetPeerGetRemoteCount(void);
extern s32 syNetPeerGetSilentPlayer(u32 threshold_ms, u32 *out_silent_ms);
extern sb32 syNetPeerCheckStartBarrierReleased(void);
extern void syNetPeerUpdateBattleGate(void);
extern void syNetPeerUpdate(void);
extern void syNetPeerStopVSSession(void);
extern void syNetPeerStartOnline(s32 local_player, s32 player_count, const s32 *transports, u32 delay, sb32 is_host, const SYNetInputReplayMetadata *metadata);
extern void syNetPeerMakeOnlineMetadata(SYNetInputReplayMetadata *metadata, s32 player_count, u32 stage, u32 stocks, sb32 is_items, u32 seed, const u8 *fighter_kinds, const u8 *costumes);

#endif /* _SYNETPEER_H_ */
