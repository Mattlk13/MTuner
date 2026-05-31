//--------------------------------------------------------------------------//
/// Copyright 2026 Milos Tosic. All Rights Reserved.                       ///
/// License: http://www.opensource.org/licenses/BSD-2-Clause               ///
//--------------------------------------------------------------------------//

#include <MTuner_pch.h>
#include <MTuner/src/insights.h>
#include <MTuner/src/loader/capture.h>
#include <MTuner/src/loader/util.h>
#include <algorithm>
#include <unordered_map>
#include <vector>

//--------------------------------------------------------------------------
// Small formatting + op-type helpers
//--------------------------------------------------------------------------
static QString formatBytes(uint64_t _bytes)
{
	static const char* units[] = { "B", "KB", "MB", "GB", "TB" };
	double v = (double)_bytes;
	int u = 0;
	while ((v >= 1024.0) && (u < 4)) { v /= 1024.0; ++u; }
	if (u == 0)
		return QString("%1 B").arg(_bytes);
	return QString::asprintf("%.1f %s", v, units[u]);
}

static QString formatDuration(float _seconds)
{
	if (_seconds < 1e-3f)	return QString::asprintf("%.0f us", _seconds * 1e6f);
	if (_seconds < 1.0f)	return QString::asprintf("%.1f ms", _seconds * 1e3f);
	return QString::asprintf("%.2f s", _seconds);
}

static inline bool opIsFree(uint16_t _t)    { return _t == rmem::LogMarkers::OpFree; }
static inline bool opIsRealloc(uint16_t _t) { return (_t == rmem::LogMarkers::OpRealloc) || (_t == rmem::LogMarkers::OpReallocAligned); }
static inline bool opIsTrueAlloc(uint16_t _t) { return !opIsFree(_t) && !opIsRealloc(_t); }	// OpAlloc / OpCalloc / OpAllocAligned

static void addInsight(std::vector<Insight>& _out, Insight::Severity _sev, const QString& _cat,
					   const QString& _title, const QString& _detail,
					   rtm::StackTrace* _st = nullptr, bool _hasTime = false, uint64_t _time = 0)
{
	Insight ins;
	ins.m_severity   = _sev;
	ins.m_category   = _cat;
	ins.m_title      = _title;
	ins.m_detail     = _detail;
	ins.m_stackTrace = _st;
	ins.m_hasTime    = _hasTime;
	ins.m_time       = _time;
	_out.push_back(ins);
}

