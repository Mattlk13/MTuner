//--------------------------------------------------------------------------//
/// Copyright 2026 Milos Tosic. All Rights Reserved.                       ///
/// License: http://www.opensource.org/licenses/BSD-2-Clause               ///
//--------------------------------------------------------------------------//

#include <MTuner_pch.h>
#include <MTuner/src/gcc.h>
#include <rbase/inc/widechar.h>

GCCSetup::GCCSetup(QWidget* _parent, Qt::WindowFlags _flags) :
	QDialog(_parent, Qt::WindowSystemMenuHint | Qt::WindowTitleHint)
{
	RTM_UNUSED(_flags);

	ui.setupUi(this);

	m_currentToolchain = 0;

	m_toolchainCombo = findChild<QComboBox*>("toolchainCombo");

	m_ToolchainNameLabel = findChild<QLabel*>("labelToolchainName");
	m_ToolchainNameEdit = findChild<QLineEdit*>("lineEditToolchainName");
	m_ToolchainNameLabel->hide();
	m_ToolchainNameEdit->hide();

	m_group32				= findChild<QGroupBox*>("groupBox32");
	m_group64				= findChild<QGroupBox*>("groupBox64");
	m_groupProDGps3			= findChild<QGroupBox*>("groupBoxProDGps3");
	m_groupProDGps3->hide();

	m_leditEnv32			= findChild<QLineEdit*>("lineEditEnv32");
	m_leditBinutils32		= findChild<QLineEdit*>("lineEditBinutils32");
	m_leditPrefix32			= findChild<QLineEdit*>("lineEditPrefix32");

	m_leditEnv64			= findChild<QLineEdit*>("lineEditEnv64");
	m_leditBinutils64		= findChild<QLineEdit*>("lineEditBinutils64");
	m_leditPrefix64			= findChild<QLineEdit*>("lineEditPrefix64");

	m_leditEnvProDGps3		= findChild<QLineEdit*>("lineEditEnvProDGps3");
	m_leditBinutilsProDgps3	= findChild<QLineEdit*>("lineEditBinutilsProDGps3");

	m_labelOk64				= findChild<QLabel*>("isOkLabel64");
	m_labelOk32				= findChild<QLabel*>("isOkLabel32");
	m_labelOkProDGps3		= findChild<QLabel*>("isOkLabelProDGps3");

	m_labelFound64			= findChild<QLabel*>("labelFound64");
	m_labelFound32			= findChild<QLabel*>("labelFound32");
	m_labelFoundProDGps3	= findChild<QLabel*>("labelFoundProDGps3");

	m_toolchainCombo->setIconSize(QSize(m_toolchainCombo->size().width(), m_toolchainCombo->iconSize().height()));
}

void GCCSetup::changeEvent(QEvent* _event)
{
	QDialog::changeEvent(_event);
	if (_event->type() == QEvent::LanguageChange)
		ui.retranslateUi(this);
}

int GCCSetup::exec()
{
	toolchainSelected(m_currentToolchain);
	// fix combo box names
	for (int i=0; i<m_toolchains.length(); ++i)
	{
		if (m_toolchains[i].m_toolchain >= rmem::ToolChain::Custom1)
			m_toolchainCombo->setItemText(i, m_toolchains[i].m_name);
	}
	return QDialog::exec();
}

void GCCSetup::readSettings(QSettings& _settings)
{
	setupDefaultTC(m_toolchains);

#if RTM_DEBUG // Qt crashes on Windows/debug
	return;
#endif

	QSettings& settings = _settings;
	if (settings.childGroups().contains("GCCToolchains_5"))
	{
		settings.beginGroup("GCCToolchains_5");
		int numToolchains = settings.beginReadArray("GCCToolchainsArray");
		for (int i=0; i<numToolchains; ++i)
		{
			settings.setArrayIndex(i);

			Toolchain tc;
			tc.m_toolchain	= (rmem::ToolChain::Enum)settings.value("tcToolchain").toInt();

			for (int j=0; j<m_toolchains.size(); ++j)
			if (tc.m_toolchain == m_toolchains[j].m_toolchain)
			{
				m_toolchains[j].m_name				= settings.value("tcName").toString();
				m_toolchains[j].m_Environment32		= settings.value("tcEnv32").toString();
				m_toolchains[j].m_ToolchainPath32	= settings.value("tcPath32").toString();
				m_toolchains[j].m_ToolchainPrefix32	= settings.value("tcPrefix32").toString();
				m_toolchains[j].m_Environment64		= settings.value("tcEnv64").toString();
				m_toolchains[j].m_ToolchainPath64	= settings.value("tcPath64").toString();
				m_toolchains[j].m_ToolchainPrefix64	= settings.value("tcPrefix64").toString();
				break;
			}
		}
		settings.endArray();
		settings.endGroup();
	}
}

