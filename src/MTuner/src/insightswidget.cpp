//--------------------------------------------------------------------------//
/// Copyright 2026 Milos Tosic. All Rights Reserved.                       ///
/// License: http://www.opensource.org/licenses/BSD-2-Clause               ///
//--------------------------------------------------------------------------//

#include <MTuner_pch.h>
#include <MTuner/src/insightswidget.h>
#include <MTuner/src/capturecontext.h>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QTextBrowser>
#include <QtWidgets/QVBoxLayout>

InsightsWidget::InsightsWidget(QWidget* _parent) :
	QWidget(_parent)
{
	QVBoxLayout* layout = new QVBoxLayout(this);
	layout->setContentsMargins(4, 4, 4, 4);
	layout->setSpacing(4);

	m_list = new QListWidget(this);
	m_list->setSelectionMode(QAbstractItemView::SingleSelection);
	m_list->setWordWrap(true);
	m_list->setMinimumWidth(0);

	m_detail = new QTextBrowser(this);
	m_detail->setOpenExternalLinks(false);
	m_detail->setMaximumHeight(150);

	layout->addWidget(m_list, 1);
	layout->addWidget(m_detail, 0);

	connect(m_list, SIGNAL(itemSelectionChanged()), this, SLOT(selectionChanged()));
}

void InsightsWidget::setContext(CaptureContext* _context)
{
	m_context = _context;
	rebuild();
}

static QColor severityColor(Insight::Severity _s)
{
	switch (_s)
	{
		case Insight::High:		return QColor(232,  90,  90);
		case Insight::Medium:	return QColor(230, 160,  70);
		case Insight::Low:		return QColor(214, 196,  90);
		default:				return QColor(140, 165, 205);
	}
}

void InsightsWidget::rebuild()
{
	m_list->clear();
	m_detail->clear();
	m_insights.clear();

	if (!m_context || !m_context->m_capture)
		return;

	m_insights = captureInsightsAnalyze(m_context->m_capture);

	for (size_t i=0; i<m_insights.size(); ++i)
	{
		const Insight& ins = m_insights[i];
		QListWidgetItem* item = new QListWidgetItem(QString("[%1]  %2").arg(ins.m_category, ins.m_title));
		item->setForeground(severityColor(ins.m_severity));
		item->setData(Qt::UserRole, (int)i);
		m_list->addItem(item);
	}

	if (m_list->count() > 0)
		m_list->setCurrentRow(0);
}

void InsightsWidget::selectionChanged()
{
	QListWidgetItem* item = m_list->currentItem();
	if (!item)
	{
		m_detail->clear();
		return;
	}

	const int idx = item->data(Qt::UserRole).toInt();
	if ((idx < 0) || (idx >= (int)m_insights.size()))
		return;

	Insight& ins = m_insights[idx];
	m_detail->setText(ins.m_detail);

	if (ins.m_stackTrace)
		emit setStackTrace(&ins.m_stackTrace, 1);	// deep-link: show in the Stack trace dock
	if (ins.m_hasTime)
		emit highlightTime(ins.m_time);				// deep-link: highlight on the memory timeline
}
