#include <sys/netpeer.h>

#include <ft/fighter.h>
#include <gr/ground.h>
#include <it/item.h>
#include <sc/scmanager.h>
#include <sys/netinput.h>
#include <sys/netreplay.h>
#include <sys/netrollback.h>
#include <sys/netsync.h>
#include <sys/utils.h>

#ifdef PORT
extern char *getenv(const char *name);
extern int atoi(const char *s);
extern void port_log(const char *fmt, ...);
#endif

#ifdef PORT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <net/port_udp.h>
#endif

#define SYNETPEER_MAGIC 0x53534E50 // SSNP
#define SYNETPEER_VERSION 5
#define SYNETPEER_MAX_PACKET_FRAMES 16
#define SYNETPEER_FRAME_BYTES 8
#define SYNETPEER_INPUT_HEADER_BYTES (4 + 2 + 2 + 4 + 4 + 1 + 1 + (2 * MAXCONTROLLERS))
#define SYNETPEER_SYNC_CHECK_BYTES (4 + (4 * SYNETSYNC_COLUMN_NUM))
#define SYNETPEER_PACKET_BYTES ((SYNETPEER_INPUT_HEADER_BYTES) + ((SYNETPEER_MAX_PACKET_FRAMES) * (SYNETPEER_FRAME_BYTES)) + SYNETPEER_SYNC_CHECK_BYTES + 4)
#define SYNETPEER_VALIDATION_INPUT_WINDOW 120
#define SYNETPEER_METADATA_BYTES (11 * 4 + 8 + (MAXCONTROLLERS * 7))
#define SYNETPEER_BOOTSTRAP_PACKET_BYTES (4 + 2 + 2 + 4 + SYNETPEER_METADATA_BYTES + 4)
#define SYNETPEER_CONTROL_PACKET_BYTES (4 + 2 + 2 + 4 + 4)
#define SYNETPEER_DEFAULT_INPUT_DELAY 2
#define SYNETPEER_DEFAULT_SESSION_ID 1
#define SYNETPEER_DEFAULT_BOOTSTRAP_SEED 12345
#define SYNETPEER_LOG_INTERVAL 120
#define SYNETPEER_BOOTSTRAP_RETRY_COUNT 300
#define SYNETPEER_BOOTSTRAP_RETRY_USECS 16666
#define SYNETPEER_BARRIER_LOG_INTERVAL 30
#define SYNETPEER_BATTLE_START_REPEAT_FRAMES 60
#define SYNETPEER_STANDIN_GIVEUP_FRAMES 900     // a stand-in barrier master (host migration) starts skipping players the others agreed dropped
#define SYNETPEER_STANDIN_ABORT_FRAMES 18000    // ...and gives up on the whole barrier after this long (5 min at 60 fps)

/* Barrier chatter: READY goes out every 4th frame and the host's START every
 * other frame (every frame would cost the relay 60 messages a second per
 * player for nothing). Replies to a waiting peer are immediate. */
#define SYNETPEER_BARRIER_READY_INTERVAL 4

/* Every other player in the match is a remote: a full mesh, each remote
 * sends us its own input directly (or through the lobby relay). */
#define SYNETPEER_MAX_REMOTES (MAXCONTROLLERS - 1)

/* Rollback pacing: how many ticks a peer may simulate past the newest
 * contiguously confirmed remote input (of the slowest remote). Resimulation
 * must reach back to the first mispredicted tick through the snapshot ring,
 * so this stays below SYNETROLLBACK_SLOT_NUM. */
#define SYNETPEER_MAX_PREDICTION 6

/* Time sync: each packet carries the sender's frame advantage (ticks it runs
 * ahead of the newest remote input it has confirmed). The peer that is
 * further ahead of any remote skips one frame at a time, at most every N
 * frames, so the players share the rollback load instead of one peer
 * predicting at the cap. */
#define SYNETPEER_TIMESYNC_THRESHOLD 2
#define SYNETPEER_TIMESYNC_INTERVAL 10

void syNetPeerReceiveRemoteInput(void);
void syNetPeerUpdateStartBarrier(void);
void syNetPeerSendLocalInput(void);
#ifdef PORT
static void syNetPeerResetDrops(void);
void syNetPeerAbortMatch(const char *why);
static void syNetPeerUpdateDrops(void);
#endif

#define SYNETPEER_PACKET_INPUT 0
#define SYNETPEER_PACKET_MATCH_CONFIG 1
#define SYNETPEER_PACKET_READY 2
#define SYNETPEER_PACKET_START 3
#define SYNETPEER_PACKET_BATTLE_READY 4
#define SYNETPEER_PACKET_BATTLE_START 5
#define SYNETPEER_PACKET_DROP 6

/* Drop-outs (a player quits or loses its connection mid-match). The
 * survivors agree on the tick T from which the player is gone: every one of
 * them keeps playing its neutral input and the fighter is knocked out at T.
 * See docs/bugs/netplay_mid_match_drop_2026-10-22.md. */
#define SYNETPEER_DROP_SILENCE_MS 3000      // nothing heard from a remote for this long: suspect it dropped
#define SYNETPEER_DROP_RESEND_FRAMES 12     // REPORT / DECIDE are repeated this often until acknowledged
#define SYNETPEER_DROP_REQUEST_FRAMES 6     // how often missing input of the dropped player is requested
#define SYNETPEER_DROP_FRAMES 8             // input frames per FORWARD packet
#define SYNETPEER_DROP_NO_TICK 0xFFFFFFFFU
#define SYNETPEER_DROP_PACKET_BYTES (4 + 2 + 2 + 4 + 4 + 4 + 4 + (SYNETPEER_DROP_FRAMES * 8) + 4)
#define SYNETPEER_DROP_NOTIFY_EVERY 5       // every Nth resend also tells the dropped player itself (best effort, never stops)
#define SYNETPEER_DROP_MAX_AHEAD 600        // a drop tick further ahead of our own tick than this is not believed
#define SYNETPEER_DROP_REWIND_MAX 7         // the snapshot ring (8 slots) reaches this far back from our newest tick
#define SYNETPEER_DROP_RESUME_GAP_MS 1500   // our own loop stalled this long: forgive everybody's silence

#define SYNETPEER_DROP_STATE_NONE 0
#define SYNETPEER_DROP_STATE_SUSPECT 1      // someone noticed it is gone; collecting every survivor's REPORT
#define SYNETPEER_DROP_STATE_DECIDED 2      // the drop tick is agreed

#define SYNETPEER_DROP_KIND_REPORT 1        // A = how much of its input the sender holds, B = agreed tick or none
#define SYNETPEER_DROP_KIND_DECIDE 2        // A = agreed drop tick
#define SYNETPEER_DROP_KIND_ACK 3           // A = agreed drop tick
#define SYNETPEER_DROP_KIND_REQUEST 4       // A = first missing tick of the dropped player, B = drop tick
#define SYNETPEER_DROP_KIND_FORWARD 5       // up to SYNETPEER_DROP_FRAMES of the dropped player's inputs

typedef struct SYNetPeerPacketFrame
{
	u32 tick;
	u16 buttons;
	s8 stick_x;
	s8 stick_y;

} SYNetPeerPacketFrame;

/* Per-remote-player transport state. */
typedef struct SYNetPeerRemote
{
	s32 player;                 // the remote's game slot
	s32 transport;              // port_udp peer index (its lobby slot)
	sb32 is_host;               // the match host (game slot 0)
	u32 highest_tick;           // newest input tick received from it
	u32 recv_seq_high_water;
	sb32 recv_seq_initialized;
	u32 seq_gaps;
	u32 seq_duplicates;
	u32 seq_out_of_order;
	u32 last_packet_oldest_tick;
	u32 last_packet_newest_tick;
	sb32 last_packet_ticks_valid;
	s32 advantage;              // frames it runs ahead of our input (from its packets)
	sb32 battle_ready;          // in-battle barrier: it reached the battle
	sb32 battle_start;          // in-battle barrier: it released
	u32 packets_received;
	u32 last_heard_ms;          // port_udp_now_ms() of the newest datagram from it
	sb32 dropped;               // the survivors agreed it left the match: no more traffic with it
	sb32 left_hint;             // the lobby reported it left the room
	sb32 barrier_skip;          // a stand-in barrier master gave up waiting for it (the barrier only; the drop still goes through the agreement)

} SYNetPeerRemote;

/* Drop-out agreement for one subject (game slot). */
typedef struct SYNetPeerDrop
{
	s32 state;
	u32 report;                          // how much of the subject's input we held when we noticed (frozen)
	u32 tick;                            // the agreed drop tick
	sb32 have_report[MAXCONTROLLERS];    // per survivor: its REPORT arrived
	u32 reports[MAXCONTROLLERS];
	u32 known[MAXCONTROLLERS];           // the drop tick a survivor already knew, or none
	sb32 acked[MAXCONTROLLERS];          // per survivor: it knows the agreed tick
	u32 first_want_ms;                   // test knob: when we first wanted to suspect it

} SYNetPeerDrop;

sb32 sSYNetPeerIsEnabled;
sb32 sSYNetPeerIsConfigured;
sb32 sSYNetPeerIsActive;
s32 sSYNetPeerLocalPlayer;
SYNetPeerRemote sSYNetPeerRemotes[SYNETPEER_MAX_REMOTES];
s32 sSYNetPeerRemoteCount;
u32 sSYNetPeerInputDelay;
u32 sSYNetPeerSessionID;
u32 sSYNetPeerPacketsSent;
u32 sSYNetPeerPacketsReceived;
u32 sSYNetPeerPacketsDropped;
u32 sSYNetPeerFramesStaged;
u32 sSYNetPeerLateFrames;
u32 sSYNetPeerInputChecksum;
u32 sSYNetPeerLastLogTick;
u32 sSYNetPeerSendSeq;               // one sequence for the broadcast input packets
u32 sSYNetPeerSendPhase;             // advances once per frame; relayed peers hear us every other frame
sb32 sSYNetPeerBootstrapIsEnabled;
sb32 sSYNetPeerBootstrapIsHost;
sb32 sSYNetPeerBootstrapMetadataApplied;
sb32 sSYNetPeerBootstrapPeerReady;
sb32 sSYNetPeerBootstrapStartReceived;
u32 sSYNetPeerBootstrapSeed;
SYNetInputReplayMetadata sSYNetPeerBootstrapMetadata;
sb32 sSYNetPeerBattleBarrierEnabled;
sb32 sSYNetPeerBattleLocalReady;
sb32 sSYNetPeerBattleStartSent;
sb32 sSYNetPeerBattleStartReplyNeeded;   // a peer still in the barrier asked (READY) after we were released
sb32 sSYNetPeerBattleBarrierReleased;
u32 sSYNetPeerBattleBarrierWaitFrames;
u32 sSYNetPeerBattleStartRepeatFrames;
u32 sSYNetPeerExecutionHoldFrames;
sb32 sSYNetPeerIsHoldRandSaved;
s32 sSYNetPeerHoldRandSeed;
sb32 sSYNetPeerIsTestHold;
sb32 sSYNetPeerExecutionBeginLogged;

sb32 sSYNetPeerIsRollback;           // SSB64_NETPLAY_ROLLBACK (default on)
u32 sSYNetPeerFuzzSeed;              // SSB64_NETPLAY_FUZZ
sb32 sSYNetPeerFrameReady;           // decided once per frame in syNetPeerBeginFrame
u32 sSYNetPeerStallFrames;
u32 sSYNetPeerLockstepStallFrames;
u32 sSYNetPeerTimeSyncStallFrames;
u32 sSYNetPeerFramesSinceTimeSync;
sb32 sSYNetPeerIsSocketExternal;     // the lobby owns the UDP endpoint; don't close it
SYNetPeerDrop sSYNetPeerDrops[MAXCONTROLLERS];   // drop-out agreement per subject (game slot)
sb32 sSYNetPeerDropArmed;            // the match is running: silence counts from here on
u32 sSYNetPeerDropLastPollMs;
u32 sSYNetPeerDropResumeMs;          // forgive silence before this time (our own loop was stalled)
u32 sSYNetPeerDropEventMs;           // newest drop-out state change, for the on-screen notice
sb32 sSYNetPeerSelfDropped;          // the others agreed that we are the one who dropped
const char *sSYNetPeerAbortReason;   // the match is being ended because it can no longer stay in sync

#ifdef PORT
const char *sSYNetPeerBindSpec;
const char *sSYNetPeerPeerSpec;
#endif

u32 syNetPeerChecksumAccumulateU32(u32 checksum, u32 value)
{
	checksum ^= value;
	checksum *= 16777619U;

	return checksum;
}

u32 syNetPeerChecksumAccumulateFrame(u32 checksum, const SYNetPeerPacketFrame *frame)
{
	checksum = syNetPeerChecksumAccumulateU32(checksum, frame->tick);
	checksum = syNetPeerChecksumAccumulateU32(checksum, frame->buttons);
	checksum = syNetPeerChecksumAccumulateU32(checksum, (u8)frame->stick_x);
	checksum = syNetPeerChecksumAccumulateU32(checksum, (u8)frame->stick_y);

	return checksum;
}

u32 syNetPeerChecksumPacket(u32 session_id, u32 packet_seq, u8 player, u8 frame_count, const s16 *leads, const SYNetPeerPacketFrame *frames)
{
	u32 checksum = 2166136261U;
	s32 i;

	checksum = syNetPeerChecksumAccumulateU32(checksum, SYNETPEER_MAGIC);
	checksum = syNetPeerChecksumAccumulateU32(checksum, SYNETPEER_VERSION);
	checksum = syNetPeerChecksumAccumulateU32(checksum, SYNETPEER_PACKET_INPUT);
	checksum = syNetPeerChecksumAccumulateU32(checksum, session_id);
	checksum = syNetPeerChecksumAccumulateU32(checksum, packet_seq);
	checksum = syNetPeerChecksumAccumulateU32(checksum, player);
	checksum = syNetPeerChecksumAccumulateU32(checksum, frame_count);

	for (i = 0; i < MAXCONTROLLERS; i++)
	{
		checksum = syNetPeerChecksumAccumulateU32(checksum, (u16)leads[i]);
	}
	for (i = 0; i < frame_count; i++)
	{
		checksum = syNetPeerChecksumAccumulateFrame(checksum, &frames[i]);
	}
	return checksum;
}

u32 syNetPeerChecksumBytes(const u8 *bytes, u32 size)
{
	u32 checksum = 2166136261U;
	u32 i;

	for (i = 0; i < size; i++)
	{
		checksum ^= bytes[i];
		checksum *= 16777619U;
	}
	return checksum;
}

void syNetPeerWriteU8(u8 **cursor, u8 value)
{
	*(*cursor)++ = value;
}

void syNetPeerWriteU16(u8 **cursor, u16 value)
{
	*(*cursor)++ = (value >> 8) & 0xFF;
	*(*cursor)++ = value & 0xFF;
}

void syNetPeerWriteU32(u8 **cursor, u32 value)
{
	*(*cursor)++ = (value >> 24) & 0xFF;
	*(*cursor)++ = (value >> 16) & 0xFF;
	*(*cursor)++ = (value >> 8) & 0xFF;
	*(*cursor)++ = value & 0xFF;
}

u8 syNetPeerReadU8(const u8 **cursor)
{
	return *(*cursor)++;
}

u16 syNetPeerReadU16(const u8 **cursor)
{
	u16 value = (u16)(*(*cursor)++) << 8;

	value |= *(*cursor)++;

	return value;
}

