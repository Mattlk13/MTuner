//--------------------------------------------------------------------------//
/// Copyright 2026 Milos Tosic. All Rights Reserved.                       ///
/// License: http://www.opensource.org/licenses/BSD-2-Clause               ///
//--------------------------------------------------------------------------//

#include <MTuner_pch.h>
#include <MTuner/src/operationslist.h>
#include <MTuner/src/capturecontext.h>

#if RTM_PLATFORM_WINDOWS && RTM_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable: 4211)	// nonstandard extension used: redefined extern to static
static bool __uncaught_exception() { return true; }
#pragma warning(pop)
#include <ppl.h>

#define RTM_PARALLEL_FOR_EACH	Concurrency::parallel_for_each
#define RTM_PARALLEL_SORT		Concurrency::parallel_sort
//radixsort
#endif // RTM_PLATFORM_WINDOWS && RTM_COMPILER_MSVC

#if RTM_COMPILER_GCC
#include <parallel/algorithm>

#define RTM_PARALLEL_FOR_EACH	__gnu_parallel::for_each
#define RTM_PARALLEL_SORT		__gnu_parallel::sort
#endif // RTM_COMPILER_GCC

#ifndef RTM_PARALLEL_FOR_EACH // fallback
#define RTM_PARALLEL_FOR_EACH	std::for_each
#define RTM_PARALLEL_SORT		std::sort
#endif // ifndef RTM_PARALLEL_FOR_EACH

struct Mapping
{
	std::vector<uint32_t>						m_sortedIndex;
	const rtm::MemoryOpArray*	m_allOps;
};

struct OperationColumn
{
	enum Enum
	{
		ThreadID,
		Heap,
		Address,
		Type,
		Size,
		Alignment,
		Time,

		Count
	};
};

class OperationTableSource : public BigTableSource
{
	private:
	public:
		CaptureContext*	m_context;
		OperationsList*	m_list;
		uint32_t		m_numColumns;
		uint32_t		m_numRows;
		Mapping			m_mapping;
		uint32_t		m_currentColumn;
		bool			m_valid;
		Qt::SortOrder	m_sortOrder;
		QLocale			m_locale;
		const rtm::MemoryOpArray*	m_allOps;

	public:
		OperationTableSource(CaptureContext* _context, bool _valid, OperationsList* _list, bool _leaksOnly);
		virtual ~OperationTableSource() {}

		void prepareData(bool _onlyLeaks);

		virtual QStringList	getHeaderInfo(int32_t& _sortColumn, Qt::SortOrder& _sortOrder, QList<int>& _widths);
		virtual uint32_t	getNumberOfRows();
		virtual	QString		getItem(uint32_t _index, int32_t _column, QColor* _color, bool* _setColor);
		virtual void 		getItem(uint32_t _index, void**);
		virtual Qt::AlignmentFlag getAlignment(uint32_t _index);
		virtual uint32_t	getItemIndex(void* _item);
		virtual void 		sortColumn(uint32_t _columnIndex, Qt::SortOrder _sortOrder);

		void* FindNextByAddress(uint64_t _address, uint32_t _startIndex);
		void* FindNextBySize(uint64_t _size, uint32_t _startIndex);

		void saveState(QSettings& _settings);
};

struct pSetIndex
{
	const std::vector<uint32_t>* m_Indices;
	pSetIndex(const std::vector<uint32_t>& _indices) : m_Indices(&_indices) {}
	inline void operator()(uint32_t& _index) const
	{
		uint32_t idx = &_index - &((*m_Indices)[0]);
		_index = idx;
	}
};

// ThreadID
struct pSortThreadID
{
	const rtm::MemoryOpArray* m_allOps;
	pSortThreadID(const rtm::MemoryOpArray* _ops) : m_allOps(_ops) {}

	inline uint64_t operator()(const uint32_t _val1, const uint32_t _val2) const 
	{
		return m_allOps->operator[](_val1)->m_threadIndex < m_allOps->operator[](_val2)->m_threadIndex;
	}
};

// Heap
struct pSortHeap
{
	const rtm::MemoryOpArray* m_allOps;
	pSortHeap(const rtm::MemoryOpArray* _ops) : m_allOps(_ops) {}

	inline uint64_t operator()(const uint32_t _val1, const uint32_t _val2) const
	{
		return m_allOps->operator[](_val1)->m_allocatorIndex < m_allOps->operator[](_val2)->m_allocatorIndex;
	}
};

