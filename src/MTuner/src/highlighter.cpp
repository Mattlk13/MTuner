//--------------------------------------------------------------------------//
/// Copyright 2026 Milos Tosic. All Rights Reserved.                       ///
/// License: http://www.opensource.org/licenses/BSD-2-Clause               ///
//--------------------------------------------------------------------------//

#include <MTuner_pch.h>
#include <MTuner/src/highlighter.h>

Highlighter::Highlighter(QTextDocument* _parent)
    : QSyntaxHighlighter(_parent)
{
	applyTheme();
}

void Highlighter::applyTheme()
{
	m_highlightingRules.clear();		// rebuilt below with the active theme's colors

	HighlightingRule rule;

	// The default keyword colors are tuned for dark backgrounds and wash out on light themes
	// (Bright Owl, Wise Green). Decide from the active background's lightness so keywords keep
	// strong contrast on any light theme.
	const QColor bg = rqt::appThemeColor("RQT_DEFAULT_BACKGROUND_COLOR", QColor(30, 30, 30));
	const bool lightTheme = bg.lightnessF() > 0.5f;
	const QColor keywordColor = lightTheme ? QColor(0, 51, 161)  : QColor(86, 156, 214);	// deep blue vs VS-dark blue
	const QColor typeColor    = lightTheme ? QColor(0, 102, 120) : QColor(51, 172, 174);	// deep teal vs light teal
	// The remaining token colors are also dark-tuned; give them darker, higher-contrast variants
	// on light themes so they don't wash out on the cream/sage backgrounds.
	const QColor commentColor = lightTheme ? QColor( 34, 122,  34) : QColor( 86, 164,  51);
	const QColor stringColor  = lightTheme ? QColor(163,  21,  21) : QColor(214, 157, 133);
	const QColor funcColor    = lightTheme ? QColor( 38, 110, 150) : QColor( 78, 201, 176);
	const QColor preprocColor = lightTheme ? QColor(136,  49, 144) : QColor(189,  99, 197);
	const QColor preproc2Color= lightTheme ? QColor(105, 105, 105) : QColor(155, 155, 155);
	// preprocessor3 (#warning/#error) stays red - reads fine on both light and dark.

	m_keywordFormat.setForeground(keywordColor);
	QStringList keywordPatterns;
	keywordPatterns << "\\bclass\\b" << "\\bconst\\b" << "\\benum\\b" << "\\bexplicit\\b"
					<< "\\bfriend\\b" << "\\binline\\b" << "\\bnew\\b" << "\\bdelete\\b"
					<< "\\bnamespace\\b" << "\\boperator\\b" << "\\bprivate\\b" << "\\bprotected\\b" << "\\bpublic\\b"
					<< "\\bsignals\\b" << "\\bbreak\\b" << "\\bcase\\b" << "\\bslots\\b" << "\\bstatic\\b" << "\\bstruct\\b" << "\\bcase\\b"
					<< "\\btemplate\\b" << "\\btypedef\\b" << "\\btypename\\b" << "\\bunion\\b" << "\\bvirtual\\b" << "\\breturn\\b" << "\\bvolatile\\b";
	foreach (const QString &pattern, keywordPatterns) {
		rule.m_pattern = QRegularExpression(pattern);
		rule.m_format = m_keywordFormat;
		m_highlightingRules.append(rule);
	}
	
	m_keywordFormat2.setForeground(typeColor);
	QStringList keywordPatterns2;
	keywordPatterns2 << "\\bchar\\b" << "\\bdouble\\b" << "\\bint\\b" << "\\blong\\b" << "\\bnamespace\\b"
					<< "\\bshort\\b" << "\\bsigned\\b" << "\\bunsigned\\b" << "\\buint8_t\\b" << "\\buint16_t\\b" << "\\buint32_t\\b" << "\\buint64_t\\b"
					<< "\\bint8_t\\b" << "\\bint16_t\\b" << "\\bint32_t\\b" << "\\bint64_t\\b"
					<< "\\bvoid\\b" << "\\bvolatile\\b" << "\\bsize_t\\b";
	foreach (const QString &pattern, keywordPatterns2) {
		rule.m_pattern = QRegularExpression(pattern);
		rule.m_format = m_keywordFormat2;
		m_highlightingRules.append(rule);
	}

	m_preprocessorFormat.setForeground(preprocColor);
	rule.m_pattern = QRegularExpression("#.*");
	rule.m_format = m_preprocessorFormat;
	m_highlightingRules.append(rule);

	m_preprocessorFormat2.setForeground(preproc2Color);
	QStringList keywordPatterns3;
	keywordPatterns3 << "\\binclude\\b" << "\\bif\\b" << "\\bifdef\\b" << "\\bifndef\\b" << "\\belif\\b"
					<< "\\bdefine\\b" << "\\bundef\\b" << "\\bifdef\\b" << "\\bifndef\\b" << "\\belse\\b" << "\\bendif\\b";
	foreach (const QString &pattern, keywordPatterns3) {
		rule.m_pattern = QRegularExpression(QString("#") + pattern);
		rule.m_format = m_preprocessorFormat2;
		m_highlightingRules.append(rule);
	}

	m_preprocessorFormat3.setForeground(QColor(255, 67, 54));
	QStringList keywordPatterns4;
	keywordPatterns4 << "\\bwarning\\b" << "\\berror\\b";
	foreach (const QString &pattern, keywordPatterns4) {
		rule.m_pattern = QRegularExpression(QString("#") + pattern);
		rule.m_format = m_preprocessorFormat3;
		m_highlightingRules.append(rule);
	}

	m_quotationFormat.setForeground(stringColor);
	rule.m_pattern = QRegularExpression("\"[^\"]*\"");
	rule.m_format = m_quotationFormat;
	m_highlightingRules.append(rule);

	m_includeFormat.setForeground(stringColor);
	rule.m_pattern = QRegularExpression("<[^>]*>");
	rule.m_format = m_includeFormat;
	m_highlightingRules.append(rule);

	m_functionFormat.setForeground(funcColor);
	rule.m_pattern = QRegularExpression("\\b[A-Za-z0-9_]+(?=\\()");
	rule.m_format = m_functionFormat;
	m_highlightingRules.append(rule);

	m_functionFormat2.setForeground(funcColor);
	rule.m_pattern = QRegularExpression("\\b[A-Za-z0-9_]+::[A-Za-z0-9_]+(?=\\()");
	rule.m_format = m_functionFormat2; 
	m_highlightingRules.append(rule);

	m_functionFormat3.setForeground(funcColor);
	rule.m_pattern = QRegularExpression("\\b[A-Za-z0-9_]+::~[A-Za-z0-9_]+(?=\\()");
	rule.m_format = m_functionFormat2;
	m_highlightingRules.append(rule);

	m_singleLineCommentFormat.setForeground(commentColor);
	rule.m_pattern = QRegularExpression("//[^\n]*");
	rule.m_format = m_singleLineCommentFormat;
	m_highlightingRules.append(rule);

	m_multiLineCommentFormat.setForeground(commentColor);

	rehighlight();		// re-apply with the (possibly changed) colors
}

