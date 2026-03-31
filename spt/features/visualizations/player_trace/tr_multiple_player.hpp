#pragma once

#include "tr_structs.hpp"
#include "tr_render_cache.hpp"

#ifdef SPT_PLAYER_TRACE_ENABLED

#include "spt/utils/file.hpp"
#include "spt/utils/serialize.hpp"

#include "thirdparty/imgui/imgui.h"

#include <memory>
#include <map>
#include <filesystem>
#include <utility>
#include <list>

namespace player_trace
{
	/*
	* Assigns each trace to a file on disk by using std::filesystem::path as keys. To prevent
	* double loading a file, each path should be "as canonical as possible", which in practice
	* means std::filesystem::absolute is probably good enough?
	* 
	* For ImGui purposes, we also store color settings and visibility options for each trace and
	* allow the grouping of traces. To allow O(1) insertion/deletion of all of these, we store a LL
	* of groups each of which also has an LL of trace entries pointing back to the main map. On top
	* of that, we need to store which entries are selected in ImGui to let the user rearrange them.
	* All in all that results in 3 collections that need to be kept in sync, so I try to enforce
	* const-ness and use setters where invariants need to be maintained.
	*/
	class TrTracePlayer
	{
	public:
		inline static const char* COMPRESSED_FILE_EXT = ".sptr.xz";

		class TraceGroup;
		struct Entry;

		using trace_map = std::map<std::filesystem::path, Entry, utils::NaturalCompare>;
		using trace_it = trace_map::iterator;
		using trace_itc = trace_map::const_iterator;

		using trace_groups = std::list<TraceGroup>;
		using group_it = trace_groups::iterator;
		using group_itc = trace_groups::const_iterator;

		using group_entries = std::list<trace_it>;
		using group_entry_it = group_entries::iterator;
		using group_entry_itc = group_entries::const_iterator;

		// TODO readonly public?
		trace_groups traceGroups;
		group_it defaultGroupIt = traceGroups.end();

		struct Entry
		{
			TrPlayerTrace tr;
			mutable bool visible = true;
			mutable bool selectedInImgui = false;

			group_it groupIt;
			std::list<trace_it>::iterator entryInGroupIt;
		};

		class TraceGroup
		{
			std::optional<TrRenderStyleConfig> cfg; // if not set, re-init from main style with tint
			color32 tint;

		public:
			std::string name;
			// TODO all of these can be readonly public
			bool visible = true;
			bool isDefault = false;
			group_entries entries;

			TraceGroup(std::string name, color32 tint) : name(std::move(name)), tint(tint) {}

			color32 GetTint() const
			{
				return tint;
			}

			void SetTint(color32 newTint)
			{
				if (tint != newTint)
				{
					InvalidateCfg();
					tint = newTint;
				}
			}

			TrRenderStyleConfig& GetCfg(const TrRenderStyleConfig& majorCfg)
			{
				if (!cfg.has_value())
				{
					cfg = majorCfg;
					cfg->Multiply(tint);
				}
				return *cfg;
			}

			void InvalidateCfg()
			{
				cfg.reset();
			}
		};

		TrRenderStyleConfig mainStyleConfig;

	private:
		struct RecordingTrace;

		trace_map traces;
		// TODO highlight this in imgui
		std::unique_ptr<RecordingTrace> recordingTrace;

		size_t nCustomGroupsAdded = 0;

		TrTracePlayer();
		TrTracePlayer(TrTracePlayer&) = delete;
		TrTracePlayer(TrTracePlayer&&) = delete;

	public:
		// this is the trace that's selected for viewing detailed info in the UI (e.g. player pos/vel, entity info, etc.)
		trace_itc detailedImGuiTraceIt = traces.end();
		/*
		* These are all public read/write in order of priority:
		* - a solo trace/group is the only one that is shown
		* - the ImGui hovered trace is highlighted in the UI (TODO) and is basically a "super-solo" trace
		*/
		trace_itc imguiHoveredTraceIt = traces.end();
		trace_itc soloTraceIt = traces.end();
		group_itc soloGroupIt = traceGroups.end();

		// ImGui structure that allows for entry multi-selection
		ImGuiSelectionBasicStorage imguiEntrySelect;

		static TrTracePlayer& Singleton()
		{
			static TrTracePlayer tp;
			return tp;
		}

		const auto& AllTraces() const
		{
			return traces;
		}

		/*
		* Bit of a hardcoded setup for solo/visibility options for traces/groups. These are set
		* through here to try and enforce invariants. Assuming that:
		* - spt_trace_draw_recording is true
		* - spt_trace_draw_while_recording is true
		* - spt_draw_trace is true
		* - the user is not hovering over any traces
		* Then:
		* - the user should always be able to press all buttons
		* - a trace is not visible if it is marked as invisible or is in an invisible group
		* - a solo trace is always marked as visible and in a visible group
		* - a solo trace or group is never marked as invisible
		*/

		struct BoolOptInfo
		{
			bool enabled, allowUiChange;
		};

		BoolOptInfo GetGroupSoloOpt(group_itc it) const;
		void SetGroupSoloOpt(group_it it, bool val);
		BoolOptInfo GetGroupVisibleOpt(group_itc it) const;
		void SetGroupVisibleOpt(group_it it, bool val);

		BoolOptInfo GetTraceSoloOpt(trace_itc it) const;
		void SetTraceSoloOpt(trace_it it, bool val);
		BoolOptInfo GetTraceVisibleOpt(trace_itc it) const;
		void SetTraceVisibleOpt(trace_it it, bool val);

		group_itc AddGroup(color32 tint);
		group_entry_itc MoveTraceToGroup(group_it gIt, group_entry_it where, trace_it tIt);

		trace_it GetRecordingTrace();

		trace_itc TryStartRecording(std::filesystem::path path, ser::StatusTracker& stat);
		trace_itc StopAndFlushRecording(ser::StatusTracker& stat);

		// load trace from disk, return <iterator, isNew>
		std::pair<trace_itc, bool> TryLoadFromDisk(const std::filesystem::path& path, ser::StatusTracker& stat);

		void Remove(trace_itc it);
		void Remove(group_itc it);

		// returns number of deleted elements
		size_t ClearTraces(bool clearRecording);

		~TrTracePlayer();
	};
} // namespace player_trace

#endif