// Address
struct pSortAddress
{
	const rtm::MemoryOpArray* m_allOps;
	pSortAddress(const rtm::MemoryOpArray* _ops) : m_allOps(_ops) {}

	inline uint64_t operator()(const uint32_t _val1, const uint32_t _val2) const
	{
		return m_allOps->operator[](_val1)->m_pointer < m_allOps->operator[](_val2)->m_pointer; 
	}
};

// Type
struct pSortOpType
{
	const rtm::MemoryOpArray* m_allOps;
	pSortOpType(const rtm::MemoryOpArray* _ops) : m_allOps(_ops) {}

	inline uint8_t operator()(const uint32_t _val1, const uint32_t _val2) const
	{
		return m_allOps->operator[](_val1)->m_operationType < m_allOps->operator[](_val2)->m_operationType;
	}
};

// Size
struct pSortOpSize
{
	const rtm::MemoryOpArray* m_allOps;
	pSortOpSize(const rtm::MemoryOpArray* _ops) : m_allOps(_ops) {}

	inline uint32_t operator()(const uint32_t _val1, const uint32_t _val2) const
	{
		return m_allOps->operator[](_val1)->m_allocSize < m_allOps->operator[](_val2)->m_allocSize;
	}
};

// Alignment
struct pSortOpAlignment
{
	const rtm::MemoryOpArray* m_allOps;
	pSortOpAlignment(const rtm::MemoryOpArray* _ops) : m_allOps(_ops) {}

	inline uint32_t operator()(const uint32_t _val1, const uint32_t _val2) const
	{
		return m_allOps->operator[](_val1)->m_alignment < m_allOps->operator[](_val2)->m_alignment;
	}
};

struct pSetOpMappings
{
	const rtm::MemoryOpArray* m_allOps;
	const Mapping* m_mapping;
	rtm::Capture* m_capture;

	pSetOpMappings(const rtm::MemoryOpArray* _ops, const Mapping& _mapping, rtm::Capture* _capture) :
		m_allOps(_ops), m_mapping(&_mapping), m_capture(_capture)
	{}

	inline void operator()(const uint32_t _index) const
	{
		uint32_t idx = m_mapping->m_sortedIndex[_index];
		rtm::MemoryOperation* op = (*m_allOps)[idx];
		m_capture->setOperationRow(op, _index);	// concurrent: distinct ops -> distinct slots; vector pre-sized in sortColumn
	}
};

OperationTableSource::OperationTableSource(CaptureContext* _context, bool _valid, OperationsList* _list, bool _leaksOnly)
	: m_context(_context)
	, m_list(_list)
	, m_valid(_valid)
{
	m_numColumns	= OperationColumn::Count;
	m_context		= _context;
	prepareData(_leaksOnly);
}

void OperationTableSource::prepareData(bool /*_onlyLeaks*/)
{
	bool filterEnabled = m_list->getFilteringState();
	const rtm::MemoryOpArray& _ops = (m_valid == false)
													? m_context->m_capture->getMemoryOpsInvalid()
													: filterEnabled ? m_context->m_capture->getMemoryOpsFiltered() : m_context->m_capture->getMemoryOps();

	m_numRows		= (uint32_t)_ops.size();
	m_sortOrder		= Qt::AscendingOrder;
	m_allOps		= &_ops;

	// init index arrays for columns
	const uint32_t numItems = m_numRows;

	m_mapping.m_sortedIndex.resize(numItems);
	m_mapping.m_allOps = m_allOps;

	pSetIndex psSetIdx(m_mapping.m_sortedIndex);
	RTM_PARALLEL_FOR_EACH(m_mapping.m_sortedIndex.begin(), m_mapping.m_sortedIndex.end(), psSetIdx);

	m_context->m_capture->ensureOperationRowMapping();	// pre-size before the parallel fill below
	pSetOpMappings psMap(m_allOps, m_mapping, m_context->m_capture);
	RTM_PARALLEL_FOR_EACH(m_mapping.m_sortedIndex.begin(), m_mapping.m_sortedIndex.end(), psMap);

	m_currentColumn = OperationColumn::Time;
}

QStringList	OperationTableSource::getHeaderInfo(int32_t& _sortColumn, Qt::SortOrder& _sortOrder, QList<int>& _widths)
{
	QStringList header;
	header << QObject::tr("Thread ID") << QObject::tr("Heap") << QObject::tr("Address") << QObject::tr("Type") << QObject::tr("Size") << QObject::tr("Alignment") << QObject::tr("Time");

	_widths << 75
			<< 75
			<< 75
			<< 40
			<< 100
			<< 65
			<< 100;

	_sortColumn	= m_currentColumn;
	_sortOrder	= m_sortOrder;
	return header;
}

