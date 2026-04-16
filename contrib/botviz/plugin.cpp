/*
** BotViz plugin for Q3Rally Radiant
** Renders bot JSONL debug routes in the 3D camera view.
** Shows per-node telemetry when a bot_path_node is selected.
*/

#include "botviz.h"

#include "debugging/debugging.h"
#include "iplugin.h"
#include "iglrender.h"
#include "irender.h"
#include "renderable.h"
#include "iscenegraph.h"
#include "iselection.h"
#include "ientity.h"
#include "scenelib.h"
#include "qerplugin.h"
#include "stream/stringstream.h"
#include "string/string.h"
#include "modulesystem/singletonmodule.h"
#include "signal/isignal.h"

#include <QWidget>
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QSlider>
#include <QLabel>
#include <QPushButton>
#include <QFileDialog>
#include <QGroupBox>
#include <QSpinBox>
#include <QString>
#include <QComboBox>
#include <QCheckBox>

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

// ── Selected entity reader ────────────────────────────────────────────────────

// Returns the order key of the selected bot_path_node, or -1 if no unique valid selection exists.
static int getSelectedNodeOrder() {
	if ( GlobalSelectionSystem().countSelected() != 1 )
		return -1;

	class SelectedNodeOrderVisitor : public SelectionSystem::Visitor {
	public:
		mutable int  order = -1;
		mutable bool hasValidOrder = false;

		void visit( scene::Instance& instance ) const override {
			Entity* ent = Node_getEntity( instance.path().top() );
			if ( !ent ) return;
			if ( !string_equal( ent->getClassName(), "bot_path_node" ) ) return;

			const char* orderStr = ent->getKeyValue( "order" );
			if ( !orderStr || !orderStr[0] ) return;
			order = atoi( orderStr );
			hasValidOrder = true;
		}
	};

	SelectedNodeOrderVisitor visitor;
	GlobalSelectionSystem().foreachSelected( visitor );
	return visitor.hasValidOrder ? visitor.order : -1;
}

// ── Node Stats Panel ──────────────────────────────────────────────────────────

class NodeStatsPanel : public QDialog {
	QLabel* m_lblNode;
	QLabel* m_lblFrames;
	QLabel* m_lblAvgSpeed;
	QLabel* m_lblMaxSpeed;
	QLabel* m_lblMinSpeed;
	QLabel* m_lblCollisions;
	QLabel* m_lblTopState;
	QLabel* m_lblNoData;
	QGroupBox* m_statsBox = nullptr;
	QCheckBox* m_autoSelection = nullptr;

public:
	NodeStatsPanel( QWidget* parent )
		: QDialog( parent, Qt::Tool | Qt::WindowStaysOnTopHint )
	{
		setWindowTitle( "BotViz — Node Telemetry" );
		setMinimumWidth( 280 );

		auto* vbox = new QVBoxLayout( this );

		// Node selector row
		auto* selectorRow = new QHBoxLayout;
		selectorRow->addWidget( new QLabel( "Node order:" ) );
		auto* spin = new QSpinBox;
		spin->setMinimum( 0 );
		spin->setMaximum( 9999 );
		spin->setValue( 0 );
		selectorRow->addWidget( spin );
		auto* btnLookup = new QPushButton( "Show" );
		selectorRow->addWidget( btnLookup );
		vbox->addLayout( selectorRow );

		connect( btnLookup, &QPushButton::clicked, [this, spin]() {
			showStats( spin->value() );
		});
		connect( spin, QOverload<int>::of(&QSpinBox::valueChanged), [this]( int val ) {
			showStats( val );
		});

		m_autoSelection = new QCheckBox( "Auto from selection" );
		m_autoSelection->setChecked( true );
		vbox->addWidget( m_autoSelection );

		m_lblNoData = new QLabel( "Load a JSONL file and\nenter a node order number." );
		m_lblNoData->setAlignment( Qt::AlignCenter );
		m_lblNoData->setStyleSheet( "color: gray; padding: 12px;" );
		vbox->addWidget( m_lblNoData );

		auto* box = new QGroupBox( "Node Statistics" );
		auto* grid = new QGridLayout( box );

		auto addRow = [&]( int row, const QString& label, QLabel*& valueLabel ) {
			grid->addWidget( new QLabel( label ), row, 0 );
			valueLabel = new QLabel( "—" );
			valueLabel->setAlignment( Qt::AlignRight );
			grid->addWidget( valueLabel, row, 1 );
		};

		addRow( 0, "Node order:",     m_lblNode );
		addRow( 1, "Frames logged:",  m_lblFrames );
		addRow( 2, "Avg speed:",      m_lblAvgSpeed );
		addRow( 3, "Max speed:",      m_lblMaxSpeed );
		addRow( 4, "Min speed:",      m_lblMinSpeed );
		addRow( 5, "Collisions:",     m_lblCollisions );
		addRow( 6, "Top state:",      m_lblTopState );

		vbox->addWidget( box );
		box->hide();
		m_statsBox = box;
	}

