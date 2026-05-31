//--------------------------------------------------------------------------//
/// Copyright 2026 Milos Tosic. All Rights Reserved.                       ///
/// License: http://www.opensource.org/licenses/BSD-2-Clause               ///
//--------------------------------------------------------------------------//

#include <MTuner_pch.h>
#include <MTuner/src/startpage.h>
#include <MTuner/src/version.h>
#include <QtGui/QDesktopServices>
#include <QtCore/QUrl>

namespace rqt { QColor appThemeColor(const char* _define, const QColor& _fallback); }

static QString getMessage(const QString& _string)
{
	return QString("<html><head/><body><p><span style=\" font-size:16pt; font-weight:600; color:#787896;\">") +
			_string + QString("</span></p></body></html>");
}

StartPageWidget::StartPageWidget(QWidget* _parent, Qt::WindowFlags _flags) :
	QWidget(_parent, _flags)
{
	ui.setupUi(this);
	ui.label_version->setText(getMessage(QString("v") + QString(MTunerVersion)));

	// Rudji Games logo, top-right: clickable (opens the website) and theme-aware.
	ui.label_logo->setCursor(Qt::PointingHandCursor);
	ui.label_logo->setToolTip("www.rudji.com");
	ui.label_logo->installEventFilter(this);
	updateLogo();
}

void StartPageWidget::updateLogo()
{
	// Light themes need the dark-ink logo, dark themes the light-ink one. Decide from the active
	// background lightness so it follows any theme.
	const bool light = rqt::appThemeColor("RQT_DEFAULT_BACKGROUND_COLOR", QColor(30, 30, 30)).lightnessF() > 0.5f;
	const QString path = light ? ":/MTuner/resources/images/rudji_games_logo_black.png"
							   : ":/MTuner/resources/images/rudji_games_logo.png";

	const QPixmap src(path);
	if (src.isNull())
		return;

	// Match the height of the "MTuner" title (36pt ~ 48px); render at the device pixel ratio so it
	// stays crisp on HiDPI (the source PNG is ~450px tall, so this only ever downscales).
	const qreal dpr = devicePixelRatioF() > 0.0 ? devicePixelRatioF() : 1.0;
	const int   h   = (int)(48 * dpr);
	QPixmap pm = src.scaledToHeight(h, Qt::SmoothTransformation);
	pm.setDevicePixelRatio(dpr);
	ui.label_logo->setPixmap(pm);
}

void StartPageWidget::changeEvent(QEvent* _event)
{
	QWidget::changeEvent(_event);
	if (_event->type() == QEvent::LanguageChange)
		ui.retranslateUi(this);
	else if ((_event->type() == QEvent::StyleChange) || (_event->type() == QEvent::PaletteChange))
		updateLogo();			// theme switched -> reload the matching (light/dark) logo
}

bool StartPageWidget::eventFilter(QObject* _obj, QEvent* _event)
{
	if ((_obj == ui.label_logo) && (_event->type() == QEvent::MouseButtonRelease))
	{
		QDesktopServices::openUrl(QUrl("https://www.rudji.com"));
		return true;
	}
	return QWidget::eventFilter(_obj, _event);
}
