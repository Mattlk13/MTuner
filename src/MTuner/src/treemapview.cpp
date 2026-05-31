//--------------------------------------------------------------------------//
/// Copyright 2026 Milos Tosic. All Rights Reserved.                       ///
/// License: http://www.opensource.org/licenses/BSD-2-Clause               ///
//--------------------------------------------------------------------------//

#include <MTuner_pch.h>
#include <MTuner/src/treemapview.h>
#include <MTuner/src/treemap.h>
#include <MTuner/src/capturecontext.h>
#include <rqt/inc/rqt.h>

static QFont s_sizeFont(QFont("Arial", 7));
static QFont s_toolTipFont(QFont("Arial", 8));

// The classic teal was tuned for (and looks best on) the dark "MTuner Dark" / "Shanghai Night"
// themes, so keep it there; every other theme follows its render background.
static inline bool treeMapUsesClassicTeal()
{
	const rqt::AppStyle::Enum style = rqt::appGetStyle();
	return (style == rqt::AppStyle::RTM) || (style == rqt::AppStyle::Shanghai);
}

QColor treeMapCellColor()
{
	if (treeMapUsesClassicTeal())
		return QColor(33, 80, 90);
	if (rqt::appGetStyle() == rqt::AppStyle::Monokai)
		// Monokai's render background is near-black, which makes the tiles read as too dark; lift it.
		return rqt::appThemeColor("RQT_RENDER_BACKGROUND_COLOR", QColor(30, 31, 27)).lighter(190);
	return rqt::appThemeColor("RQT_RENDER_BACKGROUND_COLOR", QColor(33, 80, 90));
}

// Linear blend of two colors: _t=0 -> _a, _t=1 -> _b.
static inline QColor blendColor(const QColor& _a, const QColor& _b, float _t)
{
	return QColor((int)(_a.red()   + (_b.red()   - _a.red())   * _t),
				  (int)(_a.green() + (_b.green() - _a.green()) * _t),
				  (int)(_a.blue()  + (_b.blue()  - _a.blue())  * _t));
}

QString QStringColor(const QString& _string, const char* _color, bool _addColon = true);

static inline uint64_t getTotalMem(std::vector<TreeMapNode>& _nodes, int _start, int _end)
{
	uint64_t sum = 0;
	for (int i=_start; i<=_end; ++i)
		sum += _nodes[i].m_size;
	return sum;
}

void sliceLayout(std::vector<TreeMapNode>& _nodes, int _start, int _end, QRectF& _rect)
{
	float total = getTotalMem(_nodes, _start, _end);
	float a = 0.0;

	if (_rect.bottom() < _rect.top())
		_rect.setBottom(_rect.top());

	if (_rect.right() < _rect.left())
		_rect.setLeft(_rect.right());

	bool bVertical = (_rect.right() - _rect.left()) < (_rect.bottom() - _rect.top());
	bool bOrderAsc = true;

	if (total == 0.0)
		return;

	for (int i=_start; i<=_end; ++i) 
	{
		TreeMapNode& child = _nodes[i];
		QRectF& itemRect = _nodes[i].m_rect;
		float b = float(child.m_size) / total;
		if (bVertical)
		{
			itemRect.setLeft(_rect.left());
			itemRect.setRight(_rect.right());
			if (bOrderAsc)
				itemRect.setTop(_rect.top() + (_rect.bottom() - _rect.top()) * a);
			else
				itemRect.setTop(_rect.top() + (_rect.bottom() - _rect.top()) * (1.0 - a - b));
			itemRect.setBottom(itemRect.top() + (_rect.bottom() - _rect.top()) * b);
		}
		else 
		{
			if (bOrderAsc)
				itemRect.setLeft(_rect.left() + (_rect.right() - _rect.left()) * a);
			else
				itemRect.setLeft(_rect.left() + (_rect.right() - _rect.left()) * (1.0 - a - b));
			itemRect.setRight(itemRect.left() + (_rect.right() - _rect.left()) * b);
			itemRect.setTop(_rect.top());
			itemRect.setBottom(_rect.bottom());
		}
		a += b;
	}
}

static inline float getNormAspect(float _big, float _small, float _a, float _b)
{
	float x = (_big*_b) / (_small*_a/_b);
	if (x < 1.0)
		return 1.0/x;
	return x;
}