//--------------------------------------------------------------------------
std::vector<Insight> captureInsightsAnalyze(rtm::Capture* _capture)
{
	std::vector<Insight> out;
	if (!_capture)
		return out;

	const rtm::MemoryStats&	g		= _capture->getGlobalStats();
	const rtm::MemoryOpArray& ops	= _capture->getMemoryOps();
	const size_t			numOps	= ops.size();
	const uint64_t			tSpan	= (_capture->getMaxTime() > _capture->getMinTime()) ? (_capture->getMaxTime() - _capture->getMinTime()) : 0;

	//======================================================================
	// 1. Leaks (+ top leak sites)
	//======================================================================
	{
		const rtm::MemoryOpArray& leaks = _capture->getMemoryLeaks();
		const size_t n = leaks.size();
		if (n == 0)
		{
			addInsight(out, Insight::Info, "Leaks", "No memory leaks detected",
				"Every tracked allocation has a matching free at the end of the capture.");
		}
		else
		{
			struct Site { uint64_t bytes = 0; uint32_t count = 0; rtm::StackTrace* st = nullptr; };
			std::unordered_map<uint32_t, Site> sites;
			uint64_t totalBytes = 0;
			for (size_t i = 0; i < n; ++i)
			{
				rtm::MemoryOperation* op = leaks[i];
				totalBytes += op->m_allocSize;
				Site& s = sites[op->m_stackTraceIndex];
				s.bytes += op->m_allocSize;
				s.count += 1;
				if (!s.st) s.st = _capture->getStackTraceByIndex(op->m_stackTraceIndex);
			}

			addInsight(out, (totalBytes > (1u << 20)) ? Insight::High : Insight::Medium, "Leaks",
				QString("%1 leaked allocation(s), %2 never freed").arg(n).arg(formatBytes(totalBytes)),
				QString("%1 allocation(s) were never freed, totalling %2 across %3 call site(s). The largest "
						"sites are listed below - select one to view its stack trace.").arg(n).arg(formatBytes(totalBytes)).arg(sites.size()));

			std::vector<Site> sorted;
			sorted.reserve(sites.size());
			for (std::unordered_map<uint32_t, Site>::const_iterator it = sites.begin(); it != sites.end(); ++it)
				sorted.push_back(it->second);
			std::sort(sorted.begin(), sorted.end(), [](const Site& a, const Site& b) { return a.bytes > b.bytes; });

			const int topN = (int)qMin((size_t)5, sorted.size());
			for (int i = 0; i < topN; ++i)
				addInsight(out, Insight::Medium, "Leaks",
					QString("Leak site: %1 in %2 block(s)").arg(formatBytes(sorted[i].bytes)).arg(sorted[i].count),
					QString("This call site leaked %1 across %2 allocation(s) that were never freed. Select to view the stack trace.")
						.arg(formatBytes(sorted[i].bytes)).arg(sorted[i].count),
					sorted[i].st);
		}
	}

	//======================================================================
	// Single pass over all ops: per-thread / per-heap / per-tag / overhead / alignment
	//======================================================================
	struct ThreadAgg { uint64_t bytes = 0; uint32_t count = 0; };
	struct HeapAgg   { uint64_t bytes = 0; uint64_t overhead = 0; uint32_t count = 0; };
	std::unordered_map<uint32_t, ThreadAgg>	perThread;
	std::unordered_map<uint32_t, HeapAgg>	perHeap;
	std::unordered_map<uint16_t, uint64_t>	perTagBytes;
	uint64_t allocBytesTotal = 0, overheadTotal = 0, untaggedBytes = 0, taggedBytes = 0;
	uint32_t trueAllocCount = 0;
	uint64_t overAlignedWaste = 0; uint32_t overAlignedCount = 0;

	for (size_t i = 0; i < numOps; ++i)
	{
		rtm::MemoryOperation* op = ops[i];
		if (opIsFree(op->m_operationType))
			continue;	// allocations + reallocs contribute "new" bytes; frees don't

		const uint32_t size = op->m_allocSize;
		allocBytesTotal += size;
		overheadTotal   += op->m_overhead;

		ThreadAgg& t = perThread[op->m_threadIndex]; t.bytes += size; t.count += 1;
		HeapAgg&   h = perHeap[op->m_allocatorIndex]; h.bytes += size; h.overhead += op->m_overhead; h.count += 1;

		if (opIsTrueAlloc(op->m_operationType))
		{
			++trueAllocCount;
			perTagBytes[op->m_tag] += size;
			if (op->m_tag == 0) untaggedBytes += size;
			taggedBytes += size;

			if (op->m_alignment != 255)
			{
				const uint64_t align = (uint64_t)1u << op->m_alignment;
				if ((align > size) && (size > 0))	// alignment larger than the payload -> wasteful padding
				{
					overAlignedWaste += (align - size);
					++overAlignedCount;
				}
			}
		}
	}

	//======================================================================
	// Chain pass over allocation lineages: reallocs, lifetime, cross-thread frees
	//======================================================================
	struct ReallocSite { uint64_t reallocs = 0; uint32_t blocks = 0; uint32_t maxOnOne = 0; rtm::StackTrace* st = nullptr; };
	std::unordered_map<uint32_t, ReallocSite>	reallocSites;	// keyed by chain-head stack-trace index
	uint64_t	reallocCopyBytes = 0;
	uint64_t	freedBlocks = 0, crossThreadFrees = 0;

	struct ShortLived { uint64_t size; float lifetime; rtm::StackTrace* st; };
	std::vector<ShortLived> shortLivedLarge;
	const uint64_t kLargeBytes = 1u << 20;	// 1 MB

	for (size_t i = 0; i < numOps; ++i)
	{
		rtm::MemoryOperation* head = ops[i];
		if (_capture->getChainPrev(head) != nullptr)	// only process chain heads
			continue;
		if (!opIsTrueAlloc(head->m_operationType))
			continue;

		uint32_t reallocs = 0, maxSize = head->m_allocSize, prevSize = head->m_allocSize;
		rtm::MemoryOperation* tail = head;
		for (rtm::MemoryOperation* cur = _capture->getChainNext(head); cur != nullptr; cur = _capture->getChainNext(cur))
		{
			if (opIsRealloc(cur->m_operationType))
			{
				++reallocs;
				reallocCopyBytes += qMin(prevSize, cur->m_allocSize);
			}
			if (cur->m_allocSize > maxSize) maxSize = cur->m_allocSize;
			prevSize = cur->m_allocSize;
			tail = cur;
		}

		if (reallocs > 0)
		{
			ReallocSite& rs = reallocSites[head->m_stackTraceIndex];
			rs.reallocs += reallocs;
			rs.blocks   += 1;
			rs.maxOnOne  = qMax(rs.maxOnOne, reallocs);
			if (!rs.st) rs.st = _capture->getStackTraceByIndex(head->m_stackTraceIndex);
		}

		if (opIsFree(tail->m_operationType))
		{
			++freedBlocks;
			if (tail->m_threadIndex != head->m_threadIndex)
				++crossThreadFrees;

			if ((maxSize >= kLargeBytes) && (tSpan > 0))
			{
				const uint64_t life = (tail->m_operationTime > head->m_operationTime) ? (tail->m_operationTime - head->m_operationTime) : 0;
				if (life * 100 < tSpan)	// freed within 1% of the capture timeline
					shortLivedLarge.push_back({ maxSize, _capture->getFloatTime(life), _capture->getStackTraceByIndex(head->m_stackTraceIndex) });
			}
		}
	}

	//======================================================================
	// 2. Unbounded growth (steady upward usage trend) + locate global peak time
	//======================================================================
	uint64_t peakTime = 0; uint64_t sampledPeak = 0;
	{
		const uint64_t t0 = _capture->getMinTime();
		const uint64_t t1 = _capture->getMaxTime();
		if (t1 > t0)
		{
			const int N = 24;
			rtm::GraphEntry e;
			uint64_t first = 0, last = 0, prev = 0;
			int rising = 0;
			for (int k = 0; k <= N; ++k)
			{
				const uint64_t t = t0 + (uint64_t)((double)(t1 - t0) * k / N);
				_capture->getGraphAtTime(t, e);
				if (k == 0) first = e.m_usage;
				last = e.m_usage;
				if (e.m_usage > sampledPeak) { sampledPeak = e.m_usage; peakTime = t; }
				if ((k > 0) && (e.m_usage >= prev)) ++rising;
				prev = e.m_usage;
			}
			if ((first > 0) && (last > first + first / 2) && (rising >= (N * 3) / 4))
				addInsight(out, Insight::Medium, "Growth",
					QString("Memory grew steadily over the capture (%1 -> %2)").arg(formatBytes(first)).arg(formatBytes(last)),
					QString("Live memory rose from %1 to %2 and increased across most of the timeline. This often indicates a "
							"leak or an unbounded cache - check the leak sites above and the memory timeline.").arg(formatBytes(first)).arg(formatBytes(last)),
					nullptr, true, t1);
		}
	}

	//======================================================================
	// 3. Transient peak spike (peak far above the final/steady usage)
	//======================================================================
	{
		const uint64_t peak  = g.m_memoryUsagePeak;
		const uint64_t final = g.m_memoryUsage;
		if ((peak > final + final / 3) && ((peak - final) >= (16u << 20)))	// >1.3x and >16MB above final
			addInsight(out, Insight::Medium, "Peak",
				QString("Transient peak spike: %1 peak vs %2 at the end").arg(formatBytes(peak)).arg(formatBytes(final)),
				QString("Peak memory (%1) is well above the final usage (%2) - a transient spike of ~%3 that briefly inflates the "
						"high-water mark (problematic for fixed budgets/consoles). Select to jump to the peak on the timeline.")
					.arg(formatBytes(peak)).arg(formatBytes(final)).arg(formatBytes(peak - final)),
				nullptr, peakTime != 0, peakTime);
	}

	//======================================================================
	// 4. Peak drivers (largest simultaneous bytes per call site)
	//======================================================================
	{
		struct P { int64_t peak; rtm::StackTrace* st; };
		std::vector<P> peaks;
		const rtm::MemoryGroupsHashType& groups = _capture->getMemoryGroups();
		for (rtm::MemoryGroupsHashType::const_iterator it = groups.begin(); it != groups.end(); ++it)
		{
			const rtm::MemoryOperationGroup& grp = it->second;
			if (grp.m_groupOperations.size() == 0) continue;
			rtm::MemoryOperation* rep = grp.m_groupOperations[0];
			if (!rtm::isAlloc(rep->m_operationType)) continue;
			if (grp.m_peakSize > 0)
				peaks.push_back({ grp.m_peakSize, _capture->getStackTraceByIndex(rep->m_stackTraceIndex) });
		}
		std::sort(peaks.begin(), peaks.end(), [](const P& a, const P& b) { return a.peak > b.peak; });
		const int topN = (int)qMin((size_t)3, peaks.size());
		for (int i = 0; i < topN; ++i)
			addInsight(out, Insight::Info, "Peak",
				QString("Top peak contributor: up to %1 live at one site").arg(formatBytes((uint64_t)peaks[i].peak)),
				QString("This call site held up to %1 simultaneously - one of the largest contributors to peak memory usage. "
						"Select to view the stack trace.").arg(formatBytes((uint64_t)peaks[i].peak)),
				peaks[i].st);
	}

	//======================================================================
	// 5. Allocation churn (high count, almost all freed)
	//======================================================================
	{
		struct C { uint32_t count; uint32_t freed; rtm::StackTrace* st; };
		std::vector<C> churn;
		const rtm::MemoryGroupsHashType& groups = _capture->getMemoryGroups();
		for (rtm::MemoryGroupsHashType::const_iterator it = groups.begin(); it != groups.end(); ++it)
		{
			const rtm::MemoryOperationGroup& grp = it->second;
			if (grp.m_groupOperations.size() == 0) continue;
			rtm::MemoryOperation* rep = grp.m_groupOperations[0];
			if (!rtm::isAlloc(rep->m_operationType)) continue;
			const uint32_t count = grp.m_count;
			const uint32_t freed = (grp.m_count > grp.m_liveCount) ? (grp.m_count - grp.m_liveCount) : 0;
			if ((count >= 5000) && (freed >= (uint32_t)(count * 0.9)))
				churn.push_back({ count, freed, _capture->getStackTraceByIndex(rep->m_stackTraceIndex) });
		}
		std::sort(churn.begin(), churn.end(), [](const C& a, const C& b) { return a.count > b.count; });
		const int topN = (int)qMin((size_t)4, churn.size());
		for (int i = 0; i < topN; ++i)
			addInsight(out, Insight::Medium, "Churn",
				QString("Allocation churn: %1 short-lived allocations at one site").arg(churn[i].count),
				QString("This call site made %1 allocation(s) and freed ~%2 of them. High-frequency transient allocations add "
						"allocator overhead and fragmentation - consider a pool, arena, or a reserved/stack buffer. Select to view the stack trace.")
					.arg(churn[i].count).arg(churn[i].freed),
				churn[i].st);
	}

	//======================================================================
	// 6. Reallocation growth (buffers reallocated repeatedly -> reserve up front)
	//======================================================================
	{
		std::vector<ReallocSite> sites;
		sites.reserve(reallocSites.size());
		for (std::unordered_map<uint32_t, ReallocSite>::const_iterator it = reallocSites.begin(); it != reallocSites.end(); ++it)
			sites.push_back(it->second);
		std::sort(sites.begin(), sites.end(), [](const ReallocSite& a, const ReallocSite& b) { return a.reallocs > b.reallocs; });

		int emitted = 0;
		for (size_t i = 0; (i < sites.size()) && (emitted < 3); ++i)
		{
			// Either a lot of reallocs in aggregate, or a single buffer reallocated many times.
			if ((sites[i].reallocs >= 1000) || (sites[i].maxOnOne >= 32))
			{
				addInsight(out, Insight::Medium, "Realloc",
					QString("Repeated reallocations: %1 realloc(s) across %2 buffer(s) at one site").arg(sites[i].reallocs).arg(sites[i].blocks),
					QString("This call site performed %1 reallocation(s) (up to %2 on a single buffer). Repeatedly growing a buffer "
							"reallocates and copies its contents each time - reserve the final capacity up front (e.g. vector::reserve) "
							"to avoid the churn. Select to view the stack trace.").arg(sites[i].reallocs).arg(sites[i].maxOnOne),
					sites[i].st);
				++emitted;
			}
		}

		if (reallocCopyBytes >= (64u << 20))	// >64MB copied by reallocs
			addInsight(out, Insight::Low, "Realloc",
				QString("Reallocations copied ~%1 of data").arg(formatBytes(reallocCopyBytes)),
				QString("Across the capture, reallocations copied approximately %1 of existing data. Reserving capacity up front "
						"for growing buffers eliminates most of these copies.").arg(formatBytes(reallocCopyBytes)));
	}

	//======================================================================
	// 7. Short-lived large allocations (big buffers freed almost immediately)
	//======================================================================
	{
		std::sort(shortLivedLarge.begin(), shortLivedLarge.end(), [](const ShortLived& a, const ShortLived& b) { return a.size > b.size; });
		const int topN = (int)qMin((size_t)3, shortLivedLarge.size());
		for (int i = 0; i < topN; ++i)
			addInsight(out, Insight::Medium, "Lifetime",
				QString("Short-lived large allocation: %1 freed after %2").arg(formatBytes(shortLivedLarge[i].size)).arg(formatDuration(shortLivedLarge[i].lifetime)),
				QString("A %1 allocation was freed after only %2. Large, briefly-lived allocations spike the high-water mark and "
						"thrash the allocator - a reusable scratch buffer or stack allocation avoids the round trip. Select to view the stack trace.")
					.arg(formatBytes(shortLivedLarge[i].size)).arg(formatDuration(shortLivedLarge[i].lifetime)),
				shortLivedLarge[i].st);
	}

	//======================================================================
	// 8. Cross-thread frees (allocated on one thread, freed on another)
	//======================================================================
	if ((freedBlocks >= 1000) && (crossThreadFrees * 100 >= freedBlocks * 5) && (perThread.size() > 1))
	{
		const int pct = (int)((crossThreadFrees * 100) / freedBlocks);
		addInsight(out, Insight::Medium, "Threads",
			QString("%1% of blocks are freed on a different thread than allocated").arg(pct),
			QString("%1 of %2 freed blocks were released on a different thread than the one that allocated them. Cross-thread "
					"alloc/free traffic causes allocator lock contention and can hurt scalability - consider thread-local pools or "
					"handing ownership back to the originating thread.").arg(crossThreadFrees).arg(freedBlocks));
	}

	//======================================================================
	// 9. Per-thread allocation pressure (one thread dominates)
	//======================================================================
	if ((perThread.size() > 1) && (allocBytesTotal > 0))
	{
		uint32_t topIdx = 0; uint64_t topBytes = 0; uint32_t topCount = 0;
		for (std::unordered_map<uint32_t, ThreadAgg>::const_iterator it = perThread.begin(); it != perThread.end(); ++it)
			if (it->second.bytes > topBytes) { topBytes = it->second.bytes; topIdx = it->first; topCount = it->second.count; }

		if (topBytes * 100 >= allocBytesTotal * 70)	// one thread does >=70% of allocation bytes
			addInsight(out, Insight::Low, "Threads",
				QString("One thread drives %1% of all allocations").arg((int)((topBytes * 100) / allocBytesTotal)),
				QString("Thread 0x%1 performed %2 allocation(s) totalling %3 - the dominant allocator across %4 thread(s). If this is a "
						"hot path, a thread-local pool or arena can remove allocator contention.")
					.arg(_capture->getThreadId(topIdx), 0, 16).arg(topCount).arg(formatBytes(topBytes)).arg(perThread.size()));
	}

	//======================================================================
	// 10. Allocator overhead (+ dominant heap)
	//======================================================================
	if ((allocBytesTotal > 0) && (overheadTotal * 100 >= allocBytesTotal * 8) && (overheadTotal >= (1u << 20)))
	{
		addInsight(out, Insight::Medium, "Overhead",
			QString("Allocator overhead is %1% of requested bytes (%2)").arg((int)((overheadTotal * 100) / allocBytesTotal)).arg(formatBytes(overheadTotal)),
			QString("Bookkeeping/alignment overhead totals %1 (%2% of the %3 actually requested). This is typical of many small "
					"allocations - pooling or larger block sizes reduce it.")
				.arg(formatBytes(overheadTotal)).arg((int)((overheadTotal * 100) / allocBytesTotal)).arg(formatBytes(allocBytesTotal)));
	}
	if ((perHeap.size() > 1) && (allocBytesTotal > 0))
	{
		uint32_t topIdx = 0; uint64_t topBytes = 0;
		for (std::unordered_map<uint32_t, HeapAgg>::const_iterator it = perHeap.begin(); it != perHeap.end(); ++it)
			if (it->second.bytes > topBytes) { topBytes = it->second.bytes; topIdx = it->first; }

		if (topBytes * 100 >= allocBytesTotal * 80)
		{
			const uint64_t handle = _capture->getHeapHandle(topIdx);
			rtm::HeapsType& heaps = _capture->getHeaps();
			rtm::HeapsType::iterator hit = heaps.find(handle);
			const QString name = (hit != heaps.end() && !hit->second.empty()) ? QString(hit->second.c_str()) : QString("0x%1").arg(handle, 0, 16);
			addInsight(out, Insight::Info, "Heaps",
				QString("Allocator '%1' handles %2% of all memory").arg(name).arg((int)((topBytes * 100) / allocBytesTotal)),
				QString("Across %1 allocator(s), '%2' accounts for %3 (%4%). If unrelated subsystems share one heap, splitting them into "
						"dedicated allocators improves locality and makes budgets easier to track.")
					.arg(perHeap.size()).arg(name).arg(formatBytes(topBytes)).arg((int)((topBytes * 100) / allocBytesTotal)));
		}
	}

	//======================================================================
	// 11. Tag coverage (untagged memory / no tags at all)
	//======================================================================
	{
		size_t taggedSubsystems = 0;	// distinct non-zero tags
		for (std::unordered_map<uint16_t, uint64_t>::const_iterator it = perTagBytes.begin(); it != perTagBytes.end(); ++it)
			if (it->first != 0) ++taggedSubsystems;

		if ((trueAllocCount >= 5000) && (taggedSubsystems == 0))
			addInsight(out, Insight::Low, "Tags",
				"No memory tags found in this capture",
				"None of the allocations are tagged. Adding RMEM tags (RMEM_TAG / rmemEnterTag) around subsystems lets MTuner attribute "
				"memory to features (rendering, audio, assets, ...) and makes budgets and these insights far more actionable.");
		else if ((taggedSubsystems > 0) && (taggedBytes > 0) && (untaggedBytes * 100 >= taggedBytes * 50))
			addInsight(out, Insight::Low, "Tags",
				QString("%1% of allocated bytes are untagged").arg((int)((untaggedBytes * 100) / taggedBytes)),
				QString("%1 of %2 allocated bytes have no memory tag, even though %3 tagged subsystem(s) exist. Extending tag coverage "
						"to the untagged allocations gives a complete per-subsystem memory breakdown.")
					.arg(formatBytes(untaggedBytes)).arg(formatBytes(taggedBytes)).arg(taggedSubsystems));
	}

	//======================================================================
	// 12. Over-alignment waste
	//======================================================================
	if ((overAlignedCount >= 1000) && (overAlignedWaste >= (1u << 20)))
		addInsight(out, Insight::Low, "Alignment",
			QString("Over-alignment wastes ~%1 across %2 allocations").arg(formatBytes(overAlignedWaste)).arg(overAlignedCount),
			QString("%1 allocation(s) request an alignment larger than their own size, wasting roughly %2 in padding. Review whether such "
					"strong alignment is actually required for these objects.").arg(overAlignedCount).arg(formatBytes(overAlignedWaste)));

	//======================================================================
	// 13. Small-allocation overhead (size distribution)
	//======================================================================
	{
		const uint64_t small = (uint64_t)g.m_histogram[0].m_count + g.m_histogram[1].m_count + g.m_histogram[2].m_count;	// <= 32 bytes
		const uint64_t total = g.m_numberOfAllocations;
		if ((total >= 10000) && (small * 100 >= total * 40))
			addInsight(out, Insight::Medium, "Overhead",
				QString("%1% of allocations are <= 32 bytes").arg((int)((small * 100) / total)),
				QString("%1 of %2 allocations are <= 32 bytes. Many tiny allocations carry disproportionate allocator overhead and hurt "
						"cache locality - a small-object pool or arena can help.").arg(small).arg(total));
	}

	return out;
}
