#include "stdafx.hpp"

#include "ar_decls.hpp"
#include "ar_util.hpp"
#include "spt/feature.hpp"
#include "spt/features/visualizations/imgui/imgui_interface.hpp"
#include "spt/utils/interfaces.hpp"

#include "thirdparty/imgui/imgui_stdlib.h"

#include <variant>
#include <chrono>
#include <mutex>

#include <Rpc.h>
#pragma comment(lib, "Rpcrt4")

enum ArSyncMode
{
	AR_SYNC_FULL,
	AR_SYNC_ASYNC,
	AR_SYNC_THREADED,
};

struct ArDeferredMovieJob
{
	ArFfmpegWriter::InitArgs args;
	ar_frame_idx maxNFrames;
	ArSyncMode syncMode;
	size_t nFramesInFlight; // only used if asyncMode != AR_SYNC_FULL
};

struct ArRunningJob
{
	using clock = std::chrono::high_resolution_clock;

	clock::time_point startTime = clock::now();
	ar_frame_idx maxConsumeFrames;
	std::unique_ptr<ArSyncManager> mgr;
	// ImGui may request a kill -> kill actually happens in present hook
	std::atomic_flag killSignal = ATOMIC_FLAG_INIT;

	auto GetElapsedTime() const
	{
		return clock::now() - startTime;
	}

	void PrintTimeStats() const
	{
		auto curTime = clock::now();
		auto duration = std::chrono::duration_cast<std::chrono::microseconds>(curTime - startTime).count();

		size_t nConsumedFrames = mgr->GetNumConsumedFrames();
		Msg("SPT: finished job, consumed %u frames in %.3fs (avg %.3fms/frame)\n\n",
		    nConsumedFrames,
		    duration / 1e6f,
		    duration / 1e3f / nConsumedFrames);
	}
};

namespace patterns
{
	PATTERNS(S_TransferStereo16,
	         "portal1-5135",
	         "53 55 56 57 E8 ?? ?? ?? ?? D8 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ??");
}

struct portable_samplepair_t
{
	int left;
	int right;
};

// placeholders

class AutoRenderFeature : public FeatureWrapper<AutoRenderFeature>
{
public:
	static constexpr float DEFAULT_RENDER_FRAMERATE = 60.f;

	static inline std::atomic<bool> imGuiCallbackActive = false;
	static inline std::atomic<bool> cmdLineFormattedDirty;

	static inline std::mutex jobMtx;
	std::unique_ptr<ArRunningJob> movieJob;
	std::optional<DWORD> appReturnCode;

	// all strings here are stored as utf8 to display in ImGui
	struct ImGuiPersist
	{
		bool lastFfmpegSearchSuccess = false;
		std::string cmdLineUnformatted;

		struct
		{
			std::vector<std::string> unrecognizedPlaceholders;
			std::string text;
		} cmdLineFormatResult;
	} persist;

protected:
	virtual void InitHooks() override;
	virtual void LoadFeature() override;
	virtual void UnloadFeature() override;

private:
	void OnShaderDevicePresentSignal(IDirect3DDevice9* device);
	void ImGuiTabCallback();

	DECL_STATIC_HOOK_CDECL(void,
	                       S_TransferStereo16,
	                       void* pOutput,
	                       const portable_samplepair_t* pfront,
	                       int lpaintedtime,
	                       int endtime);

} static spt_auto_render_feat;

class ArPlaceholder
{
	inline static std::vector<std::reference_wrapper<const ArPlaceholder>> allPlaceHolders;

	explicit ArPlaceholder(const char* key, const char* helpText) : key(key), helpText(helpText)
	{
		allPlaceHolders.push_back(std::ref(*this));
	}

	ArPlaceholder(ArPlaceholder&) = delete;
	ArPlaceholder(ArPlaceholder&&) = delete;

	friend class ArPlaceholders;

public:
	static const auto& GetAll()
	{
		return allPlaceHolders;
	}

	const char* key;
	const char* helpText;
	std::atomic<std::shared_ptr<std::string>> val;

	static std::string UnformattedStr(const std::string& s)
	{
		return std::format("{}{}{}", '{', s, '}');
	}

	std::string UnformattedKey() const
	{
		return UnformattedStr(key);
	}

