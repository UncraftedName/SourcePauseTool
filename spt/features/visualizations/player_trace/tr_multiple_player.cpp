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
public:
	entry_handle handle{};
	ser::FileWriter fWr;
	ser::XzWriter xzWr;

	// create the file first, and if that goes well then add to the map and populate handle
	RecordingTrace(std::filesystem::path path) : fWr(std::move(path)), xzWr(fWr) {}
};

TrTracePlayer::TrTracePlayer()
{
	defaultGroup = groups.emplace(groups.end(), "default", color32(255, 255, 255, 255));

	imguiEntrySelect.AdapterIndexToStorageId = nullptr;
	imguiEntrySelect.UserData = nullptr;
	imguiEntrySelect.PreserveOrder = true;
}

void TrTracePlayer::InitTraceGroup(entry_handle entry)
{
	Assert(entry != traces.end());
	// assume the entry is not in a group yet, put it in the default one
	entry.it->second.group = defaultGroup;
	entry.it->second.groupPos = defaultGroup.it->entries.insert(defaultGroup->entries.end(), entry);
}

auto TrTracePlayer::GetRecordingTraceHandle() -> entry_handle
{
	return recordingTrace ? recordingTrace->handle : entry_handle::End(traces);
}

TrPlayerTrace* TrTracePlayer::GetRecordingTrace()
{
	return recordingTrace ? &recordingTrace->handle.it->second.tr : nullptr;
}

auto TrTracePlayer::TryStartRecording(std::filesystem::path path, ser::StatusTracker& stat) -> entry_handle
{
	Assert(path.is_absolute());

	if (!stat.Ok())
		return {};

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
		return {};
	}

	bool isNew;
	std::tie(rt->handle, isNew) = traces.try_emplace(std::move(path));
	if (isNew)
	{
		// new entries - add them to the default group
		// old entries - keep all settings the same
		InitTraceGroup(rt->handle);
	}

	recordingTrace = std::move(rt);
	recordingTrace->handle.it->second.tr.StartRecording();
	return recordingTrace->handle;
}

auto TrTracePlayer::StopAndFlushRecording(ser::StatusTracker& stat) -> entry_handle
{
	if (!recordingTrace)
	{
		stat.Warn("no recording trace");
		return {};
	}

	TrTracePlayer::TraceEntry& entry = recordingTrace->handle.it->second;
	entry.tr.StopRecording();
	entry.tr.Serialize(recordingTrace->xzWr);
	recordingTrace->xzWr.Finish();
	recordingTrace->fWr.Finish(); // usually not necessary, but do it in case the xz finish fails
	stat.Concat(std::move(recordingTrace->xzWr));

	auto ret = recordingTrace->handle;
	recordingTrace.reset();
	return ret;
}

auto TrTracePlayer::TryLoadFromDisk(const std::filesystem::path& path, ser::StatusTracker& stat)
    -> std::pair<entry_handle, bool>
{
	Assert(path.is_absolute());

	if (!stat.Ok())
		return {};

	// find where the key *would* go if it existed
	// TODO really check this key_comp thingy
	auto it = traces.lower_bound(path);
	if (it != traces.end() && traces.key_comp().Compare3Way(path, it->first) == 0)
		return {it, false}; // already loaded

	if (!!recordingTrace && traces.key_comp().Compare3Way(path, recordingTrace->handle->first) == 0)
		return {recordingTrace->handle, false}; // this is the trace that's being recorded to

	// try to load from disk

	// TODO FileReader should use std::filesystem::path
	ser::FileReader fRd(path.string().c_str());
	ser::XzReader xzRd(fRd);
	TrPlayerTrace newTr;
	newTr.Deserialize(xzRd);
	stat.Concat(std::move(xzRd));

	if (!stat.Ok())
		return {};

	// only now do we add to the map
	it = traces.emplace_hint(it, path, std::move(newTr));
	InitTraceGroup(it);

	return {it, true};
}

void TrTracePlayer::Remove(entry_handle entry)
{
	if (entry == traces.end())
		return;

	if (recordingTrace && recordingTrace->handle == entry)
	{
		AssertMsg(0, "spt: use StopAndFlushRecording instead");
		ser::StatusTracker stat;
		StopAndFlushRecording(stat); // flush silently
	}

	if (imguiHoveredTrace == entry) [[unlikely]]
		imguiHoveredTrace = traces.end();
	if (soloTrace == entry) [[unlikely]]
		soloTrace = traces.end();
	if (detailedImGuiTrace == entry) [[unlikely]]
		detailedImGuiTrace = traces.end();

	// unlink from group
	entry->second.group.it->entries.erase(entry->second.groupPos.it);
	// deselect in imgui
	imguiEntrySelect.SetItemSelected(HandleToMultiSelectId(entry), false);

	traces.erase(entry.it);
}

