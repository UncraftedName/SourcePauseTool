#include "stdafx.hpp"

#include "tr_structs.hpp"

#ifdef SPT_PLAYER_TRACE_ENABLED

#include <unordered_map>
#include <algorithm>

#include "tr_record_cache.hpp"
#include "tr_render_cache.hpp"
#include "tr_multiple_player.hpp"
#include "imgui/tr_imgui.hpp"

#include "spt/feature.hpp"
#include "signals.hpp"
#include "spt/utils/ent_list.hpp"
#include "spt/utils/interfaces.hpp"
#include "spt/utils/file.hpp"
#include "spt/utils/game_detection.hpp"
#include "spt/utils/serialize.hpp"
#include "spt/features/hud.hpp"
#include "spt/features/visualizations/imgui/imgui_interface.hpp"
#include "spt/features/portalled_pause.hpp"
#include "spt/features/visualizations/fcps/fcps_config.hpp"
#include "spt/features/visualizations/fcps/fcps_event.hpp"

#include <ShlObj_core.h>
#include <Shlwapi.h>

#ifdef clamp
#undef clamp
#endif

using namespace player_trace;

// RAII context to disable certain render options while drawing multiple traces
class TrMultiTraceRenderContext
{
	// clang-format off
	struct DisableContext
	{
		bool& ref;
		bool oldVal;
		DisableContext(bool& b) : oldVal(b), ref(b) { ref = false; }
		~DisableContext() { ref = oldVal; }
	};
	// clang-format on

	DisableContext dcEntPhys, dcEntObb, dcEntCollect, dcPortals;

public:
	TrMultiTraceRenderContext(TrRenderStyleConfig& renderCfg)
	    : dcEntPhys(renderCfg.entPhys.draw)
	    , dcEntObb(renderCfg.entObb.draw)
	    , dcEntCollect(renderCfg.entCollectAabb.draw)
	    , dcPortals(renderCfg.portals.draw)
	{
	}
};

class PlayerTraceFeature : public FeatureWrapper<PlayerTraceFeature>
{
public:
	void ChangeDisplayTick(int diff);
	void SetDisplayTick(tr_tick val);

	// TODO this should really be moved into the player huh
	tr_tick absDrawTick = 0;

	// allows us to notify the trace of things that happened from other features
	TrSegmentReason deferredSegmentReason = TR_SR_NONE;

protected:
	virtual bool ShouldLoadFeature() override;
	virtual void LoadFeature() override;
	virtual void UnloadFeature() override;

private:
	void OnTickSignal(bool simulating);
	void OnFinishRestoreSignal(void*);
	void OnFcpsSignal(const FcpsEvent& event);
	void OnMeshRenderSignal(MeshRendererDelegate& mr);
#ifdef SPT_HUD_ENABLED
	void OnHudCallback();
#endif
	void OnPortalTeleportSignal(IServerEntity* portal, IServerEntity* ent);
};

static PlayerTraceFeature spt_player_trace_feat;

static void CC_Trace_Start(const std::filesystem::path& fsPath)
{
	auto& tp = TrTracePlayer::Singleton();
	auto rt = tp.GetRecordingTraceHandle();
	if (rt != tp.Traces().end())
	{
		if (TrTracePlayer::trace_map::key_compare{}.Compare3Way(rt->first, fsPath) == 0)
		{
			Warning("Overwriting in-progress trace recording\n");
		}
		else
		{
			Warning("Trace recording to '%s' already in progress, stopping\n", rt->first.string().c_str());
		}

		// flush silently
		ser::StatusTracker stat;
		tp.StopAndFlushRecording(stat);
	}

	ser::StatusTracker stat;
	tp.TryStartRecording(fsPath, stat);
	if (stat.Ok())
	{
		spt_player_trace_feat.deferredSegmentReason = TR_SR_NONE;
		Msg("Started recording trace, '%s' will stop recording and write to '%s'\n",
		    "spt_trace_stop",
		    fsPath.string().c_str());
	}
	else
	{
		Warning("Error: %s\n", stat.GetStatus().errMsg.c_str());
	}
}

