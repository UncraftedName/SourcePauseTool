#pragma once

#include "tr_structs.hpp"

#ifdef SPT_PLAYER_TRACE_ENABLED

#define LZMA_API_STATIC
#include "thirdparty/xz/include/lzma.h"

#define TR_FILE_EXTENSION ".sptr.xz"

namespace player_trace
{
	constexpr char TR_FILE_ID[8] = "sausage";
	constexpr uint32_t TR_SERIALIZE_VERSION = 1;
	constexpr size_t TR_MAX_SPT_VERSION_LEN = 32;

	struct TrPreamble
	{
		char fileId[sizeof TR_FILE_ID];
		uint32_t fileVersion;
	};

	struct TrHeader
	{
		char sptVersion[TR_MAX_SPT_VERSION_LEN];
		uint32_t lumpsOff;
		uint32_t nLumps;
		uint32_t numRecordedTicks;
		TrIdx<TrAbsBox> playerStandBboxIdx, playerDuckBboxIdx;
	};

	struct TrLump_v1
	{
		char name[TR_MAX_LUMP_NAME_LEN];
		tr_struct_version struct_version;
		uint32_t dataOff;
		uint32_t dataLenBytes;
		// not necessary but useful for external tools so they don't have to know how to parse the each lump
		uint32_t nElems;
	};

	using TrLump = struct TrLump_v1;

	class ITrWriter
	{
	public:
		bool WriteTrace(const TrPlayerTrace& tr);

	protected:
		virtual bool Write(std::span<const std::byte> sp) = 0;
		virtual void DoneWritingTrace() = 0;

		template<typename T>
		bool Write(const T& o)
		{
			return Write(std::span{(const std::byte*)&o, sizeof o});
		}

	private:
		template<typename T>
		bool WriteLumpHeader(const std::vector<T>& vec, uint32_t& fileOff, uint32_t& lumpDataFileOff);

		template<typename T>
		bool WriteLumpData(const std::vector<T>& vec, uint32_t& fileOff);
	};

	class ITrReader
	{
	public:
		template<typename T>
		bool ReadTo(T& o, uint32_t at)
		{
			return ReadTo(std::span{(std::byte*)&o, sizeof o}, at);
		}

		virtual bool ReadTo(std::span<std::byte> sp, uint32_t at) = 0;

		virtual void DoneReadingTrace() = 0;
	};

	struct TrLumpCompatHandler
	{
		inline static std::unordered_map<std::string, std::vector<const TrLumpCompatHandler*>> handlersByName;

		std::string lumpName;
		tr_struct_version lumpVersion;
		std::vector<std::string> lumpDependencies;

		TrLumpCompatHandler(const std::string& lumpName,
		                    tr_struct_version lumpVersion,
		                    const std::vector<std::string>& lumpDependencies)
		    : lumpName{lumpName}, lumpVersion{lumpVersion}, lumpDependencies{lumpDependencies}
		{
			auto& vec = handlersByName[lumpName];
			auto biggerVerHandlerIt = std::ranges::find_if(vec,
			                                               [lumpVersion](auto handler)
			                                               { return handler->lumpVersion >= lumpVersion; });
			AssertMsg2(biggerVerHandlerIt == vec.cend()
			               || (*biggerVerHandlerIt)->lumpVersion != lumpVersion,
			           "SPT: duplicate TrLumpCompatHandler version %d for lump %s",
			           lumpVersion,
			           lumpName.c_str());
			vec.insert(biggerVerHandlerIt, this);
		}

		virtual bool HandleCompat(ITrReader& rd, TrLump& lump, std::vector<std::byte>& dataInOut) const = 0;
	};

	class TrRestore
	{
	public:
		// public, read these

		std::string errMsg;
		std::vector<std::string> warnings;

		// internal, used while parsing

		struct TrLumpDepInfo
		{
			size_t nDependencies;
			std::span<const TrLumpCompatHandler*> handlers;
		};

		TrPlayerTrace* trace = nullptr;
		ITrReader* reader = nullptr;
		std::unordered_map<std::string_view, TrLump*> nameToLump;
		std::unordered_map<const TrLump*, TrLumpDepInfo> lumpDependencyMap;

		void Clear()
		{
			errMsg.clear();
			warnings.clear();
			trace = nullptr;
			reader = nullptr;
			nameToLump.clear();
			lumpDependencyMap.clear();
		}

		// if false is returned, errMsg should have a message
		// if true is returned, the trace is valid and 'warnings' may have messages
		bool Restore(TrPlayerTrace& tr, ITrReader& rd);
	};

	constexpr char TR_XZ_FILE_ID[] = "omg_hi!";
	constexpr uint32_t TR_XZ_FILE_VERSION = 1;

	struct TrXzFooter
	{
		uint32_t numCompressedBytes;
		uint32_t numUncompressedBytes;
		char id[sizeof TR_XZ_FILE_ID];
		uint32_t version;
	};

	class TrXzFileWriter : public ITrWriter
	{
		lzma_stream lzma_strm;
		bool alive;
		std::vector<uint8_t> outBuf;
		std::ostream& oStream;

	public:
		TrXzFileWriter(std::ostream& oStream, uint32_t compressionLevel = 1);
		~TrXzFileWriter();

	protected:
		virtual bool Write(std::span<const std::byte> sp);
		virtual void DoneWritingTrace();

	private:
		bool LzmaToOfStream(bool finish);
	};

	class TrXzFileReader : public ITrReader
	{
		/*
		* Since lzma streams don't support seeking and we can't guarantee that we will read data
		* sequentially, I'm going with the naive approach of decompressing the whole thing at once.
		*/
		std::vector<uint8_t> outBuf;

	public:
		/*
		* If an error happens while decoding the lzma stream, it will be logged here and all ReadTo
		* calls (and therefore ReadTrace) will return false. Prioritize reading this message over
		* the message returned by ITrReader::ReadTrace
		* 
		* TODO change this
		*/
		std::string errMsg;

		TrXzFileReader(std::istream& iStream);

	protected:
		virtual bool ReadTo(std::span<std::byte> sp, uint32_t at);
		virtual void DoneReadingTrace() {};
	};

} // namespace player_trace

#endif
