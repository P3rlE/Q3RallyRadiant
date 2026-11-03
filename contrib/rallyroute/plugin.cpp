/*
** RallyRoute plugin for Q3Rally Radiant.
** Visualizes and edits checkpoint Bezier routes stored on rally_checkpoint entities.
**
** The BPD files next to the .map are the reference route that ships with the game.
** Maps whose checkpoints differ between track lengths (onlyTrackLength) keep one
** file per length, <map>_tl<n>.bpd; the plugin always works on one track length.
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
#include "math/aabb.h"
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
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QSlider>
#include <QGroupBox>
#include <QSignalBlocker>
#include <QTimer>
#include <QFileDialog>
#include <QFile>
#include <QTextStream>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QByteArray>
#include <QLocale>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <limits>
#include <vector>

namespace {

struct Vec3 {
	float x = 0.f, y = 0.f, z = 0.f;
};

struct Checkpoint {
	int number = 0;
	Entity* entity = nullptr;
	scene::Path path;
	Vec3 anchor;
	Vec3 handle;
	bool hasAnchor = false;
	bool hasHandle = false;
	bool hasBounds = false;
	Vec3 boundsCenter;
	Vec3 boundsExtents;
};

struct SprintEndpoints {
	Vec3 start;
	Vec3 finish;
	bool valid = false;
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

/* Track lengths as in g_trackLength: 0 short, 1 medium, 2 long. */
static const int k_trackLengthCount = 3;
static const char* const k_trackLengthNames[k_trackLengthCount] = { "Short", "Medium", "Long" };
static int g_trackLength = 0;

/* Same rule as the game's spawn filter: a missing key means all lengths. */
static const char* onlyTrackLengthOf( const Entity* entity ) {
	if ( !entity ) return "012";
	if ( entity->hasKeyValue( "onlyTrackLength" ) ) return entity->getKeyValue( "onlyTrackLength" );
	if ( entity->hasKeyValue( "onlytracklength" ) ) return entity->getKeyValue( "onlytracklength" );
	return "012";
}

static bool entityInTrackLength( const Entity* entity, int trackLength ) {
	if ( trackLength < 0 ) return true;
	return strchr( onlyTrackLengthOf( entity ), '0' + trackLength ) != nullptr;
}

static bool isLengthRestricted( const Entity* entity ) {
	const char* value = onlyTrackLengthOf( entity );
	return !strchr( value, '0' ) || !strchr( value, '1' ) || !strchr( value, '2' );
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
	std::vector<Entity*>* m_entities;
	int m_trackLength;
public:
	CheckpointCollector( std::vector<Checkpoint>& checkpoints, std::vector<Entity*>* entities, int trackLength )
		: m_checkpoints( checkpoints ), m_entities( entities ), m_trackLength( trackLength ) {}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		Entity* entity = Node_getEntity( path.top() );
		if ( entity && m_entities &&
			std::find( m_entities->begin(), m_entities->end(), entity ) == m_entities->end() ) {
			m_entities->push_back( entity );
		}
		Checkpoint checkpoint;
		if ( readCheckpoint( entity, checkpoint ) && entityInTrackLength( entity, m_trackLength ) ) {
			checkpoint.path = path;
			const AABB& bounds = instance.worldAABB();
			checkpoint.hasBounds = aabb_valid( bounds );
			if ( checkpoint.hasBounds ) {
				checkpoint.boundsCenter = { bounds.origin.x(), bounds.origin.y(), bounds.origin.z() };
				checkpoint.boundsExtents = { bounds.extents.x(), bounds.extents.y(), bounds.extents.z() };
			}
			m_checkpoints.push_back( checkpoint );
		}
		return true;
	}
};

/* Checkpoints of one track length (-1: all checkpoints of the map). */
static std::vector<Checkpoint> collectCheckpointsForLength( int trackLength, std::vector<Entity*>* entities = nullptr ) {
	std::vector<Checkpoint> checkpoints;
	CheckpointCollector collector( checkpoints, entities, trackLength );
	GlobalSceneGraph().traverse( collector );
	std::sort( checkpoints.begin(), checkpoints.end(), []( const Checkpoint& a, const Checkpoint& b ) {
		return a.number < b.number;
	} );
	return checkpoints;
}

/* Checkpoints of the track length the plugin is working on. */
static std::vector<Checkpoint> collectCheckpoints( std::vector<Entity*>* entities = nullptr ) {
	return collectCheckpointsForLength( g_trackLength, entities );
}

struct MapTrackInfo {
	bool allowed[k_trackLengthCount] = { true, true, true };
	bool hasVariants = false;	// a rally_checkpoint exists only for some lengths
};

static MapTrackInfo collectMapTrackInfo() {
	static const char* const allowKeys[k_trackLengthCount] = { "allowShortTrack", "allowMediumTrack", "allowLongTrack" };
	MapTrackInfo info;
	std::vector<Entity*> entities;
	const std::vector<Checkpoint> checkpoints = collectCheckpointsForLength( -1, &entities );
	for ( Entity* entity : entities ) {
		if ( !entity || !string_equal( entity->getClassName(), "worldspawn" ) ) continue;
		for ( int i = 0; i < k_trackLengthCount; ++i ) {
			if ( entity->hasKeyValue( allowKeys[i] ) ) info.allowed[i] = atoi( entity->getKeyValue( allowKeys[i] ) ) != 0;
		}
	}
	for ( const Checkpoint& checkpoint : checkpoints ) {
		if ( isLengthRestricted( checkpoint.entity ) ) info.hasVariants = true;
	}
	return info;
}

static int firstAllowedTrackLength( const MapTrackInfo& info ) {
	for ( int i = 0; i < k_trackLengthCount; ++i ) {
		if ( info.allowed[i] && !collectCheckpointsForLength( i ).empty() ) return i;
	}
	for ( int i = 0; i < k_trackLengthCount; ++i ) {
		if ( info.allowed[i] ) return i;
	}
	return 0;
}

static QString trackLengthLabel( int trackLength ) {
	return QString( "%1 track" ).arg( k_trackLengthNames[trackLength] );
}

static bool readSprintEndpoint( const std::vector<Entity*>& entities, const char* className, Vec3& position ) {
	Entity* gate = nullptr;
	for ( Entity* entity : entities ) {
		if ( entity && string_equal( entity->getClassName(), className ) && entityInTrackLength( entity, g_trackLength ) ) {
			gate = entity;
			break;
		}
	}
	if ( !gate ) return false;

	const char* target = gate->getKeyValue( "target" );
	if ( target && target[0] ) {
		for ( Entity* entity : entities ) {
			const char* targetName = entity ? entity->getKeyValue( "targetname" ) : nullptr;
			if ( targetName && string_equal( targetName, target ) &&
				parseVec3( entity->getKeyValue( "origin" ), position ) ) {
				return true;
			}
		}
	}
	return parseVec3( gate->getKeyValue( "origin" ), position );
}

static SprintEndpoints collectSprintEndpoints( const std::vector<Entity*>& entities ) {
	SprintEndpoints endpoints;
	endpoints.valid = readSprintEndpoint( entities, "rally_start", endpoints.start ) &&
		readSprintEndpoint( entities, "rally_finish", endpoints.finish );
	return endpoints;
}

static SprintEndpoints collectSprintEndpoints() {
	std::vector<Entity*> entities;
	collectCheckpoints( &entities );
	return collectSprintEndpoints( entities );
}

static void bezierPoint( const Checkpoint& start, const Checkpoint& end, float t, Vec3& point ) {
	const Vec3 p1 = add( start.anchor, start.handle );
	const Vec3 p2 = subtract( end.anchor, end.handle );
	const float oneMinusT = 1.f - t;
	point = add( add( scale( start.anchor, oneMinusT * oneMinusT * oneMinusT ),
		scale( p1, 3.f * t * oneMinusT * oneMinusT ) ),
		add( scale( p2, 3.f * t * t * oneMinusT ), scale( end.anchor, t * t * t ) ) );
}

static void renderRouteSegment( const Checkpoint& start, const Checkpoint& end ) {
	if ( !start.hasAnchor || !end.hasAnchor ) return;
	gl().glBegin( GL_LINE_STRIP );
	for ( int sample = 0; sample <= 32; ++sample ) {
		Vec3 point;
		bezierPoint( start, end, sample / 32.f, point );
		gl().glVertex3f( point.x, point.y, point.z );
	}
	gl().glEnd();
}

