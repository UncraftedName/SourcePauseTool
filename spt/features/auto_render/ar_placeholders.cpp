#include "stdafx.hpp"

#include "ar_placeholders.hpp"
#include "ar_interface.hpp"

#include <random>

std::shared_ptr<const std::string> ArPlaceholder::GetValue() const
{
	auto ret = val.load();
	AssertMsg1(!!ret, "SPT: placeholder '%s' has no value set", key.c_str());
	return ret ? ret : std::make_shared<std::string>("NONE");
}

void ArPlaceholder::SetValue(std::string newVal)
{
	auto newValPtr = std::make_shared<std::string>(std::move(newVal));
	std::shared_lock lk(writeLock);
	auto oldVal = val.exchange(newValPtr, std::memory_order_release);
	if (pDirtyFlag && (!oldVal || *oldVal != *newValPtr))
		pDirtyFlag->store(true);
}

bool ArPhExePath::FindFfmpeg()
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

	SetValue(std::string(exePath, len));
	return true;
}

void ArPhUuid::Regenerate()
{
	// something random *enough*, don't care about it being a true UUID

	static std::random_device dev;
	static std::minstd_rand rng(dev());

	std::uniform_int_distribution<int> dist(0, 15);

	const char* chars = "0123456789abcdef";
	char str[36];

	for (int i = 0; i < ARRAYSIZE(str); i++)
		str[i] = chars[dist(rng)];
	str[8] = str[13] = str[18] = str[23] = '-';
	SetValue(std::string(str, sizeof(str)));
}

void ArPhPipeName::Regenerate()
{
	const char* pipeName = R"(\\.\pipe\spt_autorender)";
	if (appendUuid)
	{
		auto uuid = ArGlobalPlaceholders::UUID.GetValue();
		SetValue(std::format("{}_{}", pipeName, uuid->c_str()));
	}
	else
	{
		SetValue(pipeName);
	}
}

void ArPhDatetime::Update()
{
	auto now = std::chrono::round<std::chrono::seconds>(clock::now());
	if (lastSetDateTime.exchange(now) == now)
		return;
	lastSetDateTime = now; // store as default (UTC)
	auto localNow = std::chrono::zoned_time{std::chrono::current_zone(), now};
	SetValue(std::format("{0:%F}_{0:%H}-{0:%M}-{0:%S}", localNow));
}

std::string ArPlaceholder::FormatString(const std::vector<const ArPlaceholder*>& placeholders,
                                        const std::string& unformatted,
                                        std::vector<std::string>* unrecognizedPlaceholders)
{
	std::string formatted;
	formatted.reserve(unformatted.size() * 2);
	if (unrecognizedPlaceholders)
		unrecognizedPlaceholders->clear();

	std::string curPlaceholder;

	for (char c : unformatted)
	{
		if (isspace(c))
			c = ' ';
		if (c == '{')
		{
			if (!curPlaceholder.empty())
				formatted += curPlaceholder; // curPlaceholder is not a real placeholder - flush it
			curPlaceholder = '{';
		}
		else if (!curPlaceholder.empty())
		{
			curPlaceholder += c;
			if (c == '}')
			{
				// actual placeholder - check if it matches anything we have
				auto it =
				    std::ranges::find(placeholders, curPlaceholder, [](auto& p) { return p->key; });
				auto placeHolderVal = it == placeholders.cend() ? nullptr : (*it)->GetValue();

				if (placeHolderVal)
					formatted += *placeHolderVal;
				else
					formatted += curPlaceholder;
				curPlaceholder.clear();
			}
		}
		else
		{
			formatted += c;
		}
	}

	formatted += curPlaceholder; // not a real placeholder - flush it

	return formatted;
}