void  GCCSetup::writeSettings(QSettings& _settings)
{
	_settings.beginGroup("GCCToolchains_5");
	_settings.beginWriteArray("GCCToolchainsArray", m_toolchains.length());
	for (int i=0; i<m_toolchains.length(); ++i)
	{
		_settings.setArrayIndex(i);

		Toolchain& tc = m_toolchains[i];
		_settings.setValue("tcName", tc.m_name);
		_settings.setValue("tcEnv32", tc.m_Environment32);
		_settings.setValue("tcPath32", tc.m_ToolchainPath32);
		_settings.setValue("tcPrefix32", tc.m_ToolchainPrefix32);
		_settings.setValue("tcEnv64", tc.m_Environment64);
		_settings.setValue("tcPath64", tc.m_ToolchainPath64);
		_settings.setValue("tcPrefix64", tc.m_ToolchainPrefix64);
		_settings.setValue("tcToolchain", (int)tc.m_toolchain);
	}
	_settings.endArray();
	_settings.endGroup();
}

bool GCCSetup::isConfigured(rmem::ToolChain::Enum _toolchain, bool _64bit)
{
	rdebug::Toolchain tc = getToolchainInfo(_toolchain,_64bit);
	if (strcmp(tc.m_toolchainPath, "") == 0)
	{
		if (tc.m_type != rdebug::Toolchain::MSVC)
			return false;
	}
	return true;
}

rdebug::Toolchain GCCSetup::getToolchainInfo(rmem::ToolChain::Enum _toolchain, bool _64bit)
{
	rdebug::Toolchain tc;
	for (int i=0; i<m_toolchains.length(); ++i)
	{
		if (m_toolchains[i].m_toolchain == _toolchain)
		{
			resolveToolchain(m_toolchains[i], _64bit, tc);
			break;
		}
	}

	return tc;
}

rdebug::Toolchain::Type getTCType(rmem::ToolChain::Enum _toolchain)
{
	switch (_toolchain)
	{
		case rmem::ToolChain::Win_MSVC: return rdebug::Toolchain::MSVC;
		default:						return rdebug::Toolchain::GCC;
	};
}

static void fixSlashes(QString& _path, QString& _slash)
{
	// first double slashes, then single
	_path = _path.replace("//", _slash);
	_path = _path.replace("\\\\", _slash);
	_path = _path.replace("/", _slash);
	_path = _path.replace("\\", _slash);
}

bool GCCSetup::resolveToolchain(Toolchain& _toolchain, bool _64bit, rdebug::Toolchain& _tc)
{
	QString append10 = "nm";
	QString append11 = "llvm-nm";
	QString append20 = "addr2line";
	QString append21 = "llvm-symbolizer";
	QString append30 = "c++filt";
	QString append31 = "llvm-cxxfilt";

#if RTM_PLATFORM_WINDOWS
	append10 += QString(".exe"); append11 += QString(".exe");
	append20 += QString(".exe"); append21 += QString(".exe");
	append30 += QString(".exe"); append31 += QString(".exe");
#endif

	QString envVar = _64bit ? _toolchain.m_Environment64		: _toolchain.m_Environment32;
	QString tcPath = _64bit ? _toolchain.m_ToolchainPath64		: _toolchain.m_ToolchainPath32;
	QString prefix = _64bit ? _toolchain.m_ToolchainPrefix64	: _toolchain.m_ToolchainPrefix32;

	// try to match absolute path + prefix + postfix
	QString basePath = tcPath + QString("/");

	QString fullPath = basePath + prefix;

	fullPath = fullPath.replace("//", "/");
	fullPath = fullPath.replace("\\", "/");

	if ((QFileInfo(fullPath + append10).exists() || QFileInfo(fullPath + append11).exists()) &&
		(QFileInfo(fullPath + append20).exists() || QFileInfo(fullPath + append21).exists()) &&
		(QFileInfo(fullPath + append30).exists() || QFileInfo(fullPath + append31).exists()))
	{
		rtm::strlCpy(_tc.m_toolchainPath,	RTM_NUM_ELEMENTS(_tc.m_toolchainPath),	fullPath.toUtf8().constData());
		rtm::strlCpy(_tc.m_toolchainPrefix,	RTM_NUM_ELEMENTS(_tc.m_toolchainPrefix),	prefix.toUtf8().constData());

		_tc.m_type				= getTCType(_toolchain.m_toolchain);
		return true;
	}

	// try to match environment variable + relative path + postfix
	if (envVar.length())
	{
#if RTM_PLATFORM_WINDOWS
		std::wstring env = (wchar_t*)envVar.utf16();
		wchar_t* envData = _wgetenv(env.c_str());
		if (!envData)
			return false;
		basePath = QString::fromUtf16((const char16_t*)envData) + QString("/");
#else
		auto env = envVar.toUtf8();
		const char *const envData = getenv(env.constData());
		if (!envData)
			return false;
		basePath = QString::fromUtf8(envData) + QString("/");
#endif

#if RTM_PLATFORM_WINDOWS
		QString slash("\\");
#else
		QString slash("/");
#endif

		basePath += tcPath + QString("/");
		fixSlashes(basePath, slash);

		fullPath = basePath + prefix;
		fixSlashes(fullPath, slash);

		if ((QFileInfo(fullPath + append10).exists() || QFileInfo(fullPath + append11).exists()) &&
			(QFileInfo(fullPath + append20).exists() || QFileInfo(fullPath + append21).exists()) &&
			(QFileInfo(fullPath + append30).exists() || QFileInfo(fullPath + append31).exists()))
		{
			rtm::strlCpy(_tc.m_toolchainPath,	RTM_NUM_ELEMENTS(_tc.m_toolchainPath),	basePath.toUtf8().constData());
			rtm::strlCpy(_tc.m_toolchainPrefix,	RTM_NUM_ELEMENTS(_tc.m_toolchainPrefix),	prefix.toUtf8().constData());

			_tc.m_type				= getTCType(_toolchain.m_toolchain);
			return true;
		}
	}

	// not found
	return false;
}

