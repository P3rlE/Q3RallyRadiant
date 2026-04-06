/*
** BotViz plugin for Q3Rally Radiant
** Renders bot JSONL debug routes in the 3D camera view.
**
** Pattern: prtview (render hook) + sunplug (menu/Qt dialog structure)
*/

#include "botviz.h"

#include "debugging/debugging.h"
#include "iplugin.h"
#include "iglrender.h"
#include "irender.h"
#include "renderable.h"
#include "iscenegraph.h"
#include "ientity.h"
#include "scenelib.h"
#include "qerplugin.h"
#include "stream/stringstream.h"
#include "string/string.h"
#include "modulesystem/singletonmodule.h"

#include <QWidget>
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSlider>
#include <QLabel>
#include <QPushButton>
#include <QFileDialog>
#include <QString>

#include <map>

// ── Scene walker: collect bot_path_node origins ───────────────────────────────

class BotNodeCollector : public scene::Graph::Walker {
	std::map<int, Vector3>& m_nodes;
public:
	BotNodeCollector( std::map<int,Vector3>& nodes ) : m_nodes( nodes ) {}

	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		Entity* ent = Node_getEntity( path.top() );
		if ( !ent ) return true;
		if ( !string_equal( ent->getClassName(), "bot_path_node" ) ) return true;

		const char* orderStr  = ent->getKeyValue( "order" );
		const char* originStr = ent->getKeyValue( "origin" );
		if ( !orderStr || !originStr || !orderStr[0] || !originStr[0] ) return true;

		int order = atoi( orderStr );
		float x = 0, y = 0, z = 0;
		sscanf( originStr, "%f %f %f", &x, &y, &z );
		m_nodes[order] = Vector3( x, y, z );
		return true;
	}
};

static std::map<int,Vector3> g_nodePositions;

static int collectNodes() {
	g_nodePositions.clear();
	BotNodeCollector collector( g_nodePositions );
	GlobalSceneGraph().traverse( collector );
	globalOutputStream() << "[BotViz] Found " << (int)g_nodePositions.size() << " bot_path_nodes\n";
	return (int)g_nodePositions.size();
}

// ── Renderable classes (same pattern as prtview) ──────────────────────────────

class BotVizDrawWireframe : public OpenGLRenderable {
public:
	void render( RenderStateFlags ) const override {
		BotViz::instance().renderWireframe();
	}
};

class BotVizDrawSolid : public OpenGLRenderable {
public:
	void render( RenderStateFlags ) const override {
		BotViz::instance().renderSolid();
	}
};

class BotVizRenderable : public Renderable {
public:
	BotVizDrawWireframe m_wireframe;
	BotVizDrawSolid     m_solid;

	void renderWireframe( Renderer& renderer, const VolumeTest& ) const override {
		if ( BotViz::instance().isLoaded() && BotViz::instance().showRoute )
			renderer.addRenderable( m_wireframe, g_matrix4_identity );
	}
	void renderSolid( Renderer& renderer, const VolumeTest& ) const override {
		if ( BotViz::instance().isLoaded() )
			renderer.addRenderable( m_solid, g_matrix4_identity );
	}
};

BotVizRenderable g_botvizRenderable;

// ── Timeline dialog ───────────────────────────────────────────────────────────

static QWidget* g_mainWindow = nullptr;

class TimelineDialog : public QDialog {
	QSlider* m_slider;
	QLabel*  m_lblFrame;
	QLabel*  m_lblTime;
	QLabel*  m_lblSpeed;
	QLabel*  m_lblState;

public:
	TimelineDialog( QWidget* parent ) : QDialog( parent, Qt::Tool ) {
		setWindowTitle( "BotViz — Timeline" );
		setMinimumWidth( 480 );

		auto* vbox = new QVBoxLayout( this );

		// Info row
		auto* infoRow = new QHBoxLayout;
		m_lblFrame = new QLabel( "Frame: —" );
		m_lblTime  = new QLabel( "t: —" );
		m_lblSpeed = new QLabel( "Speed: —" );
		m_lblState = new QLabel( "—" );
		infoRow->addWidget( m_lblFrame );
		infoRow->addWidget( m_lblTime );
		infoRow->addWidget( m_lblSpeed );
		infoRow->addWidget( m_lblState );
		vbox->addLayout( infoRow );

		// Slider
		m_slider = new QSlider( Qt::Horizontal );
		m_slider->setMinimum( 0 );
		m_slider->setMaximum( 0 );
		connect( m_slider, &QSlider::valueChanged, this, &TimelineDialog::onSlider );
		vbox->addWidget( m_slider );

		// Buttons
		auto* btnRow = new QHBoxLayout;
		auto* btnRefresh = new QPushButton( "Refresh nodes" );
		connect( btnRefresh, &QPushButton::clicked, [this]() {
			collectNodes();
			BotViz::instance().resolvePositions( g_nodePositions );
			SceneChangeNotify();
		});
		auto* btnToggleCollisions = new QPushButton( "Toggle collisions" );
		connect( btnToggleCollisions, &QPushButton::clicked, []() {
			BotViz::instance().showCollisions = !BotViz::instance().showCollisions;
			SceneChangeNotify();
		});
		btnRow->addWidget( btnRefresh );
		btnRow->addWidget( btnToggleCollisions );
		btnRow->addStretch();
		vbox->addLayout( btnRow );
	}