u32 syNetPeerReadU32(const u8 **cursor)
{
	u32 value = (u32)(*(*cursor)++) << 24;

	value |= (u32)(*(*cursor)++) << 16;
	value |= (u32)(*(*cursor)++) << 8;
	value |= *(*cursor)++;

	return value;
}

void syNetPeerWriteMetadata(u8 **cursor, const SYNetInputReplayMetadata *metadata)
{
	s32 player;

	syNetPeerWriteU32(cursor, metadata->magic);
	syNetPeerWriteU32(cursor, metadata->version);
	syNetPeerWriteU32(cursor, metadata->scene_kind);
	syNetPeerWriteU32(cursor, metadata->player_count);
	syNetPeerWriteU32(cursor, metadata->stage_kind);
	syNetPeerWriteU32(cursor, metadata->stocks);
	syNetPeerWriteU32(cursor, metadata->time_limit);
	syNetPeerWriteU32(cursor, metadata->item_switch);
	syNetPeerWriteU32(cursor, metadata->item_toggles);
	syNetPeerWriteU32(cursor, metadata->rng_seed);
	syNetPeerWriteU32(cursor, metadata->game_type);
	syNetPeerWriteU8(cursor, metadata->game_rules);
	syNetPeerWriteU8(cursor, metadata->is_team_battle);
	syNetPeerWriteU8(cursor, metadata->handicap);
	syNetPeerWriteU8(cursor, metadata->is_team_attack);
	syNetPeerWriteU8(cursor, metadata->is_stage_select);
	syNetPeerWriteU8(cursor, metadata->damage_ratio);
	syNetPeerWriteU8(cursor, metadata->item_appearance_rate);
	syNetPeerWriteU8(cursor, metadata->is_not_teamshadows);

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		syNetPeerWriteU8(cursor, metadata->player_kinds[player]);
		syNetPeerWriteU8(cursor, metadata->fighter_kinds[player]);
		syNetPeerWriteU8(cursor, metadata->costumes[player]);
		syNetPeerWriteU8(cursor, metadata->teams[player]);
		syNetPeerWriteU8(cursor, metadata->handicaps[player]);
		syNetPeerWriteU8(cursor, metadata->levels[player]);
		syNetPeerWriteU8(cursor, metadata->shades[player]);
	}
}

void syNetPeerReadMetadata(const u8 **cursor, SYNetInputReplayMetadata *metadata)
{
	s32 player;

	metadata->magic = syNetPeerReadU32(cursor);
	metadata->version = syNetPeerReadU32(cursor);
	metadata->scene_kind = syNetPeerReadU32(cursor);
	metadata->player_count = syNetPeerReadU32(cursor);
	metadata->stage_kind = syNetPeerReadU32(cursor);
	metadata->stocks = syNetPeerReadU32(cursor);
	metadata->time_limit = syNetPeerReadU32(cursor);
	metadata->item_switch = syNetPeerReadU32(cursor);
	metadata->item_toggles = syNetPeerReadU32(cursor);
	metadata->rng_seed = syNetPeerReadU32(cursor);
	metadata->game_type = syNetPeerReadU32(cursor);
	metadata->game_rules = syNetPeerReadU8(cursor);
	metadata->is_team_battle = syNetPeerReadU8(cursor);
	metadata->handicap = syNetPeerReadU8(cursor);
	metadata->is_team_attack = syNetPeerReadU8(cursor);
	metadata->is_stage_select = syNetPeerReadU8(cursor);
	metadata->damage_ratio = syNetPeerReadU8(cursor);
	metadata->item_appearance_rate = syNetPeerReadU8(cursor);
	metadata->is_not_teamshadows = syNetPeerReadU8(cursor);

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		metadata->player_kinds[player] = syNetPeerReadU8(cursor);
		metadata->fighter_kinds[player] = syNetPeerReadU8(cursor);
		metadata->costumes[player] = syNetPeerReadU8(cursor);
		metadata->teams[player] = syNetPeerReadU8(cursor);
		metadata->handicaps[player] = syNetPeerReadU8(cursor);
		metadata->levels[player] = syNetPeerReadU8(cursor);
		metadata->shades[player] = syNetPeerReadU8(cursor);
	}
}

sb32 syNetPeerCheckMetadata(const SYNetInputReplayMetadata *metadata)
{
	if ((metadata->magic != SYNETINPUT_REPLAY_MAGIC) ||
		(metadata->version != SYNETINPUT_REPLAY_VERSION) ||
		(metadata->scene_kind != nSCKindVSBattle) ||
		(metadata->player_count == 0) ||
		(metadata->player_count > MAXCONTROLLERS))
	{
		return FALSE;
	}
	return TRUE;
}

/* ---- remotes ---- */

static void syNetPeerResetRemoteStats(SYNetPeerRemote *remote)
{
	remote->highest_tick = 0;
	remote->recv_seq_high_water = 0;
	remote->recv_seq_initialized = FALSE;
	remote->seq_gaps = 0;
	remote->seq_duplicates = 0;
	remote->seq_out_of_order = 0;
	remote->last_packet_oldest_tick = 0;
	remote->last_packet_newest_tick = 0;
	remote->last_packet_ticks_valid = FALSE;
	remote->advantage = 0;
	remote->battle_ready = FALSE;
	remote->battle_start = FALSE;
	remote->packets_received = 0;
	remote->dropped = FALSE;
#ifdef PORT
	remote->last_heard_ms = port_udp_now_ms();
#endif
}

static void syNetPeerAddRemote(s32 player, s32 transport, sb32 is_host)
{
	SYNetPeerRemote *remote;

	if (sSYNetPeerRemoteCount >= SYNETPEER_MAX_REMOTES)
	{
		return;
	}
	remote = &sSYNetPeerRemotes[sSYNetPeerRemoteCount++];
	remote->player = player;
	remote->transport = transport;
	remote->is_host = is_host;
	remote->left_hint = FALSE;
	remote->barrier_skip = FALSE;
	syNetPeerResetRemoteStats(remote);
}

static SYNetPeerRemote *syNetPeerFindRemoteByTransport(s32 transport)
{
	s32 i;

	for (i = 0; i < sSYNetPeerRemoteCount; i++)
	{
		if (sSYNetPeerRemotes[i].transport == transport)
		{
			return &sSYNetPeerRemotes[i];
		}
	}
	return NULL;
}

/* Newest tick for which every remote's input is confirmed (the minimum over
 * remotes). Everything below it can no longer change. */
u32 syNetPeerGetConfirmedTick(void)
{
	u32 confirmed = 0xFFFFFFFFU;
	s32 i;

	for (i = 0; i < sSYNetPeerRemoteCount; i++)
	{
		u32 next = syNetInputGetRemoteNextUnconfirmed(sSYNetPeerRemotes[i].player);

		if (next < confirmed)
		{
			confirmed = next;
		}
	}
	return confirmed;
}

s32 syNetPeerGetRemoteCount(void)
{
	return sSYNetPeerRemoteCount;
}

/* The remote that has been silent longest, if it has been silent for more than
 * `threshold_ms` during an active online match: its game slot, else -1. Every
 * other peer stalls behind a missing player, so the UI says who it is waiting
 * for. */
s32 syNetPeerGetSilentPlayer(u32 threshold_ms, u32 *out_silent_ms)
{
#ifdef PORT
	s32 worst = -1;
	u32 worst_ms = 0;
	s32 i;

	if ((sSYNetPeerIsActive == FALSE) || (sSYNetPeerIsSocketExternal == FALSE))
	{
		return -1;
	}
	for (i = 0; i < sSYNetPeerRemoteCount; i++)
	{
		u32 silent = port_udp_now_ms() - sSYNetPeerRemotes[i].last_heard_ms;

		if (sSYNetPeerDrops[sSYNetPeerRemotes[i].player].state != SYNETPEER_DROP_STATE_NONE)
		{
			continue;       // already being dropped: the Online window says it left
		}

		if ((silent > threshold_ms) && (silent > worst_ms))
		{
			worst = sSYNetPeerRemotes[i].player;
			worst_ms = silent;
		}
	}
	if ((worst >= 0) && (out_silent_ms != NULL))
	{
		*out_silent_ms = worst_ms;
	}
	return worst;
#else
	return -1;
#endif
}

#ifdef PORT
void syNetPeerSleepBootstrapRetry(void)
{
	port_udp_sleep_us(SYNETPEER_BOOTSTRAP_RETRY_USECS);
}

void syNetPeerCloseSocket(void)
{
	port_udp_close();
}

sb32 syNetPeerOpenSocket(void)
{
	if (port_udp_is_open())
	{
		return TRUE;
	}
	if ((port_udp_open(sSYNetPeerBindSpec) == FALSE) || (port_udp_peer_set(0, sSYNetPeerPeerSpec) == FALSE))
	{
		syNetPeerCloseSocket();
		return FALSE;
	}
	return TRUE;
}
#endif

void syNetPeerMakeBootstrapMetadata(SYNetInputReplayMetadata *metadata)
{
	s32 player;

	memset(metadata, 0, sizeof(*metadata));
	metadata->magic = SYNETINPUT_REPLAY_MAGIC;
	metadata->version = SYNETINPUT_REPLAY_VERSION;
	metadata->scene_kind = nSCKindVSBattle;
	metadata->player_count = 2;
	metadata->stage_kind = nGRKindCastle;
	metadata->stocks = 3;
	metadata->time_limit = SCBATTLE_TIMELIMIT_INFINITE;
	metadata->item_switch = nSCBattleItemSwitchNone;
	metadata->item_toggles = 0;
	metadata->rng_seed = sSYNetPeerBootstrapSeed;
	metadata->game_type = nSCBattleGameTypeRoyal;
	metadata->game_rules = SCBATTLE_GAMERULE_STOCK;
	metadata->is_team_battle = FALSE;
	metadata->handicap = nSCBattleHandicapOff;
	metadata->is_team_attack = FALSE;
	metadata->is_stage_select = FALSE;
	metadata->damage_ratio = 100;
	metadata->item_appearance_rate = nSCBattleItemSwitchNone;
	metadata->is_not_teamshadows = TRUE;

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		metadata->player_kinds[player] = nFTPlayerKindNot;
		metadata->fighter_kinds[player] = nFTKindNull;
		metadata->costumes[player] = 0;
		metadata->teams[player] = player;
		metadata->handicaps[player] = 9;
		metadata->levels[player] = 3;
		metadata->shades[player] = 0;
	}
	metadata->player_kinds[0] = nFTPlayerKindMan;
	metadata->fighter_kinds[0] = nFTKindMario;
	metadata->player_kinds[1] = nFTPlayerKindMan;
	metadata->fighter_kinds[1] = nFTKindFox;
}

void syNetPeerApplyBootstrapMetadata(const SYNetInputReplayMetadata *metadata)
{
	sSYNetPeerBootstrapMetadata = *metadata;
	syNetReplayApplyBattleMetadata(&sSYNetPeerBootstrapMetadata);
	syUtilsSetRandomSeed(sSYNetPeerBootstrapMetadata.rng_seed);
	gSCManagerSceneData.scene_prev = nSCKindVSMode;
	gSCManagerSceneData.scene_curr = nSCKindVSBattle;
	sSYNetPeerBootstrapMetadataApplied = TRUE;

#ifdef PORT
	port_log("SSB64 NetPeer: bootstrap metadata applied host=%d stage=%u seed=%u players=%u p0=%u/%u p1=%u/%u p2=%u/%u p3=%u/%u\n",
	         sSYNetPeerBootstrapIsHost, sSYNetPeerBootstrapMetadata.stage_kind,
	         sSYNetPeerBootstrapMetadata.rng_seed, sSYNetPeerBootstrapMetadata.player_count,
	         sSYNetPeerBootstrapMetadata.player_kinds[0], sSYNetPeerBootstrapMetadata.fighter_kinds[0],
	         sSYNetPeerBootstrapMetadata.player_kinds[1], sSYNetPeerBootstrapMetadata.fighter_kinds[1],
	         sSYNetPeerBootstrapMetadata.player_kinds[2], sSYNetPeerBootstrapMetadata.fighter_kinds[2],
	         sSYNetPeerBootstrapMetadata.player_kinds[3], sSYNetPeerBootstrapMetadata.fighter_kinds[3]);
#endif
}

#ifdef PORT
/* Sends one datagram to every remote. With `is_input` set, remotes that are
 * only reachable through the lobby server's relay get it every other frame:
 * the relay's cost is per message, and the redundant input bundle makes up
 * for the skipped frame. (Control packets always go out: a skipped one could
 * be systematically skipped, since the replies arrive on a fixed parity.) The
 * relayed copies share one relay frame. */
sb32 syNetPeerSendBytes(const u8 *buffer, u32 size, sb32 is_input)
{
	s32 peers[SYNETPEER_MAX_REMOTES];
	s32 count = 0;
	s32 i;

	for (i = 0; i < sSYNetPeerRemoteCount; i++)
	{
		s32 transport = sSYNetPeerRemotes[i].transport;

		if (sSYNetPeerRemotes[i].dropped != FALSE)
		{
			continue;
		}
		if ((is_input != FALSE) && (sSYNetPeerIsSocketExternal != FALSE) && (port_udp_peer_is_relay(transport) != FALSE) &&
		    ((sSYNetPeerSendPhase & 1) != 0))
		{
			continue;
		}
		peers[count++] = transport;
	}
	if (count == 0)
	{
		return FALSE;
	}
	return (port_udp_peers_send(peers, count, buffer, (s32)size) == (s32)size) ? TRUE : FALSE;
}
#endif

void syNetPeerSendControlPacket(u16 packet_type)
{
#ifdef PORT
	u8 buffer[SYNETPEER_CONTROL_PACKET_BYTES];
	u8 *cursor = buffer;
	u32 checksum;

	syNetPeerWriteU32(&cursor, SYNETPEER_MAGIC);
	syNetPeerWriteU16(&cursor, SYNETPEER_VERSION);
	syNetPeerWriteU16(&cursor, packet_type);
	syNetPeerWriteU32(&cursor, sSYNetPeerSessionID);
	checksum = syNetPeerChecksumBytes(buffer, cursor - buffer);
	syNetPeerWriteU32(&cursor, checksum);
	syNetPeerSendBytes(buffer, SYNETPEER_CONTROL_PACKET_BYTES, FALSE);
#endif
}

#ifdef PORT
void syNetPeerSendMatchConfigPacket(void)
{
	u8 buffer[SYNETPEER_BOOTSTRAP_PACKET_BYTES];
	u8 *cursor = buffer;
	u32 checksum;

	syNetPeerWriteU32(&cursor, SYNETPEER_MAGIC);
	syNetPeerWriteU16(&cursor, SYNETPEER_VERSION);
	syNetPeerWriteU16(&cursor, SYNETPEER_PACKET_MATCH_CONFIG);
	syNetPeerWriteU32(&cursor, sSYNetPeerSessionID);
	syNetPeerWriteMetadata(&cursor, &sSYNetPeerBootstrapMetadata);
	checksum = syNetPeerChecksumBytes(buffer, cursor - buffer);
	syNetPeerWriteU32(&cursor, checksum);
	syNetPeerSendBytes(buffer, SYNETPEER_BOOTSTRAP_PACKET_BYTES, FALSE);
}

