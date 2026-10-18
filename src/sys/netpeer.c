#include <sys/netpeer.h>

#include <ft/fighter.h>
#include <gr/ground.h>
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
#include <stdlib.h>
#include <string.h>
#include <net/port_udp.h>
#endif

#define SYNETPEER_MAGIC 0x53534E50 // SSNP
#define SYNETPEER_VERSION 3
#define SYNETPEER_MAX_PACKET_FRAMES 16
#define SYNETPEER_FRAME_BYTES 8
#define SYNETPEER_INPUT_HEADER_BYTES (4 + 2 + 2 + 4 + 4 + 4 + 1 + 1 + 1 + 1)
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
#define SYNETPEER_BATTLE_START_REPEAT_FRAMES 30

/* Rollback pacing: how many ticks a peer may simulate past the newest
 * contiguously confirmed remote input. Resimulation must reach back to the
 * first mispredicted tick through the snapshot ring, so this stays below
 * SYNETROLLBACK_SLOT_NUM. */
#define SYNETPEER_MAX_PREDICTION 6

/* Time sync: each packet carries the sender's frame advantage (ticks it runs
 * ahead of the newest remote input it has confirmed). The peer that is
 * further ahead skips one frame at a time, at most every N frames, so both
 * sides share the rollback load instead of one peer predicting at the cap. */
#define SYNETPEER_TIMESYNC_THRESHOLD 2
#define SYNETPEER_TIMESYNC_INTERVAL 10

void syNetPeerReceiveRemoteInput(void);
void syNetPeerUpdateStartBarrier(void);
void syNetPeerSendLocalInput(void);

#define SYNETPEER_PACKET_INPUT 0
#define SYNETPEER_PACKET_MATCH_CONFIG 1
#define SYNETPEER_PACKET_READY 2
#define SYNETPEER_PACKET_START 3
#define SYNETPEER_PACKET_BATTLE_READY 4
#define SYNETPEER_PACKET_BATTLE_START 5

typedef struct SYNetPeerPacketFrame
{
	u32 tick;
	u16 buttons;
	s8 stick_x;
	s8 stick_y;

} SYNetPeerPacketFrame;

sb32 sSYNetPeerIsEnabled;
sb32 sSYNetPeerIsConfigured;
sb32 sSYNetPeerIsActive;
s32 sSYNetPeerLocalPlayer;
s32 sSYNetPeerRemotePlayer;
u32 sSYNetPeerInputDelay;
u32 sSYNetPeerSessionID;
u32 sSYNetPeerHighestRemoteTick;
u32 sSYNetPeerPacketsSent;
u32 sSYNetPeerPacketsReceived;
u32 sSYNetPeerPacketsDropped;
u32 sSYNetPeerFramesStaged;
u32 sSYNetPeerLateFrames;
u32 sSYNetPeerInputChecksum;
u32 sSYNetPeerLastLogTick;
u32 sSYNetPeerSendSeq;
u32 sSYNetPeerRecvSeqHighWater;
sb32 sSYNetPeerRecvSeqInitialized;
u32 sSYNetPeerSeqGaps;
u32 sSYNetPeerSeqDuplicates;
u32 sSYNetPeerSeqOutOfOrder;
u32 sSYNetPeerLastPeerAckTick;
u32 sSYNetPeerLastPacketOldestTick;
u32 sSYNetPeerLastPacketNewestTick;
sb32 sSYNetPeerLastPacketTicksValid;
sb32 sSYNetPeerBootstrapIsEnabled;
sb32 sSYNetPeerBootstrapIsHost;
sb32 sSYNetPeerBootstrapMetadataApplied;
sb32 sSYNetPeerBootstrapPeerReady;
sb32 sSYNetPeerBootstrapStartReceived;
u32 sSYNetPeerBootstrapSeed;
SYNetInputReplayMetadata sSYNetPeerBootstrapMetadata;
sb32 sSYNetPeerBattleBarrierEnabled;
sb32 sSYNetPeerBattleLocalReady;
sb32 sSYNetPeerBattlePeerReady;
sb32 sSYNetPeerBattleStartSent;
sb32 sSYNetPeerBattleStartReceived;
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
s32 sSYNetPeerRemoteAdvantage;
u32 sSYNetPeerTimeSyncStallFrames;
u32 sSYNetPeerFramesSinceTimeSync;
sb32 sSYNetPeerIsSocketExternal;     // the lobby owns the UDP endpoint; don't close it

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