	void reset( int frameCount ) {
		m_slider->setMaximum( std::max( 0, frameCount - 1 ) );
		m_slider->setValue( 0 );
		updateLabels( 0 );
	}

private:
	void onSlider( int value ) {
		BotViz::instance().playhead = value;
		updateLabels( value );
		SceneChangeNotify();
	}

	void updateLabels( int frame ) {
		const BotFrame* fr = BotViz::instance().frame( frame );
		if ( !fr ) return;
		m_lblFrame->setText( QString("Frame: %1 / %2").arg(frame).arg(BotViz::instance().frameCount()-1) );
		m_lblTime->setText( QString("t=%1s").arg(fr->time, 0, 'f', 2) );
		m_lblSpeed->setText( QString("Speed: %1").arg((int)fr->actualSpeed) );
		m_lblState->setText( QString(fr->decisionState) );
	}
};

static TimelineDialog* g_timeline = nullptr;

static void ensureTimeline() {
	if ( !g_timeline )
		g_timeline = new TimelineDialog( g_mainWindow );
}

// ── Menu dispatch ─────────────────────────────────────────────────────────────

namespace BotVizPlugin {

QWidget* main_window = nullptr;

const char* init( void* hApp, void* pMainWidget ) {
	main_window  = static_cast<QWidget*>( pMainWidget );
	g_mainWindow = main_window;
	GlobalShaderCache().attachRenderable( g_botvizRenderable );
	return "Initializing BotViz for Q3Rally";
}

const char* getName() {
	return "BotViz";
}

const char* getCommandList() {
	return "Load JSONL...;-;Toggle Route;Toggle Collisions;-;Show Timeline";
}

const char* getCommandTitleList() {
	return "";
}

void dispatch( const char* command, float*, float*, bool ) {
	if ( string_equal( command, "Load JSONL..." ) ) {
		QString path = QFileDialog::getOpenFileName(
			main_window,
			"Load Bot Debug JSONL",
			QString(),
			"JSONL files (*.jsonl);;All files (*)"
		);
		if ( path.isEmpty() ) return;

		int n = collectNodes();
		if ( n == 0 ) {
			GlobalRadiant().m_pfnMessageBox( main_window,
				"No bot_path_nodes found in the current map.\n"
				"Load a Q3Rally map first.",
				"BotViz", EMessageBoxType::Warning, 0 );
			return;
		}

		int frames = BotViz::instance().load( path.toUtf8().constData() );
		if ( frames < 0 ) {
			GlobalRadiant().m_pfnMessageBox( main_window,
				"Failed to open JSONL file.",
				"BotViz", EMessageBoxType::Error, 0 );
			return;
		}

		BotViz::instance().resolvePositions( g_nodePositions );
		globalOutputStream() << "[BotViz] Loaded " << frames << " frames\n";

		ensureTimeline();
		g_timeline->reset( frames );
		g_timeline->show();
		g_timeline->raise();

		SceneChangeNotify();
		return;
	}

	if ( string_equal( command, "Toggle Route" ) ) {
		BotViz::instance().showRoute = !BotViz::instance().showRoute;
		SceneChangeNotify();
		return;
	}

	if ( string_equal( command, "Toggle Collisions" ) ) {
		BotViz::instance().showCollisions = !BotViz::instance().showCollisions;
		SceneChangeNotify();
		return;
	}

	if ( string_equal( command, "Show Timeline" ) ) {
		if ( !BotViz::instance().isLoaded() ) {
			GlobalRadiant().m_pfnMessageBox( main_window,
				"No JSONL loaded yet. Use 'Load JSONL...' first.",
				"BotViz", EMessageBoxType::Info, 0 );
			return;
		}
		ensureTimeline();
		g_timeline->show();
		g_timeline->raise();
		return;
	}
}

} // namespace BotVizPlugin

// ── Module registration ───────────────────────────────────────────────────────

class BotVizPluginDependencies :
	public GlobalRadiantModuleRef,
	public GlobalSceneGraphModuleRef,
	public GlobalEntityModuleRef,
	public GlobalShaderCacheModuleRef,
	public GlobalOpenGLModuleRef,
	public GlobalOpenGLStateLibraryModuleRef
{
public:
	BotVizPluginDependencies() :
		GlobalEntityModuleRef( GlobalRadiant().getRequiredGameDescriptionKeyValue( "entities" ) )
	{}
};

class BotVizPluginModule : public TypeSystemRef {
	_QERPluginTable m_plugin;
public:
	typedef _QERPluginTable Type;
	STRING_CONSTANT( Name, "BotViz" );

	BotVizPluginModule() {
		m_plugin.m_pfnQERPlug_Init             = &BotVizPlugin::init;
		m_plugin.m_pfnQERPlug_GetName          = &BotVizPlugin::getName;
		m_plugin.m_pfnQERPlug_GetCommandList   = &BotVizPlugin::getCommandList;
		m_plugin.m_pfnQERPlug_GetCommandTitleList = &BotVizPlugin::getCommandTitleList;
		m_plugin.m_pfnQERPlug_Dispatch         = &BotVizPlugin::dispatch;
	}

	_QERPluginTable* getTable() { return &m_plugin; }
};

typedef SingletonModule<BotVizPluginModule, BotVizPluginDependencies> SingletonBotVizModule;
SingletonBotVizModule g_BotVizModule;

extern "C" void RADIANT_DLLEXPORT Radiant_RegisterModules( ModuleServer& server ) {
	initialiseModule( server );
	g_BotVizModule.selfRegister();
}