void syNetPeerHandleControlPacket(SYNetPeerRemote *remote, const u8 *buffer, s32 size)
{
	const u8 *cursor = buffer;
	u32 magic;
	u32 session_id;
	u32 checksum;
	u32 expected_checksum;
	u16 version;
	u16 packet_type;

	if (size != SYNETPEER_CONTROL_PACKET_BYTES)
	{
		return;
	}
	expected_checksum = syNetPeerChecksumBytes(buffer, SYNETPEER_CONTROL_PACKET_BYTES - 4);

	magic = syNetPeerReadU32(&cursor);
	version = syNetPeerReadU16(&cursor);
	packet_type = syNetPeerReadU16(&cursor);
	session_id = syNetPeerReadU32(&cursor);
	checksum = syNetPeerReadU32(&cursor);

	if ((magic != SYNETPEER_MAGIC) || (version != SYNETPEER_VERSION) ||
		(session_id != sSYNetPeerSessionID) || (checksum != expected_checksum))
	{
		sSYNetPeerPacketsDropped++;
		return;
	}
	if (packet_type == SYNETPEER_PACKET_READY)
	{
		sSYNetPeerBootstrapPeerReady = TRUE;
	}
	else if (packet_type == SYNETPEER_PACKET_START)
	{
		sSYNetPeerBootstrapStartReceived = TRUE;
	}
	else if (packet_type == SYNETPEER_PACKET_BATTLE_READY)
	{
		if (remote->battle_ready == FALSE)
		{
			port_log("SSB64 NetPeer: received BATTLE_READY from player %d role=%s tick=%u local=%d\n",
			         remote->player, (sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client",
			         syNetInputGetTick(), sSYNetPeerLocalPlayer);
		}
		remote->battle_ready = TRUE;

		/* It is still waiting at the barrier although we were released (our
		 * START or its answer was lost): say it again. */
		if ((sSYNetPeerBattleBarrierEnabled != FALSE) && (sSYNetPeerBattleBarrierReleased != FALSE))
		{
			sSYNetPeerBattleStartReplyNeeded = TRUE;
		}
	}
	else if (packet_type == SYNETPEER_PACKET_BATTLE_START)
	{
		if (remote->battle_start == FALSE)
		{
			port_log("SSB64 NetPeer: received BATTLE_START from player %d role=%s tick=%u local=%d\n",
			         remote->player, (sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client",
			         syNetInputGetTick(), sSYNetPeerLocalPlayer);
		}
		/* A START implies the sender reached the battle too (its READY
		 * packets stop once it is released). */
		remote->battle_start = TRUE;
		remote->battle_ready = TRUE;
	}
}

void syNetPeerHandleMatchConfigPacket(const u8 *buffer, s32 size)
{
	const u8 *cursor = buffer;
	SYNetInputReplayMetadata metadata;
	u32 magic;
	u32 session_id;
	u32 checksum;
	u32 expected_checksum;
	u16 version;
	u16 packet_type;

	if (size != SYNETPEER_BOOTSTRAP_PACKET_BYTES)
	{
		return;
	}
	expected_checksum = syNetPeerChecksumBytes(buffer, SYNETPEER_BOOTSTRAP_PACKET_BYTES - 4);

	magic = syNetPeerReadU32(&cursor);
	version = syNetPeerReadU16(&cursor);
	packet_type = syNetPeerReadU16(&cursor);
	session_id = syNetPeerReadU32(&cursor);
	syNetPeerReadMetadata(&cursor, &metadata);
	checksum = syNetPeerReadU32(&cursor);

	if ((magic != SYNETPEER_MAGIC) || (version != SYNETPEER_VERSION) ||
		(packet_type != SYNETPEER_PACKET_MATCH_CONFIG) ||
		(session_id != sSYNetPeerSessionID) || (checksum != expected_checksum) ||
		(syNetPeerCheckMetadata(&metadata) == FALSE))
	{
		sSYNetPeerPacketsDropped++;
		return;
	}
	if (sSYNetPeerBootstrapIsHost == FALSE)
	{
		syNetPeerApplyBootstrapMetadata(&metadata);
	}
}

void syNetPeerHandleBootstrapPacket(SYNetPeerRemote *remote, const u8 *buffer, s32 size)
{
	if (size == SYNETPEER_CONTROL_PACKET_BYTES)
	{
		syNetPeerHandleControlPacket(remote, buffer, size);
	}
	else if (size == SYNETPEER_BOOTSTRAP_PACKET_BYTES)
	{
		syNetPeerHandleMatchConfigPacket(buffer, size);
	}
	else sSYNetPeerPacketsDropped++;
}

void syNetPeerReceiveBootstrapPackets(void)
{
	u8 buffer[SYNETPEER_BOOTSTRAP_PACKET_BYTES];

	while (TRUE)
	{
		s32 transport = -1;
		s32 size = port_udp_recv_peer(buffer, sizeof(buffer), &transport);
		SYNetPeerRemote *remote;

		if (size <= 0)
		{
			if (size < 0)
			{
				sSYNetPeerPacketsDropped++;
			}
			break;
		}
		remote = syNetPeerFindRemoteByTransport(transport);

		if (remote != NULL)
		{
			syNetPeerHandleBootstrapPacket(remote, buffer, (s32)size);
		}
	}
}

void syNetPeerRunBootstrap(void)
{
	s32 i;

	if ((sSYNetPeerBootstrapIsEnabled == FALSE) || (sSYNetPeerIsConfigured == FALSE))
	{
		return;
	}
	if (syNetPeerOpenSocket() == FALSE)
	{
		return;
	}
	sSYNetPeerIsActive = TRUE;

	if (sSYNetPeerBootstrapIsHost != FALSE)
	{
		syNetPeerMakeBootstrapMetadata(&sSYNetPeerBootstrapMetadata);
		syNetPeerApplyBootstrapMetadata(&sSYNetPeerBootstrapMetadata);

		for (i = 0; i < SYNETPEER_BOOTSTRAP_RETRY_COUNT; i++)
		{
			syNetPeerSendMatchConfigPacket();
			syNetPeerReceiveBootstrapPackets();

			if (sSYNetPeerBootstrapPeerReady != FALSE)
			{
				break;
			}
			syNetPeerSleepBootstrapRetry();
		}
		if (sSYNetPeerBootstrapPeerReady == FALSE)
		{
			port_log("SSB64 NetPeer: bootstrap host timed out waiting for READY\n");
			return;
		}
		for (i = 0; i < 30; i++)
		{
			syNetPeerSendControlPacket(SYNETPEER_PACKET_START);
			syNetPeerSleepBootstrapRetry();
		}
		port_log("SSB64 NetPeer: bootstrap host sent START stage=%u seed=%u\n",
		         sSYNetPeerBootstrapMetadata.stage_kind, sSYNetPeerBootstrapMetadata.rng_seed);
	}
	else
	{
		for (i = 0; i < SYNETPEER_BOOTSTRAP_RETRY_COUNT; i++)
		{
			syNetPeerReceiveBootstrapPackets();

			if (sSYNetPeerBootstrapMetadataApplied != FALSE)
			{
				syNetPeerSendControlPacket(SYNETPEER_PACKET_READY);
				break;
			}
			syNetPeerSleepBootstrapRetry();
		}
		if (sSYNetPeerBootstrapMetadataApplied == FALSE)
		{
			port_log("SSB64 NetPeer: bootstrap client timed out waiting for MATCH_CONFIG\n");
			return;
		}
		for (i = 0; i < SYNETPEER_BOOTSTRAP_RETRY_COUNT; i++)
		{
			syNetPeerSendControlPacket(SYNETPEER_PACKET_READY);
			syNetPeerReceiveBootstrapPackets();

			if (sSYNetPeerBootstrapStartReceived != FALSE)
			{
				break;
			}
			syNetPeerSleepBootstrapRetry();
		}
		if (sSYNetPeerBootstrapStartReceived == FALSE)
		{
			port_log("SSB64 NetPeer: bootstrap client timed out waiting for START\n");
			return;
		}
		port_log("SSB64 NetPeer: bootstrap client received START stage=%u seed=%u\n",
		         sSYNetPeerBootstrapMetadata.stage_kind, sSYNetPeerBootstrapMetadata.rng_seed);
	}
}
#endif

void syNetPeerInitDebugEnv(void)
{
#ifdef PORT
	char *netplay_env = getenv("SSB64_NETPLAY");
	char *local_player_env;
	char *remote_player_env;
	char *delay_env;
	char *session_env;
	char *bind_env;
	char *peer_env;
	char *bootstrap_env;
	char *bootstrap_host_env;
	char *bootstrap_seed_env;
	s32 remote_player = 1;

	sSYNetPeerIsEnabled = FALSE;
	sSYNetPeerIsConfigured = FALSE;
	sSYNetPeerLocalPlayer = 0;
	sSYNetPeerRemoteCount = 0;
	sSYNetPeerInputDelay = SYNETPEER_DEFAULT_INPUT_DELAY;
	sSYNetPeerSessionID = SYNETPEER_DEFAULT_SESSION_ID;
	sSYNetPeerBootstrapIsEnabled = FALSE;
	sSYNetPeerBootstrapIsHost = FALSE;
	sSYNetPeerBootstrapMetadataApplied = FALSE;
	sSYNetPeerBootstrapPeerReady = FALSE;
	sSYNetPeerBootstrapStartReceived = FALSE;
	sSYNetPeerBootstrapSeed = SYNETPEER_DEFAULT_BOOTSTRAP_SEED;
	sSYNetPeerBattleBarrierEnabled = FALSE;
	sSYNetPeerBattleLocalReady = FALSE;
	sSYNetPeerBattleStartSent = FALSE;
	sSYNetPeerBattleStartReplyNeeded = FALSE;
	sSYNetPeerBattleBarrierReleased = TRUE;
	sSYNetPeerBattleBarrierWaitFrames = 0;
	sSYNetPeerBattleStartRepeatFrames = 0;
	sSYNetPeerExecutionHoldFrames = 0;
	sSYNetPeerIsHoldRandSaved = FALSE;
	sSYNetPeerExecutionBeginLogged = FALSE;

	if ((netplay_env == NULL) || (atoi(netplay_env) == 0))
	{
		return;
	}
	sSYNetPeerIsEnabled = TRUE;

	local_player_env = getenv("SSB64_NETPLAY_LOCAL_PLAYER");
	remote_player_env = getenv("SSB64_NETPLAY_REMOTE_PLAYER");
	delay_env = getenv("SSB64_NETPLAY_DELAY");
	session_env = getenv("SSB64_NETPLAY_SESSION");
	bind_env = getenv("SSB64_NETPLAY_BIND");
	peer_env = getenv("SSB64_NETPLAY_PEER");
	bootstrap_env = getenv("SSB64_NETPLAY_BOOTSTRAP");
	bootstrap_host_env = getenv("SSB64_NETPLAY_HOST");
	bootstrap_seed_env = getenv("SSB64_NETPLAY_SEED");
	{
		char *rollback_env = getenv("SSB64_NETPLAY_ROLLBACK");
		char *fuzz_env = getenv("SSB64_NETPLAY_FUZZ");

		sSYNetPeerIsRollback = ((rollback_env == NULL) || (atoi(rollback_env) != 0)) ? TRUE : FALSE;
		sSYNetPeerFuzzSeed = (fuzz_env != NULL) ? (u32)atoi(fuzz_env) : 0;
	}

	if (local_player_env != NULL)
	{
		sSYNetPeerLocalPlayer = atoi(local_player_env);
	}
	if (remote_player_env != NULL)
	{
		remote_player = atoi(remote_player_env);
	}
	if (delay_env != NULL)
	{
		s32 delay = atoi(delay_env);

		if (delay >= 0)
		{
			sSYNetPeerInputDelay = delay;
		}
	}
	if (session_env != NULL)
	{
		s32 session_id = atoi(session_env);

		if (session_id > 0)
		{
			sSYNetPeerSessionID = session_id;
		}
	}
	if ((bootstrap_env != NULL) && (atoi(bootstrap_env) != 0))
	{
		sSYNetPeerBootstrapIsEnabled = TRUE;
	}
	if ((bootstrap_host_env != NULL) && (atoi(bootstrap_host_env) != 0))
	{
		sSYNetPeerBootstrapIsHost = TRUE;
	}
	if (bootstrap_seed_env != NULL)
	{
		s32 seed = atoi(bootstrap_seed_env);

		if (seed > 0)
		{
			sSYNetPeerBootstrapSeed = seed;
		}
	}
	if ((sSYNetPeerLocalPlayer < 0) || (sSYNetPeerLocalPlayer >= MAXCONTROLLERS) ||
		(remote_player < 0) || (remote_player >= MAXCONTROLLERS) ||
		(sSYNetPeerLocalPlayer == remote_player))
	{
		port_log("SSB64 NetPeer: invalid players local=%d remote=%d\n",
		         sSYNetPeerLocalPlayer, remote_player);
		return;
	}
	if ((bind_env == NULL) || (peer_env == NULL))
	{
		port_log("SSB64 NetPeer: SSB64_NETPLAY_BIND and SSB64_NETPLAY_PEER (host:port) are required\n");
		return;
	}
	sSYNetPeerBindSpec = bind_env;
	sSYNetPeerPeerSpec = peer_env;
	syNetPeerAddRemote(remote_player, 0, (sSYNetPeerBootstrapIsHost == FALSE) ? TRUE : FALSE);
	sSYNetPeerIsConfigured = TRUE;
	port_log("SSB64 NetPeer: configured local=%d remote=%d delay=%u session=%u bootstrap=%d host=%d seed=%u bind=%s peer=%s\n",
	         sSYNetPeerLocalPlayer, remote_player, sSYNetPeerInputDelay,
	         sSYNetPeerSessionID, sSYNetPeerBootstrapIsEnabled, sSYNetPeerBootstrapIsHost,
	         sSYNetPeerBootstrapSeed, bind_env, peer_env);
	syNetPeerRunBootstrap();
#endif
}

void syNetPeerStartVSSession(void)
{
#ifdef PORT
	if ((sSYNetPeerIsEnabled == FALSE) || (sSYNetPeerIsConfigured == FALSE))
	{
		return;
	}
	if (syNetPeerOpenSocket() == FALSE)
	{
		return;
	}
	sSYNetPeerPacketsSent = 0;
	sSYNetPeerPacketsReceived = 0;
	sSYNetPeerPacketsDropped = 0;
	sSYNetPeerFramesStaged = 0;
	sSYNetPeerLateFrames = 0;
	sSYNetPeerInputChecksum = 2166136261U;
	sSYNetPeerLastLogTick = 0;
	sSYNetPeerSendSeq = 0;
	sSYNetPeerSendPhase = 0;
	sSYNetPeerIsActive = TRUE;
	sSYNetPeerBattleBarrierEnabled = sSYNetPeerBootstrapIsEnabled;
	sSYNetPeerBattleLocalReady = sSYNetPeerBattleBarrierEnabled;
	sSYNetPeerBattleStartSent = FALSE;
	sSYNetPeerBattleStartReplyNeeded = FALSE;
	sSYNetPeerBattleBarrierReleased = (sSYNetPeerBattleBarrierEnabled == FALSE) ? TRUE : FALSE;
	sSYNetPeerBattleBarrierWaitFrames = 0;
	sSYNetPeerBattleStartRepeatFrames = 0;
	sSYNetPeerExecutionHoldFrames = 0;
	sSYNetPeerIsHoldRandSaved = FALSE;
	sSYNetPeerExecutionBeginLogged = (sSYNetPeerBattleBarrierEnabled == FALSE) ? TRUE : FALSE;

	{
		s32 slot;
		s32 i;

		/* Slots nobody plays must read identically on every peer: the
		 * Saved source with no saved frames resolves to neutral input.
		 * Left as Local they'd read whatever extra controllers happen to
		 * be plugged into each PC (a pause or reset combo on one peer only). */
		for (slot = 0; slot < MAXCONTROLLERS; slot++)
		{
			syNetInputSetSlotSource(slot, nSYNetInputSourceSaved);
		}
		syNetInputSetSlotSource(sSYNetPeerLocalPlayer, nSYNetInputSourceLocal);
		syNetInputSetLocalDelay(sSYNetPeerInputDelay);

		for (i = 0; i < sSYNetPeerRemoteCount; i++)
		{
			syNetPeerResetRemoteStats(&sSYNetPeerRemotes[i]);
			syNetInputSetSlotSource(sSYNetPeerRemotes[i].player, nSYNetInputSourceRemotePredicted);
			syNetInputPreconfirmRemote(sSYNetPeerRemotes[i].player, sSYNetPeerInputDelay);
		}
	}
	syNetInputSetFuzzSeed(sSYNetPeerFuzzSeed);
	syNetInputSetLocalDevice(sSYNetPeerLocalPlayer, (sSYNetPeerIsSocketExternal != FALSE) ? 0 : sSYNetPeerLocalPlayer);
	sSYNetPeerFrameReady = FALSE;
	syNetPeerResetDrops();
	sSYNetPeerStallFrames = 0;
	sSYNetPeerLockstepStallFrames = 0;
	sSYNetPeerTimeSyncStallFrames = 0;
	sSYNetPeerFramesSinceTimeSync = 0;

	if ((sSYNetPeerBootstrapMetadataApplied != FALSE) && (getenv("SSB64_NETPLAY_NO_RESEED") == NULL))
	{
		/* The seed was set when the match was requested, but each player's
		 * menu kept running (and drawing random numbers) until the battle
		 * scene loaded - a different amount on each PC. Re-seed here, before
		 * the scene's own setup (itManagerInitItems, stage setup) uses it. */
		syUtilsSetRandomSeedPtr(NULL);
		syUtilsSetRandomSeed(sSYNetPeerBootstrapMetadata.rng_seed);

		/* Item-manager globals that itManagerInitItems only fills in when
		 * items are on (or rebuilds lazily, like the Poke Ball list): left
		 * as the previous match on this PC set them, so two players' values
		 * differ. Start every online match from the fresh-boot state. */
		memset(&gITManagerRandomWeights, 0, sizeof(gITManagerRandomWeights));
		memset(&gITManagerMonsterData, 0, sizeof(gITManagerMonsterData));
		memset(&gITManagerAppearActor, 0, sizeof(gITManagerAppearActor));

		/* Hit-refresh counters: ftManagerInitAll sets them to 1, but stage items (the Castle bumper)
		 * are created before it and record the current value in their attack data. Whatever the
		 * previous scenes on this PC left there would differ from the other players. */
		gFTManagerMotionCount = 0;
		gFTManagerStatUpdateCount = 0;
	}

	port_log("SSB64 NetPeer: VS session start role=%s local=%d remotes=%d delay=%u barrier=%d rollback=%d fuzz=%u tick=%u\n",
	         (sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client",
	         sSYNetPeerLocalPlayer, sSYNetPeerRemoteCount, sSYNetPeerInputDelay,
	         sSYNetPeerBattleBarrierEnabled, sSYNetPeerIsRollback, sSYNetPeerFuzzSeed, syNetInputGetTick());
#endif
}

static sb32 syNetPeerCheckBarrierReleased(void)
{
	if ((sSYNetPeerIsEnabled == FALSE) || (sSYNetPeerBootstrapIsEnabled == FALSE) ||
		(sSYNetPeerBattleBarrierEnabled == FALSE))
	{
		return TRUE;
	}
	return sSYNetPeerBattleBarrierReleased;
}

/* May the tick about to be published run now? Rollback-safe ticks may run
 * up to SYNETPEER_MAX_PREDICTION ticks ahead of the confirmed input of the
 * slowest remote; ticks a snapshot can't capture (countdown/announcer
 * threads) run in lockstep, i.e. only with every remote's input for that
 * tick confirmed. */
static sb32 syNetPeerCheckPacing(void)
{
	u32 tick = syNetInputGetTick();
	u32 confirmed = syNetPeerGetConfirmedTick();

	if (sSYNetPeerIsRollback == FALSE)
	{
		return TRUE;
	}
	if (syNetRollbackCheckCanSave() != FALSE)
	{
		s32 worst_lead = -0x7FFFFFFF;
		s32 i;

		if ((confirmed != 0xFFFFFFFFU) && (tick >= confirmed + SYNETPEER_MAX_PREDICTION))
		{
			sSYNetPeerStallFrames++;
			return FALSE;
		}
		/* Time sync against the peer we are furthest ahead of: how much
		 * more we lead than it says it leads us. */
		for (i = 0; i < sSYNetPeerRemoteCount; i++)
		{
			s32 advantage;
			s32 lead;

			if (sSYNetPeerRemotes[i].dropped != FALSE)
			{
				continue;       // gone: it neither runs ahead nor behind
			}
			advantage = (s32)tick - (s32)syNetInputGetRemoteNextUnconfirmed(sSYNetPeerRemotes[i].player);
			lead = advantage - sSYNetPeerRemotes[i].advantage;

			if (lead > worst_lead)
			{
				worst_lead = lead;
			}
		}
		if ((worst_lead != -0x7FFFFFFF) && (worst_lead >= SYNETPEER_TIMESYNC_THRESHOLD) &&
		    (sSYNetPeerFramesSinceTimeSync >= SYNETPEER_TIMESYNC_INTERVAL))
		{
			sSYNetPeerFramesSinceTimeSync = 0;
			sSYNetPeerTimeSyncStallFrames++;
			return FALSE;
		}
		return TRUE;
	}
	if (tick < confirmed)
	{
		return TRUE;
	}
	sSYNetPeerLockstepStallFrames++;
	return FALSE;
}

/* Called by syNetInputFuncRead before it resolves the tick's inputs: drain
 * the socket, then decide - once for the whole frame - whether this tick
 * runs. Input publishing, the tick advance and the battle update all read
 * the same answer, so they can never disagree within a frame. */
void syNetPeerBeginFrame(void)
{
	if (sSYNetPeerIsActive == FALSE)
	{
		sSYNetPeerFrameReady = TRUE;
		return;
	}
	sSYNetPeerSendPhase++;
	syNetPeerReceiveRemoteInput();
	syNetPeerUpdateStartBarrier();
#ifdef PORT
	syNetPeerUpdateDrops();
#endif
	sSYNetPeerFramesSinceTimeSync++;

	sSYNetPeerFrameReady = ((syNetPeerCheckBarrierReleased() != FALSE) && (syNetPeerCheckPacing() != FALSE)) ? TRUE : FALSE;
}

sb32 syNetPeerCheckBattleExecutionReady(void)
{
	if (sSYNetPeerIsActive == FALSE)
	{
		return TRUE;
	}
	return ((sSYNetPeerFrameReady != FALSE) && (sSYNetPeerIsTestHold == FALSE)) ? TRUE : FALSE;
}

sb32 syNetPeerIsRollbackActive(void)
{
	return ((sSYNetPeerIsActive != FALSE) && (sSYNetPeerIsRollback != FALSE)) ? TRUE : FALSE;
}

sb32 syNetPeerCheckStartBarrierReleased(void)
{
	return syNetPeerCheckBattleExecutionReady();
}

/* The input packet: our recent local inputs, how far we run ahead of each
 * remote's confirmed input (their time sync), and the newest confirmed-state
 * hash for the desync check. Identical for every remote, so it can be
 * broadcast (and relayed as one frame). */
void syNetPeerBuildPacket(u8 *buffer, u32 *out_size)
{
	SYNetInputFrame history_frame;
	SYNetPeerPacketFrame frames[SYNETPEER_MAX_PACKET_FRAMES];
	SYNetPeerPacketFrame zero_frame;
	s16 leads[MAXCONTROLLERS];
	u8 *cursor = buffer;
	u32 latest_tick;
	u32 checksum;
	s32 frame_count = 0;
	s32 back;
	s32 i;

	memset(frames, 0, sizeof(frames));
	memset(&zero_frame, 0, sizeof(zero_frame));
	memset(leads, 0, sizeof(leads));

	/* The newest sample was scheduled for (tick being published) + delay; the
	 * packet repeats the last 16 scheduled frames for loss tolerance. */
	latest_tick = syNetInputGetTick() + sSYNetPeerInputDelay;

	for (back = SYNETPEER_MAX_PACKET_FRAMES - 1; back >= 0; back--)
	{
		if ((latest_tick >= (u32)back) &&
			(syNetInputGetLocalScheduledFrame(sSYNetPeerLocalPlayer, latest_tick - back, &history_frame) != FALSE))
		{
			frames[frame_count].tick = history_frame.tick;
			frames[frame_count].buttons = history_frame.buttons;
			frames[frame_count].stick_x = history_frame.stick_x;
			frames[frame_count].stick_y = history_frame.stick_y;
			frame_count++;
		}
	}
	for (i = 0; i < sSYNetPeerRemoteCount; i++)
	{
		s32 advantage;

		if (sSYNetPeerRemotes[i].dropped != FALSE)
		{
			continue;
		}
		advantage = (s32)syNetInputGetTick() - (s32)syNetInputGetRemoteNextUnconfirmed(sSYNetPeerRemotes[i].player);

		advantage = (advantage < -32768) ? -32768 : (advantage > 32767) ? 32767 : advantage;
		leads[sSYNetPeerRemotes[i].player] = (s16)advantage;
	}
	checksum = syNetPeerChecksumPacket
	(
		sSYNetPeerSessionID,
		sSYNetPeerSendSeq,
		(u8)sSYNetPeerLocalPlayer,
		(u8)frame_count,
		leads,
		frames
	);

	syNetPeerWriteU32(&cursor, SYNETPEER_MAGIC);
	syNetPeerWriteU16(&cursor, SYNETPEER_VERSION);
	syNetPeerWriteU16(&cursor, 0);
	syNetPeerWriteU32(&cursor, sSYNetPeerSessionID);
	syNetPeerWriteU32(&cursor, sSYNetPeerSendSeq);
	syNetPeerWriteU8(&cursor, (u8)sSYNetPeerLocalPlayer);
	syNetPeerWriteU8(&cursor, (u8)frame_count);

	for (i = 0; i < MAXCONTROLLERS; i++)
	{
		syNetPeerWriteU16(&cursor, (u16)leads[i]);
	}
	for (i = 0; i < SYNETPEER_MAX_PACKET_FRAMES; i++)
	{
		SYNetPeerPacketFrame *frame = (i < frame_count) ? &frames[i] : &zero_frame;

		syNetPeerWriteU32(&cursor, frame->tick);
		syNetPeerWriteU16(&cursor, frame->buttons);
		syNetPeerWriteU8(&cursor, (u8)frame->stick_x);
		syNetPeerWriteU8(&cursor, (u8)frame->stick_y);
	}
	{
		u32 check_tick;
		u32 check_columns[SYNETSYNC_COLUMN_NUM];
		s32 c;

		if (syNetRollbackGetLatestConfirmedHash(&check_tick, check_columns) == FALSE)
		{
			check_tick = 0xFFFFFFFFU;
			memset(check_columns, 0, sizeof(check_columns));
		}
		syNetPeerWriteU32(&cursor, check_tick);

		for (c = 0; c < SYNETSYNC_COLUMN_NUM; c++)
		{
			syNetPeerWriteU32(&cursor, check_columns[c]);
		}
	}
	syNetPeerWriteU32(&cursor, checksum);
	*out_size = SYNETPEER_PACKET_BYTES;
}

void syNetPeerSendLocalInput(void)
{
#ifdef PORT
	u8 buffer[SYNETPEER_PACKET_BYTES];
	u32 size;

	syNetPeerBuildPacket(buffer, &size);

	if (size == 0)
	{
		return;
	}
	if (syNetPeerSendBytes(buffer, size, TRUE) != FALSE)
	{
		sSYNetPeerPacketsSent++;
		sSYNetPeerSendSeq++;
	}
#endif
}

#ifdef PORT
/* ---- drop-outs ----
 *
 * When a remote vanishes mid-match every survivor would wait for its input
 * forever. Instead the survivors agree on a tick T:
 *
 *  1. A survivor SUSPECTS a player when nothing was heard for
 *     SYNETPEER_DROP_SILENCE_MS or the lobby reported it left, or when another
 *     survivor says so. It freezes REPORT = how many of that player's inputs it
 *     holds (contiguously, from tick 0) and tells everybody.
 *  2. The coordinator, the lowest slot that is still a survivor, waits for every
 *     survivor's REPORT and DECIDEs T = the largest report (or a T somebody
 *     already knows, if an earlier coordinator got that far). Every survivor
 *     repeats DECIDE to those that have not acknowledged it, so a lost packet
 *     or a dying coordinator cannot leave anyone behind.
 *  3. From T on, the player's input is neutral and counts as confirmed (netinput),
 *     and its fighter is knocked out at T (syNetPeerApplyDrops). T is never in
 *     the past beyond the snapshot ring: no survivor simulates more than
 *     SYNETPEER_MAX_PREDICTION ticks past the contiguous input it holds, and T is
 *     at least that. A survivor that holds less than T of the player's input is
 *     sent the rest (REQUEST / FORWARD) by whoever has it.
 *
 * Two drops are two independent agreements; a suspect is not a survivor of
 * anyone else's agreement. */

static SYNetPeerRemote *syNetPeerFindRemoteByPlayer(s32 player)
{
	s32 i;

	for (i = 0; i < sSYNetPeerRemoteCount; i++)
	{
		if (sSYNetPeerRemotes[i].player == player)
		{
			return &sSYNetPeerRemotes[i];
		}
	}
	return NULL;
}

static sb32 syNetPeerIsMatchSlot(s32 slot)
{
	return ((slot == sSYNetPeerLocalPlayer) || (syNetPeerFindRemoteByPlayer(slot) != NULL)) ? TRUE : FALSE;
}

/* Does `slot` still take part in the agreement about `subject`? */
static sb32 syNetPeerIsSurvivor(s32 slot, s32 subject)
{
	return ((slot != subject) && (syNetPeerIsMatchSlot(slot) != FALSE) &&
	        (sSYNetPeerDrops[slot].state == SYNETPEER_DROP_STATE_NONE)) ? TRUE : FALSE;
}

/* Who decides the drop tick of `subject`: the lowest match slot that is not itself
 * being dropped (suspected or decided). Every peer computes it from its own view,
 * so DECIDE is only believed from this slot or a lower one (a peer that still
 * counts someone as alive that we already suspect). */
static s32 syNetPeerGetCoordinator(s32 subject)
{
	s32 slot;

	for (slot = 0; slot < MAXCONTROLLERS; slot++)
	{
		if (syNetPeerIsSurvivor(slot, subject) != FALSE)
		{
			return slot;
		}
	}
	return -1;
}

static void syNetPeerResetDrops(void)
{
	memset(sSYNetPeerDrops, 0, sizeof(sSYNetPeerDrops));
	sSYNetPeerAbortReason = NULL;
	sSYNetPeerDropArmed = FALSE;
	sSYNetPeerDropLastPollMs = 0;
	sSYNetPeerDropResumeMs = 0;
	sSYNetPeerSelfDropped = FALSE;
}

/* kind/subject/A/B plus up to SYNETPEER_DROP_FRAMES inputs. `target` is a game
 * slot, or -1 for every remote that is still in the match except `subject`. */
static void syNetPeerSendDrop(s32 target, u8 kind, s32 subject, u32 a, u32 b, const SYNetPeerPacketFrame *frames, s32 count)
{
	u8 buffer[SYNETPEER_DROP_PACKET_BYTES];
	u8 *cursor = buffer;
	s32 peers[SYNETPEER_MAX_REMOTES];
	s32 peer_count = 0;
	s32 i;

	syNetPeerWriteU32(&cursor, SYNETPEER_MAGIC);
	syNetPeerWriteU16(&cursor, SYNETPEER_VERSION);
	syNetPeerWriteU16(&cursor, SYNETPEER_PACKET_DROP);
	syNetPeerWriteU32(&cursor, sSYNetPeerSessionID);
	syNetPeerWriteU8(&cursor, (u8)sSYNetPeerLocalPlayer);
	syNetPeerWriteU8(&cursor, kind);
	syNetPeerWriteU8(&cursor, (u8)subject);
	syNetPeerWriteU8(&cursor, (u8)count);
	syNetPeerWriteU32(&cursor, a);
	syNetPeerWriteU32(&cursor, b);

	for (i = 0; i < SYNETPEER_DROP_FRAMES; i++)
	{
		syNetPeerWriteU32(&cursor, (i < count) ? frames[i].tick : 0);
		syNetPeerWriteU16(&cursor, (i < count) ? frames[i].buttons : 0);
		syNetPeerWriteU8(&cursor, (i < count) ? (u8)frames[i].stick_x : 0);
		syNetPeerWriteU8(&cursor, (i < count) ? (u8)frames[i].stick_y : 0);
	}
	syNetPeerWriteU32(&cursor, syNetPeerChecksumBytes(buffer, cursor - buffer));

	for (i = 0; i < sSYNetPeerRemoteCount; i++)
	{
		const SYNetPeerRemote *remote = &sSYNetPeerRemotes[i];

		if (target >= 0)
		{
			if (remote->player != target)
			{
				continue;
			}
		}
		else if ((remote->dropped != FALSE) || (remote->player == subject))
		{
			continue;
		}
		peers[peer_count++] = remote->transport;
	}
	if (peer_count != 0)
	{
		port_udp_peers_send(peers, peer_count, buffer, SYNETPEER_DROP_PACKET_BYTES);
	}
}

static void syNetPeerSendReport(s32 subject)
{
	syNetPeerSendDrop(-1, SYNETPEER_DROP_KIND_REPORT, subject, sSYNetPeerDrops[subject].report, SYNETPEER_DROP_NO_TICK, NULL, 0);
}

/* `subject` is probably gone: freeze what we hold of its input and tell the others. */
static void syNetPeerSuspect(s32 subject, const char *why)
{
	SYNetPeerDrop *drop = &sSYNetPeerDrops[subject];

	if ((drop->state != SYNETPEER_DROP_STATE_NONE) || (syNetPeerIsMatchSlot(subject) == FALSE) || (subject == sSYNetPeerLocalPlayer))
	{
		return;
	}
	{
		/* SSB64_NETPLAY_TEST_SUSPECT_DELAY=<slot>:<ms>, test only: this peer notices (and
		 * believes) a drop that late, to exercise survivors with different views. */
		static s32 delay_slot = -2;
		static u32 delay_ms = 0;
		u32 now = port_udp_now_ms();

		if (delay_slot == -2)
		{
			const char *env = getenv("SSB64_NETPLAY_TEST_SUSPECT_DELAY");

			delay_slot = -1;
			if ((env != NULL) && (strchr(env, ':') != NULL))
			{
				delay_slot = atoi(env);
				delay_ms = (u32)atoi(strchr(env, ':') + 1);
			}
		}
		if ((delay_slot == sSYNetPeerLocalPlayer) && (delay_ms != 0))
		{
			if (drop->first_want_ms == 0)
			{
				drop->first_want_ms = now;
			}
			if ((now - drop->first_want_ms) < delay_ms)
			{
				return;
			}
		}
	}
	memset(drop, 0, sizeof(*drop));
	drop->state = SYNETPEER_DROP_STATE_SUSPECT;
	drop->report = syNetInputGetRemoteNextUnconfirmed(subject);
	/* From here on input of the subject at or beyond what we hold is ignored until the drop tick is
	 * agreed, so the contiguous input (and with it how far we may simulate) cannot grow past the report. */
	syNetInputSetFreezeTick(subject, drop->report);
	drop->have_report[sSYNetPeerLocalPlayer] = TRUE;
	drop->reports[sSYNetPeerLocalPlayer] = drop->report;
	drop->known[sSYNetPeerLocalPlayer] = SYNETPEER_DROP_NO_TICK;
	sSYNetPeerDropEventMs = port_udp_now_ms();

	port_log("SSB64 Drop: player %d suspected gone (%s) at tick=%u holding input up to %u\n",
	         subject, why, syNetInputGetTick(), drop->report);
	syNetPeerSendReport(subject);
}

/* The survivors agreed: `subject` is out of the match from `tick`. */
static void syNetPeerDecide(s32 subject, u32 tick, const char *why)
{
	SYNetPeerDrop *drop = &sSYNetPeerDrops[subject];
	SYNetPeerRemote *remote = syNetPeerFindRemoteByPlayer(subject);

	if (drop->state == SYNETPEER_DROP_STATE_DECIDED)
	{
		if (drop->tick != tick)
		{
			port_log("SSB64 Drop: CONFLICT for player %d: have tick %u, got %u (%s)\n", subject, drop->tick, tick, why);
			syNetPeerAbortMatch("The players disagreed about who left the match");
		}
		return;
	}
	if (drop->state == SYNETPEER_DROP_STATE_NONE)
	{
		syNetPeerSuspect(subject, why);
	}
	drop->state = SYNETPEER_DROP_STATE_DECIDED;
	drop->tick = tick;
	memset(drop->acked, 0, sizeof(drop->acked));
	drop->acked[sSYNetPeerLocalPlayer] = TRUE;
	sSYNetPeerDropEventMs = port_udp_now_ms();

	if (remote != NULL)
	{
		remote->dropped = TRUE;
	}
	syNetInputSetFreezeTick(subject, 0xFFFFFFFFU);
	syNetInputSetDropTick(subject, tick);

	port_log("SSB64 Drop: player %d leaves the match at tick=%u (%s) now=%u have_input_to=%u\n",
	         subject, tick, why, syNetInputGetTick(), syNetInputGetRemoteNextUnconfirmed(subject));
	if (syNetPeerGetCoordinator(subject) == sSYNetPeerLocalPlayer)
	{
		syNetPeerSendDrop(-1, SYNETPEER_DROP_KIND_DECIDE, subject, tick, 0, NULL, 0);
	}
}

static void syNetPeerDropHandleReport(SYNetPeerRemote *remote, s32 subject, u32 a, u32 b)
{
	SYNetPeerDrop *drop = &sSYNetPeerDrops[subject];

	if (subject == sSYNetPeerLocalPlayer)
	{
		return;     // they think we are gone; the DECIDE (if it comes) tells us
	}
	if (drop->state == SYNETPEER_DROP_STATE_DECIDED)
	{
		/* It may become the coordinator: tell it the tick we know (only the coordinator decides). */
		syNetPeerSendDrop(remote->player, SYNETPEER_DROP_KIND_REPORT, subject, drop->report, drop->tick, NULL, 0);
		return;
	}
	if (drop->state == SYNETPEER_DROP_STATE_NONE)
	{
		syNetPeerSuspect(subject, "reported by another player");
	}
	if (drop->state == SYNETPEER_DROP_STATE_SUSPECT)
	{
		/* Recorded only; the coordinator is the one who turns reports into a decision. */
		drop->have_report[remote->player] = TRUE;
		drop->reports[remote->player] = a;
		drop->known[remote->player] = b;
	}
}

static void syNetPeerDropHandleDecide(SYNetPeerRemote *remote, s32 subject, u32 tick)
{
	SYNetPeerDrop *drop = &sSYNetPeerDrops[subject];

	u32 now_tick = syNetInputGetTick();
	s32 coordinator = syNetPeerGetCoordinator(subject);

	/* Only the coordinator decides. A peer we count as dead may be the real coordinator (it is lower),
	 * but a higher slot than ours is not. */
	if ((coordinator >= 0) && (remote->player > coordinator))
	{
		port_log("SSB64 Drop: ignored DECIDE(player %d, tick %u) from player %d: not the coordinator (%d)\n",
		         subject, tick, remote->player, coordinator);
		return;
	}
	if (drop->state == SYNETPEER_DROP_STATE_DECIDED)
	{
		if (drop->tick == tick)
		{
			drop->acked[remote->player] = TRUE;
			syNetPeerSendDrop(-1, SYNETPEER_DROP_KIND_ACK, subject, tick, 0, NULL, 0);
		}
		else syNetPeerDecide(subject, tick, "decided by a survivor");
		return;
	}
	/* A tick the snapshot ring can't rewind to, one that is far ahead of us, or one below what we hold: the
	 * players no longer share a picture of the match. End it cleanly instead of desyncing. */
	if ((subject != sSYNetPeerLocalPlayer) &&
	    ((((tick + SYNETPEER_DROP_REWIND_MAX) < now_tick) || (tick > (now_tick + SYNETPEER_DROP_MAX_AHEAD))) ||
	     ((drop->state == SYNETPEER_DROP_STATE_SUSPECT) && (tick < drop->report))))
	{
		port_log("SSB64 Drop: DECIDE(player %d, tick %u) from player %d is not usable at tick %u (held input up to %u)\n",
		         subject, tick, remote->player, now_tick, drop->report);
		syNetPeerAbortMatch("Lost sync with the other players");
		return;
	}
	if (subject == sSYNetPeerLocalPlayer)
	{
		if (sSYNetPeerSelfDropped == FALSE)
		{
			sSYNetPeerSelfDropped = TRUE;
			sSYNetPeerDropEventMs = port_udp_now_ms();
			port_log("SSB64 Drop: the other players dropped us from the match at tick=%u\n", tick);
			syNetPeerAbortMatch("You were disconnected from the match");
		}
		return;
	}
	syNetPeerDecide(subject, tick, "decided by the coordinator");

	if ((drop->state == SYNETPEER_DROP_STATE_DECIDED) && (drop->tick == tick))
	{
		drop->acked[remote->player] = TRUE;
		syNetPeerSendDrop(-1, SYNETPEER_DROP_KIND_ACK, subject, tick, 0, NULL, 0);
	}
}

/* A survivor asks for the dropped player's inputs from tick `from`: send what we hold. */
static void syNetPeerDropHandleRequest(SYNetPeerRemote *remote, s32 subject, u32 from, u32 to)
{
	SYNetPeerPacketFrame frames[SYNETPEER_DROP_FRAMES];
	SYNetInputFrame frame;
	s32 count = 0;
	u32 tick;

	/* Only for a drop we agreed to, and only up to its tick: a request for anything else is bogus. */
	if ((sSYNetPeerDrops[subject].state != SYNETPEER_DROP_STATE_DECIDED) || (to != sSYNetPeerDrops[subject].tick) || (from >= to))
	{
		return;
	}
	for (tick = from; (tick < to) && (tick < (from + SYNETINPUT_HISTORY_LENGTH)) && (count < SYNETPEER_DROP_FRAMES); tick++)
	{
		if (syNetInputGetRemoteConfirmedFrame(subject, tick, &frame) != FALSE)
		{
			frames[count].tick = tick;
			frames[count].buttons = frame.buttons;
			frames[count].stick_x = frame.stick_x;
			frames[count].stick_y = frame.stick_y;
			count++;
		}
	}
	if (count != 0)
	{
		syNetPeerSendDrop(remote->player, SYNETPEER_DROP_KIND_FORWARD, subject, 0, 0, frames, count);
	}
}

void syNetPeerHandleDropPacket(SYNetPeerRemote *remote, const u8 *buffer, s32 size)
{
	const u8 *cursor = buffer;
	SYNetPeerPacketFrame frames[SYNETPEER_DROP_FRAMES];
	u32 magic;
	u32 session_id;
	u32 a;
	u32 b;
	u32 checksum;
	u16 version;
	u16 packet_type;
	u8 sender;
	u8 kind;
	u8 subject;
	u8 count;
	s32 i;

	if ((size != SYNETPEER_DROP_PACKET_BYTES) || (remote->dropped != FALSE) || (sSYNetPeerIsSocketExternal == FALSE))
	{
		return;
	}
	magic = syNetPeerReadU32(&cursor);
	version = syNetPeerReadU16(&cursor);
	packet_type = syNetPeerReadU16(&cursor);
	session_id = syNetPeerReadU32(&cursor);
	sender = syNetPeerReadU8(&cursor);
	kind = syNetPeerReadU8(&cursor);
	subject = syNetPeerReadU8(&cursor);
	count = syNetPeerReadU8(&cursor);
	a = syNetPeerReadU32(&cursor);
	b = syNetPeerReadU32(&cursor);

	for (i = 0; i < SYNETPEER_DROP_FRAMES; i++)
	{
		frames[i].tick = syNetPeerReadU32(&cursor);
		frames[i].buttons = syNetPeerReadU16(&cursor);
		frames[i].stick_x = (s8)syNetPeerReadU8(&cursor);
		frames[i].stick_y = (s8)syNetPeerReadU8(&cursor);
	}
	checksum = syNetPeerReadU32(&cursor);

	if ((magic != SYNETPEER_MAGIC) || (version != SYNETPEER_VERSION) || (packet_type != SYNETPEER_PACKET_DROP) ||
	    (session_id != sSYNetPeerSessionID) || (checksum != syNetPeerChecksumBytes(buffer, size - 4)) ||
	    (sender != (u8)remote->player) || (subject >= MAXCONTROLLERS) || (count > SYNETPEER_DROP_FRAMES))
	{
		sSYNetPeerPacketsDropped++;
		return;
	}
	switch (kind)
	{
	case SYNETPEER_DROP_KIND_REPORT:
		syNetPeerDropHandleReport(remote, subject, a, b);
		break;

	case SYNETPEER_DROP_KIND_DECIDE:
		syNetPeerDropHandleDecide(remote, subject, a);
		break;

	case SYNETPEER_DROP_KIND_ACK:
		if ((sSYNetPeerDrops[subject].state == SYNETPEER_DROP_STATE_DECIDED) && (sSYNetPeerDrops[subject].tick == a))
		{
			sSYNetPeerDrops[subject].acked[remote->player] = TRUE;
		}
		break;

	case SYNETPEER_DROP_KIND_REQUEST:
		if (subject != sSYNetPeerLocalPlayer)
		{
			syNetPeerDropHandleRequest(remote, subject, a, b);
		}
		break;

	case SYNETPEER_DROP_KIND_FORWARD:
		/* Genuine inputs of the dropped player, relayed by a survivor. Ticks at
		 * or after the drop tick are ignored by netinput. */
		if ((subject != sSYNetPeerLocalPlayer) && (sSYNetPeerDrops[subject].state != SYNETPEER_DROP_STATE_NONE))
		{
			for (i = 0; i < count; i++)
			{
				syNetInputSetRemoteInput(subject, frames[i].tick, frames[i].buttons, frames[i].stick_x, frames[i].stick_y);
			}
		}
		break;

	default:
		break;
	}
}

/* Coordinator: once every survivor's REPORT is in, decide the drop tick. */
static void syNetPeerDropCoordinate(s32 subject)
{
	SYNetPeerDrop *drop = &sSYNetPeerDrops[subject];
	u32 tick = 0;
	u32 known = SYNETPEER_DROP_NO_TICK;
	s32 slot;

	if (syNetPeerGetCoordinator(subject) != sSYNetPeerLocalPlayer)
	{
		return;     // not the coordinator: it decides
	}
	for (slot = 0; slot < MAXCONTROLLERS; slot++)
	{
		if (syNetPeerIsSurvivor(slot, subject) != FALSE)
		{
			if (drop->have_report[slot] == FALSE)
			{
				return;     // a survivor we have not heard from yet
			}
		}
		if (drop->have_report[slot] != FALSE)
		{
			/* Every report counts, also from a peer we already doubt: the drop tick must not be lower
			 * than what anyone of them holds, or somebody would have to rewind past the snapshot ring. */
			if (drop->reports[slot] > tick)
			{
				tick = drop->reports[slot];
			}
			if (drop->known[slot] != SYNETPEER_DROP_NO_TICK)
			{
				known = drop->known[slot];
			}
		}
	}
	syNetPeerDecide(subject, (known != SYNETPEER_DROP_NO_TICK) ? known : tick, "coordinator");
}

/* Once per frame, after the socket was drained. */
static void syNetPeerUpdateDrops(void)
{
	u32 now;
	s32 i;
	s32 subject;
	sb32 resend;
	sb32 request;

	if ((sSYNetPeerIsSocketExternal == FALSE) || (sSYNetPeerBattleBarrierReleased == FALSE) ||
	    (sSYNetPeerBattleBarrierEnabled == FALSE))
	{
		return;
	}
	now = port_udp_now_ms();

	if (sSYNetPeerDropArmed == FALSE)
	{
		sSYNetPeerDropArmed = TRUE;
		sSYNetPeerDropResumeMs = now;       // the barrier may have taken a while
	}
	else if ((now - sSYNetPeerDropLastPollMs) > SYNETPEER_DROP_RESUME_GAP_MS)
	{
		sSYNetPeerDropResumeMs = now;       // we were stalled ourselves: the others did not go quiet, we did
	}
	sSYNetPeerDropLastPollMs = now;

	for (i = 0; i < sSYNetPeerRemoteCount; i++)
	{
		SYNetPeerRemote *remote = &sSYNetPeerRemotes[i];
		u32 heard = (remote->last_heard_ms > sSYNetPeerDropResumeMs) ? remote->last_heard_ms : sSYNetPeerDropResumeMs;

		if (sSYNetPeerDrops[remote->player].state != SYNETPEER_DROP_STATE_NONE)
		{
			continue;
		}
		if (remote->left_hint != FALSE)
		{
			syNetPeerSuspect(remote->player, "left the lobby");
		}
		else if ((now - heard) > SYNETPEER_DROP_SILENCE_MS)
		{
			syNetPeerSuspect(remote->player, "silent");
		}
	}
	{
		/* SSB64_NETPLAY_TEST_FORGE=<slot>, test only: that peer sends bogus drop messages once (a DECIDE for tick 0,
		 * a REQUEST for ~4 billion ticks, a REPORT claiming a decided tick) about the highest other slot. The
		 * receivers must shrug them off. */
		static s32 forge_slot = -2;
		static sb32 forged = FALSE;

		if (forge_slot == -2)
		{
			const char *env = getenv("SSB64_NETPLAY_TEST_FORGE");

			forge_slot = (env != NULL) ? atoi(env) : -1;
		}
		if ((forge_slot == sSYNetPeerLocalPlayer) && (forged == FALSE) && (syNetInputGetTick() > 600))
		{
			s32 target = (sSYNetPeerLocalPlayer == 0) ? 2 : ((sSYNetPeerLocalPlayer == 3) ? 2 : 3);

			forged = TRUE;
			for (target = MAXCONTROLLERS - 1; target >= 0; target--)
			{
				if ((target != sSYNetPeerLocalPlayer) && (syNetPeerIsMatchSlot(target) != FALSE))
				{
					break;
				}
			}
			port_log("SSB64 Drop: TEST forging drop messages about player %d\n", target);
			syNetPeerSendDrop(-1, SYNETPEER_DROP_KIND_DECIDE, target, 0, 0, NULL, 0);
			syNetPeerSendDrop(-1, SYNETPEER_DROP_KIND_REQUEST, target, 0, 0xFFFFFFF0U, NULL, 0);
			syNetPeerSendDrop(-1, SYNETPEER_DROP_KIND_FORWARD, target, 0, 0, NULL, 0);
		}
	}
	resend = ((sSYNetPeerSendPhase % SYNETPEER_DROP_RESEND_FRAMES) == 0) ? TRUE : FALSE;
	request = ((sSYNetPeerSendPhase % SYNETPEER_DROP_REQUEST_FRAMES) == 0) ? TRUE : FALSE;

	for (subject = 0; subject < MAXCONTROLLERS; subject++)
	{
		SYNetPeerDrop *drop = &sSYNetPeerDrops[subject];

		if (drop->state == SYNETPEER_DROP_STATE_SUSPECT)
		{
			syNetPeerDropCoordinate(subject);

			if ((drop->state == SYNETPEER_DROP_STATE_SUSPECT) && (resend != FALSE))
			{
				syNetPeerSendReport(subject);
			}
		}
		if (drop->state == SYNETPEER_DROP_STATE_DECIDED)
		{
			u32 next = syNetInputGetRemoteNextUnconfirmed(subject);
			s32 slot;

			if (resend != FALSE)
			{
				sb32 is_coordinator = (syNetPeerGetCoordinator(subject) == sSYNetPeerLocalPlayer) ? TRUE : FALSE;

				for (slot = 0; slot < MAXCONTROLLERS; slot++)
				{
					if ((syNetPeerIsSurvivor(slot, subject) != FALSE) && (slot != sSYNetPeerLocalPlayer) &&
					    (drop->acked[slot] == FALSE))
					{
						/* The coordinator repeats the decision; anyone else only says which tick it knows, so a
						 * survivor that becomes coordinator after the old one died reuses it. */
						if (is_coordinator != FALSE)
						{
							syNetPeerSendDrop(slot, SYNETPEER_DROP_KIND_DECIDE, subject, drop->tick, 0, NULL, 0);
						}
						else syNetPeerSendDrop(slot, SYNETPEER_DROP_KIND_REPORT, subject, drop->report, drop->tick, NULL, 0);
					}
				}
				if ((sSYNetPeerSendPhase % (SYNETPEER_DROP_RESEND_FRAMES * SYNETPEER_DROP_NOTIFY_EVERY)) == 0)
				{
					/* Best effort: lets a player that was only cut off or frozen learn it was dropped. */
					syNetPeerSendDrop(subject, SYNETPEER_DROP_KIND_DECIDE, subject, drop->tick, 0, NULL, 0);
				}
			}
			/* Inputs of the dropped player below the drop tick that we still miss. */
			if ((request != FALSE) && (next != 0xFFFFFFFFU) && (next < drop->tick))
			{
				syNetPeerSendDrop(-1, SYNETPEER_DROP_KIND_REQUEST, subject, next, drop->tick, NULL, 0);
			}
		}
	}
}

/* The players can no longer be kept in sync (disagreement about a drop, a rollback that cannot be
 * served, being dropped ourselves): leave the match through the same exit as the menu's Reset and
 * tell the lobby why, instead of playing on diverged. */
void syNetPeerAbortMatch(const char *why)
{
#ifdef PORT
	extern void portSCManagerRequestReset(void);
	extern void port_netlobby_set_match_note(const char *note);

	if ((sSYNetPeerAbortReason != NULL) || (sSYNetPeerIsActive == FALSE))
	{
		return;
	}
	sSYNetPeerAbortReason = why;
	sSYNetPeerDropEventMs = port_udp_now_ms();
	port_log("SSB64 Drop: ending the match: %s\n", why);
	port_netlobby_set_match_note(why);
	portSCManagerRequestReset();
#endif
}

const char *syNetPeerGetAbortReason(void)
{
#ifdef PORT
	return (sSYNetPeerIsActive != FALSE) ? sSYNetPeerAbortReason : NULL;
#else
	return NULL;
#endif
}

sb32 syNetPeerHasDrops(void)
{
#ifdef PORT
	s32 player;

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		if (sSYNetPeerDrops[player].state != SYNETPEER_DROP_STATE_NONE)
		{
			return TRUE;
		}
	}
#endif
	return FALSE;
}

/* The knock-out: the fighter of a dropped player leaves exactly like a player
 * who lost the last stock, so places, the end of the match and the results
 * screen work as they do offline. Called at the start of every simulated tick
 * (also when it is resimulated); it keys on the tick and on the game state
 * only, so every survivor does it on the same tick. */
static void syNetPeerKnockOutFighter(s32 player, u32 tick)
{
	GObj *fighter_gobj;
	FTStruct *fp;

	if ((gSCManagerBattleState == NULL) || (gSCManagerBattleState->game_status != nSCBattleGameStatusGo))
	{
		return;     // countdown or already over: the next Go tick does it
	}
	fighter_gobj = gSCManagerBattleState->players[player].fighter_gobj;

	if (fighter_gobj == NULL)
	{
		return;
	}
	fp = ftGetStruct(fighter_gobj);

	if (gSCManagerBattleState->game_rules & SCBATTLE_GAMERULE_STOCK)
	{
		if (fp->stock_count == -1)
		{
			return;     // already out
		}
		fp->stock_count = 0;
		gSCManagerBattleState->players[player].stock_count = 0;
		ftCommonDeadDownSetStatus(fighter_gobj);
		port_log("SSB64 Drop: player %d knocked out (stock) on tick %u\n", player, tick);
	}
	else if (fp->status_id != nFTCommonStatusSleep)
	{
		ftCommonDeadResetCommonVars(fighter_gobj);
		ftCommonSleepSetStatus(fighter_gobj);
		port_log("SSB64 Drop: player %d put to sleep (timed match) on tick %u\n", player, tick);
	}
}
#endif /* PORT */

void syNetPeerApplyDrops(u32 tick)
{
#ifdef PORT
	s32 player;

	if (sSYNetPeerIsActive == FALSE)
	{
		return;
	}
	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		if ((sSYNetPeerDrops[player].state == SYNETPEER_DROP_STATE_DECIDED) && (tick >= sSYNetPeerDrops[player].tick))
		{
			syNetPeerKnockOutFighter(player, tick);
		}
	}
#endif
}

/* The lobby says the player on this UDP peer index left the room. */
void syNetPeerNotePlayerLeft(s32 transport)
{
#ifdef PORT
	SYNetPeerRemote *remote = syNetPeerFindRemoteByTransport(transport);

	if (remote != NULL)
	{
		remote->left_hint = TRUE;
	}
#endif
}

/* For the Online window: who is leaving or gone (bit per game slot), whether we
 * were dropped ourselves, and how long ago the newest change happened. */
sb32 syNetPeerGetDropNotice(u32 *out_pending_mask, u32 *out_gone_mask, u32 *out_age_ms)
{
#ifdef PORT
	u32 pending = 0;
	u32 gone = 0;
	s32 player;

	if ((sSYNetPeerIsActive == FALSE) || (sSYNetPeerIsSocketExternal == FALSE))
	{
		return FALSE;
	}
	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		if (sSYNetPeerDrops[player].state == SYNETPEER_DROP_STATE_SUSPECT)
		{
			pending |= 1U << player;
		}
		else if (sSYNetPeerDrops[player].state == SYNETPEER_DROP_STATE_DECIDED)
		{
			gone |= 1U << player;
		}
	}
	*out_pending_mask = pending;
	*out_gone_mask = gone;
	*out_age_ms = port_udp_now_ms() - sSYNetPeerDropEventMs;

	return ((pending != 0) || (gone != 0) || (sSYNetPeerSelfDropped != FALSE)) ? TRUE : FALSE;
#else
	return FALSE;
#endif
}

