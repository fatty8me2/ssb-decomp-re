#include <sys/netinput.h>

#include <sys/netpeer.h>
#include <sys/taskman.h>

typedef struct SYNetInputSlot
{
	SYNetInputSource source;
	SYNetInputFrame last_confirmed;
	SYNetInputFrame last_published;

} SYNetInputSlot;

SYNetInputSlot sSYNetInputSlots[MAXCONTROLLERS];
SYNetInputFrame sSYNetInputHistory[MAXCONTROLLERS][SYNETINPUT_HISTORY_LENGTH];
SYNetInputFrame sSYNetInputRemoteHistory[MAXCONTROLLERS][SYNETINPUT_HISTORY_LENGTH];
SYNetInputFrame sSYNetInputSavedHistory[MAXCONTROLLERS][SYNETINPUT_HISTORY_LENGTH];
SYNetInputFrame sSYNetInputReplayFrames[MAXCONTROLLERS][SYNETINPUT_REPLAY_MAX_FRAMES];
SYNetInputReplayMetadata sSYNetInputReplayMetadata;
u32 sSYNetInputTick;
u32 sSYNetInputRecordedFrameCount;
sb32 sSYNetInputIsRecording;
sb32 sSYNetInputIsReplayMetadataValid;
/* Source-independent checksum over every frame published for a tick that
 * was actually advanced by syNetInputFuncRead(). Accumulated at the same
 * point sSYNetInputTick is incremented, so a tick re-published while the
 * P2P start barrier holds is counted once, like the recorder. When a limit
 * is set the value at exactly that many ticks is frozen for the caller.
 * sSYNetInputPublishedTickCount counts advances and today moves in lockstep
 * with sSYNetInputTick; anything that rewinds the tick (a future rollback
 * via syNetInputSetTick) must save and restore these four values with it. */
u32 sSYNetInputPublishedChecksum;
u32 sSYNetInputPublishedTickCount;
u32 sSYNetInputPublishedChecksumLimit;
u32 sSYNetInputPublishedChecksumAtLimit;

/* Rollback netplay. Local input sampled on tick t is scheduled for tick
 * t + delay (exactly once, so a frame already sent to the peer never
 * changes); the peer receives the scheduled ticks and both sides apply the
 * input on the same tick. */
u32 sSYNetInputLocalDelay;
SYNetInputFrame sSYNetInputLocalSchedule[MAXCONTROLLERS][SYNETINPUT_HISTORY_LENGTH];
/* Every remote tick below this has a confirmed input (per player). */
u32 sSYNetInputRemoteNextUnconfirmed[MAXCONTROLLERS];
/* A player dropped from the match (netpeer.c agrees the tick with the survivors):
 * from this tick on its input is neutral and always confirmed. SYNETINPUT_NO_DROP = none. */
#define SYNETINPUT_NO_DROP 0xFFFFFFFFU
u32 sSYNetInputDropTick[MAXCONTROLLERS];
/* A player is suspected gone but the drop tick is not agreed yet: its input at or beyond this tick is not
 * taken, so what we hold of it (and how far we may simulate past it) stops growing. */
u32 sSYNetInputFreezeTick[MAXCONTROLLERS];
/* Earliest already-published tick whose confirmed remote input differs from
 * the prediction that was simulated; consumed by the rollback driver. */
u32 sSYNetInputMispredictTick;
sb32 sSYNetInputIsMispredicted;
/* SSB64_NETPLAY_FUZZ=<seed>: local slots play seeded pseudo-random input
 * (test harness: real mispredictions without anyone at the controls). */
u32 sSYNetInputFuzzSeed;
/* Physical controller each local slot reads (online: the client plays fighter
 * slot 1 from controller port 1). Hardware state is copied before the publish
 * loop, which overwrites gSYControllerDevices slot by slot. */
s32 sSYNetInputLocalDevice[MAXCONTROLLERS];
SYController sSYNetInputHardware[MAXCONTROLLERS];

u32 syNetInputGetTick(void)
{
	return sSYNetInputTick;
}

void syNetInputSetTick(u32 tick)
{
	sSYNetInputTick = tick;
}

