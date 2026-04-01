#pragma once

#include "tr_structs.hpp"
#include "tr_render_cache.hpp"

#ifdef SPT_PLAYER_TRACE_ENABLED

#include "spt/utils/file.hpp"
#include "spt/utils/serialize.hpp"

#include "thirdparty/imgui/imgui.h"
#include "thirdparty/imgui/ImGuiFileDialog/ImGuiFileDialog.h"

#include <memory>
#include <map>
#include <filesystem>
#include <utility>
#include <list>

#define TR_COMPRESSED_FILE_EXT ".sptr.xz"
#define TR_DEFAULT_PATH "traces/default"

namespace player_trace
{

	template<typename container>
	class TrIteratorHandle
	{
		container::iterator it;

		// trace player has full access, users only get const access
		friend class TrTracePlayer;

	public:
		using value_type = typename container::value_type;

		TrIteratorHandle() = default;
		TrIteratorHandle(container::iterator it) : it(it) {}

		static TrIteratorHandle Begin(const container& c)
		{
			return const_cast<container&>(c).begin();
		}

		static TrIteratorHandle End(const container& c)
		{
			return const_cast<container&>(c).end();
		}

		const value_type& operator*() const
		{
			return *it;
		}

		const value_type* operator->() const
		{
			return &*it;
		}

		TrIteratorHandle& operator++()
		{
			++it;
			return *this;
		}

		TrIteratorHandle operator++(int)
		{
			return it++;
		}

		bool operator==(const TrIteratorHandle& other) const
		{
			return it == other.it;
		}

		bool operator==(container::const_iterator o) const
		{
			return it == o;
		}
	};

	/*
	* Assigns each trace to a file on disk by using std::filesystem::path as keys. To prevent
	* double loading a file, each path should be "as canonical as possible", which in practice
	* means std::filesystem::absolute is probably good enough?
	* 
	* For ImGui purposes, we also store color settings and visibility options for each trace and
	* allow the grouping of traces. To allow O(1) insertion/deletion of all of these, we store a LL
	* of groups each of which also has an LL of trace entries pointing back to the main map. On top
	* of that, we need to store which entries are selected in ImGui to let the user rearrange them.
	* 
	* All in all that results in 4 collections that need to be kept in sync. I also wish to keep
	* insertions/deletions O(1), so everything is stored in std::map/std::list; handles (which are
	* just iterators) are passed to the user which provide const access. The const access is to
	* prevent users from editing the underlying data structures directly, but some stuff (e.g.
	* colors & group names) is fine for users to edit directly.
	*/
	class TrTracePlayer
	{
	public:
		class Group;
		struct TraceEntry;

		using trace_map = std::map<std::filesystem::path, TraceEntry, utils::NaturalCompare>;
		using entry_handle = TrIteratorHandle<trace_map>;

		using trace_groups = std::list<Group>;
		using group_handle = TrIteratorHandle<trace_groups>;

		using group_entries = std::list<entry_handle>;
		using group_entry_handle = TrIteratorHandle<group_entries>;

		struct TraceEntry
		{
			TrPlayerTrace tr;
			bool visible = true;
			group_handle group;
			group_entry_handle groupPos;
		};

		class Group
		{
			mutable std::optional<TrRenderStyleConfig> cfg; // if not set, re-init from main style with tint
			mutable color32 tint;

		public:
			bool visible = true;
			mutable std::string name;
			group_entries entries;

			Group(std::string name, color32 tint) : name(std::move(name)), tint(tint) {}

			color32 GetTint() const
			{
				return tint;
			}

			void SetTint(color32 newTint) const
			{
				if (tint != newTint)
				{
					InvalidateCfg();
					tint = newTint;
				}
			}

			TrRenderStyleConfig& GetCfg(const TrRenderStyleConfig& mainCfg) const
			{
				if (!cfg.has_value())
				{
					cfg = mainCfg;
					cfg->Multiply(tint);
				}
				return *cfg;
			}

			void InvalidateCfg() const
			{
				cfg.reset();
			}
		};