void Highlighter::highlightBlock(const QString& _text)
{
	// Apply each rule to ALL occurrences on the line (not just the first match).
	foreach (const HighlightingRule &rule, m_highlightingRules) {
		QRegularExpressionMatchIterator it = rule.m_pattern.globalMatch(_text);
		while (it.hasNext()) {
			QRegularExpressionMatch match = it.next();
			setFormat(match.capturedStart(), match.capturedLength(), rule.m_format);
		}
	}

	// Multi-line /* ... */ comments, tracked across blocks via the block state.
	static const QRegularExpression commentStart("/\\*");
	static const QRegularExpression commentEnd("\\*/");

	setCurrentBlockState(0);

	int startIndex = 0;
	if (previousBlockState() != 1)
		startIndex = _text.indexOf(commentStart);

	while (startIndex >= 0) {
		QRegularExpressionMatch endMatch;
		int endIndex = _text.indexOf(commentEnd, startIndex, &endMatch);
		int commentLength;
		if (endIndex == -1) {
			setCurrentBlockState(1);
			commentLength = _text.length() - startIndex;
		} else {
			commentLength = endIndex - startIndex + endMatch.capturedLength();
		}
		setFormat(startIndex, commentLength, m_multiLineCommentFormat);
		startIndex = _text.indexOf(commentStart, startIndex + commentLength);
	}
}
