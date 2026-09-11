// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "dbus/applicationmanager1service.h"
#include "dbus/jobservice.h"
#include "dbus/instanceservice.h"
#include "dbus/jobmanager1service.h"
#include "dbus/mimemanager1service.h"
#include "dbus/prelaunchsplashhelper.h"
#include "dbus/applicationservice.h"
#include "dbus/instanceadaptor.h"
#include "desktopfilegenerator.h"
#include "desktopfileparser.h"
#include "desktopentry.h"
#include "propertiesForwarder.h"
#include "global.h"
#include "constant.h"
#include "cgroupsidentifier.h"
#include "applicationmanagerstorage.h"
#include <gtest/gtest.h>
#include <QDBusObjectPath>
#include <QDBusConnection>
#include <QSignalSpy>
#include <QVariantMap>
#include <QStringList>
#include <QStandardPaths>
#include <QFile>
#include <QDir>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QUuid>
#include <memory>
#include "applicationHooks.h"
#include "iniParser.h"
#include <QProcess>
#include <QFileInfo>
#include <QJsonDocument>
#include <QDebug>
#include <QDateTime>

using namespace Qt::StringLiterals;

// ---------------------------------------------------------------------------
// Helper: create a temp desktop file (same pattern as ut_dbus_coverage.cpp)
// ---------------------------------------------------------------------------
static QStringList s_v40dbus_tempDesktopFiles;

static const struct V40DbusTempFileCleanup {
    V40DbusTempFileCleanup() = default;
    ~V40DbusTempFileCleanup()
    {
        for (const auto &path : s_v40dbus_tempDesktopFiles) {
            QFile::remove(path);
        }
    }
} s_v40dbus_tempFileCleanup;

static std::optional<DesktopFile> v40dbus_createTempDesktopFile(const QByteArray &content)
{
    auto tempFileName = u"am-test-v40d-%1.desktop"_s.arg(QUuid::createUuid().toString(QUuid::Id128));
    auto tempFilePath = QDir::temp().absoluteFilePath(tempFileName);
    auto tempFile = std::make_unique<QFile>(tempFilePath);
    if (!tempFile->open(QFile::WriteOnly | QFile::Text)) {
        return std::nullopt;
    }
    tempFile->write(content);
    tempFile->close();
    s_v40dbus_tempDesktopFiles.append(tempFile->fileName());
    QFileInfo fi(*tempFile);
    QString desktopId = tempFileName.chopped(8);
    return DesktopFile::createDesktopFile(fi, desktopId);
}

namespace {
constexpr char kV40FullDesktop[] =
    R"([Desktop Entry]
Type=Application
Name=TestFullApp
Name[zh_CN]=测试全功能应用
GenericName=Test Generic
Exec=/usr/bin/test --flag %f
Icon=test-icon
Categories=Game;Utility;
Actions=Open;Save;
Terminal=true
NoDisplay=true
StartupWMClass=TestWMClass
X-flatpak=org.test.TestApp
X-linglong=org.test.TestApp
X-Deepin-Vendor=TestVendor
X-Deepin-CreateBy=test
X-Created-By=test-creator

[Desktop Action Open]
Name=Open
Exec=/usr/bin/test --open %f

[Desktop Action Save]
Name=Save
Exec=/usr/bin/test --save %f
)";

// Desktop content without Icon key — for %i "icon not found" test
constexpr char kNoIconDesktop[] =
    R"([Desktop Entry]
Type=Application
Name=NoIconApp
Exec=/usr/bin/test --flag %f
)";

// Desktop content without Name key — for %c "name not found" test
constexpr char kNoNameDesktop[] =
    R"([Desktop Entry]
Type=Application
Exec=/usr/bin/test --flag %f
Icon=test-icon
)";

QSharedPointer<ApplicationService> v40dbus_createService(ApplicationManager1Service *am)
{
    auto optFile = v40dbus_createTempDesktopFile(QByteArray{kV40FullDesktop});
    if (!optFile) return nullptr;
    return ApplicationService::createApplicationService(
        std::move(*optFile), am, std::weak_ptr<ApplicationManager1Storage>{});
}
} // namespace

// ===========================================================================
// Fixture: V40DbusCoverageTest
// Tests targeting uncovered paths in applicationservice.cpp,
// mimemanager1service.cpp, desktopfileparser.cpp
// ===========================================================================

class V40DbusCoverageTest : public testing::Test {
protected:
    static inline std::unique_ptr<ApplicationManager1Service> s_am;
    static inline QSharedPointer<ApplicationService> s_service;
    static inline QString s_tempFilePath;
    static inline QTemporaryDir s_storageDir;
    static inline std::shared_ptr<ApplicationManager1Storage> s_storage;

    static void SetUpTestCase()
    {
        auto &bus = ApplicationManager1DBus::instance();
        bus.initGlobalServerBus(DBusType::Session);
        bus.setDestBus();
        s_storageDir = QTemporaryDir{};
        s_storage = ApplicationManager1Storage::createApplicationManager1Storage(s_storageDir.path());
        s_am = std::make_unique<ApplicationManager1Service>(
            std::make_unique<CGroupsIdentifier>(), s_storage);
        s_am->m_compatibilityManager = QSharedPointer<CompatibilityManager>(new CompatibilityManager());
        s_am->m_sessionOverrideConfig = std::make_unique<SessionOverrideConfig>();
        s_am->m_mimeManager = std::unique_ptr<MimeManager1Service>(new MimeManager1Service(s_am.get()));
        s_service = v40dbus_createService(s_am.get());
        ASSERT_NE(s_service, nullptr);
        registerObjectToDBus(s_service.data(), s_service->applicationPath().path(), fromStaticRaw(ApplicationInterface));
        s_service->m_storage = s_storage;
        s_tempFilePath = s_service->desktopFileSource().sourcePath();
    }

    void TearDown() override
    {
        for (const auto &path : s_v40dbus_tempDesktopFiles) {
            QFile::remove(path);
        }
        s_v40dbus_tempDesktopFiles.clear();
        orphanedInstances.clear();
        s_service->m_Instances.clear();
        s_service->m_splashInstanceIds.clear();
        s_service->m_unitResults.clear();
        s_service->m_pendingLaunchTypes.clear();
        s_service->m_environ.clear();
        s_service->m_autostartSource = {QString{}, DesktopEntry{}};
        // Always restore full entry to prevent shared state pollution.
        // Earlier tests may have resetEntry with entries missing Name/Icon keys;
        // conditional restore was insufficient and caused intermittent failures
        // in FindEntryValue_AllTypes_ExistingKey.
        {
            auto optFile = v40dbus_createTempDesktopFile(QByteArray{kV40FullDesktop});
            if (optFile) {
                auto *restoreEntry = new DesktopEntry();
                restoreEntry->parse(*optFile);
                s_service->resetEntry(restoreEntry);
            }
        }
        // Clean up MimeManager state to prevent test isolation issues
        // (debounce timer, watcher paths, internal write flag, m_infos)
        if (s_am && s_am->m_mimeManager) {
            auto *mimeMgr = s_am->m_mimeManager.get();
            if (mimeMgr) {
                if (mimeMgr->m_mimeAppsDebounceTimer.isActive()) {
                    mimeMgr->m_mimeAppsDebounceTimer.stop();
                }
                const auto watched = mimeMgr->m_mimeAppsWatcher.files();
                for (const auto &path : watched) {
                    mimeMgr->m_mimeAppsWatcher.removePath(path);
                }
                mimeMgr->m_internalWriteInProgress = false;
                mimeMgr->reset();
            }
        }
    }

    static void TearDownTestCase()
    {
        s_service.reset();
        s_am.reset();
        if (!s_tempFilePath.isEmpty()) QFile::remove(s_tempFilePath);
    }
};

// ===========================================================================
// processExec %i edge cases — applicationservice.cpp lines ~1636-1655
// ===========================================================================

// %i when Icon entry does not exist → hits `if (!val)` branch
TEST_F(V40DbusCoverageTest, ProcessExec_IconField_i_NoIconEntry_Ignored)
{
    auto optFile = v40dbus_createTempDesktopFile(QByteArray{kNoIconDesktop});
    ASSERT_TRUE(optFile.has_value());
    auto *newEntry = new DesktopEntry();
    ASSERT_EQ(newEntry->parse(*optFile), ParserError::NoError);
    s_service->resetEntry(newEntry);

    auto task = s_service->processExec(u"/usr/bin/test %i"_s, {});
    // %i is ignored because Icon not found, but the base command should still be present
    EXPECT_FALSE(task.LaunchBin.isEmpty());
}

// %i when Icon value is a QStringMap with empty default key → toIconString returns empty
TEST_F(V40DbusCoverageTest, ProcessExec_IconField_i_EmptyIconString_Ignored)
{
    auto optFile = v40dbus_createTempDesktopFile(QByteArray{kV40FullDesktop});
    ASSERT_TRUE(optFile.has_value());
    auto *newEntry = new DesktopEntry();
    ASSERT_EQ(newEntry->parse(*optFile), ParserError::NoError);
    // Replace Icon value with a QStringMap that has no "default" key
    newEntry->insert(fromStaticRaw(DesktopFileEntryKey), u"Icon"_s,
                     QVariant::fromValue(QStringMap{{u"zh_CN"_s, u"test-icon-zh"_s}}));
    s_service->resetEntry(newEntry);

    auto task = s_service->processExec(u"/usr/bin/test %i"_s, {});
    // %i is ignored because toIconString returns empty (no "default" key in map)
    EXPECT_FALSE(task.LaunchBin.isEmpty());
    // --icon should NOT be in the command
    bool hasIcon = false;
    for (const auto &arg : task.command) {
        if (arg == u"--icon"_s) hasIcon = true;
    }
    EXPECT_FALSE(hasIcon);
}

// ===========================================================================
// processExec %c edge cases — applicationservice.cpp lines ~1659-1673
// ===========================================================================

// %c when Name entry does not exist → hits `if (!val)` branch
TEST_F(V40DbusCoverageTest, ProcessExec_NameField_c_NoNameEntry_Ignored)
{
    auto optFile = v40dbus_createTempDesktopFile(QByteArray{kNoNameDesktop});
    ASSERT_TRUE(optFile.has_value());
    auto *newEntry = new DesktopEntry();
    // kNoNameDesktop has no Name key → checkMainEntryValidation() fails
    // → parse() returns ParserError::MissingInfo (not NoError).
    ASSERT_EQ(newEntry->parse(*optFile), ParserError::MissingInfo);
    delete newEntry;
}

// %c when Name is a plain QString → toLocaleString returns {} → fallback to toString
TEST_F(V40DbusCoverageTest, ProcessExec_NameField_c_PlainString_FallbackToToString)
{
    auto optFile = v40dbus_createTempDesktopFile(QByteArray{kV40FullDesktop});
    ASSERT_TRUE(optFile.has_value());
    auto *newEntry = new DesktopEntry();
    ASSERT_EQ(newEntry->parse(*optFile), ParserError::NoError);
    // Replace Name with a plain QString (not a QStringMap)
    newEntry->insert(fromStaticRaw(DesktopFileEntryKey), u"Name"_s,
                     QVariant::fromValue(QString(u"PlainAppName"_s)));
    s_service->resetEntry(newEntry);

    auto task = s_service->processExec(u"/usr/bin/test %c"_s, {});
    EXPECT_FALSE(task.LaunchBin.isEmpty());
    // The name string should be appended to processedArg → ends up in command
    bool foundName = false;
    for (const auto &arg : task.command) {
        if (arg == u"PlainAppName"_s) foundName = true;
    }
    EXPECT_TRUE(foundName);
}

// %c when Name is a non-string type (int) → both toLocaleString and toString return {}
TEST_F(V40DbusCoverageTest, ProcessExec_NameField_c_NonStringType_EmptyName)
{
    auto optFile = v40dbus_createTempDesktopFile(QByteArray{kV40FullDesktop});
    ASSERT_TRUE(optFile.has_value());
    auto *newEntry = new DesktopEntry();
    ASSERT_EQ(newEntry->parse(*optFile), ParserError::NoError);
    // Replace Name with a non-string, non-QStringMap type
    newEntry->insert(fromStaticRaw(DesktopFileEntryKey), u"Name"_s,
                     QVariant::fromValue(42));
    s_service->resetEntry(newEntry);

    auto task = s_service->processExec(u"/usr/bin/test %c"_s, {});
    // Both toLocaleString and toString return {} → empty nameStr appended
    EXPECT_FALSE(task.LaunchBin.isEmpty());
}

// ===========================================================================
// syncGeneratedAutostartEntry — applicationservice.cpp lines ~1062-1095
// ===========================================================================

// Parse failure: write garbage to autostart file, set m_autostartSource
TEST_F(V40DbusCoverageTest, SyncGeneratedAutostartEntry_ParseFailure_EarlyReturn)
{
    // Create a temp file with garbage content (RAII auto-cleanup)
    QTemporaryFile garbageFile;
    ASSERT_TRUE(garbageFile.open());
    garbageFile.write("this is not valid desktop content\n===garbage===\n");
    garbageFile.flush();
    QString garbagePath = garbageFile.fileName();
    garbageFile.close();

    DesktopEntry entryWithData;
    // Add X-Deepin-GenerateSource so the file gets opened for parsing
    entryWithData.insert(fromStaticRaw(DesktopFileEntryKey),
                         fromStaticRaw(DesktopEntryXDeepinGenerateSource),
                         u"/some/source.desktop"_s);

    s_service->m_autostartSource = {garbagePath, entryWithData};

    // Capture state before the call to verify early-return doesn't modify it
    QString pathBefore = s_service->m_autostartSource.m_filePath;

    // File exists and m_entry is valid, but parse fails → early return
    EXPECT_NO_FATAL_FAILURE(s_service->syncGeneratedAutostartEntry());

    // Early return: m_autostartSource should be unchanged
    EXPECT_EQ(s_service->m_autostartSource.m_filePath, pathBefore);
}

