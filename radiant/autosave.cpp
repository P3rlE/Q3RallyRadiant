/*
   Copyright (C) 1999-2006 Id Software, Inc. and contributors.
   For a list of contributors, see the accompanying CONTRIBUTORS file.

   This file is part of GtkRadiant.

   GtkRadiant is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   GtkRadiant is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with GtkRadiant; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "autosave.h"

#include <algorithm>
#include <ctime>
#include <vector>

#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileInfoList>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QTextStream>
#include <QVBoxLayout>

#include "os/file.h"
#include "os/path.h"
#include "commandlib.h"
#include "stream/stringstream.h"
#include "gtkutil/messagebox.h"
#include "scenelib.h"
#include "mapfile.h"

#include "map.h"
#include "mainframe.h"
#include "qe3.h"
#include "preferences.h"


#if defined( WIN32 )
#define PATH_MAX 260
#endif


bool DoesFileExist( const char* name, std::size_t& size ){
	if ( file_exists( name ) ) {
		size += file_size( name );
		return true;
	}
	return false;
}

void Map_Snapshot(){
	// we need to do the following
	// 1. make sure the snapshot directory exists (create it if it doesn't)
	// 2. find out what the lastest save is based on number
	// 3. inc that and save the map
	const char* mapname = Map_Name( g_map );
	const auto snapshotsDir = StringStream( PathFilenameless( mapname ), "snapshots" );

	if ( file_exists( snapshotsDir ) || Q_mkdir( snapshotsDir ) ) {
		std::size_t lSize = 0;
		const auto strNewPath = StringStream( snapshotsDir, '/', path_get_filename_start( mapname ) );
		const char* ext = path_get_filename_base_end( strNewPath );

		StringOutputStream snapshotFilename( 256 );
		for ( int nCount = 0; ; ++nCount )
		{
			// The original map's filename is "<path>/<name>.<ext>"
			// The snapshot's filename will be "<path>/snapshots/<name>.<count>.<ext>"
			snapshotFilename( StringRange( strNewPath.c_str(), ext ), '.', nCount, ext );

			if ( !DoesFileExist( snapshotFilename, lSize ) ) {
				break;
			}
		}

		// save in the next available slot
		Map_SaveFile( snapshotFilename );

		if ( lSize > 50 * 1024 * 1024 ) { // total size of saves > 50 mb
			globalOutputStream() << "The snapshot files in " << snapshotsDir << " total more than 50 megabytes. You might consider cleaning up.";
		}
	}
	else
	{
		const auto strMsg = StringStream( "Snapshot save failed.. unabled to create directory\n", snapshotsDir );
		qt_MessageBox( MainFrame_getWindow(), strMsg );
	}
}
/*
   ===============
   QE_CheckAutoSave

   If five minutes have passed since making a change
   and the map hasn't been saved, save it out.
   ===============
 */

bool g_AutoSave_Enabled = true;
int m_AutoSave_Frequency = 15;
bool g_SnapShots_Enabled = false;
bool g_Recovery_Enabled = true;
int g_Recovery_MaxSnapshots = 40;
int g_Recovery_MaxAgeDays = 14;