static inline bool sortMapItems(const TreeMapNode& _in1, const TreeMapNode& _in2)
{
	return _in1.m_size > _in2.m_size;
}

void calcLines(const std::vector<TreeMapNode>& _nodes, QVector<QLineF>& _lines)
{
	_lines.clear();
	_lines.reserve(static_cast<int>(_nodes.size() * 2));
	for (const TreeMapNode& node : _nodes)
	{
		_lines.push_back(QLineF(node.m_rect.bottomLeft(), node.m_rect.bottomRight()));
		_lines.push_back(QLineF(node.m_rect.topRight(), node.m_rect.bottomRight()));
	}
}

void squaredLayout(std::vector<TreeMapNode>& _nodes, int _start, int _end, QRectF& _rect)
{
	if (_start > _end)
		return;

	if (_end - _start < 2)
	{
		sliceLayout(_nodes, _start, _end, _rect);
		return;
	}
	
	float x = _rect.left();
	float y = _rect.top();
	float w = _rect.right() - _rect.left();
	float h = _rect.bottom() - _rect.top();
	float dblTotal = getTotalMem(_nodes, _start, _end);

	if (dblTotal == 0.0)
		return;

	int iMid = _start;
	float a = float(_nodes[_start].m_size) / dblTotal;
	float b = a;

	if (w < h)
	{
		while (iMid < _end)
		{
			float dblAspect = getNormAspect(h, w, a, b);
			float q = float(_nodes[iMid + 1].m_size) / dblTotal;
			if (getNormAspect(h, w, a, b + q) > dblAspect)
				break;
			b += q;
			++iMid;
		}
		QRectF rcSliced( x, y, w, h*b );
		QRectF rcSquared( x, y+h*b, w, h*(1.0-b) );
		sliceLayout(_nodes, _start, iMid, rcSliced);
		squaredLayout(_nodes, iMid + 1, _end, rcSquared);
	}
	else
	{
		while (iMid < _end)
		{
			float dblAspect = getNormAspect(w, h, a, b);
			float q = float(_nodes[iMid + 1].m_size) / dblTotal;
			if (getNormAspect(w, h, a, b + q) > dblAspect)
				break;
		 b += q;
			++iMid;
		}
		QRectF rcSlice( x, y, w*b, h );
		QRectF rcSquared( x + w*b, y, w*(1.0-b), h );
		sliceLayout(_nodes, _start, iMid, rcSlice);
		squaredLayout(_nodes, iMid + 1, _end, rcSquared);
	}
}

static inline uint64_t getNodeValueByType(TreeMapNode& _tree, uint32_t _type)
{
	// Source fields are signed (int64_t usage / int32_t overhead) and can go
	// negative for filtered call sites whose frees exceed allocs in the window.
	// Clamp to 0 so a negative value doesn't become a huge uint64_t that swallows
	// the whole treemap.
	int64_t v = 0;
	switch (_type)
	{
		case 0: v = _tree.m_tree->m_memUsage;		break;
		case 1: v = _tree.m_tree->m_memUsagePeak;	break;
		case 2: v = _tree.m_tree->m_overhead;		break;
		case 3: v = _tree.m_tree->m_overheadPeak;	break;
	};
	return (v < 0) ? 0 : (uint64_t)v;
}

TreeMapView::TreeMapView(QWidget* _parent) :
	QGraphicsView(_parent)
{
	m_context		= NULL;
	m_highlightNode	= NULL;
	m_clickedNode	= NULL;
	m_mapType		= 0;
	m_item			= NULL;	// set later via setItem(); guard redraw() until then (resizeEvent can fire first)
	setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	setMouseTracking(true);
	m_toolTipLabel = new QLabel;
	m_toolTipLabel->setWindowFlag(Qt::ToolTip);
	// Force a dark tooltip background regardless of theme: the tooltip's rich-text uses fixed
	// light/saturated label colors, which are unreadable on the light themes' default light tooltip.
	m_toolTipLabel->setStyleSheet("QLabel { background-color: rgb(30,30,30); color: rgb(230,230,230); border: 1px solid rgb(90,90,90); padding: 2px; }");

	s_sizeFont.setStyleHint(QFont::Monospace);
	s_toolTipFont.setStyleHint(QFont::Monospace);
}

void TreeMapView::setContext(CaptureContext* _context)
{
	m_context = _context;
	buildTree();
}

