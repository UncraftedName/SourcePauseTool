#include "stdafx.h"

#include "logger.hpp"
#include "spt\utils\file.hpp"
#include "spt\utils\signals.hpp"
#include "interfaces.hpp"
#include "spt\features\shadow.hpp"

CON_COMMAND_F(un_begin_rng_tracking,
              "Starts logging stuff on every tick to track rng, create a reference to track against first",
              FCVAR_DONTRECORD)
{
	if (args.ArgC() < 2)
	{
		Warning("%s <bool create_reference>\n", args[0]);
		return;
	}
	spt_logger.SetRngLogState((RngLogState)atoi(args[1]));
}

CON_COMMAND_F(un_begin_rng_tracking_on_load,
              "Starts tracking rng stuff as soon as possible on the next load",
              FCVAR_DONTRECORD)
{
	if (args.ArgC() < 2)
	{
		Warning("%s <bool create_reference>\n", args[0]);
		return;
	}
	g_queuedLogState = (RngLogState)atoi(args[1]);
	Msg("waiting for load...\n");
}

CON_COMMAND_F(un_end_rng_tracking, "Finishes logging stuff to track rng", FCVAR_DONTRECORD)
{
	spt_logger.EndRngLogging();
	Msg("fine then...\n");
}

CON_COMMAND_F(un_dump_rng_logs, "Dumps the reference and new rng tracking logs to a file", FCVAR_DONTRECORD)
{
	const auto& refLines = spt_logger.rngLogs[RNG_LOG_STATE_REFERENCE].lines;
	const auto& compLines = spt_logger.rngLogs[RNG_LOG_STATE_COMPARE].lines;
	int refChars = 0;
	int compChars = 0;
	{
		std::fstream o1(GetGameDir() + "/spt-rng-ref.log", std::ios_base::out | std::ios_base::trunc);
		for (size_t i = 0; i < refLines.size(); i++)
		{
			o1 << refLines[i] << "\n";
			refChars += refLines[i].size() + 1;
		}
	}
	{
		std::fstream o2(GetGameDir() + "/spt-rng-comp.log", std::ios_base::out | std::ios_base::trunc);
		for (size_t i = 0; i < compLines.size(); i++)
		{
			o2 << compLines[i] << "\n";
			compChars += compLines[i].size() + 1;
		}
	}
	Msg("wrote %d lines (%d chars) to (ref) & %d lines (%d chars) to (comp) files\n",
	    refLines.size(),
	    refChars,
	    compLines.size(),
	    compChars);
}

void LoggerFeature::LoadFeature()
{
	InitCommand(un_begin_rng_tracking);
	InitCommand(un_begin_rng_tracking_on_load);
	InitCommand(un_end_rng_tracking);
	InitCommand(un_dump_rng_logs);
	if (TickSignal.Works)
		TickSignal.Connect(this, &LoggerFeature::OnTickSignal);
}

void LoggerFeature::OnTickSignal()
{
	if (rngLogState != RNG_LOG_STATE_NONE)
		rngLogs[rngLogState].tick++;
	URINATE_SIMPLE(false);
}

void LoggerFeature::LogRngLineAtTick(const char* fmt, ...)
{
	if (rngLogState == RNG_LOG_STATE_NONE)
		return;

	auto& curLogs = rngLogs[rngLogState];
	auto& otherLogs = rngLogs[1 - rngLogState];

	if (rngLogState == RNG_LOG_STATE_COMPARE && otherLogs.lines.empty())
		EndRngLogging();

	va_list vargs;
	va_start(vargs, fmt);

	char nl1[1024];
	char nl2[1024];
	char fill[32];

	// setup fill buf
	int fillLen = MIN(curLogs.callDepth + 1, sizeof(fill));
	memset(fill, '-', fillLen);

	// handle va args
	int newLineLen = vsnprintf(nl1, sizeof(nl1), fmt, vargs);

	// add tick and fill
	newLineLen = snprintf(nl2,
	                      sizeof(nl2),
	                      "[%d %d]%.*s%.*s",
	                      interfaces::engine_tool->ServerTick(),
	                      curLogs.tick,
	                      fillLen,
	                      fill,
	                      newLineLen,
	                      nl1);

	curLogs.lines.emplace_back(nl2, newLineLen);

	if (otherLogs.lines.size() == curLogs.lines.size())
		EndRngLogging();
	if (rngLogState == RNG_LOG_STATE_COMPARE && curLogs.lines.back() != otherLogs.lines[curLogs.lines.size() - 1])
		EndRngLogging();
	if (curLogs.lines.size() >= MAX_LOG_LINES)
	{
		Warning("spt: Logger has exceeded %d lines, ending\n", MAX_LOG_LINES);
		EndRngLogging();
	}
}

void LoggerFeature::EndRngLogging()
{
	if (rngLogState == RNG_LOG_STATE_COMPARE)
	{
		auto& refLogs = rngLogs[RNG_LOG_STATE_REFERENCE];
		auto& compLogs = rngLogs[RNG_LOG_STATE_COMPARE];

		if (refLogs.lines.size() == 0)
		{
			if (compLogs.lines.size() == 0)
				Warning("spt-rng: Both reference and new logs are empty!\n");
			else
				Warning("spt-rng: Nothing to compare against!\n");
		}
		else
		{
			ShowRngLogMismatch(10);
		}
	}
	g_queuedLogState = RNG_LOG_STATE_NONE;
	rngLogState = RNG_LOG_STATE_NONE;
}

