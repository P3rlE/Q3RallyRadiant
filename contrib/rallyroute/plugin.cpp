/*
** RallyRoute plugin for Q3Rally Radiant.
** Visualizes and edits checkpoint Bezier routes stored on rally_checkpoint entities.
*/

#include "debugging/debugging.h"
#include "iplugin.h"
#include "igl.h"
#include "iglrender.h"
#include "irender.h"
#include "renderable.h"
#include "iscenegraph.h"
#include "ientity.h"
#include "iselection.h"
#include "iundo.h"
#include "scenelib.h"
#include "qerplugin.h"
#include "string/string.h"
#include "modulesystem/singletonmodule.h"
#include "signal/isignal.h"

#include <QWidget>
#include <QDialog>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFile>
#include <QTextStream>
#include <QDir>
#include <QByteArray>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

namespace {

struct Vec3 {
	float x = 0.f, y = 0.f, z = 0.f;
};

struct Checkpoint {
	int number = 0;
	Entity* entity = nullptr;
	Vec3 anchor;
	Vec3 handle;
	bool hasAnchor = false;
	bool hasHandle = false;
};

static Vec3 add( const Vec3& a, const Vec3& b ) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
static Vec3 subtract( const Vec3& a, const Vec3& b ) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static Vec3 scale( const Vec3& a, float s ) { return { a.x * s, a.y * s, a.z * s }; }

static bool parseVec3( const char* text, Vec3& value ) {
	if ( !text || !text[0] ) return false;
	float x, y, z;
	char tail;
	if ( sscanf( text, " %f %f %f %c", &x, &y, &z, &tail ) != 3 ) return false;
	if ( !std::isfinite( x ) || !std::isfinite( y ) || !std::isfinite( z ) ) return false;
	value = { x, y, z };
	return true;
}

static bool readCheckpoint( Entity* entity, Checkpoint& checkpoint ) {
	if ( !entity || !string_equal( entity->getClassName(), "rally_checkpoint" ) ) return false;
	const char* number = entity->getKeyValue( "number" );
	if ( !number || !number[0] ) return false;
	char* end = nullptr;
	const long parsed = strtol( number, &end, 10 );
	if ( end == number || parsed < 0 || parsed > 100000 ) return false;
	checkpoint.number = static_cast<int>( parsed );
	checkpoint.entity = entity;
	checkpoint.hasAnchor = parseVec3( entity->getKeyValue( "bezierPos" ), checkpoint.anchor );
	checkpoint.hasHandle = parseVec3( entity->getKeyValue( "bezierDir" ), checkpoint.handle );
	return true;
}

class CheckpointCollector : public scene::Graph::Walker {
	std::vector<Checkpoint>& m_checkpoints;
public:
	explicit CheckpointCollector( std::vector<Checkpoint>& checkpoints ) : m_checkpoints( checkpoints ) {}
	bool pre( const scene::Path& path, scene::Instance& ) const override {
		Entity* entity = Node_getEntity( path.top() );
		Checkpoint checkpoint;
		if ( readCheckpoint( entity, checkpoint ) ) m_checkpoints.push_back( checkpoint );
		return true;
	}
};

static std::vector<Checkpoint> collectCheckpoints() {
	std::vector<Checkpoint> checkpoints;
	CheckpointCollector collector( checkpoints );
	GlobalSceneGraph().traverse( collector );
	std::sort( checkpoints.begin(), checkpoints.end(), []( const Checkpoint& a, const Checkpoint& b ) {
		return a.number < b.number;
	} );
	return checkpoints;
}

static void bezierPoint( const Checkpoint& start, const Checkpoint& end, float t, Vec3& point ) {
	const Vec3 p1 = add( start.anchor, start.handle );
	const Vec3 p2 = subtract( end.anchor, end.handle );
	const float oneMinusT = 1.f - t;
	point = add( add( scale( start.anchor, oneMinusT * oneMinusT * oneMinusT ),
		scale( p1, 3.f * t * oneMinusT * oneMinusT ) ),
		add( scale( p2, 3.f * t * t * oneMinusT ), scale( end.anchor, t * t * t ) ) );
}

static void renderRoute() {
	const std::vector<Checkpoint> checkpoints = collectCheckpoints();
	if ( checkpoints.size() < 2 ) return;

	gl().glLineWidth( 3.f );
	gl().glColor3f( 0.1f, 0.95f, 0.55f );
	for ( std::size_t i = 0; i < checkpoints.size(); ++i ) {
		const Checkpoint& start = checkpoints[i];
		const Checkpoint& end = checkpoints[( i + 1 ) % checkpoints.size()];
		if ( !start.hasAnchor || !end.hasAnchor ) continue;
		gl().glBegin( GL_LINE_STRIP );
		for ( int sample = 0; sample <= 32; ++sample ) {
			Vec3 point;
			bezierPoint( start, end, sample / 32.f, point );
			gl().glVertex3f( point.x, point.y, point.z );
		}
		gl().glEnd();
	}

	gl().glLineWidth( 1.5f );
	gl().glBegin( GL_LINES );
	for ( const Checkpoint& checkpoint : checkpoints ) {
		if ( !checkpoint.hasAnchor ) continue;
		const Vec3 anchor = checkpoint.anchor;
		const Vec3 handlePoint = add( anchor, checkpoint.handle );
		const Vec3 nextControl = subtract( anchor, checkpoint.handle );
		gl().glColor3f( 1.f, 0.65f, 0.1f );
		gl().glVertex3f( anchor.x, anchor.y, anchor.z );
		gl().glVertex3f( handlePoint.x, handlePoint.y, handlePoint.z );
		gl().glColor3f( 0.15f, 0.7f, 1.f );
		gl().glVertex3f( anchor.x, anchor.y, anchor.z );
		gl().glVertex3f( nextControl.x, nextControl.y, nextControl.z );
	}
	gl().glEnd();

	gl().glPointSize( 7.f );
	gl().glBegin( GL_POINTS );
	for ( const Checkpoint& checkpoint : checkpoints ) {
		if ( !checkpoint.hasAnchor ) continue;
		gl().glColor3f( 1.f, 1.f, 0.15f );
		gl().glVertex3f( checkpoint.anchor.x, checkpoint.anchor.y, checkpoint.anchor.z );
		if ( checkpoint.hasHandle ) {
			const Vec3 control = add( checkpoint.anchor, checkpoint.handle );
			gl().glColor3f( 1.f, 0.65f, 0.1f );
			gl().glVertex3f( control.x, control.y, control.z );
		}
	}
	gl().glEnd();
}

static bool g_showRoute = true;
static QWidget* g_mainWindow = nullptr;

class RouteDrawWireframe : public OpenGLRenderable {
public:
	void render( RenderStateFlags ) const override { if ( g_showRoute ) renderRoute(); }
};

class RouteDrawSolid : public OpenGLRenderable {
public:
	void render( RenderStateFlags ) const override { if ( g_showRoute ) renderRoute(); }
};

class RouteRenderable : public Renderable {
	RouteDrawWireframe m_wireframe;
	RouteDrawSolid m_solid;
public:
	void renderWireframe( Renderer& renderer, const VolumeTest& ) const override {
		if ( g_showRoute ) renderer.addRenderable( m_wireframe, g_matrix4_identity );
	}
	void renderSolid( Renderer& renderer, const VolumeTest& ) const override {
		if ( g_showRoute ) renderer.addRenderable( m_solid, g_matrix4_identity );
	}
};

static RouteRenderable g_routeRenderable;

class SelectedCheckpointVisitor : public SelectionSystem::Visitor {
public:
	mutable Entity* entity = nullptr;
	void visit( scene::Instance& instance ) const override {
		Entity* candidate = Node_getEntity( instance.path().top() );
		Checkpoint checkpoint;
		if ( readCheckpoint( candidate, checkpoint ) ) entity = candidate;
	}
};

static Entity* selectedCheckpoint() {
	if ( GlobalSelectionSystem().countSelected() != 1 ) return nullptr;
	SelectedCheckpointVisitor visitor;
	GlobalSelectionSystem().foreachSelected( visitor );
	return visitor.entity;
}

class RouteEditorDialog : public QDialog {
	QLabel* m_selectionLabel = nullptr;
	QDoubleSpinBox* m_values[6] = {};
	static const char* s_names[6];
public:
	explicit RouteEditorDialog( QWidget* parent ) : QDialog( parent, Qt::Tool ) {
		setWindowTitle( "RallyRoute — Checkpoint Bezier" );
		auto* layout = new QVBoxLayout( this );
		m_selectionLabel = new QLabel( "Select one rally_checkpoint in the map." );
		layout->addWidget( m_selectionLabel );
		auto* form = new QFormLayout;
		for ( int i = 0; i < 6; ++i ) {
			m_values[i] = new QDoubleSpinBox;
			m_values[i]->setRange( -131072.0, 131072.0 );
			m_values[i]->setDecimals( 2 );
			m_values[i]->setSingleStep( 8.0 );
			form->addRow( s_names[i], m_values[i] );
		}
		layout->addLayout( form );
		layout->addWidget( new QLabel( "Anchor and handle offset are map-space route data; origin remains the respawn point." ) );
		auto* apply = new QPushButton( "Apply to selected checkpoint" );
		layout->addWidget( apply );
		connect( apply, &QPushButton::clicked, this, [this]() { applyToSelection(); } );
		updateFromSelection();
	}