CON_COMMAND_F(spt_trace_start,
              "Creates a file and starts recording the player trace;"
              " if no file is specified, uses '" TR_DEFAULT_PATH "'",
              FCVAR_DONTRECORD)
{
	const char* userStrPath = args.ArgC() < 2 ? TR_DEFAULT_PATH : args.Arg(1);
	std::error_code ec;
	std::filesystem::path fsPath = utils::ResolveUserPath(userStrPath, TR_COMPRESSED_FILE_EXT, ec);
	if (ec)
	{
		Warning("Failed to resolve '%s' to a valid path: %s\n", userStrPath, ec.message().c_str());
		return;
	}
	CC_Trace_Start(fsPath);
}

CON_COMMAND_F(spt_trace_start_auto_increment,
              "Creates a file and starts recording the player trace;"
              " if the given path exists, creates a new one",
              FCVAR_DONTRECORD)
{
	const char* userStrPath = args.ArgC() < 2 ? TR_DEFAULT_PATH : args.Arg(1);
	std::error_code ec;
	std::filesystem::path fsPath = utils::ResolveUserPath(userStrPath, TR_COMPRESSED_FILE_EXT, ec);
	if (ec)
	{
		Warning("Failed to resolve '%s' to a valid path: %s\n", userStrPath, ec.message().c_str());
		return;
	}

	static std::unordered_map<std::filesystem::path, size_t> counterLookup;
	auto [it, _] = counterLookup.emplace(fsPath, 1);

	Assert(fsPath.string().ends_with(TR_COMPRESSED_FILE_EXT));
	auto basePathView = std::wstring_view(fsPath.c_str());
	basePathView.remove_suffix(strlen(TR_COMPRESSED_FILE_EXT));

	fsPath = utils::GetNextFileName(basePathView, TR_COMPRESSED_FILE_EXT, &it->second);
	CC_Trace_Start(fsPath);
}

CON_COMMAND_F(spt_trace_stop,
              "Stops recording the current player trace, writes it to disk, and draws it",
              FCVAR_DONTRECORD)
{
	auto& tp = TrTracePlayer::Singleton();
	if (!tp.GetRecordingTrace())
	{
		Warning("SPT: No active trace!\n");
		return;
	}

	ser::StatusTracker stat;
	auto it = tp.StopAndFlushRecording(stat);

	if (stat.Ok())
		Msg("Done, recorded for %d ticks.\n", it->second.tr.numRecordedTicks);
	else
		Msg("Error: %s\n", stat.GetStatus().errMsg.c_str());
}

CON_COMMAND_F(spt_trace_list, "List all loaded traces", FCVAR_DONTRECORD)
{
	auto& tp = TrTracePlayer::Singleton();
	auto& traces = tp.Traces();
	if (traces.empty())
	{
		Msg("No loaded traces\n");
		return;
	}

	auto rt = tp.GetRecordingTraceHandle();

	// TODO use BufferedCmdWriter from fcps

	for (auto it = traces.begin(); it != traces.end(); ++it)
	{
		Msg("\"%s\" %u ticks%s\n",
		    it->first.string().c_str(),
		    it->second.tr.numRecordedTicks,
		    it == rt ? " - RECORDING" : "");
	}
}

CON_COMMAND_F(spt_trace_unload, "Unload trace(s) with the given path(s), supports wildcards", FCVAR_DONTRECORD)
{
	if (args.ArgC() == 1)
	{
		Warning("Usage: %s [path_spec]\n", spt_trace_unload_command.GetName());
		return;
	}

	std::filesystem::path pathSpec;

	std::string_view userSv = args.Arg(1);
	if (userSv.starts_with('*'))
	{
		pathSpec = args.Arg(1);
	}
	else
	{
		std::error_code ec;
		pathSpec = utils::ResolveUserPath(args.Arg(1), TR_COMPRESSED_FILE_EXT, ec);
		if (ec)
		{
			Warning("Failed to resolve '%s' to a valid path: %s\n", args.Arg(1), ec.message().c_str());
			return;
		}
	}

	auto& tp = TrTracePlayer::Singleton();
	auto& traces = tp.Traces();
	auto rt = tp.GetRecordingTraceHandle();

	size_t nDeleted = 0;
	for (auto it = TrTracePlayer::entry_handle::Begin(traces); it != TrTracePlayer::entry_handle::End(traces);)
	{
		if (it == rt)
			continue;
		if (PathMatchSpecW(it->first.c_str(), pathSpec.c_str()))
		{
			tp.Remove(it++);
			++nDeleted;
		}
		else
		{
			++it;
		}
	}

	Msg("%u traces unloaded using path spec '%s'\n", nDeleted, pathSpec.string().c_str());
}

