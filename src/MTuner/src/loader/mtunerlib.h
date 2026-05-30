//--------------------------------------------------------------------------//
/// Copyright 2026 Milos Tosic. All Rights Reserved.                       ///
/// License: http://www.opensource.org/licenses/BSD-2-Clause               ///
//--------------------------------------------------------------------------//

#ifndef RTM_MTUNER_MTUNERLIB_H
#define RTM_MTUNER_MTUNERLIB_H

#include <string>
#include <rmem/src/rmem_enums.h>
#include "../3rd/unordered_dense/include/ankerl/unordered_dense.h"

namespace rtm {

bool mtunerLoaderInit(bool _MTuner = false);
bool mtunerLoaderShutDown();

struct StackTrace;
struct MemoryStatLocalPeak;

//--------------------------------------------------------------------------
/// Methods of sorting memory operations
//--------------------------------------------------------------------------
enum eOperationSort
{
	OP_SORT_POINTER,							///< Sort by pointer value
	OP_SORT_TIME								///< Sort by operation time
};

//--------------------------------------------------------------------------
/// Methods of sorting memory operation groups
//--------------------------------------------------------------------------
enum eGroupSort
{
	GROUP_SORT_COUNT,							///< Sort by number of operations in the group
	GROUP_SORT_SIZE,							///< Sort by size of the operation in the group
	GROUP_SORT_TOTAL_SIZE						///< Sort by total size (number*size) of the group
};

//--------------------------------------------------------------------------
/// Structure adding information on top of memory operation
//--------------------------------------------------------------------------
// Fields are ordered by descending alignment so the struct packs to 48 bytes with no internal
// padding (down from the original 80). The two 64-bit fields force 8-byte alignment, so 48 is the
// natural floor; sizeof being a multiple of 16 also keeps ops contiguous in the 16-byte-aligned
// arena (chain indices rely on that). See the static_assert below.
struct MemoryOperation
{
	uint64_t			m_pointer;				//< Allocated/freed pointer
	uint64_t			m_operationTime;
	uint32_t			m_chainPrev;			//< Index of the previous op on this block (kInvalidOpIndex = none)
	uint32_t			m_chainNext;			//< Index of the next op on this block (kInvalidOpIndex = none)
	uint32_t			m_stackTraceIndex;		//< Index into Capture::m_stackTraces
	uint32_t			m_threadIndex;			//< Index into Capture::m_threadIds (thread ID table)
	uint32_t			m_allocSize;
	uint32_t			m_overhead;
	uint16_t			m_allocatorIndex;		//< Index into Capture::m_heapHandles (allocator handle table)
	uint16_t			m_tag;
	uint8_t				m_operationType			: 5;	//< rmem::LogMarkers Op* value (max 5, fits 5 bits)
	uint8_t				m_isValid				: 1;
	uint8_t				m_isLeaked				: 1;
	uint8_t				m_hasPreviousPointer	: 1;	//< realloc carried a previous pointer (value lives in Capture's transient load map)
	uint8_t				m_alignment;
};

static_assert(sizeof(MemoryOperation) == 48, "MemoryOperation must stay 48 bytes (and a multiple of 16 for arena contiguity)");

//--------------------------------------------------------------------------
/// MemoryOperation chain links and the stack-trace/thread/allocator fields are
/// stored as indices into contiguous tables instead of pointers, keeping the
/// struct small. Chain indices reference the operations arena (stable arrival
/// order); kInvalidOpIndex marks "no link".
//--------------------------------------------------------------------------
static const uint32_t kInvalidOpIndex = 0xffffffffu;

inline MemoryOperation* opChainPrev(const MemoryOperation* _op, MemoryOperation* _base)
{
	return (_op->m_chainPrev == kInvalidOpIndex) ? (MemoryOperation*)0 : (_base + _op->m_chainPrev);
}

inline MemoryOperation* opChainNext(const MemoryOperation* _op, MemoryOperation* _base)
{
	return (_op->m_chainNext == kInvalidOpIndex) ? (MemoryOperation*)0 : (_base + _op->m_chainNext);
}

inline uint32_t opToIndex(const MemoryOperation* _op, const MemoryOperation* _base)
{
	return _op ? (uint32_t)(_op - _base) : kInvalidOpIndex;
}

//--------------------------------------------------------------------------
/// Array of memory operations stored as 32-bit arena indices (4 bytes each)
/// instead of 64-bit pointers, halving these (often huge) lists. Reads still
/// hand back MemoryOperation* via operator[] / iteration, so consumers are
/// unchanged; setBase() must be called once (with Capture::m_operationBase)
/// before the first push_back. Sorting / remove_if operate on indices()
/// directly with a base-aware projection (the iterator's value type is
/// MemoryOperation*, which std algorithms cannot swap against uint32 storage).
//--------------------------------------------------------------------------
class MemoryOpArray
{
	std::vector<uint32_t>	m_idx;
	MemoryOperation*		m_base = nullptr;

public:
	struct const_iterator
	{
		const uint32_t*		m_p;
		MemoryOperation*	m_base;
		MemoryOperation*	operator*()  const { return m_base + *m_p; }
		const_iterator&		operator++()       { ++m_p; return *this; }
		bool				operator==(const const_iterator& _o) const { return m_p == _o.m_p; }
		bool				operator!=(const const_iterator& _o) const { return m_p != _o.m_p; }
	};