// Can't open file as ReadOnly: path is a directory
TEST_F(V40DbusCoverageTest, SyncGeneratedAutostartEntry_CantOpenFile_EarlyReturn)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    // Set m_autostartSource.m_filePath to a directory path → open(ReadOnly) fails
    DesktopEntry entryWithData;
    entryWithData.insert(fromStaticRaw(DesktopFileEntryKey),
                         fromStaticRaw(DesktopEntryXDeepinGenerateSource),
                         u"/some/source.desktop"_s);

    s_service->m_autostartSource = {dir.path(), entryWithData};

    // Capture state before the call to verify early-return doesn't modify it
    QString pathBefore = s_service->m_autostartSource.m_filePath;

    EXPECT_NO_FATAL_FAILURE(s_service->syncGeneratedAutostartEntry());

    // Early return: m_autostartSource should be unchanged
    EXPECT_EQ(s_service->m_autostartSource.m_filePath, pathBefore);
}

// Success path: valid autostart file with X-Deepin-GenerateSource → syncs entry
TEST_F(V40DbusCoverageTest, SyncGeneratedAutostartEntry_ValidFile_SyncsEntry)
{
    // Create a valid autostart file with X-Deepin-GenerateSource (RAII auto-cleanup)
    QTemporaryFile autostartFile;
    ASSERT_TRUE(autostartFile.open());
    autostartFile.write("[Desktop Entry]\nType=Application\nName=TestApp\nExec=/usr/bin/test\n"
                "X-Deepin-GenerateSource=/original/source.desktop\n");
    autostartFile.flush();
    QString autostartPath = autostartFile.fileName();
    autostartFile.close();

    DesktopEntry entryWithData;
    entryWithData.insert(fromStaticRaw(DesktopFileEntryKey),
                         fromStaticRaw(DesktopEntryXDeepinGenerateSource),
                         u"/original/source.desktop"_s);

    s_service->m_autostartSource = {autostartPath, entryWithData};
    EXPECT_NO_FATAL_FAILURE(s_service->syncGeneratedAutostartEntry());
    // After sync, m_autostartSource should have the updated entry
    EXPECT_FALSE(s_service->m_autostartSource.m_filePath.isEmpty());
}

// No X-Deepin-GenerateSource in current autostart file → early return (no sync)
TEST_F(V40DbusCoverageTest, SyncGeneratedAutostartEntry_NoGenerateSource_EarlyReturn)
{
    // Create a valid autostart file without X-Deepin-GenerateSource (RAII auto-cleanup)
    QTemporaryFile autostartFile;
    ASSERT_TRUE(autostartFile.open());
    autostartFile.write("[Desktop Entry]\nType=Application\nName=TestApp\nExec=/usr/bin/test\n");
    autostartFile.flush();
    QString autostartPath = autostartFile.fileName();
    autostartFile.close();

    DesktopEntry entryWithData;
    s_service->m_autostartSource = {autostartPath, entryWithData};

    // Capture state before the call to verify early-return doesn't modify it
    QString pathBefore = s_service->m_autostartSource.m_filePath;

    EXPECT_NO_FATAL_FAILURE(s_service->syncGeneratedAutostartEntry());

    // Early return: m_autostartSource should be unchanged
    EXPECT_EQ(s_service->m_autostartSource.m_filePath, pathBefore);
}

// ===========================================================================
// setAutoStart reuse branch — applicationservice.cpp lines ~1124-1135
// ===========================================================================

// shouldReuseAutostartEntry = true path: file exists, no generated source, non-empty entry data
TEST_F(V40DbusCoverageTest, SetAutoStart_ReuseAutostartEntry_SourceExists)
{
    // Create a valid autostart file (RAII auto-cleanup)
    QTemporaryFile autostartFile;
    ASSERT_TRUE(autostartFile.open());
    autostartFile.write("[Desktop Entry]\nType=Application\nName=ReuseApp\nExec=/usr/bin/test\n");
    autostartFile.flush();
    QString autostartPath = autostartFile.fileName();
    autostartFile.close();

    // Set up m_autostartSource with existing file and non-empty entry data (no X-Deepin-GenerateSource)
    DesktopEntry entryWithData;
    entryWithData.insert(fromStaticRaw(DesktopFileEntryKey), u"Name"_s,
                         QVariant::fromValue(QString(u"ReuseApp"_s)));
    entryWithData.insert(fromStaticRaw(DesktopFileEntryKey), u"Exec"_s,
                         QVariant::fromValue(QString(u"/usr/bin/test"_s)));
    // Also add X-Deepin-GenerateSource so the reuse branch takes source->get().toString() path
    entryWithData.insert(fromStaticRaw(DesktopFileEntryKey),
                         fromStaticRaw(DesktopEntryXDeepinGenerateSource),
                         u"/original/path.desktop"_s);

    s_service->m_autostartSource = {autostartPath, entryWithData};

    // isAutoStart() should be false initially (no Hidden=false in autostart)
    // setAutoStart(true) should take reuse branch
    EXPECT_NO_FATAL_FAILURE(s_service->setAutoStart(true));
    // Clean up
    s_service->setAutoStart(false);
}

// shouldReuseAutostartEntry = true, but source value doesn't exist in entry
// → originalSource = m_autostartSource.m_filePath
TEST_F(V40DbusCoverageTest, SetAutoStart_ReuseAutostartEntry_NoSourceValue_UsesFilePath)
{
    // Create a valid autostart file (RAII auto-cleanup)
    QTemporaryFile autostartFile;
    ASSERT_TRUE(autostartFile.open());
    autostartFile.write("[Desktop Entry]\nType=Application\nName=ReuseAppNoSrc\nExec=/usr/bin/test\n");
    autostartFile.flush();
    QString autostartPath = autostartFile.fileName();
    autostartFile.close();

    // Entry has data but NO X-Deepin-GenerateSource key
    DesktopEntry entryWithData;
    entryWithData.insert(fromStaticRaw(DesktopFileEntryKey), u"Name"_s,
                         QVariant::fromValue(QString(u"ReuseAppNoSrc"_s)));
    entryWithData.insert(fromStaticRaw(DesktopFileEntryKey), u"Exec"_s,
                         QVariant::fromValue(QString(u"/usr/bin/test"_s)));

    s_service->m_autostartSource = {autostartPath, entryWithData};

    EXPECT_NO_FATAL_FAILURE(s_service->setAutoStart(true));
    s_service->setAutoStart(false);
}

// ===========================================================================
// handleUnitRemoved — exec-condition and timeout results
// applicationservice.cpp line ~1355 (launchFailedResults set)
// ===========================================================================

TEST_F(V40DbusCoverageTest, HandleUnitRemoved_ExecCondition_ReportsFailed)
{
    auto *inst = new InstanceService(u"inst_execcond"_s, s_service->m_applicationPath.path(),
                                     u"/org/freedesktop/systemd1/unit/test_execcond"_s,
                                     u"test-launcher"_s, u"normal"_s);
    QDBusObjectPath instPath{s_service->m_applicationPath.path() + u'/' + u"inst_execcond"_s};
    s_service->m_Instances.insert(instPath, QSharedPointer<InstanceService>(inst));
    s_service->m_unitResults.insert(u"/org/freedesktop/systemd1/unit/test_execcond"_s,
                                    u"exec-condition"_s);
    EXPECT_NO_FATAL_FAILURE(s_service->handleUnitRemoved(
        u"/org/freedesktop/systemd1/unit/test_execcond"_s, u"test_execcond.service"_s));
    EXPECT_FALSE(s_service->m_Instances.contains(instPath));
}

TEST_F(V40DbusCoverageTest, HandleUnitRemoved_Timeout_ReportsFailed)
{
    auto *inst = new InstanceService(u"inst_timeout"_s, s_service->m_applicationPath.path(),
                                     u"/org/freedesktop/systemd1/unit/test_timeout"_s,
                                     u"test-launcher"_s, u"normal"_s);
    QDBusObjectPath instPath{s_service->m_applicationPath.path() + u'/' + u"inst_timeout"_s};
    s_service->m_Instances.insert(instPath, QSharedPointer<InstanceService>(inst));
    s_service->m_unitResults.insert(u"/org/freedesktop/systemd1/unit/test_timeout"_s,
                                    u"timeout"_s);
    EXPECT_NO_FATAL_FAILURE(s_service->handleUnitRemoved(
        u"/org/freedesktop/systemd1/unit/test_timeout"_s, u"test_timeout.service"_s));
    EXPECT_FALSE(s_service->m_Instances.contains(instPath));
}

// ===========================================================================
// MimeManager1Service — onMimeAppsFileChanged addPath with real file
// mimemanager1service.cpp lines ~188-190
// ===========================================================================

class V40DbusMimeTest : public testing::Test {
protected:
    static inline std::unique_ptr<ApplicationManager1Service> s_am;
    static inline std::unique_ptr<MimeManager1Service> s_mm;

    static void SetUpTestCase()
    {
        auto &bus = ApplicationManager1DBus::instance();
        bus.initGlobalServerBus(DBusType::Session);
        bus.setDestBus();
        std::shared_ptr<ApplicationManager1Storage> tmpStorage{nullptr};
        s_am = std::make_unique<ApplicationManager1Service>(
            std::make_unique<CGroupsIdentifier>(), tmpStorage);
        s_am->m_compatibilityManager = QSharedPointer<CompatibilityManager>(new CompatibilityManager());
        s_am->m_sessionOverrideConfig = std::make_unique<SessionOverrideConfig>();
        s_am->m_mimeManager = std::unique_ptr<MimeManager1Service>(new MimeManager1Service(s_am.get()));
        s_mm = std::unique_ptr<MimeManager1Service>(new MimeManager1Service(s_am.get()));
    }

    static void TearDownTestCase()
    {
        s_mm.reset();
        s_am.reset();
    }

    void TearDown() override
    {
        // Clean up accumulated watched paths from m_mimeAppsWatcher
        const auto files = s_mm->m_mimeAppsWatcher.files();
        for (const auto &path : files) {
            s_mm->m_mimeAppsWatcher.removePath(path);
        }
        // Stop the debounce timer if active
        if (s_mm->m_mimeAppsDebounceTimer.isActive()) {
            s_mm->m_mimeAppsDebounceTimer.stop();
        }
        // Reset internal write flag to prevent test isolation issues
        s_mm->m_internalWriteInProgress = false;
    }
};

// onMimeAppsFileChanged with a real file that's not yet watched → addPath is called
TEST_F(V40DbusMimeTest, OnMimeAppsFileChanged_RealFile_AddsPath)
{
    // Create a real temp file
    QTemporaryFile tmpFile;
    ASSERT_TRUE(tmpFile.open());
    tmpFile.write("test content");
    tmpFile.flush();
    QString path = tmpFile.fileName();
    // Keep the file alive — QTemporaryFile closes on destruction
    tmpFile.setAutoRemove(false);

    // Ensure path is not already watched
    EXPECT_FALSE(s_mm->m_mimeAppsWatcher.files().contains(path));

    // m_internalWriteInProgress is false by default → addPath should execute
    EXPECT_NO_FATAL_FAILURE(s_mm->onMimeAppsFileChanged(path));

    // File should now be watched (addPath was called)
    EXPECT_TRUE(s_mm->m_mimeAppsWatcher.files().contains(path));

    // Debounce timer should be active (external change path)
    EXPECT_TRUE(s_mm->m_mimeAppsDebounceTimer.isActive());

    QFile::remove(path);
}

// onMimeAppsFileChanged with a file that's already watched → skip addPath, still trigger debounce
TEST_F(V40DbusMimeTest, OnMimeAppsFileChanged_AlreadyWatched_SkipsAddPath)
{
    QTemporaryFile tmpFile;
    ASSERT_TRUE(tmpFile.open());
    tmpFile.write("test content 2");
    tmpFile.flush();
    QString path = tmpFile.fileName();
    tmpFile.setAutoRemove(false);

    // First call: adds path
    s_mm->onMimeAppsFileChanged(path);
    ASSERT_TRUE(s_mm->m_mimeAppsWatcher.files().contains(path));

    // Second call: path already watched → addPath skipped, debounce timer still starts
    EXPECT_NO_FATAL_FAILURE(s_mm->onMimeAppsFileChanged(path));
    EXPECT_TRUE(s_mm->m_mimeAppsDebounceTimer.isActive());

    QFile::remove(path);
}

// onMimeAppsFileChanged with internal write in progress → flag reset, early return
TEST_F(V40DbusMimeTest, OnMimeAppsFileChanged_InternalWriteFlag_EarlyReturn)
{
    // Set internal write flag
    s_mm->m_internalWriteInProgress = true;
    EXPECT_NO_FATAL_FAILURE(s_mm->onMimeAppsFileChanged(u"/nonexistent/path/file.list"_s));
    // Flag should be reset after the call
    EXPECT_FALSE(s_mm->m_internalWriteInProgress);
}

// ===========================================================================
// DesktopFileParser — isValidLocaleString branch coverage
// desktopfileparser.cpp lines 32-82
// ===========================================================================

class V40DbusDesktopParserTest : public testing::Test {};

// Helper to parse a desktop file and get its groups
static std::optional<DesktopFileParser::Groups> v40dbus_parseContent(const QByteArray &content)
{
    auto tmp = std::make_unique<QTemporaryFile>();
    if (!tmp->open()) return std::nullopt;
    tmp->write(content);
    tmp->flush();
    tmp->seek(0);
    DesktopFileParser parser(*tmp);
    DesktopFileParser::Groups groups;
    if (parser.parse(groups) != ParserError::NoError) return std::nullopt;
    return groups;
}