sb32 syNetInputCheckPlayer(s32 player)
{
	return ((player >= 0) && (player < MAXCONTROLLERS)) ? TRUE : FALSE;
}

void syNetInputClearFrame(SYNetInputFrame *frame)
{
	frame->tick = 0;
	frame->buttons = 0;
	frame->stick_x = 0;
	frame->stick_y = 0;
	frame->source = nSYNetInputSourceLocal;
	frame->is_predicted = FALSE;
	frame->is_valid = FALSE;
}

void syNetInputMakeFrame(SYNetInputFrame *frame, u32 tick, u16 buttons, s8 stick_x, s8 stick_y, SYNetInputSource source, sb32 is_predicted)
{
	frame->tick = tick;
	frame->buttons = buttons;
	frame->stick_x = stick_x;
	frame->stick_y = stick_y;
	frame->source = source;
	frame->is_predicted = is_predicted;
	frame->is_valid = TRUE;
}

void syNetInputStoreFrame(SYNetInputFrame history[][SYNETINPUT_HISTORY_LENGTH], s32 player, SYNetInputFrame *frame)
{
	history[player][frame->tick % SYNETINPUT_HISTORY_LENGTH] = *frame;
}

sb32 syNetInputGetStoredFrame(SYNetInputFrame history[][SYNETINPUT_HISTORY_LENGTH], s32 player, u32 tick, SYNetInputFrame *out_frame)
{
	SYNetInputFrame *frame;

	if (syNetInputCheckPlayer(player) == FALSE)
	{
		return FALSE;
	}
	frame = &history[player][tick % SYNETINPUT_HISTORY_LENGTH];

	if ((frame->is_valid == FALSE) || (frame->tick != tick))
	{
		return FALSE;
	}
	if (out_frame != NULL)
	{
		*out_frame = *frame;
	}
	return TRUE;
}

void syNetInputReset(void)
{
	s32 player;
	s32 i;

	sSYNetInputTick = 0;
	sSYNetInputRecordedFrameCount = 0;
	sSYNetInputIsRecording = FALSE;
	sSYNetInputIsReplayMetadataValid = FALSE;
	sSYNetInputPublishedChecksum = 2166136261U;
	sSYNetInputPublishedTickCount = 0;
	sSYNetInputPublishedChecksumLimit = 0;
	sSYNetInputPublishedChecksumAtLimit = 0;
	sSYNetInputIsMispredicted = FALSE;
	sSYNetInputMispredictTick = 0;

	sSYNetInputReplayMetadata.magic = SYNETINPUT_REPLAY_MAGIC;
	sSYNetInputReplayMetadata.version = SYNETINPUT_REPLAY_VERSION;
	sSYNetInputReplayMetadata.scene_kind = 0;
	sSYNetInputReplayMetadata.player_count = 0;
	sSYNetInputReplayMetadata.stage_kind = 0;
	sSYNetInputReplayMetadata.stocks = 0;
	sSYNetInputReplayMetadata.time_limit = 0;
	sSYNetInputReplayMetadata.item_switch = 0;
	sSYNetInputReplayMetadata.item_toggles = 0;
	sSYNetInputReplayMetadata.rng_seed = 0;
	sSYNetInputReplayMetadata.game_type = 0;
	sSYNetInputReplayMetadata.game_rules = 0;
	sSYNetInputReplayMetadata.is_team_battle = FALSE;
	sSYNetInputReplayMetadata.handicap = 0;
	sSYNetInputReplayMetadata.is_team_attack = FALSE;
	sSYNetInputReplayMetadata.is_stage_select = FALSE;
	sSYNetInputReplayMetadata.damage_ratio = 0;
	sSYNetInputReplayMetadata.item_appearance_rate = 0;
	sSYNetInputReplayMetadata.is_not_teamshadows = FALSE;

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		sSYNetInputSlots[player].source = nSYNetInputSourceLocal;
		syNetInputClearFrame(&sSYNetInputSlots[player].last_confirmed);
		syNetInputClearFrame(&sSYNetInputSlots[player].last_published);
		sSYNetInputReplayMetadata.player_kinds[player] = 0;
		sSYNetInputReplayMetadata.fighter_kinds[player] = 0;
		sSYNetInputReplayMetadata.costumes[player] = 0;
		sSYNetInputReplayMetadata.teams[player] = 0;
		sSYNetInputReplayMetadata.handicaps[player] = 0;
		sSYNetInputReplayMetadata.levels[player] = 0;
		sSYNetInputReplayMetadata.shades[player] = 0;

		for (i = 0; i < SYNETINPUT_HISTORY_LENGTH; i++)
		{
			syNetInputClearFrame(&sSYNetInputHistory[player][i]);
			syNetInputClearFrame(&sSYNetInputRemoteHistory[player][i]);
			syNetInputClearFrame(&sSYNetInputSavedHistory[player][i]);
			syNetInputClearFrame(&sSYNetInputLocalSchedule[player][i]);
		}
		sSYNetInputRemoteNextUnconfirmed[player] = 0;
		sSYNetInputDropTick[player] = SYNETINPUT_NO_DROP;
		sSYNetInputFreezeTick[player] = SYNETINPUT_NO_DROP;
		sSYNetInputLocalDevice[player] = player;
		for (i = 0; i < SYNETINPUT_REPLAY_MAX_FRAMES; i++)
		{
			syNetInputClearFrame(&sSYNetInputReplayFrames[player][i]);
		}
	}
}