u32 syNetPeerChecksumPacket(u32 session_id, u32 ack_tick, u32 packet_seq, u8 player, u8 frame_count, const SYNetPeerPacketFrame *frames)
{
	u32 checksum = 2166136261U;
	s32 i;

	checksum = syNetPeerChecksumAccumulateU32(checksum, SYNETPEER_MAGIC);
	checksum = syNetPeerChecksumAccumulateU32(checksum, SYNETPEER_VERSION);
	checksum = syNetPeerChecksumAccumulateU32(checksum, SYNETPEER_PACKET_INPUT);
	checksum = syNetPeerChecksumAccumulateU32(checksum, session_id);
	checksum = syNetPeerChecksumAccumulateU32(checksum, ack_tick);
	checksum = syNetPeerChecksumAccumulateU32(checksum, packet_seq);
	checksum = syNetPeerChecksumAccumulateU32(checksum, player);
	checksum = syNetPeerChecksumAccumulateU32(checksum, frame_count);

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
	if ((port_udp_open(sSYNetPeerBindSpec) == FALSE) || (port_udp_set_peer(sSYNetPeerPeerSpec) == FALSE))
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
	port_log("SSB64 NetPeer: bootstrap metadata applied host=%d stage=%u seed=%u players=%u p0=%u/%u p1=%u/%u\n",
	         sSYNetPeerBootstrapIsHost, sSYNetPeerBootstrapMetadata.stage_kind,
	         sSYNetPeerBootstrapMetadata.rng_seed, sSYNetPeerBootstrapMetadata.player_count,
	         sSYNetPeerBootstrapMetadata.player_kinds[0], sSYNetPeerBootstrapMetadata.fighter_kinds[0],
	         sSYNetPeerBootstrapMetadata.player_kinds[1], sSYNetPeerBootstrapMetadata.fighter_kinds[1]);
#endif
}

#ifdef PORT
void syNetPeerSendBytes(const u8 *buffer, u32 size)
{
	port_udp_send(buffer, (s32)size);
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
	syNetPeerSendBytes(buffer, SYNETPEER_CONTROL_PACKET_BYTES);
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
	syNetPeerSendBytes(buffer, SYNETPEER_BOOTSTRAP_PACKET_BYTES);
}