	void updateFromSelection() {
		Entity* entity = selectedCheckpoint();
		Checkpoint checkpoint;
		if ( !readCheckpoint( entity, checkpoint ) ) {
			m_selectionLabel->setText( "Select exactly one rally_checkpoint in the map." );
			for ( auto* value : m_values ) value->setEnabled( false );
			return;
		}
		m_selectionLabel->setText( QString( "Checkpoint %1" ).arg( checkpoint.number ) );
		const Vec3 anchor = checkpoint.anchor;
		const Vec3 handle = checkpoint.handle;
		const float values[6] = { anchor.x, anchor.y, anchor.z, handle.x, handle.y, handle.z };
		for ( int i = 0; i < 6; ++i ) {
			m_values[i]->setValue( values[i] );
			m_values[i]->setEnabled( true );
		}
	}

private:
	void applyToSelection() {
		Entity* entity = selectedCheckpoint();
		Checkpoint checkpoint;
		if ( !readCheckpoint( entity, checkpoint ) ) return;
		const QByteArray anchor = QString( "%1 %2 %3" ).arg( m_values[0]->value(), 0, 'f', 3 )
			.arg( m_values[1]->value(), 0, 'f', 3 ).arg( m_values[2]->value(), 0, 'f', 3 ).toLocal8Bit();
		const QByteArray handle = QString( "%1 %2 %3" ).arg( m_values[3]->value(), 0, 'f', 3 )
			.arg( m_values[4]->value(), 0, 'f', 3 ).arg( m_values[5]->value(), 0, 'f', 3 ).toLocal8Bit();
		UndoableCommand undo( "RallyRoute.setCheckpointBezier" );
		entity->setKeyValue( "bezierPos", anchor.constData() );
		entity->setKeyValue( "bezierDir", handle.constData() );
		SceneChangeNotify();
	}
};

const char* RouteEditorDialog::s_names[6] = {
	"Anchor X", "Anchor Y", "Anchor Z", "Handle X", "Handle Y", "Handle Z"
};

static RouteEditorDialog* g_editor = nullptr;

static void updateEditorFromSelection( const Selectable& ) {
	if ( g_editor && g_editor->isVisible() ) g_editor->updateFromSelection();
}

static void showRouteStatus() {
	const std::vector<Checkpoint> checkpoints = collectCheckpoints();
	int anchors = 0, handles = 0;
	for ( const Checkpoint& checkpoint : checkpoints ) {
		if ( checkpoint.hasAnchor ) ++anchors;
		if ( checkpoint.hasHandle ) ++handles;
	}
	const QString text = QString( "Checkpoints: %1\nBezier anchors: %2/%1\nHandle vectors: %3/%1\n\nThe route preview uses bezierPos/bezierDir exactly as Q3Rally does. Missing anchors are skipped; missing handles act as zero offsets. Respawn origin is never used as a route point." )
		.arg( checkpoints.size() ).arg( anchors ).arg( handles );
	GlobalRadiant().m_pfnMessageBox( g_mainWindow, text.toLocal8Bit().constData(), "RallyRoute", EMessageBoxType::Info, 0 );
}

static void exportBpd() {
	const std::vector<Checkpoint> checkpoints = collectCheckpoints();
	if ( checkpoints.empty() ) {
		GlobalRadiant().m_pfnMessageBox( g_mainWindow, "No rally_checkpoint entities found in the map.", "RallyRoute", EMessageBoxType::Warning, 0 );
		return;
	}
	for ( const Checkpoint& checkpoint : checkpoints ) {
		if ( !checkpoint.hasAnchor ) {
			const QString error = QString( "Checkpoint %1 has no valid bezierPos. BPD export stopped." ).arg( checkpoint.number );
			GlobalRadiant().m_pfnMessageBox( g_mainWindow, error.toLocal8Bit().constData(), "RallyRoute", EMessageBoxType::Warning, 0 );
			return;
		}
	}
	const QString path = QFileDialog::getSaveFileName( g_mainWindow, "Export Q3Rally Bezier path", QDir::homePath(), "Bezier paths (*.bpd);;All files (*)" );
	if ( path.isEmpty() ) return;
	QFile file( path );
	if ( !file.open( QIODevice::WriteOnly | QIODevice::Text ) ) {
		GlobalRadiant().m_pfnMessageBox( g_mainWindow, "Could not open the selected BPD file for writing.", "RallyRoute", EMessageBoxType::Error, 0 );
		return;
	}
	QTextStream stream( &file );
	stream.setRealNumberNotation( QTextStream::FixedNotation );
	stream.setRealNumberPrecision( 3 );
	for ( const Checkpoint& checkpoint : checkpoints ) {
		const Vec3 handle = checkpoint.hasHandle ? checkpoint.handle : Vec3{};
		stream << checkpoint.number << " : "
			<< checkpoint.anchor.x << " " << checkpoint.anchor.y << " " << checkpoint.anchor.z << " : "
			<< handle.x << " " << handle.y << " " << handle.z << "\n";
	}
	file.close();
}

static void importBpd() {
	const QString path = QFileDialog::getOpenFileName( g_mainWindow, "Import Q3Rally Bezier path", QDir::homePath(), "Bezier paths (*.bpd);;All files (*)" );
	if ( path.isEmpty() ) return;
	QFile file( path );
	if ( !file.open( QIODevice::ReadOnly | QIODevice::Text ) ) {
		GlobalRadiant().m_pfnMessageBox( g_mainWindow, "Could not open the selected BPD file.", "RallyRoute", EMessageBoxType::Error, 0 );
		return;
	}
	std::map<int, Entity*> entities;
	for ( const Checkpoint& checkpoint : collectCheckpoints() ) entities[checkpoint.number] = checkpoint.entity;
	struct ImportedPoint { int number; Vec3 anchor; Vec3 handle; };
	std::vector<ImportedPoint> imported;
	QTextStream stream( &file );
	int lineNumber = 0;
	while ( !stream.atEnd() ) {
		const QByteArray line = stream.readLine().toLocal8Bit();
		++lineNumber;
		const char* p = line.constData();
		while ( *p == ' ' || *p == '\t' ) ++p;
		if ( !*p || *p == '#' || *p == '/' ) continue;
		ImportedPoint point{};
		char tail;
		if ( sscanf( p, " %d : %f %f %f : %f %f %f %c", &point.number,
			&point.anchor.x, &point.anchor.y, &point.anchor.z,
			&point.handle.x, &point.handle.y, &point.handle.z, &tail ) < 7 ||
			!std::isfinite( point.anchor.x ) || !std::isfinite( point.anchor.y ) || !std::isfinite( point.anchor.z ) ||
			!std::isfinite( point.handle.x ) || !std::isfinite( point.handle.y ) || !std::isfinite( point.handle.z ) ) {
			const QString error = QString( "Invalid BPD record on line %1." ).arg( lineNumber );
			GlobalRadiant().m_pfnMessageBox( g_mainWindow, error.toLocal8Bit().constData(), "RallyRoute", EMessageBoxType::Error, 0 );
			return;
		}
		if ( entities.find( point.number ) == entities.end() ) {
			const QString error = QString( "BPD checkpoint %1 has no matching rally_checkpoint in this map." ).arg( point.number );
			GlobalRadiant().m_pfnMessageBox( g_mainWindow, error.toLocal8Bit().constData(), "RallyRoute", EMessageBoxType::Error, 0 );
			return;
		}
		if ( std::any_of( imported.begin(), imported.end(), [&point]( const ImportedPoint& other ) { return other.number == point.number; } ) ) {
			const QString error = QString( "BPD checkpoint %1 occurs more than once." ).arg( point.number );
			GlobalRadiant().m_pfnMessageBox( g_mainWindow, error.toLocal8Bit().constData(), "RallyRoute", EMessageBoxType::Error, 0 );
			return;
		}
		imported.push_back( point );
	}
	file.close();
	if ( imported.empty() ) {
		GlobalRadiant().m_pfnMessageBox( g_mainWindow, "The BPD file contains no checkpoint records.", "RallyRoute", EMessageBoxType::Warning, 0 );
		return;
	}
	if ( imported.size() != entities.size() ) {
		GlobalRadiant().m_pfnMessageBox( g_mainWindow, "The BPD checkpoint count does not match this map. No values were imported.", "RallyRoute", EMessageBoxType::Error, 0 );
		return;
	}
	UndoableCommand undo( "RallyRoute.importBpd" );
	for ( const ImportedPoint& point : imported ) {
		Entity* entity = entities[point.number];
		const QByteArray anchor = QString( "%1 %2 %3" ).arg( point.anchor.x, 0, 'f', 3 )
			.arg( point.anchor.y, 0, 'f', 3 ).arg( point.anchor.z, 0, 'f', 3 ).toLocal8Bit();
		const QByteArray handle = QString( "%1 %2 %3" ).arg( point.handle.x, 0, 'f', 3 )
			.arg( point.handle.y, 0, 'f', 3 ).arg( point.handle.z, 0, 'f', 3 ).toLocal8Bit();
		entity->setKeyValue( "bezierPos", anchor.constData() );
		entity->setKeyValue( "bezierDir", handle.constData() );
	}
	SceneChangeNotify();
}

} // namespace

