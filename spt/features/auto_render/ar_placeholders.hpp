#pragma once

#include <atomic>
#include <string>
#include <memory>
#include <optional>
#include <format>
#include <chrono>
#include <shared_mutex>

/*
* A placeholder is a key/value pair of utf8 strings in the form "{x}"->"y". These are named tokens
* that are replaced at runtime - it makes it possible to build a cmdLine template with generic
* values (e.g. a file path, video framerate, video dimensions, etc.).
* 
* For example, this unformatted cmdLine:
* "{EXE_PATH} -o \"{OUTPUT_DIR}\\{DATE_TIME}.mp4\""
* 
* Can get converted to:
* "C:\\YourMom\\ffmpeg.exe -o \"A:\\Renders\\2025-10-15_1345.mp4\""
* 
* Since multiple threads read/write to these, they are make to be thread safe, but they do have an
* allocation cost so they should be updated infrequently. They can also set an external atomic bool
* if a value has changed.
* 
* The strings are utf8 for compatibility with ImGui, but they are converted to utf16 when launching
* ffmpeg.
*/
class ArPlaceholder
{
	std::atomic<std::shared_ptr<std::string>> val;
	std::atomic<bool>* pDirtyFlag;

public:
	/*
	* Lock this exclusively to disable writes to all placeholders. This is used when formatting and copying
	* values to FfmpegArgs to ensure that no values change mid-format. A shared mutex is definitely not the
	* best way to do this, but it's simple enough.
	*/
	static inline std::shared_mutex writeLock;

	const std::string key;
	const std::string helpText;

	// if the dirty flag exists, it will be set when the value is changed through SetValue
	explicit ArPlaceholder(std::string key, std::string helpText, std::atomic<bool>* pDirtyFlag = nullptr)
	    : key(std::move(key)), helpText(std::move(helpText)), pDirtyFlag(pDirtyFlag)
	{
	}

	void SetValue(std::string newVal);
	std::shared_ptr<const std::string> GetValue() const;

	// make a copy in order to replace one of the ArGlobalPlaceholders
	template<typename T = ArPlaceholder>
	T CopyByKey(std::atomic<bool>* pDirtyFlag_ = nullptr)
	{
		return T{key, "NO HELP TEXT", pDirtyFlag_};
	}

	/*
	* Substitutes all of the placeholders in the unformatted string. If GetValue() returns nullptr,
	* the sampleVal is substituted instead.
	*/
	static std::string FormatString(const std::vector<const ArPlaceholder*>& placeholders,
	                                const std::string& unformatted,
	                                std::vector<std::string>* unrecognizedPlaceholders);
};

class ArPhExePath : public ArPlaceholder
{
	bool success = false;

public:
	using ArPlaceholder::ArPlaceholder;
	void FindFfmpeg();

	bool LastWasSuccess() const
	{
		return success;
	}
};

class ArPhUuid : public ArPlaceholder
{
public:
	using ArPlaceholder::ArPlaceholder;
	void Regenerate();
};

class ArPhPipeName : public ArPlaceholder
{
public:
	using ArPlaceholder::ArPlaceholder;
	int appendUuid = true; // use int for ImGui compatibility
	void Regenerate();
};

class ArPhDatetime : public ArPlaceholder
{
	using clock = std::chrono::system_clock;
	std::atomic<clock::time_point> lastSetDateTime;

public:
	using ArPlaceholder::ArPlaceholder;
	void Update();
};

// a bunch of placeholders, default values are set in LoadFeature
class ArGlobalPlaceholders
{
	inline static std::atomic<bool> imGuiFormattedCmdLineDirty = true;
	friend struct ArImGuiPersist;

public:
#define AR_GLOBAL_PLACEHOLDER(type, name, helpText) \
	inline static type name \
	{ \
		"{" #name "}", helpText, &imGuiFormattedCmdLineDirty \
	}

	// clang-format off
	AR_GLOBAL_PLACEHOLDER(ArPhExePath, EXE_PATH, "Path to the rendering application");
	AR_GLOBAL_PLACEHOLDER(ArPlaceholder, VID_WIDTH, "Input video width in pixels");
	AR_GLOBAL_PLACEHOLDER(ArPlaceholder, VID_HEIGHT, "Input video height in pixels");
	AR_GLOBAL_PLACEHOLDER(ArPlaceholder, FRAMERATE, "Output framerate");
	AR_GLOBAL_PLACEHOLDER(ArPhUuid, UUID, "Unique UUID for each video");
	AR_GLOBAL_PLACEHOLDER(ArPhPipeName, PIPE_NAME, "Name of the pipe that the data will be fed to");
	AR_GLOBAL_PLACEHOLDER(ArPhDatetime, DATE_TIME, "Date and time formatted as YYYY-MM-DD_HH-MM-SS");
	AR_GLOBAL_PLACEHOLDER(ArPlaceholder, RENDER_WORKING_DIR, "Working directory of the rendering application");
	AR_GLOBAL_PLACEHOLDER(ArPlaceholder, GAME_WORKING_DIR, "Working directory of the game");
	AR_GLOBAL_PLACEHOLDER(ArPlaceholder, MOD_DIR, "Mod directory");
	AR_GLOBAL_PLACEHOLDER(ArPlaceholder, DEMO_SEQ, "The demo index start at 0 formated as '_X'");
	AR_GLOBAL_PLACEHOLDER(ArPlaceholder, DEMO_NAME, "The demo file name without extension start at 0 formated as '_name'");
	// clang-format on

#undef AR_GLOBAL_PLACEHOLDER

	static const auto& GetAll()
	{
		static std::vector<const ArPlaceholder*> phs = {
		    &EXE_PATH,
		    &VID_WIDTH,
		    &VID_HEIGHT,
		    &FRAMERATE,
		    &UUID,
		    &PIPE_NAME,
		    &DATE_TIME,
		    &RENDER_WORKING_DIR,
		    &MOD_DIR,
		    &DEMO_SEQ,
		    &DEMO_NAME,
		};
		return phs;
	}
};