CON_COMMAND_F(spt_trace_unload_all, "Unloads all non-recording traces", FCVAR_DONTRECORD)
{
	Msg("Deleted %u traces\n", TrTracePlayer::Singleton().ClearTraces(false));
}

CON_COMMAND_F(spt_trace_next_tick, "Increments the trace draw tick", FCVAR_DONTRECORD)
{
	spt_player_trace_feat.ChangeDisplayTick(1);
}

CON_COMMAND_F(spt_trace_prev_tick, "Decrements the trace draw tick", FCVAR_DONTRECORD)
{
	spt_player_trace_feat.ChangeDisplayTick(-1);
}

CON_COMMAND_F(spt_trace_set_tick, "Sets the trace draw tick", FCVAR_DONTRECORD)
{
	if (args.ArgC() < 2)
	{
		Warning("Must provide an integer value\n");
		return;
	}
	spt_player_trace_feat.SetDisplayTick(strtoul(args.Arg(1), nullptr, 10));
}

CON_COMMAND_AUTOCOMPLETEFILE(spt_trace_import,
                             "Load trace(s) from binary file(s), supports wildcards",
                             FCVAR_DONTRECORD,
                             "",
                             TR_COMPRESSED_FILE_EXT)
{
	if (args.ArgC() < 2)
	{
		Msg("Usage: %s <fileName>\n", spt_trace_import_command.GetName());
		return;
	}

	std::error_code ec;
	std::filesystem::path pathSpec = utils::ResolveUserPath(args.Arg(1), TR_COMPRESSED_FILE_EXT, ec);
	if (ec)
	{
		Warning("Failed to resolve '%s' to a valid path: %s\n", args.Arg(1), ec.message().c_str());
		return;
	}

	/*
	* I had a bit of a struggle with FindFirstFileW as it didn't seem to work with exact absolute
	* paths. A silly workaround - instead we iterate over all the files in the directory and
	* PathMatchSpecW each filename instead.
	*/

	size_t nNewImports = 0, nAlreadyImported = 0;
	bool anyErrors = false;
	auto& tp = TrTracePlayer::Singleton();
	TrTracePlayer::entry_handle lastAddedTrace{};
	std::filesystem::path parentPath = pathSpec.parent_path();
	std::filesystem::path fileSpec = pathSpec.filename();

	ser::StatusTracker status;

	for (auto& entry : std::filesystem::directory_iterator(parentPath, ec))
	{
		if (!entry.is_regular_file())
			continue;
		std::filesystem::path newPath = entry.path();
		if (!PathMatchSpecW(newPath.filename().c_str(), fileSpec.c_str()))
			continue;
		status = ser::StatusTracker{};
		auto [handle, isNew] = tp.TryLoadFromDisk(newPath, status);
		if (!status.Ok())
		{
			Warning("Error importing file '%s': %s\n",
			        newPath.string().c_str(),
			        status.GetStatus().errMsg.c_str());
			anyErrors = true;
		}
		else if (isNew)
		{
			++nNewImports;
			lastAddedTrace = handle;
		}
		else
		{
			++nAlreadyImported;
		}
	}

	if (ec)
	{
		Warning("Failed to iterate over directories in '%s': %s\n",
		        parentPath.string().c_str(),
		        ec.message().c_str());
		return;
	}

	// only 1 trace imported? assume it was an exact path and spew a bunch more info

	if (nNewImports == 1 && !anyErrors)
	{
		auto& newTr = lastAddedTrace->second.tr;
		TrReadContextScope scope{newTr};
		auto& maps = newTr.Get<TrMap>();
		Msg("Loaded trace '%s' with %d ticks\n"
		    " - game: '%s' v%d\n"
		    " - mod name: '%s'\n"
		    " - player name: '%s'\n"
		    " - start map: '%s'\n",
		    lastAddedTrace->first.string().c_str(),
		    newTr.numRecordedTicks,
		    newTr.firstRecordedInfo.gameName.c_str(),
		    newTr.firstRecordedInfo.gameVersion,
		    newTr.firstRecordedInfo.gameModName.c_str(),
		    newTr.firstRecordedInfo.playerName.c_str(),
		    maps.empty() || !maps[0].nameIdx.IsValid() ? "INVALID" : *maps[0].nameIdx);

		if (!status.GetStatus().warnings.empty())
		{
			Warning("Warning(s):\n");
			for (const std::string& s : status.GetStatus().warnings)
				Warning("  - %s\n", s.c_str());
		}
	}
	else
	{
		char buf[32];
		if (nAlreadyImported > 0)
			snprintf(buf, sizeof buf, "(ignored %u already imported) ", nAlreadyImported);
		Msg("Imported %u trace(s) %swith path spec '%s'\n",
		    nNewImports,
		    nAlreadyImported > 0 ? buf : "",
		    pathSpec.string().c_str());
	}
}