uint32_t OperationTableSource::getNumberOfRows()
{
	return m_numRows;
}

QString getTimeString(float _time, uint64_t* _msec = 0);
QString OperationTableSource::getItem(uint32_t _index, int32_t _column, QColor* _color, bool* _setColor)
{
	uint32_t index = _index;
	if (m_sortOrder == Qt::DescendingOrder)
		index = m_numRows - index - 1;

	uint32_t idx = m_mapping.m_sortedIndex[index];
	const rtm::MemoryOperation* op = m_mapping.m_allOps->operator[](idx);

	if (_color)
	{
		*_color = QColor(255,169,40);
		if (_setColor)
			*_setColor = op->m_isLeaked ? true : false;
	}

	switch (_column)
	{
		case OperationColumn::ThreadID: 
			return "0x" + QString::number(m_context->m_capture->getThreadId(op->m_threadIndex),16);
			
		case OperationColumn::Heap:
			{
				const uint64_t allocatorHandle = m_context->m_capture->getHeapHandle(op->m_allocatorIndex);
				rtm::HeapsType& heaps = m_context->m_capture->getHeaps();
				rtm::HeapsType::iterator it = heaps.find(allocatorHandle);
				if (it != heaps.end())
					return it->second.c_str();
				else
					return "0x" + QString::number(allocatorHandle, 16);
			}
			
		case OperationColumn::Address: 
			return "0x" + QString::number(op->m_pointer,16);

		case OperationColumn::Type:
		{
			static QString	typeName[rmem::LogMarkers::OpCount] = {
				QObject::tr("Alloc"),
				QObject::tr("Alloc aligned"),
				QObject::tr("Calloc"),
				QObject::tr("Free"),
				QObject::tr("Realloc"),
				QObject::tr("Realloc aligned")
			};

			return typeName[op->m_operationType];
		}

		case OperationColumn::Size:
		{
			return m_locale.toString(op->m_allocSize);
		}

		case OperationColumn::Alignment:
		{
			if (op->m_alignment == 255)
				return QObject::tr("Default");
			else
				return QString::number(1 << op->m_alignment);
		}

		case OperationColumn::Time:
			return getTimeString(m_context->m_capture->getFloatTime(op->m_operationTime));
	};

	return "";
}

void OperationTableSource::getItem(uint32_t _index, void** _pointer)
{
	if (_index == (uint32_t)-1)
		return;

	uint32_t index = _index;
	if (m_sortOrder == Qt::DescendingOrder)
		index = m_numRows - index - 1;
	uint32_t idx = m_mapping.m_sortedIndex[index];
	const rtm::MemoryOperation* op = m_mapping.m_allOps->operator[](idx);
	*_pointer = (void*)(op);
}

Qt::AlignmentFlag OperationTableSource::getAlignment(uint32_t _index)
{
	switch (_index)
	{
		case OperationColumn::Size:
		case OperationColumn::Alignment:
		case OperationColumn::Time:
			return Qt::AlignRight;
	}

	return Qt::AlignLeft;
}

uint32_t OperationTableSource::getItemIndex(void* _item)
{
	uint32_t index = m_context->m_capture->getOperationRow((rtm::MemoryOperation*)_item);
	if (m_sortOrder == Qt::DescendingOrder)
		index = m_numRows - index - 1;
	return index;
}

