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
	traces_it it{}; // uninitialized
	ser::FileWriter fWr;
	ser::XzWriter xzWr;

	RecordingTrace(std::filesystem::path path) : fWr(std::move(path)), xzWr(fWr) {}
};

void TrTracePlayer::AddNewEntryToDefaultGroup(traces_it it) const
{
	it->second.groupIt = defaultGroupIt;
	it->second.entryInGroupIt = defaultGroupIt->entries.emplace(defaultGroupIt->entries.end(), it);
}

TrTracePlayer::TrTracePlayer()
{
	defaultGroupIt = traceGroups.emplace(traceGroups.end(), "default", color32(255, 255, 255, 255));
	defaultGroupIt->name = "default";
	defaultGroupIt->isDefault = true;

	imguiEntrySelect.AdapterIndexToStorageId = nullptr;
	imguiEntrySelect.UserData = nullptr;
	imguiEntrySelect.PreserveOrder = true; // possibly bugged
}

auto TrTracePlayer::GetRecordingTrace() -> traces_it
{
	return recordingTrace ? recordingTrace->it : traces.end();
}

auto TrTracePlayer::TryStartRecording(std::filesystem::path path, ser::StatusTracker& stat) -> traces_itc
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
	TrTracePlayer::Entry& entry = rt->it->second;
	if (isNew)
	{
		// new entries - add them to the default group
		// old entries - keep all settings the same
		AddNewEntryToDefaultGroup(rt->it);
	}

	recordingTrace = std::move(rt);
	entry.tr.StartRecording();
	return recordingTrace->it;
}

auto TrTracePlayer::StopAndFlushRecording(ser::StatusTracker& stat) -> traces_itc
{
	if (!recordingTrace)
	{
		stat.Warn("no recording trace");
		return traces.end();
	}

	TrTracePlayer::Entry& entry = recordingTrace->it->second;
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
    -> std::pair<traces_itc, bool>
{
	Assert(path.is_absolute());

	std::pair<traces_itc, bool> failRet(traces.end(), false);

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
	AddNewEntryToDefaultGroup(it);

	return {it, true};
}

void TrTracePlayer::Remove(traces_itc it)
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
	for (auto& group : traceGroups)
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

#endif