void TreeMapView::setMapType(uint32_t _type)
{
	m_mapType = _type;
	m_tree.clear();
	m_treeLines.clear();
	buildTree();
	if (m_item)
		m_item->redraw();
	repaint();
}

void TreeMapView::updateHighlight(const QPoint& _pos)
{
	QPoint scenePos = mapToScene(_pos).toPoint();

	m_highlightNode = 0;
	for (size_t i=0; i<m_tree.size(); ++i)
	{
		if (!m_tree[i].m_rect.contains(scenePos))
			continue;

		if (m_highlightNode == &m_tree[i])
			break;

		m_highlightNode = &m_tree[i];

		QString str =	QString("<pre>") +
								QStringColor(tr("Total size"), "ff42a6ba") + QStringColor(m_locale.toString(qulonglong(m_highlightNode->m_size)), "ffffff33", false) + QString("<br>") +
								QStringColor(tr("Operations"), "ff83cf67") + m_locale.toString(qulonglong(m_highlightNode->m_tree->m_opCount[rtm::StackTraceTree::Alloc] + m_highlightNode->m_tree->m_opCount[rtm::StackTraceTree::Realloc] + m_highlightNode->m_tree->m_opCount[rtm::StackTraceTree::Free])) + QString("<br><br>") +
								QStringColor(tr("  Allocs"), "ffffffff") + m_locale.toString(qulonglong(m_highlightNode->m_tree->m_opCount[rtm::StackTraceTree::Alloc])) + QString("<br>") +
								QStringColor(tr("Reallocs"), "ffffffff") + m_locale.toString(qulonglong(m_highlightNode->m_tree->m_opCount[rtm::StackTraceTree::Realloc])) + QString("<br>") +
								QStringColor(tr("   Frees"), "ffffffff") + m_locale.toString(qulonglong(m_highlightNode->m_tree->m_opCount[rtm::StackTraceTree::Free])) + QString("</pre>");
		m_toolTipLabel->setText(str);
		if (m_item)
			m_item->redraw();
		invalidateScene();
		return;
	}
}

void TreeMapView::buildTreeRecurse(rtm::StackTraceTree* _tree)
{
	if (_tree->m_children.size() == 0)
	{
		TreeMapNode node;

		node.m_tree		= _tree;
		node.m_size		= getNodeValueByType( node, m_mapType );

		m_tree.push_back(node);
	}

	rtm::StackTraceTree::ChildNodes& children = _tree->m_children;
	rtm::StackTraceTree::ChildNodes::iterator it  = children.begin();
	rtm::StackTraceTree::ChildNodes::iterator end = children.end();
	while (it != end)
	{
		buildTreeRecurse(*it);
		++it;
	}
}

void TreeMapView::buildTree()
{
	m_tree.clear();
	// m_tree is rebuilt (and reallocated) below; the hover/click nodes point into it, so clear them
	// to avoid dangling pointers that leave a phantom cell stuck highlighted/selected.
	const bool hadSelection = (m_clickedNode != NULL);
	m_highlightNode	= NULL;
	m_clickedNode	= NULL;
	if (hadSelection)
		emit setStackTrace(NULL, 0);	// selected cell is gone (filter/map-type/reload) - clear downstream views
	if (!(m_context && m_context->m_capture))
		return;

	bool filtered = m_context->m_capture->getFilteringEnabled();
	const rtm::StackTraceTree& tree = filtered ? m_context->m_capture->getStackTraceTreeFiltered() : m_context->m_capture->getStackTraceTree();
	rtm::StackTraceTree* rootNode = const_cast<rtm::StackTraceTree*>(&tree);
	
	buildTreeRecurse(rootNode);
	std::sort(m_tree.begin(), m_tree.end(), sortMapItems);
}

void TreeMapView::resizeEvent(QResizeEvent* _event)
{
	RTM_UNUSED(_event);
	if (m_item)
		m_item->redraw();
	invalidateScene();
}

void TreeMapView::mousePressEvent(QMouseEvent* _event)
{
	QGraphicsView::mousePressEvent(_event);
}