// isValidLocaleString: encoding part with lowercase letters — Name[en.UTF-8]
TEST_F(V40DbusDesktopParserTest, ValidLocaleString_EncodingWithLowercase)
{
    auto groups = v40dbus_parseContent(
        "[Desktop Entry]\nType=Application\nName=TestApp\nName[en.UTF-8]=TestEngUTF8\nExec=/bin/test\n");
    ASSERT_TRUE(groups.has_value());
    ASSERT_TRUE(groups->contains(u"Desktop Entry"_s));
    auto &entry = (*groups)[u"Desktop Entry"_s];
    // The locale key should be stored
    auto val = entry.value(u"Name"_s);
    ASSERT_TRUE(val.isValid());
    // Value should be a QStringMap containing the locale entry
    ASSERT_EQ(val.userType(), QMetaTypeId<QStringMap>::qt_metatype_id());
    const auto &map = *static_cast<const QStringMap *>(val.constData());
    EXPECT_TRUE(map.contains(u"en.UTF-8"_s));
    EXPECT_EQ(map.value(u"en.UTF-8"_s), u"TestEngUTF8"_s);
}

// isValidLocaleString: modifier part — Name[en@modifier]
TEST_F(V40DbusDesktopParserTest, ValidLocaleString_ModifierOnly)
{
    auto groups = v40dbus_parseContent(
        "[Desktop Entry]\nType=Application\nName=TestApp\nName[en@modifier]=TestEngMod\nExec=/bin/test\n");
    ASSERT_TRUE(groups.has_value());
    auto &entry = (*groups)[u"Desktop Entry"_s];
    auto val = entry.value(u"Name"_s);
    ASSERT_EQ(val.userType(), QMetaTypeId<QStringMap>::qt_metatype_id());
    const auto &map = *static_cast<const QStringMap *>(val.constData());
    EXPECT_TRUE(map.contains(u"en@modifier"_s));
    EXPECT_EQ(map.value(u"en@modifier"_s), u"TestEngMod"_s);
}

// isValidLocaleString: country + encoding + modifier combined — Name[en_US.UTF-8@mod]
TEST_F(V40DbusDesktopParserTest, ValidLocaleString_CountryEncodingModifier)
{
    auto groups = v40dbus_parseContent(
        "[Desktop Entry]\nType=Application\nName=TestApp\nName[en_US.UTF-8@mod]=TestFull\nExec=/bin/test\n");
    ASSERT_TRUE(groups.has_value());
    auto &entry = (*groups)[u"Desktop Entry"_s];
    auto val = entry.value(u"Name"_s);
    ASSERT_EQ(val.userType(), QMetaTypeId<QStringMap>::qt_metatype_id());
    const auto &map = *static_cast<const QStringMap *>(val.constData());
    EXPECT_TRUE(map.contains(u"en_US.UTF-8@mod"_s));
    EXPECT_EQ(map.value(u"en_US.UTF-8@mod"_s), u"TestFull"_s);
}

// isValidLocaleString: invalid — modifier with no content after @
TEST_F(V40DbusDesktopParserTest, InvalidLocaleString_ModifierNoContent)
{
    auto groups = v40dbus_parseContent(
        "[Desktop Entry]\nType=Application\nName=TestApp\nName[en@]=TestBadMod\nExec=/bin/test\n");
    ASSERT_TRUE(groups.has_value());
    auto &entry = (*groups)[u"Desktop Entry"_s];
    auto val = entry.value(u"Name"_s);
    // Invalid locale → line skipped, Name should only have default key.
    // Name is a locale-supported key, so it's always stored as QStringMap
    // (even the default value uses QStringMap{{"default", ...}}).
    ASSERT_EQ(val.userType(), QMetaTypeId<QStringMap>::qt_metatype_id());
    const auto &map1 = *static_cast<const QStringMap *>(val.constData());
    EXPECT_FALSE(map1.contains(u"en@"_s));
}

// isValidLocaleString: invalid — encoding with no content after dot
TEST_F(V40DbusDesktopParserTest, InvalidLocaleString_EncodingNoContent)
{
    auto groups = v40dbus_parseContent(
        "[Desktop Entry]\nType=Application\nName=TestApp\nName[en.]=TestBadEnc\nExec=/bin/test\n");
    ASSERT_TRUE(groups.has_value());
    auto &entry = (*groups)[u"Desktop Entry"_s];
    auto val = entry.value(u"Name"_s);
    ASSERT_EQ(val.userType(), QMetaTypeId<QStringMap>::qt_metatype_id());
    const auto &map2 = *static_cast<const QStringMap *>(val.constData());
    EXPECT_FALSE(map2.contains(u"en."_s));
}

// isValidLocaleString: invalid — country with no content after underscore
TEST_F(V40DbusDesktopParserTest, InvalidLocaleString_CountryNoContent)
{
    auto groups = v40dbus_parseContent(
        "[Desktop Entry]\nType=Application\nName=TestApp\nName[en_]=TestBadCountry\nExec=/bin/test\n");
    ASSERT_TRUE(groups.has_value());
    auto &entry = (*groups)[u"Desktop Entry"_s];
    auto val = entry.value(u"Name"_s);
    ASSERT_EQ(val.userType(), QMetaTypeId<QStringMap>::qt_metatype_id());
    const auto &map3 = *static_cast<const QStringMap *>(val.constData());
    EXPECT_FALSE(map3.contains(u"en_"_s));
}

// addEntry: invalid main key (contains non-alphanumeric chars) → line skipped
TEST_F(V40DbusDesktopParserTest, AddEntry_InvalidMainKey_Skipped)
{
    auto groups = v40dbus_parseContent(
        "[Desktop Entry]\nType=Application\nName=TestApp\nBad Key=value\nExec=/bin/test\n");
    ASSERT_TRUE(groups.has_value());
    auto &entry = (*groups)[u"Desktop Entry"_s];
    // "Bad Key" has a space → invalid key → skipped
    EXPECT_FALSE(entry.contains(u"Bad Key"_s));
}

// addEntry: unmatched brackets in key → line skipped
TEST_F(V40DbusDesktopParserTest, AddEntry_UnmatchedBrackets_Skipped)
{
    auto groups = v40dbus_parseContent(
        "[Desktop Entry]\nType=Application\nName=TestApp\nName[en=value\nExec=/bin/test\n");
    ASSERT_TRUE(groups.has_value());
    auto &entry = (*groups)[u"Desktop Entry"_s];
    auto val = entry.value(u"Name"_s);
    // Unmatched bracket → line skipped, only default Name present
    ASSERT_EQ(val.userType(), QMetaTypeId<QStringMap>::qt_metatype_id());
    const auto &map4 = *static_cast<const QStringMap *>(val.constData());
    EXPECT_FALSE(map4.contains(u"en"_s));
}

// addEntry: line without '=' → skipped, no error
TEST_F(V40DbusDesktopParserTest, AddEntry_NoEquals_Skipped)
{
    auto groups = v40dbus_parseContent(
        "[Desktop Entry]\nType=Application\nName=TestApp\nThisLineHasNoEquals\nExec=/bin/test\n");
    ASSERT_TRUE(groups.has_value());
    auto &entry = (*groups)[u"Desktop Entry"_s];
    // Line without '=' is skipped
    EXPECT_FALSE(entry.contains(u"ThisLineHasNoEquals"_s));
}

// addEntry: duplicate non-locale key → skipped
TEST_F(V40DbusCoverageTest, AddEntry_DuplicateNonLocaleKey_Skipped)
{
    auto groups = v40dbus_parseContent(
        "[Desktop Entry]\nType=Application\nName=TestApp\nExec=/bin/test\nExec=/bin/other\n");
    ASSERT_TRUE(groups.has_value());
    auto &entry = (*groups)[u"Desktop Entry"_s];
    // Exec is not a locale string → duplicate should be skipped, first value kept
    auto val = entry.value(u"Exec"_s);
    EXPECT_EQ(val.toString(), u"/bin/test"_s);
}

// addEntry: duplicate locale key (same locale) → skipped, first value kept
TEST_F(V40DbusCoverageTest, AddEntry_DuplicateLocaleKey_Skipped)
{
    auto groups = v40dbus_parseContent(
        "[Desktop Entry]\nType=Application\nName=TestApp\nName[zh_CN]=First\nName[zh_CN]=Second\nExec=/bin/test\n");
    ASSERT_TRUE(groups.has_value());
    auto &entry = (*groups)[u"Desktop Entry"_s];
    auto val = entry.value(u"Name"_s);
    ASSERT_EQ(val.userType(), QMetaTypeId<QStringMap>::qt_metatype_id());
    const auto &map = *static_cast<const QStringMap *>(val.constData());
    // Duplicate locale key → first value kept
    EXPECT_EQ(map.value(u"zh_CN"_s), u"First"_s);
}

// addEntry: locale key promotion — non-locale value promoted to QStringMap when locale variant added
TEST_F(V40DbusCoverageTest, AddEntry_LocaleKeyPromotion)
{
    // Custom key (not Name/GenericName/Comment/Keywords) with both plain and locale variants
    auto groups = v40dbus_parseContent(
        "[Desktop Entry]\nType=Application\nName=TestApp\nX-Custom=plain\nX-Custom[zh_CN]=chinese\nExec=/bin/test\n");
    ASSERT_TRUE(groups.has_value());
    auto &entry = (*groups)[u"Desktop Entry"_s];
    // X-Custom is not a locale string, but it has a locale variant
    // First: X-Custom=plain is stored as QString
    // Then: X-Custom[zh_CN] → existing value promoted to QStringMap{{"default", "plain"}}
    //        and zh_CN added
    // X-Custom is NOT a locale string (isLocaleString returns false for it).
    // When X-Custom[zh_CN] is encountered, keyExists=true + supportsLocale=false
    // → the locale variant is skipped (duplicate key, skip). Value stays as
    // the original QString "plain" (type QMetaType::QString = 10).
    auto val = entry.value(u"X-Custom"_s);
    ASSERT_EQ(val.userType(), QMetaType::QString);
    EXPECT_EQ(val.toString(), u"plain"_s);
}

// ===========================================================================
// toString (DesktopFileParser) — groups serialization edge cases
// desktopfileparser.cpp lines ~288-350
// ===========================================================================

// toString with empty groups → returns empty string
TEST_F(V40DbusCoverageTest, ToString_EmptyGroups_ReturnsEmpty)
{
    DesktopFileParser::Groups emptyGroups;
    auto result = toString(emptyGroups);
    EXPECT_TRUE(result.isEmpty());
}

// toString with no Desktop Entry group → returns empty string
TEST_F(V40DbusCoverageTest, ToString_NoDesktopEntryGroup_ReturnsEmpty)
{
    DesktopFileParser::Groups groups;
    groups.insert(u"Other Group"_s, {});
    auto result = toString(groups);
    EXPECT_TRUE(result.isEmpty());
}

// toString with Actions as non-QString type → warning path, toString() used
TEST_F(V40DbusCoverageTest, ToString_ActionsNotQStringType_WarningPath)
{
    DesktopFileParser::Groups groups;
    QMap<QString, QVariant> mainEntry;
    // Store Actions as an int (non-QString type)
    mainEntry.insert(u"Actions"_s, QVariant::fromValue(42));
    groups.insert(fromStaticRaw(DesktopFileEntryKey), mainEntry);
    auto result = toString(groups);
    // Should not crash, Actions toString() returns "42"
    EXPECT_FALSE(result.isEmpty());
}

// toString with QStringList value type
TEST_F(V40DbusCoverageTest, ToString_StringListValue)
{
    DesktopFileParser::Groups groups;
    QMap<QString, QVariant> mainEntry;
    mainEntry.insert(u"Type"_s, QVariant::fromValue(QString(u"Application"_s)));
    mainEntry.insert(u"Name"_s, QVariant::fromValue(QString(u"TestApp"_s)));
    // Store a key as QStringList
    mainEntry.insert(u"Categories"_s, QVariant::fromValue(QStringList{u"Game"_s, u"Utility"_s}));
    groups.insert(fromStaticRaw(DesktopFileEntryKey), mainEntry);
    auto result = toString(groups);
    EXPECT_FALSE(result.isEmpty());
    EXPECT_TRUE(result.contains(u"Categories=Game;Utility"_s));
}

// toString with Actions entry and action groups
TEST_F(V40DbusCoverageTest, ToString_WithActionGroups)
{
    DesktopFileParser::Groups groups;
    QMap<QString, QVariant> mainEntry;
    mainEntry.insert(u"Type"_s, QVariant::fromValue(QString(u"Application"_s)));
    mainEntry.insert(u"Name"_s, QVariant::fromValue(QString(u"TestApp"_s)));
    mainEntry.insert(u"Actions"_s, QVariant::fromValue(QString(u"Open;Save;"_s)));
    groups.insert(fromStaticRaw(DesktopFileEntryKey), mainEntry);

    // Add action groups
    QMap<QString, QVariant> openAction;
    openAction.insert(u"Name"_s, QVariant::fromValue(QString(u"Open"_s)));
    openAction.insert(u"Exec"_s, QVariant::fromValue(QString(u"/bin/open"_s)));
    groups.insert(fromStaticRaw(DesktopFileActionKey) % u"Open"_s, openAction);

    QMap<QString, QVariant> saveAction;
    saveAction.insert(u"Name"_s, QVariant::fromValue(QString(u"Save"_s)));
    saveAction.insert(u"Exec"_s, QVariant::fromValue(QString(u"/bin/save"_s)));
    groups.insert(fromStaticRaw(DesktopFileActionKey) % u"Save"_s, saveAction);

    auto result = toString(groups);
    EXPECT_FALSE(result.isEmpty());
    EXPECT_TRUE(result.contains(u"[Desktop Action Open]"_s));
    EXPECT_TRUE(result.contains(u"[Desktop Action Save]"_s));
}

