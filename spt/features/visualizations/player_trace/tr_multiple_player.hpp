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
	* This class is also responsible for keeping track of trace-specific rendering info for
	* multiple traces.
	*/
	class TrTracePlayer
	{
	public:
		inline static const char* COMPRESSED_FILE_EXT = ".sptr.xz";

		class TraceGroup;
		struct Entry;

		using trace_map = std::map<std::filesystem::path, Entry, utils::NaturalCompare>;
		using traces_it = trace_map::iterator;
		using traces_itc = trace_map::const_iterator;

		using trace_groups = std::list<TraceGroup>;
		using group_it = trace_groups::iterator;

		using group_entries = std::list<traces_it>;

		// TODO readonly public?
		trace_groups traceGroups;
		group_it defaultGroupIt = traceGroups.end();

		struct Entry
		{
			TrPlayerTrace tr;
			mutable bool visible = true;
			mutable bool selectedInImgui = false;

			group_it groupIt;
			std::list<traces_it>::iterator entryInGroupIt;
		};

		class TraceGroup
		{
			std::optional<TrRenderStyleConfig> cfg; // if not set, re-init from main style with tint
			color32 tint;

		public:
			std::string name;
			bool visible = true;
			bool isDefault = false;
			group_entries entries; // TODO readonly public?

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
		struct RecordingTrace
		{
			traces_it it{}; // uninitialized
			// TODO just make these pointers and make pimpl
			ser::FileWriter fWr;
			ser::XzWriter xzWr;

			RecordingTrace(std::filesystem::path path) : fWr(std::move(path)), xzWr(fWr) {}
		};

		trace_map traces;
		// TODO highlight this in imgui
		std::unique_ptr<RecordingTrace> recordingTrace;

		tr_tick maxAbsTick = 0; // TODO

		size_t nCustomGroupsAdded = 0;

		void AddNewEntryToDefaultGroup(traces_it it) const;

		TrTracePlayer();
		TrTracePlayer(TrTracePlayer&) = delete;
		TrTracePlayer(TrTracePlayer&&) = delete;

	public:
		traces_itc detailedImGuiTraceIt = traces.end();
		// these are all public read/write listed in order of priority
		traces_itc imguiHoveredTraceIt = traces.end();
		traces_itc soloTraceIt = traces.end();
		group_it soloGroupIt = traceGroups.end();

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

		tr_tick GetMaxAbsTick() const
		{
			return maxAbsTick;
		}

		group_it AddCustomGroup(color32 tint)
		{
			std::string name = "custom_" + std::to_string(++nCustomGroupsAdded);
			return traceGroups.emplace(traceGroups.end(), std::move(name), tint);
		}

		void DeleteGroup(group_it it)
		{
			Assert(it != defaultGroupIt);
			if (it == defaultGroupIt || it == traceGroups.end())
				return;
			for (auto& entryIt : it->entries)
				entryIt->second.groupIt = defaultGroupIt;
			defaultGroupIt->entries.splice(defaultGroupIt->entries.end(), it->entries);
			if (soloGroupIt == it)
				soloGroupIt = traceGroups.end();
			traceGroups.erase(it);
		}

		traces_it GetRecordingTrace();

		traces_itc TryStartRecording(std::filesystem::path path, ser::StatusTracker& stat);
		traces_itc StopAndFlushRecording(ser::StatusTracker& stat);

		// load trace from disk, return <iterator, isNew>
		std::pair<traces_itc, bool> TryLoadFromDisk(const std::filesystem::path& path,
		                                            ser::StatusTracker& stat);

		void Remove(traces_itc it);

		// returns number of deleted elements
		size_t Clear(bool clearRecording);

		~TrTracePlayer()
		{
			Clear(true);
		}
	};
} // namespace player_trace

#endif
