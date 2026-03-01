#pragma once
#include <string>
#include <filesystem>

bool FileExists(const std::string& fileName);
std::string GetGameDir();

namespace utils
{
	struct NaturalCompare
	{
		// general <=> comparison
		std::strong_ordering Compare3Way(const std::filesystem::path& a, const std::filesystem::path& b) const;

		// for e.g. std::map<std::filesystem::path>
		bool operator()(const std::filesystem::path& a, const std::filesystem::path& b) const;
	};

	/*
	* Given a user path and optional extention, returns a path relative to the mod directory:
	* - ResolveUserPath("my_file", nullptr) -> "MOD_DIR/my_file"
	* - ResolveUserPath("my_file", ".dem") -> "MOD_DIR/my_file.dem"
	* - ResolveUserPath("my_file.dem", ".dem") -> "MOD_DIR/my_file.dem"
	* - ResolveUserPath("F:\old_demos\my_demo", ".dem") -> "F:\old_demos\my_demo.dem"
	*/
	std::filesystem::path ResolveUserPath(const char* userPath, const char* optExt, std::error_code& ec);

	/*
	* Like PathYetAnotherMakeUniqueName, but:
	* - works with multiple extensions like .sptr.xz
	* - works directly on std::filesystem::path, so the file doesn't need to exist
	* - formats as 'file_1.txt', 'file_2.txt', etc. instead of window's 'file.txt', 'file (1).txt', etc.
	* 
	* base - everything without the extension(s) (e.g. myfile -> myfile_1, myfile_2...)
	* optExt - just the extension(s), if present (can be null)
	* counter - used to prevent re-iterating the directory, e.g. if counter is 7 and file_7.txt
	*           exists, will start by checking file_8.txt and update counter; can be null
	*/
	std::filesystem::path GetNextFileName(const std::filesystem::path& base, const char* optExt, size_t* counter);
} // namespace utils