void syNetInputStartVSSession(void)
{
	syNetInputReset();
}

void syNetInputSetSlotSource(s32 player, SYNetInputSource source)
{
	if (syNetInputCheckPlayer(player) != FALSE)
	{
		sSYNetInputSlots[player].source = source;
	}
}

SYNetInputSource syNetInputGetSlotSource(s32 player)
{
	if (syNetInputCheckPlayer(player) == FALSE)
	{
		return nSYNetInputSourceLocal;
	}
	return sSYNetInputSlots[player].source;
}

void syNetInputSetRemoteInput(s32 player, u32 tick, u16 buttons, s8 stick_x, s8 stick_y)
{
	SYNetInputFrame frame;
	SYNetInputFrame published;

	if (syNetInputCheckPlayer(player) == FALSE)
	{
		return;
	}
	if ((tick >= sSYNetInputDropTick[player]) || (tick >= sSYNetInputFreezeTick[player]))
	{
		return;     // the player is gone from this tick on (or about to be): its input there is not taken
	}
	if (syNetInputGetStoredFrame(sSYNetInputRemoteHistory, player, tick, NULL) != FALSE)
	{
		return;     // redundant copy (every packet repeats recent frames)
	}
	syNetInputMakeFrame(&frame, tick, buttons, stick_x, stick_y, nSYNetInputSourceRemoteConfirmed, FALSE);
	syNetInputStoreFrame(sSYNetInputRemoteHistory, player, &frame);

	if ((sSYNetInputSlots[player].last_confirmed.is_valid == FALSE) || (tick >= sSYNetInputSlots[player].last_confirmed.tick))
	{
		sSYNetInputSlots[player].last_confirmed = frame;
	}
	while (syNetInputGetStoredFrame(sSYNetInputRemoteHistory, player, sSYNetInputRemoteNextUnconfirmed[player], NULL) != FALSE)
	{
		sSYNetInputRemoteNextUnconfirmed[player]++;
	}
	/* Already simulated with a guess? Flag a rollback if the guess was wrong. */
	if ((tick < sSYNetInputTick) && (syNetInputGetStoredFrame(sSYNetInputHistory, player, tick, &published) != FALSE))
	{
		if ((published.buttons != buttons) || (published.stick_x != stick_x) || (published.stick_y != stick_y))
		{
			if ((sSYNetInputIsMispredicted == FALSE) || (tick < sSYNetInputMispredictTick))
			{
				sSYNetInputMispredictTick = tick;
				sSYNetInputIsMispredicted = TRUE;
			}
		}
	}
}

/* Ticks before the remote's input delay can never be sent (nothing was
 * sampled that early); both sides treat them as neutral. */
void syNetInputPreconfirmRemote(s32 player, u32 tick_count)
{
	u32 tick;

	for (tick = 0; tick < tick_count; tick++)
	{
		syNetInputSetRemoteInput(player, tick, 0, 0, 0);
	}
}