namespace RallyRoutePlugin {

static QWidget* main_window = nullptr;

const char* init( void*, void* parent ) {
	main_window = static_cast<QWidget*>( parent );
	g_mainWindow = main_window;
	GlobalShaderCache().attachRenderable( g_routeRenderable );
	typedef FreeCaller<void(const Selectable&), updateEditorFromSelection> SelectionChangedCaller;
	GlobalSelectionSystem().addSelectionChangeCallback( makeSignalHandler1( SelectionChangedCaller() ) );
	return "Initializing RallyRoute Bezier editor";
}

const char* getName() { return "RallyRoute"; }
const char* getCommandTitleList() { return ""; }
const char* getCommandList() { return "Toggle Bezier Route;Edit Selected Checkpoint...;Import BPD...;Export BPD...;Route Status"; }

void dispatch( const char* command, float*, float*, bool ) {
	if ( string_equal( command, "Toggle Bezier Route" ) ) {
		g_showRoute = !g_showRoute;
		SceneChangeNotify();
	} else if ( string_equal( command, "Edit Selected Checkpoint..." ) ) {
		if ( !g_editor ) g_editor = new RouteEditorDialog( main_window );
		g_editor->updateFromSelection();
		g_editor->show();
		g_editor->raise();
	} else if ( string_equal( command, "Export BPD..." ) ) {
		exportBpd();
	} else if ( string_equal( command, "Import BPD..." ) ) {
		importBpd();
	} else if ( string_equal( command, "Route Status" ) ) {
		showRouteStatus();
	}
}

} // namespace RallyRoutePlugin