	// if changed, sets the dirty flag for the formatted string
	void SetValue(std::string newVal)
	{
		auto newValPtr = std::make_shared<std::string>(std::move(newVal));
		auto oldVal = val.exchange(newValPtr);
		if (!oldVal || *oldVal != *newValPtr)
			spt_auto_render_feat.cmdLineFormattedDirty.store(true);
	}

	// return value is const - you must set the value through SetVal to update the dirty flag
	std::shared_ptr<const std::string> GetValue() const
	{
		auto ret = val.load();
		if (ret)
			return ret;
		return ret ? ret : std::make_shared<std::string>("NONE");
	}
};

// default placeholder values are set in LoadFeature
class ArPlaceholders
{
public:
#define AR_DEFINE_PLACEHOLDER(name, helpText) \
	inline static ArPlaceholder name \
	{ \
		#name, helpText \
	}

	AR_DEFINE_PLACEHOLDER(EXE_PATH, "Path to the ffmpeg executable (or an executable of your choice)");
	AR_DEFINE_PLACEHOLDER(VID_WIDTH,
	                      "Input video width in pixels (set to whatever the game is currently running at)");
	AR_DEFINE_PLACEHOLDER(VID_HEIGHT,
	                      "Input video height in pixels (set to whatever the game is currently running at)");
	AR_DEFINE_PLACEHOLDER(FRAMERATE, "Input framerate");
	AR_DEFINE_PLACEHOLDER(VIDEO_PIPE_NAME, "Name of the video pipe that the raw frames will be fed to");
	AR_DEFINE_PLACEHOLDER(AUDIO_PIPE_NAME, "Name of the audio pipe that the stereo PCM will be fed to");
	AR_DEFINE_PLACEHOLDER(RENDER_WORKING_DIR, "Working directory of the rendering application");
	AR_DEFINE_PLACEHOLDER(GAME_WORKING_DIR, "Working directory of the game");
	AR_DEFINE_PLACEHOLDER(MOD_DIR, "Mod directory");
	AR_DEFINE_PLACEHOLDER(UUID, "Unique UUID for each video");
	// TODO these options require special handling and need to be conditionally enabled
	/*AR_DEFINE_PLACEHOLDER(MAP_NAME, "The name of the loaded map on the first frame of recording");
	AR_DEFINE_PLACEHOLDER(MAP_SEQ, "The map index start at 0 on the first frame of recording");
	AR_DEFINE_PLACEHOLDER(DEMO_SEQ, "The demo index starting at 0 (only applicable if rendering demos)");
	AR_DEFINE_PLACEHOLDER(DEMO_FILENAME, "The demo file name (only applicable if rendering demos)");*/

#undef AR_DEFINE_PLACEHOLDER

	static bool FindFFmpeg();
	static void ResetUnformattedCmdLine();
	static void FormatCmdLine();
	static void RegenerateUuid();
	static void SetPipeNames(bool appendUuid);
};

void AutoRenderFeature::InitHooks()
{
	HOOK_FUNCTION(engine, S_TransferStereo16);
}

void AutoRenderFeature::LoadFeature()
{
	if (!ShaderDevicePresentSignal.Works || !SptImGui::Loaded())
		return;

	// fill in default placeholders

	// TODO test with portal in a non-ascii folder
	std::filesystem::path modDir = interfaces::_engine_client->GetGameDirectory();
	modDir = std::filesystem::canonical(modDir);
	ArPlaceholders::MOD_DIR.SetValue(modDir.string());
	std::error_code ec;
	std::filesystem::path workingDir = std::filesystem::current_path(ec);
	ArPlaceholders::GAME_WORKING_DIR.SetValue((ec ? modDir : workingDir).string());
	ArPlaceholders::RENDER_WORKING_DIR.SetValue((workingDir / "spt_autorender").string());
	ArPlaceholders::EXE_PATH.SetValue("C:\\YOUR_MOM\\ffmpeg.exe");
	ArPlaceholders::FRAMERATE.SetValue(std::to_string(DEFAULT_RENDER_FRAMERATE));

	ArPlaceholders::FindFFmpeg();
	ArPlaceholders::ResetUnformattedCmdLine();
	ArPlaceholders::RegenerateUuid();
	ArPlaceholders::SetPipeNames(true);

	ShaderDevicePresentSignal.Connect(this, &AutoRenderFeature::OnShaderDevicePresentSignal);
	/*InitCommand(spt_ar_screenshot);
	InitCommand(spt_ar_profile);
	InitCommand(spt_ar_stop_all);
	InitCommand(spt_ar_startmovie);
	InitCommand(spt_ar_startmovie_ffmpeg);*/

	SptImGuiGroup::QoL_AutoRender.RegisterUserCallback([this]() { ImGuiTabCallback(); });
}