void TreeMapView::mouseMoveEvent(QMouseEvent* _event)
{
	TreeMapNode* prev = m_highlightNode;
	updateHighlight(_event->pos());

	if (m_highlightNode)
	{
		m_toolTipLabel->move(QCursor::pos() + QPoint(15,15));
		m_toolTipLabel->setFont(s_toolTipFont);
		m_toolTipLabel->adjustSize();
		if(m_toolTipLabel->isHidden())
			m_toolTipLabel->show();
	}
	else
		m_toolTipLabel->hide();

	QGraphicsView::mouseMoveEvent(_event);

	// Per-instance hover tracking (a static would be shared across capture tabs); repaint when the
	// hovered cell changes - including moving onto a gap, so the previous hover is cleared.
	if (prev != m_highlightNode)
		repaint();
}

void TreeMapView::mouseReleaseEvent(QMouseEvent* _event)
{
	if (_event->button() == Qt::LeftButton)
	{
		if (m_highlightNode && (m_clickedNode == m_highlightNode))
		{
			// Clicking the already-selected cell clears the selection (toggle off).
			m_clickedNode = NULL;
			emit setStackTrace(NULL, 0);
			repaint();
		}
		else if (m_highlightNode)
		{
			m_clickedNode = m_highlightNode;

			rtm::StackTrace** trace = &m_highlightNode->m_tree->m_stackTraceList;
			emit setStackTrace(trace, 1);

			const uint32_t numOps =	m_highlightNode->m_tree->m_opCount[rtm::StackTraceTree::Alloc] +
									m_highlightNode->m_tree->m_opCount[rtm::StackTraceTree::Free]  +
									m_highlightNode->m_tree->m_opCount[rtm::StackTraceTree::Realloc];
			if (numOps == 1)
				emit highlightTime(m_highlightNode->m_tree->m_minTime);
			else
				emit highlightRange(m_highlightNode->m_tree->m_minTime, m_highlightNode->m_tree->m_maxTime);

			repaint();
		}
		else
		{
			// Clicked empty space: clear any selection (repaint so the darker cell is restored).
			emit setStackTrace(NULL, 0);
			m_clickedNode = NULL;
			repaint();
		}
	}

	QGraphicsView::mouseReleaseEvent(_event);
}

void TreeMapView::enterEvent(QEnterEvent*)
{
	repaint();
}

void TreeMapView::leaveEvent(QEvent*)
{
	m_highlightNode		= NULL;
	m_toolTipLabel->hide();

	repaint();
}

TreeMapGraphicsItem::TreeMapGraphicsItem(TreeMapView* _treeView, CaptureContext* _context, QLocale* _locale)
{
	m_oldRect				= QRectF(0,0,0,0);
	m_treeView				= _treeView;
	m_context				= _context;
	m_locale				= _locale;
	_treeView->setItem(this);
	setCacheMode(QGraphicsItem::NoCache);
	setAcceptHoverEvents(true);
}

void TreeMapGraphicsItem::redraw()
{
	m_oldRect = QRectF(0,0,0,0);
}

QRectF TreeMapGraphicsItem::boundingRect() const
{
	// Cover the scene region paint() actually draws into (it lays out via mapToScene of the
	// viewport). Using the view widget's layout position as scene coords was wrong whenever
	// the view had a non-zero offset / scroll.
	return m_treeView->mapToScene(QRect(0, 0, m_treeView->width(), m_treeView->height())).boundingRect();
}

static inline void drawBlockText(const QString& _text, QPainter* _painter, int _fontHeight, const QFontMetrics& _metrics, QRectF& _rect, bool _highlight, const QColor& _textColor, const QColor& _highlightTextColor)
{
	if ((_rect.height() <= _fontHeight))
		return;

	int width = _metrics.horizontalAdvance(_text);
	if (_rect.width() - width <= 6.0f)
		return;

	_painter->setPen(_highlight ? _highlightTextColor : _textColor);

	QRectF textRect = _rect.adjusted(3.0f,0,-3.0f,14-_rect.height());
	_painter->setFont(s_sizeFont);
	_painter->drawText(textRect,_text);
}