sb32 syNetPeerIsSelfDropped(void)
{
#ifdef PORT
	return (sSYNetPeerIsActive != FALSE) ? sSYNetPeerSelfDropped : FALSE;
#else
	return FALSE;
#endif
}

void syNetPeerHandlePacket(SYNetPeerRemote *remote, const u8 *buffer, s32 size)
{
	const u8 *cursor = buffer;
	SYNetPeerPacketFrame frames[SYNETPEER_MAX_PACKET_FRAMES];
	u32 magic;
	u32 session_id;
	u32 packet_seq;
	u32 checksum;
	u32 expected_checksum;
	u16 version;
	u8 player;
	u8 frame_count;
	s16 leads[MAXCONTROLLERS];
	u32 check_tick;
	u32 check_columns[SYNETSYNC_COLUMN_NUM];
	s32 check_c;
	u32 current_tick = syNetInputGetTick();
	s32 i;

#ifdef PORT
	if (size == SYNETPEER_CONTROL_PACKET_BYTES)
	{
		syNetPeerHandleControlPacket(remote, buffer, size);
		return;
	}
	if (size == SYNETPEER_DROP_PACKET_BYTES)
	{
		syNetPeerHandleDropPacket(remote, buffer, size);
		return;
	}
#endif
	if (size != SYNETPEER_PACKET_BYTES)
	{
		sSYNetPeerPacketsDropped++;
		return;
	}
	memset(frames, 0, sizeof(frames));

	magic = syNetPeerReadU32(&cursor);
	version = syNetPeerReadU16(&cursor);
	syNetPeerReadU16(&cursor);
	session_id = syNetPeerReadU32(&cursor);
	packet_seq = syNetPeerReadU32(&cursor);
	player = syNetPeerReadU8(&cursor);
	frame_count = syNetPeerReadU8(&cursor);

	if ((magic != SYNETPEER_MAGIC) || (version != SYNETPEER_VERSION) ||
		(session_id != sSYNetPeerSessionID) || (player != (u8)remote->player) ||
		(frame_count > SYNETPEER_MAX_PACKET_FRAMES))
	{
		sSYNetPeerPacketsDropped++;
		return;
	}
	for (i = 0; i < MAXCONTROLLERS; i++)
	{
		leads[i] = (s16)syNetPeerReadU16(&cursor);
	}
	for (i = 0; i < SYNETPEER_MAX_PACKET_FRAMES; i++)
	{
		frames[i].tick = syNetPeerReadU32(&cursor);
		frames[i].buttons = syNetPeerReadU16(&cursor);
		frames[i].stick_x = (s8)syNetPeerReadU8(&cursor);
		frames[i].stick_y = (s8)syNetPeerReadU8(&cursor);
	}
	check_tick = syNetPeerReadU32(&cursor);

	for (check_c = 0; check_c < SYNETSYNC_COLUMN_NUM; check_c++)
	{
		check_columns[check_c] = syNetPeerReadU32(&cursor);
	}
	checksum = syNetPeerReadU32(&cursor);
	expected_checksum = syNetPeerChecksumPacket(session_id, packet_seq, player, frame_count, leads, frames);

	if (checksum != expected_checksum)
	{
		sSYNetPeerPacketsDropped++;
		return;
	}
	sSYNetPeerPacketsReceived++;
	remote->packets_received++;
	remote->advantage = leads[sSYNetPeerLocalPlayer];     // how far it runs ahead of our input

	if (check_tick != 0xFFFFFFFFU)
	{
		syNetRollbackCheckRemoteConfirmedHash(remote->player, check_tick, check_columns);
	}

	if (remote->recv_seq_initialized == FALSE)
	{
		remote->recv_seq_high_water = packet_seq;
		remote->recv_seq_initialized = TRUE;
	}
	else
	{
		if (packet_seq > remote->recv_seq_high_water)
		{
			remote->seq_gaps += packet_seq - remote->recv_seq_high_water - 1U;
			remote->recv_seq_high_water = packet_seq;
		}
		else if (packet_seq == remote->recv_seq_high_water)
		{
			remote->seq_duplicates++;
		}
		else
		{
			remote->seq_out_of_order++;
		}
	}
	if (frame_count > 0)
	{
		u32 oldest_tick_bundle = frames[0].tick;
		u32 newest_tick_bundle = frames[0].tick;

		for (i = 1; i < frame_count; i++)
		{
			if (frames[i].tick < oldest_tick_bundle)
			{
				oldest_tick_bundle = frames[i].tick;
			}
			if (frames[i].tick > newest_tick_bundle)
			{
				newest_tick_bundle = frames[i].tick;
			}
		}
		remote->last_packet_oldest_tick = oldest_tick_bundle;
		remote->last_packet_newest_tick = newest_tick_bundle;
		remote->last_packet_ticks_valid = TRUE;
	}
	else remote->last_packet_ticks_valid = FALSE;

	for (i = 0; i < frame_count; i++)
	{
		sb32 is_new_remote_tick = (frames[i].tick > remote->highest_tick) ? TRUE : FALSE;

		if ((is_new_remote_tick != FALSE) && (frames[i].tick < current_tick))
		{
			sSYNetPeerLateFrames++;
		}
		if (is_new_remote_tick != FALSE)
		{
			remote->highest_tick = frames[i].tick;
		}
		syNetInputSetRemoteInput
		(
			remote->player,
			frames[i].tick,
			frames[i].buttons,
			frames[i].stick_x,
			frames[i].stick_y
		);
		sSYNetPeerFramesStaged++;
		sSYNetPeerInputChecksum = syNetPeerChecksumAccumulateU32(sSYNetPeerInputChecksum, remote->player);
		sSYNetPeerInputChecksum = syNetPeerChecksumAccumulateFrame(sSYNetPeerInputChecksum, &frames[i]);
	}
}