u32 syNetInputGetRemoteNextUnconfirmed(s32 player)
{
	if (syNetInputCheckPlayer(player) == FALSE)
	{
		return 0;
	}
	/* Every input below the drop tick is in: nothing is left to wait for. */
	if (sSYNetInputRemoteNextUnconfirmed[player] >= sSYNetInputDropTick[player])
	{
		return 0xFFFFFFFFU;
	}
	return sSYNetInputRemoteNextUnconfirmed[player];
}

/* From `tick` on, `player` plays neutral input (it dropped out). Ticks that were
 * already simulated at or after it used a guess or a late real input: flag a
 * rollback to the drop tick so they are replayed with the neutral input. */
void syNetInputSetDropTick(s32 player, u32 tick)
{
	if ((syNetInputCheckPlayer(player) == FALSE) || (sSYNetInputDropTick[player] != SYNETINPUT_NO_DROP))
	{
		return;
	}
	sSYNetInputDropTick[player] = tick;

	if (tick < sSYNetInputTick)
	{
		if ((sSYNetInputIsMispredicted == FALSE) || (tick < sSYNetInputMispredictTick))
		{
			sSYNetInputMispredictTick = tick;
			sSYNetInputIsMispredicted = TRUE;
		}
	}
}

void syNetInputSetFreezeTick(s32 player, u32 tick)
{
	if (syNetInputCheckPlayer(player) != FALSE)
	{
		sSYNetInputFreezeTick[player] = tick;
	}
}

u32 syNetInputGetDropTick(s32 player)
{
	return (syNetInputCheckPlayer(player) != FALSE) ? sSYNetInputDropTick[player] : SYNETINPUT_NO_DROP;
}

sb32 syNetInputGetRemoteConfirmedFrame(s32 player, u32 tick, SYNetInputFrame *out_frame)
{
	return syNetInputGetStoredFrame(sSYNetInputRemoteHistory, player, tick, out_frame);
}

sb32 syNetInputTakeMispredictTick(u32 *out_tick)
{
	if (sSYNetInputIsMispredicted == FALSE)
	{
		return FALSE;
	}
	*out_tick = sSYNetInputMispredictTick;
	sSYNetInputIsMispredicted = FALSE;

	return TRUE;
}

void syNetInputSetLocalDelay(u32 delay)
{
	sSYNetInputLocalDelay = delay;
}

u32 syNetInputGetLocalDelay(void)
{
	return sSYNetInputLocalDelay;
}

sb32 syNetInputGetLocalScheduledFrame(s32 player, u32 tick, SYNetInputFrame *out_frame)
{
	return syNetInputGetStoredFrame(sSYNetInputLocalSchedule, player, tick, out_frame);
}

void syNetInputSetLocalDevice(s32 player, s32 device)
{
	if ((syNetInputCheckPlayer(player) != FALSE) && (device >= 0) && (device < MAXCONTROLLERS))
	{
		sSYNetInputLocalDevice[player] = device;
	}
}

void syNetInputSetFuzzSeed(u32 seed)
{
	sSYNetInputFuzzSeed = seed;
}

/* Deterministic pseudo-random controller state, held ~12 ticks at a time.
 * One button at most (never START, so no pause or A+B+R+Z reset). */
static void syNetInputMakeFuzzState(s32 player, u32 tick, u16 *buttons, s8 *stick_x, s8 *stick_y)
{
	static const u16 fuzz_buttons[8] = { 0, 0, 0, A_BUTTON, B_BUTTON, Z_TRIG, R_TRIG, A_BUTTON };
	u32 h = (sSYNetInputFuzzSeed * 2654435761U) ^ ((u32)player * 0x9E3779B9U) ^ ((tick / 12) * 0x85EBCA6BU);

	h ^= h >> 15;
	h *= 0x2C1B3C6DU;
	h ^= h >> 12;
	h *= 0x297A2D39U;
	h ^= h >> 15;

	*stick_x = (s8)((s32)((h >> 0) & 0xFF) % 161 - 80);
	*stick_y = (s8)((s32)((h >> 8) & 0xFF) % 161 - 80);
	*buttons = fuzz_buttons[(h >> 16) & 7];
}

