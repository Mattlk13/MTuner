//--------------------------------------------------------------------------//
/// Copyright (c) 2019 by Milos Tosic. All Rights Reserved.                ///
/// License: http://www.opensource.org/licenses/BSD-2-Clause               ///
//--------------------------------------------------------------------------//

#include <MTuner_pch.h>
#include <MTuner/src/stats.h>
#include <MTuner/src/capturecontext.h>

#include <rqt/inc/rqt.h>
#include <MTuner/src/projectsmanager.h>
#include <MTuner/src/symbolstore.h>
#include <MTuner/src/centralwidget.h>
#include <MTuner/src/external_editor.h>
#include <MTuner/src/graphwidget.h>
#include <MTuner/src/heapswidget.h>
#include <MTuner/src/moduleswidget.h>
#include <MTuner/src/histogramwidget.h>
#include <MTuner/src/stackandsource.h>
#include <MTuner/src/stats.h>
#include <MTuner/src/binloaderview.h>
#include <MTuner/src/sourceview.h>
#include <MTuner/src/graph.h>
#include <MTuner/src/gcc.h>
#include <MTuner/src/welcome.h>
#include <MTuner/src/tagtreewidget.h>
#include <MTuner/src/capturecontext.h>

Stats::Stats(QWidget* _parent, Qt::WindowFlags _flags) :
	QWidget(_parent, _flags)
{
	ui.setupUi(this);
	m_table = findChild<QTableWidget*>("tableWidget");
	m_context = NULL;
	m_table->setItemDelegate( new StatsDelegate() );
//	m_table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
	m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
	m_table->verticalHeader()->setHighlightSections(false);

}

void Stats::changeEvent(QEvent* _event)
{
	QWidget::changeEvent(_event);
	if (_event->type() == QEvent::LanguageChange)
		ui.retranslateUi(this);
}

void Stats::setContext(CaptureContext* _context)
{
	m_context = _context;
	updateUI();
}

void Stats::updateUI()
{
	if (!m_context)
	{
		clear();
		return;
	}

	QLocale locale;

	const rtm::MemoryStats& globalStats		= m_context->m_capture->getGlobalStats();

	m_table->item(0,0)->setText(locale.toString(qulonglong(globalStats.m_memoryUsage)));
	m_table->item(1,0)->setText(locale.toString(qulonglong(globalStats.m_memoryUsagePeak)));
	m_table->item(2,0)->setText(locale.toString(globalStats.m_numberOfOperations));
	m_table->item(3,0)->setText(locale.toString(globalStats.m_numberOfAllocations));
	m_table->item(4,0)->setText(locale.toString(globalStats.m_numberOfReAllocations));
	m_table->item(5,0)->setText(locale.toString(globalStats.m_numberOfFrees));
	m_table->item(6,0)->setText(locale.toString(globalStats.m_numberOfLiveBlocks));
	m_table->item(7,0)->setText(locale.toString(globalStats.m_overhead));
	m_table->item(8,0)->setText(locale.toString(globalStats.m_overheadPeak));

	const rtm::MemoryStats& snapshotStats	= m_context->m_capture->getSnapshotStats();

	m_table->item(0,1)->setText(locale.toString(qulonglong(snapshotStats.m_memoryUsage)));
	m_table->item(1,1)->setText(locale.toString(qulonglong(snapshotStats.m_memoryUsagePeak)));
	m_table->item(2,1)->setText(locale.toString(snapshotStats.m_numberOfOperations));
	m_table->item(3,1)->setText(locale.toString(snapshotStats.m_numberOfAllocations));
	m_table->item(4,1)->setText(locale.toString(snapshotStats.m_numberOfReAllocations));
	m_table->item(5,1)->setText(locale.toString(snapshotStats.m_numberOfFrees));
	m_table->item(6,1)->setText(locale.toString(snapshotStats.m_numberOfLiveBlocks));
	m_table->item(7,1)->setText(locale.toString(snapshotStats.m_overhead));
	m_table->item(8,1)->setText(locale.toString(snapshotStats.m_overheadPeak));
}

void Stats::clear()
{
	for (uint32_t i=0; i<9; ++i)
		for (uint32_t j=0; j<2; j++)
			m_table->item(i,j)->setText("");
}
