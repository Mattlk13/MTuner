//--------------------------------------------------------------------------//
/// Copyright 2026 Milos Tosic. All Rights Reserved.                       ///
/// License: http://www.opensource.org/licenses/BSD-2-Clause               ///
//--------------------------------------------------------------------------//

#include <MTuner_pch.h>
#include <MTuner/src/loader/capture.h>
#include <MTuner/src/loader/binloader.h>
#include <MTuner/src/loader/util.h>
#include <rbase/inc/endianswap.h>
#include <rdebug/inc/rdebug.h>

#include <thread>
#include <algorithm>
#include <vector>

#if RTM_PLATFORM_WINDOWS && RTM_COMPILER_MSVC

#pragma warning (push)
#pragma warning (disable:4211) // redefined extern to static

static bool __uncaught_exception() { return true; }
#include <ppl.h>

struct pSortOpsTime
{
	const rtm::MemoryOperation* m_base;
	pSortOpsTime(const rtm::MemoryOperation* _base) : m_base(_base) {}

	// projection (parallel_radixsort key) and comparator (parallelStableSort) over op indices
	inline uint64_t operator()(uint32_t _idx) const { return m_base[_idx].m_operationTime; }
	inline bool operator()(uint32_t _a, uint32_t _b) const { return m_base[_a].m_operationTime < m_base[_b].m_operationTime; }
};

#pragma warning (pop)

#endif // RTM_PLATFORM_WINDOWS && RTM_COMPILER_MSVC