void syNetPeerReceiveRemoteInput(void)
{
#ifdef PORT
	u8 buffer[SYNETPEER_PACKET_BYTES];
	s32 bad_reads = 0;

	while (TRUE)
	{
		s32 transport = -1;
		s32 size = port_udp_recv_peer(buffer, sizeof(buffer), &transport);
		SYNetPeerRemote *remote;

		if (size < 0)
		{
			/* An oversize or errored datagram is consumed by the socket layer: drop it and keep
			 * draining so one stray packet can't starve this frame's real ones. Bounded so a
			 * persistent socket error can't spin forever. */
			sSYNetPeerPacketsDropped++;

			if (++bad_reads >= 64)
			{
				break;
			}
			continue;
		}
		if (size == 0)
		{
			break;
		}
		remote = syNetPeerFindRemoteByTransport(transport);

		if (remote == NULL)
		{
			sSYNetPeerPacketsDropped++;     // a lobby member that isn't in this match
			continue;
		}
		if (remote->dropped != FALSE)
		{
			continue;       // agreed to have left the match: nothing it sends counts any more
		}
		remote->last_heard_ms = port_udp_now_ms();
		syNetPeerHandlePacket(remote, buffer, (s32)size);
	}
#endif
}

#ifdef PORT
/* Sums/maxima over the remotes for the periodic log lines. */
static u32 syNetPeerSumSeqGaps(void)
{
	u32 sum = 0;
	s32 i;

	for (i = 0; i < sSYNetPeerRemoteCount; i++)
	{
		sum += sSYNetPeerRemotes[i].seq_gaps;
	}
	return sum;
}