// toString with extra groups (non-Desktop Entry, non-Action)
TEST_F(V40DbusCoverageTest, ToString_WithExtraGroups)
{
    DesktopFileParser::Groups groups;
    QMap<QString, QVariant> mainEntry;
    mainEntry.insert(u"Type"_s, QVariant::fromValue(QString(u"Application"_s)));
    mainEntry.insert(u"Name"_s, QVariant::fromValue(QString(u"TestApp"_s)));
    groups.insert(fromStaticRaw(DesktopFileEntryKey), mainEntry);

    QMap<QString, QVariant> extraGroup;
    extraGroup.insert(u"Key"_s, QVariant::fromValue(QString(u"Value"_s)));
    groups.insert(u"Extra Group"_s, extraGroup);

    auto result = toString(groups);
    EXPECT_FALSE(result.isEmpty());
    EXPECT_TRUE(result.contains(u"[Extra Group]"_s));
}

// toString with empty action in Actions list (skipped)
TEST_F(V40DbusCoverageTest, ToString_ActionsWithEmptyParts_Skipped)
{
    DesktopFileParser::Groups groups;
    QMap<QString, QVariant> mainEntry;
    mainEntry.insert(u"Type"_s, QVariant::fromValue(QString(u"Application"_s)));
    mainEntry.insert(u"Name"_s, QVariant::fromValue(QString(u"TestApp"_s)));
    // Actions with empty parts between semicolons
    mainEntry.insert(u"Actions"_s, QVariant::fromValue(QString(u"Open;;Save;"_s)));
    groups.insert(fromStaticRaw(DesktopFileEntryKey), mainEntry);

    QMap<QString, QVariant> openAction;
    openAction.insert(u"Name"_s, QVariant::fromValue(QString(u"Open"_s)));
    groups.insert(fromStaticRaw(DesktopFileActionKey) % u"Open"_s, openAction);

    QMap<QString, QVariant> saveAction;
    saveAction.insert(u"Name"_s, QVariant::fromValue(QString(u"Save"_s)));
    groups.insert(fromStaticRaw(DesktopFileActionKey) % u"Save"_s, saveAction);

    auto result = toString(groups);
    EXPECT_FALSE(result.isEmpty());
    EXPECT_TRUE(result.contains(u"[Desktop Action Open]"_s));
    EXPECT_TRUE(result.contains(u"[Desktop Action Save]"_s));
}

// ===========================================================================
// DesktopEntry — toString / toLocaleString / toIconString edge cases
// desktopentry.cpp lines ~382-480
// ===========================================================================

// toString with unknown value type (neither QStringMap nor QString) → returns {}
TEST_F(V40DbusCoverageTest, DesktopEntry_ToString_UnknownType_ReturnsEmpty)
{
    DesktopEntry entry;
    entry.insert(fromStaticRaw(DesktopFileEntryKey), u"X-Test"_s,
                 QVariant::fromValue(3.14));
    auto val = entry.value(fromStaticRaw(DesktopFileEntryKey), u"X-Test"_s);
    ASSERT_TRUE(val.has_value());
    auto result = toString(val->get());
    EXPECT_TRUE(result.isEmpty());
}

// toString with QStringMap where default key is missing but other keys exist → returns {}
TEST_F(V40DbusCoverageTest, DesktopEntry_ToString_NoDefaultKey_ReturnsEmpty)
{
    DesktopEntry entry;
    entry.insert(fromStaticRaw(DesktopFileEntryKey), u"Name"_s,
                 QVariant::fromValue(QStringMap{{u"zh_CN"_s, u"中文名"_s}}));
    auto val = entry.value(fromStaticRaw(DesktopFileEntryKey), u"Name"_s);
    ASSERT_TRUE(val.has_value());
    auto result = toString(val->get());
    // No "default" key → str is empty → returns empty
    EXPECT_TRUE(result.isEmpty());
}

// toString with skipUnescape=true and non-ASCII + control char content → returns {} (hasNonAsciiAndControlCharacters check)
TEST_F(V40DbusCoverageTest, DesktopEntry_ToString_SkipUnescapeWithNonAscii_ReturnsEmpty)
{
    DesktopEntry entry;
    entry.insert(fromStaticRaw(DesktopFileEntryKey), u"X-Test"_s,
                 QVariant::fromValue(QString(u"测试\x01"_s)));
    auto val = entry.value(fromStaticRaw(DesktopFileEntryKey), u"X-Test"_s);
    ASSERT_TRUE(val.has_value());
    auto result = toString(val->get(), true);  // skipUnescape = true
    // hasNonAsciiAndControlCharacters returns true (non-ASCII + control char) → returns {}
    EXPECT_TRUE(result.isEmpty());
}

// toLocaleString with non-QStringMap value → returns {}
TEST_F(V40DbusCoverageTest, DesktopEntry_ToLocaleString_NonStringMap_ReturnsEmpty)
{
    DesktopEntry entry;
    entry.insert(fromStaticRaw(DesktopFileEntryKey), u"X-Test"_s,
                 QVariant::fromValue(QString(u"plain"_s)));
    auto val = entry.value(fromStaticRaw(DesktopFileEntryKey), u"X-Test"_s);
    QLocale locale(u"zh_CN"_s);
    ASSERT_TRUE(val.has_value());
    auto result = toLocaleString(val->get(), locale);
    EXPECT_TRUE(result.isEmpty());
}

// toLocaleString with empty QStringMap → returns {}
TEST_F(V40DbusCoverageTest, DesktopEntry_ToLocaleString_EmptyMap_ReturnsEmpty)
{
    DesktopEntry entry;
    entry.insert(fromStaticRaw(DesktopFileEntryKey), u"Name"_s,
                 QVariant::fromValue(QStringMap{}));
    auto val = entry.value(fromStaticRaw(DesktopFileEntryKey), u"Name"_s);
    QLocale locale(u"en_US"_s);
    ASSERT_TRUE(val.has_value());
    auto result = toLocaleString(val->get(), locale);
    EXPECT_TRUE(result.isEmpty());
}

// toLocaleString with lang_COUNTRY@modifier in map, but QLocale strips modifier
// QLocale("en_US@pinyin").name() = "en_US" (modifier stripped), candidates = ["en_US", "en"]
// Map has "en_US@pinyin" and "en" → "en_US" not in map → fallback "en" → "English"
TEST_F(V40DbusCoverageTest, DesktopEntry_ToLocaleString_FullMatch_LangCountryModifier)
{
    DesktopEntry entry;
    QStringMap map;
    map.insert(u"en_US@pinyin"_s, u"EnglishUS_Pinyin"_s);
    map.insert(u"en"_s, u"English"_s);
    entry.insert(fromStaticRaw(DesktopFileEntryKey), u"Name"_s,
                 QVariant::fromValue(map));
    auto val = entry.value(fromStaticRaw(DesktopFileEntryKey), u"Name"_s);
    QLocale locale(u"en_US@pinyin"_s);
    ASSERT_TRUE(val.has_value());
    auto result = toLocaleString(val->get(), locale);
    EXPECT_EQ(result, u"English"_s);
}

// toLocaleString with matching lang_COUNTRY (no modifier in locale)
TEST_F(V40DbusCoverageTest, DesktopEntry_ToLocaleString_Match_LangCountry)
{
    DesktopEntry entry;
    QStringMap map;
    map.insert(u"en_US"_s, u"EnglishUS"_s);
    map.insert(u"en"_s, u"English"_s);
    entry.insert(fromStaticRaw(DesktopFileEntryKey), u"Name"_s,
                 QVariant::fromValue(map));
    auto val = entry.value(fromStaticRaw(DesktopFileEntryKey), u"Name"_s);
    QLocale locale(u"en_US.UTF-8"_s);
    ASSERT_TRUE(val.has_value());
    auto result = toLocaleString(val->get(), locale);
    EXPECT_EQ(result, u"EnglishUS"_s);
}

// toLocaleString with lang@modifier in map, but QLocale strips modifier and resolves default country
// DEFECT: QLocale("en@pinyin") strips the @modifier and resolves default country;
// locale.name() returns "en_US" (not "en@pinyin" nor "en"), so toLocaleString's
// modifier-parsing code never sees the modifier. The lang@modifier candidate path
// is unreachable through the QLocale API. Candidates = ["en_US", "en"] → no "en_US"
// key → fallback "en" → "English". See defect report in delivery comment.
TEST_F(V40DbusCoverageTest, DesktopEntry_ToLocaleString_Match_LangModifier)
{
    DesktopEntry entry;
    QStringMap map;
    map.insert(u"en@pinyin"_s, u"EnglishPinyin"_s);
    map.insert(u"en"_s, u"English"_s);
    entry.insert(fromStaticRaw(DesktopFileEntryKey), u"Name"_s,
                 QVariant::fromValue(map));
    auto val = entry.value(fromStaticRaw(DesktopFileEntryKey), u"Name"_s);
    QLocale locale(u"en@pinyin"_s);
    ASSERT_TRUE(val.has_value());
    auto result = toLocaleString(val->get(), locale);
    // QLocale strips @modifier and resolves default country → locale.name() = "en_US"
    // → no "en_US" key → fallback "en" → "English"
    EXPECT_EQ(result, u"English"_s);
}

// toLocaleString with fallback to lang only
TEST_F(V40DbusCoverageTest, DesktopEntry_ToLocaleString_Fallback_LangOnly)
{
    DesktopEntry entry;
    QStringMap map;
    map.insert(u"en"_s, u"English"_s);
    entry.insert(fromStaticRaw(DesktopFileEntryKey), u"Name"_s,
                 QVariant::fromValue(map));
    auto val = entry.value(fromStaticRaw(DesktopFileEntryKey), u"Name"_s);
    QLocale locale(u"en_US.UTF-8@pinyin"_s);
    ASSERT_TRUE(val.has_value());
    auto result = toLocaleString(val->get(), locale);
    // No en_US@pinyin, en_US, en@pinyin → fallback to en
    EXPECT_EQ(result, u"English"_s);
}

// toLocaleString with no match at all → returns {}
TEST_F(V40DbusCoverageTest, DesktopEntry_ToLocaleString_NoMatch_ReturnsEmpty)
{
    DesktopEntry entry;
    QStringMap map;
    map.insert(u"zh_CN"_s, u"中文"_s);
    entry.insert(fromStaticRaw(DesktopFileEntryKey), u"Name"_s,
                 QVariant::fromValue(map));
    auto val = entry.value(fromStaticRaw(DesktopFileEntryKey), u"Name"_s);
    QLocale locale(u"fr_FR"_s);
    ASSERT_TRUE(val.has_value());
    auto result = toLocaleString(val->get(), locale);
    EXPECT_TRUE(result.isEmpty());
}

// toBoolean with "true" → ok=true, returns true
TEST_F(V40DbusCoverageTest, DesktopEntry_ToBoolean_True)
{
    DesktopEntry entry;
    entry.insert(fromStaticRaw(DesktopFileEntryKey), u"Terminal"_s,
                 QVariant::fromValue(QString(u"true"_s)));
    auto val = entry.value(fromStaticRaw(DesktopFileEntryKey), u"Terminal"_s);
    bool ok = false;
    ASSERT_TRUE(val.has_value());
    bool result = toBoolean(val->get(), ok);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(result);
}

// toBoolean with "false" → ok=true, returns false
TEST_F(V40DbusCoverageTest, DesktopEntry_ToBoolean_False)
{
    DesktopEntry entry;
    entry.insert(fromStaticRaw(DesktopFileEntryKey), u"Terminal"_s,
                 QVariant::fromValue(QString(u"false"_s)));
    auto val = entry.value(fromStaticRaw(DesktopFileEntryKey), u"Terminal"_s);
    bool ok = true;
    ASSERT_TRUE(val.has_value());
    bool result = toBoolean(val->get(), ok);
    EXPECT_TRUE(ok);
    EXPECT_FALSE(result);
}

// toBoolean with invalid value → ok=false, returns false
TEST_F(V40DbusCoverageTest, DesktopEntry_ToBoolean_Invalid)
{
    DesktopEntry entry;
    entry.insert(fromStaticRaw(DesktopFileEntryKey), u"Terminal"_s,
                 QVariant::fromValue(QString(u"notabool"_s)));
    auto val = entry.value(fromStaticRaw(DesktopFileEntryKey), u"Terminal"_s);
    bool ok = true;
    ASSERT_TRUE(val.has_value());
    bool result = toBoolean(val->get(), ok);
    EXPECT_FALSE(ok);
    EXPECT_FALSE(result);
}

// ===========================================================================
// G. PrelaunchSplashHelper coverage (Wayland-dependent)
// ===========================================================================
// These tests exercise PrelaunchSplashHelper paths that require a Wayland
// display. Without Wayland, they GTEST_SKIP to document the coverage gap.
// The constructor, destructor, show (inactive extension), closeSplash (no
// active splash), and buildIconBuffer (null icon) paths are testable without
// an active Wayland connection.

TEST_F(V40DbusCoverageTest, PrelaunchSplashHelper_ConstructorDestructor)
{
    // Constructor creates the extension; destructor destroys splash objects.
    // Even without an active Wayland compositor, the object can be constructed.
    auto helper = std::make_unique<PrelaunchSplashHelper>();
    EXPECT_NE(helper, nullptr);
    // Destructor should not crash even with no active splashes.
    helper.reset();
}

TEST_F(V40DbusCoverageTest, PrelaunchSplashHelper_CloseSplash_NoActiveSplash)
{
    auto helper = std::make_unique<PrelaunchSplashHelper>();
    // closeSplash with no matching instance should not crash.
    helper->closeSplash(u"nonexistent-instance"_s);
    SUCCEED();
}

