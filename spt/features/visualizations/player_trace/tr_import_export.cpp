#include "stdafx.hpp"

#include <unordered_set>

#include "tr_import_export.hpp"

#ifdef SPT_PLAYER_TRACE_ENABLED

extern const char* SPT_VERSION;

using namespace player_trace;

template<typename T>
bool ITrWriter::WriteLumpHeader(const std::vector<T>& vec, uint32_t& fileOff, uint32_t& lumpDataFileOff)
{
	TrLump lump{
	    .struct_version = TR_LUMP_VERSION(T),
	    .dataOff = lumpDataFileOff,
	    .dataLenBytes = vec.size() * sizeof(T),
	    .nElems = vec.size(),
	};
	strncpy(lump.name, TR_LUMP_NAME(T), sizeof lump.name);
	if (!Write(lump))
		return false;
	fileOff += sizeof lump;
	lumpDataFileOff += lump.dataLenBytes;
	return true;
}

template<typename T>
bool ITrWriter::WriteLumpData(const std::vector<T>& vec, uint32_t& fileOff)
{
	if (!Write(std::as_bytes(std::span{vec})))
		return false;
	fileOff += vec.size() * sizeof(T);
	return true;
}

bool ITrWriter::WriteTrace(const TrPlayerTrace& tr)
{
	size_t fileOff = 0;

	TrPreamble preamble{.fileVersion = TR_SERIALIZE_VERSION};
	memcpy(preamble.fileId, TR_FILE_ID, sizeof preamble.fileId);
	if (!Write(preamble))
		return false;
	fileOff += sizeof preamble;

	constexpr uint32_t nLumps = std::tuple_size_v<decltype(tr._storage)>;

	TrHeader header{
	    .lumpsOff = fileOff + sizeof header,
	    .nLumps = nLumps,
	    .numRecordedTicks = tr.numRecordedTicks,
	    .playerStandBboxIdx = tr.playerStandBboxIdx,
	    .playerDuckBboxIdx = tr.playerDuckBboxIdx,
	};
	strncpy(header.sptVersion, SPT_VERSION, sizeof header.sptVersion);
	if (!Write(header))
		return false;
	fileOff += sizeof header;

	uint32_t lumpDataFileOff = fileOff + sizeof(TrLump) * nLumps;
	if (!std::apply([&](auto&... vecs) { return (WriteLumpHeader(vecs, fileOff, lumpDataFileOff) && ...); },
	                tr._storage))
		return false;

	if (!std::apply([&](auto&... vecs) { return (WriteLumpData(vecs, fileOff) && ...); }, tr._storage))
		return false;

	DoneWritingTrace();
	return true;
}

template<typename T>
static bool ReadLumpData(TrRestore& restore, TrLump& lump, TrRestore::TrLumpDepInfo& depInfo)
{
	auto& tr = *restore.trace;
	auto& rd = *restore.reader;
	bool anyHandlers = !depInfo.handlers.empty();

	std::vector<std::byte> compatBuf;

	if (anyHandlers)
	{
		restore.warnings.push_back(
		    std::format("applying {} compatibility handler(s) to lump '{}' (upgrading from version {} to {})",
		                depInfo.handlers.size(),
		                lump.name,
		                lump.struct_version,
		                TR_LUMP_VERSION(T)));

		compatBuf.resize(lump.dataLenBytes);
		if (!rd.ReadTo(std::span{compatBuf}, lump.dataOff))
			return false;

		for (auto handler : depInfo.handlers)
		{
			if (handler->lumpVersion != lump.struct_version)
				continue;
			if (!handler->HandleCompat(rd, lump, compatBuf))
			{
				if (restore.errMsg.empty())
				{
					restore.errMsg = std::format("failed to upgrade lump '{}' to version {}",
					                             lump.name,
					                             TR_LUMP_VERSION(T));
				}
			}
		}
	}

	if (lump.struct_version != TR_LUMP_VERSION(T))
	{
		restore.errMsg = std::format("bad lump version for lump '{}'{} (expected {}, got {})",
		                             lump.name,
		                             depInfo.handlers.empty() ? "" : " after upgrading",
		                             TR_LUMP_VERSION(T),
		                             lump.struct_version);
		return false;
	}

	std::vector<T>& vec = tr.Get<T>();
	bool numBytesSeemsReasonable = depInfo.handlers.empty() ? sizeof(T) * lump.nElems == lump.dataLenBytes
	                                                        : compatBuf.size() % sizeof(vec[0]) == 0;

	if (!numBytesSeemsReasonable)
	{
		restore.errMsg = std::format("unexpected number of bytes for lump '{}'", lump.name);
		return false;
	}

	if (anyHandlers)
	{
		vec.resize(compatBuf.size() / sizeof(vec[0]));
		memcpy(vec.data(), compatBuf.data(), compatBuf.size());
	}
	else
	{
		vec.resize(lump.nElems);
		if (!rd.ReadTo(std::as_writable_bytes(std::span{vec}), lump.dataOff))
			return false;
	}
	return true;
}

