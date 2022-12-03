#include "stdafx.h"

#include "logger.hpp"
#include "signals.hpp"

class LoggerHooks : public FeatureWrapper<LoggerHooks>
{
protected:
	void InitHooks() override;
	void LoadFeature() override;

private:
	// don't include tick signal - that's handled from logger.cpp
	void OnFrameSignal();
	void OnRestoreSignal(void*);
	void OnPauseSignal(void*, bool paused);
};

static LoggerHooks spt_lh;

void LoggerHooks::InitHooks() {}

void LoggerHooks::LoadFeature()
{
	FrameSignal.Connect(this, &LoggerHooks::OnFrameSignal);
	FinishRestoreSignal.Connect(this, &LoggerHooks::OnRestoreSignal);
	SetPausedSignal.Connect(this, &LoggerHooks::OnPauseSignal);
}

void LoggerHooks::OnFrameSignal()
{
	URINATE_SIMPLE(false);
}

void LoggerHooks::OnRestoreSignal(void*)
{
	URINATE_BEGIN_ON_LOAD();
}

void LoggerHooks::OnPauseSignal(void*, bool paused)
{
	URINATE_WITH_INFO(false, { uu.Spew("paused: %d", paused); });
}
