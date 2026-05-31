//--------------------------------------------------------------------------//
/// Copyright 2026 Milos Tosic. All Rights Reserved.                       ///
/// License: http://www.opensource.org/licenses/BSD-2-Clause               ///
//--------------------------------------------------------------------------//

#include <MTuner_pch.h>
#include <MTuner/src/mtuner.h>
#include <MTuner/src/stacktrace.h>
#include <MTuner/src/capturecontext.h>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLineEdit>
#include <algorithm>

extern QString QStringColor(const QString& _string, const char* _color, bool _addColon = true);

// Fuzzy (subsequence) match: every char of _needle must appear in _haystack, in order,
// case-insensitively, not necessarily contiguously. Empty needle matches everything.
static bool fuzzyMatch(const QString& _needle, const QString& _haystack)
{
	const int needleLen = _needle.length();
	if (needleLen == 0)
		return true;

	const int hayLen = _haystack.length();
	int n = 0;
	for (int h = 0; (h < hayLen) && (n < needleLen); ++h)
		if (_haystack.at(h).toCaseFolded() == _needle.at(n).toCaseFolded())
			++n;
	return n == needleLen;
}

// Compact human-readable byte count (e.g. "12.3 MB").
static QString formatBytes(uint64_t _bytes)
{
	const char* units[] = { "B", "KB", "MB", "GB", "TB" };
	double v = (double)_bytes;
	int u = 0;
	while ((v >= 1024.0) && (u < 4)) { v /= 1024.0; ++u; }
	QLocale locale;
	return locale.toString(v, 'f', (u == 0) ? 0 : 1) + QString(" ") + QString::fromLatin1(units[u]);
}

// Sort metric (mirrors the combo item order populated in the constructor).
enum SortMode { SortOrder = 0, SortLiveBytes, SortAllocations, SortTotalBytes };

bool QToolTipper::eventFilter(QObject* _object, QEvent* _event)
{
	if (_event->type() == QEvent::ToolTip)
		return true;

	if (_event->type() == QEvent::MouseMove)
	{
		QAbstractItemView* view = qobject_cast<QAbstractItemView*>(_object->parent());
		if (!view)
			return false;

		QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(_event);
		QPoint pos = mouseEvent->pos();
		QModelIndex index = view->indexAt(pos);
		if (!index.isValid())
			return false;

		QModelIndex idxmodule	= index.siblingAtColumn(0);
		QModelIndex idxfunction	= index.siblingAtColumn(1);
		QModelIndex idxfile		= index.siblingAtColumn(2);
		QModelIndex idxline		= index.siblingAtColumn(3);

		QString textModule   = view->model()->data(idxmodule).toString();
		QString textFunction = view->model()->data(idxfunction).toString();
		QString textFile     = view->model()->data(idxfile).toString();
		QString textLine     = view->model()->data(idxline).toString();

		QString itemTooltip =
			QString("<pre><b>") + QStringColor("Func: ", "ff42a6ba", false) + textFunction + QString("</b><br>") +
			QString("<b>") + QStringColor("File: ", "ff83cf67", false) + QString("</b>") + textFile + QString(":") + textLine + QString("<br>") +
			QString("<b>") + QStringColor("Module: ", "ffefef33", false) + QString("</b>") + textModule + QString("</pre>");

		// only elided text
		if (!itemTooltip.isEmpty())
		{
			m_stackTrace->showToolTip(mouseEvent->globalPosition().toPoint(), itemTooltip);
		}
		else
		{
			m_stackTrace->hideToolTip();
			_event->ignore();
		}

		return true;
	}
	return false;
}

enum StackTraceColumns
{
	Module,
	Function,
	File,
	Line,
	Path
};