void TrTracePlayer::Remove(group_handle group)
{
	// never delete the default group
	Assert(group != defaultGroup);
	if (group == defaultGroup || group == groups.end())
		return;
	// move all entries to default group
	for (auto& entryHandle : group->entries)
		entryHandle.it->second.group = defaultGroup;
	defaultGroup.it->entries.splice(defaultGroup->entries.end(), group.it->entries);
	// if the solo group is being deleted, then no solo group anymore
	if (soloGroup == group)
		soloGroup = groups.end();
	groups.erase(group.it);
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
			auto rtHandle = recordingTrace->handle;
			recordingTraceSelectedInImGui = imguiEntrySelect.Contains(HandleToMultiSelectId(rtHandle));
			recordingTraceSolo = rtHandle == soloTrace;
			recordingNh = traces.extract(rtHandle.it);
		}
	}

	// clear all traces

	imguiHoveredTrace = traces.end(); // no need to restore this
	soloTrace = traces.end();
	detailedImGuiTrace = traces.end(); // no need to restore this

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
		auto rtHandle = recordingTrace->handle = insertRet.position;
		if (recordingTraceSolo)
			soloTrace = rtHandle;
		if (recordingTraceSelectedInImGui)
			imguiEntrySelect.SetItemSelected(HandleToMultiSelectId(rtHandle), true);
		auto& groupEntries = rtHandle->second.group.it->entries;
		rtHandle.it->second.groupPos = groupEntries.insert(groupEntries.end(), rtHandle.it);
	}

	return oldSize - traces.size();
}

TrTracePlayer::~TrTracePlayer()
{
	ClearTraces(true);
}

auto TrTracePlayer::GetGroupSoloOpt(group_handle group) const -> BoolOptInfo
{
	return BoolOptInfo{.enabled = group == soloGroup, .allowUiChange = true};
}

void TrTracePlayer::SetGroupSoloOpt(group_handle group, bool val)
{
	if (group == groups.end())
		return;
	Assert(GetGroupSoloOpt(group).allowUiChange);
	if (val)
	{
		soloGroup = group;
		group.it->visible = true;
		if (soloTrace != traces.end() && soloTrace->second.group != group)
			soloTrace = traces.end();
	}
	else
	{
		soloGroup = groups.end();
	}
}

auto TrTracePlayer::GetGroupVisibleOpt(group_handle group) const -> BoolOptInfo
{
	Assert(group != groups.end());
	return BoolOptInfo{.enabled = group->visible, .allowUiChange = true};
}

void TrTracePlayer::SetGroupVisibleOpt(group_handle group, bool val)
{
	if (group == groups.end())
		return;
	Assert(GetGroupVisibleOpt(group).allowUiChange);
	if (!val)
	{
		if (soloGroup == group)
			soloGroup = groups.end();
		if (soloTrace != traces.end() && soloTrace->second.group == group)
			soloTrace = traces.end();
	}
	group.it->visible = val;
}

auto TrTracePlayer::GetTraceSoloOpt(entry_handle entry) const -> BoolOptInfo
{
	// spt_trace_draw_while_recording will disable the UI higher up
	return BoolOptInfo{
	    .enabled = entry == soloTrace,
	    .allowUiChange = !recordingTrace || entry != recordingTrace->handle || spt_trace_draw_recording.GetBool(),
	};
}

void TrTracePlayer::SetTraceSoloOpt(entry_handle entry, bool val)
{
	if (entry == traces.end())
		return;
	Assert(GetTraceSoloOpt(entry).allowUiChange);
	if (val)
	{
		entry.it->second.visible = true;
		entry->second.group.it->visible = true;
		if (entry->second.group != soloGroup)
			soloGroup = groups.end();
		soloTrace = entry;
	}
	else if (soloTrace == entry)
	{
		soloTrace = traces.end();
	}
}

auto TrTracePlayer::GetTraceVisibleOpt(entry_handle entry) const -> BoolOptInfo
{
	// spt_trace_draw_while_recording will disable the UI higher up
	Assert(entry != traces.end());
	return BoolOptInfo{
	    .enabled = entry->second.visible,
	    .allowUiChange = !recordingTrace || entry != recordingTrace->handle || spt_trace_draw_recording.GetBool(),
	};
}

void TrTracePlayer::SetTraceVisibleOpt(entry_handle entry, bool val)
{
	if (entry == traces.end())
		return;
	Assert(GetTraceVisibleOpt(entry).allowUiChange);
	if (!val && entry == soloTrace)
		soloTrace = traces.end();
	entry.it->second.visible = val;
}

auto TrTracePlayer::AddGroup(color32 tint) -> group_handle
{
	std::string name = "custom_" + std::to_string(++nCustomGroupsAdded);
	return groups.emplace(groups.end(), std::move(name), tint);
}

void TrTracePlayer::MoveTraceToGroup(entry_handle entry, group_handle toGroup, group_entry_handle toPos)
{
	auto& trEntry = entry.it->second;
	trEntry.group.it->entries.erase(trEntry.groupPos.it);
	trEntry.groupPos = toGroup.it->entries.insert(toPos.it, entry.it);
	trEntry.group = toGroup;
}

void TrTracePlayer::ReorderGroup(group_handle from, group_handle to)
{
	groups.splice(to.it, groups, from.it);
}

#endif
