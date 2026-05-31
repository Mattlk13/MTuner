//--------------------------------------------------------------------------//
/// Copyright 2026 Milos Tosic. All Rights Reserved.                       ///
/// License: http://www.opensource.org/licenses/BSD-2-Clause               ///
//--------------------------------------------------------------------------//

#include <MTuner_pch.h>
#include <MTuner/src/symbolstore.h>
#include <QtCore/QStandardPaths>
#include <QtCore/QCoreApplication>

#if RTM_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

SymbolStore::SymbolStore(QWidget* _parent, Qt::WindowFlags _flags)
	: QDialog(_parent, Qt::WindowSystemMenuHint | Qt::WindowTitleHint)
{
	RTM_UNUSED(_flags);
	ui.setupUi(this);

	m_publicStore	= findChild<QLineEdit*>("textPublicStore");
	m_localStore	= findChild<QLineEdit*>("textLocalStore");
	m_srcRegistry	= findChild<QCheckBox*>("checkRegistry");
	m_buttonDefault	= findChild<QToolButton*>("buttonDefault");
	m_buttonBrowse	= findChild<QToolButton*>("buttonBrowse");

	// When no local store is set, downloaded symbols are cached in the persistent default
	// folder. Show it as placeholder so the dialog reflects where symbols actually go.
	m_localStore->setPlaceholderText(defaultSymbolCacheDir());

	m_srcRegistry->setChecked(false);

#if RTM_PLATFORM_WINDOWS
	wchar_t buffer[4096];
	bool enableRegistry = GetEnvironmentVariableW(L"_NT_SYMBOL_PATH", (LPWSTR)buffer, RTM_NUM_ELEMENTS(buffer)) > 0;
	m_hasRegistryEntry = enableRegistry;
	m_srcRegistry->setEnabled(enableRegistry);
#else
	m_srcRegistry->setEnabled(false);
#endif
}

void SymbolStore::changeEvent(QEvent* _event)
{
	QDialog::changeEvent(_event);
	if (_event->type() == QEvent::LanguageChange)
		ui.retranslateUi(this);
}

QString	SymbolStore::getSymbolStoreString() const
{
	QString ret("");

	if (m_srcRegistry->isChecked())
	{
#if RTM_PLATFORM_WINDOWS
		wchar_t buffer[4096];
		GetEnvironmentVariableW(L"_NT_SYMBOL_PATH", (LPWSTR)buffer, RTM_NUM_ELEMENTS(buffer));
		ret = QString::fromWCharArray(buffer);
#endif
	}

	// The local symbol store field also doubles as one or more ';'-separated directories
	// to search directly for PDBs - e.g. point it at your Qt/SDK bin folder when its PDBs
	// aren't sitting next to the loaded binaries (DIA already treats ';' as a separator).
	if (!m_localStore->text().isEmpty())
	{
		if (ret.length())
			ret = ret + QString(";");
		ret = ret + m_localStore->text();
	}

	// Default to the Microsoft public symbol server when none is configured, so system
	// and third-party modules (ntdll, kernel32, ucrtbase, ...) can be resolved.
	QString publicUrl = m_publicStore->text();
	if (publicUrl.isEmpty())
		publicUrl = "https://msdl.microsoft.com/download/symbols";

	// http server need a local cache folder to store the symbol
	if (publicUrl.contains(QRegularExpression("https?://")))
	{
		if (ret.length())
			ret = ret + QString(";");

		ret = ret + QString("SRV*");
		// Cache downloaded symbols in a persistent per-user folder by default. Using %TEMP%
		// (the old behaviour) meant Windows disk cleanup wiped the cache, so every module's
		// PDB was re-downloaded on each run - slow on large captures (e.g. UnrealEditor).
		ret = ret + QDir::toNativeSeparators(getEffectiveCacheDir());

		ret = ret + QString("*");
		ret = ret + publicUrl;
	}

	return ret;
}

