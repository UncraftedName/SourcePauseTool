#include "stdafx.hpp"

#include "tr_multiple_player.hpp"

#ifdef SPT_PLAYER_TRACE_ENABLED

#include "tr_entity_cache.hpp"
#include "tr_record_cache.hpp"
#include "tr_render_cache.hpp"

#ifdef min
#undef min
#undef max
#endif

#ifdef clamp
#undef clamp
#endif

#include <numeric>

using namespace player_trace;

struct TrTracePlayer::RecordingTrace
{
	trace_it it{}; // uninitialized
	ser::FileWriter fWr;
	ser::XzWriter xzWr;

	RecordingTrace(std::filesystem::path path) : fWr(std::move(path)), xzWr(fWr) {}
};

TrTracePlayer::TrTracePlayer()
{
	defaultGroupIt = groups.emplace(groups.end(), "default", color32(255, 255, 255, 255));
	defaultGroupIt->name = "default";
	defaultGroupIt->isDefault = true;

	imguiEntrySelect.AdapterIndexToStorageId = nullptr;
	imguiEntrySelect.UserData = nullptr;
	imguiEntrySelect.PreserveOrder = true; // possibly bugged
}

auto TrTracePlayer::GetRecordingTrace() -> trace_it
{
	return recordingTrace ? recordingTrace->it : traces.end();
}

auto TrTracePlayer::TryStartRecording(std::filesystem::path path, ser::StatusTracker& stat) -> trace_itc
{
	Assert(path.is_absolute());

	if (!stat.Ok())
		return traces.end();

	if (!!recordingTrace)
	{
		AssertMsg(0, "spt: trying to start a trace recording without stopping the previous one");
		// silently flush the previous recording trace if ignored
		ser::StatusTracker tmpStat;
		StopAndFlushRecording(tmpStat);
	}

	// attempt to create a new file
	std::unique_ptr<RecordingTrace> rt = std::make_unique<RecordingTrace>(path);
	if (!rt->xzWr.Ok())
	{
		stat.Concat(std::move(rt->xzWr));
		return traces.end();
	}

	bool isNew;
	std::tie(rt->it, isNew) = traces.try_emplace(std::move(path));
	TrTracePlayer::TraceEntry& entry = rt->it->second;
	if (isNew)
	{
		// new entries - add them to the default group
		// old entries - keep all settings the same
		MoveTraceToGroup(rt->it, defaultGroupIt, defaultGroupIt->entries.end());
	}

	recordingTrace = std::move(rt);
	entry.tr.StartRecording();
	return recordingTrace->it;
}

auto TrTracePlayer::StopAndFlushRecording(ser::StatusTracker& stat) -> trace_itc
{
	if (!recordingTrace)
	{
		stat.Warn("no recording trace");
		return traces.end();
	}

	TrTracePlayer::TraceEntry& entry = recordingTrace->it->second;
	entry.tr.StopRecording();
	entry.tr.Serialize(recordingTrace->xzWr);
	recordingTrace->xzWr.Finish();
	recordingTrace->fWr.Finish(); // usually not necessary, but do it in case the xz finish fails
	stat.Concat(std::move(recordingTrace->xzWr));

	auto ret = recordingTrace->it;
	recordingTrace.reset();
	return ret;
}

auto TrTracePlayer::TryLoadFromDisk(const std::filesystem::path& path, ser::StatusTracker& stat)
    -> std::pair<trace_itc, bool>
{
	Assert(path.is_absolute());

	std::pair<trace_it, bool> failRet(traces.end(), false);

	if (!stat.Ok())
		return failRet;

	// find where the key *would* go if it existed
	// TODO really check this key_comp thingy
	auto it = traces.lower_bound(path);
	if (it != traces.end() && traces.key_comp().Compare3Way(path, it->first) == 0)
		return {it, false}; // already loaded

	if (!!recordingTrace && traces.key_comp().Compare3Way(path, recordingTrace->it->first) == 0)
		return {it, false}; // this is the trace that's being recorded to

	// try to load from disk

	// TODO FileReader should use std::filesystem::path
	ser::FileReader fRd(path.string().c_str());
	ser::XzReader xzRd(fRd);
	TrPlayerTrace newTr;
	newTr.Deserialize(xzRd);
	stat.Concat(std::move(xzRd));

	if (!stat.Ok())
		return failRet;

	// only now do we add to the map
	it = traces.emplace_hint(it, path, std::move(newTr));
	MoveTraceToGroup(it, defaultGroupIt, defaultGroupIt->entries.end());

	return {it, true};
}