void GCCSetup::setupDefaultTC(QVector<Toolchain>& _toolchains)
{
	Toolchain minGW;
	minGW.m_name					= "MinGW";
	minGW.m_Environment32			= "MINGW";
	minGW.m_ToolchainPath32			= "";
	minGW.m_ToolchainPrefix32		= "";
	minGW.m_Environment64			= "MINGW64";
	minGW.m_ToolchainPath64			= "";
	minGW.m_ToolchainPrefix64		= "";
	minGW.m_toolchain				= rmem::ToolChain::Win_gcc;

	Toolchain androidARM;
	androidARM.m_name				= "Android ARM";
	androidARM.m_Environment32		= "ANDROID_NDK_ROOT";
	androidARM.m_ToolchainPath32	= "/toolchains/arm-linux-androideabi-4.9/prebuilt/windows-x86_64/bin/";
	androidARM.m_ToolchainPrefix32	= "arm-linux-androideabi-";
	androidARM.m_Environment64		= "ANDROID_NDK_ROOT";
	androidARM.m_ToolchainPath64	= "/toolchains/arm-linux-androideabi-4.9/prebuilt/windows-x86_64/bin/";
	androidARM.m_ToolchainPrefix64	= "arm-linux-androideabi-";
	androidARM.m_toolchain			= rmem::ToolChain::Android_arm;

	Toolchain androidMIPS;
	androidMIPS.m_name				= "Android MIPS";
	androidMIPS.m_Environment32		= "ANDROID_NDK_ROOT";
	androidMIPS.m_ToolchainPath32	= "/toolchains/mips64el-linux-android-4.9/prebuilt/windows-x86_64/bin/";
	androidMIPS.m_ToolchainPrefix32	= "mips64el-linux-android-";
	androidMIPS.m_Environment64		= "ANDROID_NDK_ROOT";
	androidMIPS.m_ToolchainPath64	= "/toolchains/mips64el-linux-android-4.9/prebuilt/windows-x86_64/bin/";
	androidMIPS.m_ToolchainPrefix64	= "mips64el-linux-android-";
	androidMIPS.m_toolchain			= rmem::ToolChain::Android_mips;

	Toolchain androidX86;
	androidX86.m_name				= "Android x86";
	androidX86.m_Environment32		= "ANDROID_NDK_ROOT";
	androidX86.m_ToolchainPath32	= "/toolchains/aarch64-linux-android-4.9/prebuilt/windows-x86_64/bin/";
	androidX86.m_ToolchainPrefix32	= "aarch64-linux-android-";
	androidX86.m_Environment64		= "ANDROID_NDK_ROOT";
	androidX86.m_ToolchainPath64	= "/toolchains/aarch64-linux-android-4.9/prebuilt/windows-x86_64/bin/";
	androidX86.m_ToolchainPrefix64	= "aarch64-linux-android-";
	androidX86.m_toolchain			= rmem::ToolChain::Android_x86;

	_toolchains.append(minGW);
	_toolchains.append(androidARM);
	_toolchains.append(androidMIPS);
	_toolchains.append(androidX86);
	
	for (int i=0; i<9; ++i)
	{
		rmem::ToolChain::Enum tc = (rmem::ToolChain::Enum)(rmem::ToolChain::Custom1 + i);
		Toolchain ctc;
		ctc.m_name				= QString("Custom Toolchain ") + QString::number(i+1);
		ctc.m_toolchain			= tc;

		_toolchains.append(ctc);
	}
}