namespace
{
time_t s_start = 0;
std::size_t s_changes = 0;
bool s_uncleanShutdownDetected = false;

constexpr const char* c_recoveryDirectory = "recovery";
constexpr const char* c_recoveryLockFile = "session.lock";

QString RecoveryDirectoryPath(){
	return QString::fromLocal8Bit( SettingsPath_get() ) + c_recoveryDirectory;
}

QString RecoveryLockFilePath(){
	return QDir( RecoveryDirectoryPath() ).filePath( c_recoveryLockFile );
}

struct RecoverySnapshotInfo
{
	QString mapFile;
	QString metaFile;
	QString sourceMapFileName;
	QDateTime timestamp;
	qint64 fileSize = 0;
	int entityCount = -1;
	int brushCount = -1;
};

QString RecoveryMetaPathForMap( const QString& mapPath ){
	return mapPath + ".meta";
}

std::vector<RecoverySnapshotInfo> RecoveryCollectSnapshots(){
	std::vector<RecoverySnapshotInfo> snapshots;
	const QDir recoveryDir( RecoveryDirectoryPath() );
	if ( !recoveryDir.exists() ) {
		return snapshots;
	}

	const QFileInfoList files = recoveryDir.entryInfoList( QStringList() << "*.map", QDir::Files, QDir::Time );
	for ( const QFileInfo& fileInfo : files )
	{
		RecoverySnapshotInfo info;
		info.mapFile = fileInfo.absoluteFilePath();
		info.metaFile = RecoveryMetaPathForMap( info.mapFile );
		info.fileSize = fileInfo.size();
		info.timestamp = fileInfo.lastModified();
		info.sourceMapFileName = fileInfo.fileName();

		QFile metaFile( info.metaFile );
		if ( metaFile.open( QIODevice::ReadOnly | QIODevice::Text ) ) {
			QTextStream stream( &metaFile );
			while ( !stream.atEnd() )
			{
				const QString line = stream.readLine();
				const int separator = line.indexOf( '=' );
				if ( separator <= 0 ) {
					continue;
				}
				const QString key = line.left( separator ).trimmed();
				const QString value = line.mid( separator + 1 ).trimmed();
				if ( key == "timestamp" ) {
					const QDateTime timestamp = QDateTime::fromString( value, Qt::ISODateWithMs );
					if ( timestamp.isValid() ) {
						info.timestamp = timestamp;
					}
				}
				else if ( key == "sourceMapFileName" ) {
					info.sourceMapFileName = value;
				}
				else if ( key == "entityCount" ) {
					info.entityCount = value.toInt();
				}
				else if ( key == "brushCount" ) {
					info.brushCount = value.toInt();
				}
			}
		}

		snapshots.push_back( info );
	}

	std::sort( snapshots.begin(), snapshots.end(), []( const RecoverySnapshotInfo& a, const RecoverySnapshotInfo& b ){
		return a.timestamp > b.timestamp;
	} );
	return snapshots;
}

QString RecoveryDisplayText( const RecoverySnapshotInfo& snapshot ){
	const QString timestampText = snapshot.timestamp.isValid()
		? snapshot.timestamp.toLocalTime().toString( "yyyy-MM-dd HH:mm:ss" )
		: QObject::tr( "Unknown time" );
	const QString sizeText = QObject::tr( "%1 KiB" ).arg( QString::number( snapshot.fileSize / 1024.0, 'f', 1 ) );
	QString details = QObject::tr( "%1 | %2 | %3" ).arg( timestampText, snapshot.sourceMapFileName, sizeText );
	if ( snapshot.entityCount >= 0 && snapshot.brushCount >= 0 ) {
		details += QObject::tr( " | E:%1 B:%2" ).arg( snapshot.entityCount ).arg( snapshot.brushCount );
	}
	return details;
}

void RecoveryRemoveAllData(){
	QDir recoveryDir( RecoveryDirectoryPath() );
	if ( !recoveryDir.exists() ) {
		return;
	}
	const QFileInfoList files = recoveryDir.entryInfoList( QDir::NoDotAndDotDot | QDir::AllEntries );
	for ( const QFileInfo& file : files )
	{
		if ( file.fileName() == c_recoveryLockFile ) {
			continue;
		}
		if ( file.isDir() ) {
			QDir( file.absoluteFilePath() ).removeRecursively();
		}
		else{
			QFile::remove( file.absoluteFilePath() );
		}
	}
}

void RecoveryCleanupSnapshots(){
	QDir recoveryDir( RecoveryDirectoryPath() );
	if ( !recoveryDir.exists() ) {
		return;
	}

	const QDateTime now = QDateTime::currentDateTimeUtc();
	std::vector<RecoverySnapshotInfo> snapshots = RecoveryCollectSnapshots();
	for ( const RecoverySnapshotInfo& snapshot : snapshots )
	{
		const bool tooOld = g_Recovery_MaxAgeDays > 0
		                    && snapshot.timestamp.isValid()
		                    && snapshot.timestamp.toUTC().daysTo( now ) >= g_Recovery_MaxAgeDays;
		if ( tooOld ) {
			QFile::remove( snapshot.mapFile );
			QFile::remove( snapshot.metaFile );
		}
	}

	snapshots = RecoveryCollectSnapshots();
	if ( g_Recovery_MaxSnapshots <= 0 || snapshots.size() <= static_cast<std::size_t>( g_Recovery_MaxSnapshots ) ) {
		return;
	}

	for ( std::size_t i = g_Recovery_MaxSnapshots; i < snapshots.size(); ++i )
	{
		QFile::remove( snapshots[i].mapFile );
		QFile::remove( snapshots[i].metaFile );
	}
}

QString RecoverySafeMapName(){
	QString name = Map_Unnamed( g_map ) ? "unnamed" : QString::fromLocal8Bit( path_get_filename_start( Map_Name( g_map ) ) );
	name.replace( QRegularExpression( "[^A-Za-z0-9._-]" ), "_" );
	if ( name.isEmpty() ) {
		name = "unnamed";
	}
	return name;
}
}

void AutoSave_clear(){
	s_changes = 0;
}

scene::Node& Map_Node(){
	return GlobalSceneGraph().root();
}