static bool TrResolveLumpDependencies(TrRestore& restore, const TrLump& lump, int recurseCount = 0)
{
	if (recurseCount > 100)
	{
		restore.errMsg = "recursive limit reached while calculating lump dependencies";
		return false;
	}

	std::span<TrLumpCompatHandler*> test{};
	auto [lumpDepIt, isNew] = restore.lumpDependencyMap.try_emplace(&lump);
	if (!isNew)
		return true;

	auto handlersIt = TrLumpCompatHandler::handlersByName.find(lump.name);
	if (handlersIt == TrLumpCompatHandler::handlersByName.cend())
		return true;

	auto firstHandlerIt =
	    std::ranges::find_if(handlersIt->second,
	                         [&lump](auto handler) { return handler->lumpVersion >= lump.struct_version; });

	lumpDepIt->second.handlers = std::span{firstHandlerIt, handlersIt->second.cend()};

	for (auto handler : lumpDepIt->second.handlers)
	{
		for (auto& depName : handler->lumpDependencies)
		{
			auto depLumpIt = restore.nameToLump.find(depName);
			if (depLumpIt == restore.nameToLump.cend())
			{
				restore.errMsg =
				    std::format("handler for lump '{}' has dependency on non-existent lump '{}'",
				                lump.name,
				                depName);
				return false;
			}

			if (!TrResolveLumpDependencies(restore, *restore.nameToLump[depName], recurseCount + 1))
				return false;

			lumpDepIt->second.nDependencies += restore.lumpDependencyMap.at(&lump).nDependencies;
		}
	}
	return true;
}

struct TrReadDispatch
{
	TrLump& lump;
	TrRestore::TrLumpDepInfo& depInfo;
	std::function<bool()> readFunc;

	bool operator<(const TrReadDispatch& o) const
	{
		return depInfo.nDependencies < o.depInfo.nDependencies;
	}
};

template<typename T>
void TrSetupLumpRead(TrRestore& restore, std::multiset<TrReadDispatch>& orderedLumps)
{
	const char* lumpName = TR_LUMP_NAME(T);

	auto it = restore.nameToLump.find(lumpName);
	if (it == restore.nameToLump.cend())
	{
		restore.warnings.push_back(
		    std::format("lump '{}' declared in TrPlayerTrace but not found in file", lumpName));
		return;
	}

	TrLump& lump = *it->second;
	TrRestore::TrLumpDepInfo& depInfo = restore.lumpDependencyMap.at(&lump);

	orderedLumps.emplace(lump, depInfo, [&]() { return ReadLumpData<T>(restore, lump, depInfo); });
	restore.nameToLump.erase(it);
}