namespace player_trace
{

	ConVar spt_draw_trace{"spt_draw_trace", "1", FCVAR_DONTRECORD, "Enable drawing traces."};

#ifdef SPT_HUD_ENABLED
	ConVar spt_hud_trace{"spt_hud_trace", "0", FCVAR_DONTRECORD, "Show info about the player trace."};
#endif

	ConVar spt_trace_autoplay("spt_trace_autoplay",
	                          "0",
	                          FCVAR_DONTRECORD,
	                          "Play the trace recording in real time.");
	ConVar spt_trace_ent_collect_radius("spt_trace_ent_collect_radius",
	                                    "250",
	                                    FCVAR_DONTRECORD,
	                                    "The radius around the player used for entity collection");
	ConVar spt_trace_draw_recording{"spt_trace_draw_recording",
	                                "1",
	                                FCVAR_DONTRECORD,
	                                "If enabled, draws the trace that is currently being recorded."};
	ConVar
	    spt_trace_draw_while_recording{"spt_trace_draw_while_recording",
	                                   "1",
	                                   FCVAR_DONTRECORD,
	                                   "If disabled, will not draw any traces while a trace is being recorded to."};
	ConVar spt_trace_draw_portal_collision_entities(
	    "spt_trace_draw_portal_collision_entities",
	    "0",
	    FCVAR_DONTRECORD,
	    "If enabled, draws all portalsimulator_collisionentity when drawing the trace.");
	ConVar spt_trace_draw_path_cones(
	    "spt_trace_draw_path_cones",
	    "1",
	    FCVAR_DONTRECORD,
	    "If enabled, draws cones along the player path to indicate the player travel direction.");
	ConVar spt_trace_draw_cam_style("spt_trace_draw_cam_style",
	                                "0",
	                                FCVAR_DONTRECORD,
	                                "Player trace camera type:\n"
	                                "  0 = camera frustum\n"
	                                "  1 = box and line\n");
	ConVar spt_trace_draw_contact_points("spt_trace_draw_contact_points",
	                                     "1",
	                                     FCVAR_DONTRECORD,
	                                     "If enabled, draws recorded contact points for the player.");
} // namespace player_trace

bool PlayerTraceFeature::ShouldLoadFeature()
{
	return TickSignal.Works && interfaces::engine_tool;
}

// TODO move this to the imgui file
// for tabs which draw detailed information about a single trace, draw a dropdown to select it and exit early if there are none
static void WrapSingleTraceInfoTab(SptImGuiGroup::Tab& imguiTab, tr_imgui::single_trace_info_tab_fn fn)
{
	imguiTab.RegisterUserCallback(
	    [fn]()
	    {
		    if (!tr_imgui::DrawDetailedTraceSelect())
			    return;
		    TrReadContextScope scope{TrTracePlayer::Singleton().detailedImGuiTrace->second.tr};
		    tr_tick tickToShow = spt_player_trace_feat.absDrawTick;
		    tr_imgui::SingleTraceInfoTabHeader(tickToShow);
		    fn(tickToShow);
	    });
}

