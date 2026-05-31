//--------------------------------------------------------------------------//
/// Copyright 2026 Milos Tosic. All Rights Reserved.                       ///
/// License: http://www.opensource.org/licenses/BSD-2-Clause               ///
//--------------------------------------------------------------------------//

#include <MTuner_pch.h>
#include <MTuner/src/heapswidget.h>
#include <MTuner/src/capturecontext.h>

HeapsWidget::HeapsWidget(QWidget* _parent, Qt::WindowFlags _flags)
	: QWidget(_parent, _flags)
	, m_currentItem(0)
	, m_context(0)
{
	ui.setupUi(this);
	
	m_treeWidget = findChild<QTreeWidget*>("treeWidget");
	connect(m_treeWidget, SIGNAL(itemSelectionChanged()), this, SLOT(selectionChanged()));
}

void HeapsWidget::changeEvent(QEvent* _event)
{
	QWidget::changeEvent(_event);
	if (_event->type() == QEvent::LanguageChange)
		ui.retranslateUi(this);
}

void HeapsWidget::setContext(CaptureContext* _context)
{
	if (m_context == _context)
		return;

	m_context = _context;

	// Reset m_currentItem before clear() (clear deletes the items it points at) and block
	// signals across the repopulate: a clear() that emits itemSelectionChanged would otherwise
	// run selectionChanged() with a dangling m_currentItem (null-deref) or emit a spurious
	// heapSelected.
	m_currentItem = 0;
	const bool blocked = m_treeWidget->blockSignals(true);
	m_treeWidget->clear();

	if (m_context)
	{
		rtm::HeapsType& heaps = m_context->m_capture->getHeaps();
		rtm::HeapsType::iterator it = heaps.begin();
		rtm::HeapsType::iterator end = heaps.end();

		while (it != end)
		{
			QTreeWidgetItem* item = new QTreeWidgetItem(QStringList()
										<< ("0x" + QString::number((qlonglong)it->first,16))
										<< QString(it->second.c_str()));
			item->setData(0, Qt::UserRole, (qlonglong)it->first);
			m_treeWidget->addTopLevelItem(item);
			++it;
		}
	}

	m_treeWidget->blockSignals(blocked);
}

void HeapsWidget::selectionChanged()
{
	QTreeWidgetItem* currItem = m_treeWidget->currentItem();
	if (m_currentItem == currItem)
	{
		m_currentItem = 0;
		m_treeWidget->setCurrentItem(0);
		emit heapSelected((uint64_t)-1);
	}
	else
	{
		m_currentItem = currItem;
		m_treeWidget->setCurrentItem(currItem);
		uint64_t address = currItem->data(0, Qt::UserRole).toULongLong();
		emit heapSelected(address);
	}
}

void HeapsWidget::setCurrentHeap(uint64_t _handle)
{
	QTreeWidgetItemIterator it(m_treeWidget);
	while (*it)
	{
		if ((*it)->data(0,Qt::UserRole).toULongLong() == _handle)
		{
			(*it)->setSelected(true);
			return;
		}
		++it;
	}
}