class RallyRoutePluginDependencies :
	public GlobalRadiantModuleRef,
	public GlobalSceneGraphModuleRef,
	public GlobalUndoModuleRef,
	public GlobalEntityModuleRef,
	public GlobalSelectionModuleRef,
	public GlobalShaderCacheModuleRef,
	public GlobalOpenGLModuleRef,
	public GlobalOpenGLStateLibraryModuleRef
{
public:
	RallyRoutePluginDependencies() : GlobalEntityModuleRef( GlobalRadiant().getRequiredGameDescriptionKeyValue( "entities" ) ) {}
};

class RallyRoutePluginModule : public TypeSystemRef {
	_QERPluginTable m_plugin;
public:
	typedef _QERPluginTable Type;
	STRING_CONSTANT( Name, "RallyRoute" );
	RallyRoutePluginModule() {
		m_plugin.m_pfnQERPlug_Init = &RallyRoutePlugin::init;
		m_plugin.m_pfnQERPlug_GetName = &RallyRoutePlugin::getName;
		m_plugin.m_pfnQERPlug_GetCommandList = &RallyRoutePlugin::getCommandList;
		m_plugin.m_pfnQERPlug_GetCommandTitleList = &RallyRoutePlugin::getCommandTitleList;
		m_plugin.m_pfnQERPlug_Dispatch = &RallyRoutePlugin::dispatch;
	}
	_QERPluginTable* getTable() { return &m_plugin; }
};

typedef SingletonModule<RallyRoutePluginModule, RallyRoutePluginDependencies> SingletonRallyRouteModule;
SingletonRallyRouteModule g_RallyRouteModule;

extern "C" void RADIANT_DLLEXPORT Radiant_RegisterModules( ModuleServer& server ) {
	initialiseModule( server );
	g_RallyRouteModule.selfRegister();
}