	void				setBase(MemoryOperation* _base)	{ m_base = _base; }
	MemoryOperation*	getBase() const					{ return m_base; }

	size_t				size() const					{ return m_idx.size(); }
	bool				empty() const					{ return m_idx.empty(); }
	void				clear()							{ m_idx.clear(); }
	void				reserve(size_t _n)				{ m_idx.reserve(_n); }
	void				resize(size_t _n)				{ m_idx.resize(_n); }
	void				push_back(MemoryOperation* _op)	{ m_idx.push_back((uint32_t)(_op - m_base)); }

	MemoryOperation*	operator[](size_t _i) const		{ return m_base + m_idx[_i]; }

	const_iterator		begin() const					{ return { m_idx.data(),               m_base }; }
	const_iterator		end()   const					{ return { m_idx.data() + m_idx.size(), m_base }; }

	std::vector<uint32_t>&			indices()			{ return m_idx; }	///< raw index storage for base-aware sort / remove_if
	const std::vector<uint32_t>&	indices() const		{ return m_idx; }
};

//--------------------------------------------------------------------------
/// Histogram data for a single bin (memory size range) - AOS
//--------------------------------------------------------------------------
struct HistogramBin
{
	uint64_t			m_size;					///< Memory usage at the end of time slice
	uint64_t			m_sizePeak;				///< Peak memory usage inside the time slice
	uint32_t			m_overhead;				///< Overhead at the end of time slice
	uint32_t			m_overheadPeak;			///< Peak overhead inside the time slice
	uint32_t			m_count;				///< Number of surviving memory blocks at the end of time slice
	uint32_t			m_countPeak;			///< Peak number of live blocks inside the time slice
};

//--------------------------------------------------------------------------
/// We need additional information for quick searches
//--------------------------------------------------------------------------
struct MemoryStats
{
	enum { 
		MIN_HISTOGRAM_SIZE	= 8,
		HISTOGRAM_BIN_SHIFT	= 3,				///< 3 bits for minimum of 8 bytes
		NUM_HISTOGRAM_BINS	= 23				///< up to 8 bytes to 32M+
	}; 

	uint64_t			m_memoryUsage;			///< Memory usage at the end of time slice
	uint64_t			m_memoryUsagePeak;		///< Peak memory usage inside the time slice
	uint32_t			m_overhead;				///< Allocation overhead at the end of time slice
	uint32_t			m_overheadPeak;			///< Peak allocation overhead inside the time slice
	uint32_t			m_numberOfOperations;	///< Number of operations inside the time slice
	uint32_t			m_numberOfAllocations;	///< Number of allocations inside the time slice
	uint32_t			m_numberOfReAllocations;///< Number of reallocations inside the time slice
	uint32_t			m_numberOfFrees;		///< Number of frees inside the time slice
	uint32_t			m_numberOfLiveBlocks;	///< Memory blocks allocated but not freed inside the time slice - Memory leaks!
	uint32_t			m_numberOfLiveBlocksPeak;
	HistogramBin		m_histogram[NUM_HISTOGRAM_BINS];

	void reset()
	{
		memset(this, 0, sizeof(MemoryStats));
	}

	void setPeaksToCurrent();
	void setPeaksFrom(MemoryStatLocalPeak& _peaks);
};

//--------------------------------------------------------------------------
/// Group of memory operations
//--------------------------------------------------------------------------
struct MemoryOperationGroup
{
	enum { INDEX_MAPPINGS = 11 };

	uint32_t			m_minSize;				///< single allocation size
	uint32_t			m_maxSize;				///< single allocation size
	int64_t				m_peakSize;				///< group size
	int64_t				m_peakSizeGlobal;		///< total memory usage at the time of group peak size
	int64_t				m_liveSize;				///< group size
	uint32_t			m_count;
	uint32_t			m_liveCount;
	uint32_t			m_liveCountPeak;
	uint32_t			m_liveCountPeakGlobal;
	MemoryOpArray		m_groupOperations;
	uint32_t			m_indexMappings[INDEX_MAPPINGS];
	uint32_t			m_histogram[rtm::MemoryStats::NUM_HISTOGRAM_BINS];
	uint32_t			m_histogramPeak[rtm::MemoryStats::NUM_HISTOGRAM_BINS];