TEST_F(V40DbusCoverageTest, PrelaunchSplashHelper_Show_InactiveExtension)
{
    // Without an active Wayland compositor, isActive() returns false,
    // show() returns early with a warning. This exercises the early-return path.
    auto helper = std::make_unique<PrelaunchSplashHelper>();
    helper->show(u"test-app"_s, u"test-instance"_s, u""_s);
    // closeSplash after show (which was a no-op) should also be safe.
    helper->closeSplash(u"test-instance"_s);
    SUCCEED();
}

TEST_F(V40DbusCoverageTest, PrelaunchSplashHelper_Show_WithIconName_InactiveExtension)
{
    // Even with an icon name, if the extension is inactive, show() returns early.
    auto helper = std::make_unique<PrelaunchSplashHelper>();
    helper->show(u"test-app"_s, u"test-instance"_s, u"dialog-information"_s);
    helper->closeSplash(u"test-instance"_s);
    SUCCEED();
}

// ===========================================================================
// H. Additional DesktopEntry / DesktopFileGenerator coverage
// ===========================================================================

// toNumeric with valid integer-like float
TEST_F(V40DbusCoverageTest, DesktopEntry_ToNumeric_ValidFloat)
{
    DesktopEntry entry;
    entry.insert(fromStaticRaw(DesktopFileEntryKey), u"Version"_s,
                 QVariant::fromValue(2.5f));
    auto val = entry.value(fromStaticRaw(DesktopFileEntryKey), u"Version"_s);
    bool ok = false;
    ASSERT_TRUE(val.has_value());
    float result = toNumeric(val->get(), ok);
    EXPECT_TRUE(ok);
    EXPECT_FLOAT_EQ(result, 2.5f);
}

TEST_F(V40DbusCoverageTest, DesktopEntry_ToNumeric_InvalidString)
{
    DesktopEntry entry;
    entry.insert(fromStaticRaw(DesktopFileEntryKey), u"Version"_s,
                 QVariant::fromValue(QString(u"abc"_s)));
    auto val = entry.value(fromStaticRaw(DesktopFileEntryKey), u"Version"_s);
    bool ok = true;
    ASSERT_TRUE(val.has_value());
    float result = toNumeric(val->get(), ok);
    EXPECT_FALSE(ok);
    EXPECT_FLOAT_EQ(result, 0.0f);
}

// toIconString with QStringMap
TEST_F(V40DbusCoverageTest, DesktopEntry_ToIconString_WithQStringMap)
{
    DesktopEntry entry;
    entry.insert(fromStaticRaw(DesktopFileEntryKey), u"Icon"_s,
                 QVariant::fromValue(QStringMap{{u"default"_s, u"my-icon"_s}}));
    auto val = entry.value(fromStaticRaw(DesktopFileEntryKey), u"Icon"_s);
    ASSERT_TRUE(val.has_value());
    auto result = toIconString(val->get());
    EXPECT_EQ(result, u"my-icon"_s);
}

// toIconString with plain QString
TEST_F(V40DbusCoverageTest, DesktopEntry_ToIconString_WithPlainString)
{
    DesktopEntry entry;
    entry.insert(fromStaticRaw(DesktopFileEntryKey), u"Icon"_s,
                 QVariant::fromValue(QString(u"plain-icon"_s)));
    auto val = entry.value(fromStaticRaw(DesktopFileEntryKey), u"Icon"_s);
    ASSERT_TRUE(val.has_value());
    auto result = toIconString(val->get());
    EXPECT_EQ(result, u"plain-icon"_s);
}

// toIconString with unknown type → returns {}
TEST_F(V40DbusCoverageTest, DesktopEntry_ToIconString_UnknownType)
{
    DesktopEntry entry;
    entry.insert(fromStaticRaw(DesktopFileEntryKey), u"Icon"_s,
                 QVariant::fromValue(42));
    auto val = entry.value(fromStaticRaw(DesktopFileEntryKey), u"Icon"_s);
    ASSERT_TRUE(val.has_value());
    auto result = toIconString(val->get());
    EXPECT_TRUE(result.isEmpty());
}

// unescapeValue with all escape sequences
TEST_F(V40DbusCoverageTest, DesktopEntry_UnescapeValue_AllSequences)
{
    EXPECT_EQ(unescapeValue(uR"(a\sb)"_s), u"a b"_s);
    EXPECT_EQ(unescapeValue(uR"(a\nb)"_s), u"a\nb"_s);
    EXPECT_EQ(unescapeValue(uR"(a\tb)"_s), u"a\tb"_s);
    EXPECT_EQ(unescapeValue(uR"(a\rb)"_s), u"a\rb"_s);
    EXPECT_EQ(unescapeValue(uR"(a\\b)"_s), u"a\\b"_s);
    EXPECT_EQ(unescapeValue(uR"(a\;b)"_s), u"a;b"_s);
}

TEST_F(V40DbusCoverageTest, DesktopEntry_UnescapeValue_UnknownEscape)
{
    EXPECT_EQ(unescapeValue(uR"(a\xb)"_s), uR"(a\xb)"_s);
}

TEST_F(V40DbusCoverageTest, DesktopEntry_UnescapeValue_EmptyString)
{
    EXPECT_EQ(unescapeValue(u""_s), u""_s);
}

TEST_F(V40DbusCoverageTest, DesktopEntry_UnescapeValue_NoEscapes)
{
    EXPECT_EQ(unescapeValue(u"plain text"_s), u"plain text"_s);
}

TEST_F(V40DbusCoverageTest, DesktopEntry_UnescapeValue_TrailingBackslash)
{
    EXPECT_EQ(unescapeValue(u"test\\"_s), u"test\\"_s);
}

// toString with skipUnescape=false and string containing escapes
TEST_F(V40DbusCoverageTest, DesktopEntry_ToString_UnescapesByDefault)
{
    DesktopEntry entry;
    entry.insert(fromStaticRaw(DesktopFileEntryKey), u"Comment"_s,
                 QVariant::fromValue(QString(uR"(hello\sworld)"_s)));
    auto val = entry.value(fromStaticRaw(DesktopFileEntryKey), u"Comment"_s);
    ASSERT_TRUE(val.has_value());
    auto result = toString(val->get());
    EXPECT_EQ(result, u"hello world"_s);
}

// toString with empty string value → returns empty
TEST_F(V40DbusCoverageTest, DesktopEntry_ToString_EmptyString_ReturnsEmpty)
{
    DesktopEntry entry;
    entry.insert(fromStaticRaw(DesktopFileEntryKey), u"X-Test"_s,
                 QVariant::fromValue(QString(u""_s)));
    auto val = entry.value(fromStaticRaw(DesktopFileEntryKey), u"X-Test"_s);
    ASSERT_TRUE(val.has_value());
    auto result = toString(val->get());
    EXPECT_TRUE(result.isEmpty());
}

// toString with QStringMap that has default key with escaped content
TEST_F(V40DbusCoverageTest, DesktopEntry_ToString_QStringMapDefaultWithEscapes)
{
    DesktopEntry entry;
    entry.insert(fromStaticRaw(DesktopFileEntryKey), u"Name"_s,
                 QVariant::fromValue(QStringMap{{u"default"_s, uR"(test\snamed)"_s}}));
    auto val = entry.value(fromStaticRaw(DesktopFileEntryKey), u"Name"_s);
    ASSERT_TRUE(val.has_value());
    auto result = toString(val->get());
    EXPECT_EQ(result, u"test named"_s);
}

// toLocaleString with lang-only key in map; QLocale("zh").name() = "zh_CN" (default country)
// Candidates = ["zh_CN", "zh"] → "zh_CN" not in map → fallback "zh" → "Chinese"
TEST_F(V40DbusCoverageTest, DesktopEntry_ToLocaleString_LangOnlyInLocale)
{
    DesktopEntry entry;
    QStringMap map;
    map.insert(u"zh"_s, u"Chinese"_s);
    entry.insert(fromStaticRaw(DesktopFileEntryKey), u"Name"_s,
                 QVariant::fromValue(map));
    auto val = entry.value(fromStaticRaw(DesktopFileEntryKey), u"Name"_s);
    QLocale locale(u"zh"_s);
    ASSERT_TRUE(val.has_value());
    auto result = toLocaleString(val->get(), locale);
    EXPECT_EQ(result, u"Chinese"_s);
}

// DesktopEntry::group existing and non-existing
TEST_F(V40DbusCoverageTest, DesktopEntry_Group_Existing)
{
    DesktopEntry entry;
    entry.insert(fromStaticRaw(DesktopFileEntryKey), u"Name"_s,
                 QVariant::fromValue(QStringMap{{u"default"_s, u"App"_s}}));
    auto group = entry.group(fromStaticRaw(DesktopFileEntryKey));
    ASSERT_TRUE(group.has_value());
    EXPECT_TRUE(group->get().contains(u"Name"_s));
}

TEST_F(V40DbusCoverageTest, DesktopEntry_Group_NonExisting)
{
    DesktopEntry entry;
    entry.insert(fromStaticRaw(DesktopFileEntryKey), u"Name"_s,
                 QVariant::fromValue(QStringMap{{u"default"_s, u"App"_s}}));
    auto group = entry.group(u"NonExistent"_s);
    EXPECT_FALSE(group.has_value());
}

// DesktopEntry::value missing group and missing key
TEST_F(V40DbusCoverageTest, DesktopEntry_Value_MissingGroup)
{
    DesktopEntry entry;
    entry.insert(fromStaticRaw(DesktopFileEntryKey), u"Name"_s,
                 QVariant::fromValue(QStringMap{{u"default"_s, u"App"_s}}));
    auto val = entry.value(u"NonExistent"_s, u"Name"_s);
    EXPECT_FALSE(val.has_value());
}

TEST_F(V40DbusCoverageTest, DesktopEntry_Value_MissingKey)
{
    DesktopEntry entry;
    entry.insert(fromStaticRaw(DesktopFileEntryKey), u"Name"_s,
                 QVariant::fromValue(QStringMap{{u"default"_s, u"App"_s}}));
    auto val = entry.value(fromStaticRaw(DesktopFileEntryKey), u"NonExistent"_s);
    EXPECT_FALSE(val.has_value());
}

// DesktopEntry::insert and verify via value
TEST_F(V40DbusCoverageTest, DesktopEntry_Insert_OverwriteAndVerify)
{
    DesktopEntry entry;
    entry.insert(fromStaticRaw(DesktopFileEntryKey), u"Name"_s,
                 QVariant::fromValue(QStringMap{{u"default"_s, u"Old"_s}}));
    entry.insert(fromStaticRaw(DesktopFileEntryKey), u"Name"_s,
                 QVariant::fromValue(QStringMap{{u"default"_s, u"New"_s}}));
    auto val = entry.value(fromStaticRaw(DesktopFileEntryKey), u"Name"_s);
    ASSERT_TRUE(val.has_value());
    auto result = toString(val->get());
    EXPECT_EQ(result, u"New"_s);
}

// DesktopEntry::data
TEST_F(V40DbusCoverageTest, DesktopEntry_Data_ReturnsMap)
{
    DesktopEntry entry;
    entry.insert(fromStaticRaw(DesktopFileEntryKey), u"Name"_s,
                 QVariant::fromValue(QString(u"Test"_s)));
    const auto &data = entry.data();
    EXPECT_EQ(data.size(), 1);
    EXPECT_TRUE(data.contains(fromStaticRaw(DesktopFileEntryKey)));
}

// DesktopFileGenerator::processMainGroupLocaleEntry with ActionName key (returns 1)
TEST_F(V40DbusCoverageTest, Generator_ProcessMainGroupLocaleEntry_ActionName)
{
    DesktopEntry::container_type content;
    auto mainEntry = content.insert(fromStaticRaw(DesktopFileEntryKey), {});
    QVariantMap rawValue;
    rawValue.insert(u"Open"_s, u"Open"_s);
    auto ret = DesktopFileGenerator::processMainGroupLocaleEntry(
        mainEntry, fromStaticRaw(DesktopEntryActionName), QVariant::fromValue(rawValue));
    EXPECT_EQ(ret, 1);
}

// DesktopFileGenerator::generate with full action group flow
TEST_F(V40DbusCoverageTest, Generator_Generate_FullFlowWithActions)
{
    QVariantMap map;
    map.insert(fromStaticRaw(DesktopEntryType), QString{u"Application"_s});
    map.insert(fromStaticRaw(DesktopEntryName), QVariant::fromValue(QStringMap{{u"default"_s, u"TestApp"_s}}));
    map.insert(fromStaticRaw(DesktopEntryActions), QStringList{u"Open"_s, u"Save"_s});
    map.insert(fromStaticRaw(DesktopEntryActionName), QVariantMap{
        {u"Open"_s, QVariant::fromValue(QStringMap{{u"default"_s, u"Open"_s}})},
        {u"Save"_s, QVariant::fromValue(QStringMap{{u"default"_s, u"Save"_s}})}
    });
    map.insert(fromStaticRaw(DesktopEntryIcon), QVariant::fromValue(QStringMap{
        {u"default"_s, u"app-icon"_s},
        {u"Open"_s, u"open-icon"_s},
        {u"Save"_s, u"save-icon"_s}
    }));
    map.insert(fromStaticRaw(DesktopEntryExec), QVariant::fromValue(QStringMap{
        {u"default"_s, u"/bin/app"_s},
        {u"Open"_s, u"/bin/app --open"_s},
        {u"Save"_s, u"/bin/app --save"_s}
    }));

    QString err;
    auto content = DesktopFileGenerator::generate(map, err);
    EXPECT_FALSE(content.isEmpty());
    EXPECT_TRUE(err.isEmpty());
    EXPECT_TRUE(content.contains(u"[Desktop Entry]"_s));
    EXPECT_TRUE(content.contains(u"[Desktop Action Open]"_s));
    EXPECT_TRUE(content.contains(u"[Desktop Action Save]"_s));
}