StackTrace::StackTrace(QWidget* _parent, Qt::WindowFlags _flags) : 
	QWidget(_parent, _flags)
{
	m_toolTipLabel = new QLabel();
	m_toolTipLabel->setStyleSheet("border: 1px solid gray");
	m_toolTipLabel->setWindowFlag(Qt::ToolTip);
	m_toolTipLabel->hide();

	m_context			= nullptr;
	m_currentTrace		= nullptr;
	m_currentTraceCnt	= 0;
	m_currentTraceIdx	= 0;
	m_searchBuilt		= false;
	m_sortMode			= SortOrder;
	m_nodeTotal			= 0;

	ui.setupUi(this);

	m_table = findChild<QTableWidget*>("tableWidget");
	m_table->horizontalHeader()->setHighlightSections(false);
	m_table->setGridStyle(Qt::NoPen);
	m_table->viewport()->installEventFilter(new QToolTipper(m_table, this));
	connect(m_table, SIGNAL(currentCellChanged(int, int, int, int)), this, SLOT(currentCellChanged(int, int, int, int)));

	m_buttonDec		= findChild<QToolButton*>("button_dec");
	m_buttonInc		= findChild<QToolButton*>("button_inc");
	m_spinBox		= findChild<QSpinBox*>("spinBox");
	m_totalTraces	= findChild<QLabel*>("label_total");
	m_sortCombo		= findChild<QComboBox*>("comboSort");
	m_filterEdit	= findChild<QLineEdit*>("lineEditFilter");
	m_statLabel		= findChild<QLabel*>("labelStat");

	// Order must match the SortMode enum.
	m_sortCombo->addItem(tr("Order"));
	m_sortCombo->addItem(tr("Live bytes"));
	m_sortCombo->addItem(tr("Allocations"));
	m_sortCombo->addItem(tr("Total bytes"));
	connect(m_sortCombo,  SIGNAL(currentIndexChanged(int)),     this, SLOT(sortModeChanged(int)));
	connect(m_filterEdit, SIGNAL(textChanged(const QString&)),  this, SLOT(filterTextChanged(const QString&)));

	m_actionCopy	= new QAction(QString(tr("Copy")), this);
	m_actionCopyAll	= new QAction(QString(tr("Copy all")), this);

	m_contextMenu = new QMenu();
	m_contextMenu->addAction(m_actionCopy);
	m_contextMenu->addAction(m_actionCopyAll);

	connect(m_actionCopy,		SIGNAL(triggered()), this, SLOT(copy()));
	connect(m_actionCopyAll,	SIGNAL(triggered()), this, SLOT(copyAll()));
	copyResetIndex();

	updateView();
}

void StackTrace::changeEvent(QEvent* _event)
{
	QWidget::changeEvent(_event);
	if (_event->type() == QEvent::LanguageChange)
		ui.retranslateUi(this);
}

void StackTrace::contextMenuEvent(QContextMenuEvent* _event)
{
	if (!m_context)
		return;

	QPoint pos = m_table->viewport()->mapFromGlobal(_event->globalPos());
	m_copyIndex = m_table->indexAt(pos).row();

	if (m_copyIndex != -1)
		m_contextMenu->exec(mapToGlobal(_event->pos()));
}

void StackTrace::setContext(CaptureContext* _context)
{
	m_context			= _context;
	m_currentTrace		= 0;
	m_currentTraceCnt	= 0;
	m_currentTraceIdx	= 0;
	m_searchBuilt		= false;
	m_searchText.clear();
	rebuildView();
}

void StackTrace::clear()
{
	m_selectedFunc.clear();
	m_table->setRowCount(0);
	m_table->update();
}

void StackTrace::currentCellChanged(int _currentRow, int _currentColumn, int _previousRow, int _previousColumn)
{
	RTM_UNUSED_3(_currentColumn, _previousRow, _previousColumn);
	QTableWidgetItem* lineItem = m_table->item(_currentRow, StackTraceColumns::Line);
	QTableWidgetItem* funcItem = m_table->item(_currentRow, StackTraceColumns::Function);
	QTableWidgetItem* pathItem = m_table->item(_currentRow, StackTraceColumns::Path);

	if (!lineItem || !funcItem || !pathItem)
		return;

	QString file = pathItem->text();
	int		line = lineItem->text().toInt();
	m_selectedFunc	= funcItem->text();
	emit openFile(file, line, 0);
}

void StackTrace::setStackTrace(rtm::StackTrace** _stackTrace, int _num)
{
	m_currentTrace		= _stackTrace;

	if (_stackTrace && (_stackTrace[0] == 0))
	{
		m_currentTrace	= 0;
		_num			= 0;
	}

	m_currentTraceCnt	= _num;
	m_currentTraceIdx	= 0;
	m_searchBuilt		= false;	// per-node search text is rebuilt lazily on first filter
	m_searchText.clear();

	rebuildView();
}

void StackTrace::sortModeChanged(int _index)
{
	m_sortMode = _index;
	m_currentTraceIdx = 0;
	rebuildView();
}

void StackTrace::filterTextChanged(const QString&)
{
	m_currentTraceIdx = 0;
	rebuildView();
}

void StackTrace::ensureSearchText()
{
	if (m_searchBuilt || !m_currentTrace || !m_context)
		return;

	// Resolve each trace's function names once (per node) and cache them joined, so later
	// keystrokes filter against strings instead of re-resolving. The symbol resolver caches per
	// address, so shared frames across traces are cheap.
	m_searchText.assign(m_currentTraceCnt, QString());
	for (uint32_t i=0; i<m_currentTraceCnt; ++i)
	{
		rtm::StackTrace* st = m_currentTrace[i];
		const uint32_t frames = st->m_numFrames;
		QString joined;
		for (uint32_t f=0; f<frames; ++f)
		{
			rdebug::StackFrame frame;
			m_context->resolveStackFrame(st->m_frames[f], frame);
			joined += QString::fromUtf8(frame.m_func);
			joined += QChar('\n');
		}
		m_searchText[i] = joined;
	}
	m_searchBuilt = true;
}