void syNetInputSetSavedInput(s32 player, u32 tick, u16 buttons, s8 stick_x, s8 stick_y)
{
	SYNetInputFrame frame;

	if (syNetInputCheckPlayer(player) != FALSE)
	{
		syNetInputMakeFrame(&frame, tick, buttons, stick_x, stick_y, nSYNetInputSourceSaved, FALSE);
		syNetInputStoreFrame(sSYNetInputSavedHistory, player, &frame);
	}
}

void syNetInputMakeLocalFrame(s32 player, u32 tick, SYNetInputFrame *out_frame)
{
	SYController *controller = &sSYNetInputHardware[sSYNetInputLocalDevice[player]];
	SYNetInputFrame scheduled;
	u16 buttons = controller->button_hold;
	s8 stick_x = controller->stick_range.x;
	s8 stick_y = controller->stick_range.y;

	if (sSYNetInputFuzzSeed != 0)
	{
		syNetInputMakeFuzzState(player, tick, &buttons, &stick_x, &stick_y);
	}
	if ((sSYNetInputLocalDelay == 0) && (sSYNetInputFuzzSeed == 0))
	{
		syNetInputMakeFrame(out_frame, tick, buttons, stick_x, stick_y, nSYNetInputSourceLocal, FALSE);
		return;
	}
	/* Schedule this sample once; a stalled tick re-reads the controller but
	 * must not rewrite a frame that may already be on the wire. */
	if (syNetInputGetStoredFrame(sSYNetInputLocalSchedule, player, tick + sSYNetInputLocalDelay, NULL) == FALSE)
	{
		syNetInputMakeFrame(&scheduled, tick + sSYNetInputLocalDelay, buttons, stick_x, stick_y, nSYNetInputSourceLocal, FALSE);
		syNetInputStoreFrame(sSYNetInputLocalSchedule, player, &scheduled);
	}
	if (syNetInputGetStoredFrame(sSYNetInputLocalSchedule, player, tick, out_frame) == FALSE)
	{
		syNetInputMakeFrame(out_frame, tick, 0, 0, 0, nSYNetInputSourceLocal, FALSE);
	}
}

void syNetInputMakePredictedFrame(s32 player, u32 tick, SYNetInputFrame *out_frame)
{
	SYNetInputFrame *last_confirmed = &sSYNetInputSlots[player].last_confirmed;

	if (last_confirmed->is_valid != FALSE)
	{
		syNetInputMakeFrame
		(
			out_frame,
			tick,
			last_confirmed->buttons,
			last_confirmed->stick_x,
			last_confirmed->stick_y,
			nSYNetInputSourceRemotePredicted,
			TRUE
		);
	}
	else syNetInputMakeFrame(out_frame, tick, 0, 0, 0, nSYNetInputSourceRemotePredicted, TRUE);
}

void syNetInputResolveFrame(s32 player, u32 tick, SYNetInputFrame *out_frame)
{
	switch (sSYNetInputSlots[player].source)
	{
	case nSYNetInputSourceRemoteConfirmed:
	case nSYNetInputSourceRemotePredicted:
		if (tick >= sSYNetInputDropTick[player])
		{
			syNetInputMakeFrame(out_frame, tick, 0, 0, 0, nSYNetInputSourceRemoteConfirmed, FALSE);
		}
		else if (syNetInputGetStoredFrame(sSYNetInputRemoteHistory, player, tick, out_frame) != FALSE)
		{
			sSYNetInputSlots[player].last_confirmed = *out_frame;
		}
		else syNetInputMakePredictedFrame(player, tick, out_frame);
		break;

	case nSYNetInputSourceSaved:
		if (syNetInputGetReplayFrame(player, tick, out_frame) != FALSE)
		{
			out_frame->source = nSYNetInputSourceSaved;
			out_frame->is_predicted = FALSE;
		}
		else if (syNetInputGetStoredFrame(sSYNetInputSavedHistory, player, tick, out_frame) == FALSE)
		{
			syNetInputMakeFrame(out_frame, tick, 0, 0, 0, nSYNetInputSourceSaved, FALSE);
		}
		break;

	case nSYNetInputSourceLocal:
	default:
		syNetInputMakeLocalFrame(player, tick, out_frame);
		break;
	}
}

