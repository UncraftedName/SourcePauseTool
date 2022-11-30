#pragma once
#include <fstream>
#include <ostream>
#include <iomanip>
#include "spt\feature.hpp"
#include "spt\features\shadow.hpp"
#include "spt\utils\file.hpp"

#define F_HUD_FMT "%.3f"
#define V_HUD_FMT "<" F_HUD_FMT "," F_HUD_FMT "," F_HUD_FMT ">"
#define V_FMT "<%f, %f, %f>"
#define VEC_UNP(v) (v).x, (v).y, (v).z

enum RngLogState
{
	RNG_LOG_STATE_NONE = -1,
	RNG_LOG_STATE_COMPARE,
	RNG_LOG_STATE_REFERENCE,
};

inline RngLogState g_queuedLogState = RNG_LOG_STATE_NONE;

enum RngLogFuncType
{
	RNG_LOG_FUNC_NAME_ONLY,
	RNG_LOG_PRE_FUNC,
	RNG_LOG_POST_FUNC
};

#define MAX_LOG_LINES 1'000'000

class LoggerFeature : public FeatureWrapper<LoggerFeature>
{
protected:
	virtual void LoadFeature() override;

	virtual void UnloadFeature() override{};

	void SetPaused(void*, bool paused);
	void OnTick();
	void OnRestore(void*);

public: // public, but not meant to be used from elsewhere
	void SetRngLogState(RngLogState state)
	{
		switch (rngLogState = state)
		{
		case RNG_LOG_STATE_REFERENCE:
			rngLogs[RNG_LOG_STATE_REFERENCE].Clear();
		case RNG_LOG_STATE_COMPARE:
			rngLogs[RNG_LOG_STATE_COMPARE].Clear();
			Warning("spt: started logging\n");
			break;
		}

	}

	void LogRngLineAtTick(const char* fmt, ...);

	void EndRngLogging();
	void ShowRngLogMismatch(int maxLines);

	struct RngLogs
	{
		std::vector<std::string> lines;
		bool active;
		int tick;
		int callDepth;

		void Clear()
		{
			lines.clear();
			tick = 0;
			callDepth = 0;
		}
	} rngLogs[2];

	RngLogState rngLogState = RNG_LOG_STATE_NONE;

	void RngLogFunc(const char* funcName, RngLogFuncType type, const char* fmt, va_list vargs);

	void RngLogFunc(const char* funcName, int type, const char* fmt, ...)
	{
		va_list vargs;
		va_start(vargs, fmt);
		RngLogFunc(funcName, type, fmt, vargs);
	}
};

inline LoggerFeature spt_logger;

typedef std::function<void(const class Urinator&)> UrinateFunc;

class Urinator
{
private:
	bool pre, logPrePost;
	const char* funcName;
	const UrinateFunc urinateFunc;

private:
	void SpewInternal() const
	{
		if (urinateFunc)
			urinateFunc(*this);
		else
			Spew("");
	}

public:
	// the log bool param allows me to reuse the same macros if I don't want to log pre/post
	Urinator(const char* funcName, bool logPrePost, UrinateFunc urinateFunc = nullptr)
	    : pre(true), funcName(funcName), urinateFunc(urinateFunc), logPrePost(logPrePost)
	{
		SpewInternal();
	}

	~Urinator()
	{
		pre = false;
		SpewInternal();
	}

	void Spew(const char* fmt, ...) const;

	// logs vphys info followed by whatever other stuff is passed in
	void SpewWithVPhysInfo(const char* fmt, ...) const;
};

#define URINATE_SIMPLE(logPrePost) Urinator urinator(__FUNCTION__, logPrePost)

#define URINATE_WITH_INFO(logPrePost, urinate_body) \
	Urinator urinator(__FUNCTION__, logPrePost, [&](const Urinator& uu) { urinate_body })

#define URINATE_WITH_VPHYS(logPrePost) URINATE_WITH_INFO(logPrePost, { uu.SpewWithVPhysInfo(nullptr); })

#define URINATE_BEGIN_ON_LOAD() \
	if (g_queuedLogState != RNG_LOG_STATE_NONE) \
	{ \
		spt_logger.SetRngLogState(g_queuedLogState); \
		g_queuedLogState = RNG_LOG_STATE_NONE; \
		URINATE_WITH_INFO(false, uu.Spew("Starting logging!");); \
	}