namespace rtm {

static inline bool stackTraceCompare(uint64_t* _e1, uint64_t _c1, uint64_t* _e2, uint64_t _c2)
{
	if (_c1 != _c2)
		return false;
	const int cnt = (int)_c1;
	for (int i=0; i<cnt; ++i)
		if (_e1[i] != _e2[i])
			return false;
	return true;
}

static uint32_t getGranularityMask(uint64_t _ops)
{
	// Aim for roughly TARGET_SAMPLES timed checkpoints regardless of op count, so m_timedStats
	// stays bounded (each MemoryStatsTimed is ~1.2 KB). Granularity is a power of two (used as a
	// bit mask); coarser granularity on huge captures just means snapshot reconstruction replays
	// a few thousand more ops from the nearest checkpoint.
	const uint64_t TARGET_SAMPLES = 16384;
	uint64_t granularity = 2048;					// floor for small/medium captures (64-bit to avoid shift overflow)
	const uint64_t want = _ops / TARGET_SAMPLES;
	while (granularity < want)
		granularity <<= 1;
	return (uint32_t)(granularity - 1);
}

inline bool psTime(MemoryOperation* inOp1, MemoryOperation* inOp2)
{
	return inOp1->m_operationTime < inOp2->m_operationTime;
}

// Portable parallel stable sort: stable-sorts equal-size blocks on worker threads, then merges
// the sorted blocks with a pairwise (parallel-per-level) merge tree. Stable, matching the
// MSVC parallel_radixsort path. Falls back to std::stable_sort for small inputs / single core.
template <typename Iter, typename Less>
static void parallelStableSort(Iter _begin, Iter _end, Less _less)
{
	const size_t n = (size_t)(_end - _begin);

	unsigned hw = std::thread::hardware_concurrency();
	if (hw == 0)
		hw = 4;

	const size_t kMinPerThread = 1u << 16;	// don't spin up threads for small ranges

	size_t threads = hw;
	if ((threads <= 1) || (n < kMinPerThread * 2))
	{
		std::stable_sort(_begin, _end, _less);
		return;
	}
	if ((n / threads) < kMinPerThread)
		threads = n / kMinPerThread;

	// round down to a power of two so the merge tree is a clean binary tree
	size_t p = 1;
	while (p * 2 <= threads)
		p *= 2;
	threads = p;

	std::vector<size_t> bounds(threads + 1);
	for (size_t i = 0; i <= threads; ++i)
		bounds[i] = (n * i) / threads;

	// sort each block concurrently (this thread takes block 0)
	std::vector<std::thread> pool;
	pool.reserve(threads - 1);
	for (size_t t = 1; t < threads; ++t)
		pool.emplace_back([=]() { std::stable_sort(_begin + bounds[t], _begin + bounds[t + 1], _less); });
	std::stable_sort(_begin + bounds[0], _begin + bounds[1], _less);
	for (size_t t = 0; t < pool.size(); ++t)
		pool[t].join();

	// merge sorted blocks; merges within a level are independent, so run them in parallel
	for (size_t width = 1; width < threads; width *= 2)
	{
		std::vector<std::thread> mpool;
		for (size_t t = 0; (t + width) < threads; t += width * 2)
		{
			const size_t lo  = bounds[t];
			const size_t mid = bounds[t + width];
			const size_t hi  = bounds[((t + width * 2) < threads) ? (t + width * 2) : threads];
			mpool.emplace_back([=]() { std::inplace_merge(_begin + lo, _begin + mid, _begin + hi, _less); });
		}
		for (size_t t = 0; t < mpool.size(); ++t)
			mpool[t].join();
	}
}

static bool isLeakedBlock(const rtm::MemoryOperation* _op, rtm::MemoryOperation* _base)
{
	do {
		if (_op->m_operationType == rmem::LogMarkers::OpFree)
			return false;

		if (_op->m_chainNext == kInvalidOpIndex)
			return true;

		_op = opChainNext(_op, _base);

	} while (_op);

	return true;
}

template <uint32_t Len>
inline uint32_t	ReadString(char _string[Len], BinLoader& _loader, bool _swapEndian, uint8_t _xor = 0)
{
	uint32_t len;
	if (_loader.readVar(len) != 1)
		return 0;

	if (_swapEndian)
		len = endianSwap(len);

	if (len < Len)
	{
		_loader.read(_string, sizeof(char) * len);
		uint8_t* xBuff = (uint8_t*)_string;
		for (uint32_t i=0; i<sizeof(char)*len; i++)
			xBuff[i] = xBuff[i] ^ _xor;
		_string[len] = (char)'\0';
		return (uint32_t)(len*sizeof(char) + sizeof(uint32_t));
	}
	// Name longer than the destination buffer (corrupt/crafted capture, or an unusually long
	// name): consume the payload anyway so the stream stays aligned. Without this every later
	// read is misaligned and the rest of the capture silently misparses.
	_string[0] = (char)'\0';
	size_t toSkip = (size_t)len * sizeof(char);
	char skip[512];
	while (toSkip > 0)
	{
		const size_t chunk = (toSkip < sizeof(skip)) ? toSkip : sizeof(skip);
		if (_loader.read(skip, chunk) != 1)
			break;	// EOF / read error - parsing will fail cleanly on the next read
		toSkip -= chunk;
	}
	return (uint32_t)(len*sizeof(char) + sizeof(uint32_t));
}

template <uint32_t Len>
inline uint32_t	ReadString(char16_t _string[Len], BinLoader& _loader, bool _swapEndian, uint8_t _xor = 0)
{
	uint32_t len;
	if (_loader.readVar(len) != 1)
		return 0;

	if (_swapEndian)
		len = endianSwap(len);

	if (len < Len)
	{
		_loader.read(_string, sizeof(char16_t) * len);
		uint8_t* xBuff = (uint8_t*)_string;
		for (uint32_t i=0; i<sizeof(char16_t)*len; i++)
			xBuff[i] = xBuff[i] ^ _xor;
		_string[len] = (char16_t)'\0';
		return (uint32_t)(len*sizeof(char16_t) + sizeof(uint32_t));
	}
	// See the char overload: skip the over-long payload so the stream stays aligned.
	_string[0] = (char16_t)'\0';
	size_t toSkip = (size_t)len * sizeof(char16_t);
	char skip[512];
	while (toSkip > 0)
	{
		const size_t chunk = (toSkip < sizeof(skip)) ? toSkip : sizeof(skip);
		if (_loader.read(skip, chunk) != 1)
			break;	// EOF / read error - parsing will fail cleanly on the next read
		toSkip -= chunk;
	}
	return (uint32_t)(len*sizeof(char16_t) + sizeof(uint32_t));
}

static inline uintptr_t calcGroupHash(MemoryOperation* _op)
{
	// The stack-trace index is a 1:1 stand-in for the old pointer when grouping ops by call stack.
	return (uintptr_t)_op->m_stackTraceIndex;
}

static inline void addHeap(HeapsType& _heaps, uint64_t _heap)
{
	if (_heaps.find(_heap) == _heaps.end())
		_heaps[_heap] = "";
}

static inline bool isLeaked(MemoryOperation* _op)
{
	bool isFreed = _op->m_operationType == rmem::LogMarkers::OpFree;
	isFreed = isFreed || ((_op->m_operationType == rmem::LogMarkers::OpRealloc) && (_op->m_allocSize == 0));
	isFreed = isFreed || ((_op->m_operationType == rmem::LogMarkers::OpReallocAligned) && (_op->m_allocSize == 0));
	return !isFreed;
}

static inline void updateLiveBlocks(MemoryOperation* _op, uint64_t& _liveBlocks)
{
	switch (_op->m_operationType)
	{
	case rmem::LogMarkers::OpAlloc:
	case rmem::LogMarkers::OpCalloc:
	case rmem::LogMarkers::OpAllocAligned:
		++_liveBlocks;
		break;
	case rmem::LogMarkers::OpRealloc:
	case rmem::LogMarkers::OpReallocAligned:
		if (!_op->m_hasPreviousPointer)
			++_liveBlocks;
		else if (_op->m_allocSize == 0)
			--_liveBlocks;
		break;
	case rmem::LogMarkers::OpFree:
		--_liveBlocks;
		break;
	};
}

static inline void updateLiveSize(MemoryOperation* _op, uint64_t& _liveSize, MemoryOperation* _base)
{
	switch (_op->m_operationType)
	{
	case rmem::LogMarkers::OpAlloc:
	case rmem::LogMarkers::OpCalloc:
	case rmem::LogMarkers::OpAllocAligned:
		_liveSize += _op->m_allocSize;
		break;
	case rmem::LogMarkers::OpRealloc:
	case rmem::LogMarkers::OpReallocAligned:
		_liveSize += _op->m_allocSize;
		if (_op->m_hasPreviousPointer && _op->m_chainPrev != kInvalidOpIndex)
			_liveSize -= opChainPrev(_op, _base)->m_allocSize;
		break;
	case rmem::LogMarkers::OpFree:
		if (_op->m_chainPrev != kInvalidOpIndex)
			_liveSize -= opChainPrev(_op, _base)->m_allocSize;
		break;
	};
}

//--------------------------------------------------------------------------
/// Address space reserved for the operations arena. Only touched pages are
/// committed (lazily, as the bump pointer advances), so this large reservation
/// costs virtual address space - not RAM - until ops actually stream in.
/// 64 GiB / sizeof(MemoryOperation) is far above any realistic capture; the
/// loader is 64-bit so the reservation always fits the address space.
//--------------------------------------------------------------------------
static const uint64_t s_operationArenaReserve = 64ull << 30;

//--------------------------------------------------------------------------
/// Address space for the stack-trace arena. Records are variable-size and far
/// fewer than ops (one per unique call stack), so a smaller lazy reservation
/// is plenty; only touched pages are committed.
//--------------------------------------------------------------------------
static const uint64_t s_stackTraceArenaReserve = 16ull << 30;

//--------------------------------------------------------------------------
/// Bump-allocates one MemoryOperation from the contiguous ops arena, creating
/// the arena on first use. Freshly committed VM pages are zero-filled, so the
/// returned op is zero-initialized (matching the old ChunkAllocator). Returns
/// NULL only if the reservation is exhausted.
//--------------------------------------------------------------------------
MemoryOperation* Capture::allocOperation()
{
	if (!rgArenaIsValid(&m_operationArena))
	{
		// NOTE: deliberately NOT using RGM_ARENA_FLAG_HUGE_PAGES here. On Windows huge pages are
		// eager-committed across the *entire* reservation, which on this large over-reservation
		// would physically back tens of GiB of RAM whenever the caller holds SeLockMemoryPrivilege.
		// Lazy normal pages keep commit proportional to the actual op count.
		if (rgArenaCreate(&m_operationArena, s_operationArenaReserve) != 0)
			return NULL;
	}
	MemoryOperation* op = (MemoryOperation*)rgArenaAlloc(&m_operationArena, sizeof(MemoryOperation));
	if (op && !m_operationBase)
	{
		// First op anchors the index space; sizeof is a multiple of the 16-byte alloc alignment,
		// so ops stay contiguous. Point every Capture-owned op list at this base now, before any
		// of them is populated, so their push_back can encode op->index.
		m_operationBase = op;
		m_operations.setBase(op);
		m_operationsInvalid.setBase(op);
		m_memoryLeaks.setBase(op);
		m_filter.m_operations.setBase(op);
	}
	if (op)
		++m_operationCount;
	return op;
}

//--------------------------------------------------------------------------
/// Bump-allocates a variable-size StackTrace from the stack-trace arena (8-byte
/// aligned for its uint64 frame array), creating the arena on first use. Fresh
/// committed pages are zero-filled, which StackTrace::init relies on (it does
/// not clear m_addedToTree). Returns NULL only if the reservation is exhausted.
//--------------------------------------------------------------------------
void* Capture::allocStackTrace(uint32_t _size)
{
	if (!rgArenaIsValid(&m_stackTraceArena))
	{
		if (rgArenaCreate(&m_stackTraceArena, s_stackTraceArenaReserve) != 0)
			return NULL;
	}
	return rgArenaAllocAligned(&m_stackTraceArena, _size, 8);
}

//--------------------------------------------------------------------------
/// Maps an allocator handle to a small index, assigning a new one the first
/// time a handle is seen. Allocators are few, so the index fits in 16 bits
/// and lets MemoryOperation store m_allocatorIndex instead of a 64-bit handle.
//--------------------------------------------------------------------------
uint32_t Capture::internHeap(uint64_t _handle)
{
	ankerl::unordered_dense::map<uint64_t, uint32_t>::iterator it = m_heapHandleToIndex.find(_handle);
	if (it != m_heapHandleToIndex.end())
		return it->second;

	const uint32_t index = (uint32_t)m_heapHandles.size();
	m_heapHandles.push_back(_handle);
	m_heapHandleToIndex[_handle] = index;
	return index;
}

//--------------------------------------------------------------------------
/// Maps a thread ID to a small index, assigning a new one the first time a
/// thread is seen. Threads are few, so the index fits in 32 bits and lets
/// MemoryOperation store m_threadIndex instead of a 64-bit thread ID.
//--------------------------------------------------------------------------
uint32_t Capture::internThread(uint64_t _threadID)
{
	ankerl::unordered_dense::map<uint64_t, uint32_t>::iterator it = m_threadIdToIndex.find(_threadID);
	if (it != m_threadIdToIndex.end())
		return it->second;

	const uint32_t index = (uint32_t)m_threadIds.size();
	m_threadIds.push_back(_threadID);
	m_threadIdToIndex[_threadID] = index;
	return index;
}

//--------------------------------------------------------------------------
/// Capture constructor
//--------------------------------------------------------------------------
Capture::Capture()
{
	m_loadProgressCallback		= NULL;
	m_loadProgressCustomData	= NULL;

	clearData();
}

//--------------------------------------------------------------------------
/// Capture destructor
//--------------------------------------------------------------------------
Capture::~Capture()
{
	clearData();
}

//--------------------------------------------------------------------------
/// Clears all previously loaded data
//--------------------------------------------------------------------------
void Capture::clearData()
{
	m_filteringEnabled	= false;
	m_swapEndian		= false;
	m_64bit				= false;

	m_loadedFile.clear();
	// Release the ops arena's VM reservation; allocOperation() lazily recreates it on the next
	// load. Recreating (rather than rgArenaClear) guarantees freshly-committed, zero-filled pages
	// per capture - matching the old ChunkAllocator, whose value-initialized chunks zeroed each op.
	rgArenaDestroy(&m_operationArena);
	m_operationBase = nullptr;
	m_operationCount = 0;
	m_operationRowMapping = {};
	rgArenaDestroy(&m_stackTraceArena);	// recreate fresh per load -> zero-filled pages for StackTrace::init
	m_operations.clear();
	m_operationsInvalid.clear();
	m_memoryLeaks.clear();
	m_statsGlobal.reset();
	m_statsSnapshot.reset();

	// symbols

	m_moduleInfos.clear();

	// -----

	m_stackTracesHash.clear();
	m_stackTraces.clear();
	m_timedStats.clear();

	m_minTime = 0;
	m_maxTime = 0;

	m_filter.m_minTimeSnapshot	= 0;
	m_filter.m_maxTimeSnapshot	= 0;
	m_filter.m_histogramIndex	= 0xffffffff;
	m_filter.m_tagHash			= 0;
	m_filter.m_threadID			= 0;
	m_filter.m_leakedOnly		= false;

	m_usageGraph.clear();
	m_usageGraphStride = 1;

	m_memoryMarkers.clear();
	m_memoryMarkerTimes.clear();

	m_Heaps.clear();
	m_heapHandles.clear();
	m_heapHandleToIndex.clear();
	m_threadIds.clear();
	m_threadIdToIndex.clear();
	m_threadNames.clear();
	m_stackTraceStats.clear();
	m_stackTracePtrToIndex.clear();
	m_stackTraceStatsBuilt = false;
	m_loadPrevPointers.clear();
	m_currentHeap = (uint64_t)-1;
	m_currentModule  = 0;

	tagTreeDestroy(m_tagTree);
	m_stackTraceTree.reset();
}

inline static uint32_t getStackTraceAndFramesSize(uint32_t numFrames)
{
	// StackTrace::m_frames[1] is already included in sizeof(StackTrace), so we need to add (numFrames*2-1) frames
	return (uint32_t)(sizeof(StackTrace) + (numFrames * 2 - 1) * sizeof(uint64_t));
}

inline static uint32_t getStackTraceNextArraySize(uint32_t numFrames)
{
	// The next array is indexed [0..numFrames]: index 0 holds the root list and
	// indices 1..numFrames hold the per-node list for each frame depth, so we
	// need numFrames+1 entries (see addToTree).
	return (uint32_t)(sizeof(StackTrace*) * (numFrames + 1));
}

inline uint32_t StackTrace::calculateSize(uint32_t numFrames)
{
	return	getStackTraceAndFramesSize(numFrames) +
			getStackTraceNextArraySize(numFrames);
}

inline void StackTrace::init(StackTrace* st, uint32_t numFrames)
{
	// m_numFrames must be set first: getNextArray() derives its offset from it.
	st->m_numFrames = numFrames;
	// The next array holds numFrames+1 entries (see getStackTraceNextArraySize).
	rtm::memSet(StackTrace::getNextArray(st), 0, sizeof(StackTrace*) * (numFrames + 1));
}

StackTrace** StackTrace::getNextArray(StackTrace* st)
{
	uint32_t offset = getStackTraceAndFramesSize(st->m_numFrames);
	return (StackTrace**)(((uint8_t*)(st)) + offset);
}

//--------------------------------------------------------------------------
#define VERIFY_READ_SIZE(x)				\
	if (1 != loader.readVar(x))			\
	{									\
		loadSuccess = false;			\
		break;							\
	}

#define VERIFY_MARKER(m,v)				\
	if (m != v)							\
	{									\
		loadSuccess = false;			\
		break;							\
	}

Capture::LoadResult Capture::loadBin(const char* _path)
{
	clearData();

	m_loadedFile = _path;

#if RTM_PLATFORM_WINDOWS
	rtm::MultiToWide path(_path);
	FILE* f  = _wfopen(path.m_ptr, L"rb");
#else
	FILE *f = fopen(_path, "rb");
#endif

	if (!f)
		return Capture::LoadFail;

#if RTM_PLATFORM_WINDOWS
	_fseeki64(f, 0, SEEK_END);
	uint64_t fileSize = (uint64_t)_ftelli64(f);
	_fseeki64(f, 0, SEEK_SET);
#elif RTM_PLATFORM_LINUX
	fseeko64(f, 0, SEEK_END);
	uint64_t fileSize = (uint64_t)ftello64(f);
	fseeko64(f, 0, SEEK_SET);
#elif RTM_PLATFORM_OSX
	fseeko(f, 0, SEEK_END);
	uint64_t fileSize = (uint64_t)ftello(f);
	fseeko(f, 0, SEEK_SET);
#else
	#error "Not implemented for target platform!"
#endif

	uint32_t compressSignature;
	if (!fread(&compressSignature, 1, sizeof(uint32_t), f))
	{
		fclose(f);		// not yet handed to a BinLoader, close it ourselves
		return Capture::LoadFail;
	}

#if RTM_PLATFORM_WINDOWS
	_fseeki64(f, 0, SEEK_SET);
#elif RTM_PLATFORM_LINUX
	fseeko64(f, 0, SEEK_SET);
#elif RTM_PLATFORM_OSX
	fseeko(f, 0, SEEK_SET);
#endif

	bool isCompressed = ((compressSignature == 0x23234646) || compressSignature == endianSwap(uint32_t(0x23234646)));

	BinLoader loader(f, isCompressed);

	uint64_t fileSizeOver100 = fileSize/100;

	uint8_t endianess;
	uint8_t pointerSize;
	uint8_t verHigh;
	uint8_t verLow;
	uint8_t toolChain;
	uint64_t cpuFrequency;

	size_t headerItems = 0;
	headerItems += loader.readVar(endianess);
	headerItems += loader.readVar(pointerSize);
	headerItems += loader.readVar(verHigh);
	headerItems += loader.readVar(verLow);
	headerItems += loader.readVar(toolChain);
	headerItems += loader.readVar(cpuFrequency);

	if (headerItems != 6)
		return Capture::LoadFail;

	// The capture format changed in v1.4 ('Add' stack-trace records carry their 32-bit hash, and
	// compressed chunks are record-aligned). Older captures are not supported - re-capture.
	// v1.5 adds optional ThreadName records; it is otherwise identical, so v1.4 captures still
	// load (they simply carry no thread names).
	if ((verHigh != 1) || ((verLow != 4) && (verLow != 5)))
	{
		if (m_loadProgressCallback)
			m_loadProgressCallback(m_loadProgressCustomData, 100.0f, "Unsupported capture version - please re-capture with this build of MTuner.");
		return Capture::LoadFail;
	}

#if RTM_LITTLE_ENDIAN
	m_swapEndian	= (endianess == 0xff) ? true : false;
#else
	m_swapEndian	= (endianess == 0xff) ? false : true;
#endif

	m_64bit			= (pointerSize == 64) ? true : false;
	m_toolchain		= (rmem::ToolChain::Enum)toolChain;

	if (m_swapEndian)
		cpuFrequency = endianSwap(cpuFrequency);
	m_CPUFrequency = cpuFrequency;

	printf("Load bin:\n  version %d.%d\n  %s endian\n  %sbit\n",
			verHigh,
			verLow,
			m_swapEndian ? "Big" : "Little",
			m_64bit ? "64" : "32" );

	if (!loadModuleInfo(loader, fileSize))
	{
		clearData();
		return Capture::LoadFail;
	}

	bool loadSuccess = true;

	ankerl::unordered_dense::map<uint64_t, std::vector<uint32_t>>  perThreadTagStack;

	uint64_t minMarkerTime		= (uint64_t)-1;
	int64_t  filePos			= 0;
	uint64_t fileEntries		= 0;
	uint64_t fileProgress		= 1;

	for (;loadSuccess;)
	{
		if (loader.eof())
			break;

		++fileEntries;
		uint64_t newFileProgress = fileEntries >> 16;

		uint8_t	marker;
		if (loader.readVar(marker) == 0)
			break;

		if (newFileProgress != fileProgress)
		{
			fileProgress = newFileProgress;

			filePos = (int64_t)loader.fileTell();
			if (m_loadProgressCallback)
			{
				float percent = float(filePos) / fileSizeOver100;
				m_loadProgressCallback(m_loadProgressCustomData, percent, "Loading capture file...");
			}
		}

		switch (marker)
		{
			case rmem::LogMarkers::OpAlloc:
			case rmem::LogMarkers::OpAllocAligned:
			case rmem::LogMarkers::OpCalloc:
			case rmem::LogMarkers::OpFree:
			case rmem::LogMarkers::OpRealloc:
			case rmem::LogMarkers::OpReallocAligned:
				{
					// read memory op
					MemoryOperation* op = allocOperation();
					if (!op)
					{
						loadSuccess = false;
						break;
					}

					// The wire format stores a full 64-bit allocator handle and thread ID; the op
					// only keeps small indices into m_heapHandles / m_threadIds, assigned once the
					// raw values are read & endian-swapped below.
					uint64_t allocatorHandle = 0;
					uint64_t threadID = 0;
					uint64_t previousPointer = 0;	// realloc-only; the op keeps just a flag, the value goes to the transient load map
					if (loader.readVar(allocatorHandle) != 1)
					{
						loadSuccess = false;
						break;
					}

					op->m_operationType	= marker;
					op->m_alignment		= 255;

					uint8_t bitIndex;
					size_t itemsRead = 0;

					switch (marker)
					{
						case rmem::LogMarkers::OpAlloc:
						case rmem::LogMarkers::OpCalloc:
							itemsRead += loader.readVar(threadID);
							if (m_64bit)
								itemsRead += loader.readVar(op->m_pointer);
							else
							{
								uint32_t ptr;
								itemsRead += loader.readVar(ptr);
								op->m_pointer = ptr;
							}
							itemsRead += loader.readVar(op->m_operationTime);
							itemsRead += loader.readVar(op->m_allocSize);
							itemsRead += loader.readVar(op->m_overhead);

							loadSuccess = itemsRead == 5;
							break;

						case rmem::LogMarkers::OpRealloc:
							itemsRead += loader.readVar(threadID);
							if (m_64bit)
							{
								itemsRead += loader.readVar(op->m_pointer);
								itemsRead += loader.readVar(previousPointer);
							}
							else
							{
								uint32_t ptr;
								itemsRead += loader.readVar(ptr);
								op->m_pointer = ptr;
								itemsRead += loader.readVar(ptr);
								previousPointer = ptr;
							}
							itemsRead += loader.readVar(op->m_operationTime);
							itemsRead += loader.readVar(op->m_allocSize);
							itemsRead += loader.readVar(op->m_overhead);

							loadSuccess = itemsRead == 6;
							break;

						case rmem::LogMarkers::OpAllocAligned:
							itemsRead += loader.readVar(threadID);
							if (m_64bit)
								itemsRead += loader.readVar(op->m_pointer);
							else
							{
								uint32_t ptr;
								itemsRead += loader.readVar(ptr);
								op->m_pointer = ptr;
							}
							itemsRead += loader.readVar(op->m_operationTime);
							itemsRead += loader.readVar(bitIndex);
							op->m_alignment = bitIndex;
							itemsRead += loader.readVar(op->m_allocSize);
							itemsRead += loader.readVar(op->m_overhead);

							loadSuccess = itemsRead == 6;
							break;

						case rmem::LogMarkers::OpFree:
							itemsRead += loader.readVar(threadID);
							if (m_64bit)
								itemsRead += loader.readVar(op->m_pointer);
							else
							{
								uint32_t ptr;
								itemsRead += loader.readVar(ptr);
								op->m_pointer = ptr;
							}
							itemsRead += loader.readVar(op->m_operationTime);

							loadSuccess = itemsRead == 3;
							break;

						case rmem::LogMarkers::OpReallocAligned:
							itemsRead += loader.readVar(threadID);
							if (m_64bit)
							{
								itemsRead += loader.readVar(op->m_pointer);
								itemsRead += loader.readVar(previousPointer);
							}
							else
							{
								uint32_t ptr;
								itemsRead += loader.readVar(ptr);
								op->m_pointer = ptr;
								itemsRead += loader.readVar(ptr);
								previousPointer = ptr;
							}
							itemsRead += loader.readVar(op->m_operationTime);
							itemsRead += loader.readVar(bitIndex);
							itemsRead += loader.readVar(op->m_allocSize);
							itemsRead += loader.readVar(op->m_overhead);

							op->m_alignment = bitIndex;
							loadSuccess = itemsRead == 7;
							break;
					};

					if (!loadSuccess)
						break;

					if (m_swapEndian)
					{
						allocatorHandle				= endianSwap(allocatorHandle);
						threadID					= endianSwap(threadID);
						op->m_operationTime			= endianSwap(op->m_operationTime);
						op->m_allocSize				= endianSwap(op->m_allocSize);
						op->m_overhead				= endianSwap(op->m_overhead);

						if (m_64bit)
						{
							op->m_pointer			= endianSwap(op->m_pointer);
							previousPointer			= endianSwap(previousPointer);
						}
						else
						{
							uint32_t actualPtr		= (uint32_t)op->m_pointer;
							actualPtr				= endianSwap(actualPtr);
							op->m_pointer			= (uint64_t)actualPtr;

							actualPtr				= (uint32_t)previousPointer;
							actualPtr				= endianSwap(actualPtr);
							previousPointer			= (uint64_t)actualPtr;
						}
					}

					uint64_t backTrace64[512];
					uint32_t backTrace32[512];

					//
					// handle stack trace compression/hashing
					//

					uint32_t stackTraceHash = 0;
					uint32_t numFrames32 = 0;
					uint16_t numFrames16 = 0;

					// read back trace
					uint8_t stackTraceTag;
					VERIFY_READ_SIZE(stackTraceTag)

					if (stackTraceTag == rmem::EntryTags::Exists)
					{
						VERIFY_READ_SIZE(stackTraceHash)
					}
					else
					if (stackTraceTag == rmem::EntryTags::Add)
					{
						VERIFY_READ_SIZE(stackTraceHash)	// v1.4: Add records carry their hash, before the frame count
						VERIFY_READ_SIZE(numFrames16)
					}
					else
					{
						loadSuccess = false;
						break;
					}

					if (m_swapEndian)
						numFrames16 = endianSwap(numFrames16);

					numFrames32 = numFrames16;
					if (numFrames32 > 512)
					{
						loadSuccess = false;
						break;
					}

					if (m_swapEndian && stackTraceHash)
						stackTraceHash = endianSwap(stackTraceHash);

					uint32_t stIndex = 0xffffffff;	// resolved index into m_stackTraces (0xffffffff = none)

					if (stackTraceTag == rmem::EntryTags::Add)
					{
						if (m_64bit)
						{
							for (uint32_t i=0; i<numFrames32; ++i)
								if (loader.readVar(backTrace64[i]) != 1)
								{
									loadSuccess = false;
									break;
								}

							if (!loadSuccess)
								break;

							if (m_swapEndian)
								for (uint32_t i=0; i<numFrames32; i++)
									backTrace64[i] = endianSwap(backTrace64[i]);
						}
						else
						{
							for (uint32_t i=0; i<numFrames32; ++i)
								if (loader.readVar(backTrace32[i]) != 1)
								{
									loadSuccess = false;
									break;
								}

							if (!loadSuccess)
								break;

							if (m_swapEndian)
								for (uint32_t i=0; i<numFrames32; i++)
									backTrace32[i] = endianSwap(backTrace32[i]);

							for (uint32_t i=0; i<numFrames32; i++)
								backTrace64[i] = (uint64_t)backTrace32[i];
						}

						// stackTraceHash was read straight from the record (v1.4) - no recompute.

						bool allocateAndAdd = true;

						StackTraceHashType::iterator it = m_stackTracesHash.find(stackTraceHash);
						if (it != m_stackTracesHash.end())
						{
							StackTrace* s = m_stackTraces[it->second];
							if (stackTraceCompare(s->m_frames, s->m_numFrames, backTrace64, numFrames32))
							{
								allocateAndAdd = false;
								stIndex = it->second;
							}
						}

						if (allocateAndAdd)
						{
							StackTrace* s = (StackTrace*)allocStackTrace(StackTrace::calculateSize(numFrames32));
							if (!s)
							{
								loadSuccess = false;
								break;
							}
							StackTrace::init(s, numFrames32);
							memcpy(&s->m_frames[0], backTrace64, numFrames32 * sizeof(uint64_t));
							stIndex = (uint32_t)m_stackTraces.size();
							m_stackTracesHash[stackTraceHash] = stIndex;
							m_stackTraces.push_back(s);
						}
					}
					else
					{
						// Stack trace exists - resolve its index
						StackTraceHashType::iterator it = m_stackTracesHash.find(stackTraceHash);
						if (it != m_stackTracesHash.end())
							stIndex = it->second;
					}

					if (stIndex == 0xffffffff)
					{
						loadSuccess = false;
						break;
					}

					// get tag for this operation
					uint32_t tag = 0;
					if (isAlloc(op->m_operationType))
					{
						std::vector<uint32_t>& tagStack = perThreadTagStack[threadID];
						const size_t ss = tagStack.size();
						if (ss)
							tag = tagStack[ss-1];
					}

					// fill the rest of mem op struct
					op->m_stackTraceIndex = stIndex;
					op->m_chainPrev  = kInvalidOpIndex;
					op->m_chainNext  = kInvalidOpIndex;
					op->m_tag        = tag;
					op->m_isValid    = 1;
					op->m_isLeaked	 = 0;
					op->m_allocatorIndex = internHeap(allocatorHandle);
					op->m_threadIndex	 = internThread(threadID);
					op->m_hasPreviousPointer = previousPointer ? 1 : 0;
					if (previousPointer)
						m_loadPrevPointers[op] = previousPointer;	// needed only during linking; freed afterwards

					m_operations.push_back(op);

					HeapsType::iterator it = m_Heaps.find(allocatorHandle);
					if (it == m_Heaps.end())
					{
						char buff[512];
#if RTM_COMPILER_MSVC
						sprintf(buff, "0x%llx", allocatorHandle);
#else
						snprintf(buff, 512, "0x%llux", allocatorHandle);
#endif
						m_Heaps[allocatorHandle] = buff;
					}
				}
				break;

			case rmem::LogMarkers::RegisterTag:
				{
					char tagName[1024];
					char tagParentName[1024];
					uint32_t tagHash;
					uint32_t tagParentHash = 0;

					ReadString<1024>(tagName, loader, m_swapEndian);
					ReadString<1024>(tagParentName, loader, m_swapEndian);
					VERIFY_READ_SIZE(tagHash)
					if (strlen(tagParentName) != 0)
					{
						VERIFY_READ_SIZE(tagParentHash)
					}

					if (m_swapEndian)
					{
						tagHash			= endianSwap(tagHash);
						tagParentHash	= endianSwap(tagParentHash);
					}

					addMemoryTag(tagName,tagHash,tagParentHash);
				}
				break;

			case rmem::LogMarkers::EnterTag:
				{
					uint32_t tagHash;
					uint64_t threadID;

					VERIFY_READ_SIZE(tagHash)
					VERIFY_READ_SIZE(threadID)

					if (m_swapEndian)
					{
						tagHash		= endianSwap(tagHash);
						threadID	= endianSwap(threadID);
					}

					std::vector<uint32_t>& tagStack = perThreadTagStack[threadID];
					tagStack.push_back(tagHash);
				}
				break;

			case rmem::LogMarkers::LeaveTag:
				{
					uint32_t tagHash;
					uint64_t threadID;

					VERIFY_READ_SIZE(tagHash)
					VERIFY_READ_SIZE(threadID)

					if (m_swapEndian)
					{
						tagHash		= endianSwap(tagHash);
						threadID	= endianSwap(threadID);
					}

					std::vector<uint32_t>& tagStack = perThreadTagStack[threadID];
					if (!tagStack.empty())
						tagStack.pop_back();
				}
				break;

			case rmem::LogMarkers::RegisterMarker:
				{
					char markerName[1024];
					uint32_t markerNameHash;
					uint32_t markerColor;

					ReadString<1024>(markerName, loader, m_swapEndian);
					VERIFY_READ_SIZE(markerNameHash)
					VERIFY_READ_SIZE(markerColor)

					if (m_swapEndian)
					{
						markerNameHash	= endianSwap(markerNameHash);
						markerColor		= endianSwap(markerColor);
					}

					MemoryMarkerEvent me;
					me.m_name		= markerName;
					me.m_nameHash	= markerNameHash;
					me.m_color		= markerColor;
					m_memoryMarkers[markerNameHash] = me;
				}
				break;

			case rmem::LogMarkers::Marker:
				{
					uint32_t markerNameHash;
					uint64_t threadID;
					uint64_t time;

					VERIFY_READ_SIZE(markerNameHash)
					VERIFY_READ_SIZE(threadID)
					VERIFY_READ_SIZE(time)

					if (m_swapEndian)
					{
						markerNameHash	= endianSwap(markerNameHash);
						threadID		= endianSwap(threadID);
						time			= endianSwap(time);
					}

					if (minMarkerTime > time)
						minMarkerTime = time;

					MemoryMarkerTime mt;
					mt.m_threadID	= threadID;
					mt.m_time		= time;
					mt.m_eventHash	= markerNameHash;
					m_memoryMarkerTimes.push_back(mt);
				}
				break;

			case rmem::LogMarkers::Module:
				{
					uint8_t sz;
					uint64_t modBase;
					uint32_t modSize;
					uint64_t time;
					VERIFY_READ_SIZE(sz);
					char modName[1024];
					if (sz == 1)
					{
						ReadString<1024>(modName, loader, m_swapEndian);
					}
					else
					{
						char16_t modNameC[1024];
						ReadString<1024>(modNameC, loader, m_swapEndian);
						rtm::strlCpy(modName, RTM_NUM_ELEMENTS(modName), QString::fromUtf16(modNameC).toUtf8().constData());
					}

					VERIFY_READ_SIZE(modBase);
					VERIFY_READ_SIZE(modSize);
					VERIFY_READ_SIZE(time)

					if (m_swapEndian)
					{
						modBase = endianSwap(modBase);
						modSize = endianSwap(modSize);
					}

					addModule(modName, modBase, modSize, time);
				}
				break;

			case rmem::LogMarkers::ModuleUnload:
				{
					uint8_t sz;
					uint64_t modBase;
					uint32_t modSize;
					uint64_t time;
					VERIFY_READ_SIZE(sz);
					char modName[1024];
					if (sz == 1)
					{
						ReadString<1024>(modName, loader, m_swapEndian);
					}
					else
					{
						char16_t modNameC[1024];
						ReadString<1024>(modNameC, loader, m_swapEndian);
						rtm::strlCpy(modName, RTM_NUM_ELEMENTS(modName), QString::fromUtf16(modNameC).toUtf8().constData());
					}

					VERIFY_READ_SIZE(modBase);
					VERIFY_READ_SIZE(modSize);
					VERIFY_READ_SIZE(time)

					if (m_swapEndian)
					{
						modBase = endianSwap(modBase);
						modSize = endianSwap(modSize);
					}

					removeModule(modName, modBase, modSize, time);
				}
				break;

			case rmem::LogMarkers::Allocator:
				{
					char		allocatorName[1024];
					uint64_t	allocatorHandle;

					ReadString<1024>(allocatorName, loader, m_swapEndian);
					VERIFY_READ_SIZE(allocatorHandle);
					if (m_swapEndian)
						allocatorHandle = endianSwap(allocatorHandle);

					m_Heaps[allocatorHandle] = allocatorName;
				}
				break;

			case rmem::LogMarkers::ThreadName:
				{
					uint64_t	threadID;
					char		threadName[1024];

					VERIFY_READ_SIZE(threadID);
					if (m_swapEndian)
						threadID = endianSwap(threadID);
					ReadString<1024>(threadName, loader, m_swapEndian);

					// Last writer wins (a thread may be renamed during its lifetime).
					m_threadNames[threadID] = threadName;
				}
				break;

			default:
				loadSuccess = false;
				break;
		};
	}

	m_stackTracesHash.clear();

	// tolerate invalid data at the end of file
	Capture::LoadResult loadResult = Capture::LoadSuccess;
	if (loadSuccess == false)
	{
		uint64_t pos = loader.fileTell();
		if ((fileSize - pos < 1000) || (m_operations.size() > 0))
		{
			loadResult	= Capture::LoadPartial;
			loadSuccess	= true;
		}
	}

	loader.stop();	// join the background producer; the BinLoader destructor closes the file it owns

	if (loadSuccess == false)
	{
		if (m_loadProgressCallback)
			m_loadProgressCallback(m_loadProgressCustomData, 100.0f, "Error reading .MTuner file!");

		clearData();
		return Capture::LoadFail;
	}

	if (m_loadProgressCallback)
		m_loadProgressCallback(m_loadProgressCustomData, 100.0f, "Sorting...");

	pSortOpsTime psTime(m_operationBase);
#if RTM_PLATFORM_WINDOWS && RTM_COMPILER_MSVC
	concurrency::parallel_radixsort(m_operations.indices().begin(), m_operations.indices().end(), psTime);
#else
	parallelStableSort(m_operations.indices().begin(), m_operations.indices().end(), psTime);
#endif

	if (!setLinksAndRemoveInvalid(minMarkerTime))
	{
		if (m_loadProgressCallback)
			m_loadProgressCallback(m_loadProgressCustomData, 100.0f, "Invalid data in .MTuner file!");

		clearData();
		return Capture::LoadFail;
	}

	if (m_loadProgressCallback)
		m_loadProgressCallback(m_loadProgressCustomData, 100.0f, "Calculating global stats..");

	calculateGlobalStats();

	if (!verifyGlobalStats())
	{
		if (m_loadProgressCallback)
			m_loadProgressCallback(m_loadProgressCustomData, 100.0f, "Invalid data in .MTuner file!");

		clearData();
		return Capture::LoadFail;
	}

	return loadResult;
}

//--------------------------------------------------------------------------
///
//--------------------------------------------------------------------------
void Capture::setFilteringEnabled(bool inState)
{
	m_filteringEnabled = inState;
	if (m_filteringEnabled)
		calculateFilteredData();
}

//--------------------------------------------------------------------------
/// Returns true if operation is inside the filtering criteria
//--------------------------------------------------------------------------
bool Capture::isInFilter(MemoryOperation* _op)
{
	if (!_op->m_isValid)
		return false;

	if (!m_filteringEnabled)
		return true;

	if ((m_currentHeap != (uint64_t)-1) && (getHeapHandle(_op->m_allocatorIndex) != m_currentHeap))
		return false;

	if ((m_filter.m_histogramIndex != (uint32_t)-1) && (m_filter.m_histogramIndex != getHistogramBinIndex(_op->m_allocSize)))
		return false;

	if ((m_filter.m_tagHash != 0) && (m_filter.m_tagHash != _op->m_tag))
		return false;

	if ((m_filter.m_threadID != 0) && (m_filter.m_threadID != getThreadId(_op->m_threadIndex)))
		return false;

	if ((_op->m_operationTime < m_filter.m_minTimeSnapshot) ||
		(_op->m_operationTime > m_filter.m_maxTimeSnapshot))
		return false;

	if (m_currentModule)
	{
		bool moduleInStack = false;
		StackTrace* opStackTrace = getStackTraceByIndex(_op->m_stackTraceIndex);
		const uint32_t numEntries = (uint32_t)opStackTrace->m_numFrames;
		for (uint32_t i=0; i<numEntries; ++i)
		{
			rdebug::ModuleInfo info;
			if (m_currentModule->checkAddress(opStackTrace->m_frames[i]))
			{
				moduleInStack = true;
				break;
			}
		}

		if (!moduleInStack)
			return false;
	}

	if (m_filter.m_leakedOnly && !isLeaked(_op))
		return false;

	return true;
}

//--------------------------------------------------------------------------
/// Selects the bin for snapshot filtering
//--------------------------------------------------------------------------
void Capture::selectHistogramBin(uint32_t _index)
{
	if (_index != m_filter.m_histogramIndex)
	{
		m_filter.m_histogramIndex = _index;
		calculateSnapshotStats();
	}
}

//--------------------------------------------------------------------------
/// Removes the histogram bin filter
//--------------------------------------------------------------------------
void Capture::deselectHistogramBin()
{
	if (m_filter.m_histogramIndex != 0xffffffff)
	{
		m_filter.m_histogramIndex = 0xffffffff;
		calculateSnapshotStats();
	}
}

//--------------------------------------------------------------------------
/// Selects the tag for snapshot filtering
//--------------------------------------------------------------------------
void Capture::selectTag(uint32_t _tagHash)
{
	if (_tagHash != m_filter.m_tagHash)
	{
		m_filter.m_tagHash = _tagHash;
		calculateSnapshotStats();
	}
}

//--------------------------------------------------------------------------
/// Removes the tag filter
//--------------------------------------------------------------------------
void Capture::deselectTag()
{
	if (m_filter.m_tagHash != 0)
	{
		m_filter.m_tagHash = 0;
		calculateSnapshotStats();
	}
}

//--------------------------------------------------------------------------
/// Selects the thread for snapshot filtering
//--------------------------------------------------------------------------
void Capture::selectThread( uint64_t inThread )
{
	if (inThread != m_filter.m_threadID)
	{
		m_filter.m_threadID = inThread;
		calculateSnapshotStats();
	}
}

//--------------------------------------------------------------------------
/// Removes the thread filter
//--------------------------------------------------------------------------
void Capture::deselectThread()
{
	if (m_filter.m_threadID != 0)
	{
		m_filter.m_threadID = 0;
		calculateSnapshotStats();
	}
}

//--------------------------------------------------------------------------
/// Sets leaked ops condition
//--------------------------------------------------------------------------
void Capture::setLeakedOnly(bool _leaked)
{
	m_filter.m_leakedOnly = _leaked;
}

//--------------------------------------------------------------------------
/// Sets the selected snapshot rage
//--------------------------------------------------------------------------
void Capture::setSnapshot(uint64_t _minTime, uint64_t _maxTime)
{
	if (_minTime < m_minTime)
		return;

	if (_maxTime > m_maxTime)
		return;

	if ((m_filter.m_minTimeSnapshot != _minTime) ||
		(m_filter.m_maxTimeSnapshot != _maxTime))
	{
		m_filter.m_minTimeSnapshot = _minTime;
		m_filter.m_maxTimeSnapshot = _maxTime;

		calculateSnapshotStats();
	}
}

//--------------------------------------------------------------------------
/// Returns memory usage at specified time
//--------------------------------------------------------------------------
void Capture::getGraphAtTime(uint64_t _time, GraphEntry& _entry)
{
	uint32_t tIdx;
	uint32_t idx = getIndexBefore(_time,tIdx);
	uint32_t gi  = idx / m_usageGraphStride;	// the usage graph is downsampled by m_usageGraphStride
	if (gi >= m_usageGraph.size())
		gi = m_usageGraph.empty() ? 0 : (uint32_t)m_usageGraph.size() - 1;
	if (!m_usageGraph.empty())
		_entry = m_usageGraph[gi];
}

//--------------------------------------------------------------------------
/// Loads symbol information
//--------------------------------------------------------------------------
bool Capture::loadModuleInfo(BinLoader& _loader, uint64_t _fileSize)
{
	uint32_t symbolInfoSize;
	_loader.readVar(symbolInfoSize);

	if (m_swapEndian)
		symbolInfoSize = endianSwap(symbolInfoSize);

	QByteArray executablePath;

	int64_t symSize = (int64_t)symbolInfoSize;

	if (!symSize)
		return true;

	uint8_t charSize;
	_loader.readVar(charSize);

	--symSize;
	while (symSize > 0)
	{
		char16_t	exePath[1024];
		char		exePathA[1024];

		uint64_t modBase = 0;
		uint64_t modSize = 0;

		size_t bytesRead = 0;

		if (charSize == 2)
			bytesRead += ReadString<1024>(exePath, _loader, m_swapEndian, 0x23);
		else
			bytesRead += ReadString<1024>(exePathA, _loader, m_swapEndian, 0x23);

		if (bytesRead == 0)			// EOF / read failure on a truncated capture - stop, don't spin forever
			break;

		if (bytesRead == sizeof(uint32_t))
			break;

		bytesRead += sizeof(uint64_t) * _loader.readVar(modBase);
		bytesRead += sizeof(uint64_t) * _loader.readVar(modSize);

		if (charSize == 2)
			executablePath = QString::fromUtf16((const char16_t*)exePath).toUtf8();
		else
			executablePath = QString::fromUtf8((const char*)exePathA).toUtf8();

		// exePath holds up to 1023 UTF-16 units; UTF-8 expansion is up to ~3 bytes/unit, so size
		// the buffer for the worst case and still clamp defensively (a corrupt/crafted capture can
		// otherwise overflow this stack buffer).
		char pathBuffer[4096];
		size_t sz = executablePath.size();
		if (sz > sizeof(pathBuffer) - 1)
			sz = sizeof(pathBuffer) - 1;
		memcpy(pathBuffer, executablePath.data(), sz);
		pathBuffer[sz] = 0;
		rtm::pathCanonicalize(pathBuffer);

		if (m_swapEndian)
		{
			modBase = endianSwap(modBase);
			modSize = endianSwap(modSize);
		}

		addModule(pathBuffer, modBase, modSize, 0);

		if (m_loadProgressCallback)
		{
			uint64_t pos = _loader.tell();

			float percent = float(pos)*100.0f / float(_fileSize);
			char message[2048];
			rtm::strlCpy(message, RTM_NUM_ELEMENTS(message), "Loading module information ");
			rtm::strlCat(message, RTM_NUM_ELEMENTS(message), executablePath.constData());
			m_loadProgressCallback(m_loadProgressCustomData, percent, message);
		}

		symSize -= bytesRead;
	}

	return symSize == 0;
}

//--------------------------------------------------------------------------
/// Builds stack trace trees and group operations by type/call stack/size
//--------------------------------------------------------------------------
void Capture::buildAnalyzeData(uintptr_t _symResolver)
{
	RTM_ASSERT(_symResolver != 0, "Invalid symbol resolver!");

	// get stack traces unique IDs
	std::vector<StackTrace*>::iterator it  = m_stackTraces.begin();
	std::vector<StackTrace*>::iterator end = m_stackTraces.end();

	const uint32_t numStackTraces = (uint32_t)m_stackTraces.size();
	uint32_t nextProgressPoint = 0;
	uint32_t numOpsOver100 = numStackTraces/100;
	uint32_t idx = 0;

	if (!numOpsOver100) numOpsOver100 = 1; // prevent unlikely division by zero

	ankerl::unordered_dense::map<uint64_t, uint64_t> address_IDs;

	while (it != end)
	{
		if ((idx > nextProgressPoint) && m_loadProgressCallback)
		{
			nextProgressPoint += numOpsOver100;
			float percent = float(idx) / float(numOpsOver100);
			m_loadProgressCallback(m_loadProgressCustomData, percent, "Generating unique symbol IDs...");
		}

		StackTrace* st = *it;
		
		int numFrames = (int)st->m_numFrames;

		for (int i=0; i<numFrames; ++i)
		{
			auto itAdd = address_IDs.find(st->m_frames[i]);
			if (itAdd != address_IDs.end())
				st->m_frames[i + numFrames] = itAdd->second;
			else
			{
				uint64_t addID = rdebug::symbolResolverGetAddressID(_symResolver, st->m_frames[i]);
				st->m_frames[i + numFrames] = addID;
				address_IDs[st->m_frames[i]] = addID;
			}
		}

		int skip = 0;
		while (skip < numFrames && st->m_frames[skip + numFrames] == 0)
			skip++;

		// remove mtunerdll from the top of call stack
		if (skip)
		{
			const uint32_t newCount = numFrames > skip ? numFrames - skip : 1;
			for (uint32_t i=0; i<newCount; ++i)
			{
				st->m_frames[i]				= st->m_frames[i + skip];
				st->m_frames[i + newCount]	= st->m_frames[i + numFrames + skip];
			}

			st->m_numFrames = newCount;
		}

		st->m_addedToTree[StackTrace::Global] = 0;
		++it;
		++idx;
	}

	const uint32_t numOps = (uint32_t)m_operations.size();
	MemoryOperation* base = m_operationBase;

	// Phase 1 (serial): tag inheritance (each op may set its chain-next's tag, so this must run
	// in time order and complete before the tag tree is built), leak collection and the heaps
	// list. These touch shared state (op tags, m_memoryLeaks, m_Heaps), so they stay serial.
	if (m_loadProgressCallback)
		m_loadProgressCallback(m_loadProgressCustomData, 0.0f, "Building analysis data...");

	for (uint32_t i=0; i<numOps; i++)
	{
		MemoryOperation* op = m_operations[i];

		if (op->m_chainNext != kInvalidOpIndex)
		{
			MemoryOperation* chainNext = opChainNext(op, base);
			if (chainNext->m_tag == 0)
				chainNext->m_tag = op->m_tag;
		}
		else
		{
			if (isLeaked(op))
				m_memoryLeaks.push_back(op);
		}

		addHeap(m_Heaps, getHeapHandle(op->m_allocatorIndex));
	}

	// Phase 2 (parallel): the three analysis structures are independent - each thread writes a
	// distinct output (groups map / stack-trace tree + its per-stack Global scratch / tag tree)
	// and only reads the now-immutable ops, so there is no shared mutable state between them.
	std::thread tGroups([this, numOps, base]()
	{
		uint64_t liveBlocks = 0, liveSize = 0;
		for (uint32_t i=0; i<numOps; i++)
		{
			MemoryOperation* op = m_operations[i];
			updateLiveBlocks(op, liveBlocks);
			updateLiveSize(op, liveSize, base);
			addToMemoryGroups(m_operationGroups, op, liveBlocks, liveSize);
		}
	});

	std::thread tStack([this, numOps]()
	{
		for (uint32_t i=0; i<numOps; i++)
			addToStackTraceTree(m_stackTraceTree, m_operations[i], StackTrace::Global);
	});

	std::thread tTag([this, numOps, base]()
	{
		MemoryTagTree* prevTag = NULL;
		for (uint32_t i=0; i<numOps; i++)
			tagAddOp(m_tagTree, m_operations[i], prevTag, base);
	});

	tGroups.join();
	tStack.join();
	tTag.join();

	if (m_loadProgressCallback)
		m_loadProgressCallback(m_loadProgressCustomData, 100.0f, "Done!");
}

//--------------------------------------------------------------------------
/// Fills the per-call-stack aggregate cache (one op pass). Idempotent.
//--------------------------------------------------------------------------
void Capture::buildStackTraceStats()
{
	if (m_stackTraceStatsBuilt)
		return;

	const uint32_t numTraces = (uint32_t)m_stackTraces.size();
	m_stackTraceStats.assign(numTraces, StackTraceStats());
	m_stackTracePtrToIndex.clear();
	m_stackTracePtrToIndex.reserve(numTraces);
	for (uint32_t i=0; i<numTraces; ++i)
		m_stackTracePtrToIndex[m_stackTraces[i]] = i;

	const uint32_t numOps = (uint32_t)m_operations.size();
	for (uint32_t i=0; i<numOps; ++i)
	{
		MemoryOperation* op = m_operations[i];
		if (!isAlloc(op->m_operationType))
			continue;
		const uint32_t idx = op->m_stackTraceIndex;
		if (idx < numTraces)
		{
			m_stackTraceStats[idx].m_allocCount++;
			m_stackTraceStats[idx].m_totalBytes += op->m_allocSize;
		}
	}

	// Live (still-allocated) bytes come from the already-computed leak list.
	const uint32_t numLeaks = (uint32_t)m_memoryLeaks.size();
	for (uint32_t i=0; i<numLeaks; ++i)
	{
		MemoryOperation* op = m_memoryLeaks[i];
		const uint32_t idx = op->m_stackTraceIndex;
		if (idx < numTraces)
			m_stackTraceStats[idx].m_liveBytes += op->m_allocSize;
	}

	m_stackTraceStatsBuilt = true;
}

const StackTraceStats& Capture::getStackTraceStats(const StackTrace* _stackTrace)
{
	buildStackTraceStats();	// lazy: first caller pays the single op pass, later calls are O(1)

	static const StackTraceStats s_zero;
	ankerl::unordered_dense::map<const StackTrace*, uint32_t>::const_iterator it = m_stackTracePtrToIndex.find(_stackTrace);
	if (it == m_stackTracePtrToIndex.end())
		return s_zero;
	return m_stackTraceStats[it->second];
}

//--------------------------------------------------------------------------
/// Links operations that are performed on the same address/memory block
//--------------------------------------------------------------------------
bool Capture::setLinksAndRemoveInvalid(uint64_t inMinMarkerTime)
{
	ankerl::unordered_dense::map<uint64_t, MemoryOperation*> opMap;
	uint32_t numOps = (uint32_t)m_operations.size();
	uint32_t nextProgressPoint = 0;
	uint32_t numOpsOver100 = numOps/100;
	if (!numOpsOver100) numOpsOver100 = 1; // prevent division by zero / progress spam for < 100 ops

	for (uint32_t i=0; i<numOps; i++)
	{
		MemoryOperation* op = m_operations[i];
		op->m_isValid = 1;

		if ((i > nextProgressPoint) && m_loadProgressCallback)
		{
			nextProgressPoint += numOpsOver100;
			float percent = float(i) / float(numOpsOver100);
			m_loadProgressCallback(m_loadProgressCustomData, percent, "Processing...");
		}

		RTM_ASSERT(op->m_chainPrev == kInvalidOpIndex, "");
		RTM_ASSERT(op->m_chainNext == kInvalidOpIndex, "");

		switch (op->m_operationType)
		{
		case rmem::LogMarkers::OpAlloc:
		case rmem::LogMarkers::OpCalloc:
		case rmem::LogMarkers::OpAllocAligned:
			{
				ankerl::unordered_dense::map<uint64_t, MemoryOperation*>::iterator it = opMap.find(op->m_pointer);
				if (it == opMap.end())
					opMap[op->m_pointer] = op;
				else
					op->m_isValid = 0;
			}
			break;

		case rmem::LogMarkers::OpRealloc:
		case rmem::LogMarkers::OpReallocAligned:
			{
				MemoryOperation* oldOp = 0;

				// ako postoji prethodni pointer onda mora da postoji op u mapi sa tim rezultatom - rezultat moze da bude isti
				if (op->m_hasPreviousPointer)
				{
					const uint64_t opPreviousPointer = m_loadPrevPointers[op];	// transient value stashed during parse
					ankerl::unordered_dense::map<uint64_t, MemoryOperation*>::iterator itP = opMap.find(opPreviousPointer);
					if (itP == opMap.end())
					{
						m_operationsInvalid.push_back(op);
						op->m_isValid = 0; // mora da postoji op u mapi sa tim rezultatom
					}
					else
					{
						oldOp = itP->second;
						opMap.erase(itP);
					}
				}
				else
				{
					// no previous block, there can't be a block already in the map with the same address
					ankerl::unordered_dense::map<uint64_t, MemoryOperation*>::iterator itP = opMap.find(op->m_pointer);
					if (itP != opMap.end())
					{
						m_operationsInvalid.push_back(op);
						op->m_isValid = 0; // mora da postoji op u mapi sa tim rezultatom
					}
				}

				if (oldOp)
				{
					op->m_chainPrev = opToIndex(oldOp, m_operationBase);
					oldOp->m_chainNext = opToIndex(op, m_operationBase);
				}

				// Only a valid op may become the live block at this address. Inserting an
				// invalid realloc here orphaned the real live block in the map, corrupting
				// later free linkage and producing false leaks (the alloc/free cases above
				// already skip the map on invalid).
				if (op->m_isValid)
					opMap[op->m_pointer] = op;
			}
			break;

		case rmem::LogMarkers::OpFree:
			{
				ankerl::unordered_dense::map<uint64_t, MemoryOperation*>::iterator it = opMap.find(op->m_pointer);
				if (it == opMap.end())
				{
					m_operationsInvalid.push_back(op);
					op->m_isValid = 0;
				}
				else
				{
					MemoryOperation* oldOp = it->second;
					RTM_ASSERT(oldOp->m_operationType != rmem::LogMarkers::OpFree, "");

					oldOp->m_chainNext = opToIndex(op, m_operationBase);
					op->m_chainPrev = opToIndex(oldOp, m_operationBase);
					op->m_allocSize	= oldOp->m_allocSize;
					op->m_overhead	= oldOp->m_overhead;

					opMap.erase(it);
				}
			}
			break;
		};
	}

	if (m_loadProgressCallback)
		m_loadProgressCallback(m_loadProgressCustomData, 100.0f, "Removing invalid operations..");

	/// Remove invalid operations (operate on the raw index storage with a base-aware predicate)
	{
		MemoryOperation* base = m_operationBase;
		std::vector<uint32_t>& idx = m_operations.indices();
		std::vector<uint32_t>::iterator newEnd = std::remove_if(idx.begin(), idx.end(),
			[base](uint32_t _i) { return base[_i].m_isValid == 0; });
		idx.resize((size_t)(newEnd - idx.begin()));
	}

	// Linking is done; the transient previous-pointer values are no longer needed. Release them
	// so they don't add to steady-state memory (the whole point of dropping the field from the op).
	m_loadPrevPointers = {};

	// calcluate leak info
	for (auto* op : m_operations)
	{
		op->m_isLeaked = isLeakedBlock(op, m_operationBase);
	}
	// get time range
	numOps = (uint32_t)m_operations.size();
	
	if (numOps == 0)
		return false;

	m_minTime = m_operations[0]->m_operationTime;
	if (m_minTime > inMinMarkerTime)
		m_minTime = inMinMarkerTime;
	m_maxTime = m_operations[numOps-1]->m_operationTime;

	m_filter.m_minTimeSnapshot = m_minTime;
	m_filter.m_maxTimeSnapshot = m_maxTime;

	if (m_loadProgressCallback)
		m_loadProgressCallback(m_loadProgressCustomData, 100.0f, "2Processing...");

	return true;
}

rdebug::Toolchain::Type convertToolchain(rmem::ToolChain::Enum _tc)
{
	switch (_tc)
	{
	case rmem::ToolChain::Win_MSVC:		return rdebug::Toolchain::MSVC;
	case rmem::ToolChain::PS3_snc:		return rdebug::Toolchain::PS3SNC;
	case rmem::ToolChain::PS4_clang:	return rdebug::Toolchain::PS4;
	case rmem::ToolChain::PS5_clang:	return rdebug::Toolchain::PS5;
	default:							return rdebug::Toolchain::GCC;
	};
}

//--------------------------------------------------------------------------
/// Adds module to list of infos
//--------------------------------------------------------------------------
void Capture::addModule(const char* _path, uint64_t inModBase, uint64_t inModSize, uint64_t inTimeStamp)
{
	char exePath[1024];
	rtm::strlCpy(exePath, RTM_NUM_ELEMENTS(exePath), _path);
	
	const char* moduleName = rtm::strStr(exePath, "/");
	if (!moduleName)
		moduleName = rtm::strStr(exePath, "\\");

	const char* nextSlash = 0;
	do {
		if (moduleName)
		{
			nextSlash = rtm::strStr(moduleName, "/");
			if (!nextSlash)
				nextSlash = rtm::strStr(moduleName, "\\");
		}
		if (nextSlash != NULL)
			moduleName = nextSlash+1;
	} while (nextSlash);

	if (moduleName == NULL)
		return;

	size_t numModules = m_moduleInfos.size();
	for (size_t i=0; i<numModules; i++)
	{
		rdebug::ModuleInfo& info = m_moduleInfos[i];
		if (rtm::strCmp(rtm::pathGetFileName(info.m_modulePath), rtm::pathGetFileName(_path)) == 0)
		{
			// update base address if new one encountered
			info.m_baseAddress		= inModBase;
			info.m_size				= inModSize;
			return;
		}
	}

	rdebug::ModuleInfo info;
	info.m_baseAddress		= inModBase;
	info.m_size				= inModSize;
	info.m_loadTime			= inTimeStamp;
	info.m_unloadTime		= 0xffffffffffffffffUL;
	info.m_toolchain.m_type	= convertToolchain(m_toolchain);
	rtm::strlCpy(info.m_modulePath, RTM_NUM_ELEMENTS(info.m_modulePath), _path);

	m_moduleInfos.push_back(info);
}

void Capture::removeModule(const char* _path, uint64_t inModBase, uint64_t inModSize, uint64_t inTimeStamp)
{
	for (auto& module : m_moduleInfos)
		if (rtm::strCmp(module.m_modulePath, _path) == 0)
		{
			// try and match the correct module

			if (inTimeStamp < module.m_loadTime)				// should never happen
				continue;

			if (module.m_unloadTime != 0xffffffffffffffffUL)	// already unloaded
				continue;

			if (module.m_baseAddress != inModBase)
				continue;

			if (module.m_size != inModSize)
				continue;

			module.m_unloadTime = inTimeStamp;
			return;
		}
}

//--------------------------------------------------------------------------
/// Calculates statistics for entire binary
//--------------------------------------------------------------------------
void Capture::calculateGlobalStats()
{
	if (m_loadProgressCallback)
		m_loadProgressCallback(m_loadProgressCustomData, 100.0f, "Calculating stats...");

	memset(&m_statsGlobal, 0, sizeof(MemoryStats));
	
	MemoryStatLocalPeak localPeak;
	memset(&localPeak, 0, sizeof(MemoryStatLocalPeak));

	const size_t numOps = m_operations.size();

	uint32_t timedGranularityMask = getGranularityMask(numOps);

	// Downsample the usage graph so it stays ~screen-resolution instead of one 16-byte entry per
	// op (gigabytes on large captures). getGraphAtTime() divides the op index by this stride.
	const uint64_t kUsageGraphCap = 1u << 20;	// ~1M samples max (~16 MB)
	m_usageGraphStride = (uint32_t)((numOps + kUsageGraphCap - 1) / kUsageGraphCap);
	if (m_usageGraphStride == 0)
		m_usageGraphStride = 1;
	m_usageGraph.reserve(numOps / m_usageGraphStride + 2);

	for (size_t i=0; i<numOps; i++)
	{
		MemoryOperation* op = m_operations[i];

		if ((i & timedGranularityMask) == 0)
		{
			MemoryStatsTimed st;
			st.m_time			= op->m_operationTime;
			st.m_operationIndex	= (uint32_t)i;
			st.m_localPeak		= localPeak;
			st.m_stats			= m_statsGlobal;
			m_timedStats.emplace_back(st);

			// reset local peak structure
			memset(&localPeak, 0, sizeof(MemoryStatLocalPeak));
		}

		++m_statsGlobal.m_numberOfOperations;

		switch (op->m_operationType)
		{
		case rmem::LogMarkers::OpAlloc:
		case rmem::LogMarkers::OpCalloc:
		case rmem::LogMarkers::OpAllocAligned:
			{
				const uint32_t binIdx = fillStats_Alloc(op, m_statsGlobal);

				// update local peak struct
				localPeak.m_memoryUsagePeak							= qMax(localPeak.m_memoryUsagePeak, m_statsGlobal.m_memoryUsage);
				localPeak.m_overheadPeak							= qMax(localPeak.m_overheadPeak, m_statsGlobal.m_overhead);
				localPeak.m_numberOfLiveBlocksPeak					= qMax(localPeak.m_numberOfLiveBlocksPeak, m_statsGlobal.m_numberOfLiveBlocks);
				localPeak.m_HistogramPeak[binIdx].m_sizePeak		= qMax(localPeak.m_HistogramPeak[binIdx].m_sizePeak, m_statsGlobal.m_histogram[binIdx].m_size);
				localPeak.m_HistogramPeak[binIdx].m_overheadPeak	= qMax(localPeak.m_HistogramPeak[binIdx].m_overheadPeak, m_statsGlobal.m_histogram[binIdx].m_overhead);
				localPeak.m_HistogramPeak[binIdx].m_countPeak		= qMax(localPeak.m_HistogramPeak[binIdx].m_countPeak, m_statsGlobal.m_histogram[binIdx].m_count);
			}
			break;

		case rmem::LogMarkers::OpRealloc:
		case rmem::LogMarkers::OpReallocAligned:
			{
				const uint32_t binIdx = fillStats_ReAlloc(op, m_statsGlobal, m_operationBase);

				// update local peak struct
				localPeak.m_memoryUsagePeak							= qMax(localPeak.m_memoryUsagePeak, m_statsGlobal.m_memoryUsage);
				localPeak.m_overheadPeak							= qMax(localPeak.m_overheadPeak, m_statsGlobal.m_overhead);
				localPeak.m_numberOfLiveBlocksPeak					= qMax(localPeak.m_numberOfLiveBlocksPeak, m_statsGlobal.m_numberOfLiveBlocks);
				localPeak.m_HistogramPeak[binIdx].m_sizePeak		= qMax(localPeak.m_HistogramPeak[binIdx].m_sizePeak, m_statsGlobal.m_histogram[binIdx].m_size);
				localPeak.m_HistogramPeak[binIdx].m_overheadPeak	= qMax(localPeak.m_HistogramPeak[binIdx].m_overheadPeak, m_statsGlobal.m_histogram[binIdx].m_overhead);
				localPeak.m_HistogramPeak[binIdx].m_countPeak		= qMax(localPeak.m_HistogramPeak[binIdx].m_countPeak, m_statsGlobal.m_histogram[binIdx].m_count);
			}
			break;

		case rmem::LogMarkers::OpFree:
			{
				fillStats_Free(op, m_statsGlobal);
			}
			break;
		};

		if ((i % m_usageGraphStride) == 0)
		{
			GraphEntry entry;
			entry.m_usage			= m_statsGlobal.m_memoryUsage;
			entry.m_numLiveBlocks	= m_statsGlobal.m_numberOfLiveBlocks;
			m_usageGraph.emplace_back(entry);
		}
	}

	MemoryStatsTimed st;
	st.m_time		= m_operations[m_operations.size()-1]->m_operationTime;
	st.m_operationIndex	= (uint32_t)(m_operations.size()-1);
	st.m_localPeak	= localPeak;
	st.m_stats		= m_statsGlobal;
	m_timedStats.push_back(st);

	m_statsSnapshot = m_statsGlobal;

	if (m_loadProgressCallback)
		m_loadProgressCallback(m_loadProgressCustomData, 100.0f, "Loading complete!");
}

bool Capture::verifyGlobalStats()
{
	if (m_statsGlobal.m_memoryUsage		& UINT64_C(0x8000000000000000)) return false;
	if (m_statsGlobal.m_memoryUsagePeak	& UINT64_C(0x8000000000000000))	return false;
	if (m_statsGlobal.m_overhead				& UINT32_C(0x80000000))	return false;
	if (m_statsGlobal.m_overheadPeak			& UINT32_C(0x80000000))	return false;
	if (m_statsGlobal.m_numberOfOperations		& UINT32_C(0x80000000))	return false;
	if (m_statsGlobal.m_numberOfAllocations		& UINT32_C(0x80000000))	return false;
	if (m_statsGlobal.m_numberOfReAllocations	& UINT32_C(0x80000000))	return false;
	if (m_statsGlobal.m_numberOfFrees			& UINT32_C(0x80000000))	return false;
	if (m_statsGlobal.m_numberOfLiveBlocks		& UINT32_C(0x80000000))	return false;

	for (unsigned i=0; i<MemoryStats::NUM_HISTOGRAM_BINS; ++i)
	{
		if (m_statsGlobal.m_histogram[i].m_size		& UINT64_C(0x8000000000000000)) return false;
		if (m_statsGlobal.m_histogram[i].m_sizePeak	& UINT64_C(0x8000000000000000)) return false;
		if (m_statsGlobal.m_histogram[i].m_overhead			& UINT32_C(0x80000000))	return false;
		if (m_statsGlobal.m_histogram[i].m_overheadPeak		& UINT32_C(0x80000000))	return false;
		if (m_statsGlobal.m_histogram[i].m_count			& UINT32_C(0x80000000))	return false;
		if (m_statsGlobal.m_histogram[i].m_countPeak		& UINT32_C(0x80000000))	return false;
	}

	return true;
}

//--------------------------------------------------------------------------
/// Calculates filtered data
//--------------------------------------------------------------------------
void Capture::calculateFilteredData()
{
	std::vector<StackTrace*>::iterator it  = m_stackTraces.begin();
	std::vector<StackTrace*>::iterator end = m_stackTraces.end();

	const uint32_t numStackTraces = (uint32_t)m_stackTraces.size();
	uint32_t nextProgressPoint = 0;
	uint32_t numOpsOver100 = numStackTraces/100;
	uint32_t idx = 0;

	while (it != end)
	{
		StackTrace* st = *it;
		st->m_addedToTree[StackTrace::Filtered] = 0;

		++it;

		if ((idx > nextProgressPoint) && m_loadProgressCallback)
		{
			nextProgressPoint += numOpsOver100;
			float percent = float(idx) / float(numOpsOver100);
			m_loadProgressCallback(m_loadProgressCustomData, percent, "Fixing up stack traces...");
		}
		++idx;
	}


	uint32_t minTimedIdx;
	uint32_t maxTimedIdx;
	uint32_t minTimeOpIndex = getIndexBefore(m_filter.m_minTimeSnapshot, minTimedIdx);
	if (minTimeOpIndex != 0)
		minTimeOpIndex++;														// first op at/after minTime (window start)
	uint32_t maxTimeOpIndex = getIndexAfter(m_filter.m_maxTimeSnapshot, maxTimedIdx);	// first op past maxTime (exclusive end)

	if (maxTimeOpIndex > (uint32_t)m_operations.size())
		maxTimeOpIndex = (uint32_t)m_operations.size();
	if (maxTimeOpIndex < minTimeOpIndex)										// degenerate/empty selection
		maxTimeOpIndex = minTimeOpIndex;

	m_filter.m_operations.clear();
	m_filter.m_operations.reserve(maxTimeOpIndex - minTimeOpIndex);

	m_filter.m_operationGroups.clear();

	m_filter.m_stackTraceTree.reset();

	const uint32_t numOps = maxTimeOpIndex - minTimeOpIndex;
	nextProgressPoint = minTimeOpIndex;
	numOpsOver100 = numOps/100;

	(void)nextProgressPoint; (void)numOpsOver100;

	// 1) Collect the ops that pass the filter (serial; order preserved).
	for (uint32_t i=minTimeOpIndex; i<maxTimeOpIndex; i++)
	{
		MemoryOperation* op = m_operations[i];
		if (isInFilter(op))
			m_filter.m_operations.push_back(op);
	}

	if (m_loadProgressCallback)
		m_loadProgressCallback(m_loadProgressCustomData, 50.0f, "Building filtered data...");

	// 2) Build the three independent structures concurrently. Each thread writes a distinct
	// output (groups map / stack-trace tree + its per-stack Filtered scratch / tag tree) and only
	// reads the (now immutable) ops, so there is no shared mutable state between them.
	const MemoryOpArray& fops = m_filter.m_operations;
	MemoryOperation* base = m_operationBase;

	std::thread tGroups([this, &fops, base]()
	{
		uint64_t lb = 0, ls = 0;
		for (size_t k=0; k<fops.size(); ++k)
		{
			MemoryOperation* op = fops[k];
			updateLiveBlocks(op, lb);
			updateLiveSize(op, ls, base);
			addToMemoryGroups(m_filter.m_operationGroups, op, lb, ls);
		}
	});

	std::thread tStack([this, &fops]()
	{
		for (size_t k=0; k<fops.size(); ++k)
			addToStackTraceTree(m_filter.m_stackTraceTree, fops[k], StackTrace::Filtered);
	});

	std::thread tTag([this, &fops, base]()
	{
		MemoryTagTree* prevTag = NULL;
		for (size_t k=0; k<fops.size(); ++k)
			tagAddOp(m_filter.m_tagTree, fops[k], prevTag, base);
	});

	tGroups.join();
	tStack.join();
	tTag.join();

	if (m_loadProgressCallback)
		m_loadProgressCallback(m_loadProgressCustomData, 100.0f, "Done!");
}

//--------------------------------------------------------------------------
/// Returns the index of the last operation strictly before the given time
/// (or 0 if none). Callers add 1 to get the first operation at/after _time.
//--------------------------------------------------------------------------
uint32_t Capture::getIndexBefore(uint64_t _time, uint32_t& _outTimedIndex) const
{
	uint32_t tsIdx = 0;
	int32_t tsIdxMin = 0;
	int32_t tsIdxMax = (uint32_t)m_timedStats.size()-1;
	
	if (tsIdxMax == 1)
		tsIdx = 1;
	else
	{
		while (tsIdxMax > tsIdxMin)
		{
			uint32_t tsIdxMid = (tsIdxMin + tsIdxMax) / 2;

			if (m_timedStats[tsIdxMid].m_time < _time)
				tsIdxMin = tsIdxMid;
			else
				tsIdxMax = tsIdxMid;

			if (tsIdxMax-tsIdxMin == 1)
			{
				tsIdx = tsIdxMax;
				break;
			}
		}
	}

	uint32_t startIdx = m_timedStats[tsIdx-1].m_operationIndex;
	uint32_t endIdx = m_timedStats[tsIdx].m_operationIndex + 1;
	
	_outTimedIndex = tsIdx - 1;

	while (endIdx > startIdx)
	{
		uint32_t idxMid = (startIdx + endIdx) / 2;

		if (m_operations[idxMid]->m_operationTime < _time)
			startIdx = idxMid;
		else
			endIdx = idxMid;

		if (endIdx-startIdx == 1)
		{
			// Return the last operation strictly before _time (or 0 if none).
			// Previously this returned endIdx (the first op at/after _time), which
			// is one op too far - it broke getGraphAtTime (showed the next op's
			// usage) and the snapshot/filter window start (the '+1' callers add).
			if (m_operations[startIdx]->m_operationTime < _time)
				return startIdx;
			return (startIdx == 0) ? 0 : startIdx - 1;
		}
	}

	RTM_ASSERT(false, "Should not reach here!");
	return 0;
}

uint32_t Capture::getIndexAfter(uint64_t _time, uint32_t& _outTimedIndex) const
{
	uint32_t tsIdx = 0;
	int32_t tsIdxMin = 0;
	int32_t tsIdxMax = (uint32_t)m_timedStats.size()-1;

	// Mirror getIndexBefore: with only two timed checkpoints the loop can leave tsIdx==0, which
	// would then index m_timedStats[tsIdx-1] out of bounds (e.g. a snapshot at/before the first op
	// on a small capture). Force the single segment instead.
	if (tsIdxMax == 1)
		tsIdx = 1;
	else
	{
		while (tsIdxMax > tsIdxMin)
		{
			uint32_t tsIdxMid = (tsIdxMin + tsIdxMax) / 2;

			if (m_timedStats[tsIdxMid].m_time < _time)
				tsIdxMin = tsIdxMid;
			else
				tsIdxMax = tsIdxMid;

			if (tsIdxMax-tsIdxMin == 1)
			{
				tsIdx = tsIdxMax;
				break;
			}
		}
	}

	uint32_t startIdx = m_timedStats[tsIdx-1].m_operationIndex;
	uint32_t endIdx = m_timedStats[tsIdx].m_operationIndex + 1;
	
	_outTimedIndex = tsIdx - 1;

	while (endIdx > startIdx)
	{
		uint32_t idxMid = (startIdx + endIdx) / 2;

		if (m_operations[idxMid]->m_operationTime < _time)
			startIdx = idxMid;
		else
			endIdx = idxMid;
		
		if (endIdx-startIdx == 1)
		{
			if (m_operations[startIdx]->m_operationTime > _time)
				return startIdx;
			else
				return endIdx;
		}
	}

	RTM_ASSERT(false, "Should not reach here!");
	return 0;
}

//--------------------------------------------------------------------------
/// Calculates statistics for the selected time slice
//--------------------------------------------------------------------------
void Capture::calculateSnapshotStats()
{
	uint32_t minTimedIdx;
	uint32_t maxTimedIdx;
	uint32_t minTimeOpIndex = getIndexBefore(m_filter.m_minTimeSnapshot, minTimedIdx);
	uint32_t maxTimeOpIndex = getIndexAfter(m_filter.m_maxTimeSnapshot, maxTimedIdx);
	
	if (minTimeOpIndex != 0)
		minTimeOpIndex++;

	MemoryStats startStats = m_timedStats[minTimedIdx].m_stats; 
	m_statsSnapshot = startStats;

	// check if it's fully manual 
	if (maxTimedIdx - minTimedIdx < 2)
	{
		const uint32_t startIndex = m_timedStats[minTimedIdx].m_operationIndex;
		GetRangedStats(m_statsSnapshot, startIndex, minTimeOpIndex);
		m_statsSnapshot.setPeaksToCurrent();

		GetRangedStats(m_statsSnapshot, minTimeOpIndex, maxTimeOpIndex);

		m_statsSnapshot.m_numberOfOperations	-= startStats.m_numberOfOperations;
		m_statsSnapshot.m_numberOfAllocations	-= startStats.m_numberOfAllocations;
		m_statsSnapshot.m_numberOfFrees			-= startStats.m_numberOfFrees;
		m_statsSnapshot.m_numberOfReAllocations	-= startStats.m_numberOfReAllocations;
	}
	else
	{
		const uint32_t startIndex1	= m_timedStats[minTimedIdx].m_operationIndex;
		RTM_ASSERT(startIndex1 <= minTimeOpIndex, "");
		GetRangedStats(startStats, startIndex1, minTimeOpIndex);
		m_statsSnapshot = startStats;
		m_statsSnapshot.setPeaksToCurrent();
		GetRangedStats(m_statsSnapshot, minTimeOpIndex, m_timedStats[minTimedIdx+1].m_operationIndex);

		MemoryStatLocalPeak localPeak;
		localPeak.m_memoryUsagePeak = m_statsSnapshot.m_memoryUsage;
		localPeak.m_overheadPeak	= m_statsSnapshot.m_overhead;
		for (uint32_t i=0; i<MemoryStats::NUM_HISTOGRAM_BINS; i++)
		{
			localPeak.m_HistogramPeak[i].m_sizePeak		= m_statsSnapshot.m_histogram[i].m_sizePeak;
			localPeak.m_HistogramPeak[i].m_overheadPeak	= m_statsSnapshot.m_histogram[i].m_overheadPeak;
			localPeak.m_HistogramPeak[i].m_countPeak	= m_statsSnapshot.m_histogram[i].m_countPeak;
		}
		
		for (uint32_t t=minTimedIdx+2; t<=maxTimedIdx; t++)
		{
			MemoryStatLocalPeak& peakT = m_timedStats[t].m_localPeak;

			localPeak.m_memoryUsagePeak	= qMax(localPeak.m_memoryUsagePeak, peakT.m_memoryUsagePeak);
			localPeak.m_overheadPeak	= qMax(localPeak.m_overheadPeak, peakT.m_overheadPeak);

			for (uint32_t i=0; i<MemoryStats::NUM_HISTOGRAM_BINS; i++)
			{
				localPeak.m_HistogramPeak[i].m_sizePeak		= qMax(localPeak.m_HistogramPeak[i].m_sizePeak, peakT.m_HistogramPeak[i].m_sizePeak);
				localPeak.m_HistogramPeak[i].m_overheadPeak	= qMax(localPeak.m_HistogramPeak[i].m_overheadPeak, peakT.m_HistogramPeak[i].m_overheadPeak);
				localPeak.m_HistogramPeak[i].m_countPeak	= qMax(localPeak.m_HistogramPeak[i].m_countPeak, peakT.m_HistogramPeak[i].m_countPeak);
			}
		}

		m_statsSnapshot.setPeaksFrom(localPeak);
		MemoryStatsTimed& ts = m_timedStats[maxTimedIdx];
		const uint32_t startIndex2	= ts.m_operationIndex;

		m_statsSnapshot.m_memoryUsage			= ts.m_stats.m_memoryUsage;
		m_statsSnapshot.m_overhead				= ts.m_stats.m_overhead;
		m_statsSnapshot.m_numberOfOperations	= ts.m_stats.m_numberOfOperations - startStats.m_numberOfOperations;
		m_statsSnapshot.m_numberOfAllocations	= ts.m_stats.m_numberOfAllocations - startStats.m_numberOfAllocations;
		m_statsSnapshot.m_numberOfFrees			= ts.m_stats.m_numberOfFrees - startStats.m_numberOfFrees;
		m_statsSnapshot.m_numberOfReAllocations	= ts.m_stats.m_numberOfReAllocations - startStats.m_numberOfReAllocations;
		m_statsSnapshot.m_numberOfLiveBlocks	= ts.m_stats.m_numberOfLiveBlocks;

		for (uint32_t i=0; i<MemoryStats::NUM_HISTOGRAM_BINS; i++)
		{
			m_statsSnapshot.m_histogram[i].m_size		= ts.m_stats.m_histogram[i].m_size;
			m_statsSnapshot.m_histogram[i].m_overhead	= ts.m_stats.m_histogram[i].m_overhead;
			m_statsSnapshot.m_histogram[i].m_count		= ts.m_stats.m_histogram[i].m_count;
		}

		GetRangedStats(m_statsSnapshot, startIndex2, maxTimeOpIndex);	// half-open [.., maxTimeOpIndex), matching the manual branch above
	}
}

//--------------------------------------------------------------------------
/// Calculates the stats inside the given range
//--------------------------------------------------------------------------
void Capture::GetRangedStats(MemoryStats& _stats, uint32_t _minIdx, uint32_t _maxIdx)
{
	const uint32_t minIdx = _minIdx;
	const uint32_t maxIdx = _maxIdx;

	for (size_t i=minIdx; i<maxIdx; i++)
	{
		MemoryOperation* op = m_operations[i];
		
		++_stats.m_numberOfOperations;

		switch (op->m_operationType)
		{
		case rmem::LogMarkers::OpAlloc:
		case rmem::LogMarkers::OpCalloc:
		case rmem::LogMarkers::OpAllocAligned:
			fillStats_Alloc( op, _stats );
			break;

		case rmem::LogMarkers::OpRealloc:
		case rmem::LogMarkers::OpReallocAligned:
			fillStats_ReAlloc(op, _stats, m_operationBase);
			break;

		case rmem::LogMarkers::OpFree:
			fillStats_Free(op, _stats);
			break;
		};
	}
}

//--------------------------------------------------------------------------
/// Registers memory tag with the loader
//--------------------------------------------------------------------------
void Capture::addMemoryTag(char* _tagName, uint32_t _tagHash, uint32_t _parentTagHash)
{
	MemoryTagTree* mtt = new MemoryTagTree();
	mtt->m_hash		= _tagHash;
	mtt->m_name		= _tagName;
	if (!tagInsert(&m_tagTree, mtt, _parentTagHash))
		delete mtt;
}

//--------------------------------------------------------------------------
/// Adds operation to memory groups
//--------------------------------------------------------------------------
void Capture::addToMemoryGroups(MemoryGroupsHashType& _groups, MemoryOperation* _op, uint64_t _liveBlocks, uint64_t _liveSize)
{
	uintptr_t groupHash;

	switch (_op->m_operationType)
	{
		case rmem::LogMarkers::OpAlloc:
		case rmem::LogMarkers::OpCalloc:
		case rmem::LogMarkers::OpAllocAligned:
			{
				groupHash = calcGroupHash(_op);
				MemoryOperationGroup& group = _groups[groupHash];
				group.m_groupOperations.setBase(m_operationBase);
				group.m_groupOperations.push_back(_op);
				group.m_count++;
				group.m_liveCount++;

				group.m_minSize = qMin(group.m_minSize, _op->m_allocSize);
				group.m_maxSize = qMax(group.m_maxSize, _op->m_allocSize);

				group.m_liveSize += _op->m_allocSize;

				const uint32_t binIdx = getHistogramBinIndex(_op->m_allocSize);
				group.m_histogram[binIdx]++;
				group.m_histogramPeak[binIdx] = qMax(group.m_histogram[binIdx], group.m_histogramPeak[binIdx]);

				int64_t newPeakSize = qMax(group.m_peakSize, group.m_liveSize);
				if (newPeakSize > group.m_peakSize)
				{
					group.m_peakSize		= newPeakSize;
					group.m_peakSizeGlobal	= _liveSize;
				}

				uint32_t newPeakCount = qMax(group.m_liveCountPeak, group.m_liveCount);
				if (newPeakCount > group.m_liveCountPeak)
				{
					group.m_liveCountPeak		= newPeakCount;
					group.m_liveCountPeakGlobal	= _liveBlocks;
				}
			}
			break;

		case rmem::LogMarkers::OpFree:
			{
				MemoryOperation* prevOp = getChainPrev(_op);
				if (isInFilter(prevOp))
				{
					groupHash = calcGroupHash(prevOp);

					MemoryOperationGroup& prevGroup = _groups[groupHash];

					prevGroup.m_liveCount--;
					prevGroup.m_liveSize -= prevOp->m_allocSize;

					const uint32_t prevBinIdx = getHistogramBinIndex(prevOp->m_allocSize);
					prevGroup.m_histogram[prevBinIdx]--;
				}

				groupHash = calcGroupHash(_op);
				MemoryOperationGroup& group = _groups[groupHash];
				group.m_groupOperations.setBase(m_operationBase);
				group.m_groupOperations.push_back(_op);
				group.m_count++;

				group.m_minSize = qMin(group.m_minSize, _op->m_allocSize);
				group.m_maxSize = qMax(group.m_maxSize, _op->m_allocSize);

				//group.m_liveSize -= _op->m_allocSize;
				group.m_peakSize  = qMax(group.m_peakSize, group.m_liveSize);

			}
			break;

		case rmem::LogMarkers::OpReallocAligned:
		case rmem::LogMarkers::OpRealloc:
			{
				MemoryOperation* prevOp = getChainPrev(_op);
				if (prevOp)
				{
					if (isInFilter(prevOp))
					{
						groupHash = calcGroupHash(prevOp);

						MemoryOperationGroup& prevGroup = _groups[groupHash];

						prevGroup.m_liveCount--;
						prevGroup.m_liveSize -= prevOp->m_allocSize;

						const uint32_t prevBinIdx = getHistogramBinIndex(prevOp->m_allocSize);
						prevGroup.m_histogram[prevBinIdx]--;
					}
				}

				groupHash = calcGroupHash(_op);
				MemoryOperationGroup& group = _groups[groupHash];
				group.m_groupOperations.setBase(m_operationBase);
				group.m_groupOperations.push_back(_op);
				group.m_count++;

				group.m_minSize = qMin(group.m_minSize, _op->m_allocSize);
				group.m_maxSize = qMax(group.m_maxSize, _op->m_allocSize);

				if (_op->m_allocSize != 0 || !prevOp)
				{
					group.m_liveCount++;
					group.m_liveSize += _op->m_allocSize;

					int64_t newPeakSize = qMax(group.m_peakSize, group.m_liveSize);
					if (newPeakSize > group.m_peakSize)
					{
						group.m_peakSize		= newPeakSize;
						group.m_peakSizeGlobal	= _liveSize;
					}

					uint32_t newPeakCount = qMax(group.m_liveCountPeak, group.m_liveCount);
					if (newPeakCount > group.m_liveCountPeak)
					{
						group.m_liveCountPeak		= newPeakCount;
						group.m_liveCountPeakGlobal = _liveBlocks;
					}

					const uint32_t binIdx = getHistogramBinIndex(_op->m_allocSize);
					group.m_histogram[binIdx]++;
					group.m_histogramPeak[binIdx] = qMax(group.m_histogram[binIdx], group.m_histogramPeak[binIdx]);
				}
			}
			break;
	};
}

static void addToTree(StackTraceTree* _root, StackTrace* _trace, int64_t _size, int32_t _overhead, StackTrace::Scope _offset, StackTraceTree::Enum _opType, uint64_t _operationTime)
{
	const int32_t numFrames = (int32_t)_trace->m_numFrames;
	int32_t currFrame = numFrames;
	StackTraceTree* currNode = _root;

	currNode->m_memUsage	+= _size;
	currNode->m_memUsagePeak = qMax(currNode->m_memUsage, currNode->m_memUsagePeak);

	currNode->m_overhead	+= _overhead;
	currNode->m_overheadPeak = qMax(currNode->m_overhead, currNode->m_overheadPeak);

	if (_opType != StackTraceTree::Count)
		++currNode->m_opCount[_opType];

	if (currNode->m_minTime == 0)
		currNode->m_minTime = _operationTime;

	currNode->m_maxTime = _operationTime;

	// add stack trace to root node
	StackTrace** ar = StackTrace::getNextArray(_trace);
	ar[0]		= _root->m_stackTraceList;
	_root->m_stackTraceList	= _trace;

	while (--currFrame >= 0)
	{
		int32_t depth = numFrames-currFrame;

		const uint64_t	currUniqueID = _trace->m_frames[currFrame+numFrames];

		StackTraceTree* nextNode = 0;

		size_t numChildren = currNode->m_children.size();
		size_t found = numChildren;
		for (size_t i=0; i<numChildren; i++)
		{
			if (currNode->m_children[i]->m_addressID == currUniqueID)
			{
				found			= i;
				break;
			}
		}

		if (found == numChildren)
		{
			// not found
			StackTraceTree* newNode = new StackTraceTree();
			newNode->m_parent		= currNode;
			newNode->m_addressID	= currUniqueID;
			newNode->m_depth		= depth;
			currNode->m_children.emplace_back(newNode);
			nextNode = currNode->m_children[numChildren];
		}
		else
			nextNode = currNode->m_children[found];

		currNode = nextNode;

		if (_trace->m_addedToTree[_offset] < depth)
		{
			StackTrace::getNextArray(_trace)[depth] = currNode->m_stackTraceList;
			currNode->m_stackTraceList = _trace;
			_trace->m_addedToTree[_offset] = depth;
		}

		currNode->m_memUsage		+= _size;
		currNode->m_memUsagePeak	= qMax(currNode->m_memUsage, currNode->m_memUsagePeak);

		currNode->m_overhead		+= _overhead;
		currNode->m_overheadPeak	= qMax(currNode->m_overhead, currNode->m_overheadPeak);

		if (_opType != StackTraceTree::Count)
			++currNode->m_opCount[_opType];

		if (currNode->m_minTime == 0)
			currNode->m_minTime = _operationTime;

		currNode->m_maxTime = _operationTime;
	}
}

void Capture::addToStackTraceTree(StackTraceTree& _tree, MemoryOperation* _op, StackTrace::Scope _offset)
{
	switch (_op->m_operationType)
	{
		case rmem::LogMarkers::OpAlloc:
		case rmem::LogMarkers::OpCalloc:
		case rmem::LogMarkers::OpAllocAligned:
			{
				addToTree(&_tree, getStackTraceByIndex(_op->m_stackTraceIndex), _op->m_allocSize, _op->m_overhead, _offset, StackTraceTree::Alloc, _op->m_operationTime);
			}
			break;

		case rmem::LogMarkers::OpFree:
			{
				MemoryOperation* prevOp = getChainPrev(_op);
				RTM_ASSERT(prevOp != NULL, "");

				if (isInFilter(prevOp))
					addToTree(&_tree, getStackTraceByIndex(prevOp->m_stackTraceIndex), -(int64_t)prevOp->m_allocSize, -(int32_t)prevOp->m_overhead, _offset, StackTraceTree::Free, _op->m_operationTime);
				else
					// prev op not in filter, do not reduce used memory to avoid going (possibly) negative
					addToTree(&_tree, getStackTraceByIndex(prevOp->m_stackTraceIndex), 0, 0, _offset, StackTraceTree::Free, _op->m_operationTime);
			}
			break;

		case rmem::LogMarkers::OpReallocAligned:
		case rmem::LogMarkers::OpRealloc:
			{
				MemoryOperation* prevOp = getChainPrev(_op);
				if (prevOp)
				{
					if (isInFilter(prevOp))
						addToTree(&_tree, getStackTraceByIndex(prevOp->m_stackTraceIndex), -(int64_t)prevOp->m_allocSize, -(int32_t)prevOp->m_overhead, _offset, StackTraceTree::Count, _op->m_operationTime);
				}
				addToTree(&_tree, getStackTraceByIndex(_op->m_stackTraceIndex), _op->m_allocSize, _op->m_overhead, _offset, StackTraceTree::Realloc, _op->m_operationTime);
			}
			break;
	};
}

} // namespace rtm