static void renderRoute() {
	std::vector<Entity*> entities;
	const std::vector<Checkpoint> checkpoints = collectCheckpoints( &entities );
	if ( checkpoints.empty() ) return;
	const SprintEndpoints endpoints = collectSprintEndpoints( entities );
	if ( checkpoints.size() < 2 && !endpoints.valid ) return;

	gl().glLineWidth( 3.f );
	gl().glColor3f( 0.1f, 0.95f, 0.55f );
	if ( endpoints.valid ) {
		Checkpoint start;
		start.anchor = endpoints.start;
		start.handle = scale( subtract( checkpoints.front().anchor, endpoints.start ), 1.f / 3.f );
		start.hasAnchor = true;
		Checkpoint finish;
		finish.anchor = endpoints.finish;
		finish.handle = scale( subtract( endpoints.finish, checkpoints.back().anchor ), 1.f / 3.f );
		finish.hasAnchor = true;
		renderRouteSegment( start, checkpoints.front() );
		for ( std::size_t i = 0; i + 1 < checkpoints.size(); ++i )
			renderRouteSegment( checkpoints[i], checkpoints[i + 1] );
		renderRouteSegment( checkpoints.back(), finish );
	} else {
		for ( std::size_t i = 0; i < checkpoints.size(); ++i )
			renderRouteSegment( checkpoints[i], checkpoints[( i + 1 ) % checkpoints.size()] );
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
	if ( endpoints.valid ) {
		gl().glColor3f( 1.f, 0.25f, 0.25f );
		gl().glVertex3f( endpoints.start.x, endpoints.start.y, endpoints.start.z );
		gl().glColor3f( 0.25f, 0.65f, 1.f );
		gl().glVertex3f( endpoints.finish.x, endpoints.finish.y, endpoints.finish.z );
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
	mutable std::vector<Entity*> entities;
	void visit( scene::Instance& instance ) const override {
		scene::Path path( instance.path() );
		while ( !path.empty() ) {
			Entity* candidate = Node_getEntity( path.top() );
			if ( candidate ) {
				if ( std::find( entities.begin(), entities.end(), candidate ) == entities.end() ) {
					entities.push_back( candidate );
				}
				return;
			}
			path.pop();
		}
	}
};

static Entity* selectedCheckpoint() {
	SelectedCheckpointVisitor visitor;
	if ( GlobalSelectionSystem().Mode() == SelectionSystem::eComponent )
		GlobalSelectionSystem().foreachSelectedComponent( visitor );
	else
		GlobalSelectionSystem().foreachSelected( visitor );
	if ( visitor.entities.size() != 1 ) return nullptr;
	Checkpoint checkpoint;
	return readCheckpoint( visitor.entities.front(), checkpoint ) ? visitor.entities.front() : nullptr;
}

static bool findCheckpoint( Entity* entity, Checkpoint& checkpoint ) {
	if ( !entity ) return false;
	for ( const Checkpoint& candidate : collectCheckpoints() ) {
		if ( candidate.entity == entity ) {
			checkpoint = candidate;
			return true;
		}
	}
	return false;
}

static float wrapDegrees( float value ) {
	while ( value > 180.f ) value -= 360.f;
	while ( value < -180.f ) value += 360.f;
	return value;
}

static bool checkpointTangentYaw( const std::vector<Checkpoint>& checkpoints, Entity* entity, float& yaw ) {
	if ( checkpoints.empty() ) return false;
	std::size_t index = checkpoints.size();
	for ( std::size_t i = 0; i < checkpoints.size(); ++i ) {
		if ( checkpoints[i].entity == entity ) { index = i; break; }
	}
	if ( index == checkpoints.size() ) return false;
	if ( !checkpoints[index].hasAnchor ) return false;
	const SprintEndpoints endpoints = collectSprintEndpoints();
	Vec3 tangent;
	if ( endpoints.valid ) {
		if ( checkpoints.size() == 1 ) {
			tangent = subtract( endpoints.finish, endpoints.start );
		} else if ( index == 0 ) {
			if ( !checkpoints[1].hasAnchor ) return false;
			tangent = subtract( checkpoints[1].anchor, endpoints.start );
		} else if ( index + 1 == checkpoints.size() ) {
			if ( !checkpoints[index - 1].hasAnchor ) return false;
			tangent = subtract( endpoints.finish, checkpoints[index - 1].anchor );
		} else {
			if ( !checkpoints[index - 1].hasAnchor || !checkpoints[index + 1].hasAnchor ) return false;
			tangent = subtract( checkpoints[index + 1].anchor, checkpoints[index - 1].anchor );
		}
	} else {
		if ( checkpoints.size() < 2 ) return false;
		const std::size_t previous = ( index + checkpoints.size() - 1 ) % checkpoints.size();
		const std::size_t next = ( index + 1 ) % checkpoints.size();
		if ( !checkpoints[previous].hasAnchor || !checkpoints[next].hasAnchor ) return false;
		tangent = subtract( checkpoints[next].anchor, checkpoints[previous].anchor );
	}
	float length = sqrtf( tangent.x * tangent.x + tangent.y * tangent.y );
	if ( length < 0.001f ) {
		if ( !checkpoints[index].hasAnchor ) return false;
		if ( endpoints.valid && index == 0 && checkpoints.size() > 1 )
			tangent = subtract( checkpoints[1].anchor, checkpoints[index].anchor );
		else if ( endpoints.valid && index + 1 == checkpoints.size() && checkpoints.size() > 1 )
			tangent = subtract( endpoints.finish, checkpoints[index].anchor );
		else if ( endpoints.valid )
			tangent = subtract( endpoints.finish, endpoints.start );
		else if ( !endpoints.valid ) {
			const std::size_t next = ( index + 1 ) % checkpoints.size();
			if ( !checkpoints[next].hasAnchor ) return false;
			tangent = subtract( checkpoints[next].anchor, checkpoints[index].anchor );
		}
		length = sqrtf( tangent.x * tangent.x + tangent.y * tangent.y );
	}
	if ( length < 0.001f ) return false;
	yaw = atan2f( tangent.y, tangent.x ) * 57.2957795131f;
	return true;
}

static bool switchTrackLength( int trackLength );

class RouteEditorDialog : public QDialog {
	QComboBox* m_trackLengthCombo = nullptr;
	QLabel* m_selectionLabel = nullptr;
	QLabel* m_tangentLabel = nullptr;
	QPushButton* m_previousButton = nullptr;
	QPushButton* m_nextButton = nullptr;
	QDoubleSpinBox* m_position[3] = {};
	QSlider* m_bendSlider = nullptr;
	QDoubleSpinBox* m_bend = nullptr;
	QSlider* m_lengthSlider = nullptr;
	QDoubleSpinBox* m_length = nullptr;
	QSlider* m_tiltSlider = nullptr;
	QDoubleSpinBox* m_tilt = nullptr;
	float m_baseYaw = 0.f;
	bool m_hasTangent = false;
	bool m_loading = false;
	bool m_curveDirty = false;
	Entity* m_boundEntity = nullptr;
	Entity* m_lastSelection = nullptr;
	std::unique_ptr<UndoableCommand> m_editUndo;
public:
	explicit RouteEditorDialog( QWidget* parent ) : QDialog( parent, Qt::Tool ) {
		setWindowTitle( "RallyRoute — Bezier point and curve" );
		setMinimumWidth( 440 );
		auto* layout = new QVBoxLayout( this );
		auto* trackLengthRow = new QHBoxLayout;
		trackLengthRow->addWidget( new QLabel( "Track length" ) );
		m_trackLengthCombo = new QComboBox;
		for ( int i = 0; i < k_trackLengthCount; ++i ) m_trackLengthCombo->addItem( k_trackLengthNames[i] );
		m_trackLengthCombo->setToolTip( "Checkpoints, preview, import and export follow this track length (onlyTrackLength)." );
		trackLengthRow->addWidget( m_trackLengthCombo, 1 );
		layout->addLayout( trackLengthRow );
		m_selectionLabel = new QLabel( "Select one rally_checkpoint in the map." );
		layout->addWidget( m_selectionLabel );
		auto* navigation = new QHBoxLayout;
		m_previousButton = new QPushButton( "Previous checkpoint" );
		m_nextButton = new QPushButton( "Next checkpoint" );
		m_previousButton->setEnabled( false );
		m_nextButton->setEnabled( false );
		navigation->addWidget( m_previousButton );
		navigation->addWidget( m_nextButton );
		layout->addLayout( navigation );

		auto* pointBox = new QGroupBox( "Bezier point position" );
		auto* pointForm = new QFormLayout( pointBox );
		for ( int i = 0; i < 3; ++i ) {
			m_position[i] = new QDoubleSpinBox;
			m_position[i]->setRange( -131072.0, 131072.0 );
			m_position[i]->setDecimals( 2 );
			m_position[i]->setSingleStep( 8.0 );
			pointForm->addRow( i == 2 ? "Height Z" : ( i == 0 ? "Position X" : "Position Y" ), m_position[i] );
		}
		layout->addWidget( pointBox );

		auto* curveBox = new QGroupBox( "Curve shape" );
		auto* curveForm = new QFormLayout( curveBox );
		{
			auto* row = new QWidget;
			auto* rowLayout = new QHBoxLayout( row );
			rowLayout->setContentsMargins( 0, 0, 0, 0 );
			m_bendSlider = new QSlider( Qt::Horizontal );
			m_bendSlider->setRange( -180, 180 );
			m_bendSlider->setSingleStep( 1 );
			m_bendSlider->setPageStep( 10 );
			m_bend = new QDoubleSpinBox;
			m_bend->setRange( -180.0, 180.0 );
			m_bend->setDecimals( 1 );
			m_bend->setSingleStep( 1.0 );
			m_bend->setSuffix( "°" );
			m_bendSlider->setToolTip( "Rotate the helper around the local direction from the previous to the next checkpoint." );
			m_bend->setToolTip( m_bendSlider->toolTip() );
			rowLayout->addWidget( m_bendSlider, 1 );
			rowLayout->addWidget( m_bend );
			curveForm->addRow( "Curve bend", row );
		}
		{
			auto* row = new QWidget;
			auto* rowLayout = new QHBoxLayout( row );
			rowLayout->setContentsMargins( 0, 0, 0, 0 );
			m_lengthSlider = new QSlider( Qt::Horizontal );
			m_lengthSlider->setRange( 0, 262144 );
			m_lengthSlider->setSingleStep( 8 );
			m_lengthSlider->setPageStep( 64 );
			m_length = new QDoubleSpinBox;
			m_length->setRange( 0.0, 262144.0 );
			m_length->setDecimals( 1 );
			m_length->setSingleStep( 8.0 );
			m_length->setSuffix( " units" );
			m_lengthSlider->setToolTip( "Set the magnitude of the Bezier helper vector." );
			m_length->setToolTip( m_lengthSlider->toolTip() );
			rowLayout->addWidget( m_lengthSlider, 1 );
			rowLayout->addWidget( m_length );
			curveForm->addRow( "Helper length", row );
		}
		{
			auto* row = new QWidget;
			auto* rowLayout = new QHBoxLayout( row );
			rowLayout->setContentsMargins( 0, 0, 0, 0 );
			m_tiltSlider = new QSlider( Qt::Horizontal );
			m_tiltSlider->setRange( -90, 90 );
			m_tiltSlider->setSingleStep( 1 );
			m_tiltSlider->setPageStep( 5 );
			m_tilt = new QDoubleSpinBox;
			m_tilt->setRange( -90.0, 90.0 );
			m_tilt->setDecimals( 1 );
			m_tilt->setSingleStep( 1.0 );
			m_tilt->setSuffix( "°" );
			m_tiltSlider->setToolTip( "Tilt the helper above or below the map plane." );
			m_tilt->setToolTip( m_tiltSlider->toolTip() );
			rowLayout->addWidget( m_tiltSlider, 1 );
			rowLayout->addWidget( m_tilt );
			curveForm->addRow( "Helper tilt", row );
		}
		m_tangentLabel = new QLabel( "Bend is relative to the local route tangent." );
		curveForm->addRow( m_tangentLabel );
		layout->addWidget( curveBox );
		layout->addWidget( new QLabel( "Changes update the map route immediately and are undoable. `origin` remains the respawn point." ) );

		for ( auto* position : m_position ) {
			connect( position, QOverload<double>::of( &QDoubleSpinBox::valueChanged ), this, [this]( double ) {
				if ( m_loading ) return;
				beginSpinEdit();
				storeCurrent();
			} );
			connect( position, &QDoubleSpinBox::editingFinished, this, [this]() { endSpinEdit(); } );
		}
		connect( m_bendSlider, &QSlider::sliderPressed, this, [this]() { beginSliderEdit(); } );
		connect( m_bendSlider, &QSlider::sliderReleased, this, [this]() { endSliderEdit(); } );
		connect( m_bendSlider, &QSlider::valueChanged, this, [this]( int value ) {
			if ( m_loading ) return;
			QSignalBlocker blocker( m_bend );
			m_bend->setValue( value );
			m_curveDirty = true;
			storeCurrent();
		} );
		connect( m_bend, QOverload<double>::of( &QDoubleSpinBox::valueChanged ), this, [this]( double value ) {
			if ( m_loading ) return;
			QSignalBlocker blocker( m_bendSlider );
			m_bendSlider->setValue( qRound( value ) );
			m_curveDirty = true;
			beginSpinEdit();
			storeCurrent();
		} );
		connect( m_bend, &QDoubleSpinBox::editingFinished, this, [this]() { endSpinEdit(); } );

		connect( m_lengthSlider, &QSlider::sliderPressed, this, [this]() { beginSliderEdit(); } );
		connect( m_lengthSlider, &QSlider::sliderReleased, this, [this]() { endSliderEdit(); } );
		connect( m_lengthSlider, &QSlider::valueChanged, this, [this]( int value ) {
			if ( m_loading ) return;
			QSignalBlocker blocker( m_length );
			m_length->setValue( value );
			m_curveDirty = true;
			storeCurrent();
		} );
		connect( m_length, QOverload<double>::of( &QDoubleSpinBox::valueChanged ), this, [this]( double value ) {
			if ( m_loading ) return;
			QSignalBlocker blocker( m_lengthSlider );
			m_lengthSlider->setValue( qRound( value ) );
			m_curveDirty = true;
			beginSpinEdit();
			storeCurrent();
		} );
		connect( m_length, &QDoubleSpinBox::editingFinished, this, [this]() { endSpinEdit(); } );

		connect( m_tiltSlider, &QSlider::sliderPressed, this, [this]() { beginSliderEdit(); } );
		connect( m_tiltSlider, &QSlider::sliderReleased, this, [this]() { endSliderEdit(); } );
		connect( m_tiltSlider, &QSlider::valueChanged, this, [this]( int value ) {
			if ( m_loading ) return;
			QSignalBlocker blocker( m_tilt );
			m_tilt->setValue( value );
			m_curveDirty = true;
			storeCurrent();
		} );
		connect( m_tilt, QOverload<double>::of( &QDoubleSpinBox::valueChanged ), this, [this]( double value ) {
			if ( m_loading ) return;
			QSignalBlocker blocker( m_tiltSlider );
			m_tiltSlider->setValue( qRound( value ) );
			m_curveDirty = true;
			beginSpinEdit();
			storeCurrent();
		} );
		connect( m_tilt, &QDoubleSpinBox::editingFinished, this, [this]() { endSpinEdit(); } );
		connect( m_trackLengthCombo, QOverload<int>::of( &QComboBox::currentIndexChanged ), this, [this]( int index ) {
			if ( m_loading ) return;
			if ( !switchTrackLength( index ) ) refreshTrackLength();
		} );
		connect( m_previousButton, &QPushButton::clicked, this, [this]() { navigateCheckpoint( -1 ); } );
		connect( m_nextButton, &QPushButton::clicked, this, [this]() { navigateCheckpoint( 1 ); } );
		refreshTrackLength();
		auto* selectionTimer = new QTimer( this );
		selectionTimer->setInterval( 100 );
		connect( selectionTimer, &QTimer::timeout, this, [this]() {
			if ( isVisible() && selectedCheckpoint() != m_lastSelection ) updateFromSelection();
		} );
		selectionTimer->start();
	}

	/* Shows the plugin's current track length and reloads the selected checkpoint. */
	void refreshTrackLength() {
		const MapTrackInfo info = collectMapTrackInfo();
		{
			QSignalBlocker blocker( m_trackLengthCombo );
			for ( int i = 0; i < k_trackLengthCount; ++i ) {
				m_trackLengthCombo->setItemText( i, info.allowed[i] ? QString( k_trackLengthNames[i] )
					: QString( "%1 (not enabled in worldspawn)" ).arg( k_trackLengthNames[i] ) );
			}
			m_trackLengthCombo->setCurrentIndex( g_trackLength );
		}
		updateFromSelection();
	}

	void updateFromSelection() {
		m_editUndo.reset();
		m_loading = true;
		m_curveDirty = false;
		m_boundEntity = selectedCheckpoint();
		m_lastSelection = m_boundEntity;
		Checkpoint checkpoint;
		Entity* entity = findCheckpoint( m_boundEntity, checkpoint ) ? m_boundEntity : nullptr;
		if ( !entity ) {
			m_selectionLabel->setText( m_boundEntity
				? QString( "This checkpoint is not part of the %1." ).arg( trackLengthLabel( g_trackLength ) )
				: QString( "Select exactly one rally_checkpoint in the map." ) );
			m_boundEntity = nullptr;
			for ( auto* value : m_position ) value->setEnabled( false );
			m_previousButton->setEnabled( false );
			m_nextButton->setEnabled( false );
			setCurveControlsEnabled( false );
			m_loading = false;
			return;
		}
		m_selectionLabel->setText( QString( "Checkpoint %1" ).arg( checkpoint.number ) );
		for ( auto* value : m_position ) value->setEnabled( true );
		const std::vector<Checkpoint> checkpoints = collectCheckpoints();
		m_previousButton->setEnabled( checkpoints.size() > 1 );
		m_nextButton->setEnabled( checkpoints.size() > 1 );
		m_position[0]->setValue( checkpoint.anchor.x );
		m_position[1]->setValue( checkpoint.anchor.y );
		m_position[2]->setValue( checkpoint.anchor.z );
		m_hasTangent = checkpointTangentYaw( checkpoints, entity, m_baseYaw );
		const Vec3 handle = checkpoint.handle;
		const float horizontalLength = sqrtf( handle.x * handle.x + handle.y * handle.y );
		const float handleLength = sqrtf( horizontalLength * horizontalLength + handle.z * handle.z );
		const int lengthRange = static_cast<int>( std::min( 262144.f, std::max( 4096.f, ceilf( handleLength * 2.f / 256.f ) * 256.f ) ) );
		m_lengthSlider->setRange( 0, lengthRange );
		m_length->setRange( 0.0, lengthRange );
		float bend = 0.f;
		if ( m_hasTangent && horizontalLength > 0.001f )
			bend = wrapDegrees( atan2f( handle.y, handle.x ) * 57.2957795131f - m_baseYaw );
		const float tilt = handleLength > 0.001f ? asinf( std::max( -1.f, std::min( 1.f, handle.z / handleLength ) ) ) * 57.2957795131f : 0.f;
		m_bend->setValue( bend );
		m_bendSlider->setValue( qRound( bend ) );
		m_length->setValue( handleLength );
		m_lengthSlider->setValue( qRound( handleLength ) );
		m_tilt->setValue( tilt );
		m_tiltSlider->setValue( qRound( tilt ) );
		m_tangentLabel->setText( m_hasTangent
			? QString( "Local tangent: %1° — bend rotates the helper around this direction." ).arg( m_baseYaw, 0, 'f', 1 )
			: "Curve controls need valid anchors on the previous and next checkpoints." );
		setCurveControlsEnabled( m_hasTangent );
		m_loading = false;
	}

private:
	void navigateCheckpoint( int direction ) {
		const std::vector<Checkpoint> checkpoints = collectCheckpoints();
		if ( checkpoints.size() < 2 ) return;
		Entity* current = selectedCheckpoint();
		std::size_t index = checkpoints.size();
		for ( std::size_t i = 0; i < checkpoints.size(); ++i ) {
			if ( checkpoints[i].entity == current ) { index = i; break; }
		}
		if ( index == checkpoints.size() ) return;
		const std::size_t next = ( index + checkpoints.size() + ( direction < 0 ? checkpoints.size() - 1 : 1 ) ) % checkpoints.size();
		scene::Instance* target = GlobalSceneGraph().find( checkpoints[next].path );
		if ( !target ) return;
		SelectionSystem& selection = GlobalSelectionSystem();
		selection.setSelectedAllComponents( false );
		if ( selection.Mode() != SelectionSystem::ePrimitive ) selection.SetMode( SelectionSystem::ePrimitive );
		selection.setSelectedAll( false );
		Entity_setSelected( *target, true );
	}

	void setCurveControlsEnabled( bool enabled ) {
		m_bendSlider->setEnabled( enabled );
		m_bend->setEnabled( enabled );
		m_lengthSlider->setEnabled( enabled );
		m_length->setEnabled( enabled );
		m_tiltSlider->setEnabled( enabled );
		m_tilt->setEnabled( enabled );
	}

	void beginSpinEdit() {
		if ( !m_editUndo ) m_editUndo = std::make_unique<UndoableCommand>( "RallyRoute.editCheckpointBezier" );
	}

	void endSpinEdit() {
		m_editUndo.reset();
	}

	void beginSliderEdit() {
		if ( !m_editUndo ) m_editUndo = std::make_unique<UndoableCommand>( "RallyRoute.adjustBezierCurve" );
	}

	void endSliderEdit() {
		m_editUndo.reset();
	}

	void storeCurrent() {
		if ( m_loading ) return;
		Entity* entity = m_boundEntity;
		Checkpoint checkpoint;
		if ( !entity || selectedCheckpoint() != entity || !findCheckpoint( entity, checkpoint ) ) return;
		const Vec3 anchor = { (float)m_position[0]->value(), (float)m_position[1]->value(), (float)m_position[2]->value() };
		Vec3 handle = checkpoint.handle;
		if ( m_hasTangent && m_curveDirty ) {
			const float length = (float)m_length->value();
			const float yaw = ( m_baseYaw + (float)m_bend->value() ) * 0.0174532925199f;
			const float tilt = (float)m_tilt->value() * 0.0174532925199f;
			const float horizontal = length * cosf( tilt );
			handle = { horizontal * cosf( yaw ), horizontal * sinf( yaw ), length * sinf( tilt ) };
		}
		const QByteArray anchorText = QString( "%1 %2 %3" ).arg( anchor.x, 0, 'f', 3 )
			.arg( anchor.y, 0, 'f', 3 ).arg( anchor.z, 0, 'f', 3 ).toLocal8Bit();
		const QByteArray handleText = QString( "%1 %2 %3" ).arg( handle.x, 0, 'f', 3 )
			.arg( handle.y, 0, 'f', 3 ).arg( handle.z, 0, 'f', 3 ).toLocal8Bit();
		auto write = [&]() {
			entity->setKeyValue( "bezierPos", anchorText.constData() );
			entity->setKeyValue( "bezierDir", handleText.constData() );
		};
		if ( m_editUndo ) write();
		else {
			UndoableCommand undo( "RallyRoute.setCheckpointBezier" );
			write();
		}
		m_curveDirty = false;
		SceneChangeNotify();
	}
};

static RouteEditorDialog* g_editor = nullptr;

static void refreshEditor() {
	if ( g_editor ) g_editor->refreshTrackLength();
}

static void updateEditorFromSelection( const Selectable& ) {
	if ( g_editor && g_editor->isVisible() ) g_editor->updateFromSelection();
}

static QString bpdDirectory() {
	const QString directory = QDir( QDir::homePath() ).filePath( "AppData/Roaming/Q3Rally/baseq3r/maps" );
	return QDir( directory ).exists() ? directory : QDir::homePath();
}

static QString gameMapsDirectory() {
	return QDir( QDir::homePath() ).filePath( "AppData/Roaming/Q3Rally/baseq3r/maps" );
}

static void showMessage( const QString& text, EMessageBoxType type, const char* caption = "RallyRoute" ) {
	GlobalRadiant().m_pfnMessageBox( g_mainWindow, text.toLocal8Bit().constData(), caption, type, 0 );
}

static QString currentMapPath() {
	const char* mapNameText = GlobalRadiant().getMapName ? GlobalRadiant().getMapName() : nullptr;
	return mapNameText ? QString::fromLocal8Bit( mapNameText ) : QString();
}

static QString currentMapBaseName() {
	const QString baseName = QFileInfo( currentMapPath() ).completeBaseName();
	if ( baseName.isEmpty() || baseName.compare( "unnamed", Qt::CaseInsensitive ) == 0 ) return QString();
	return baseName;
}

/* The reference BPD files live next to the .map, in the maps folder that ships with the game. */
static QString bpdFolder() {
	const QFileInfo mapFile( currentMapPath() );
	if ( !currentMapBaseName().isEmpty() && mapFile.isAbsolute() && mapFile.absoluteDir().exists() )
		return mapFile.absolutePath();
	return QString();
}

static bool lengthFilesExist( const QString& folder, const QString& baseName ) {
	for ( int i = 0; i < k_trackLengthCount; ++i ) {
		if ( QFileInfo::exists( QDir( folder ).filePath( QString( "%1_tl%2.bpd" ).arg( baseName ).arg( i ) ) ) ) return true;
	}
	return false;
}

/* Same file names the game loads: <map>_tl<n>.bpd on maps with length variants,
   otherwise <map>.bpd (unless the map already uses per-length files). */
static QString trackLengthBpdPath( const MapTrackInfo& info, int trackLength ) {
	const QString folder = bpdFolder();
	const QString baseName = currentMapBaseName();
	if ( folder.isEmpty() || baseName.isEmpty() ) return QString();
	if ( info.hasVariants || lengthFilesExist( folder, baseName ) )
		return QDir( folder ).filePath( QString( "%1_tl%2.bpd" ).arg( baseName ).arg( trackLength ) );
	return QDir( folder ).filePath( baseName + ".bpd" );
}

struct BpdRecord {
	int number = 0;
	Vec3 anchor;
	Vec3 handle;
};

static bool readBpdFile( const QString& path, std::vector<BpdRecord>& records, QString& error ) {
	records.clear();
	QFile file( path );
	if ( !file.open( QIODevice::ReadOnly | QIODevice::Text ) ) {
		error = QString( "Could not open %1." ).arg( QDir::toNativeSeparators( path ) );
		return false;
	}
	QTextStream stream( &file );
	int lineNumber = 0;
	while ( !stream.atEnd() ) {
		const QByteArray line = stream.readLine().toLocal8Bit();
		++lineNumber;
		const char* p = line.constData();
		while ( *p == ' ' || *p == '\t' ) ++p;
		if ( !*p || *p == '#' || *p == '/' ) continue;
		BpdRecord record;
		char tail;
		if ( sscanf( p, " %d : %f %f %f : %f %f %f %c", &record.number,
			&record.anchor.x, &record.anchor.y, &record.anchor.z,
			&record.handle.x, &record.handle.y, &record.handle.z, &tail ) < 7 ||
			!std::isfinite( record.anchor.x ) || !std::isfinite( record.anchor.y ) || !std::isfinite( record.anchor.z ) ||
			!std::isfinite( record.handle.x ) || !std::isfinite( record.handle.y ) || !std::isfinite( record.handle.z ) ) {
			error = QString( "Invalid BPD record on line %1." ).arg( lineNumber );
			return false;
		}
		if ( std::any_of( records.begin(), records.end(), [&record]( const BpdRecord& other ) { return other.number == record.number; } ) ) {
			error = QString( "BPD checkpoint %1 occurs more than once." ).arg( record.number );
			return false;
		}
		records.push_back( record );
	}
	if ( records.empty() ) {
		error = "The BPD file contains no checkpoint records.";
		return false;
	}
	return true;
}

/* Checkpoint numbers of one track length must run 1..N without gaps or duplicates. */
static bool checkCheckpointNumbers( const std::vector<Checkpoint>& checkpoints, QString& problem ) {
	QStringList duplicates, missing;
	std::map<int, int> counts;
	int highest = 0;
	for ( const Checkpoint& checkpoint : checkpoints ) {
		++counts[checkpoint.number];
		highest = std::max( highest, checkpoint.number );
	}
	for ( const auto& count : counts ) {
		if ( count.second > 1 ) duplicates << QString::number( count.first );
	}
	for ( int number = 1; number <= highest; ++number ) {
		if ( counts.find( number ) == counts.end() ) missing << QString::number( number );
	}
	if ( duplicates.isEmpty() && missing.isEmpty() && counts.find( 0 ) == counts.end() ) return true;
	QStringList parts;
	if ( !duplicates.isEmpty() ) parts << QString( "duplicate %1" ).arg( duplicates.join( ", " ) );
	if ( !missing.isEmpty() ) parts << QString( "missing %1" ).arg( missing.join( ", " ) );
	if ( counts.find( 0 ) != counts.end() ) parts << "number 0 is invalid";
	problem = parts.join( "; " );
	return false;
}

static bool checkExportable( const std::vector<Checkpoint>& checkpoints, QString& error ) {
	if ( checkpoints.empty() ) {
		error = QString( "No rally_checkpoint entities on the %1." ).arg( trackLengthLabel( g_trackLength ) );
		return false;
	}
	QString problem;
	if ( !checkCheckpointNumbers( checkpoints, problem ) ) {
		error = QString( "Checkpoint numbers on the %1 are not 1..N (%2). BPD export stopped." )
			.arg( trackLengthLabel( g_trackLength ), problem );
		return false;
	}
	for ( const Checkpoint& checkpoint : checkpoints ) {
		if ( !checkpoint.hasAnchor ) {
			error = QString( "Checkpoint %1 has no valid bezierPos. BPD export stopped." ).arg( checkpoint.number );
			return false;
		}
	}
	return true;
}

static bool writeBpdFile( const QString& path, const std::vector<Checkpoint>& checkpoints, QString& error ) {
	QFile file( path );
	if ( !file.open( QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text ) ) {
		error = QString( "Could not open %1 for writing." ).arg( QDir::toNativeSeparators( path ) );
		return false;
	}
	QTextStream stream( &file );
	stream.setLocale( QLocale::c() );
	stream.setRealNumberNotation( QTextStream::FixedNotation );
	stream.setRealNumberPrecision( 3 );
	for ( const Checkpoint& checkpoint : checkpoints ) {
		const Vec3 handle = checkpoint.hasHandle ? checkpoint.handle : Vec3{};
		stream << checkpoint.number << " : "
			<< checkpoint.anchor.x << " " << checkpoint.anchor.y << " " << checkpoint.anchor.z << " : "
			<< handle.x << " " << handle.y << " " << handle.z << "\n";
	}
	stream.flush();
	if ( stream.status() != QTextStream::Ok ) {
		error = QString( "Could not write all checkpoint records to %1." ).arg( QDir::toNativeSeparators( path ) );
		return false;
	}
	file.close();
	return true;
}

static bool sameVector( const Vec3& a, const Vec3& b ) {
	return std::fabs( a.x - b.x ) < 0.01f && std::fabs( a.y - b.y ) < 0.01f && std::fabs( a.z - b.z ) < 0.01f;
}

static bool recordsMatchCheckpoints( const std::vector<Checkpoint>& checkpoints, const std::vector<BpdRecord>& records ) {
	if ( checkpoints.size() != records.size() ) return false;
	for ( const Checkpoint& checkpoint : checkpoints ) {
		const auto record = std::find_if( records.begin(), records.end(), [&checkpoint]( const BpdRecord& candidate ) {
			return candidate.number == checkpoint.number;
		} );
		if ( record == records.end() || !checkpoint.hasAnchor ) return false;
		if ( !sameVector( checkpoint.anchor, record->anchor ) ) return false;
		if ( !sameVector( checkpoint.hasHandle ? checkpoint.handle : Vec3{}, record->handle ) ) return false;
	}
	return true;
}

/* Writes the records into the checkpoints of the current track length as one undo step. */
static bool applyBpdRecords( const std::vector<Checkpoint>& checkpoints, const std::vector<BpdRecord>& records,
	const char* undoName, QString& error ) {
	std::map<int, Entity*> entities;
	for ( const Checkpoint& checkpoint : checkpoints ) entities[checkpoint.number] = checkpoint.entity;
	for ( const BpdRecord& record : records ) {
		if ( entities.find( record.number ) == entities.end() ) {
			error = QString( "BPD checkpoint %1 has no matching rally_checkpoint on the %2." )
				.arg( record.number ).arg( trackLengthLabel( g_trackLength ) );
			return false;
		}
	}
	if ( records.size() != checkpoints.size() ) {
		error = QString( "The BPD file has %1 checkpoints, the %2 has %3. No values were imported." )
			.arg( records.size() ).arg( trackLengthLabel( g_trackLength ) ).arg( checkpoints.size() );
		return false;
	}
	UndoableCommand undo( undoName );
	for ( const BpdRecord& record : records ) {
		Entity* entity = entities[record.number];
		const QByteArray anchor = QString( "%1 %2 %3" ).arg( record.anchor.x, 0, 'f', 3 )
			.arg( record.anchor.y, 0, 'f', 3 ).arg( record.anchor.z, 0, 'f', 3 ).toLocal8Bit();
		const QByteArray handle = QString( "%1 %2 %3" ).arg( record.handle.x, 0, 'f', 3 )
			.arg( record.handle.y, 0, 'f', 3 ).arg( record.handle.z, 0, 'f', 3 ).toLocal8Bit();
		entity->setKeyValue( "bezierPos", anchor.constData() );
		entity->setKeyValue( "bezierDir", handle.constData() );
	}
	SceneChangeNotify();
	return true;
}

enum class BpdSync { NoFile, InSync, Differs, Unreadable };

static BpdSync bpdSyncState( const QString& path, const std::vector<Checkpoint>& checkpoints ) {
	if ( path.isEmpty() || !QFileInfo::exists( path ) ) return BpdSync::NoFile;
	std::vector<BpdRecord> records;
	QString error;
	if ( !readBpdFile( path, records, error ) ) return BpdSync::Unreadable;
	return recordsMatchCheckpoints( checkpoints, records ) ? BpdSync::InSync : BpdSync::Differs;
}

static bool loadBpdIntoCheckpoints( const QString& path, const char* undoName, QString& error ) {
	std::vector<BpdRecord> records;
	return readBpdFile( path, records, error ) && applyBpdRecords( collectCheckpoints(), records, undoName, error );
}

static void showRouteStatus() {
	const MapTrackInfo info = collectMapTrackInfo();
	const std::vector<Checkpoint> checkpoints = collectCheckpoints();
	int anchors = 0, handles = 0;
	for ( const Checkpoint& checkpoint : checkpoints ) {
		if ( checkpoint.hasAnchor ) ++anchors;
		if ( checkpoint.hasHandle ) ++handles;
	}
	QStringList allowed;
	for ( int i = 0; i < k_trackLengthCount; ++i ) {
		if ( info.allowed[i] ) allowed << k_trackLengthNames[i];
	}
	QString numbering = "1..N, ok";
	QString problem;
	if ( checkpoints.empty() ) numbering = "none";
	else if ( !checkCheckpointNumbers( checkpoints, problem ) ) numbering = "PROBLEM: " + problem;

	const QString path = trackLengthBpdPath( info, g_trackLength );
	QString fileState;
	switch ( bpdSyncState( path, checkpoints ) ) {
	case BpdSync::NoFile: fileState = "missing"; break;
	case BpdSync::InSync: fileState = "matches the map"; break;
	case BpdSync::Differs: fileState = "differs from the map (export or import to sync)"; break;
	case BpdSync::Unreadable: fileState = "unreadable"; break;
	}
	QString text = QString( "Track length: %1%2\nEnabled in worldspawn: %3\nCheckpoints differ between lengths: %4\n\n"
		"Checkpoints: %5 (numbers %6)\nBezier anchors: %7/%5\nHandle vectors: %8/%5\n\n" )
		.arg( k_trackLengthNames[g_trackLength] )
		.arg( info.allowed[g_trackLength] ? "" : " (not enabled in worldspawn)" )
		.arg( allowed.isEmpty() ? "none" : allowed.join( ", " ) )
		.arg( info.hasVariants ? "yes, one BPD per length" : "no" )
		.arg( checkpoints.size() ).arg( numbering ).arg( anchors ).arg( handles );
	if ( path.isEmpty() ) {
		text += "BPD file: save the map first; the BPD files belong next to the .map.\n";
	} else {
		text += QString( "BPD file: %1\n  %2\n" ).arg( QDir::toNativeSeparators( path ), fileState );
		const QString homeCopy = QDir( gameMapsDirectory() ).filePath( QFileInfo( path ).fileName() );
		if ( QFileInfo( homeCopy ).absoluteFilePath() != QFileInfo( path ).absoluteFilePath() && QFileInfo::exists( homeCopy ) )
			text += QString( "  Note: %1 exists too and overrides the shipped file in the game.\n" ).arg( QDir::toNativeSeparators( homeCopy ) );
	}
	text += "\nThe route preview uses bezierPos/bezierDir exactly as Q3Rally does. Missing anchors are skipped; missing handles act as zero offsets. Respawn origin is never used as a route point.";
	showMessage( text, EMessageBoxType::Info );
}

static void exportBpd() {
	const MapTrackInfo info = collectMapTrackInfo();
	const std::vector<Checkpoint> checkpoints = collectCheckpoints();
	QString error;
	if ( !checkExportable( checkpoints, error ) ) {
		showMessage( error, EMessageBoxType::Warning );
		return;
	}
	const QString suggested = trackLengthBpdPath( info, g_trackLength );
	const QString path = QFileDialog::getSaveFileName( g_mainWindow,
		QString( "Export Q3Rally Bezier path — %1" ).arg( trackLengthLabel( g_trackLength ) ),
		suggested.isEmpty() ? bpdDirectory() : suggested, "Bezier paths (*.bpd);;All files (*)" );
	if ( path.isEmpty() ) return;
	if ( !writeBpdFile( path, checkpoints, error ) ) showMessage( error, EMessageBoxType::Error );
}

static void importBpd() {
	const MapTrackInfo info = collectMapTrackInfo();
	const QString suggested = trackLengthBpdPath( info, g_trackLength );
	const QString start = !suggested.isEmpty() && QFileInfo::exists( suggested ) ? suggested
		: ( bpdFolder().isEmpty() ? bpdDirectory() : bpdFolder() );
	const QString path = QFileDialog::getOpenFileName( g_mainWindow,
		QString( "Import Q3Rally Bezier path — %1" ).arg( trackLengthLabel( g_trackLength ) ),
		start, "Bezier paths (*.bpd);;All files (*)" );
	if ( path.isEmpty() ) return;
	QString error;
	if ( !loadBpdIntoCheckpoints( path, "RallyRoute.importBpd", error ) ) showMessage( error, EMessageBoxType::Error );
}

static void refreshEditor();

/* Switch the track length the plugin works on. Unsaved Bezier keys of the current length
   can be written to its BPD first; the new length then loads its reference BPD. */
static bool switchTrackLength( int trackLength ) {
	if ( trackLength < 0 || trackLength >= k_trackLengthCount ) return false;
	if ( trackLength == g_trackLength ) {
		refreshEditor();
		return true;
	}
	const MapTrackInfo info = collectMapTrackInfo();
	const QString currentPath = trackLengthBpdPath( info, g_trackLength );
	if ( currentPath == trackLengthBpdPath( info, trackLength ) ) {
		/* One shared <map>.bpd: both lengths use the same file and checkpoints. */
		g_trackLength = trackLength;
		SceneChangeNotify();
		refreshEditor();
		return true;
	}
	const std::vector<Checkpoint> current = collectCheckpoints();
	const BpdSync currentState = bpdSyncState( currentPath, current );
	const bool hasKeys = std::any_of( current.begin(), current.end(), []( const Checkpoint& checkpoint ) { return checkpoint.hasAnchor; } );
	if ( !currentPath.isEmpty() && ( currentState == BpdSync::Differs || ( currentState == BpdSync::NoFile && hasKeys ) ) ) {
		const QString question = QString( "The Bezier route of the %1 is not saved in\n%2\n\nSave it before switching to the %3?" )
			.arg( trackLengthLabel( g_trackLength ), QDir::toNativeSeparators( currentPath ), trackLengthLabel( trackLength ) );
		const EMessageBoxReturn answer = GlobalRadiant().m_pfnMessageBox( g_mainWindow, question.toLocal8Bit().constData(),
			"RallyRoute", EMessageBoxType::Question, eIDYES | eIDNO | eIDCANCEL );
		if ( answer == eIDCANCEL ) return false;
		if ( answer == eIDYES ) {
			QString error;
			if ( !checkExportable( current, error ) || !writeBpdFile( currentPath, current, error ) ) {
				showMessage( error, EMessageBoxType::Error );
				return false;
			}
		}
	}

	g_trackLength = trackLength;
	const QString nextPath = trackLengthBpdPath( info, g_trackLength );
	if ( bpdSyncState( nextPath, collectCheckpoints() ) == BpdSync::Differs ) {
		QString error;
		if ( !loadBpdIntoCheckpoints( nextPath, "RallyRoute.loadTrackLengthBpd", error ) )
			showMessage( QString( "Could not load %1 for the %2:\n%3" )
				.arg( QDir::toNativeSeparators( nextPath ), trackLengthLabel( g_trackLength ), error ), EMessageBoxType::Warning );
	}
	SceneChangeNotify();
	refreshEditor();
	return true;
}

struct GhostRouteSample {
	int timeMs = 0;
	Vec3 origin;
};

struct GhostRouteFile {
	QFileInfo fileInfo;
	QString mapName;
	int trackLength = -1;
	int trackReversed = 0;
	int gameType = -1;
	int clean = 0;
	std::vector<GhostRouteSample> samples;
};

struct GeneratedBpdPoint {
	int number = 0;
	Vec3 anchor;
	Vec3 handle;
};

static bool parseGhostRoute( const QFileInfo& fileInfo, GhostRouteFile& ghost ) {
	QFile file( fileInfo.absoluteFilePath() );
	if ( !file.open( QIODevice::ReadOnly | QIODevice::Text ) ) return false;
	ghost.fileInfo = fileInfo;
	QTextStream stream( &file );
	while ( !stream.atEnd() ) {
		const QByteArray line = stream.readLine().toLocal8Bit();
		const char* text = line.constData();
		if ( line.startsWith( "map " ) ) {
			ghost.mapName = QString::fromLocal8Bit( text + 4 ).trimmed();
			continue;
		}
		if ( line.startsWith( "track_length " ) ) {
			ghost.trackLength = line.mid( 13 ).trimmed().toInt();
			continue;
		}
		if ( line.startsWith( "track_reversed " ) ) {
			ghost.trackReversed = line.mid( 14 ).trimmed().toInt();
			continue;
		}
		if ( line.startsWith( "gametype " ) ) {
			ghost.gameType = line.mid( 9 ).trimmed().toInt();
			continue;
		}
		if ( line.startsWith( "clean " ) ) {
			ghost.clean = line.mid( 6 ).trimmed().toInt();
			continue;
		}
		if ( line.isEmpty() || ( text[0] != '-' && ( text[0] < '0' || text[0] > '9' ) ) ) continue;

		GhostRouteSample sample;
		float pitch, yaw, roll, velocityX, velocityY, velocityZ;
		int buttons, forwardMove, upMove;
		char extra;
		const int parsed = sscanf( text, " %d %f %f %f %f %f %f %f %f %f %d %d %d %c",
			&sample.timeMs, &sample.origin.x, &sample.origin.y, &sample.origin.z,
			&pitch, &yaw, &roll, &velocityX, &velocityY, &velocityZ,
			&buttons, &forwardMove, &upMove, &extra );
		if ( parsed != 13 || !std::isfinite( sample.origin.x ) || !std::isfinite( sample.origin.y ) || !std::isfinite( sample.origin.z ) )
			continue;
		ghost.samples.push_back( sample );
		if ( ghost.samples.size() > 200000 ) return false;
	}
	return ghost.mapName.length() > 0 && ghost.samples.size() > 1;
}

/* Best forward Ghost of the map; trackLength >= 0 also requires that length. */
static bool findGhostForMap( const QString& mapName, int trackLength, GhostRouteFile& bestGhost ) {
	const QString ghostPath = QDir( QDir::homePath() ).filePath( "AppData/Roaming/Q3Rally/baseq3r/ghosts" );
	QDir directory( ghostPath );
	if ( !directory.exists() ) return false;
	const QFileInfoList files = directory.entryInfoList( QStringList() << "*.ghost", QDir::Files, QDir::Time );
	bool found = false;
	for ( const QFileInfo& fileInfo : files ) {
		GhostRouteFile candidate;
		if ( !parseGhostRoute( fileInfo, candidate ) || candidate.mapName.compare( mapName, Qt::CaseInsensitive ) != 0 ) continue;
		const bool supportedRaceMode = candidate.gameType < 0 || candidate.gameType == 0 || candidate.gameType == 1 ||
			candidate.gameType == 2 || candidate.gameType == 7 || candidate.gameType == 8 ||
			candidate.gameType == 17 || candidate.gameType == 18;
		if ( candidate.trackReversed != 0 || !supportedRaceMode ) continue;
		if ( trackLength >= 0 && candidate.trackLength != trackLength ) continue;
		if ( !found || candidate.clean > bestGhost.clean ||
			( candidate.clean == bestGhost.clean && candidate.fileInfo.lastModified() > bestGhost.fileInfo.lastModified() ) ) {
			bestGhost = std::move( candidate );
			found = true;
		}
	}
	return found;
}

static double checkpointFrameCost( const Checkpoint& checkpoint, const Vec3& position, double& outsideDistance ) {
	const double dx = position.x - checkpoint.boundsCenter.x;
	const double dy = position.y - checkpoint.boundsCenter.y;
	const double dz = position.z - checkpoint.boundsCenter.z;
	const double outsideX = std::max( 0.0, std::fabs( dx ) - checkpoint.boundsExtents.x );
	const double outsideY = std::max( 0.0, std::fabs( dy ) - checkpoint.boundsExtents.y );
	const double outsideZ = std::max( 0.0, std::fabs( dz ) - checkpoint.boundsExtents.z );
	outsideDistance = sqrt( outsideX * outsideX + outsideY * outsideY + outsideZ * outsideZ );
	return outsideDistance * outsideDistance + 0.015 * ( dx * dx + dy * dy + dz * dz );
}

static bool buildBpdPoints( const std::vector<Checkpoint>& checkpoints, const GhostRouteFile& ghost,
	std::vector<GeneratedBpdPoint>& points, QString& error ) {
	const std::size_t checkpointCount = checkpoints.size();
	const std::size_t sampleCount = ghost.samples.size();
	if ( checkpointCount < 2 || sampleCount < checkpointCount ) {
		error = "The map needs at least two checkpoints and the Ghost must contain enough frames.";
		return false;
	}
	for ( const Checkpoint& checkpoint : checkpoints ) {
		if ( !checkpoint.hasBounds ) {
			error = QString( "Checkpoint %1 has no usable trigger bounds." ).arg( checkpoint.number );
			return false;
		}
	}

	const double infinity = std::numeric_limits<double>::infinity();
	std::vector<std::vector<double>> costs( checkpointCount, std::vector<double>( sampleCount, infinity ) );
	std::vector<std::vector<int>> parents( checkpointCount, std::vector<int>( sampleCount, -1 ) );
	for ( std::size_t checkpointIndex = 0; checkpointIndex < checkpointCount; ++checkpointIndex ) {
		for ( std::size_t sampleIndex = checkpointIndex; sampleIndex <= sampleCount - ( checkpointCount - checkpointIndex ); ++sampleIndex ) {
			double outsideDistance = 0.0;
			costs[checkpointIndex][sampleIndex] = checkpointFrameCost( checkpoints[checkpointIndex], ghost.samples[sampleIndex].origin, outsideDistance );
		}
	}

	std::vector<std::vector<double>> scores( checkpointCount, std::vector<double>( sampleCount, infinity ) );
	for ( std::size_t sampleIndex = 0; sampleIndex <= sampleCount - checkpointCount; ++sampleIndex )
		scores[0][sampleIndex] = costs[0][sampleIndex];
	for ( std::size_t checkpointIndex = 1; checkpointIndex < checkpointCount; ++checkpointIndex ) {
		double bestPrevious = infinity;
		int bestPreviousIndex = -1;
		const std::size_t lastAllowed = sampleCount - ( checkpointCount - checkpointIndex );
		for ( std::size_t sampleIndex = 0; sampleIndex <= lastAllowed; ++sampleIndex ) {
			if ( sampleIndex > 0 && scores[checkpointIndex - 1][sampleIndex - 1] < bestPrevious ) {
				bestPrevious = scores[checkpointIndex - 1][sampleIndex - 1];
				bestPreviousIndex = static_cast<int>( sampleIndex - 1 );
			}
			if ( bestPreviousIndex >= 0 ) {
				scores[checkpointIndex][sampleIndex] = bestPrevious + costs[checkpointIndex][sampleIndex];
				parents[checkpointIndex][sampleIndex] = bestPreviousIndex;
			}
		}
	}

	int sampleIndex = -1;
	for ( std::size_t i = 0; i < sampleCount; ++i ) {
		if ( scores[checkpointCount - 1][i] < infinity && ( sampleIndex < 0 || scores[checkpointCount - 1][i] < scores[checkpointCount - 1][sampleIndex] ) )
			sampleIndex = static_cast<int>( i );
	}
	if ( sampleIndex < 0 ) {
		error = "The Ghost frames could not be matched to the checkpoint order.";
		return false;
	}
	std::vector<int> matchedSamples( checkpointCount, -1 );
	for ( int checkpointIndex = static_cast<int>( checkpointCount ) - 1; checkpointIndex >= 0; --checkpointIndex ) {
		matchedSamples[checkpointIndex] = sampleIndex;
		if ( checkpointIndex > 0 ) sampleIndex = parents[checkpointIndex][sampleIndex];
	}

	points.clear();
	points.reserve( checkpointCount );
	for ( std::size_t i = 0; i < checkpointCount; ++i ) {
		const GhostRouteSample& sample = ghost.samples[matchedSamples[i]];
		double outsideDistance = 0.0;
		checkpointFrameCost( checkpoints[i], sample.origin, outsideDistance );
		if ( outsideDistance > 160.0 ) {
			error = QString( "Ghost is too far from checkpoint %1's trigger volume; no BPD was written." ).arg( checkpoints[i].number );
			return false;
		}
		GeneratedBpdPoint point;
		point.number = checkpoints[i].number;
		point.anchor = sample.origin;
		const int beforeIndex = std::max( 0, matchedSamples[i] - 6 );
		const int afterIndex = std::min( static_cast<int>( sampleCount ) - 1, matchedSamples[i] + 6 );
		Vec3 tangent = subtract( ghost.samples[afterIndex].origin, ghost.samples[beforeIndex].origin );
		float tangentLength = sqrtf( tangent.x * tangent.x + tangent.y * tangent.y + tangent.z * tangent.z );
		if ( tangentLength < 0.001f ) {
			const std::size_t previous = ( i + checkpointCount - 1 ) % checkpointCount;
			const std::size_t next = ( i + 1 ) % checkpointCount;
			tangent = subtract( ghost.samples[matchedSamples[next]].origin, ghost.samples[matchedSamples[previous]].origin );
			tangentLength = sqrtf( tangent.x * tangent.x + tangent.y * tangent.y + tangent.z * tangent.z );
		}
		const std::size_t previous = ( i + checkpointCount - 1 ) % checkpointCount;
		const std::size_t next = ( i + 1 ) % checkpointCount;
		const float toPrevious = sqrtf( powf( point.anchor.x - ghost.samples[matchedSamples[previous]].origin.x, 2.f ) +
			powf( point.anchor.y - ghost.samples[matchedSamples[previous]].origin.y, 2.f ) +
			powf( point.anchor.z - ghost.samples[matchedSamples[previous]].origin.z, 2.f ) );
		const float toNext = sqrtf( powf( point.anchor.x - ghost.samples[matchedSamples[next]].origin.x, 2.f ) +
			powf( point.anchor.y - ghost.samples[matchedSamples[next]].origin.y, 2.f ) +
			powf( point.anchor.z - ghost.samples[matchedSamples[next]].origin.z, 2.f ) );
		const float handleLength = std::min( 2048.f, std::max( 8.f, std::min( toPrevious, toNext ) * 0.35f ) );
		if ( tangentLength > 0.001f ) point.handle = scale( tangent, handleLength / tangentLength );
		points.push_back( point );
	}
	return true;
}

static bool writeGeneratedBpd( const QString& path, const std::vector<GeneratedBpdPoint>& points, QString& error ) {
	if ( QFileInfo::exists( path ) ) {
		error = "A BPD file already exists at the destination and was left unchanged.";
		return false;
	}
	QFile file( path );
	if ( !file.open( QIODevice::WriteOnly | QIODevice::NewOnly | QIODevice::Text ) ) {
		error = "Could not create the BPD file next to the map.";
		return false;
	}
	QTextStream stream( &file );
	stream.setLocale( QLocale::c() );
	stream.setRealNumberNotation( QTextStream::FixedNotation );
	stream.setRealNumberPrecision( 3 );
	for ( const GeneratedBpdPoint& point : points ) {
		stream << point.number << " : " << point.anchor.x << " " << point.anchor.y << " " << point.anchor.z << " : "
			<< point.handle.x << " " << point.handle.y << " " << point.handle.z << "\n";
	}
	if ( stream.status() != QTextStream::Ok ) {
		file.close();
		QFile::remove( path );
		error = "Could not write all generated checkpoint records.";
		return false;
	}
	file.close();
	if ( file.error() != QFileDevice::NoError ) {
		QFile::remove( path );
		error = "Could not finish writing the generated BPD file.";
		return false;
	}
	return true;
}

static QString g_watchedMapName;
static QString g_attemptedMapName;
static int g_mapStableTicks = 0;
static QTimer* g_mapWatcher = nullptr;

/* Writes a BPD seed from a recorded Ghost for every enabled track length that has no BPD yet.
   Existing files are never touched. Returns the paths written. */
static QStringList seedBpdsFromGhosts( const MapTrackInfo& info, QStringList& problems ) {
	QStringList created;
	const QString baseName = currentMapBaseName();
	for ( int trackLength = 0; trackLength < k_trackLengthCount; ++trackLength ) {
		if ( !info.allowed[trackLength] ) continue;
		const QString outputPath = trackLengthBpdPath( info, trackLength );
		if ( outputPath.isEmpty() || QFileInfo::exists( outputPath ) ) continue;
		const std::vector<Checkpoint> checkpoints = collectCheckpointsForLength( trackLength );
		if ( checkpoints.size() < 2 ) continue;
		/* A shared <map>.bpd may come from any length; per-length files need the matching Ghost. */
		const bool perLengthFile = !outputPath.endsWith( baseName + ".bpd", Qt::CaseInsensitive );
		GhostRouteFile ghost;
		if ( !findGhostForMap( baseName, perLengthFile ? trackLength : -1, ghost ) ) continue;
		std::vector<GeneratedBpdPoint> points;
		QString error;
		if ( !buildBpdPoints( checkpoints, ghost, points, error ) || !writeGeneratedBpd( outputPath, points, error ) ) {
			problems << QString( "%1: %2" ).arg( trackLengthLabel( trackLength ), error );
			continue;
		}
		created << QString( "%1 (%2, from %3)" ).arg( QDir::toNativeSeparators( outputPath ),
			trackLengthLabel( trackLength ), ghost.fileInfo.fileName() );
	}
	return created;
}

/* Runs once per opened map: picks the track length, offers to load the reference BPD when the
   map's Bezier keys differ from it, and seeds missing BPDs from Ghost recordings. */
static void checkMapReady() {
	const QString mapName = currentMapPath();
	if ( mapName.isEmpty() ) return;
	if ( mapName != g_watchedMapName ) {
		g_watchedMapName = mapName;
		g_mapStableTicks = 0;
		return;
	}
	if ( ++g_mapStableTicks < 3 || g_attemptedMapName == mapName ) return;

	if ( collectCheckpointsForLength( -1 ).size() < 2 ) {
		if ( g_mapStableTicks >= 10 ) g_attemptedMapName = mapName;
		return;
	}
	g_attemptedMapName = mapName;
	const MapTrackInfo info = collectMapTrackInfo();
	g_trackLength = firstAllowedTrackLength( info );
	refreshEditor();
	if ( currentMapBaseName().isEmpty() ) return;

	const QString path = trackLengthBpdPath( info, g_trackLength );
	if ( bpdSyncState( path, collectCheckpoints() ) == BpdSync::Differs ) {
		const QString question = QString( "The Bezier keys of the %1 differ from its BPD file\n%2\n\n"
			"The BPD file is the reference route. Load it into the checkpoints?" )
			.arg( trackLengthLabel( g_trackLength ), QDir::toNativeSeparators( path ) );
		if ( GlobalRadiant().m_pfnMessageBox( g_mainWindow, question.toLocal8Bit().constData(), "RallyRoute",
			EMessageBoxType::Question, eIDYES | eIDNO ) == eIDYES ) {
			QString error;
			if ( !loadBpdIntoCheckpoints( path, "RallyRoute.loadReferenceBpd", error ) ) showMessage( error, EMessageBoxType::Error );
			refreshEditor();
		}
	}

	QStringList problems;
	const QStringList created = seedBpdsFromGhosts( info, problems );
	if ( !problems.isEmpty() )
		showMessage( "Could not create a BPD seed from a Ghost:\n" + problems.join( "\n" ), EMessageBoxType::Warning, "RallyRoute — Ghost route seed" );
	if ( created.isEmpty() ) return;
	QString message = QString( "Created BPD seeds from Ghost recordings:\n%1\n\nThe routes use the Ghosts' world positions and are ready to refine." )
		.arg( created.join( "\n" ) );
	if ( bpdSyncState( path, collectCheckpoints() ) == BpdSync::Differs ) {
		message += QString( "\n\nLoad the seed for the %1 into the checkpoints now?" ).arg( trackLengthLabel( g_trackLength ) );
		if ( GlobalRadiant().m_pfnMessageBox( g_mainWindow, message.toLocal8Bit().constData(), "RallyRoute — Ghost route seed",
			EMessageBoxType::Question, eIDYES | eIDNO ) == eIDYES ) {
			QString error;
			if ( !loadBpdIntoCheckpoints( path, "RallyRoute.loadGhostSeed", error ) ) showMessage( error, EMessageBoxType::Error );
			refreshEditor();
		}
		return;
	}
	showMessage( message, EMessageBoxType::Info, "RallyRoute — Ghost route seed" );
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
	g_mapWatcher = new QTimer( main_window );
	g_mapWatcher->setInterval( 750 );
	QObject::connect( g_mapWatcher, &QTimer::timeout, checkMapReady );
	g_mapWatcher->start();
	return "Initializing RallyRoute Bezier editor";
}

const char* getName() { return "RallyRoute"; }
const char* getCommandTitleList() { return ""; }
const char* getCommandList() {
	return "Toggle Bezier Route;Edit Selected Checkpoint...;-;"
		"Track Length: Short;Track Length: Medium;Track Length: Long;-;"
		"Import BPD...;Export BPD...;Route Status";
}

void dispatch( const char* command, float*, float*, bool ) {
	if ( string_equal( command, "Toggle Bezier Route" ) ) {
		g_showRoute = !g_showRoute;
		SceneChangeNotify();
	} else if ( string_equal( command, "Edit Selected Checkpoint..." ) ) {
		if ( !g_editor ) g_editor = new RouteEditorDialog( main_window );
		g_editor->updateFromSelection();
		g_editor->show();
		g_editor->raise();
	} else if ( string_equal( command, "Track Length: Short" ) ) {
		switchTrackLength( 0 );
	} else if ( string_equal( command, "Track Length: Medium" ) ) {
		switchTrackLength( 1 );
	} else if ( string_equal( command, "Track Length: Long" ) ) {
		switchTrackLength( 2 );
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