void TrTracePlayer::Remove(trace_itc it)
{
	if (it == traces.end())
		return;
	if (recordingTrace && recordingTrace->it == it)
	{
		AssertMsg(0, "spt: use StopAndFlushRecording instead");
		ser::StatusTracker stat;
		StopAndFlushRecording(stat); // flush silently
	}

	if (imguiHoveredTraceIt == it) [[unlikely]]
		imguiHoveredTraceIt = traces.end();
	if (soloTraceIt == it) [[unlikely]]
		soloTraceIt = traces.end();
	if (detailedImGuiTraceIt == it) [[unlikely]]
		detailedImGuiTraceIt = traces.end();

	// unlink from group
	it->second.groupIt->entries.erase(it->second.entryInGroupIt);
	// deselect in imgui
	imguiEntrySelect.SetItemSelected((ImGuiID)(&*it), false);

	traces.erase(it);
}

void TrTracePlayer::Remove(group_itc it)
{
	// never delete the default group
	Assert(it != defaultGroupIt);
	if (it == defaultGroupIt || it == groups.end())
		return;
	// move all entries to default group
	for (auto& entryIt : it->entries)
		entryIt->second.groupIt = defaultGroupIt;
	defaultGroupIt->entries.splice(defaultGroupIt->entries.end(), const_cast<group_entries&>(it->entries));
	// if the solo group is being deleted, then no solo group anymore
	if (soloGroupIt == it)
		soloGroupIt = groups.end();
	groups.erase(it);
}

size_t TrTracePlayer::ClearTraces(bool clearRecording)
{
	size_t oldSize = traces.size();

	// save recording trace state if applicable

	trace_map::node_type recordingNh;
	bool recordingTraceSelectedInImGui = false;
	bool recordingTraceSolo = false;

	if (recordingTrace)
	{
		if (clearRecording)
		{
			ser::StatusTracker stat;
			StopAndFlushRecording(stat);
		}
		else
		{
			auto recordIt = recordingTrace->it;
			recordingTraceSelectedInImGui = imguiEntrySelect.Contains((ImGuiID)(&*recordIt));
			recordingTraceSolo = recordIt == soloTraceIt;
			recordingNh = traces.extract(recordIt);
		}
	}

	// clear all traces

	imguiHoveredTraceIt = traces.end(); // no need to restore this
	soloTraceIt = traces.end();
	detailedImGuiTraceIt = traces.end(); // no need to restore this

	imguiEntrySelect.Clear();
	traces.clear();
	for (auto& group : groups)
		group.entries.clear();

	// re-insert recording trace if applicable

	auto insertRet = traces.insert(std::move(recordingNh));

	// restore recording trace state if applicable

	Assert(insertRet.inserted == !!recordingTrace);

	if (insertRet.inserted)
	{
		auto recordIt = recordingTrace->it = insertRet.position;
		if (recordingTraceSolo)
			soloTraceIt = recordIt;
		if (recordingTraceSelectedInImGui)
			imguiEntrySelect.SetItemSelected((ImGuiID)(&*recordIt), true);
		auto& groupEntries = recordIt->second.groupIt->entries;
		recordIt->second.entryInGroupIt = groupEntries.insert(groupEntries.end(), recordIt);
	}

	return oldSize - traces.size();
}

TrTracePlayer::~TrTracePlayer()
{
	ClearTraces(true);
}