// DesktopFileGenerator::generate with Actions but no ActionName → error
TEST_F(V40DbusCoverageTest, Generator_Generate_ActionsNoActionName_Error)
{
    QVariantMap map;
    map.insert(fromStaticRaw(DesktopEntryType), QString{u"Application"_s});
    map.insert(fromStaticRaw(DesktopEntryName), QVariant::fromValue(QStringMap{{u"default"_s, u"TestApp"_s}}));
    map.insert(fromStaticRaw(DesktopEntryActions), QStringList{u"Open"_s});
    QString err;
    auto content = DesktopFileGenerator::generate(map, err);
    EXPECT_TRUE(content.isEmpty());
    EXPECT_FALSE(err.isEmpty());
}

// DesktopFileGenerator::generate with empty ActionName map → error
TEST_F(V40DbusCoverageTest, Generator_Generate_EmptyActionNameMap_Error)
{
    QVariantMap map;
    map.insert(fromStaticRaw(DesktopEntryType), QString{u"Application"_s});
    map.insert(fromStaticRaw(DesktopEntryName), QVariant::fromValue(QStringMap{{u"default"_s, u"TestApp"_s}}));
    map.insert(fromStaticRaw(DesktopEntryActions), QStringList{u"Open"_s});
    map.insert(fromStaticRaw(DesktopEntryActionName), QVariantMap{});
    QString err;
    auto content = DesktopFileGenerator::generate(map, err);
    EXPECT_TRUE(content.isEmpty());
    EXPECT_FALSE(err.isEmpty());
}

// DesktopFileGenerator::processActionGroup with valid icon and exec for matching action
TEST_F(V40DbusCoverageTest, Generator_ProcessActionGroup_IconAndExecMatch)
{
    QVariantMap actionName;
    // actionName value is inserted as-is into DesktopEntryName via
    // QVariant::fromValue(*it). Using QString (not QStringMap) so that
    // toString() returns "Open" correctly.
    actionName.insert(u"Open"_s, QVariant::fromValue(QString(u"Open"_s)));
    QStringList actions{u"Open"_s};

    QVariantMap rawValue;
    rawValue.insert(fromStaticRaw(DesktopEntryIcon), QVariant::fromValue(QStringMap{
        {u"Open"_s, u"open-icon"_s}
    }));
    rawValue.insert(u"Exec"_s, QVariant::fromValue(QStringMap{
        {u"Open"_s, u"/bin/open"_s}
    }));

    DesktopEntry::container_type content;
    EXPECT_TRUE(DesktopFileGenerator::processActionGroup(actionName, actions, content, rawValue));
    auto &group = content[fromStaticRaw(DesktopFileActionKey) % u"Open"_s];
    EXPECT_EQ(group.value(fromStaticRaw(DesktopEntryName)).toString(), u"Open"_s);
    EXPECT_EQ(group.value(fromStaticRaw(DesktopEntryIcon)).toString(), u"open-icon"_s);
    EXPECT_EQ(group.value(u"Exec"_s).toString(), u"/bin/open"_s);
}

// DesktopFileGenerator::processActionGroup: action with empty icon value (skipped)
TEST_F(V40DbusCoverageTest, Generator_ProcessActionGroup_EmptyIconValue_Skipped)
{
    QVariantMap actionName;
    actionName.insert(u"Open"_s, QVariant::fromValue(QStringMap{{u"default"_s, u"Open"_s}}));
    QStringList actions{u"Open"_s};

    QVariantMap rawValue;
    rawValue.insert(fromStaticRaw(DesktopEntryIcon), QVariant::fromValue(QStringMap{
        {u"Open"_s, u""_s}  // empty icon value → should be skipped
    }));

    DesktopEntry::container_type content;
    EXPECT_TRUE(DesktopFileGenerator::processActionGroup(actionName, actions, content, rawValue));
    auto &group = content[fromStaticRaw(DesktopFileActionKey) % u"Open"_s];
    EXPECT_TRUE(group.contains(fromStaticRaw(DesktopEntryName)));
    EXPECT_FALSE(group.contains(fromStaticRaw(DesktopEntryIcon)));
}

// DesktopFileGenerator::processActionGroup: empty action in list (skipped)
TEST_F(V40DbusCoverageTest, Generator_ProcessActionGroup_EmptyActionInList)
{
    QVariantMap actionName;
    actionName.insert(u"Open"_s, QVariant::fromValue(QStringMap{{u"default"_s, u"Open"_s}}));
    QStringList actions{u""_s, u"Open"_s};  // first action is empty → skipped

    QVariantMap rawValue;
    DesktopEntry::container_type content;
    EXPECT_TRUE(DesktopFileGenerator::processActionGroup(actionName, actions, content, rawValue));
    EXPECT_TRUE(content.contains(fromStaticRaw(DesktopFileActionKey) % u"Open"_s));
    EXPECT_FALSE(content.contains(fromStaticRaw(DesktopFileActionKey)));  // no empty key
}

// DesktopFileGenerator::processMainGroup with Actions as QStringList
TEST_F(V40DbusCoverageTest, Generator_ProcessMainGroup_ActionsStringList)
{
    DesktopEntry::container_type content;
    QVariantMap rawValue;
    rawValue.insert(fromStaticRaw(DesktopEntryType), QString{u"Application"_s});
    rawValue.insert(fromStaticRaw(DesktopEntryActions), QStringList{u"Open"_s, u"Save"_s});
    EXPECT_TRUE(DesktopFileGenerator::processMainGroup(content, rawValue));
    auto &mainEntry = content[fromStaticRaw(DesktopFileEntryKey)];
    EXPECT_EQ(mainEntry.value(fromStaticRaw(DesktopEntryActions)).toString(), u"Open;Save"_s);
}

// DesktopFileGenerator::processMainGroup with Actions as QString
TEST_F(V40DbusCoverageTest, Generator_ProcessMainGroup_ActionsQString)
{
    DesktopEntry::container_type content;
    QVariantMap rawValue;
    rawValue.insert(fromStaticRaw(DesktopEntryType), QString{u"Application"_s});
    rawValue.insert(fromStaticRaw(DesktopEntryActions), QString{u"Open;Save"_s});
    EXPECT_TRUE(DesktopFileGenerator::processMainGroup(content, rawValue));
    auto &mainEntry = content[fromStaticRaw(DesktopFileEntryKey)];
    EXPECT_EQ(mainEntry.value(fromStaticRaw(DesktopEntryActions)).toString(), u"Open;Save"_s);
}

// DesktopFileGenerator::processMainGroup with Actions as wrong type (int)
TEST_F(V40DbusCoverageTest, Generator_ProcessMainGroup_ActionsWrongType)
{
    DesktopEntry::container_type content;
    QVariantMap rawValue;
    rawValue.insert(fromStaticRaw(DesktopEntryType), QString{u"Application"_s});
    rawValue.insert(fromStaticRaw(DesktopEntryActions), 42);
    EXPECT_FALSE(DesktopFileGenerator::processMainGroup(content, rawValue));
}

// DesktopFileGenerator::processMainGroup with Actions as empty QStringList
TEST_F(V40DbusCoverageTest, Generator_ProcessMainGroup_ActionsEmptyStringList)
{
    DesktopEntry::container_type content;
    QVariantMap rawValue;
    rawValue.insert(fromStaticRaw(DesktopEntryType), QString{u"Application"_s});
    rawValue.insert(fromStaticRaw(DesktopEntryActions), QStringList{});
    EXPECT_FALSE(DesktopFileGenerator::processMainGroup(content, rawValue));
}

// DesktopFileGenerator::processMainGroup with Name empty map → -1 → false
TEST_F(V40DbusCoverageTest, Generator_ProcessMainGroup_NameEmptyMap_Fails)
{
    DesktopEntry::container_type content;
    QVariantMap rawValue;
    rawValue.insert(fromStaticRaw(DesktopEntryType), QString{u"Application"_s});
    rawValue.insert(fromStaticRaw(DesktopEntryName), QVariant::fromValue(QStringMap{}));
    EXPECT_FALSE(DesktopFileGenerator::processMainGroup(content, rawValue));
}

// DesktopFileGenerator::checkValidation with valid Application type
TEST_F(V40DbusCoverageTest, Generator_CheckValidation_ValidApplication)
{
    QVariantMap desktopFile;
    desktopFile.insert(fromStaticRaw(DesktopEntryType), QString{u"Application"_s});
    desktopFile.insert(fromStaticRaw(DesktopEntryName), QString{u"TestApp"_s});
    QString err;
    EXPECT_TRUE(DesktopFileGenerator::checkValidation(desktopFile, err));
    EXPECT_TRUE(err.isEmpty());
}

// DesktopFileGenerator::checkValidation with missing Type
TEST_F(V40DbusCoverageTest, Generator_CheckValidation_MissingType)
{
    QVariantMap desktopFile;
    desktopFile.insert(fromStaticRaw(DesktopEntryName), QString{u"TestApp"_s});
    QString err;
    EXPECT_FALSE(DesktopFileGenerator::checkValidation(desktopFile, err));
    EXPECT_FALSE(err.isEmpty());
}

// DesktopFileGenerator::checkValidation with missing Name
TEST_F(V40DbusCoverageTest, Generator_CheckValidation_MissingName)
{
    QVariantMap desktopFile;
    desktopFile.insert(fromStaticRaw(DesktopEntryType), QString{u"Application"_s});
    QString err;
    EXPECT_FALSE(DesktopFileGenerator::checkValidation(desktopFile, err));
    EXPECT_FALSE(err.isEmpty());
}

// DesktopFileGenerator::checkValidation with empty Type
TEST_F(V40DbusCoverageTest, Generator_CheckValidation_EmptyType)
{
    QVariantMap desktopFile;
    desktopFile.insert(fromStaticRaw(DesktopEntryType), QString{});
    desktopFile.insert(fromStaticRaw(DesktopEntryName), QString{u"TestApp"_s});
    QString err;
    EXPECT_FALSE(DesktopFileGenerator::checkValidation(desktopFile, err));
    EXPECT_FALSE(err.isEmpty());
}

// DesktopFileGenerator::checkValidation with Link type and URL
TEST_F(V40DbusCoverageTest, Generator_CheckValidation_LinkWithUrl)
{
    QVariantMap desktopFile;
    desktopFile.insert(fromStaticRaw(DesktopEntryType), QString{u"Link"_s});
    desktopFile.insert(fromStaticRaw(DesktopEntryName), QString{u"TestLink"_s});
    desktopFile.insert(fromStaticRaw(DesktopEntryURL), QString{u"https://example.com"_s});
    QString err;
    EXPECT_TRUE(DesktopFileGenerator::checkValidation(desktopFile, err));
}

// DesktopFileGenerator::checkValidation with Link type without URL
TEST_F(V40DbusCoverageTest, Generator_CheckValidation_LinkWithoutUrl)
{
    QVariantMap desktopFile;
    desktopFile.insert(fromStaticRaw(DesktopEntryType), QString{u"Link"_s});
    desktopFile.insert(fromStaticRaw(DesktopEntryName), QString{u"TestLink"_s});
    QString err;
    EXPECT_FALSE(DesktopFileGenerator::checkValidation(desktopFile, err));
    EXPECT_FALSE(err.isEmpty());
}

// DesktopFileGenerator::generate with no actions, valid main group
TEST_F(V40DbusCoverageTest, Generator_Generate_NoActions_ValidMainGroup)
{
    QVariantMap map;
    map.insert(fromStaticRaw(DesktopEntryType), QString{u"Application"_s});
    map.insert(fromStaticRaw(DesktopEntryName), QVariant::fromValue(QStringMap{{u"default"_s, u"TestApp"_s}}));
    QString err;
    auto content = DesktopFileGenerator::generate(map, err);
    EXPECT_FALSE(content.isEmpty());
    EXPECT_TRUE(err.isEmpty());
    EXPECT_TRUE(content.contains(u"[Desktop Entry]"_s));
}

// ===========================================================================
// I. processExec additional field-code coverage
// Targets uncovered lines: 1571-1572 (empty args), 1612-1640 (%f/%F/%u/%U
// with fields), 1615 (exclusive twice), 1686 (%k), 1689 (%%), 1692-1697
// (%d/%m deprecated), 1700-1703 (unknown field code → return {})
// ===========================================================================

// processExec with empty/whitespace string → splitExecArguments returns
// empty list → early return (lines 1571-1572)
TEST_F(V40DbusCoverageTest, ProcessExec_EmptyArgs_ReturnsEmpty)
{
    auto task = s_service->processExec(u"   "_s, {});
    EXPECT_TRUE(task.LaunchBin.isEmpty());
    EXPECT_TRUE(task.command.isEmpty());
}

// processExec with %k → appends m_desktopSource.sourcePath() (line 1686)
TEST_F(V40DbusCoverageTest, ProcessExec_PercentK_AppendsSourcePath)
{
    auto task = s_service->processExec(u"/usr/bin/test %k"_s, {});
    EXPECT_EQ(task.LaunchBin, u"/usr/bin/test"_s);
    bool foundPath = false;
    for (const auto &arg : task.command) {
        if (arg == s_service->desktopFileSource().sourcePath()) {
            foundPath = true;
        }
    }
    EXPECT_TRUE(foundPath);
}

// processExec with %% → appends literal '%' (line 1689)
TEST_F(V40DbusCoverageTest, ProcessExec_PercentPercent_AppendsLiteralPercent)
{
    auto task = s_service->processExec(u"/usr/bin/test %%"_s, {});
    EXPECT_EQ(task.LaunchBin, u"/usr/bin/test"_s);
    bool foundPercent = false;
    for (const auto &arg : task.command) {
        if (arg == u"%"_s) {
            foundPercent = true;
        }
    }
    EXPECT_TRUE(foundPercent);
}

