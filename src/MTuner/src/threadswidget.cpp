//--------------------------------------------------------------------------//
/// Copyright 2026 Milos Tosic. All Rights Reserved.                       ///
/// License: http://www.opensource.org/licenses/BSD-2-Clause               ///
//--------------------------------------------------------------------------//

#include <MTuner_pch.h>
#include <MTuner/src/threadswidget.h>
#include <MTuner/src/capturecontext.h>

// Fuzzy (subsequence) match: every character of _needle must appear in _haystack, in order,
// case-insensitively, but not necessarily contiguously (fzf/Sublime-style). An empty needle
// matches everything. Cheap and allocation-free.
static bool fuzzyMatch(const QString& _needle, const QString& _haystack)
{
	const int needleLen = _needle.length();
	if (needleLen == 0)
		return true;

	const int hayLen = _haystack.length();
	int n = 0;
	for (int h = 0; (h < hayLen) && (n < needleLen); ++h)
	{
		if (_haystack.at(h).toCaseFolded() == _needle.at(n).toCaseFolded())
			++n;
	}
	return n == needleLen;
}

ThreadsWidget::ThreadsWidget(QWidget* _parent, Qt::WindowFlags _flags)
	: QWidget(_parent, _flags)
	, m_filter(0)
	, m_treeWidget(0)
	, m_context(0)
	, m_currentThreadID(0)
{
	ui.setupUi(this);

	m_filter = findChild<QLineEdit*>("lineEdit");
	connect(m_filter, SIGNAL(textChanged(const QString&)), this, SLOT(filterChanged(const QString&)));

	m_treeWidget = findChild<QTreeWidget*>("treeWidget");
	// itemClicked (not itemSelectionChanged) so rebuilding the list while filtering, and
	// programmatic re-selection, never emit a spurious threadSelected.
	connect(m_treeWidget, SIGNAL(itemClicked(QTreeWidgetItem*, int)), this, SLOT(itemClicked(QTreeWidgetItem*, int)));
}

void ThreadsWidget::changeEvent(QEvent* _event)
{
	QWidget::changeEvent(_event);
	if (_event->type() == QEvent::LanguageChange)
		ui.retranslateUi(this);
}

void ThreadsWidget::setContext(CaptureContext* _context)
{
	if (m_context == _context)
		return;

	m_context = _context;
	m_currentThreadID = 0;

	// New capture: reset the filter text (without re-triggering filterChanged) and rebuild.
	const bool blocked = m_filter->blockSignals(true);
	m_filter->clear();
	m_filter->blockSignals(blocked);

	repopulate(QString());
}

void ThreadsWidget::filterChanged(const QString& _text)
{
	repopulate(_text);
}

void ThreadsWidget::repopulate(const QString& _filter)
{
	m_treeWidget->clear();

	if (!m_context)
	{
		m_filter->setEnabled(false);
		return;
	}

	m_filter->setEnabled(true);

	// One row per thread that performed an operation in the capture. The thread ID always
	// exists; the name is shown when one was captured (explicit rmemSetThreadName or, on
	// Windows, an auto-detected SetThreadDescription), otherwise the cell is left blank.
	const std::vector<uint64_t>& threadIds = m_context->m_capture->getThreadIds();
	QTreeWidgetItem* curItem = 0;
	for (size_t i=0; i<threadIds.size(); ++i)
	{
		const uint64_t id = threadIds[i];
		const QString name = QString::fromUtf8(m_context->m_capture->getThreadName(id).c_str());
		const QString idHex = "0x" + QString::number((qulonglong)id, 16);

		// Fuzzy-match on the name; for unnamed threads fall back to the hex ID so they remain
		// findable (e.g. typing part of the ID).
		const QString& target = name.isEmpty() ? idHex : name;
		if (!fuzzyMatch(_filter, target))
			continue;

		QTreeWidgetItem* item = new QTreeWidgetItem(QStringList() << idHex << name);
		item->setData(0, Qt::UserRole, (qulonglong)id);
		m_treeWidget->addTopLevelItem(item);

		if (id == m_currentThreadID)
			curItem = item;
	}

	// Re-highlight the selected thread if it survived the filter. setCurrentItem does not emit
	// itemClicked, so the capture's filter is left untouched when the row is hidden.
	m_treeWidget->setCurrentItem(curItem);
}

void ThreadsWidget::itemClicked(QTreeWidgetItem* _item, int _column)
{
	RTM_UNUSED(_column);
	if (!_item)
		return;

	const uint64_t id = _item->data(0, Qt::UserRole).toULongLong();
	if (id == m_currentThreadID)
	{
		// Clicking the already-selected thread clears the filter (mirrors the Heaps dock).
		m_currentThreadID = 0;
		m_treeWidget->setCurrentItem(0);
		emit threadSelected(0);
	}
	else
	{
		m_currentThreadID = id;
		m_treeWidget->setCurrentItem(_item);
		emit threadSelected(id);
	}
}

void ThreadsWidget::setCurrentThread(uint64_t _threadID)
{
	m_currentThreadID = _threadID;

	if (_threadID == 0)
	{
		m_treeWidget->setCurrentItem(0);
		return;
	}

	QTreeWidgetItemIterator it(m_treeWidget);
	while (*it)
	{
		if ((*it)->data(0, Qt::UserRole).toULongLong() == _threadID)
		{
			m_treeWidget->setCurrentItem(*it);	// does not emit itemClicked
			(*it)->setSelected(true);
			return;
		}
		++it;
	}
}