	void showStats( int order ) {
		if ( !BotViz::instance().hasNodeStats() ) {
			showNoData( "Load a JSONL file first." );
			return;
		}
		const NodeStats* s = BotViz::instance().nodeStats( order );
		if ( !s ) {
			showNoData( QString( "No data for node %1." ).arg( order ) );
			return;
		}
		m_lblNoData->hide();
		m_statsBox->show();
		m_lblNode->setText(       QString::number( s->nodeOrder ) );
		m_lblFrames->setText(     QString::number( s->frameCount ) );
		m_lblAvgSpeed->setText(   QString( "%1 u/s" ).arg( (int)s->avgSpeed ) );
		m_lblMaxSpeed->setText(   QString( "%1 u/s" ).arg( (int)s->maxSpeed ) );
		m_lblMinSpeed->setText(   QString( "%1 u/s" ).arg( (int)s->minSpeed ) );
		m_lblCollisions->setText( QString::number( s->collisions ) );
		m_lblTopState->setText(   QString( s->topState ) );
		m_lblCollisions->setStyleSheet(
			s->collisions > 0 ? "color: #cc2222; font-weight: bold;" : "" );
		adjustSize();
	}

	void showEmpty() {
		showNoData( "No data / selection ambiguous:\nselect exactly one bot_path_node." );
	}

	bool autoFromSelectionEnabled() const {
		return m_autoSelection && m_autoSelection->isChecked();
	}

	void updateFromSelection() {
		const int order = getSelectedNodeOrder();
		if ( order >= 0 )
			showStats( order );
		else
			showEmpty();
	}

private:
	void showNoData( const QString& msg ) {
		m_lblNoData->setText( msg );
		m_lblNoData->show();
		if ( m_statsBox ) m_statsBox->hide();
		adjustSize();
	}
};

static NodeStatsPanel* g_statsPanel = nullptr;

static void updateStatsPanelFromSelection( const Selectable& ) {
	if ( !g_statsPanel || !g_statsPanel->isVisible() ) return;
	if ( !g_statsPanel->autoFromSelectionEnabled() ) return;
	g_statsPanel->updateFromSelection();
}

// ── Renderable ────────────────────────────────────────────────────────────────

class BotVizDrawWireframe : public OpenGLRenderable {
public:
	void render( RenderStateFlags ) const override { BotViz::instance().renderWireframe(); }
};

class BotVizDrawSolid : public OpenGLRenderable {
public:
	void render( RenderStateFlags ) const override { BotViz::instance().renderSolid(); }
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
	QLabel*  m_lblLegend;
	QComboBox* m_modeCombo;

public:
	TimelineDialog( QWidget* parent ) : QDialog( parent, Qt::Tool ) {
		setWindowTitle( "BotViz — Timeline" );
		setMinimumWidth( 480 );

		auto* vbox = new QVBoxLayout( this );

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

		m_slider = new QSlider( Qt::Horizontal );
		m_slider->setMinimum( 0 );
		m_slider->setMaximum( 0 );
		connect( m_slider, &QSlider::valueChanged, this, &TimelineDialog::onSlider );
		vbox->addWidget( m_slider );

		auto* modeRow = new QHBoxLayout;
		modeRow->addWidget( new QLabel( "View mode:" ) );
		m_modeCombo = new QComboBox;
		m_modeCombo->addItem( "Route" );
		m_modeCombo->addItem( "Heatmap" );
		connect( m_modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), []( int idx ) {
			BotViz::instance().renderMode =
				( idx == 1 ) ? BotVizRenderMode::Heatmap : BotVizRenderMode::Route;
			SceneChangeNotify();
		} );
		modeRow->addWidget( m_modeCombo );
		modeRow->addStretch();
		vbox->addLayout( modeRow );

		m_lblLegend = new QLabel(
			"Legend (Heatmap): blue = low collision risk, red = high collision risk" );
		m_lblLegend->setStyleSheet( "color: #7aa7d9; padding-bottom: 6px;" );
		vbox->addWidget( m_lblLegend );

		auto* btnRow = new QHBoxLayout;

		auto* btnRefresh = new QPushButton( "Refresh nodes" );
		connect( btnRefresh, &QPushButton::clicked, [this]() {
			collectNodes();
			BotViz::instance().resolvePositions( g_nodePositions );
			SceneChangeNotify();
		});

		auto* btnCollisions = new QPushButton( "Toggle collisions" );
		connect( btnCollisions, &QPushButton::clicked, []() {
			BotViz::instance().showCollisions = !BotViz::instance().showCollisions;
			SceneChangeNotify();
		});

		auto* btnNodeStats = new QPushButton( "Node Telemetry" );
		connect( btnNodeStats, &QPushButton::clicked, []() {
			if ( g_statsPanel ) {
				g_statsPanel->show();
				g_statsPanel->raise();
				g_statsPanel->updateFromSelection();
			}
		});