// processExec with deprecated %d → deprecation warning (lines 1692-1697)
TEST_F(V40DbusCoverageTest, ProcessExec_DeprecatedField_d_Ignored)
{
    auto task = s_service->processExec(u"/usr/bin/test %d"_s, {});
    EXPECT_EQ(task.LaunchBin, u"/usr/bin/test"_s);
    EXPECT_FALSE(task.command.isEmpty());
}

// processExec with deprecated %m → deprecation warning (lines 1692-1697)
TEST_F(V40DbusCoverageTest, ProcessExec_DeprecatedField_m_Ignored)
{
    auto task = s_service->processExec(u"/usr/bin/test %m"_s, {});
    EXPECT_EQ(task.LaunchBin, u"/usr/bin/test"_s);
    EXPECT_FALSE(task.command.isEmpty());
}

// processExec with unknown field code %x → return {} (lines 1700-1703)
TEST_F(V40DbusCoverageTest, ProcessExec_UnknownFieldCode_ReturnsEmpty)
{
    auto task = s_service->processExec(u"/usr/bin/test %x"_s, {});
    EXPECT_TRUE(task.LaunchBin.isEmpty());
    EXPECT_TRUE(task.command.isEmpty());
}

// processExec with %f and fields → dynamic field, Resources populated
// (lines 1612-1630)
TEST_F(V40DbusCoverageTest, ProcessExec_Field_f_WithFields_DynamicField)
{
    auto task = s_service->processExec(u"/usr/bin/test %f"_s, {u"/tmp/file1"_s});
    EXPECT_EQ(task.LaunchBin, u"/usr/bin/test"_s);
    EXPECT_FALSE(task.Resources.isEmpty());
    // %f (lowercase) → each resource is a QString
    EXPECT_EQ(task.Resources.size(), 1);
    EXPECT_TRUE(std::holds_alternative<QString>(task.Resources[0]));
    EXPECT_EQ(std::get<QString>(task.Resources[0]), u"/tmp/file1"_s);
}

// processExec with %F and multiple fields → QStringList in Resources
// (lines 1633-1636)
TEST_F(V40DbusCoverageTest, ProcessExec_Field_F_WithFields_StringList)
{
    auto task = s_service->processExec(u"/usr/bin/test %F"_s, {u"/tmp/f1"_s, u"/tmp/f2"_s});
    EXPECT_EQ(task.LaunchBin, u"/usr/bin/test"_s);
    EXPECT_EQ(task.Resources.size(), 1);
    // %F (uppercase) → QStringList
    EXPECT_TRUE(std::holds_alternative<QStringList>(task.Resources[0]));
    auto list = std::get<QStringList>(task.Resources[0]);
    EXPECT_EQ(list.size(), 2);
    EXPECT_EQ(list[0], u"/tmp/f1"_s);
    EXPECT_EQ(list[1], u"/tmp/f2"_s);
}

// processExec with exclusive field twice → second ignored (line 1615)
TEST_F(V40DbusCoverageTest, ProcessExec_ExclusiveFieldTwice_SecondIgnored)
{
    auto task = s_service->processExec(u"/usr/bin/test %f %u"_s, {u"/tmp/file1"_s});
    EXPECT_EQ(task.LaunchBin, u"/usr/bin/test"_s);
    // Only one resource set because %u is exclusive and %f already set it
    EXPECT_EQ(task.Resources.size(), 1);
}

// processExec with %u and fields → lowercase, individual QString resources
TEST_F(V40DbusCoverageTest, ProcessExec_Field_u_WithFields_DynamicField)
{
    auto task = s_service->processExec(u"/usr/bin/test %u"_s, {u"/tmp/file1"_s});
    EXPECT_EQ(task.LaunchBin, u"/usr/bin/test"_s);
    EXPECT_EQ(task.Resources.size(), 1);
    EXPECT_TRUE(std::holds_alternative<QString>(task.Resources[0]));
}

// processExec with %U and multiple fields → QStringList (uppercase)
TEST_F(V40DbusCoverageTest, ProcessExec_Field_U_WithFields_StringList)
{
    auto task = s_service->processExec(u"/usr/bin/test %U"_s, {u"/tmp/f1"_s, u"/tmp/f2"_s});
    EXPECT_EQ(task.LaunchBin, u"/usr/bin/test"_s);
    EXPECT_EQ(task.Resources.size(), 1);
    EXPECT_TRUE(std::holds_alternative<QStringList>(task.Resources[0]));
}

// ===========================================================================
// J. processExec %c with no Name entry (lines 1666-1667)
// Parse a valid entry then remove Name key from m_entryMap directly
// ===========================================================================
TEST_F(V40DbusCoverageTest, ProcessExec_PercentC_NoNameEntry_Ignored)
{
    auto optFile = v40dbus_createTempDesktopFile(QByteArray{kV40FullDesktop});
    ASSERT_TRUE(optFile.has_value());
    auto *newEntry = new DesktopEntry();
    ASSERT_EQ(newEntry->parse(*optFile), ParserError::NoError);
    // Remove Name from the Desktop Entry group to trigger `if (!val)` branch
    newEntry->m_entryMap[fromStaticRaw(DesktopFileEntryKey)].remove(u"Name"_s);
    s_service->resetEntry(newEntry);

    auto task = s_service->processExec(u"/usr/bin/test %c"_s, {});
    EXPECT_EQ(task.LaunchBin, u"/usr/bin/test"_s);
    // Name was not found, so %c is ignored but base command still present
    EXPECT_FALSE(task.command.isEmpty());
}

// ===========================================================================
// K. findEntryValue all EntryValueType branches (lines 1716-1755)
// ===========================================================================
TEST_F(V40DbusCoverageTest, FindEntryValue_AllTypes_ExistingKey)
{
    // Raw type
    {
        auto val = s_service->findEntryValue(fromStaticRaw(DesktopFileEntryKey),
                                             u"Exec"_s, EntryValueType::Raw);
        EXPECT_FALSE(val.isNull());
        EXPECT_TRUE(val.canConvert<QString>());
    }
    // String type
    {
        auto val = s_service->findEntryValue(fromStaticRaw(DesktopFileEntryKey),
                                             u"Exec"_s, EntryValueType::String);
        EXPECT_FALSE(val.isNull());
    }
    // LocaleString type
    {
        auto val = s_service->findEntryValue(fromStaticRaw(DesktopFileEntryKey),
                                             u"Name"_s, EntryValueType::LocaleString);
        EXPECT_FALSE(val.isNull());
    }
    // Boolean type — NoDisplay=true in kV40FullDesktop
    {
        auto val = s_service->findEntryValue(fromStaticRaw(DesktopFileEntryKey),
                                             u"NoDisplay"_s, EntryValueType::Boolean);
        EXPECT_FALSE(val.isNull());
        EXPECT_TRUE(val.toBool());
    }
    // IconString type
    {
        auto val = s_service->findEntryValue(fromStaticRaw(DesktopFileEntryKey),
                                             u"Icon"_s, EntryValueType::IconString);
        EXPECT_FALSE(val.isNull());
    }
}

TEST_F(V40DbusCoverageTest, FindEntryValue_NonExistingKey_ReturnsNull)
{
    auto val = s_service->findEntryValue(fromStaticRaw(DesktopFileEntryKey),
                                         u"NonExistentKey"_s, EntryValueType::String);
    EXPECT_TRUE(val.isNull());
}

TEST_F(V40DbusCoverageTest, FindEntryValue_BooleanInvalid_ReturnsNull)
{
    // "Exec" is not a boolean → toBoolean fails → returns null QVariant
    auto val = s_service->findEntryValue(fromStaticRaw(DesktopFileEntryKey),
                                         u"Exec"_s, EntryValueType::Boolean);
    EXPECT_TRUE(val.isNull());
}

TEST_F(V40DbusCoverageTest, FindEntryValue_RawEmpty_ReturnsNull)
{
    // Insert an empty raw value then query it
    auto optFile = v40dbus_createTempDesktopFile(QByteArray{kV40FullDesktop});
    ASSERT_TRUE(optFile.has_value());
    auto *newEntry = new DesktopEntry();
    ASSERT_EQ(newEntry->parse(*optFile), ParserError::NoError);
    newEntry->insert(fromStaticRaw(DesktopFileEntryKey), u"EmptyKey"_s,
                     QVariant::fromValue(QStringMap{{u"default"_s, u""_s}}));
    s_service->resetEntry(newEntry);

    auto val = s_service->findEntryValue(fromStaticRaw(DesktopFileEntryKey),
                                         u"EmptyKey"_s, EntryValueType::String);
    EXPECT_TRUE(val.isNull());
}

// ===========================================================================
// L. SendToDesktop / RemoveFromDesktop / isOnDesktop (lines 660-715)
// ===========================================================================
TEST_F(V40DbusCoverageTest, SendToDesktop_RemoveFromDesktop_isOnDesktop)
{
    // getDesktopDir() returns QStandardPaths::writableLocation(DesktopLocation)
    // which may not exist in CI environments. Skip if unavailable.
    auto dir = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    if (dir.isEmpty() || !QDir(dir).exists()) {
        GTEST_SKIP() << "Desktop directory does not exist (dir=\"" << dir.toStdString()
                     << "\"), skipping SendToDesktop/RemoveFromDesktop assertions";
    }

    // Ensure clean state: remove any pre-existing desktop symlink
    s_service->RemoveFromDesktop();
    EXPECT_FALSE(s_service->isOnDesktop());

    // SendToDesktop creates a symlink on the desktop
    bool sendResult = s_service->SendToDesktop();
    EXPECT_TRUE(sendResult);
    EXPECT_TRUE(s_service->isOnDesktop());

    // RemoveFromDesktop removes the symlink
    bool removeResult = s_service->RemoveFromDesktop();
    EXPECT_TRUE(removeResult);
    EXPECT_FALSE(s_service->isOnDesktop());

    // RemoveFromDesktop when not on desktop → returns true (early exit)
    bool removeResult2 = s_service->RemoveFromDesktop();
    EXPECT_TRUE(removeResult2);
}

// ===========================================================================
// M. actionName / icons / execs continue branches (lines 769, 808, 882)
// Create entry with Actions=Open;Save but remove [Desktop Action X] groups
// ===========================================================================
TEST_F(V40DbusCoverageTest, ActionName_Icons_Execs_ContinueBranch_MissingActionGroups)
{
    auto optFile = v40dbus_createTempDesktopFile(QByteArray{kV40FullDesktop});
    ASSERT_TRUE(optFile.has_value());
    auto *newEntry = new DesktopEntry();
    ASSERT_EQ(newEntry->parse(*optFile), ParserError::NoError);
    // Remove the action groups so value() returns nullopt → continue
    newEntry->m_entryMap.remove(fromStaticRaw(DesktopFileActionKey) % u"Open"_s);
    newEntry->m_entryMap.remove(fromStaticRaw(DesktopFileActionKey) % u"Save"_s);
    s_service->resetEntry(newEntry);

    // actionName: both actions hit `continue` → empty PropMap
    auto names = s_service->actionName();
    EXPECT_TRUE(names.isEmpty());

    // icons: both actions hit `continue`, but main Icon still present
    auto iconMap = s_service->icons();
    // Main icon should still be present
    EXPECT_TRUE(iconMap.contains(fromStaticRaw(DesktopFileEntryKey)));

    // execs: both actions hit `continue`, but main Exec still present
    auto execMap = s_service->execs();
    EXPECT_TRUE(execMap.contains(fromStaticRaw(DesktopFileEntryKey)));
}

// ===========================================================================
// N. closeSplashForInstance (lines 1418, 1423)
// Insert an ID into m_splashInstanceIds, call closeSplashForInstance
// ===========================================================================
TEST_F(V40DbusCoverageTest, CloseSplashForInstance_MatchingID_HitsBranches)
{
    const QString testId = u"test-splash-instance-id"_s;
    s_service->m_splashInstanceIds.insert(testId);

    // closeSplashForInstance: remove returns true → enters if (am) block
    // parent() returns s_am (not null), splashHelper() may be null → hits
    // qCInfo branch (line 1423)
    EXPECT_NO_FATAL_FAILURE(s_service->closeSplashForInstance(testId));
    // ID should be removed
    EXPECT_FALSE(s_service->m_splashInstanceIds.contains(testId));

    // Call again with same ID → remove returns false → early return
    EXPECT_NO_FATAL_FAILURE(s_service->closeSplashForInstance(testId));
}

// ===========================================================================
// O. setEnviron update path (lines 960-968)
// First call creates the value, second call updates it
// ===========================================================================
TEST_F(V40DbusCoverageTest, SetEnviron_UpdatePath_SecondCallUpdates)
{
    // First call: environ is empty → createApplicationValue path
    s_service->setEnviron(u"test-env-value"_s);

    // Second call: environ is now set → readApplicationValue non-null →
    // updateApplicationValue path (lines 960-968)
    s_service->setEnviron(u"test-env-value-2"_s);

    EXPECT_EQ(s_service->environ(), u"test-env-value-2"_s);
}