void PlayerTraceFeature::LoadFeature()
{
	if (!spt_meshRenderer.signal.Works)
		return;

	TickSignal.Connect(this, &PlayerTraceFeature::OnTickSignal);
	FinishRestoreSignal.Connect(this, &PlayerTraceFeature::OnFinishRestoreSignal);
	spt_meshRenderer.signal.Connect(this, &PlayerTraceFeature::OnMeshRenderSignal);
	spt_portalled_pause_feat.portalTeleportedEntitySignal.Connect(this,
	                                                              &PlayerTraceFeature::OnPortalTeleportSignal);
	if (FcpsFinishedSignalWorks())
		fcpsFinishedSignal.Connect(this, &PlayerTraceFeature::OnFcpsSignal);

	InitCommand(spt_trace_start);
	InitCommand(spt_trace_start_auto_increment);
	InitCommand(spt_trace_stop);
	InitCommand(spt_trace_list);
	InitCommand(spt_trace_unload);
	InitCommand(spt_trace_unload_all);
	InitCommand(spt_trace_next_tick);
	InitCommand(spt_trace_prev_tick);
	InitCommand(spt_trace_set_tick);
	InitCommand(spt_trace_import);

#ifdef SPT_HUD_ENABLED
	if (AddHudCallback("trace", [](auto) { spt_player_trace_feat.OnHudCallback(); }, spt_hud_trace))
		SptImGui::RegisterHudCvarCheckbox(spt_hud_trace);
#endif

	InitConcommandBase(spt_draw_trace);
	InitConcommandBase(spt_trace_draw_recording);
	InitConcommandBase(spt_trace_draw_while_recording);
	InitConcommandBase(spt_trace_autoplay);
	InitConcommandBase(spt_trace_ent_collect_radius);
	if (utils::DoesGameLookLikePortal())
		InitConcommandBase(spt_trace_draw_portal_collision_entities);
	InitConcommandBase(spt_trace_draw_path_cones);
	InitConcommandBase(spt_trace_draw_cam_style);
	InitConcommandBase(spt_trace_draw_contact_points);

	WrapSingleTraceInfoTab(SptImGuiGroup::PlayerTrace_Player, tr_imgui::PlayerTabCallback);
	WrapSingleTraceInfoTab(SptImGuiGroup::PlayerTrace_Entities, tr_imgui::EntityTabCallback);
	if (utils::DoesGameLookLikePortal())
		WrapSingleTraceInfoTab(SptImGuiGroup::PlayerTrace_Portals, tr_imgui::PortalTabCallback);

	SptImGuiGroup::PlayerTrace_Select.RegisterUserCallback(tr_imgui::TraceFileSelectionTabCallback);
	SptImGui::RegisterWindowCallback(tr_imgui::TraceFileSelectionWindowCallback);

	SptImGuiGroup::PlayerTrace_DrawStyle.RegisterUserCallback(tr_imgui::RenderStyleTab);
}

void PlayerTraceFeature::UnloadFeature()
{
	TrTracePlayer::Singleton().ClearTraces(true);
}

// TODO don't allow scrolling past end of the trace
void PlayerTraceFeature::ChangeDisplayTick(int diff)
{
	if (diff < 0 && (tr_tick)-diff > absDrawTick)
		absDrawTick = 0;
	else
		absDrawTick += diff;
}

void PlayerTraceFeature::SetDisplayTick(tr_tick val)
{
	absDrawTick = val;
}

void PlayerTraceFeature::OnTickSignal(bool simulating)
{
	auto& tp = TrTracePlayer::Singleton();
	auto rt = tp.GetRecordingTrace();
	if (!!rt)
		rt->HostTickCollect(true, deferredSegmentReason, spt_trace_ent_collect_radius.GetFloat());
	deferredSegmentReason = TR_SR_NONE;
	if (spt_trace_autoplay.GetBool())
		ChangeDisplayTick(1);
}

void PlayerTraceFeature::OnFinishRestoreSignal(void*)
{
	deferredSegmentReason = TR_SR_SAVELOAD;
}