bool TrRestore::Restore(TrPlayerTrace& tr, ITrReader& rd)
{
	Clear();
	tr.Clear();
	trace = &tr;
	reader = &rd;

	TrPreamble preamble;
	if (!rd.ReadTo(preamble, 0))
	{
		errMsg = "failed to read preamble";
		return false;
	}

	if (sizeof(preamble.fileId) != sizeof(TR_FILE_ID)
	    || memcmp(preamble.fileId, TR_FILE_ID, sizeof(preamble.fileId)))
	{
		errMsg = "bad file ID";
		return false;
	}

	if (preamble.fileVersion != TR_SERIALIZE_VERSION)
	{
		// for now, no backwards or forwards compat
		errMsg = std::format("expected file version {}, got {}", TR_SERIALIZE_VERSION, preamble.fileVersion);
		return false;
	}

	TrHeader header;
	if (!rd.ReadTo(header, sizeof preamble))
	{
		errMsg = "failed to read header";
		return false;
	}
	if (strncmp(header.sptVersion, SPT_VERSION, TR_MAX_SPT_VERSION_LEN))
	{
		warnings.push_back(
		    std::format("trace was exported with different SPT version '{}' (current version is '{}')",
		                header.sptVersion,
		                SPT_VERSION));
	}

	tr.numRecordedTicks = header.numRecordedTicks;
	tr.playerStandBboxIdx = header.playerStandBboxIdx;
	tr.playerDuckBboxIdx = header.playerDuckBboxIdx;

	std::vector<TrLump> lumps(header.nLumps);
	if (!rd.ReadTo(std::as_writable_bytes(std::span{lumps}), header.lumpsOff))
	{
		errMsg = "failed to read lump headers";
		return false;
	}

	for (auto& lump : lumps)
	{
		lump.name[sizeof(lump.name) - 1] = '\x0'; // explicitly null terminate so we can use it in errors
		nameToLump[lump.name] = &lump;
	}

	for (const TrLump& lump : lumps)
	{
		if (!TrResolveLumpDependencies(*this, lump))
		{
			if (errMsg.empty())
				errMsg = std::format("failed to compute lump dependencies for lump", lump.name);
			return false;
		}
	}

	std::multiset<TrReadDispatch> orderedLumps;

	std::apply([&](auto&... vecs)
	           { (TrSetupLumpRead<typename std::decay_t<decltype(vecs)>::value_type>(*this, orderedLumps), ...); },
	           tr._storage);

	TrReadContextScope scope{tr};

	for (auto& dispatch : orderedLumps)
	{
		if (!dispatch.readFunc())
		{
			if (errMsg.empty())
				errMsg = std::format("failed to dispatch read for lump '{}'", dispatch.lump.name);
			return false;
		}
	}

	rd.DoneReadingTrace();

	if (!nameToLump.empty())
		for (auto& [lumpName, _] : nameToLump)
			warnings.push_back(std::format("lump '{}' was ignored", lumpName));

	return true;
}

TrXzFileWriter::TrXzFileWriter(std::ostream& oStream, uint32_t compressionLevel)
    : oStream{oStream}, lzma_strm{lzma_stream LZMA_STREAM_INIT}, alive{true}
{
	lzma_mt mt{
	    .threads = std::thread::hardware_concurrency(),
	    .timeout = 0,
	    .preset = compressionLevel,
	    .check = LZMA_CHECK_CRC64,
	};
	alive = lzma_stream_encoder_mt(&lzma_strm, &mt) == LZMA_OK;
	if (alive)
		outBuf.resize(1 << 16);
}

TrXzFileWriter::~TrXzFileWriter()
{
	lzma_end(&lzma_strm);
}

bool TrXzFileWriter::Write(std::span<const std::byte> sp)
{
	if (!alive)
		return false;
	lzma_strm.avail_in = sp.size_bytes();
	lzma_strm.next_in = (uint8_t*)sp.data();
	return LzmaToOfStream(false);
}

