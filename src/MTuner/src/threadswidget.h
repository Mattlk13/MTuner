//--------------------------------------------------------------------------//
/// Copyright 2026 Milos Tosic. All Rights Reserved.                       ///
/// License: http://www.opensource.org/licenses/BSD-2-Clause               ///
//--------------------------------------------------------------------------//

#ifndef RTM_MTUNER_THREADSWIDGET_H
#define RTM_MTUNER_THREADSWIDGET_H

#include <MTuner/.qt/qt_ui/threadswidget_ui.h>

struct CaptureContext;

class ThreadsWidget : public QWidget
{
	Q_OBJECT

private:
	QLineEdit*		m_filter;
	QTreeWidget*	m_treeWidget;
	CaptureContext*	m_context;
	uint64_t		m_currentThreadID;	///< selected thread ID (0 = none); tracked by value so it survives list rebuilds on filtering

public:
	ThreadsWidget(QWidget* _parent = 0, Qt::WindowFlags _flags = (Qt::WindowFlags)0);

	void changeEvent(QEvent* _event);
	void setContext(CaptureContext* _context);
	void setCurrentThread(uint64_t _threadID);

public Q_SLOTS:
	void itemClicked(QTreeWidgetItem* _item, int _column);
	void filterChanged(const QString& _text);

Q_SIGNALS:
	void threadSelected(uint64_t);	///< emits the selected thread ID, or 0 to clear the filter

private:
	void repopulate(const QString& _filter);	///< rebuilds the list, keeping only fuzzy-matching threads

	Ui::ThreadsWidget ui;
};

#endif // RTM_MTUNER_THREADSWIDGET_H