/* Taps/releases for a tick are derived against the stored frame of tick-1 (cleared at tick 0),
 * never against whatever was published last. A tick that is held for several frames (start
 * barrier, stall) is published once per frame; without this the second publish would see its own
 * buttons as "previous" and lose the tap/release, and peers that stall differently would diverge. */
static void syNetInputSeedLastPublished(s32 player, u32 tick)
{
	SYNetInputFrame prev;

	if ((tick == 0) || (syNetInputGetStoredFrame(sSYNetInputHistory, player, tick - 1, &prev) == FALSE))
	{
		syNetInputClearFrame(&prev);
	}
	sSYNetInputSlots[player].last_published = prev;
}

void syNetInputPublishFrame(s32 player, SYNetInputFrame *frame)
{
	SYNetInputFrame *last_published = &sSYNetInputSlots[player].last_published;
	u16 prev_buttons = (last_published->is_valid != FALSE) ? last_published->buttons : 0;
	u16 pressed = (frame->buttons ^ prev_buttons) & frame->buttons;
	u16 released = (frame->buttons ^ prev_buttons) & prev_buttons;

	gSYControllerDevices[player].button_hold = frame->buttons;
	gSYControllerDevices[player].button_tap = pressed;
	gSYControllerDevices[player].button_release = released;
	gSYControllerDevices[player].button_update = pressed;
	gSYControllerDevices[player].stick_range.x = frame->stick_x;
	gSYControllerDevices[player].stick_range.y = frame->stick_y;

	sSYNetInputSlots[player].last_published = *frame;
	syNetInputStoreFrame(sSYNetInputHistory, player, frame);
}

/* Rollback resimulation: re-publish the inputs of an already-simulated tick.
 * Local slots replay what was sampled then (never the live controller);
 * remote/saved slots re-resolve, so a confirmed remote input that arrived
 * since the tick first ran replaces its prediction. Taps and releases are
 * derived against the tick before, exactly as on the first run. */
void syNetInputRepublishTick(u32 tick)
{
	SYNetInputFrame frame;
	s32 player;

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		if (sSYNetInputSlots[player].source == nSYNetInputSourceLocal)
		{
			if (syNetInputGetStoredFrame(sSYNetInputHistory, player, tick, &frame) == FALSE)
			{
				syNetInputMakeFrame(&frame, tick, 0, 0, 0, nSYNetInputSourceLocal, FALSE);
			}
		}
		else syNetInputResolveFrame(player, tick, &frame);

		syNetInputSeedLastPublished(player, tick);
		syNetInputPublishFrame(player, &frame);
	}
	syNetInputPublishMainController();
}

void syNetInputPublishMainController(void)
{
	gSYControllerMain.button_hold = gSYControllerDevices[0].button_hold;
	gSYControllerMain.button_tap = gSYControllerDevices[0].button_tap;
	gSYControllerMain.button_update = gSYControllerDevices[0].button_update;
	gSYControllerMain.button_release = gSYControllerDevices[0].button_release;
	gSYControllerMain.stick_range.x = gSYControllerDevices[0].stick_range.x;
	gSYControllerMain.stick_range.y = gSYControllerDevices[0].stick_range.y;
}

sb32 syNetInputGetHistoryFrame(s32 player, u32 tick, SYNetInputFrame *out_frame)
{
	return syNetInputGetStoredFrame(sSYNetInputHistory, player, tick, out_frame);
}

sb32 syNetInputGetPublishedFrame(s32 player, SYNetInputFrame *out_frame)
{
	if (syNetInputCheckPlayer(player) == FALSE)
	{
		return FALSE;
	}
	if (sSYNetInputSlots[player].last_published.is_valid == FALSE)
	{
		return FALSE;
	}
	if (out_frame != NULL)
	{
		*out_frame = sSYNetInputSlots[player].last_published;
	}
	return TRUE;
}