void syNetPeerHandleControlPacket(const u8 *buffer, s32 size)
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
		if (sSYNetPeerBattlePeerReady == FALSE)
		{
			port_log("SSB64 NetPeer: received BATTLE_READY role=%s tick=%u local=%d remote=%d\n",
			         (sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client",
			         syNetInputGetTick(), sSYNetPeerLocalPlayer, sSYNetPeerRemotePlayer);
		}
		sSYNetPeerBattlePeerReady = TRUE;
	}
	else if (packet_type == SYNETPEER_PACKET_BATTLE_START)
	{
		if (sSYNetPeerBattleStartReceived == FALSE)
		{
			port_log("SSB64 NetPeer: received BATTLE_START role=%s tick=%u local=%d remote=%d\n",
			         (sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client",
			         syNetInputGetTick(), sSYNetPeerLocalPlayer, sSYNetPeerRemotePlayer);
		}
		sSYNetPeerBattleStartReceived = TRUE;
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

void syNetPeerHandleBootstrapPacket(const u8 *buffer, s32 size)
{
	if (size == SYNETPEER_CONTROL_PACKET_BYTES)
	{
		syNetPeerHandleControlPacket(buffer, size);
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
		s32 size = port_udp_recv(buffer, sizeof(buffer));

		if (size <= 0)
		{
			if (size < 0)
			{
				sSYNetPeerPacketsDropped++;
			}
			break;
		}
		syNetPeerHandleBootstrapPacket(buffer, (s32)size);
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

	sSYNetPeerIsEnabled = FALSE;
	sSYNetPeerIsConfigured = FALSE;
	sSYNetPeerLocalPlayer = 0;
	sSYNetPeerRemotePlayer = 1;
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
	sSYNetPeerBattlePeerReady = FALSE;
	sSYNetPeerBattleStartSent = FALSE;
	sSYNetPeerBattleStartReceived = FALSE;
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
		sSYNetPeerRemotePlayer = atoi(remote_player_env);
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
		(sSYNetPeerRemotePlayer < 0) || (sSYNetPeerRemotePlayer >= MAXCONTROLLERS) ||
		(sSYNetPeerLocalPlayer == sSYNetPeerRemotePlayer))
	{
		port_log("SSB64 NetPeer: invalid players local=%d remote=%d\n",
		         sSYNetPeerLocalPlayer, sSYNetPeerRemotePlayer);
		return;
	}
	if ((bind_env == NULL) || (peer_env == NULL))
	{
		port_log("SSB64 NetPeer: SSB64_NETPLAY_BIND and SSB64_NETPLAY_PEER (host:port) are required\n");
		return;
	}
	sSYNetPeerBindSpec = bind_env;
	sSYNetPeerPeerSpec = peer_env;
	sSYNetPeerIsConfigured = TRUE;
	port_log("SSB64 NetPeer: configured local=%d remote=%d delay=%u session=%u bootstrap=%d host=%d seed=%u bind=%s peer=%s\n",
	         sSYNetPeerLocalPlayer, sSYNetPeerRemotePlayer, sSYNetPeerInputDelay,
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
	sSYNetPeerHighestRemoteTick = 0;
	sSYNetPeerPacketsSent = 0;
	sSYNetPeerPacketsReceived = 0;
	sSYNetPeerPacketsDropped = 0;
	sSYNetPeerFramesStaged = 0;
	sSYNetPeerLateFrames = 0;
	sSYNetPeerInputChecksum = 2166136261U;
	sSYNetPeerLastLogTick = 0;
	sSYNetPeerSendSeq = 0;
	sSYNetPeerRecvSeqInitialized = FALSE;
	sSYNetPeerRecvSeqHighWater = 0;
	sSYNetPeerSeqGaps = 0;
	sSYNetPeerSeqDuplicates = 0;
	sSYNetPeerSeqOutOfOrder = 0;
	sSYNetPeerLastPeerAckTick = 0;
	sSYNetPeerLastPacketTicksValid = FALSE;
	sSYNetPeerIsActive = TRUE;
	sSYNetPeerBattleBarrierEnabled = sSYNetPeerBootstrapIsEnabled;
	sSYNetPeerBattleLocalReady = sSYNetPeerBattleBarrierEnabled;
	sSYNetPeerBattlePeerReady = FALSE;
	sSYNetPeerBattleStartSent = FALSE;
	sSYNetPeerBattleStartReceived = FALSE;
	sSYNetPeerBattleBarrierReleased = (sSYNetPeerBattleBarrierEnabled == FALSE) ? TRUE : FALSE;
	sSYNetPeerBattleBarrierWaitFrames = 0;
	sSYNetPeerBattleStartRepeatFrames = 0;
	sSYNetPeerExecutionHoldFrames = 0;
	sSYNetPeerIsHoldRandSaved = FALSE;
	sSYNetPeerExecutionBeginLogged = (sSYNetPeerBattleBarrierEnabled == FALSE) ? TRUE : FALSE;

	{
		s32 slot;

		/* Slots nobody plays must read identically on both peers: the
		 * Saved source with no saved frames resolves to neutral input.
		 * Left as Local they'd read whatever extra controllers happen to
		 * be plugged into each PC (a pause or reset combo on one peer only). */
		for (slot = 0; slot < MAXCONTROLLERS; slot++)
		{
			syNetInputSetSlotSource(slot, nSYNetInputSourceSaved);
		}
	}
	syNetInputSetSlotSource(sSYNetPeerLocalPlayer, nSYNetInputSourceLocal);
	syNetInputSetSlotSource(sSYNetPeerRemotePlayer, nSYNetInputSourceRemotePredicted);
	syNetInputSetLocalDelay(sSYNetPeerInputDelay);
	syNetInputPreconfirmRemote(sSYNetPeerRemotePlayer, sSYNetPeerInputDelay);
	syNetInputSetFuzzSeed(sSYNetPeerFuzzSeed);
	syNetInputSetLocalDevice(sSYNetPeerLocalPlayer, (sSYNetPeerIsSocketExternal != FALSE) ? 0 : sSYNetPeerLocalPlayer);
	sSYNetPeerFrameReady = FALSE;
	sSYNetPeerStallFrames = 0;
	sSYNetPeerLockstepStallFrames = 0;
	sSYNetPeerRemoteAdvantage = 0;
	sSYNetPeerTimeSyncStallFrames = 0;
	sSYNetPeerFramesSinceTimeSync = 0;

	port_log("SSB64 NetPeer: VS session start role=%s local=%d remote=%d delay=%u barrier=%d rollback=%d fuzz=%u tick=%u\n",
	         (sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client",
	         sSYNetPeerLocalPlayer, sSYNetPeerRemotePlayer, sSYNetPeerInputDelay,
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
 * up to SYNETPEER_MAX_PREDICTION ticks ahead of the confirmed remote input;
 * ticks a snapshot can't capture (countdown/announcer threads) run in
 * lockstep, i.e. only with the remote input for that tick confirmed. */
static sb32 syNetPeerCheckPacing(void)
{
	u32 tick = syNetInputGetTick();
	u32 confirmed = syNetInputGetRemoteNextUnconfirmed(sSYNetPeerRemotePlayer);

	if (sSYNetPeerIsRollback == FALSE)
	{
		return TRUE;
	}
	if (syNetRollbackCheckCanSave() != FALSE)
	{
		s32 advantage = (s32)tick - (s32)confirmed;

		if (tick >= confirmed + SYNETPEER_MAX_PREDICTION)
		{
			sSYNetPeerStallFrames++;
			return FALSE;
		}
		if (((advantage - sSYNetPeerRemoteAdvantage) >= SYNETPEER_TIMESYNC_THRESHOLD) &&
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
	syNetPeerReceiveRemoteInput();
	syNetPeerUpdateStartBarrier();
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

s32 syNetPeerGetRemotePlayer(void)
{
	return sSYNetPeerRemotePlayer;
}

sb32 syNetPeerCheckStartBarrierReleased(void)
{
	return syNetPeerCheckBattleExecutionReady();
}

void syNetPeerBuildPacket(u8 *buffer, u32 *out_size)
{
	SYNetInputFrame history_frame;
	SYNetPeerPacketFrame frames[SYNETPEER_MAX_PACKET_FRAMES];
	SYNetPeerPacketFrame zero_frame;
	u8 *cursor = buffer;
	u32 latest_tick;
	u32 checksum;
	s32 frame_count = 0;
	s32 back;
	s32 i;

	memset(frames, 0, sizeof(frames));
	memset(&zero_frame, 0, sizeof(zero_frame));

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
	checksum = syNetPeerChecksumPacket
	(
		sSYNetPeerSessionID,
		sSYNetPeerHighestRemoteTick,
		sSYNetPeerSendSeq,
		(u8)sSYNetPeerLocalPlayer,
		(u8)frame_count,
		frames
	);

	syNetPeerWriteU32(&cursor, SYNETPEER_MAGIC);
	syNetPeerWriteU16(&cursor, SYNETPEER_VERSION);
	{
		s32 advantage = (s32)syNetInputGetTick() - (s32)syNetInputGetRemoteNextUnconfirmed(sSYNetPeerRemotePlayer);

		advantage = (advantage < -32768) ? -32768 : (advantage > 32767) ? 32767 : advantage;
		syNetPeerWriteU16(&cursor, (u16)(s16)advantage);
	}
	syNetPeerWriteU32(&cursor, sSYNetPeerSessionID);
	syNetPeerWriteU32(&cursor, sSYNetPeerHighestRemoteTick);
	syNetPeerWriteU32(&cursor, sSYNetPeerSendSeq);
	syNetPeerWriteU8(&cursor, (u8)sSYNetPeerLocalPlayer);
	syNetPeerWriteU8(&cursor, (u8)frame_count);
	syNetPeerWriteU8(&cursor, (u8)sSYNetPeerLocalPlayer);
	syNetPeerWriteU8(&cursor, (u8)sSYNetPeerRemotePlayer);

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
	if (port_udp_send(buffer, (s32)size) == (s32)size)
	{
		sSYNetPeerPacketsSent++;
		sSYNetPeerSendSeq++;
	}
#endif
}

void syNetPeerHandlePacket(const u8 *buffer, s32 size)
{
	const u8 *cursor = buffer;
	SYNetPeerPacketFrame frames[SYNETPEER_MAX_PACKET_FRAMES];
	u32 magic;
	u32 session_id;
	u32 ack_tick;
	u32 packet_seq;
	u32 checksum;
	u32 expected_checksum;
	u16 version;
	u8 player;
	u8 frame_count;
	u8 packet_local_player;
	u8 packet_remote_player;
	s32 remote_advantage;
	u32 check_tick;
	u32 check_columns[SYNETSYNC_COLUMN_NUM];
	s32 check_c;
	u32 current_tick = syNetInputGetTick();
	s32 i;

#ifdef PORT
	if (size == SYNETPEER_CONTROL_PACKET_BYTES)
	{
		syNetPeerHandleControlPacket(buffer, size);
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
	remote_advantage = (s16)syNetPeerReadU16(&cursor);
	session_id = syNetPeerReadU32(&cursor);
	ack_tick = syNetPeerReadU32(&cursor);
	packet_seq = syNetPeerReadU32(&cursor);
	player = syNetPeerReadU8(&cursor);
	frame_count = syNetPeerReadU8(&cursor);
	packet_local_player = syNetPeerReadU8(&cursor);
	packet_remote_player = syNetPeerReadU8(&cursor);

	if ((magic != SYNETPEER_MAGIC) || (version != SYNETPEER_VERSION) ||
		(session_id != sSYNetPeerSessionID) || (player != (u8)sSYNetPeerRemotePlayer) ||
		(packet_local_player != (u8)sSYNetPeerRemotePlayer) ||
		(packet_remote_player != (u8)sSYNetPeerLocalPlayer) ||
		(frame_count > SYNETPEER_MAX_PACKET_FRAMES))
	{
		sSYNetPeerPacketsDropped++;
		return;
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
	expected_checksum = syNetPeerChecksumPacket(session_id, ack_tick, packet_seq, player, frame_count, frames);

	if (checksum != expected_checksum)
	{
		sSYNetPeerPacketsDropped++;
		return;
	}
	sSYNetPeerPacketsReceived++;
	sSYNetPeerRemoteAdvantage = remote_advantage;

	if (check_tick != 0xFFFFFFFFU)
	{
		syNetRollbackCheckRemoteConfirmedHash(check_tick, check_columns);
	}

	if (sSYNetPeerRecvSeqInitialized == FALSE)
	{
		sSYNetPeerRecvSeqHighWater = packet_seq;
		sSYNetPeerRecvSeqInitialized = TRUE;
	}
	else
	{
		if (packet_seq > sSYNetPeerRecvSeqHighWater)
		{
			sSYNetPeerSeqGaps += packet_seq - sSYNetPeerRecvSeqHighWater - 1U;
			sSYNetPeerRecvSeqHighWater = packet_seq;
		}
		else if (packet_seq == sSYNetPeerRecvSeqHighWater)
		{
			sSYNetPeerSeqDuplicates++;
		}
		else
		{
			sSYNetPeerSeqOutOfOrder++;
		}
	}
	sSYNetPeerLastPeerAckTick = ack_tick;

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
		sSYNetPeerLastPacketOldestTick = oldest_tick_bundle;
		sSYNetPeerLastPacketNewestTick = newest_tick_bundle;
		sSYNetPeerLastPacketTicksValid = TRUE;
	}
	else sSYNetPeerLastPacketTicksValid = FALSE;

	for (i = 0; i < frame_count; i++)
	{
		sb32 is_new_remote_tick = (frames[i].tick > sSYNetPeerHighestRemoteTick) ? TRUE : FALSE;

		if ((is_new_remote_tick != FALSE) && (frames[i].tick < current_tick))
		{
			sSYNetPeerLateFrames++;
		}
		if (is_new_remote_tick != FALSE)
		{
			sSYNetPeerHighestRemoteTick = frames[i].tick;
		}
		syNetInputSetRemoteInput
		(
			sSYNetPeerRemotePlayer,
			frames[i].tick,
			frames[i].buttons,
			frames[i].stick_x,
			frames[i].stick_y
		);
		sSYNetPeerFramesStaged++;
		sSYNetPeerInputChecksum = syNetPeerChecksumAccumulateU32(sSYNetPeerInputChecksum, sSYNetPeerRemotePlayer);
		sSYNetPeerInputChecksum = syNetPeerChecksumAccumulateFrame(sSYNetPeerInputChecksum, &frames[i]);
	}
}

void syNetPeerReceiveRemoteInput(void)
{
#ifdef PORT
	u8 buffer[SYNETPEER_PACKET_BYTES];

	while (TRUE)
	{
		s32 size = port_udp_recv(buffer, sizeof(buffer));

		if (size <= 0)
		{
			if (size < 0)
			{
				sSYNetPeerPacketsDropped++;
			}
			break;
		}
		syNetPeerHandlePacket(buffer, (s32)size);
	}
#endif
}

#ifdef PORT
void syNetPeerLogNetSyncValidation(u32 tick)
{
	u32 checksums[MAXCONTROLLERS];
	u32 inp_all = 0;
	u32 fighter_hash;
	u32 win_begin = 0;
	u32 win_length = tick;

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
		"SSB64 NetSync: role=%s lp=%d rp=%d tick=%u hist_win=[%u,%u) all=0x%08X p0=0x%08X p1=0x%08X p2=0x%08X p3=0x%08X figh=0x%08X snd_next=%u rcv_hw=%u gap=%u dup=%u ooo=%u puck=%u pko=%u pkn=%u sent=%u recv=%u dropped=%u stg=%u hr=%u late=%u inpchk=0x%08X pkt_valid=%d\n",
		(sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client",
		sSYNetPeerLocalPlayer,
		sSYNetPeerRemotePlayer,
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
		sSYNetPeerRecvSeqHighWater,
		sSYNetPeerSeqGaps,
		sSYNetPeerSeqDuplicates,
		sSYNetPeerSeqOutOfOrder,
		sSYNetPeerLastPeerAckTick,
		(sSYNetPeerLastPacketTicksValid != FALSE) ? sSYNetPeerLastPacketOldestTick : (~(u32)0),
		(sSYNetPeerLastPacketTicksValid != FALSE) ? sSYNetPeerLastPacketNewestTick : (~(u32)0),
		sSYNetPeerPacketsSent,
		sSYNetPeerPacketsReceived,
		sSYNetPeerPacketsDropped,
		sSYNetPeerFramesStaged,
		sSYNetPeerHighestRemoteTick,
		sSYNetPeerLateFrames,
		sSYNetPeerInputChecksum,
		sSYNetPeerLastPacketTicksValid);
}
#endif

void syNetPeerLogStats(void)
{
#ifdef PORT
	u32 tick = syNetInputGetTick();

	if ((tick == 0) || ((tick - sSYNetPeerLastLogTick) < SYNETPEER_LOG_INTERVAL))
	{
		return;
	}
	sSYNetPeerLastLogTick = tick;

	port_log("SSB64 NetPeer: role=%s local=%d remote=%d barrier=%d execution_ready=%d tick=%u sent=%u recv=%u dropped=%u staged=%u highest_remote=%u late=%u snd_next=%u rcv_hw=%u seq_gap=%u seq_dup=%u seq_ooo=%u peer_ack=%u inpchk=0x%08X\n",
	         (sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client",
	         sSYNetPeerLocalPlayer, sSYNetPeerRemotePlayer,
	         sSYNetPeerBattleBarrierReleased, syNetPeerCheckBattleExecutionReady(), tick,
	         sSYNetPeerPacketsSent, sSYNetPeerPacketsReceived, sSYNetPeerPacketsDropped,
	         sSYNetPeerFramesStaged, sSYNetPeerHighestRemoteTick, sSYNetPeerLateFrames,
	         sSYNetPeerSendSeq, sSYNetPeerRecvSeqHighWater, sSYNetPeerSeqGaps,
	         sSYNetPeerSeqDuplicates, sSYNetPeerSeqOutOfOrder,
	         sSYNetPeerLastPeerAckTick,
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
		port_log("SSB64 NetPeer: execution hold role=%s local=%d remote=%d tick=%u hold=%u barrier_wait=%u peer_ready=%d start_sent=%d start_recv=%d highest_remote=%u late=%u\n",
		         (sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client",
		         sSYNetPeerLocalPlayer, sSYNetPeerRemotePlayer, syNetInputGetTick(),
		         sSYNetPeerExecutionHoldFrames, sSYNetPeerBattleBarrierWaitFrames,
		         sSYNetPeerBattlePeerReady, sSYNetPeerBattleStartSent,
		         sSYNetPeerBattleStartReceived, sSYNetPeerHighestRemoteTick,
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
		port_log("SSB64 NetPeer: execution begin role=%s local=%d remote=%d tick=%u hold=%u barrier_wait=%u highest_remote=%u late=%u\n",
		         (sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client",
		         sSYNetPeerLocalPlayer, sSYNetPeerRemotePlayer, syNetInputGetTick(),
		         sSYNetPeerExecutionHoldFrames, sSYNetPeerBattleBarrierWaitFrames,
		         sSYNetPeerHighestRemoteTick, sSYNetPeerLateFrames);
	}
#endif
}

void syNetPeerLogBarrierWait(void)
{
#ifdef PORT
	if ((sSYNetPeerBattleBarrierWaitFrames % SYNETPEER_BARRIER_LOG_INTERVAL) == 0)
	{
		port_log("SSB64 NetPeer: barrier wait role=%s local=%d remote=%d tick=%u local_ready=%d peer_ready=%d start_sent=%d start_recv=%d sent=%u recv=%u dropped=%u staged=%u highest_remote=%u late=%u\n",
		         (sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client",
		         sSYNetPeerLocalPlayer, sSYNetPeerRemotePlayer, syNetInputGetTick(),
		         sSYNetPeerBattleLocalReady, sSYNetPeerBattlePeerReady,
		         sSYNetPeerBattleStartSent, sSYNetPeerBattleStartReceived, sSYNetPeerPacketsSent,
		         sSYNetPeerPacketsReceived, sSYNetPeerPacketsDropped,
		         sSYNetPeerFramesStaged, sSYNetPeerHighestRemoteTick, sSYNetPeerLateFrames);
	}
#endif
}

void syNetPeerReleaseBattleBarrier(const char *reason)
{
	if (sSYNetPeerBattleBarrierReleased == FALSE)
	{
		sSYNetPeerBattleBarrierReleased = TRUE;

#ifdef PORT
		port_log("SSB64 NetPeer: barrier release role=%s reason=%s local=%d remote=%d tick=%u wait=%u sent=%u recv=%u dropped=%u staged=%u highest_remote=%u late=%u\n",
		         (sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client", reason,
		         sSYNetPeerLocalPlayer, sSYNetPeerRemotePlayer, syNetInputGetTick(),
		         sSYNetPeerBattleBarrierWaitFrames, sSYNetPeerPacketsSent,
		         sSYNetPeerPacketsReceived, sSYNetPeerPacketsDropped,
		         sSYNetPeerFramesStaged, sSYNetPeerHighestRemoteTick, sSYNetPeerLateFrames);
#endif
	}
}

void syNetPeerUpdateStartBarrier(void)
{
#ifdef PORT
	if ((sSYNetPeerBattleBarrierEnabled == FALSE) || (sSYNetPeerBattleBarrierReleased != FALSE))
	{
		return;
	}
	sSYNetPeerBattleBarrierWaitFrames++;
	syNetPeerSendControlPacket(SYNETPEER_PACKET_BATTLE_READY);

	if (sSYNetPeerBattlePeerReady != FALSE)
	{
		if (sSYNetPeerBootstrapIsHost != FALSE)
		{
			syNetPeerSendControlPacket(SYNETPEER_PACKET_BATTLE_START);
			sSYNetPeerBattleStartSent = TRUE;

			if (sSYNetPeerBattleStartReceived != FALSE)
			{
				sSYNetPeerBattleStartRepeatFrames = SYNETPEER_BATTLE_START_REPEAT_FRAMES;
				syNetPeerReleaseBattleBarrier("client-ack");
			}
		}
		else if (sSYNetPeerBattleStartReceived != FALSE)
		{
			syNetPeerSendControlPacket(SYNETPEER_PACKET_BATTLE_START);
			sSYNetPeerBattleStartSent = TRUE;
			sSYNetPeerBattleStartRepeatFrames = SYNETPEER_BATTLE_START_REPEAT_FRAMES;
			syNetPeerReleaseBattleBarrier("host-start");
		}
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
		 * calls the game RNG. Hold lengths differ between the two PCs (the
		 * start barrier, pacing stalls), so keep the RNG as it was when the
		 * hold began and restore it before the next tick. */
		if (sSYNetPeerIsHoldRandSaved == FALSE)
		{
			sSYNetPeerHoldRandSeed = syUtilsRandSeed();
			sSYNetPeerIsHoldRandSaved = TRUE;
		}
		sSYNetPeerExecutionHoldFrames++;
		syNetPeerLogExecutionHold();

		/* A stalled peer must keep sending, or two peers waiting on each
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
	if (sSYNetPeerBattleStartRepeatFrames != 0)
	{
		syNetPeerSendControlPacket(SYNETPEER_PACKET_BATTLE_START);
		sSYNetPeerBattleStartRepeatFrames--;
	}
	syNetPeerSendLocalInput();
	syNetPeerLogStats();
}

/* Online match from the lobby (port/net/netlobby.cpp). The UDP endpoint is
 * already open with the peer (or relay) set; this configures the session the
 * same way the env-driven bootstrap does, applies the agreed match and asks
 * the scene manager to enter VS battle. Both peers call it with mirrored
 * player slots and identical metadata; the in-battle start barrier then
 * aligns tick 0. */
void syNetPeerStartOnline(s32 local_player, s32 remote_player, u32 delay, sb32 is_host, const SYNetInputReplayMetadata *metadata)
{
#ifdef PORT
	extern void portSCManagerRequestScene(s32 target);

	sSYNetPeerIsEnabled = TRUE;
	sSYNetPeerIsConfigured = TRUE;
	sSYNetPeerIsSocketExternal = TRUE;
	sSYNetPeerLocalPlayer = local_player;
	sSYNetPeerRemotePlayer = remote_player;
	sSYNetPeerInputDelay = delay;
	sSYNetPeerSessionID = metadata->rng_seed;
	sSYNetPeerIsRollback = TRUE;
	sSYNetPeerFuzzSeed = (getenv("SSB64_NETPLAY_FUZZ") != NULL) ? (u32)atoi(getenv("SSB64_NETPLAY_FUZZ")) : 0;
	sSYNetPeerBootstrapIsEnabled = TRUE;    // enables the in-battle start barrier
	sSYNetPeerBootstrapIsHost = is_host;
	sSYNetPeerBootstrapSeed = metadata->rng_seed;

	syNetRollbackSetTickTimer(TRUE);
	syNetPeerApplyBootstrapMetadata(metadata);
	portSCManagerRequestScene(nSCKindVSBattle);

	port_log("SSB64 NetPeer: online match local=%d remote=%d delay=%u host=%d stage=%u seed=%u\n",
	         local_player, remote_player, delay, is_host, metadata->stage_kind, metadata->rng_seed);
#endif
}

/* Online match rules: 2 humans, stock battle. */
void syNetPeerMakeOnlineMetadata(SYNetInputReplayMetadata *metadata, u32 stage, u32 stocks, sb32 is_items, u32 seed, const u8 *fighter_kinds, const u8 *costumes)
{
	s32 player;

	syNetPeerMakeBootstrapMetadata(metadata);
	metadata->stage_kind = stage;
	metadata->stocks = (stocks > 0) ? stocks - 1 : 0;   // the engine stores lives - 1 (3 = "4 Stocks")
	metadata->rng_seed = seed;

	if (is_items != FALSE)
	{
		metadata->item_toggles = ~0;
		metadata->item_switch = metadata->item_appearance_rate = nSCBattleItemSwitchMiddle;
	}
	for (player = 0; player < 2; player++)
	{
		metadata->player_kinds[player] = nFTPlayerKindMan;
		metadata->fighter_kinds[player] = fighter_kinds[player];
		metadata->costumes[player] = costumes[player];
	}
}

/* Plain-int entry for the C++ lobby (keeps decomp headers out of port C++).
 * The host plays slot 0, the client slot 1. */
void syNetPeerStartOnlineMatch(s32 is_host, u32 delay, u32 stage, u32 stocks, s32 is_items, u32 seed,
                               s32 host_fkind, s32 host_costume, s32 client_fkind, s32 client_costume)
{
	SYNetInputReplayMetadata metadata;
	u8 fighter_kinds[2];
	u8 costumes[2];

	fighter_kinds[0] = host_fkind;
	fighter_kinds[1] = client_fkind;
	costumes[0] = host_costume;
	costumes[1] = client_costume;

	syNetPeerMakeOnlineMetadata(&metadata, stage, stocks, is_items, seed, fighter_kinds, costumes);
	syNetPeerStartOnline((is_host != FALSE) ? 0 : 1, (is_host != FALSE) ? 1 : 0, delay, is_host, &metadata);
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

		sSYNetPeerIsEnabled = FALSE;
		sSYNetPeerIsConfigured = FALSE;
		sSYNetPeerBootstrapIsEnabled = FALSE;
		sSYNetPeerIsSocketExternal = FALSE;
		syNetRollbackRestoreTickTimer();
		port_netlobby_on_match_end();
	}
	else syNetPeerCloseSocket();
#endif
	sSYNetPeerIsActive = FALSE;
}