void OperationTableSource::sortColumn(uint32_t _columnIndex, Qt::SortOrder _sortOrder)
{
	pSetIndex psSetIdx(m_mapping.m_sortedIndex);
	RTM_PARALLEL_FOR_EACH(m_mapping.m_sortedIndex.begin(), m_mapping.m_sortedIndex.end(), psSetIdx);

	switch (_columnIndex)
	{
		case OperationColumn::ThreadID:
		{
			pSortThreadID psThreadID(m_allOps);
			RTM_PARALLEL_SORT(m_mapping.m_sortedIndex.begin(), m_mapping.m_sortedIndex.end(), psThreadID);
		}
		break;
			
		case OperationColumn::Heap:
		{
			pSortHeap psHeap(m_allOps);
			RTM_PARALLEL_SORT(m_mapping.m_sortedIndex.begin(), m_mapping.m_sortedIndex.end(), psHeap);
		}
		break;
			
		case OperationColumn::Address:
		{
			pSortAddress psAddress(m_allOps);
			RTM_PARALLEL_SORT(m_mapping.m_sortedIndex.begin(), m_mapping.m_sortedIndex.end(), psAddress);
		}
		break;

		case OperationColumn::Type:
		{
			pSortOpType psType(m_allOps);
			RTM_PARALLEL_SORT(m_mapping.m_sortedIndex.begin(), m_mapping.m_sortedIndex.end(), psType );
		}
		break;

		case OperationColumn::Size:
		{
			pSortOpSize psSize(m_allOps);
			RTM_PARALLEL_SORT(m_mapping.m_sortedIndex.begin(), m_mapping.m_sortedIndex.end(), psSize );
		}
		break;

		case OperationColumn::Alignment:
		{
			pSortOpAlignment psAlignment(m_allOps);
			RTM_PARALLEL_SORT(m_mapping.m_sortedIndex.begin(), m_mapping.m_sortedIndex.end(), psAlignment );
		}
		break;

		case OperationColumn::Time:
		{
		}
		break;
	};

	m_context->m_capture->ensureOperationRowMapping();	// pre-size before the parallel fill below
	pSetOpMappings psMap(m_allOps, m_mapping, m_context->m_capture);
	RTM_PARALLEL_FOR_EACH(m_mapping.m_sortedIndex.begin(), m_mapping.m_sortedIndex.end(), psMap);

	m_currentColumn	= _columnIndex;
	m_sortOrder		= _sortOrder;
}

void* OperationTableSource::FindNextByAddress(uint64_t _address, uint32_t _startIndex)
{
	// _startIndex is a display row (from getItemIndex); iterate downward in display
	// order, mapping each display row back to its sorted position so the search
	// direction is correct in both ascending and descending sort order.
	uint32_t numItems = (uint32_t)m_mapping.m_sortedIndex.size();
	for (uint32_t d=_startIndex+1; d<numItems; ++d)
	{
		uint32_t sortedPos = (m_sortOrder == Qt::DescendingOrder) ? (numItems - 1 - d) : d;
		uint32_t index = m_mapping.m_sortedIndex[sortedPos];
		rtm::MemoryOperation* op = m_allOps->operator[](index);
		if (op->m_pointer == _address)
			return op;
	}

	return nullptr;
}

void* OperationTableSource::FindNextBySize(uint64_t _size, uint32_t _startIndex)
{
	uint32_t numItems = (uint32_t)m_mapping.m_sortedIndex.size();
	for (uint32_t d=_startIndex+1; d<numItems; ++d)
	{
		uint32_t sortedPos = (m_sortOrder == Qt::DescendingOrder) ? (numItems - 1 - d) : d;
		uint32_t index = m_mapping.m_sortedIndex[sortedPos];
		rtm::MemoryOperation* op = m_allOps->operator[](index);
		if (op->m_allocSize == _size)
			return op;
	}

	return nullptr;
}

void OperationTableSource::saveState(QSettings& _settings)
{
	_settings.setValue("operationListSortColumn", m_currentColumn);
	_settings.setValue("operationListSortOrder", (int)m_sortOrder);
}

OperationsList::OperationsList(QWidget* _parent, Qt::WindowFlags _flags) :
	QWidget(_parent, _flags)
{
	m_context = nullptr;
	m_currentItem = nullptr;
	m_tableSource = nullptr;
	m_enableFiltering = false;

	ui.setupUi(this);

	m_operationList = findChild<BigTable*>("bigTableWidget");
	m_operationSearch = findChild<OperationSearch*>("operationSearchWidget");

	connect(m_operationList, SIGNAL(itemSelected(void*)), this, SLOT(selectionChanged(void*)));

	connect(m_operationSearch, SIGNAL(findPrev()), this, SLOT(selectPrevious()));
	connect(m_operationSearch, SIGNAL(findNext()), this, SLOT(selectNext()));
	connect(m_operationSearch, SIGNAL(searchByAddress(uint64_t)), this, SLOT(selectNextByAddress(uint64_t)));
	connect(m_operationSearch, SIGNAL(searchBySize(uint64_t)), this, SLOT(selectNextBySize(uint64_t)));
	connect(m_operationSearch, SIGNAL(showLeaksOnly(bool)), this, SLOT(toggleLeaksOnly(bool)));
}

OperationsList::~OperationsList()
{
	delete m_tableSource;
}