u32 syNetInputGetHistoryChecksum(s32 player, u32 tick_begin, u32 frame_count)
{
	SYNetInputFrame frame;
	u32 checksum = 2166136261U;
	u32 i;

	for (i = 0; i < frame_count; i++)
	{
		if (syNetInputGetHistoryFrame(player, tick_begin + i, &frame) != FALSE)
		{
			checksum ^= frame.tick;
			checksum *= 16777619U;
			checksum ^= frame.buttons;
			checksum *= 16777619U;
			checksum ^= (u8)frame.stick_x;
			checksum *= 16777619U;
			checksum ^= (u8)frame.stick_y;
			checksum *= 16777619U;
			checksum ^= frame.source;
			checksum *= 16777619U;
			checksum ^= frame.is_predicted;
			checksum *= 16777619U;
		}
	}
	return checksum;
}

u32 syNetInputAccumulateInputChecksum(u32 checksum, s32 player, SYNetInputFrame *frame)
{
	checksum ^= (u32)player;
	checksum *= 16777619U;
	checksum ^= frame->tick;
	checksum *= 16777619U;
	checksum ^= frame->buttons;
	checksum *= 16777619U;
	checksum ^= (u8)frame->stick_x;
	checksum *= 16777619U;
	checksum ^= (u8)frame->stick_y;
	checksum *= 16777619U;

	return checksum;
}

u32 syNetInputGetHistoryInputValueChecksumForPlayer(s32 player, u32 tick_begin, u32 frame_count)
{
	SYNetInputFrame frame;
	u32 checksum = 2166136261U;
	u32 i;

	for (i = 0; i < frame_count; i++)
	{
		if (syNetInputGetHistoryFrame(player, tick_begin + i, &frame) != FALSE)
		{
			checksum = syNetInputAccumulateInputChecksum(checksum, player, &frame);
		}
	}
	return checksum;
}

void syNetInputGetHistoryInputValueChecksumWindow(u32 tick_begin, u32 frame_count, u32 *out_checksums,
                                                  u32 *out_combined_checksum)
{
	SYNetInputFrame frame;
	u32 checksum = 2166136261U;
	u32 tick_limit;
	u32 tick;
	s32 player;

	tick_limit = tick_begin + frame_count;

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		u32 player_checksum = 2166136261U;

		for (tick = tick_begin; tick < tick_limit; tick++)
		{
			if (syNetInputGetHistoryFrame(player, tick, &frame) != FALSE)
			{
				player_checksum = syNetInputAccumulateInputChecksum(player_checksum, player, &frame);
			}
		}
		checksum ^= player_checksum;
		checksum *= 16777619U;

		if (out_checksums != NULL)
		{
			out_checksums[player] = player_checksum;
		}
	}
	if (out_combined_checksum != NULL)
	{
		*out_combined_checksum = checksum;
	}
}

void syNetInputSetRecordingEnabled(sb32 is_enabled)
{
	sSYNetInputIsRecording = is_enabled;

	if (is_enabled != FALSE)
	{
		sSYNetInputRecordedFrameCount = 0;
	}
}

sb32 syNetInputGetRecordingEnabled(void)
{
	return sSYNetInputIsRecording;
}

u32 syNetInputGetRecordedFrameCount(void)
{
	return sSYNetInputRecordedFrameCount;
}

void syNetInputClearReplayFrames(void)
{
	s32 player;
	s32 i;

	sSYNetInputRecordedFrameCount = 0;

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		for (i = 0; i < SYNETINPUT_REPLAY_MAX_FRAMES; i++)
		{
			syNetInputClearFrame(&sSYNetInputReplayFrames[player][i]);
		}
	}
}

sb32 syNetInputSetReplayFrame(s32 player, u32 tick, const SYNetInputFrame *frame)
{
	if ((syNetInputCheckPlayer(player) == FALSE) || (frame == NULL) || (tick >= SYNETINPUT_REPLAY_MAX_FRAMES))
	{
		return FALSE;
	}
	sSYNetInputReplayFrames[player][tick] = *frame;
	sSYNetInputReplayFrames[player][tick].tick = tick;
	sSYNetInputReplayFrames[player][tick].is_valid = TRUE;

	if (sSYNetInputRecordedFrameCount < (tick + 1))
	{
		sSYNetInputRecordedFrameCount = tick + 1;
	}
	return TRUE;
}