void LoggerFeature::ShowRngLogMismatch(int maxLines)
{
	auto& refLogs = rngLogs[RNG_LOG_STATE_REFERENCE];
	auto& compLogs = rngLogs[RNG_LOG_STATE_COMPARE];

	if (compLogs.lines.size() > refLogs.lines.size())
	{
		Warning("spt-rng: length of new logs exceeds reference length!\n");
	}
	else if (compLogs.lines.empty())
	{
		Warning("spt-rng: New logs are empty!\n");
	}
	else if (refLogs.lines.empty())
	{
		Warning("spt-rng: Nothing to compare against!\n");
	}
	else if (compLogs.lines.back() == refLogs.lines[compLogs.lines.size() - 1])
	{
		Warning("spt-rng: Logs completely match!\n");
	}
	else if (compLogs.lines.size() == 1)
	{
		Warning("spt-rng: First Line doesn't match!\n");
		_sleep(0);
		Msg("Expected: %s\nGot:      %s\n", refLogs.lines[0].c_str(), compLogs.lines[0].c_str());
	}
	else
	{
		Warning("spt-rng: Log mismatch, reference:\n");
		size_t start = MAX(0, (int)compLogs.lines.size() - maxLines + 1);
		size_t end = MIN(start + maxLines, MIN(refLogs.lines.size(), compLogs.lines.size()));
		for (size_t i = start; i < end; i++)
		{
			_sleep(0);
			Msg("line %d: %s\n", i + 1, refLogs.lines[i].c_str());
		}
		_sleep(0);
		Warning("Actual:\n");
		_sleep(0);
		Msg("line %d: %s\n", compLogs.lines.size(), compLogs.lines.back().c_str());
	}
}

void LoggerFeature::RngLogFunc(const char* funcName, RngLogFuncType type, const char* fmt, va_list vargs)
{
	if (rngLogState == RNG_LOG_STATE_NONE)
		return;
	auto& logs = rngLogs[rngLogState];
	char buf[1024];
	int len = vsnprintf(buf, sizeof(buf), fmt, vargs);
	const char* fill = len == 0 ? "" : " ";

	switch (type)
	{
	case RNG_LOG_FUNC_NAME_ONLY:
		LogRngLineAtTick("[%s]%s%.*s", funcName, fill, len, buf);
		break;
	case RNG_LOG_PRE_FUNC:
		LogRngLineAtTick("[PRE  %s]%s%.*s", funcName, fill, len, buf);
		logs.callDepth++;
		break;
	case RNG_LOG_POST_FUNC:
		if (logs.callDepth > 0)
			logs.callDepth--;
		LogRngLineAtTick("[POST %s]%s%.*s", funcName, fill, len, buf);
		break;
	}
}

void Urinator::Spew(const char* fmt, ...) const
{
	if (!pre && !logPrePost)
		return;
	va_list vargs;
	va_start(vargs, fmt);
	spt_logger.RngLogFunc(funcName,
	                      logPrePost ? (pre ? RNG_LOG_PRE_FUNC : RNG_LOG_POST_FUNC) : RNG_LOG_FUNC_NAME_ONLY,
	                      fmt,
	                      vargs);
}

void Urinator::SpewWithVPhysInfo(const char* fmt, ...) const
{
	va_list vargs;
	va_start(vargs, fmt);
	char buf[1024];
	int len = vsnprintf(buf, sizeof(buf), fmt ? fmt : "", vargs);
	const char* fill = len == 0 ? "" : ", ";

	auto pc = spt_player_shadow.GetPlayerController();
	if (!pc)
	{
		Spew("shadow controller is null%s%.*s", fill, len, buf);
		return;
	}
	IPhysicsObject* cPhysObject = ((IPhysicsObject**)pc)[2];
	if (!cPhysObject)
	{
		Spew("shadow phys object is null%s%.*s", fill, len, buf);
		return;
	}

	struct IvpRealObject
	{
		void* Core()
		{
			return ((void**)this)[0x90 / 4];
		}

		static Vector VelToHl(IvpFloatPoint vp)
		{
			Vector v;
			v[0] = vp.k[0];
			v[1] = vp.k[2];
			v[2] = -vp.k[1];
			return v / METERS_PER_INCH;
		}

		Vector Speed()
		{
			return VelToHl(((IvpFloatPoint*)Core())[0x90 / sizeof(IvpFloatPoint)]);
		}

		Vector SpeedChange()
		{
			return VelToHl(((IvpFloatPoint*)Core())[0x70 / sizeof(IvpFloatPoint)]);
		}
	};

	IvpRealObject* pivp = ((IvpRealObject**)cPhysObject)[2];
	if (!pivp)
	{
		Spew("shadow ivp object is null%s%.*s", fill, len, buf);
		return;
	}
	Vector v;
	QAngle q;
	spt_player_shadow.ORIG_GetShadowPosition(pc, &v, &q);

	Vector s = pivp->Speed();
	Vector sc = pivp->SpeedChange();

	Spew("shadow pos/ang/vel/vel change: " V_FMT "/" V_FMT "/" V_FMT "/" V_FMT "%s%.*s",
	     VEC_UNP(v),
	     VEC_UNP(q),
	     VEC_UNP(s),
	     VEC_UNP(sc),
	     fill,
	     len,
	     buf);
}