void AutoRenderFeature::UnloadFeature()
{
	jobMtx.lock();
	movieJob.reset();
	jobMtx.unlock();
	AutoRenderFeature::ImGuiPersist newPersist{};
	std::swap(newPersist, persist);
}

void AutoRenderFeature::OnShaderDevicePresentSignal(IDirect3DDevice9* device)
{
	// technically I don't think we should be calling Msg/Warning from the render thread :/

	if (imGuiCallbackActive.exchange(false))
	{
		// update back buffer placeholder
		ser::StatusTracker stat;
		auto [_, desc] = ArGetBackBufferInfo(device, stat);
		if (!stat.Ok())
		{
			Warning("[%s]: %s\n", __FUNCTION__, stat.GetStatus().errMsg.c_str());
			return;
		}
		ArPlaceholders::VID_WIDTH.SetValue(std::to_string(desc.Width));
		ArPlaceholders::VID_HEIGHT.SetValue(std::to_string(desc.Height));
	}

	std::lock_guard lock(jobMtx);
}

void AutoRenderFeature::ImGuiTabCallback()
{
	imGuiCallbackActive = true;

	// start/stop controls

	if (ImGui::Button("Start rendering"))
	{
		// TODO
	}

	ImGui::SameLine();

	if (ImGui::Button("Stop rendering"))
	{
		// TODO
	}

	ImGui::SameLine();

	if (ImGui::Button("Open log folder"))
	{
		auto workingDirStr = ArPlaceholders::RENDER_WORKING_DIR.GetValue();
		DWORD wlen = MultiByteToWideChar(CP_UTF8, 0, workingDirStr->c_str(), workingDirStr->size(), nullptr, 0);
		std::unique_ptr<wchar[]> wWorkingDirStr = std::make_unique_for_overwrite<wchar[]>(wlen);
		MultiByteToWideChar(CP_UTF8,
		                    0,
		                    workingDirStr->c_str(),
		                    workingDirStr->size(),
		                    wWorkingDirStr.get(),
		                    wlen);

		std::filesystem::path workingDir = wWorkingDirStr.get();
		std::error_code ec;
		std::filesystem::create_directories(workingDir, ec);
		ShellExecuteW(NULL, L"open", wWorkingDirStr.get(), NULL, NULL, SW_SHOWDEFAULT);
	}

	// ffmpeg input

	{
		static std::string tmp;
		tmp = *ArPlaceholders::EXE_PATH.GetValue();
		if (ImGui::InputText("Exe path", &tmp))
		{
			cmdLineFormattedDirty.store(true);
			ArPlaceholders::EXE_PATH.SetValue(tmp);
		}
	}
	ImGui::SameLine();
	if (ImGui::Button("Auto-detect"))
		ArPlaceholders::FindFFmpeg();
	ImGui::SetItemTooltip("search for ffmpeg in the PATH");
	ImGui::SameLine();
	SptImGui::HelpMarker(
	    "This is the application that all data will be pumped into.\n"
	    "By default it's ffmpeg, but you can in theory use any application\n"
	    "that accepts named pipes as input for raw video and audio streams.");

	if (!persist.lastFfmpegSearchSuccess)
	{
		ImGui::TextColored(SPT_IMGUI_WARN_COLOR_YELLOW, "Warning: ffmpeg.exe not found,");
		ImGui::SameLine();
		ImGui::TextLinkOpenURL("download it", "https://www.gyan.dev/ffmpeg/builds/ffmpeg-git-full.7z");
		ImGui::SameLine();
		ImGui::TextColored(SPT_IMGUI_WARN_COLOR_YELLOW, "and add it to your PATH.");
	}

	// pipe name option(s)

	static int pipeNameOpt = 0;
	const char* pipeNameOpts[] = {"Append UUID (default)", "Keep name consistent"};
	if (ImGui::Combo("Pipe name", &pipeNameOpt, pipeNameOpts, ARRAYSIZE(pipeNameOpts)))
		ArPlaceholders::SetPipeNames(pipeNameOpt == 0);
	ImGui::SameLine();
	SptImGui::HelpMarker(
	    "Advanced users only!\n"
	    "By default, a random name is used for the video/audio pipes for every video.\n"
	    "This allows rendering with multiple instances and minimizes bugs if this code fails to close ffmpeg correctly.\n"
	    "This option is mostly for debugging.");

	// framerate

	static float fpsVal = DEFAULT_RENDER_FRAMERATE;
	if (ImGui::InputFloat("Framerate", &fpsVal))
	{
		fpsVal = clamp(fpsVal, 0.001f, 100'000);
		ArPlaceholders::FRAMERATE.SetValue(std::to_string(fpsVal));
	}

	// cmdline TODO: wrap

	ImGui::TextUnformatted("Program to execute:");
	if (ImGui::InputTextMultiline("##cmdline_unformatted",
	                              &persist.cmdLineUnformatted,
	                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 16)))
	{
		cmdLineFormattedDirty.store(true);
	}

	if (ImGui::Button("Reset to default"))
	{
		ArPlaceholders::ResetUnformattedCmdLine();
		cmdLineFormattedDirty.store(true);
	}

	// placeholder info

	if (!persist.cmdLineFormatResult.unrecognizedPlaceholders.empty())
	{
		std::string out = "Warning: unrecognized placeholders: ";
		for (auto& s : persist.cmdLineFormatResult.unrecognizedPlaceholders)
		{
			out += ArPlaceholder::UnformattedStr(s);
			out += ", ";
		}
		ImGui::TextColored(SPT_IMGUI_WARN_COLOR_YELLOW, "%.*s", out.size() - 2, out.c_str());
	}

	if (ImGui::TreeNode("Available placeholders"))
	{
		if (ImGui::BeginTable("##placeholders",
		                      3,
		                      ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX
		                          | ImGuiTableFlags_BordersInner | ImGuiTableFlags_BordersOuter))
		{
			ImGui::TableSetupColumn("Key");
			ImGui::TableSetupColumn("Value");
			ImGui::TableSetupColumn("Description");
			ImGui::TableHeadersRow();
			for (const ArPlaceholder& pl : ArPlaceholder::GetAll())
			{
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(pl.UnformattedKey().c_str());
				ImGui::TableNextColumn();
				auto val = pl.GetValue();
				ImGui::TextUnformatted(val ? val->c_str() : "-");
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(pl.helpText);
			}
			ImGui::EndTable();
		}

		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Formatted command"))
	{
		if (cmdLineFormattedDirty.exchange(false))
			ArPlaceholders::FormatCmdLine();

		ImGui::InputTextMultiline("##cmdline_formated",
		                          persist.cmdLineFormatResult.text.data(),
		                          persist.cmdLineFormatResult.text.size(),
		                          ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 16),
		                          ImGuiInputTextFlags_ReadOnly);
		ImGui::TreePop();
	}
}