sb32 syNetInputGetReplayFrame(s32 player, u32 tick, SYNetInputFrame *out_frame)
{
	SYNetInputFrame *frame;

	if ((syNetInputCheckPlayer(player) == FALSE) || (tick >= SYNETINPUT_REPLAY_MAX_FRAMES))
	{
		return FALSE;
	}
	frame = &sSYNetInputReplayFrames[player][tick];

	if ((frame->is_valid == FALSE) || (frame->tick != tick))
	{
		return FALSE;
	}
	if (out_frame != NULL)
	{
		*out_frame = *frame;
	}
	return TRUE;
}

u32 syNetInputGetReplayInputChecksum(void)
{
	SYNetInputFrame frame;
	u32 checksum = 2166136261U;
	u32 tick;
	s32 player;

	for (tick = 0; tick < sSYNetInputRecordedFrameCount; tick++)
	{
		for (player = 0; player < MAXCONTROLLERS; player++)
		{
			if (syNetInputGetReplayFrame(player, tick, &frame) != FALSE)
			{
				checksum = syNetInputAccumulateInputChecksum(checksum, player, &frame);
			}
		}
	}
	return checksum;
}

void syNetInputSetReplayMetadata(const SYNetInputReplayMetadata *metadata)
{
	if (metadata != NULL)
	{
		sSYNetInputReplayMetadata = *metadata;
		sSYNetInputReplayMetadata.magic = SYNETINPUT_REPLAY_MAGIC;
		sSYNetInputReplayMetadata.version = SYNETINPUT_REPLAY_VERSION;
		sSYNetInputIsReplayMetadataValid = TRUE;
	}
}

sb32 syNetInputGetReplayMetadata(SYNetInputReplayMetadata *out_metadata)
{
	if (sSYNetInputIsReplayMetadataValid == FALSE)
	{
		return FALSE;
	}
	if (out_metadata != NULL)
	{
		*out_metadata = sSYNetInputReplayMetadata;
	}
	return TRUE;
}

void syNetInputFuncRead(void)
{
	SYNetInputFrame frame;
	u32 tick;
	s32 player;

	syControllerFuncRead();

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		sSYNetInputHardware[player] = gSYControllerDevices[player];
	}
	syNetPeerBeginFrame();
	tick = syNetInputGetTick();

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		syNetInputResolveFrame(player, tick, &frame);
		syNetInputSeedLastPublished(player, tick);
		syNetInputPublishFrame(player, &frame);

		if (sSYNetInputIsRecording != FALSE)
		{
			syNetInputSetReplayFrame(player, tick, &frame);
		}
	}
	syNetInputPublishMainController();

	if (syNetPeerCheckStartBarrierReleased() == FALSE)
	{
		return;
	}
	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		sSYNetInputPublishedChecksum = syNetInputAccumulateInputChecksum(
			sSYNetInputPublishedChecksum, player, &sSYNetInputSlots[player].last_published);
	}
	sSYNetInputPublishedTickCount++;

	if ((sSYNetInputPublishedChecksumLimit != 0) && (sSYNetInputPublishedTickCount == sSYNetInputPublishedChecksumLimit))
	{
		sSYNetInputPublishedChecksumAtLimit = sSYNetInputPublishedChecksum;
	}
	sSYNetInputTick++;
}

void syNetInputSetPublishedChecksumLimit(u32 tick_limit)
{
	sSYNetInputPublishedChecksumLimit = tick_limit;
	sSYNetInputPublishedChecksumAtLimit = sSYNetInputPublishedChecksum;
}

u32 syNetInputGetPublishedTickCount(void)
{
	return sSYNetInputPublishedTickCount;
}

u32 syNetInputGetPublishedInputChecksum(void)
{
	if ((sSYNetInputPublishedChecksumLimit != 0) && (sSYNetInputPublishedTickCount >= sSYNetInputPublishedChecksumLimit))
	{
		return sSYNetInputPublishedChecksumAtLimit;
	}
	return sSYNetInputPublishedChecksum;
}