	inline MemoryOperationGroup()
		: m_minSize(0xffffffff)
		, m_maxSize(0)
		, m_peakSize(0)
		, m_liveSize(0)
		, m_count(0)
		, m_liveCount(0)
		, m_liveCountPeak(0)
		, m_liveCountPeakGlobal(0)
	{
		for (int i=0; i<rtm::MemoryStats::NUM_HISTOGRAM_BINS; i++)
		{
			m_histogram[i]		= 0;
			m_histogramPeak[i]	= 0;
		}
	}
};

//--------------------------------------------------------------------------
/// Helper structure storing peak values of a histogram bin
//--------------------------------------------------------------------------
struct HistogramBinPeak
{
	uint64_t			m_sizePeak;
	uint32_t			m_overheadPeak;
	uint32_t			m_countPeak;
};

//--------------------------------------------------------------------------
/// Helper structure storing local peak values of a time range
//--------------------------------------------------------------------------
struct MemoryStatLocalPeak
{
	uint64_t			m_memoryUsagePeak;
	uint32_t			m_overheadPeak;
	uint32_t			m_numberOfLiveBlocksPeak;
	HistogramBinPeak	m_HistogramPeak[MemoryStats::NUM_HISTOGRAM_BINS];
};

//--------------------------------------------------------------------------
/// Structure used for quick calculations of snapshot timelines/histograms
//--------------------------------------------------------------------------
struct MemoryStatsTimed
{
	uint64_t			m_time;
	uint32_t			m_operationIndex;
	MemoryStatLocalPeak	m_localPeak;
	MemoryStats			m_stats;
};

//--------------------------------------------------------------------------
/// Structure representing a single call stack
//--------------------------------------------------------------------------
struct StackTrace
{
	enum Scope
	{
		Global,
		Filtered
	};

	uint32_t			m_numFrames;
	int16_t				m_addedToTree[2];
	uint64_t			m_frames[1];

	static uint32_t		calculateSize(uint32_t numFrames);
	static void			init(StackTrace* st, uint32_t numFrames);
	static StackTrace** getNextArray(StackTrace* st);
};

//--------------------------------------------------------------------------
/// Stack trace tree
//--------------------------------------------------------------------------
struct StackTraceTree
{
	typedef std::vector<StackTraceTree*>	ChildNodes;

	enum Enum
	{
		Alloc,
		Free,
		Realloc,

		Count
	};

	uint64_t			m_addressID;
	int64_t				m_memUsage;
	int64_t				m_memUsagePeak;
	uint64_t			m_minTime;
	uint64_t			m_maxTime;
	int32_t				m_overhead;
	int32_t				m_overheadPeak;
	int32_t				m_depth;
	int32_t				m_opCount[StackTraceTree::Count];
	StackTraceTree*		m_parent;
	StackTrace*			m_stackTraceList;
	ChildNodes			m_children;
	
	inline StackTraceTree() : 
		m_addressID(0),
		m_memUsage(0),
		m_memUsagePeak(0), 
		m_minTime(0),
		m_maxTime(0),
		m_overhead(0),
		m_overheadPeak(0),
		m_depth(0),
		m_parent(NULL),
		m_stackTraceList(NULL)
	{
		memset(&m_opCount[0], 0, sizeof(int32_t)*StackTraceTree::Count);
	}

	~StackTraceTree();
	void reset();
};

//--------------------------------------------------------------------------
/// Memory tag tree
//--------------------------------------------------------------------------
struct MemoryTagTree
{
	//typedef ankerl::unordered_dense::map<uint32_t,MemoryTagTree*>	ChildMap;
	typedef ankerl::unordered_dense::map<uint32_t, MemoryTagTree* >		ChildNodes;
	typedef std::vector<MemoryOperation*>								OpList;

	std::string			m_name;
	uint32_t			m_hash;
	uint64_t			m_usage;
	uint64_t			m_usagePeak;
	uint64_t			m_overhead;
	uint64_t			m_overheadPeak;
	uint32_t			m_operationCount[rmem::LogMarkers::OpCount];
	MemoryTagTree*		m_parent;
	ChildNodes			m_children;
	OpList				m_operations;

	inline MemoryTagTree()
	{
		reset();
	}
	~MemoryTagTree();
	void reset();
};

bool tagFind(MemoryTagTree& _rootTag, uint32_t _hash, MemoryTagTree*& ioResult, MemoryTagTree*& _prevTag);
bool tagInsert(MemoryTagTree* _rootTag, MemoryTagTree* _tag, uint32_t _parentTagHash);
void tagAddOp(MemoryTagTree& _rootTag, MemoryOperation* _op, MemoryTagTree*& _prevTag, MemoryOperation* _base);
void tagTreeDestroy(MemoryTagTree& _rootTag);

struct MemoryMarkerEvent
{
	std::string			m_name;
	uint32_t			m_nameHash;
	uint32_t			m_color;
};

struct MemoryMarkerTime
{
	uint64_t			m_threadID;
	uint64_t			m_time;
	uint32_t			m_eventHash;
};

} // namespace rtm

#endif // RTM_MTUNER_MTUNERLIB_H
