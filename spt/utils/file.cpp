#include "stdafx.hpp"
#include "file.hpp"
#include "interfaces.hpp"

#include <fstream>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <stringapiset.h>

bool FileExists(const std::string& fileName)
{
	std::string dir = fileName;
	std::ifstream is;
	is.open(dir);
	return is.is_open();
}

std::string GetGameDir()
{
	char BUFFER[256];
	if (!interfaces::engine_server)
		return std::string();
	else
	{
		interfaces::engine_server->GetGameDir(BUFFER, ARRAYSIZE(BUFFER));
		return BUFFER;
	}
}

std::strong_ordering utils::NaturalCompare::Compare3Way(const std::filesystem::path& a,
                                                        const std::filesystem::path& b) const
{
	int ret =
	    CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS, a.c_str(), -1, b.c_str(), -1);

	switch (ret)
	{
	case CSTR_LESS_THAN:
		return std::strong_ordering::less;
	case CSTR_GREATER_THAN:
		return std::strong_ordering::greater;
	default:
		AssertMsg(ret, "spt: CompareStringW failed");
		[[fallthrough]];
	case CSTR_EQUAL:
		return std::strong_ordering::equivalent;
	}
}

bool utils::NaturalCompare::operator()(const std::filesystem::path& a, const std::filesystem::path& b) const
{
	return Compare3Way(a, b) < 0;
}

std::filesystem::path utils::ResolveUserPath(const char* userPath, const char* optExt, std::error_code& ec)
{
	std::filesystem::path ret = userPath;
	if (!ret.is_absolute())
		ret = std::filesystem::path(GetGameDir()) / userPath;
	if (optExt && !ret.string().ends_with(optExt))
		ret += optExt;
	return std::filesystem::absolute(ret, ec);
}

std::filesystem::path utils::GetNextFileName(const std::filesystem::path& base, const char* optExt, size_t* counter)
{
	constexpr size_t MIN_COUNTER = 1;

	size_t tmpCounter = MIN_COUNTER;
	if (!counter)
		counter = &tmpCounter;
	else if (*counter < MIN_COUNTER)
		*counter = MIN_COUNTER;

	if (std::filesystem::is_directory(base))
	{
		Assert(0);
		return base;
	}

	std::filesystem::path newPath;

	if (!std::filesystem::exists(base.parent_path()))
	{
		*counter = MIN_COUNTER;
		newPath = std::format(L"{}_{}", base.native(), *counter);
		if (optExt)
			newPath += optExt;
	}

	bool atLeastOneExists = false;
	for (;;)
	{
		newPath = std::format(L"{}_{}", base.native(), *counter);
		if (optExt)
			newPath += optExt;
		std::error_code ec;
		if (std::filesystem::exists(newPath, ec))
		{
			++*counter;
			atLeastOneExists = true;
		}
		else if (atLeastOneExists || *counter == MIN_COUNTER)
		{
			return newPath;
		}
		else
		{
			*counter = MIN_COUNTER;
		}
	}
}

std::filesystem::path utils::GetPathProximateToModDir(const std::filesystem::path& p, std::error_code* ec)
{
	static std::filesystem::path modDir;
	if (modDir.empty())
	{
		modDir = GetGameDir();
		std::error_code tmpEc;
		modDir = std::filesystem::absolute(modDir, tmpEc);
		Assert(!tmpEc);
	}
	std::error_code tmpEc;
	return std::filesystem::proximate(p, modDir, ec ? *ec : tmpEc);
}
