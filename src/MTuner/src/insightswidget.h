//--------------------------------------------------------------------------//
/// Copyright 2026 Milos Tosic. All Rights Reserved.                       ///
/// License: http://www.opensource.org/licenses/BSD-2-Clause               ///
//--------------------------------------------------------------------------//

#ifndef RTM_MTUNER_INSIGHTSWIDGET_H
#define RTM_MTUNER_INSIGHTSWIDGET_H

#include <MTuner/src/insights.h>

struct CaptureContext;
namespace rtm { struct StackTrace; }
class QListWidget;
class QTextBrowser;

//--------------------------------------------------------------------------
/// Shows rule-based recommendations for the current capture. Selecting an
/// insight deep-links into the existing views (stack trace / memory timeline).
//--------------------------------------------------------------------------
class InsightsWidget : public QWidget
{
	Q_OBJECT

public:
	InsightsWidget(QWidget* _parent = 0);

	void setContext(CaptureContext* _context);

Q_SIGNALS:
	void setStackTrace(rtm::StackTrace**, int);
	void highlightTime(uint64_t);

private Q_SLOTS:
	void selectionChanged();

private:
	void rebuild();

	CaptureContext*			m_context = nullptr;
	QListWidget*			m_list    = nullptr;
	QTextBrowser*			m_detail  = nullptr;
	std::vector<Insight>	m_insights;
};

#endif // RTM_MTUNER_INSIGHTSWIDGET_H