static u32 syNetPeerSumSeqDuplicates(void)
{
	u32 sum = 0;
	s32 i;

	for (i = 0; i < sSYNetPeerRemoteCount; i++)
	{
		sum += sSYNetPeerRemotes[i].seq_duplicates;
	}
	return sum;
}

static u32 syNetPeerSumSeqOutOfOrder(void)
{
	u32 sum = 0;
	s32 i;

	for (i = 0; i < sSYNetPeerRemoteCount; i++)
	{
		sum += sSYNetPeerRemotes[i].seq_out_of_order;
	}
	return sum;
}

static u32 syNetPeerMaxHighestRemoteTick(void)
{
	u32 highest = 0;
	s32 i;

	for (i = 0; i < sSYNetPeerRemoteCount; i++)
	{
		if (sSYNetPeerRemotes[i].highest_tick > highest)
		{
			highest = sSYNetPeerRemotes[i].highest_tick;
		}
	}
	return highest;
}

static u32 syNetPeerMinHighestRemoteTick(void)
{
	u32 lowest = 0xFFFFFFFFU;
	s32 i;

	for (i = 0; i < sSYNetPeerRemoteCount; i++)
	{
		if (sSYNetPeerRemotes[i].highest_tick < lowest)
		{
			lowest = sSYNetPeerRemotes[i].highest_tick;
		}
	}
	return (lowest == 0xFFFFFFFFU) ? 0 : lowest;
}