void OperationsList::changeEvent(QEvent* _event)
{
	QWidget::changeEvent(_event);
	if (_event->type() == QEvent::LanguageChange)
		ui.retranslateUi(this);
}

void OperationsList::setContext(CaptureContext* _context, bool _valid)
{
	m_context = _context;
	m_tableSource = new OperationTableSource(_context, _valid, this, m_operationSearch->isLeaksOnlyChecked());
	m_operationList->setSource(m_tableSource);

	m_tableSource->sortColumn(m_savedColumn, m_savedOrder);
	m_operationList->getHeader()->setSortIndicator(m_savedColumn, m_savedOrder);

	if (m_headerState.size())
		m_operationList->getHeader()->restoreState(m_headerState);
}

void OperationsList::setFilteringState(bool _state, bool _leaksOnly)
{
	m_enableFiltering = _state;
	m_tableSource->prepareData(_leaksOnly);
	m_operationList->resetView();
}

bool OperationsList::getFilteringState() const
{
	return m_enableFiltering;
}

void OperationsList::loadState(QSettings& _settings, const QString& _name, bool _resetGeometry)
{
	m_savedColumn = OperationColumn::Time;
	m_savedOrder = Qt::AscendingOrder;

	m_settingsGroupName = _name;
	_settings.beginGroup(m_settingsGroupName);

	if (_settings.contains("operationListSortColumn") && !_resetGeometry)
	{
		m_savedColumn = _settings.value("operationListSortColumn").toInt();
		m_savedOrder = (Qt::SortOrder)_settings.value("operationListSortOrder").toInt();
		m_headerState = _settings.value("operationListHeaderState").toByteArray();
	}
	_settings.endGroup();
}

void OperationsList::saveState(QSettings& _settings)
{
	_settings.beginGroup(m_settingsGroupName);
	if (m_tableSource)
	{
		m_tableSource->saveState(_settings);
		_settings.setValue("operationListHeaderState", m_operationList->getHeader()->saveState());
	}
	_settings.endGroup();
}

void OperationsList::selectionChanged(void* _item)
{
	m_currentItem = (rtm::MemoryOperation*)_item;
	m_selectedStackTrace = m_context->m_capture->getStackTraceByIndex(m_currentItem->m_stackTraceIndex);
	emit setStackTrace(&m_selectedStackTrace,1);

	m_operationSearch->setAddress(m_currentItem->m_pointer);

	rtm::MemoryOperation* chainPrev = m_context->m_capture->getChainPrev(m_currentItem);
	bool enablePrev = false;
	if (chainPrev)
		enablePrev = (m_context->m_capture->getFilteringEnabled() == false) || m_context->m_capture->isInFilter(chainPrev);
	m_operationSearch->setPrevEnabled(enablePrev);

	rtm::MemoryOperation* chainNext = m_context->m_capture->getChainNext(m_currentItem);
	bool enableNext = false;
	if (chainNext)
		enableNext = (m_context->m_capture->getFilteringEnabled() == false) || m_context->m_capture->isInFilter(chainNext);
	m_operationSearch->setNextEnabled(enableNext);

	emit highlightTime(m_currentItem->m_operationTime);
}

void OperationsList::selectPrevious()
{
	if (m_currentItem)
		m_operationList->select(m_context->m_capture->getChainPrev(m_currentItem));
}

void OperationsList::selectNext()
{
	if (m_currentItem)
		m_operationList->select(m_context->m_capture->getChainNext(m_currentItem));
}

void OperationsList::selectNextByAddress(uint64_t _address)
{
	void* item = nullptr;
	if (m_currentItem)
	{
		uint32_t index = m_tableSource->getItemIndex(m_currentItem);
		item = m_tableSource->FindNextByAddress(_address,index);
	}
	else
	{
		item = m_tableSource->FindNextByAddress(_address,0);
	}

	if (item)
	{
		m_operationList->select(item);
		selectionChanged(item);
	}
}

void OperationsList::selectNextBySize(uint64_t _size)
{
	void* item = nullptr;
	if (m_currentItem)
	{
		uint32_t index = m_tableSource->getItemIndex(m_currentItem);
		item = m_tableSource->FindNextBySize(_size,index);
	}
	else
	{
		item = m_tableSource->FindNextBySize(_size,0);
	}

	if (item)
	{
		m_operationList->select(item);
		selectionChanged(item);
	}
}

void OperationsList::toggleLeaksOnly(bool _show)
{
	m_tableSource->prepareData(_show);
	m_operationList->resetView();		// row count changes when filtering; refresh the view & scrollbar
}