auto TrTracePlayer::GetGroupSoloOpt(group_itc it) const -> BoolOptInfo
{
	return BoolOptInfo{.enabled = it == soloGroupIt, .allowUiChange = true};
}

void TrTracePlayer::SetGroupSoloOpt(group_it it, bool val)
{
	if (it == groups.end())
		return;
	Assert(GetGroupSoloOpt(it).allowUiChange);
	if (val)
	{
		soloGroupIt = it;
		it->visible = true;
		if (soloTraceIt != traces.end() && soloTraceIt->second.groupIt != it)
			soloTraceIt = traces.end();
	}
	else
	{
		soloGroupIt = groups.end();
	}
}

auto TrTracePlayer::GetGroupVisibleOpt(group_itc it) const -> BoolOptInfo
{
	Assert(it != groups.end());
	return BoolOptInfo{.enabled = it->visible, .allowUiChange = true};
}

void TrTracePlayer::SetGroupVisibleOpt(group_it it, bool val)
{
	if (it == groups.end())
		return;
	Assert(GetGroupVisibleOpt(it).allowUiChange);
	if (!val)
	{
		if (soloGroupIt == it)
			soloGroupIt = groups.end();
		if (soloTraceIt != traces.end() && soloTraceIt->second.groupIt == it)
			soloTraceIt = traces.end();
	}
	it->visible = val;
}

auto TrTracePlayer::GetTraceSoloOpt(trace_itc it) const -> BoolOptInfo
{
	// spt_trace_draw_while_recording will disable the UI higher up
	return BoolOptInfo{
	    .enabled = it == soloTraceIt,
	    .allowUiChange = !recordingTrace || it != recordingTrace->it || spt_trace_draw_recording.GetBool(),
	};
}

void TrTracePlayer::SetTraceSoloOpt(trace_it it, bool val)
{
	if (it == traces.end())
		return;
	Assert(GetTraceSoloOpt(it).allowUiChange);
	if (val)
	{
		it->second.visible = true;
		it->second.groupIt->visible = true;
		if (it->second.groupIt != soloGroupIt)
			soloGroupIt = groups.end();
		soloTraceIt = it;
	}
	else if (soloTraceIt == it)
	{
		soloTraceIt = traces.end();
	}
}

auto TrTracePlayer::GetTraceVisibleOpt(trace_itc it) const -> BoolOptInfo
{
	// spt_trace_draw_while_recording will disable the UI higher up
	Assert(it != traces.end());
	return BoolOptInfo{
	    .enabled = it->second.visible,
	    .allowUiChange = !recordingTrace || it != recordingTrace->it || spt_trace_draw_recording.GetBool(),
	};
}

void TrTracePlayer::SetTraceVisibleOpt(trace_it it, bool val)
{
	if (it == traces.end())
		return;
	Assert(GetTraceVisibleOpt(it).allowUiChange);
	if (!val && it == soloTraceIt)
		soloTraceIt = traces.end();
	it->second.visible = val;
}

auto TrTracePlayer::AddGroup(color32 tint) -> group_itc
{
	std::string name = "custom_" + std::to_string(++nCustomGroupsAdded);
	return groups.emplace(groups.end(), std::move(name), tint);
}

auto TrTracePlayer::MoveTraceToGroup(trace_it traceIt, group_it groupIt, group_entry_it entryIt) -> group_entry_itc
{
	Assert(traceIt != traces.end());
	Assert(toList != trace_groups.end());
	auto& entry = traceIt->second;
	auto& group = const_cast<Group&>(*groupIt);
	if (entry.groupIt != groups.end())
	{
		Assert(entry.entryInGroupIt != groups.end());
		group.entries.splice(entryIt, group.entries, entry.entryInGroupIt);
	}
	else
	{
		group.entries.insert(entryIt, traceIt);
	}
	entry.groupIt = groupIt;
	entry.entryInGroupIt = entryIt;
}

auto TrTracePlayer::ReorderGroup(group_itc from, group_itc to) -> group_it
{
	groups.splice(to, groups, from);
}

#endif