void StackTrace::rebuildView()
{
	m_view.clear();
	m_nodeTotal = 0;

	if (!m_currentTrace || (m_currentTraceCnt == 0))
	{
		m_currentTraceIdx = 0;
		updateView();
		return;
	}

	const QString filter = m_filterEdit ? m_filterEdit->text() : QString();

	// 1) Fuzzy filter on any frame's function name.
	if (!filter.isEmpty())
	{
		ensureSearchText();
		for (uint32_t i=0; i<m_currentTraceCnt; ++i)
			if (fuzzyMatch(filter, m_searchText[i]))
				m_view.push_back(i);
	}
	else
	{
		m_view.reserve(m_currentTraceCnt);
		for (uint32_t i=0; i<m_currentTraceCnt; ++i)
			m_view.push_back(i);
	}

	// 2) Rank by the chosen weight (descending). "Order" keeps the recorded order. The per-trace
	// stats build lazily on the first weight sort (one op pass), never at load.
	if ((m_sortMode != SortOrder) && m_context)
	{
		rtm::Capture* cap = m_context->m_capture;
		const int mode = m_sortMode;

		struct Metric {
			rtm::Capture* cap; int mode;
			uint64_t operator()(rtm::StackTrace* st) const {
				const rtm::StackTraceStats& s = cap->getStackTraceStats(st);
				if (mode == SortLiveBytes)   return s.m_liveBytes;
				if (mode == SortAllocations) return s.m_allocCount;
				if (mode == SortTotalBytes)  return s.m_totalBytes;
				return 0;
			}
		} metric{ cap, mode };

		rtm::StackTrace** traces = m_currentTrace;
		std::stable_sort(m_view.begin(), m_view.end(), [traces, &metric](uint32_t _a, uint32_t _b){
			return metric(traces[_a]) > metric(traces[_b]);
		});

		// "% of node" denominator: the metric summed over ALL of the node's traces.
		for (uint32_t i=0; i<m_currentTraceCnt; ++i)
			m_nodeTotal += metric(m_currentTrace[i]);
	}

	if (m_currentTraceIdx >= (uint32_t)m_view.size())
		m_currentTraceIdx = 0;

	updateView();
}

void StackTrace::setCount(uint32_t _cnt)
{
	m_totalTraces->setText(tr("of") + QString(" ") + QString::number(_cnt));
	m_spinBox->setValue(_cnt ? m_currentTraceIdx + 1 : 0);
	m_spinBox->setMinimum(_cnt ? 1 : 0);
	m_spinBox->setMaximum(_cnt ? _cnt : 0);
	m_spinBox->setEnabled(_cnt != 0);
	m_buttonDec->setEnabled(_cnt != 0);
	m_buttonInc->setEnabled(_cnt != 0);
}

QTableWidgetItem* makeItemWithTooltip(const QString& _string)
{
	QTableWidgetItem* item = new QTableWidgetItem(_string);
	item->setToolTip(_string);
	return item;
}

void StackTrace::updateView()
{
	const uint32_t visible = (uint32_t)m_view.size();

	if (!m_currentTrace || (visible == 0))
	{
		m_selectedFunc.clear();
		m_table->setRowCount(0);
		emit openFile("", 0, 0);
		setCount(0);
		if (m_statLabel)
			m_statLabel->clear();
		return;
	}

	setCount(visible);

	rtm::StackTrace* trace = m_currentTrace[m_view[m_currentTraceIdx]];

	// Per-trace weight readout (only meaningful when a weight metric is selected).
	if (m_statLabel)
	{
		if ((m_sortMode != SortOrder) && m_context)
		{
			const rtm::StackTraceStats& s = m_context->m_capture->getStackTraceStats(trace);
			uint64_t v = 0;
			QString txt;
			if (m_sortMode == SortLiveBytes)		{ v = s.m_liveBytes;  txt = formatBytes(v); }
			else if (m_sortMode == SortAllocations)	{ v = s.m_allocCount; txt = QLocale().toString((qulonglong)v); }
			else if (m_sortMode == SortTotalBytes)	{ v = s.m_totalBytes; txt = formatBytes(v); }

			if (m_nodeTotal > 0)
				txt += QString(" (") + QString::number((double(v) * 100.0) / double(m_nodeTotal), 'f', 1) + QString("%)");
			m_statLabel->setText(txt);
		}
		else
			m_statLabel->clear();
	}

	const uint32_t rows = trace->m_numFrames;
	m_table->model()->removeRows(0, m_table->model()->rowCount());
	m_table->setRowCount(rows);
	uint32_t selectedRow = rows;

	for (uint32_t i=0; i<rows; ++i)
	{
		uint64_t address = trace->m_frames[i];
		rdebug::StackFrame frame;
		m_context->resolveStackFrame(address, frame);

		QString func		= QString::fromUtf8(frame.m_func);
		QString symStoreDir	= QString::fromUtf8(m_context->getSymbolStoreDir().c_str());

		QTableWidgetItem* item = new QTableWidgetItem(frame.m_moduleName);
		item->setToolTip(frame.m_moduleName);

		// module, file, line, function, Path
		m_table->setItem(i, StackTraceColumns::Module,		makeItemWithTooltip(frame.m_moduleName));
		m_table->setItem(i, StackTraceColumns::Function,	makeItemWithTooltip(func));
		m_table->setItem(i, StackTraceColumns::File,		makeItemWithTooltip(QString::fromUtf8(rtm::pathGetFileName(frame.m_file))));
		m_table->setItem(i, StackTraceColumns::Line,		new QTableWidgetItem(QString::number(frame.m_line)));
		m_table->setItem(i, StackTraceColumns::Path,		makeItemWithTooltip(QString::fromUtf8(frame.m_file)));

		if (m_selectedFunc.compare(func) == 0)
			selectedRow = i;
	}

	m_table->update();
	m_table->setCurrentIndex(QModelIndex());
	if (selectedRow != rows)
	{
		m_table->selectRow(selectedRow);
		currentCellChanged(selectedRow, 0, 0, 0);
		return;
	}

	emit openFile("", 0, 0);
}