void TrXzFileWriter::DoneWritingTrace()
{
	alive = LzmaToOfStream(true);
	if (alive)
	{
		TrXzFooter footer{
		    .numCompressedBytes = (uint32_t)lzma_strm.total_out,
		    .numUncompressedBytes = (uint32_t)lzma_strm.total_in,
		    .version = TR_XZ_FILE_VERSION,
		};
		memcpy(footer.id, TR_XZ_FILE_ID, sizeof footer.id);
		alive = oStream.write((char*)&footer, sizeof footer).good();
	}
	if (alive)
		alive = oStream.flush().good();
}

bool TrXzFileWriter::LzmaToOfStream(bool finish)
{
	while (alive)
	{
		lzma_strm.avail_out = outBuf.size();
		lzma_strm.next_out = outBuf.data();
		lzma_ret ret = lzma_code(&lzma_strm, finish ? LZMA_FINISH : LZMA_RUN);
		alive = ret == LZMA_OK || ret == LZMA_STREAM_END;
		size_t nBytesToWrite = outBuf.size() - lzma_strm.avail_out;
		if (alive && nBytesToWrite > 0)
			alive = oStream.write((char*)outBuf.data(), nBytesToWrite).good();
		if ((!finish && lzma_strm.avail_out > 0) || (finish && ret == LZMA_STREAM_END))
			break;
	}
	return alive;
}

TrXzFileReader::TrXzFileReader(std::istream& iStream)
{
	TrXzFooter footer;
	bool alive = iStream.seekg(-(int)sizeof(TrXzFooter), std::ios_base::end)
	                 .read((char*)&footer, sizeof footer)
	                 .seekg(0)
	                 .good();
	if (!alive)
	{
		errMsg = "failed to read footer";
		return;
	}
	if (memcmp(footer.id, TR_XZ_FILE_ID, sizeof footer.id) || footer.version != TR_XZ_FILE_VERSION)
	{
		errMsg = "invalid footer";
		return;
	}

	lzma_stream lzma_strm LZMA_STREAM_INIT;

	lzma_mt mt{
	    .flags = LZMA_FAIL_FAST,
	    .threads = std::thread::hardware_concurrency(),
	    .timeout = 0,
	    .memlimit_threading = 1 << 29,
	    .memlimit_stop = 1 << 29,
	};
	lzma_ret ret = lzma_stream_decoder_mt(&lzma_strm, &mt);
	alive = ret == LZMA_OK;
	if (!alive)
	{
		errMsg = std::format("failed to initialize lzma decoder (error {})", (int)ret);
		return;
	}

	outBuf.resize(footer.numUncompressedBytes);
	lzma_strm.avail_out = outBuf.size();
	lzma_strm.next_out = outBuf.data();

	std::vector<uint8_t> inBuf(1 << 16);
	do
	{
		uint32_t nBytesToRead = MIN(inBuf.size(), footer.numCompressedBytes - lzma_strm.total_in);
		if (nBytesToRead == 0)
			break;
		alive = iStream.read((char*)inBuf.data(), nBytesToRead).good();
		if (alive)
		{
			lzma_strm.avail_in = inBuf.size();
			lzma_strm.next_in = inBuf.data();
			ret = lzma_code(&lzma_strm, LZMA_RUN);
			alive = ret == LZMA_OK || ret == LZMA_STREAM_END;
			if (!alive)
				errMsg = std::format("lzma_code error: {}", (int)ret);
		}
		else
		{
			errMsg = "file read error";
		}
	} while (alive && lzma_strm.avail_out > 0 && ret != LZMA_STREAM_END);

	if (alive && lzma_strm.total_out != outBuf.size())
	{
		errMsg = "stream ran out of bytes";
		alive = false;
	}

	if (!alive)
		outBuf = std::move(std::vector<uint8_t>{});

	lzma_end(&lzma_strm);
}

bool TrXzFileReader::ReadTo(std::span<std::byte> sp, uint32_t at)
{
	if (at + sp.size() <= outBuf.size())
	{
		memcpy(sp.data(), outBuf.data() + at, sp.size());
		return true;
	}
	return false;
}

#endif