void GCCSetup::toolchainSelected(int _index)
{
	m_currentToolchain = _index;
	if (m_toolchains[_index].m_toolchain >= rmem::ToolChain::Custom1)
	{
		m_ToolchainNameLabel->show();
		m_ToolchainNameEdit->show();
	}
	else
	{
		m_ToolchainNameLabel->hide();
		m_ToolchainNameEdit->hide();
	}

	m_group32->show();
	m_groupProDGps3->hide();
	m_group32->setEnabled(true);

	Toolchain& tc = m_toolchains[m_currentToolchain];

	m_ToolchainNameEdit->setText(tc.m_name);
	m_leditEnv32->setText(tc.m_Environment32);
	m_leditBinutils32->setText(tc.m_ToolchainPath32);
	m_leditPrefix32->setText(tc.m_ToolchainPrefix32);

	// special case - ProDG ps3
	m_leditEnvProDGps3->setText(tc.m_Environment32);
	m_leditBinutilsProDgps3->setText(tc.m_ToolchainPath32);

	m_leditEnv64->setText(tc.m_Environment64);
	m_leditBinutils64->setText(tc.m_ToolchainPath64);
	m_leditPrefix64->setText(tc.m_ToolchainPrefix64);

	setLabels();
}

void GCCSetup::toolchainRenamed(QString _text)
{
	m_toolchains[m_currentToolchain].m_name = _text;
	if (m_toolchains[m_currentToolchain].m_toolchain >= rmem::ToolChain::Custom1)
		m_toolchainCombo->setItemText(m_currentToolchain, _text);
}

void GCCSetup::envEdited32(QString _text)
{
	Toolchain& tc = m_toolchains[m_currentToolchain];
	tc.m_Environment32 = _text;
	setLabels();
}

void GCCSetup::pathEdited32(QString _text)
{
	Toolchain& tc = m_toolchains[m_currentToolchain];
	tc.m_ToolchainPath32 = _text;
	setLabels();
}

void GCCSetup::pathBrowse32()
{
	QString path = QFileDialog::getExistingDirectory(this, tr("Select folder with binutils"));
	if (path.length()>0)
		m_leditBinutils32->setText(path);
}

void GCCSetup::prefixEdited32(QString _text)
{
	Toolchain& tc = m_toolchains[m_currentToolchain];
	tc.m_ToolchainPrefix32 = _text;
	setLabels();
}

void GCCSetup::envEdited64(QString _text)
{
	Toolchain& tc = m_toolchains[m_currentToolchain];
	tc.m_Environment64 = _text;
	setLabels();
}

void GCCSetup::pathEdited64(QString _text)
{
	Toolchain& tc = m_toolchains[m_currentToolchain];
	tc.m_ToolchainPath64 = _text;
	setLabels();
}

void GCCSetup::pathBrowse64()
{
	QString caption = tr("Select folder with binutils");
	Toolchain& tc = m_toolchains[m_currentToolchain];

	QString path = QFileDialog::getExistingDirectory(this, caption);
	if (path.length()>0)
		m_leditBinutils64->setText(path);
}

void GCCSetup::prefixEdited64(QString _text)
{
	Toolchain& tc = m_toolchains[m_currentToolchain];
	tc.m_ToolchainPrefix64 = _text;
	setLabels();
}

void GCCSetup::envEditedProDGps3(QString _text)
{
	Toolchain& tc = m_toolchains[m_currentToolchain];
	tc.m_Environment32 = _text;
	setLabels();
}

void GCCSetup::pathEditedProDGps3(QString _text)
{
	Toolchain& tc = m_toolchains[m_currentToolchain];
	tc.m_ToolchainPath32 = _text;
	setLabels();
}

void GCCSetup::pathBrowseProDGps3()
{
	QString path = QFileDialog::getExistingDirectory(this, tr("Select folder with ps3bin.exe"));
	if (path.length()>0)
		m_leditBinutilsProDgps3->setText(path);
}

void GCCSetup::setLabels()
{
	const QString ok = "<html><head/><body><p><img src=\":/MTuner/resources/images/ok.png\"/></p></body></html>";
	const QString notok = "<html><head/><body><p><img src=\":/MTuner/resources/images/notok.png\"/></p></body></html>";

	Toolchain& tc = m_toolchains[m_currentToolchain];

	rmem::ToolChain::Enum tc32 = tc.m_toolchain;

	bool config64 = isConfigured( tc.m_toolchain, true );
	bool config32 = isConfigured( tc32, false );

	m_labelOk64->setText( config64 ? ok : notok );
	m_labelOk32->setText( config32 ? ok : notok );
	m_labelOkProDGps3->setText( config32 ? ok : notok );

	m_labelFound64->setText( config64 ? tr("toolchain found!") : tr("toolchain not found!") );
	m_labelFound32->setText( config32 ? tr("toolchain found!") : tr("toolchain not found!") );
}