QString SymbolStore::defaultSymbolCacheDir()
{
	// Persistent, per-user, machine-local cache (symbol caches can grow large and should not roam).
	// AppLocalDataLocation has been observed to come back empty on some setups; in that case fall
	// back to %LOCALAPPDATA%/%APPDATA% (built from the environment) and finally to a folder next to
	// the executable - but NEVER the TEMP dir. The old TEMP fallback made the cache volatile
	// (Windows Disk Cleanup / Storage Sense wipe %TEMP%), so symbols were re-downloaded every run.
	QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);

	if (base.isEmpty())
	{
		QByteArray env = qgetenv("LOCALAPPDATA");
		if (env.isEmpty())
			env = qgetenv("APPDATA");
		if (!env.isEmpty())
		{
			QString dir = QString::fromLocal8Bit(env);
			const QString org = QCoreApplication::organizationName();
			if (!org.isEmpty())
				dir = QDir(dir).absoluteFilePath(org);
			const QString app = QCoreApplication::applicationName();
			base = QDir(dir).absoluteFilePath(app.isEmpty() ? QStringLiteral("MTuner") : app);
		}
	}

	if (base.isEmpty())	// last resort: alongside the executable, still persistent
		base = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("symbols"));

	return QDir::toNativeSeparators(QDir(base).absoluteFilePath(QStringLiteral("SymbolCache")));
}

QString SymbolStore::getEffectiveCacheDir() const
{
	// The local-store field doubles as a ';'-separated search-path list (see getSymbolStoreString).
	// A multi-path value (or an empty one) is not a usable single download cache, so fall back to
	// the persistent default; only a single directory is used directly as the cache. This also
	// keeps the SRV*<cache>*<url> token well-formed and avoids a false "cache not writable" prompt.
	const QString local = m_localStore->text();
	if (local.isEmpty() || local.contains(QLatin1Char(';')))
		return defaultSymbolCacheDir();
	return local;
}

QString	SymbolStore::getLocalStore() const
{
	return m_localStore->text();
}

void SymbolStore::setLocalStore(const QString& _localStore)
{
	m_localStore->setText(_localStore);
}

QString	SymbolStore::getPublicStore() const
{
	return m_publicStore->text();
}

void SymbolStore::setPublicStore(const QString& _publicStore)
{
	m_publicStore->setText(_publicStore);
}

bool SymbolStore::isRegistryChecked() const
{
	return m_srcRegistry->isChecked();
}

void SymbolStore::setChecked(bool _registry)
{
	if (m_hasRegistryEntry && _registry)
		m_srcRegistry->setChecked(_registry);
}

void SymbolStore::save()
{
	m_restoreRegistryChecked	= m_srcRegistry->isChecked();
	m_restorePublicStore		= m_publicStore->text();
	m_restoreLocalStore			= m_localStore->text();
}

void SymbolStore::selectLocalStore()
{
	QString dir = QFileDialog::getExistingDirectory(this, tr("select local symbol store directory"), 
		"", QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

	if (!dir.isEmpty())
		m_localStore->setText(QDir::toNativeSeparators(dir));
}

void SymbolStore::defaultSymbolServer()
{
	m_publicStore->setText("https://msdl.microsoft.com/download/symbols");
}

void SymbolStore::visitMSDN()
{
	QDesktopServices::openUrl(QUrl("https://msdn.microsoft.com/en-us/library/ff537994(v=vs.85).aspx", QUrl::TolerantMode));
}

int SymbolStore::exec()
{
	show();
	setFixedHeight(height());
	return QDialog::exec();
}

void SymbolStore::accept()
{
	QDialog::accept();
}

void SymbolStore::reject()
{
	m_srcRegistry->setChecked(m_restoreRegistryChecked);
	m_publicStore->setText(m_restorePublicStore);
	m_localStore->setText(m_restoreLocalStore);
	QDialog::reject();
}
