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
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		Entity* entity = Node_getEntity( path.top() );
		Checkpoint checkpoint;
		if ( readCheckpoint( entity, checkpoint ) ) {
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
	if ( checkpoints.size() < 2 ) return false;
	std::size_t index = checkpoints.size();
	for ( std::size_t i = 0; i < checkpoints.size(); ++i ) {
		if ( checkpoints[i].entity == entity ) { index = i; break; }
	}
	if ( index == checkpoints.size() ) return false;
	const std::size_t previous = ( index + checkpoints.size() - 1 ) % checkpoints.size();
	const std::size_t next = ( index + 1 ) % checkpoints.size();
	if ( !checkpoints[previous].hasAnchor || !checkpoints[next].hasAnchor ) return false;
	Vec3 tangent = subtract( checkpoints[next].anchor, checkpoints[previous].anchor );
	float length = sqrtf( tangent.x * tangent.x + tangent.y * tangent.y );
	if ( length < 0.001f ) {
		if ( !checkpoints[index].hasAnchor ) return false;
		tangent = subtract( checkpoints[next].anchor, checkpoints[index].anchor );
		length = sqrtf( tangent.x * tangent.x + tangent.y * tangent.y );
	}
	if ( length < 0.001f ) return false;
	yaw = atan2f( tangent.y, tangent.x ) * 57.2957795131f;
	return true;
}

class RouteEditorDialog : public QDialog {
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
	std::unique_ptr<UndoableCommand> m_editUndo;
public:
	explicit RouteEditorDialog( QWidget* parent ) : QDialog( parent, Qt::Tool ) {
		setWindowTitle( "RallyRoute — Bezier point and curve" );
		setMinimumWidth( 440 );
		auto* layout = new QVBoxLayout( this );
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
		connect( m_previousButton, &QPushButton::clicked, this, [this]() { navigateCheckpoint( -1 ); } );
		connect( m_nextButton, &QPushButton::clicked, this, [this]() { navigateCheckpoint( 1 ); } );
		updateFromSelection();
		auto* selectionTimer = new QTimer( this );
		selectionTimer->setInterval( 100 );
		connect( selectionTimer, &QTimer::timeout, this, [this]() {
			if ( isVisible() && selectedCheckpoint() != m_boundEntity ) updateFromSelection();
		} );
		selectionTimer->start();
	}

	void updateFromSelection() {
		m_editUndo.reset();
		m_loading = true;
		m_curveDirty = false;
		m_boundEntity = selectedCheckpoint();
		Checkpoint checkpoint;
		Entity* entity = findCheckpoint( m_boundEntity, checkpoint ) ? m_boundEntity : nullptr;
		if ( !entity ) {
			m_boundEntity = nullptr;
			m_selectionLabel->setText( "Select exactly one rally_checkpoint in the map." );
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

static QString bpdDirectory() {
	const QString directory = QDir( QDir::homePath() ).filePath( "AppData/Roaming/Q3Rally/baseq3r/maps" );
	return QDir( directory ).exists() ? directory : QDir::homePath();
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
	const QString path = QFileDialog::getSaveFileName( g_mainWindow, "Export Q3Rally Bezier path", bpdDirectory(), "Bezier paths (*.bpd);;All files (*)" );
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
	const QString path = QFileDialog::getOpenFileName( g_mainWindow, "Import Q3Rally Bezier path", bpdDirectory(), "Bezier paths (*.bpd);;All files (*)" );
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

struct GhostRouteSample {
	int timeMs = 0;
	Vec3 origin;
};

struct GhostRouteFile {
	QFileInfo fileInfo;
	QString mapName;
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

static bool findGhostForMap( const QString& mapName, GhostRouteFile& bestGhost ) {
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

static QString gameMapsDirectory() {
	return QDir( QDir::homePath() ).filePath( "AppData/Roaming/Q3Rally/baseq3r/maps" );
}

static bool writeGeneratedBpd( const QString& path, const std::vector<GeneratedBpdPoint>& points, QString& error ) {
	if ( QFileInfo::exists( path ) ) {
		error = "A BPD file already exists at the destination and was left unchanged.";
		return false;
	}
	QFile file( path );
	if ( !file.open( QIODevice::WriteOnly | QIODevice::NewOnly | QIODevice::Text ) ) {
		error = "Could not create the BPD file in the Q3Rally maps folder.";
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

static void checkMapForGhostBpd() {
	const char* mapNameText = GlobalRadiant().getMapName ? GlobalRadiant().getMapName() : nullptr;
	const QString mapName = mapNameText ? QString::fromLocal8Bit( mapNameText ) : QString();
	if ( mapName.isEmpty() ) return;
	if ( mapName != g_watchedMapName ) {
		g_watchedMapName = mapName;
		g_mapStableTicks = 0;
		return;
	}
	if ( ++g_mapStableTicks < 3 || g_attemptedMapName == mapName ) return;

	const std::vector<Checkpoint> checkpoints = collectCheckpoints();
	if ( checkpoints.size() < 2 ) {
		if ( g_mapStableTicks >= 10 ) g_attemptedMapName = mapName;
		return;
	}
	g_attemptedMapName = mapName;
	const QString mapBaseName = QFileInfo( mapName ).completeBaseName();
	if ( mapBaseName.isEmpty() || mapBaseName.compare( "unnamed", Qt::CaseInsensitive ) == 0 ) return;
	const QString mapsDirectory = gameMapsDirectory();
	const QString outputPath = QDir( mapsDirectory ).filePath( mapBaseName + ".bpd" );
	if ( QFileInfo::exists( outputPath ) ) return;

	GhostRouteFile ghost;
	if ( !findGhostForMap( mapBaseName, ghost ) ) return;
	std::vector<GeneratedBpdPoint> points;
	QString error;
	if ( !buildBpdPoints( checkpoints, ghost, points, error ) ) {
		GlobalRadiant().m_pfnMessageBox( g_mainWindow, error.toLocal8Bit().constData(), "RallyRoute — Ghost route seed", EMessageBoxType::Warning, 0 );
		return;
	}
	if ( !QDir().mkpath( mapsDirectory ) ) {
		error = "Could not create the Q3Rally maps folder.";
		GlobalRadiant().m_pfnMessageBox( g_mainWindow, error.toLocal8Bit().constData(), "RallyRoute — Ghost route seed", EMessageBoxType::Warning, 0 );
		return;
	}
	if ( !writeGeneratedBpd( outputPath, points, error ) ) {
		GlobalRadiant().m_pfnMessageBox( g_mainWindow, error.toLocal8Bit().constData(), "RallyRoute — Ghost route seed", EMessageBoxType::Warning, 0 );
		return;
	}

	const QString message = QString( "Created a BPD seed from %1:\n%2\n\nThe route uses the Ghost's world positions and is ready to inspect and refine in RallyRoute." )
		.arg( ghost.fileInfo.fileName(), outputPath );
	GlobalRadiant().m_pfnMessageBox( g_mainWindow, message.toLocal8Bit().constData(), "RallyRoute — Ghost route seed", EMessageBoxType::Info, 0 );
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
	QObject::connect( g_mapWatcher, &QTimer::timeout, checkMapForGhostBpd );
	g_mapWatcher->start();
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