		auto* btnExport = new QPushButton( "Export Aggregation..." );
		connect( btnExport, &QPushButton::clicked, [this]() {
			if ( !BotViz::instance().isLoaded() ) return;
			QString path = QFileDialog::getSaveFileName(
				this, "Export Aggregated Route Data", QString(),
				"CSV files (*.csv);;JSON files (*.json)" );
			if ( path.isEmpty() ) return;
			bool ok = false;
			if ( path.endsWith( ".json", Qt::CaseInsensitive ) )
				ok = BotViz::instance().exportBucketsJson( path.toUtf8().constData() );
			else
				ok = BotViz::instance().exportBucketsCsv( path.toUtf8().constData() );

			if ( !ok ) {
				GlobalRadiant().m_pfnMessageBox( this,
					"Export failed. Ensure a JSONL file is loaded with resolvable route data.",
					"BotViz", EMessageBoxType::Error, 0 );
			}
		} );

		btnRow->addWidget( btnRefresh );
		btnRow->addWidget( btnCollisions );
		btnRow->addWidget( btnNodeStats );
		btnRow->addWidget( btnExport );
		btnRow->addStretch();
		vbox->addLayout( btnRow );
	}

	void reset( int frameCount ) {
		m_slider->setMaximum( std::max( 0, frameCount - 1 ) );
		m_slider->setValue( 0 );
		m_modeCombo->setCurrentIndex(
			BotViz::instance().renderMode == BotVizRenderMode::Heatmap ? 1 : 0 );
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
		m_lblTime->setText(  QString("t=%1s").arg(fr->time, 0, 'f', 2) );
		m_lblSpeed->setText( QString("Speed: %1").arg((int)fr->actualSpeed) );
		m_lblState->setText( QString(fr->decisionState) );
	}
};

static TimelineDialog* g_timeline = nullptr;

static void ensureTimeline() {
	if ( !g_timeline )
		g_timeline = new TimelineDialog( g_mainWindow );
}

static void ensureStatsPanel() {
	if ( !g_statsPanel ) {
		g_statsPanel = new NodeStatsPanel( g_mainWindow );
		typedef FreeCaller<void(const Selectable&), updateStatsPanelFromSelection> StatsPanelSelectionChangedCaller;
		GlobalSelectionSystem().addSelectionChangeCallback( makeSignalHandler1( StatsPanelSelectionChangedCaller() ) );
	}
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

const char* getName()            { return "BotViz"; }
const char* getCommandTitleList(){ return ""; }

const char* getCommandList() {
	return "Load JSONL...;-;Toggle Route;Toggle Collisions;-;Show Timeline;Node Telemetry";
}

void dispatch( const char* command, float*, float*, bool ) {

	if ( string_equal( command, "Load JSONL..." ) ) {
		QString path = QFileDialog::getOpenFileName(
			main_window, "Load Bot Debug JSONL", QString(),
			"JSONL files (*.jsonl);;All files (*)" );
		if ( path.isEmpty() ) return;

		int n = collectNodes();
		if ( n == 0 ) {
			GlobalRadiant().m_pfnMessageBox( main_window,
				"No bot_path_nodes found in the current map.\n"
				"Load a Q3Rally map first.",
				"BotViz", EMessageBoxType::Warning, 0 );
			return;
		}

		// Hide stats panel during load (stops QTimer)
		if ( g_statsPanel && g_statsPanel->isVisible() )
			g_statsPanel->hide();

		int frames = BotViz::instance().load( path.toUtf8().constData() );
		if ( frames < 0 ) {
			GlobalRadiant().m_pfnMessageBox( main_window,
				"Failed to open JSONL file.",
				"BotViz", EMessageBoxType::Error, 0 );
			return;
		}

		BotViz::instance().resolvePositions( g_nodePositions );

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

	if ( string_equal( command, "Node Telemetry" ) ) {
		ensureStatsPanel();
		g_statsPanel->show();
		g_statsPanel->raise();
		g_statsPanel->updateFromSelection();
		return;
	}
}

} // namespace BotVizPlugin

// ── Module registration ───────────────────────────────────────────────────────

class BotVizPluginDependencies :
	public GlobalRadiantModuleRef,
	public GlobalSceneGraphModuleRef,
	public GlobalEntityModuleRef,
	public GlobalSelectionModuleRef,
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
		m_plugin.m_pfnQERPlug_Init                = &BotVizPlugin::init;
		m_plugin.m_pfnQERPlug_GetName             = &BotVizPlugin::getName;
		m_plugin.m_pfnQERPlug_GetCommandList      = &BotVizPlugin::getCommandList;
		m_plugin.m_pfnQERPlug_GetCommandTitleList = &BotVizPlugin::getCommandTitleList;
		m_plugin.m_pfnQERPlug_Dispatch            = &BotVizPlugin::dispatch;
	}

	_QERPluginTable* getTable() { return &m_plugin; }
};

typedef SingletonModule<BotVizPluginModule, BotVizPluginDependencies> SingletonBotVizModule;
SingletonBotVizModule g_BotVizModule;

extern "C" void RADIANT_DLLEXPORT Radiant_RegisterModules( ModuleServer& server ) {
	initialiseModule( server );
	g_BotVizModule.selfRegister();
}