void StackTrace::loadState(QSettings& _settings, const QString& _name, bool _resetGeometries)
{
	m_settingsGroupName = _name;

	_settings.beginGroup(m_settingsGroupName);
	if (!_resetGeometries)
	{
		restoreGeometry(_settings.value("stackTraceGeometry").toByteArray());
		m_table->horizontalHeader()->restoreState(_settings.value("stackTraceHeader").toByteArray());
	}
	_settings.endGroup();
}

void StackTrace::saveState(QSettings& _settings)
{
	_settings.beginGroup(m_settingsGroupName);
	_settings.setValue("stackTraceGeometry", saveGeometry());
	_settings.setValue("stackTraceHeader", m_table->horizontalHeader()->saveState());
	_settings.endGroup();
}

void StackTrace::showToolTip(const QPoint& _pos, const QString& _itemTooltip)
{
	m_toolTipLabel->move(_pos + QPoint(15,15));
	m_toolTipLabel->setText(_itemTooltip);
	m_toolTipLabel->adjustSize();
	m_toolTipLabel->show();
}

void StackTrace::hideToolTip()
{
	m_toolTipLabel->hide();
}

void StackTrace::incPressed()
{
	const uint32_t visible = (uint32_t)m_view.size();
	// >= guards the count==0 case (count-1 would wrap to UINT_MAX and let the index run past the end).
	if ((visible == 0) || (m_currentTraceIdx >= visible - 1))
		return;
	++m_currentTraceIdx;
	updateView();
}

void StackTrace::decPressed()
{
	if (m_currentTraceIdx == 0)
		return;
	--m_currentTraceIdx;
	updateView();
}

void StackTrace::copy()
{
	QString text =
	QString(m_table->item(m_copyIndex, StackTraceColumns::Module)->text()	+ QString('\t')) +
	QString(m_table->item(m_copyIndex, StackTraceColumns::Function)->text()	+ QString('\t')) +
	QString(m_table->item(m_copyIndex, StackTraceColumns::File)->text()		+ QString('\t')) +
	QString(m_table->item(m_copyIndex, StackTraceColumns::Line)->text()		+ QString('\t')) +
	QString(m_table->item(m_copyIndex, StackTraceColumns::Path)->text()		+ QString('\n'));

	QApplication::clipboard()->setText(text);
	copyResetIndex();
}

void StackTrace::copyAll()
{
	const uint32_t rows = (uint32_t)m_table->rowCount();	// the table already holds the current (filtered/sorted) trace

	QString text;
	for (uint32_t i=0; i<rows; ++i)
	{
		text = text +
		QString(m_table->item(i, StackTraceColumns::Module)->text()		+ QString('\t')) +
		QString(m_table->item(i, StackTraceColumns::Function)->text()	+ QString('\t')) +
		QString(m_table->item(i, StackTraceColumns::File)->text()		+ QString('\t')) +
		QString(m_table->item(i, StackTraceColumns::Line)->text()		+ QString('\t')) +
		QString(m_table->item(i, StackTraceColumns::Path)->text()		+ QString('\n'));
	}

	QApplication::clipboard()->setText(text);
	copyResetIndex();
}

void StackTrace::copyResetIndex()
{
	m_copyIndex = -1;
}