		/*
		* The main style config is not passed directly to any traces. Instead, each group copies the main
		* style and applies its tint to it. Whenever anything is changed in this style, make sure to call
		* MarkGroupStylesDirty().
		*/
		TrRenderStyleConfig mainStyleConfig;

		void MarkGroupStylesDirty()
		{
			for (auto& group : groups)
				group.InvalidateCfg();
		}

		TrRenderEnableConfig singleTraceRenderEnableCfg;
		TrRenderEnableConfig multiTraceRenderEnableCfg;

		std::unique_ptr<ImGuiFileDialog> igfd;

	private:
		struct RecordingTrace;

		trace_map traces;
		trace_groups groups;
		group_handle defaultGroup = groups.end();
		// TODO highlight this in imgui
		std::unique_ptr<RecordingTrace> recordingTrace;

		size_t nCustomGroupsAdded = 0;

		TrTracePlayer();
		TrTracePlayer(TrTracePlayer&) = delete;
		TrTracePlayer(TrTracePlayer&&) = delete;

		void InitTraceGroup(entry_handle entry);

	public:
		// this is the trace that's selected for viewing detailed info in the UI (e.g. player pos/vel, entity info, etc.)
		entry_handle detailedImGuiTrace = traces.end();
		/*
		* These are all public read/write in order of priority:
		* - a solo trace/group is the only one that is shown
		* - the ImGui hovered trace is basically a "super-solo" trace
		*/
		entry_handle imguiHoveredTrace = traces.end();
		entry_handle soloTrace = traces.end();
		group_handle soloGroup = groups.end();

		size_t nDrawnTracesLastFrame = 0; // user global

		/*
		* ImGui structure that allows for entry multi-selection. Use the functions below to convert
		* to and from entry handles. Since I don't convert to handles/iterators directly, the
		* conversion is lossy, but a workaround is to get the group_entry_handle and from the entry.
		*/
		ImGuiSelectionBasicStorage imguiEntrySelect;

		ImGuiID HandleToMultiSelectId(entry_handle entry) const
		{
			return reinterpret_cast<ImGuiID>(&*entry);
		}

		trace_map::const_pointer MultiSelectIdToPtr(ImGuiID id) const
		{
			return reinterpret_cast<trace_map::const_pointer>(id);
		}

		static TrTracePlayer& Singleton()
		{
			static TrTracePlayer tp;
			return tp;
		}

		const auto& Traces() const
		{
			return traces;
		}

		const auto& Groups() const
		{
			return groups;
		}

		// this is the group that new traces are added to by default, and also the only group that can't be deleted
		group_handle GetDefaultGroup() const
		{
			return defaultGroup;
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

		BoolOptInfo GetGroupSoloOpt(group_handle group) const;
		void SetGroupSoloOpt(group_handle group, bool val);
		BoolOptInfo GetGroupVisibleOpt(group_handle group) const;
		void SetGroupVisibleOpt(group_handle group, bool val);

		BoolOptInfo GetTraceSoloOpt(entry_handle entry) const;
		void SetTraceSoloOpt(entry_handle entry, bool val);
		BoolOptInfo GetTraceVisibleOpt(entry_handle entry) const;
		void SetTraceVisibleOpt(entry_handle entry, bool val);

		TrPlayerTrace* GetRecordingTrace();
		entry_handle GetRecordingTraceHandle();

		entry_handle TryStartRecording(std::filesystem::path path, ser::StatusTracker& stat);
		entry_handle StopAndFlushRecording(ser::StatusTracker& stat);

		// load trace from disk, return <handle, isNew>
		std::pair<entry_handle, bool> TryLoadFromDisk(const std::filesystem::path& path,
		                                              ser::StatusTracker& stat);

		group_handle AddGroup(color32 tint);
		void MoveTraceToGroup(entry_handle entry, group_handle toGroup, group_entry_handle toPos);
		void ReorderGroup(group_handle from, group_handle to);

		void Remove(entry_handle entry);
		void Remove(group_handle group);

		// returns number of deleted elements
		size_t ClearTraces(bool clearRecording);

		~TrTracePlayer();
	};
} // namespace player_trace

#endif