/* Per-remote progress, e.g. "1:t=812/a=0 2:t=810/a=1" (newest input tick /
 * how far it says it runs ahead). */
static void syNetPeerFormatRemotes(char *out, u32 capacity)
{
	s32 i;
	u32 used = 0;

	out[0] = '\0';

	for (i = 0; i < sSYNetPeerRemoteCount; i++)
	{
		int n = snprintf(out + used, capacity - used, "%s%d:t=%u/a=%d", (i != 0) ? " " : "",
		                 sSYNetPeerRemotes[i].player, sSYNetPeerRemotes[i].highest_tick, sSYNetPeerRemotes[i].advantage);

		if ((n < 0) || ((u32)n >= (capacity - used)))
		{
			break;
		}
		used += n;
	}
}

void syNetPeerLogNetSyncValidation(u32 tick)
{
	u32 checksums[MAXCONTROLLERS];
	u32 inp_all = 0;
	u32 fighter_hash;
	u32 win_begin = 0;
	u32 win_length = tick;
	const SYNetPeerRemote *first = (sSYNetPeerRemoteCount != 0) ? &sSYNetPeerRemotes[0] : NULL;

	if ((sSYNetPeerIsActive == FALSE) || (syNetPeerCheckBattleExecutionReady() == FALSE))
	{
		return;
	}
	if (tick >= SYNETPEER_VALIDATION_INPUT_WINDOW)
	{
		win_begin = tick - SYNETPEER_VALIDATION_INPUT_WINDOW;
		win_length = SYNETPEER_VALIDATION_INPUT_WINDOW;
	}
	syNetInputGetHistoryInputValueChecksumWindow(win_begin, win_length, checksums, &inp_all);

	fighter_hash = syNetSyncHashBattleFighters();

	port_log(
		"SSB64 NetSync: role=%s lp=%d remotes=%d tick=%u hist_win=[%u,%u) all=0x%08X p0=0x%08X p1=0x%08X p2=0x%08X p3=0x%08X figh=0x%08X snd_next=%u rcv_hw=%u gap=%u dup=%u ooo=%u pko=%u pkn=%u sent=%u recv=%u dropped=%u stg=%u hr=%u late=%u inpchk=0x%08X pkt_valid=%d\n",
		(sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client",
		sSYNetPeerLocalPlayer,
		sSYNetPeerRemoteCount,
		tick,
		win_begin,
		win_begin + win_length,
		inp_all,
		checksums[0],
		checksums[1],
		checksums[2],
		checksums[3],
		fighter_hash,
		sSYNetPeerSendSeq,
		(first != NULL) ? first->recv_seq_high_water : 0,
		syNetPeerSumSeqGaps(),
		syNetPeerSumSeqDuplicates(),
		syNetPeerSumSeqOutOfOrder(),
		((first != NULL) && (first->last_packet_ticks_valid != FALSE)) ? first->last_packet_oldest_tick : (~(u32)0),
		((first != NULL) && (first->last_packet_ticks_valid != FALSE)) ? first->last_packet_newest_tick : (~(u32)0),
		sSYNetPeerPacketsSent,
		sSYNetPeerPacketsReceived,
		sSYNetPeerPacketsDropped,
		sSYNetPeerFramesStaged,
		syNetPeerMaxHighestRemoteTick(),
		sSYNetPeerLateFrames,
		sSYNetPeerInputChecksum,
		(first != NULL) ? first->last_packet_ticks_valid : 0);
}
#endif

void syNetPeerLogStats(void)
{
#ifdef PORT
	u32 tick = syNetInputGetTick();
	char remotes[128];

	if ((tick == 0) || ((tick - sSYNetPeerLastLogTick) < SYNETPEER_LOG_INTERVAL))
	{
		return;
	}
	sSYNetPeerLastLogTick = tick;
	syNetPeerFormatRemotes(remotes, sizeof(remotes));

	port_log("SSB64 NetPeer: role=%s local=%d remotes=[%s] barrier=%d execution_ready=%d tick=%u sent=%u recv=%u dropped=%u staged=%u min_remote=%u late=%u seq_gap=%u seq_dup=%u seq_ooo=%u inpchk=0x%08X\n",
	         (sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client",
	         sSYNetPeerLocalPlayer, remotes,
	         sSYNetPeerBattleBarrierReleased, syNetPeerCheckBattleExecutionReady(), tick,
	         sSYNetPeerPacketsSent, sSYNetPeerPacketsReceived, sSYNetPeerPacketsDropped,
	         sSYNetPeerFramesStaged, syNetPeerMinHighestRemoteTick(), sSYNetPeerLateFrames,
	         syNetPeerSumSeqGaps(), syNetPeerSumSeqDuplicates(), syNetPeerSumSeqOutOfOrder(),
	         sSYNetPeerInputChecksum);

	syNetPeerLogNetSyncValidation(tick);
#endif
}

void syNetPeerLogExecutionHold(void)
{
#ifdef PORT
	if ((sSYNetPeerExecutionHoldFrames == 1) ||
		((sSYNetPeerExecutionHoldFrames % SYNETPEER_BARRIER_LOG_INTERVAL) == 0))
	{
		char remotes[128];

		syNetPeerFormatRemotes(remotes, sizeof(remotes));
		port_log("SSB64 NetPeer: execution hold role=%s local=%d remotes=[%s] tick=%u hold=%u barrier_wait=%u start_sent=%d min_remote=%u late=%u\n",
		         (sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client",
		         sSYNetPeerLocalPlayer, remotes, syNetInputGetTick(),
		         sSYNetPeerExecutionHoldFrames, sSYNetPeerBattleBarrierWaitFrames,
		         sSYNetPeerBattleStartSent, syNetPeerMinHighestRemoteTick(),
		         sSYNetPeerLateFrames);
	}
#endif
}

void syNetPeerLogExecutionBegin(void)
{
#ifdef PORT
	if (sSYNetPeerExecutionBeginLogged == FALSE)
	{
		sSYNetPeerExecutionBeginLogged = TRUE;
		port_log("SSB64 NetPeer: execution begin role=%s local=%d remotes=%d tick=%u hold=%u barrier_wait=%u min_remote=%u late=%u\n",
		         (sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client",
		         sSYNetPeerLocalPlayer, sSYNetPeerRemoteCount, syNetInputGetTick(),
		         sSYNetPeerExecutionHoldFrames, sSYNetPeerBattleBarrierWaitFrames,
		         syNetPeerMinHighestRemoteTick(), sSYNetPeerLateFrames);
	}
#endif
}

#ifdef PORT
static s32 syNetPeerCountBattleReady(void)
{
	s32 count = 0;
	s32 i;

	for (i = 0; i < sSYNetPeerRemoteCount; i++)
	{
		count += ((sSYNetPeerRemotes[i].battle_ready != FALSE) || (sSYNetPeerRemotes[i].left_hint != FALSE) || (sSYNetPeerRemotes[i].barrier_skip != FALSE)) ? 1 : 0;
	}
	return count;
}

static s32 syNetPeerCountBattleStart(void)
{
	s32 count = 0;
	s32 i;

	for (i = 0; i < sSYNetPeerRemoteCount; i++)
	{
		count += ((sSYNetPeerRemotes[i].battle_start != FALSE) || (sSYNetPeerRemotes[i].left_hint != FALSE) || (sSYNetPeerRemotes[i].barrier_skip != FALSE)) ? 1 : 0;
	}
	return count;
}

/* The match host (game slot 0) left the room before the start barrier was released. */
static sb32 syNetPeerIsHostGone(void)
{
	s32 i;

	for (i = 0; i < sSYNetPeerRemoteCount; i++)
	{
		if ((sSYNetPeerRemotes[i].is_host != FALSE) && (sSYNetPeerRemotes[i].left_hint != FALSE))
		{
			return TRUE;
		}
	}
	return FALSE;
}

/* Who runs the barrier: the match host. If it left before the barrier was released (host migration:
 * it will never send START), the lowest player still in the room takes over, so the others do not wait
 * forever. Every survivor computes the same answer from the lobby's leave notices. */
static sb32 syNetPeerIsBarrierMaster(void)
{
	s32 i;

	if (sSYNetPeerBootstrapIsHost != FALSE)
	{
		return TRUE;
	}
	if (syNetPeerIsHostGone() == FALSE)
	{
		return FALSE;
	}
	for (i = 0; i < sSYNetPeerRemoteCount; i++)
	{
		/* Only players that reached the battle can run its barrier: one that is still in the lobby never will. */
		if ((sSYNetPeerRemotes[i].left_hint == FALSE) && (sSYNetPeerRemotes[i].battle_ready != FALSE) &&
		    (sSYNetPeerRemotes[i].player < sSYNetPeerLocalPlayer))
		{
			return FALSE;
		}
	}
	return TRUE;
}

static sb32 syNetPeerCheckHostBattleStart(void)
{
	sb32 host_gone = syNetPeerIsHostGone();
	s32 i;

	for (i = 0; i < sSYNetPeerRemoteCount; i++)
	{
		/* A START from anyone proves the barrier was released by the host (or its stand-in). */
		if (((sSYNetPeerRemotes[i].is_host != FALSE) || (host_gone != FALSE)) && (sSYNetPeerRemotes[i].battle_start != FALSE))
		{
			return TRUE;
		}
	}
	return FALSE;
}
#endif

void syNetPeerLogBarrierWait(void)
{
#ifdef PORT
	if ((sSYNetPeerBattleBarrierWaitFrames % SYNETPEER_BARRIER_LOG_INTERVAL) == 0)
	{
		port_log("SSB64 NetPeer: barrier wait role=%s local=%d remotes=%d tick=%u local_ready=%d peers_ready=%d start_sent=%d peers_started=%d sent=%u recv=%u dropped=%u staged=%u min_remote=%u late=%u\n",
		         (sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client",
		         sSYNetPeerLocalPlayer, sSYNetPeerRemoteCount, syNetInputGetTick(),
		         sSYNetPeerBattleLocalReady, syNetPeerCountBattleReady(),
		         sSYNetPeerBattleStartSent, syNetPeerCountBattleStart(), sSYNetPeerPacketsSent,
		         sSYNetPeerPacketsReceived, sSYNetPeerPacketsDropped,
		         sSYNetPeerFramesStaged, syNetPeerMinHighestRemoteTick(), sSYNetPeerLateFrames);
	}
#endif
}

void syNetPeerReleaseBattleBarrier(const char *reason)
{
	if (sSYNetPeerBattleBarrierReleased == FALSE)
	{
		sSYNetPeerBattleBarrierReleased = TRUE;

#ifdef PORT
		port_log("SSB64 NetPeer: barrier release role=%s reason=%s local=%d remotes=%d tick=%u wait=%u sent=%u recv=%u dropped=%u staged=%u min_remote=%u late=%u\n",
		         (sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client", reason,
		         sSYNetPeerLocalPlayer, sSYNetPeerRemoteCount, syNetInputGetTick(),
		         sSYNetPeerBattleBarrierWaitFrames, sSYNetPeerPacketsSent,
		         sSYNetPeerPacketsReceived, sSYNetPeerPacketsDropped,
		         sSYNetPeerFramesStaged, syNetPeerMinHighestRemoteTick(), sSYNetPeerLateFrames);
#endif
	}
}

/* In-battle start barrier. Every peer announces BATTLE_READY to every other
 * peer until it is released. The host answers once everyone is ready with
 * BATTLE_START; each client releases when it sees the host's START (and
 * acknowledges with its own), and the host releases when every client has
 * acknowledged. */
void syNetPeerUpdateStartBarrier(void)
{
#ifdef PORT
	if ((sSYNetPeerBattleBarrierEnabled == FALSE) || (sSYNetPeerBattleBarrierReleased != FALSE))
	{
		return;
	}
	sSYNetPeerBattleBarrierWaitFrames++;

	if ((sSYNetPeerBattleBarrierWaitFrames % SYNETPEER_BARRIER_READY_INTERVAL) == 1)
	{
		syNetPeerSendControlPacket(SYNETPEER_PACKET_BATTLE_READY);
	}
	if (syNetPeerIsBarrierMaster() != FALSE)
	{
		if ((sSYNetPeerBootstrapIsHost == FALSE) && (sSYNetPeerBattleBarrierWaitFrames > SYNETPEER_STANDIN_GIVEUP_FRAMES))
		{
			/* We stand in for a host that left before the barrier. Only a player confirmed gone (the survivors
			 * agreed it dropped) may be skipped; a merely slow one keeps being waited for. */
			s32 i;

			for (i = 0; i < sSYNetPeerRemoteCount; i++)
			{
				SYNetPeerRemote *remote = &sSYNetPeerRemotes[i];

				if ((remote->battle_ready == FALSE) && (remote->left_hint == FALSE) && (remote->barrier_skip == FALSE) &&
				    (remote->dropped != FALSE))
				{
					remote->barrier_skip = TRUE;
					port_log("SSB64 NetPeer: player %d was dropped before the battle, starting without it\n", remote->player);
				}
			}
			if (sSYNetPeerBattleBarrierWaitFrames > SYNETPEER_STANDIN_ABORT_FRAMES)
			{
				syNetPeerAbortMatch("Gave up waiting for the other players to get ready");
				return;
			}
		}
		if (syNetPeerCountBattleReady() == sSYNetPeerRemoteCount)
		{
			if (((sSYNetPeerBattleBarrierWaitFrames & 1) == 0) || (sSYNetPeerBattleStartSent == FALSE))
			{
				syNetPeerSendControlPacket(SYNETPEER_PACKET_BATTLE_START);
			}
			sSYNetPeerBattleStartSent = TRUE;

			if (syNetPeerCountBattleStart() == sSYNetPeerRemoteCount)
			{
				sSYNetPeerBattleStartRepeatFrames = SYNETPEER_BATTLE_START_REPEAT_FRAMES;
				syNetPeerReleaseBattleBarrier("clients-ack");
			}
		}
	}
	else if (syNetPeerCheckHostBattleStart() != FALSE)
	{
		syNetPeerSendControlPacket(SYNETPEER_PACKET_BATTLE_START);
		sSYNetPeerBattleStartSent = TRUE;
		sSYNetPeerBattleStartRepeatFrames = SYNETPEER_BATTLE_START_REPEAT_FRAMES;
		syNetPeerReleaseBattleBarrier("host-start");
	}
	syNetPeerLogBarrierWait();
#endif
}

void syNetPeerUpdateBattleGate(void)
{
	if (sSYNetPeerIsActive == FALSE)
	{
		return;
	}
	syNetPeerReceiveRemoteInput();
	syNetPeerUpdateStartBarrier();

	/* After the barrier: keep confirming our START for a while, and answer any
	 * peer that is still waiting. This runs even while pacing holds the tick
	 * (a released peer waiting for input must still release the others). */
	if ((sSYNetPeerBattleBarrierEnabled != FALSE) && (sSYNetPeerBattleBarrierReleased != FALSE) &&
	    ((sSYNetPeerBattleStartRepeatFrames != 0) || (sSYNetPeerBattleStartReplyNeeded != FALSE)))
	{
		if ((sSYNetPeerBattleStartReplyNeeded != FALSE) || ((sSYNetPeerBattleStartRepeatFrames & 1) == 0))
		{
			syNetPeerSendControlPacket(SYNETPEER_PACKET_BATTLE_START);
		}
		sSYNetPeerBattleStartReplyNeeded = FALSE;

		if (sSYNetPeerBattleStartRepeatFrames != 0)
		{
			sSYNetPeerBattleStartRepeatFrames--;
		}
	}

#ifdef PORT
	{
		/* SSB64_NETPLAY_EXTRA_HOLD=<frames>: test-only, hold this peer that
		 * many extra frames at the start, like a slower PC would. */
		static s32 extra_hold = -1;

		if (sSYNetPeerExecutionHoldFrames == 0)
		{
			const char *env = getenv("SSB64_NETPLAY_EXTRA_HOLD");

			extra_hold = (env != NULL) ? atoi(env) : 0;
		}
		sSYNetPeerIsTestHold = FALSE;
		if ((extra_hold > 0) && (sSYNetPeerFrameReady != FALSE))
		{
			extra_hold--;
			sSYNetPeerIsTestHold = TRUE;
		}
	}
#endif
	if (syNetPeerCheckBattleExecutionReady() == FALSE)
	{
		/* A held frame runs no tick but still draws, and some draw code
		 * calls the game RNG. Hold lengths differ between the PCs (the
		 * start barrier, pacing stalls), so keep the RNG as it was when the
		 * hold began and restore it before the next tick. */
		if (sSYNetPeerIsHoldRandSaved == FALSE)
		{
			sSYNetPeerHoldRandSeed = syUtilsRandSeed();
			sSYNetPeerIsHoldRandSaved = TRUE;
		}
		sSYNetPeerExecutionHoldFrames++;
		syNetPeerLogExecutionHold();

		/* A stalled peer must keep sending, or peers waiting on each
		 * other's input would never resume. */
		if (syNetPeerCheckBarrierReleased() != FALSE)
		{
			syNetPeerSendLocalInput();
		}
	}
	else
	{
		if ((sSYNetPeerIsHoldRandSaved != FALSE) && (getenv("SSB64_NETPLAY_NO_RAND_RESTORE") == NULL))
		{
			syUtilsSetRandomSeed(sSYNetPeerHoldRandSeed);
			sSYNetPeerIsHoldRandSaved = FALSE;
		}
		syNetPeerLogExecutionBegin();
	}
}

void syNetPeerUpdate(void)
{
	if (sSYNetPeerIsActive == FALSE)
	{
		return;
	}
	syNetPeerUpdateBattleGate();

	if (syNetPeerCheckBattleExecutionReady() == FALSE)
	{
		return;
	}
	syNetPeerSendLocalInput();
	syNetPeerLogStats();
}

/* Online match from the lobby (port/net/netlobby.cpp). The UDP endpoint is
 * already open with every peer (or the relay) set; this configures the
 * session the same way the env-driven bootstrap does, applies the agreed
 * match and asks the scene manager to enter VS battle. Every peer calls it
 * with the same player list and metadata, each with its own local slot; the
 * in-battle start barrier then aligns tick 0.
 *
 * transports[i] is the UDP peer index of game slot i (unused for the local
 * slot). Game slot 0 is the host. */
void syNetPeerStartOnline(s32 local_player, s32 player_count, const s32 *transports, u32 delay, sb32 is_host, const SYNetInputReplayMetadata *metadata)
{
#ifdef PORT
	extern void portSCManagerRequestScene(s32 target);
	s32 player;

	sSYNetPeerIsEnabled = TRUE;
	sSYNetPeerIsConfigured = TRUE;
	sSYNetPeerIsSocketExternal = TRUE;
	sSYNetPeerLocalPlayer = local_player;
	sSYNetPeerRemoteCount = 0;
	syNetPeerResetDrops();

	for (player = 0; player < player_count; player++)
	{
		if (player != local_player)
		{
			syNetPeerAddRemote(player, transports[player], (player == 0) ? TRUE : FALSE);
		}
	}
	sSYNetPeerInputDelay = delay;
	sSYNetPeerSessionID = metadata->rng_seed;
	sSYNetPeerIsRollback = TRUE;
	sSYNetPeerFuzzSeed = (getenv("SSB64_NETPLAY_FUZZ") != NULL) ? (u32)atoi(getenv("SSB64_NETPLAY_FUZZ")) : 0;
	sSYNetPeerBootstrapIsEnabled = TRUE;    // enables the in-battle start barrier
	sSYNetPeerBootstrapIsHost = is_host;
	sSYNetPeerBootstrapSeed = metadata->rng_seed;

	syNetRollbackSetTickTimer(TRUE);
	syNetPeerApplyBootstrapMetadata(metadata);
	{
		/* SSB64_NETPLAY_PRE_RAND=<n>: test-only, burn n RNG calls before the
		 * battle scene loads, like a menu that animates on one player's PC. */
		const char *pre_rand = getenv("SSB64_NETPLAY_PRE_RAND");
		s32 n = (pre_rand != NULL) ? atoi(pre_rand) : 0;

		while (n-- > 0)
		{
			syUtilsRandUShort();
		}
	}
	portSCManagerRequestScene(nSCKindVSBattle);

	port_log("SSB64 NetPeer: online match local=%d players=%d delay=%u host=%d stage=%u seed=%u\n",
	         local_player, player_count, delay, is_host, metadata->stage_kind, metadata->rng_seed);
#endif
}

/* Online match rules: 2-4 humans, free-for-all stock battle. */
void syNetPeerMakeOnlineMetadata(SYNetInputReplayMetadata *metadata, s32 player_count, u32 stage, u32 stocks, sb32 is_items, u32 seed, const u8 *fighter_kinds, const u8 *costumes)
{
	s32 player;

	syNetPeerMakeBootstrapMetadata(metadata);
	metadata->player_count = player_count;
	metadata->stage_kind = stage;
	metadata->stocks = (stocks > 0) ? stocks - 1 : 0;   // the engine stores lives - 1 (3 = "4 Stocks")
#ifdef PORT
	{
		/* SSB64_NETPLAY_TEST_MINUTES=<n>: test-only, a timed match that ends by itself
		 * (exercises the time-up sequence and rematches). Every peer must set it. */
		const char *test_minutes = getenv("SSB64_NETPLAY_TEST_MINUTES");

		if ((test_minutes != NULL) && (atoi(test_minutes) > 0))
		{
			metadata->game_rules = SCBATTLE_GAMERULE_TIME;
			metadata->time_limit = atoi(test_minutes);
		}
	}
#endif
	metadata->rng_seed = seed;

	if (is_items != FALSE)
	{
		metadata->item_toggles = ~0;
		metadata->item_switch = metadata->item_appearance_rate = nSCBattleItemSwitchMiddle;
	}
	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		if (player < player_count)
		{
			metadata->player_kinds[player] = nFTPlayerKindMan;
			metadata->fighter_kinds[player] = fighter_kinds[player];
			metadata->costumes[player] = costumes[player];
		}
		else
		{
			metadata->player_kinds[player] = nFTPlayerKindNot;
			metadata->fighter_kinds[player] = nFTKindNull;
			metadata->costumes[player] = 0;
		}
	}
}

/* Plain-int entry for the C++ lobby (keeps decomp headers out of port C++).
 * Players are listed in game-slot order: the host is slot 0, then the
 * joiners by lobby slot. transports[i] is the UDP peer index of slot i. */
void syNetPeerStartOnlineMatch(s32 is_host, s32 local_player, s32 player_count, const s32 *transports, u32 delay,
                               u32 stage, u32 stocks, s32 is_items, u32 seed, const s32 *fkinds, const s32 *costumes)
{
	SYNetInputReplayMetadata metadata;
	u8 fighter_kinds[MAXCONTROLLERS];
	u8 costume_ids[MAXCONTROLLERS];
	s32 player;

	if ((player_count < 2) || (player_count > MAXCONTROLLERS))
	{
		return;
	}
	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		fighter_kinds[player] = (player < player_count) ? fkinds[player] : nFTKindNull;
		costume_ids[player] = (player < player_count) ? costumes[player] : 0;
	}
	syNetPeerMakeOnlineMetadata(&metadata, player_count, stage, stocks, is_items, seed, fighter_kinds, costume_ids);
	syNetPeerStartOnline(local_player, player_count, transports, delay, is_host, &metadata);
}

void syNetPeerStopVSSession(void)
{
#ifdef PORT
	if (sSYNetPeerIsActive != FALSE)
	{
		port_log("SSB64 NetPeer: VS session stop sent=%u recv=%u dropped=%u staged=%u late=%u stalls=%u lockstep_stalls=%u timesync_stalls=%u checksum=0x%08X\n",
		         sSYNetPeerPacketsSent, sSYNetPeerPacketsReceived, sSYNetPeerPacketsDropped,
		         sSYNetPeerFramesStaged, sSYNetPeerLateFrames, sSYNetPeerStallFrames, sSYNetPeerLockstepStallFrames,
		         sSYNetPeerTimeSyncStallFrames, sSYNetPeerInputChecksum);
	}
	if (sSYNetPeerIsSocketExternal != FALSE)
	{
		/* Online match over: keep the lobby's socket (rematch), and drop back
		 * to offline so the next local VS match isn't netplay. */
		extern void port_netlobby_on_match_end(void);
		extern void port_netlobby_report_result(s32 count, const u8 *fkind, const u8 *place, const s8 *stocks, const s32 *kos, const s32 *falls);
		u8 res_fkind[MAXCONTROLLERS], res_place[MAXCONTROLLERS];
		s8 res_stocks[MAXCONTROLLERS];
		s32 res_kos[MAXCONTROLLERS], res_falls[MAXCONTROLLERS];
		s32 res_i;

		/* RetroHub rivalries: hand each player's result to the lobby, which writes SSB64_RESULT_FILE for the hub. */
		if (gSCManagerBattleState != NULL)
		{
			for (res_i = 0; res_i < MAXCONTROLLERS; res_i++)
			{
				SCPlayerData *pd = &gSCManagerBattleState->players[res_i];

				res_fkind[res_i] = (pd->pkind == nFTPlayerKindMan) ? pd->fkind : nFTKindNull;
				res_place[res_i] = pd->place;
				res_stocks[res_i] = pd->stock_count;
				res_kos[res_i] = pd->score;
				res_falls[res_i] = pd->falls;
			}
			port_netlobby_report_result(MAXCONTROLLERS, res_fkind, res_place, res_stocks, res_kos, res_falls);
		}

		sSYNetPeerIsEnabled = FALSE;
		sSYNetPeerIsConfigured = FALSE;
		sSYNetPeerBootstrapIsEnabled = FALSE;
		sSYNetPeerIsSocketExternal = FALSE;
		sSYNetPeerRemoteCount = 0;
		syNetRollbackRestoreTickTimer();
		port_netlobby_on_match_end();
	}
	else syNetPeerCloseSocket();
#endif
	sSYNetPeerIsActive = FALSE;
}