void PlayerTraceFeature::OnMeshRenderSignal(MeshRendererDelegate& mr)
{
	if (!spt_draw_trace.GetBool())
		return;

	auto& tp = TrTracePlayer::Singleton();
	auto& traces = tp.Traces();
	if (!spt_trace_draw_while_recording.GetBool() && tp.GetRecordingTraceHandle() != traces.end())
		return;

	// TODO - these cvars will only change the main style but non of the other ones

	auto& majorStyle = tp.mainStyleConfig;
	majorStyle.playerPath.cones.draw = spt_trace_draw_path_cones.GetBool();
	majorStyle.playerEye.style =
	    (TrPlayerCameraDrawType)std::clamp(spt_trace_draw_cam_style.GetInt(), 0, (int)TR_PCDT_COUNT);
	majorStyle.contactPoints.draw = spt_trace_draw_contact_points.GetBool();
	majorStyle.entPhys.portalCollisionEnts.draw = spt_trace_draw_portal_collision_entities.GetBool();

	TrTracePlayer::entry_handle rt = tp.GetRecordingTraceHandle();
	bool drawRecordingTrace = spt_trace_draw_recording.GetBool();
	drawRecordingTrace |= rt == traces.end(); // bit of a hack to make the logic below slightly easier

	// TODO display the count in imgui somehow
	static std::vector<TrTracePlayer::entry_handle> entriesToDraw;
	entriesToDraw.clear();

	if (tp.imguiHoveredTrace != traces.end())
	{
		// hovering over a trace in ImGui
		entriesToDraw.push_back(tp.imguiHoveredTrace);
	}
	else if (tp.soloTrace != traces.end())
	{
		// soloing a single trace in ImGui
		entriesToDraw.push_back(tp.soloTrace);
	}
	else if (tp.soloGroup != tp.Groups().end())
	{
		// soloing a single group
		for (auto& entry : tp.soloGroup->entries)
			if (entry->second.visible && (drawRecordingTrace || entry != rt))
				entriesToDraw.push_back(entry);
	}
	else
	{
		// TODO - is there really no nicer begin/end?

		// draw all visible traces
		for (auto entry = TrTracePlayer::entry_handle::Begin(traces);
		     entry != TrTracePlayer::entry_handle::End(traces);
		     ++entry)
		{
			if (entry->second.visible && entry->second.group->visible
			    && (drawRecordingTrace || entry == rt))
			{
				entriesToDraw.push_back(entry);
			}
		}
	}

	for (auto entry : entriesToDraw)
	{
		auto& trEntry = entry->second;
		TrReadContextScope scope{trEntry.tr};
		auto& renderCache = trEntry.tr.GetRenderingCache();
		auto& style = trEntry.group->GetCfg(tp.mainStyleConfig);

		if (entriesToDraw.size() == 1)
		{
			renderCache.RenderAll(mr, style, absDrawTick);
		}
		else
		{
			// TODO enable entity traces and such
			TrMultiTraceRenderContext mtrc{style};
			renderCache.RenderAll(mr, style, absDrawTick);
		}
	}

	tp.nDrawnTracesLastFrame = entriesToDraw.size();

	// imgui will set this
	tp.imguiHoveredTrace = TrTracePlayer::entry_handle::End(traces);
}

#ifdef SPT_HUD_ENABLED
void PlayerTraceFeature::OnHudCallback()
{
	// TODO

	/*ClampActiveTick();
	if (spt_draw_trace.GetBool())
	{
		spt_hud_feat.DrawTopHudElement(L"Trace draw tick: %u/%u",
		                               activeDrawTick,
		                               tr.numRecordedTicks == 0 ? 0 : tr.numRecordedTicks - 1);
	}
	else
	{
		spt_hud_feat.DrawTopHudElement(L"Recorded trace ticks: %d", tr.numRecordedTicks);
	}
	spt_hud_feat.DrawTopHudElement(L"Trace server tick: %d", tr.GetServerTickAtTick(activeDrawTick));

	float displayUsage = (float)tr.GetMemoryUsage();
	const wchar* suffixes[] = {L"B", L"KiB", L"MiB", L"GiB"};
	int i = 0;
	while (displayUsage > 1024 && i < ARRAYSIZE(suffixes) - 1)
	{
		displayUsage /= 1024.f;
		i++;
	}

	spt_hud_feat.DrawTopHudElement(L"Trace memory usage: %.*f%s", i > 0 ? 2 : 0, displayUsage, suffixes[i]);*/
}
#endif

void PlayerTraceFeature::OnPortalTeleportSignal(IServerEntity* portal, IServerEntity* ent)
{
	if (ent != utils::spt_serverEntList.GetPlayer())
		return;
	deferredSegmentReason = MAX(deferredSegmentReason, TR_SR_PLAYER_PORTALLED);
}

void PlayerTraceFeature::OnFcpsSignal(const FcpsEvent& event)
{
	if (event.params.entHandle.GetEntryIndex() != 1 || event.outcome.result != FCPS_SUCESS)
		return;
	deferredSegmentReason = MAX(deferredSegmentReason, TR_SR_FCPS);
}

bool player_trace::GetActiveTracePos(Vector& pos, QAngle& ang, float& fov)
{
	// TODO
	// TODO - also don't edit fov being 0 during loads

	/*auto& tr = spt_player_trace_feat.tr;
	TrReadContextScope scope{tr};
	auto plDataIdx = tr.GetAtTick<TrPlayerData>(spt_player_trace_feat.activeDrawTick);
	if (!plDataIdx.IsValid())
		return false;
	// TODO setting for seeing from sg eyes
	pos = **plDataIdx->transEyesIdx->posIdx;
	ang = **plDataIdx->transEyesIdx->angIdx;
	fov = plDataIdx->fov;
	return true;*/
	return true;
}

#endif
