//--------------------------------------------------------------------------//
/// Copyright (c) 2019 by Milos Tosic. All Rights Reserved.                ///
/// License: http://www.opensource.org/licenses/BSD-2-Clause               ///
//--------------------------------------------------------------------------//

#include <MTuner_pch.h>
#include <MTuner/src/heapswidget.h>
#include <MTuner/src/capturecontext.h>

HeapsWidget::HeapsWidget(QWidget* _parent, Qt::WindowFlags _flags)
	: QWidget(_parent, _flags)
	, m_currentItem(0)
{
	ui.setupUi(this);
	
	m_treeWidget = findChild<QTreeWidget*>("treeWidget");
	connect(m_treeWidget, SIGNAL(itemClicked(QTreeWidgetItem*, int)), this, SLOT(itemClicked(QTreeWidgetItem*, int)));
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

	if (m_context)
	{
		m_treeWidget->clear();
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
	else
		m_treeWidget->clear();
}

void HeapsWidget::itemClicked(QTreeWidgetItem* _currentItem, int _column)
{
	RTM_UNUSED(_column);
	if (!_currentItem)
		return;

	if (m_currentItem == _currentItem)
	{
		m_currentItem = 0;
		m_treeWidget->setCurrentItem(0);
		emit heapSelected(0);
	}
	else
	{
		m_currentItem = _currentItem;
		m_treeWidget->setCurrentItem(_currentItem);
		uint64_t address = _currentItem->data(0,Qt::UserRole).toULongLong();
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