bool ArPlaceholders::FindFFmpeg()
{
	wchar wExePath[MAX_PATH];
	DWORD wlen = SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, wExePath, nullptr);
	if (wlen >= MAX_PATH || wlen == 0)
		return false;
	char exePath[MAX_PATH];
	BOOL usedDefault;
	DWORD len = WideCharToMultiByte(CP_UTF8,
	                                WC_ERR_INVALID_CHARS,
	                                wExePath,
	                                wlen,
	                                exePath,
	                                MAX_PATH,
	                                nullptr,
	                                &usedDefault);
	if (len == 0 || len >= MAX_PATH || usedDefault)
		return false;

	EXE_PATH.SetValue(std::string(exePath, len));
	return true;
}

void ArPlaceholders::ResetUnformattedCmdLine()
{
	spt_auto_render_feat.persist.cmdLineUnformatted = std::format(
	    "\"{}\" -report -f rawvideo -pixel_format bgr0 -video_size {}x{} -framerate {} -i \"{}\" -y -c:v libx264 \"{}\\my_video.mp4\"",
	    ArPlaceholders::EXE_PATH.UnformattedKey(),
	    ArPlaceholders::VID_WIDTH.UnformattedKey(),
	    ArPlaceholders::VID_HEIGHT.UnformattedKey(),
	    ArPlaceholders::FRAMERATE.UnformattedKey(),
	    ArPlaceholders::VIDEO_PIPE_NAME.UnformattedKey(),
	    ArPlaceholders::RENDER_WORKING_DIR.UnformattedKey());
}