void QE_CheckAutoSave(){
	if ( !Map_Valid( g_map ) || !ScreenUpdates_Enabled() ) {
		return;
	}

	time_t now;
	time( &now );

	if ( s_start == 0 || s_changes == Node_getMapFile( Map_Node() )->changes() ) {
		s_start = now;
	}

	if ( ( now - s_start ) > ( 60 * m_AutoSave_Frequency ) ) {
		s_start = now;
		s_changes = Node_getMapFile( Map_Node() )->changes();

		if ( g_AutoSave_Enabled ) {
			const char* strMsg = g_SnapShots_Enabled ? "Autosaving snapshot..." : "Autosaving...";
			globalOutputStream() << strMsg << '\n';
			//Sys_Status( strMsg );

			// only snapshot if not working on a default map
			if ( g_SnapShots_Enabled && !Map_Unnamed( g_map ) ) {
				Map_Snapshot();
			}
			else
			{
				if ( Map_Unnamed( g_map ) ) {
					auto autosave = StringStream( g_qeglobals.m_userGamePath, "maps/" );
					Q_mkdir( autosave );
					autosave << "autosave.map";
					Map_SaveFile( autosave );
				}
				else
				{
					const char* name = Map_Name( g_map );
					const char* extension = path_get_filename_base_end( name );
					const auto autosave = StringStream( StringRange( name, extension ), ".autosave", extension );
					Map_SaveFile( autosave );
				}
			}

			if ( g_Recovery_Enabled ) {
				Recovery_CreateSnapshot();
			}
		}
		else
		{
			globalOutputStream() << "Autosave skipped...\n";
			//Sys_Status( "Autosave skipped..." );
		}
	}
}

void Recovery_CreateSnapshot(){
	QDir recoveryDir( RecoveryDirectoryPath() );
	if ( !recoveryDir.exists() && !recoveryDir.mkpath( "." ) ) {
		globalErrorStream() << "Recovery snapshot failed: unable to create " << RecoveryDirectoryPath().toLocal8Bit().constData() << '\n';
		return;
	}

	const QString timestamp = QDateTime::currentDateTimeUtc().toString( "yyyyMMdd-HHmmss-zzz" );
	const QString mapName = RecoverySafeMapName();
	const QString mapFileName = timestamp + "__" + mapName + ".map";
	const QString mapPath = recoveryDir.filePath( mapFileName );
	Map_SaveFile( mapPath.toLocal8Bit().constData() );

	QFileInfo mapInfo( mapPath );
	QFile metaFile( RecoveryMetaPathForMap( mapPath ) );
	if ( metaFile.open( QIODevice::WriteOnly | QIODevice::Text ) ) {
		QTextStream stream( &metaFile );
		stream << "timestamp=" << QDateTime::currentDateTimeUtc().toString( Qt::ISODateWithMs ) << '\n';
		stream << "sourceMapFileName=" << ( Map_Unnamed( g_map ) ? "unnamed.map" : QString::fromLocal8Bit( path_get_filename_start( Map_Name( g_map ) ) ) ) << '\n';
		stream << "fileSize=" << mapInfo.size() << '\n';
	}

	RecoveryCleanupSnapshots();
}

void Recovery_MarkSessionStart(){
	QDir recoveryDir( RecoveryDirectoryPath() );
	if ( !recoveryDir.exists() ) {
		recoveryDir.mkpath( "." );
	}

	QFile lockFile( RecoveryLockFilePath() );
	s_uncleanShutdownDetected = lockFile.exists();
	if ( lockFile.open( QIODevice::WriteOnly | QIODevice::Text ) ) {
		QTextStream stream( &lockFile );
		stream << QDateTime::currentDateTimeUtc().toString( Qt::ISODateWithMs ) << '\n';
	}
}

void Recovery_MarkSessionCleanShutdown(){
	QFile::remove( RecoveryLockFilePath() );
}