void TreeMapGraphicsItem::paint(QPainter* _painter, const QStyleOptionGraphicsItem* _item, QWidget* _widget)
{
	RTM_UNUSED(_item);
	RTM_UNUSED(_widget);
	std::vector<TreeMapNode>& tree = m_treeView->getTree();
	QVector<QLineF>& lines = m_treeView->getTreeLines();

	QSize s = m_treeView->size();
	QRectF rect = m_treeView->mapToScene(QRect(0,0,s.width(),s.height())).boundingRect();
	if (m_oldRect != rect)
	{
		squaredLayout(tree, 0, (int)tree.size()-1, rect);
		calcLines(tree, lines);
		m_oldRect = rect;
	}

	TreeMapNode* highlight = m_treeView->getHighlightNode();
	TreeMapNode* clicked   = m_treeView->getClickedNode();

	// Palette (re-read each paint; appThemeColor's cache invalidates on theme change). The classic
	// teal is kept for MTuner Dark / Shanghai Night; other themes follow their palette.
	const bool classic = treeMapUsesClassicTeal();
	const QColor blockBg = treeMapCellColor();

	QColor gridLine, clickedCol, textCol, textOnHi, hoverA, hoverB;
	if (classic)
	{
		gridLine   = QColor(0, 0, 0);
		// Selected was near-identical to the tile (33,80,90); use a clearly brighter blue-teal so the
		// selection stands out, with a soft lighter-teal hover below it (the two stay distinct).
		clickedCol = QColor(46, 125, 150);
		textCol    = QColor(255, 255, 255);
		textOnHi   = QColor(102, 217, 239);		// cyan highlighted-cell text (replaces the old yellow tone)
		hoverA     = QColor(50, 112, 126);
		hoverB     = QColor(40, 95, 107);
	}
	else
	{
		gridLine   = rqt::appThemeColor("RQT_BORDER_COLOR",              QColor(20, 50, 55));
		clickedCol = rqt::appThemeColor("RQT_SELECTED_BACKGROUND_COLOR", QColor(33, 70, 80));
		textCol    = rqt::appThemeColor("RQT_DEFAULT_TEXT_COLOR",        QColor(Qt::white));
		textOnHi   = textCol;					// hover is subtle, so normal text stays readable

		const rqt::AppStyle::Enum style = rqt::appGetStyle();
		if (style == rqt::AppStyle::Monokai)
		{
			const QColor accent = rqt::appThemeColor("RQT_HOVER_BACKGROUND_COLOR", QColor(176, 64, 104));
			// Hover = the menu highlight color (the accent); slight gradient for depth.
			hoverA     = accent;
			hoverB     = accent.darker(112);
			// Selected = a clearly less-pronounced (muted toward the tile) variant of the same pink,
			// so hover and selection are distinguishable.
			clickedCol = blendColor(accent, blockBg, 0.55f);
			textOnHi   = rqt::appThemeColor("RQT_HOVER_TEXT_COLOR", QColor(255, 255, 255));	// readable on the accent
		}
		else
		{
			if (style == rqt::AppStyle::WiseGreen)
				// The default selected color reads too brown; use a sage-green tint from the accent.
				clickedCol = rqt::appThemeColor("RQT_HOVER_BACKGROUND_COLOR", QColor(95, 111, 82)).lighter(150);

			// Toned-down hover: a gentle shift of the block background, not the bright accent.
			const bool darkBg = blockBg.lightnessF() < 0.5f;
			hoverA = darkBg ? blockBg.lighter(140) : blockBg.darker(114);
			hoverB = darkBg ? blockBg.lighter(118) : blockBg.darker(106);
		}
	}

	// Fill the painted region with the block background (also covers the scene background brush).
	_painter->fillRect(rect, blockBg);

	_painter->setPen(QPen(gridLine, 1.0, Qt::SolidLine));
	_painter->drawLines(lines.data(), lines.size());

	if (clicked && (clicked != highlight))
	{
		_painter->setBrush(clickedCol);
		_painter->drawRect(clicked->m_rect);
	}

	if (highlight)
	{
		QRectF highlightRect = highlight->m_rect;
		QLinearGradient gr(highlightRect.topLeft(), highlightRect.bottomRight());
		gr.setColorAt(0.0f, hoverA);
		gr.setColorAt(1.0f, hoverB);
		_painter->setBrush(gr);
		_painter->drawRect(highlightRect);
	}

	static QFontMetrics	metrics(s_sizeFont);
	int s_fontHeight = metrics.height();
	_painter->setFont(s_sizeFont);
	for (size_t i=0; i<tree.size(); ++i)
	{
		TreeMapNode& info = tree[i];
		QRectF& nodeRect = info.m_rect;

		// Skip nodes too small to display text
		if (nodeRect.height() <= s_fontHeight)
			continue;

		QString text = m_locale->toString(qulonglong(info.m_size));
		drawBlockText(text, _painter, s_fontHeight, metrics, nodeRect, &info == highlight, textCol, textOnHi);
	}
}
