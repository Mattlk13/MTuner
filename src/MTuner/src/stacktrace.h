//--------------------------------------------------------------------------//
/// Copyright 2026 Milos Tosic. All Rights Reserved.                       ///
/// License: http://www.opensource.org/licenses/BSD-2-Clause               ///
//--------------------------------------------------------------------------//

#ifndef RTM_MTUNER_STACKTRACE_H
#define RTM_MTUNER_STACKTRACE_H

#include <MTuner/.qt/qt_ui/stacktrace_ui.h>

class QSpinBox;
class QComboBox;
class QLineEdit;
class StackTrace;
struct CaptureContext;

class QToolTipper : public QObject
{
	Q_OBJECT

	StackTrace* m_stackTrace;

public:
	explicit QToolTipper(QObject* parent = NULL, StackTrace* _trace = 0)
		: QObject(parent)
		, m_stackTrace(_trace)
	{}

protected:
	bool eventFilter(QObject* obj, QEvent* event);
};

class StackTrace : public QWidget
{
	Q_OBJECT

private:
	QLabel*				m_toolTipLabel;
	rtm::StackTrace**	m_currentTrace;
	uint32_t			m_currentTraceCnt;
	uint32_t			m_currentTraceIdx;
	QTableWidget*		m_table;
	QMenu*				m_contextMenu;
	QAction*			m_actionCopy;
	QAction*			m_actionCopyAll;
	int					m_copyIndex;
	QToolButton*		m_buttonDec;
	QToolButton*		m_buttonInc;
	QSpinBox*			m_spinBox;
	QLabel*				m_totalTraces;
	QComboBox*			m_sortCombo;
	QLineEdit*			m_filterEdit;
	QLabel*				m_statLabel;
	CaptureContext*		m_context;
	rtm::StackTrace*	m_stackTrace;
	QString				m_selectedFunc;
	QString				m_settingsGroupName;

	// Filter + ranking over the (possibly thousands of) traces passing through the selected node.
	// m_view holds indices into m_currentTrace after the fuzzy filter and the chosen sort; the
	// spinbox/arrows navigate m_view, not the raw array.
	std::vector<uint32_t>	m_view;
	std::vector<QString>	m_searchText;	///< per-trace joined function names; built lazily on first filter
	bool					m_searchBuilt;
	int						m_sortMode;		///< 0 = recorded order, 1 = live bytes, 2 = allocations, 3 = total bytes
	uint64_t				m_nodeTotal;	///< sum of the active metric over all node traces (for the "% of node")

public:
	StackTrace(QWidget* _parent = 0, Qt::WindowFlags _flags = (Qt::WindowFlags)0);

	void leaveEvent(QEvent*) { m_toolTipLabel->hide(); }
	void changeEvent(QEvent* _event);
	void contextMenuEvent(QContextMenuEvent* _event);
	void setContext(CaptureContext* _context);
	void clear();
	void updateView();
	void loadState(QSettings& _settings, const QString& _name, bool _resetGeometry);
	void saveState(QSettings& _settings);
	void showToolTip(const QPoint& _pos, const QString& _itemTooltip);
	void hideToolTip();

public Q_SLOTS:
	void currentCellChanged(int _currentRow, int _currentColumn, int _previousRow, int _previousColumn);
	void setStackTrace(rtm::StackTrace** _stackTrace, int);
	void sortModeChanged(int _index);
	void filterTextChanged(const QString& _text);
	void incPressed();
	void decPressed();
	void copy();
	void copyAll();
	void copyResetIndex();

Q_SIGNALS:
	void openFile(const QString& _file, int _row, int _column);

private:
	void setCount(uint32_t _cnt);
	void rebuildView();			///< re-applies the fuzzy filter + sort, producing m_view
	void ensureSearchText();	///< lazily resolves per-trace function names for filtering
	Ui::StackTrace ui;
};

#endif // RTM_MTUNER_STACKTRACE_H