bool Recovery_CheckForUncleanShutdownAndPrompt(){
	if ( !s_uncleanShutdownDetected ) {
		return false;
	}

	const std::vector<RecoverySnapshotInfo> snapshots = RecoveryCollectSnapshots();
	if ( snapshots.empty() ) {
		qt_MessageBox( MainFrame_getWindow(), "Unclean shutdown detected, but no recovery snapshots were found.", "Recovery", EMessageBoxType::Warning );
		s_uncleanShutdownDetected = false;
		return false;
	}

	auto* dialog = new QDialog( MainFrame_getWindow() );
	dialog->setWindowTitle( "Recovery" );
	auto* layout = new QVBoxLayout( dialog );
	layout->addWidget( new QLabel( "Q3RallyRadiant detected an unclean shutdown.\nSelect a recovery snapshot to restore:" ) );

	auto* listWidget = new QListWidget( dialog );
	for ( const RecoverySnapshotInfo& snapshot : snapshots )
	{
		auto* item = new QListWidgetItem( RecoveryDisplayText( snapshot ), listWidget );
		item->setData( Qt::ItemDataRole::UserRole, snapshot.mapFile );
	}
	if ( listWidget->count() > 0 ) {
		listWidget->setCurrentRow( 0 );
	}
	layout->addWidget( listWidget );

	auto* buttonBox = new QDialogButtonBox( dialog );
	QPushButton* restoreButton = buttonBox->addButton( "Restore Selected", QDialogButtonBox::AcceptRole );
	QPushButton* discardButton = buttonBox->addButton( "Discard All Recovery Data", QDialogButtonBox::DestructiveRole );
	buttonBox->addButton( QDialogButtonBox::Cancel );
	layout->addWidget( buttonBox );

	enum class Choice { None, Restore, Discard };
	Choice choice = Choice::None;
	QObject::connect( restoreButton, &QPushButton::clicked, dialog, [&](){ choice = Choice::Restore; dialog->accept(); } );
	QObject::connect( discardButton, &QPushButton::clicked, dialog, [&](){ choice = Choice::Discard; dialog->accept(); } );
	QObject::connect( buttonBox, &QDialogButtonBox::rejected, dialog, &QDialog::reject );

	bool restored = false;
	if ( dialog->exec() == QDialog::Accepted ) {
		if ( choice == Choice::Restore ) {
			const auto* item = listWidget->currentItem();
			if ( item != nullptr ) {
				const QString mapFile = item->data( Qt::ItemDataRole::UserRole ).toString();
				Map_LoadFile( mapFile.toLocal8Bit().constData() );
				restored = true;
			}
		}
		else if ( choice == Choice::Discard ) {
			RecoveryRemoveAllData();
		}
	}
	delete dialog;

	s_uncleanShutdownDetected = false;
	return restored;
}

void Autosave_constructPreferences( PreferencesPage& page ){
	QCheckBox* autosave_enabled = page.appendCheckBox( "", "Enable Autosave", g_AutoSave_Enabled );
	QWidget* autosave_frequency = page.appendSpinner( "Autosave Frequency (minutes)", m_AutoSave_Frequency, 1, 60 );
	Widget_connectToggleDependency( autosave_frequency, autosave_enabled );
	page.appendCheckBox( "", "Save Snapshots", g_SnapShots_Enabled );
	page.appendCheckBox( "", "Enable Recovery Snapshots", g_Recovery_Enabled );
	page.appendSpinner( "Recovery max snapshots", g_Recovery_MaxSnapshots, 5, 200 );
	page.appendSpinner( "Recovery max age (days)", g_Recovery_MaxAgeDays, 1, 90 );
}
void Autosave_constructPage( PreferenceGroup& group ){
	PreferencesPage page( group.createPage( "Autosave", "Autosave Preferences" ) );
	Autosave_constructPreferences( page );
}
void Autosave_registerPreferencesPage(){
	PreferencesDialog_addSettingsPage( makeCallbackF( Autosave_constructPage ) );
}


#include "preferencesystem.h"
#include "stringio.h"

void Autosave_Construct(){
	GlobalPreferenceSystem().registerPreference( "Autosave", BoolImportStringCaller( g_AutoSave_Enabled ), BoolExportStringCaller( g_AutoSave_Enabled ) );
	GlobalPreferenceSystem().registerPreference( "AutosaveMinutes", IntImportStringCaller( m_AutoSave_Frequency ), IntExportStringCaller( m_AutoSave_Frequency ) );
	GlobalPreferenceSystem().registerPreference( "Snapshots", BoolImportStringCaller( g_SnapShots_Enabled ), BoolExportStringCaller( g_SnapShots_Enabled ) );
	GlobalPreferenceSystem().registerPreference( "RecoveryEnabled", BoolImportStringCaller( g_Recovery_Enabled ), BoolExportStringCaller( g_Recovery_Enabled ) );
	GlobalPreferenceSystem().registerPreference( "RecoveryMaxSnapshots", IntImportStringCaller( g_Recovery_MaxSnapshots ), IntExportStringCaller( g_Recovery_MaxSnapshots ) );
	GlobalPreferenceSystem().registerPreference( "RecoveryMaxAgeDays", IntImportStringCaller( g_Recovery_MaxAgeDays ), IntExportStringCaller( g_Recovery_MaxAgeDays ) );

	Autosave_registerPreferencesPage();
	RecoveryCleanupSnapshots();
}

void Autosave_Destroy(){
}