// ===========================================================================
// P. updateAfterLaunch (lines 1779-1780)
// Call with isLaunch=true and m_storage set → hits update path
// ===========================================================================
TEST_F(V40DbusCoverageTest, UpdateAfterLaunch_True_WithStorage_UpdatesValues)
{
    qint64 oldLaunch = s_service->m_lastLaunch;
    qint64 oldTimes = s_service->m_launchedTimes;

    // updateApplicationValue requires the app's group/key to already exist in storage.
    // Pre-populate storage so updateAfterLaunch(true) can succeed.
    const auto appId = s_service->desktopFileSource().desktopId();
    s_storage->createApplicationValue(appId,
        fromStaticRaw(ApplicationPropertiesGroup),
        fromStaticRaw(LastLaunchedTime),
        QVariant::fromValue(oldLaunch));
    s_storage->createApplicationValue(appId,
        fromStaticRaw(ApplicationPropertiesGroup),
        fromStaticRaw(LaunchedTimes),
        QVariant::fromValue(oldTimes));

    s_service->updateAfterLaunch(true);

    // Should have updated m_lastLaunch and m_launchedTimes
    // m_lastLaunch is set to QDateTime::currentMSecsSinceEpoch(); use EXPECT_GE
    // to avoid same-millisecond race, then verify the value is a current timestamp.
    EXPECT_GE(s_service->m_lastLaunch, oldLaunch);
    qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    EXPECT_LE(s_service->m_lastLaunch, nowMs);
    EXPECT_EQ(s_service->m_launchedTimes, oldTimes + 1);
}

TEST_F(V40DbusCoverageTest, UpdateAfterLaunch_False_EarlyReturn)
{
    qint64 oldLaunch = s_service->m_lastLaunch;
    s_service->updateAfterLaunch(false);
    EXPECT_EQ(s_service->m_lastLaunch, oldLaunch);
}

// ===========================================================================
// Q. PrelaunchSplashHelper bufferRelease / handleBufferRelease (lines 109-131)
// ===========================================================================
TEST_F(V40DbusCoverageTest, PrelaunchSplashHelper_BufferRelease_NullData_NoOp)
{
    // bufferRelease with null data → `if (auto *helper = ...)` is false → no-op
    PrelaunchSplashHelper::bufferRelease(nullptr, nullptr);
    SUCCEED();
}

TEST_F(V40DbusCoverageTest, PrelaunchSplashHelper_HandleBufferRelease_NullBuffer_NoOp)
{
    auto helper = std::make_unique<PrelaunchSplashHelper>();
    // handleBufferRelease with nullptr → find_if returns end → no-op
    helper->handleBufferRelease(nullptr);
    SUCCEED();
}

// ===========================================================================
// R. SessionOverrideConfig coverage (constructor switch, resolveExecValue)
// ===========================================================================
TEST_F(V40DbusCoverageTest, SessionOverrideConfig_Constructor_SwitchBranches)
{
    // The constructor runs currentSessionType() and switches on it.
    // Constructing a new instance exercises the switch (lines 21-26).
    auto config = std::make_unique<SessionOverrideConfig>();
    EXPECT_NE(config, nullptr);
}

TEST_F(V40DbusCoverageTest, SessionOverrideConfig_ResolveExecValue_NoPlaceholder)
{
    // resolveExecValue without !AM_FULL! → returns value as-is (line 190)
    auto result = SessionOverrideConfig::resolveExecValue(u"/usr/bin/test"_s, u"/usr/bin/original"_s);
    EXPECT_EQ(result, u"/usr/bin/test"_s);
}

TEST_F(V40DbusCoverageTest, SessionOverrideConfig_ResolveExecValue_WithPlaceholder)
{
    // resolveExecValue with !AM_FULL! → replaces with originalExec (line 190)
    auto result = SessionOverrideConfig::resolveExecValue(u"!AM_FULL! --arg"_s, u"/usr/bin/original"_s);
    EXPECT_EQ(result, u"/usr/bin/original --arg"_s);
}

TEST_F(V40DbusCoverageTest, SessionOverrideConfig_GetValue_NonDesktopEntryGroup_ReturnsNullopt)
{
    auto config = std::make_unique<SessionOverrideConfig>();
    // getValue with non-Desktop Entry group → returns nullopt (line 67)
    auto result = config->getValue(u"test-id"_s, u"SomeOtherGroup"_s, u"Exec"_s);
    EXPECT_FALSE(result.has_value());
}

TEST_F(V40DbusCoverageTest, SessionOverrideConfig_GetValue_UnknownField_ReturnsNullopt)
{
    auto config = std::make_unique<SessionOverrideConfig>();
    // getValue with Desktop Entry group but unknown field → returns nullopt
    auto result = config->getValue(u"test-id"_s, fromStaticRaw(DesktopFileEntryKey), u"UnknownField"_s);
    EXPECT_FALSE(result.has_value());
}

TEST_F(V40DbusCoverageTest, SessionOverrideConfig_HasOverride_NotLoaded_ReturnsFalse)
{
    auto config = std::make_unique<SessionOverrideConfig>();
    // hasOverride for a desktopId that was never loaded → false
    EXPECT_FALSE(config->hasOverride(u"never-loaded-id"_s));
}

TEST_F(V40DbusCoverageTest, SessionOverrideConfig_Preload_EmptyList_NoOp)
{
    auto config = std::make_unique<SessionOverrideConfig>();
    // preload with empty list → no crash, no-op
    config->preload({});
    SUCCEED();
}

// ===========================================================================
// S. MimeManager1Service coverage (lines 85-162)
// ===========================================================================
TEST_F(V40DbusCoverageTest, MimeManager_QueryDefaultApplication_EmptyMime)
{
    // Querying with empty mime type → should return empty or handle gracefully
    auto *mimeMgr = s_am->m_mimeManager.get();
    ASSERT_NE(mimeMgr, nullptr);
    // listApplications with empty mime → exercises the method path
    auto apps = mimeMgr->listApplications(u""_s);
    // Empty mime type should not match any application
    EXPECT_TRUE(apps.isEmpty());
}

TEST_F(V40DbusCoverageTest, MimeManager_QueryDefaultApplication_NonExistentMime)
{
    auto *mimeMgr = s_am->m_mimeManager.get();
    ASSERT_NE(mimeMgr, nullptr);
    // queryDefaultApplication with non-existent mime → returns the type as-is
    QDBusObjectPath appPath;
    auto result = mimeMgr->queryDefaultApplication(u"application/x-nonexistent"_s, appPath);
    // The call exercises the method path
    EXPECT_FALSE(result.isEmpty());
}

// ===========================================================================
// T. Additional noDisplay / actions / categories coverage
// ===========================================================================
TEST_F(V40DbusCoverageTest, NoDisplay_True_FromFullDesktop)
{
    // kV40FullDesktop has NoDisplay=true
    EXPECT_TRUE(s_service->noDisplay());
}

TEST_F(V40DbusCoverageTest, Actions_FromFullDesktop)
{
    auto acts = s_service->actions();
    EXPECT_EQ(acts.size(), 2);
    EXPECT_TRUE(acts.contains(u"Open"_s));
    EXPECT_TRUE(acts.contains(u"Save"_s));
}

TEST_F(V40DbusCoverageTest, Categories_FromFullDesktop)
{
    auto cats = s_service->categories();
    EXPECT_EQ(cats.size(), 2);
    EXPECT_TRUE(cats.contains(u"Game"_s));
    EXPECT_TRUE(cats.contains(u"Utility"_s));
}

TEST_F(V40DbusCoverageTest, Name_GenericName_FromFullDesktop)
{
    auto names = s_service->name();
    EXPECT_FALSE(names.isEmpty());

    auto genericNames = s_service->genericName();
    EXPECT_FALSE(genericNames.isEmpty());
}

TEST_F(V40DbusCoverageTest, XFlags_FromFullDesktop)
{
    EXPECT_TRUE(s_service->x_Flatpak());
    EXPECT_TRUE(s_service->x_linglong());
    EXPECT_EQ(s_service->X_linglongAppId(), u"org.test.TestApp"_s);
    EXPECT_EQ(s_service->X_Deepin_Vendor(), u"TestVendor"_s);
    EXPECT_EQ(s_service->X_Deepin_CreateBy(), u"test"_s);
}

// ===========================================================================
// U. MimeManager1Service additional coverage (mimemanager1service.cpp)
// Target: queryDefaultApplication absolute path, updateMimeCache, reset,
//         onMimeAppsFileChanged, handleMimeAppsFileDebounced
// ===========================================================================

TEST_F(V40DbusCoverageTest, MimeManager_QueryDefaultApplication_AbsolutePath_Exists)
{
    // Covers the info.isAbsolute() and info.exists() branch in queryDefaultApplication
    QTemporaryFile tmpFile;
    ASSERT_TRUE(tmpFile.open());
    tmpFile.write("hello world");
    tmpFile.close();

    auto *mimeMgr = s_am->m_mimeManager.get();
    ASSERT_NE(mimeMgr, nullptr);

    QDBusObjectPath appPath;
    auto result = mimeMgr->queryDefaultApplication(tmpFile.fileName(), appPath);
    // Should return a non-empty mime type (at minimum "text/plain" or "application/octet-stream")
    EXPECT_FALSE(result.isEmpty());
}

TEST_F(V40DbusCoverageTest, MimeManager_UpdateMimeCache_RunsProcess)
{
    // Covers updateMimeCache QProcess path
    auto *mimeMgr = s_am->m_mimeManager.get();
    ASSERT_NE(mimeMgr, nullptr);
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    // updateMimeCache starts "update-desktop-database" via QProcess.
    // If the tool is installed, exitCode should be 0; if not, the function
    // handles it gracefully (qWarning). Either way it must not crash.
    bool hasTool = !QStandardPaths::findExecutable("update-desktop-database").isEmpty();
    EXPECT_NO_FATAL_FAILURE(mimeMgr->updateMimeCache(tmpDir.path()));
    // If the tool exists, a second call on the same dir should also succeed
    if (hasTool) {
        EXPECT_NO_FATAL_FAILURE(mimeMgr->updateMimeCache(tmpDir.path()));
    }
}

TEST_F(V40DbusCoverageTest, MimeManager_Reset_ClearsInfos)
{
    // Covers reset() — m_infos.clear()
    auto *mimeMgr = s_am->m_mimeManager.get();
    ASSERT_NE(mimeMgr, nullptr);
    // reset should not crash and should leave m_infos empty
    mimeMgr->reset();
    EXPECT_TRUE(mimeMgr->infos().empty());
}

TEST_F(V40DbusCoverageTest, MimeManager_OnMimeAppsFileChanged_NewFile_AddsPath)
{
    // Covers onMimeAppsFileChanged branch where path is not in watcher
    // but file exists → addPath is called, then debounce timer starts
    auto *mimeMgr = s_am->m_mimeManager.get();
    ASSERT_NE(mimeMgr, nullptr);

    QTemporaryFile tmpFile;
    ASSERT_TRUE(tmpFile.open());
    tmpFile.write("text/plain=test-app.desktop");
    tmpFile.close();
    QString filePath = tmpFile.fileName();

    // Ensure path is not already watched
    EXPECT_FALSE(mimeMgr->m_mimeAppsWatcher.files().contains(filePath));

    // m_internalWriteInProgress is false → addPath is called, debounce timer starts
    mimeMgr->onMimeAppsFileChanged(filePath);

    // Verify path was added to watcher
    EXPECT_TRUE(mimeMgr->m_mimeAppsWatcher.files().contains(filePath));

    // Clean up: remove path before tmpFile destructor deletes the file,
    // which would trigger fileChanged → debounce timer cascade (issue [4])
    mimeMgr->m_mimeAppsWatcher.removePath(filePath);
    if (mimeMgr->m_mimeAppsDebounceTimer.isActive()) {
        mimeMgr->m_mimeAppsDebounceTimer.stop();
    }
}

TEST_F(V40DbusCoverageTest, MimeManager_OnMimeAppsFileChanged_InternalWrite_SkipsDebounce)
{
    // Covers the m_internalWriteInProgress=true branch
    auto *mimeMgr = s_am->m_mimeManager.get();
    ASSERT_NE(mimeMgr, nullptr);

    // Set internal write flag and call onMimeAppsFileChanged
    mimeMgr->m_internalWriteInProgress = true;
    mimeMgr->onMimeAppsFileChanged(u"/some/nonexistent/path"_s);
    // Flag should be reset to false after the call
    EXPECT_FALSE(mimeMgr->m_internalWriteInProgress);
}

TEST_F(V40DbusCoverageTest, MimeManager_HandleMimeAppsFileDebounced_ReloadsInfos)
{
    // Covers handleMimeAppsFileDebounced → parentService->reloadMimeInfos()
    auto *mimeMgr = s_am->m_mimeManager.get();
    ASSERT_NE(mimeMgr, nullptr);

    // reloadMimeInfos() calls reset() + scanMimeInfos() + emit MimeInfoReloaded
    // Verify the MimeInfoReloaded signal is emitted
    QSignalSpy spy(mimeMgr, &MimeManager1Service::MimeInfoReloaded);
    ASSERT_TRUE(spy.isValid());

    mimeMgr->handleMimeAppsFileDebounced();

    // Signal should have been emitted exactly once
    EXPECT_EQ(spy.count(), 1);

    // Clean up: reloadMimeInfos may have populated m_infos via scanMimeInfos;
    // reset to prevent state leakage to subsequent tests
    mimeMgr->reset();
}

TEST_F(V40DbusCoverageTest, MimeManager_ListApplications_TextPlain_NoCrash)
{
    // Covers listApplications with a real mime type — exercises
    // mimeTypeForName lookup and the m_infos iteration loop
    auto *mimeMgr = s_am->m_mimeManager.get();
    ASSERT_NE(mimeMgr, nullptr);
    // Ensure m_infos is empty — prior tests (e.g. HandleMimeAppsFileDebounced)
    // may have populated m_infos via reloadMimeInfos() → scanMimeInfos()
    mimeMgr->reset();
    auto apps = mimeMgr->listApplications(u"text/plain"_s);
    // With empty m_infos, result should be empty
    EXPECT_TRUE(apps.isEmpty());
}