void ArPlaceholders::FormatCmdLine()
{
	auto& out = spt_auto_render_feat.persist.cmdLineFormatResult;
	out.text.clear();
	out.unrecognizedPlaceholders.clear();

	bool inPlaceholder = false;
	std::string placeholder;

	for (char c : spt_auto_render_feat.persist.cmdLineUnformatted)
	{
		if (isspace(c))
			c = ' ';
		if (c == '{' && inPlaceholder)
		{
			// what we have so far is not a real placeholder - flush as raw string
			out.text += '{';
			out.text += placeholder;
			placeholder.clear();
			inPlaceholder = c == '{';
		}
		else if (c == '{' && !inPlaceholder)
		{
			inPlaceholder = true;
		}
		else if (c == '}' && inPlaceholder)
		{
			// actual placeholder - check if it matches anything we have
			auto& allPlaceHolders = ArPlaceholder::GetAll();
			auto it = std::ranges::find(allPlaceHolders, placeholder, [](auto& p) { return p.get().key; });
			auto placeHolderVal = it == allPlaceHolders.cend() ? nullptr : it->get().GetValue();

			if (placeHolderVal)
			{
				out.text += *placeHolderVal;
			}
			else
			{
				out.text += '{';
				out.text += placeholder;
				out.text += '}';
				/*
				* If we didn't find a match by key - this is not a valid placeholder. If it has a
				* key but no value, we expect for it to be substituted later.
				*/
				if (it == allPlaceHolders.cend())
					out.unrecognizedPlaceholders.push_back(std::move(placeholder));
			}
			placeholder.clear();
			inPlaceholder = false;
		}
		else if (inPlaceholder)
		{
			placeholder += c;
		}
		else
		{
			out.text += c;
		}
	}

	if (inPlaceholder)
	{
		// flush as raw string
		out.text += '{';
		out.text += placeholder;
	}
}

void ArPlaceholders::RegenerateUuid()
{
	::UUID uuid;
	UuidCreate(&uuid);
	char* uuidStr;
	UuidToStringA(&uuid, (RPC_CSTR*)&uuidStr);
	UUID.SetValue(uuidStr);
	RpcStringFreeA((RPC_CSTR*)&uuidStr);
}

void ArPlaceholders::SetPipeNames(bool appendUuid)
{
	const char* videoPipeName = R"(\\.\pipe\spt_autorender_video)";
	const char* audioPipeName = R"(\\.\pipe\spt_autorender_audio)";

	if (appendUuid)
	{
		auto uuid = UUID.GetValue();
		ArPlaceholders::VIDEO_PIPE_NAME.SetValue(std::format("{}_{}", videoPipeName, uuid->c_str()));
		ArPlaceholders::AUDIO_PIPE_NAME.SetValue(std::format("{}_{}", audioPipeName, uuid->c_str()));
	}
	else
	{
		ArPlaceholders::VIDEO_PIPE_NAME.SetValue(videoPipeName);
		ArPlaceholders::AUDIO_PIPE_NAME.SetValue(audioPipeName);
	}
}

IMPL_HOOK_CDECL(AutoRenderFeature,
                void,
                S_TransferStereo16,
                void* pOutput,
                const portable_samplepair_t* pfront,
                int lpaintedtime,
                int endtime)
{
	ORIG_S_TransferStereo16(pOutput, pfront, lpaintedtime, endtime);

	// try to replicate the game's logic here

	short* snd_out;
	int* snd_p = (int*)pfront;
	int snd_linear_count;

	constexpr int deviceSampleCount = 44100;
	int samplePairCount = deviceSampleCount >> 1;
	int sampleMask = samplePairCount - 1;

	while (lpaintedtime < endtime)
	{
		int lpos = lpaintedtime & sampleMask;
		snd_out = (short*)pOutput + (lpos << 1);
		snd_linear_count = std::min(samplePairCount - lpos, samplePairCount - lpos);
		snd_linear_count <<= 1;
		snd_p += snd_linear_count;
		lpaintedtime += snd_linear_count >> 1;
	}
}
