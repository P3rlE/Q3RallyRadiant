#include "botviz.h"

#include "iglrender.h"
#include "igl.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

// ── Singleton ─────────────────────────────────────────────────────────────────

BotViz& BotViz::instance() {
	static BotViz inst;
	return inst;
}

// ── JSON helpers ──────────────────────────────────────────────────────────────

static bool jsonFloat( const char* line, const char* key, float& out ) {
	const char* p = strstr( line, key );
	if ( !p ) return false;
	p += strlen( key );
	while ( *p && ( *p == '"' || *p == ':' || *p == ' ' ) ) p++;
	out = (float)atof( p );
	return true;
}
static bool jsonInt( const char* line, const char* key, int& out ) {
	float f = 0;
	if ( !jsonFloat( line, key, f ) ) return false;
	out = (int)f;
	return true;
}
static bool jsonStr( const char* line, const char* key, char* out, int maxLen ) {
	const char* p = strstr( line, key );
	if ( !p ) return false;
	p += strlen( key );
	while ( *p && *p != '"' ) p++;
	if ( *p != '"' ) return false;
	p++;
	int i = 0;
	while ( *p && *p != '"' && i < maxLen - 1 ) out[i++] = *p++;
	out[i] = '\0';
	return true;
}

// ── Load ──────────────────────────────────────────────────────────────────────

int BotViz::load( const char* path ) {
	FILE* f = fopen( path, "r" );
	if ( !f ) return -1;

	m_frames.clear();
	m_points.clear();
	m_nodeStats.clear();
	m_filePath     = path;
	m_resolved     = false;
	m_hasPositions = false;

	char line[1024];
	int  framesWithPos = 0;

	while ( fgets( line, sizeof(line), f ) ) {
		if ( line[0] != '{' ) continue;

		BotFrame fr{};
		jsonFloat( line, "\"time\"",           fr.time );
		jsonInt(   line, "\"nodeIndex\"",       fr.nodeIndex );
		jsonInt(   line, "\"routeIndex\"",      fr.routeIndex );
		jsonFloat( line, "\"actualSpeed\"",     fr.actualSpeed );
		jsonFloat( line, "\"routeDeviation\"",  fr.routeDeviation );
		jsonInt(   line, "\"collisionRisk\"",   fr.collisionRisk );
		jsonStr(   line, "\"decisionState\"",   fr.decisionState, 32 );

		if ( jsonFloat( line, "\"ox\"", fr.ox ) &&
		     jsonFloat( line, "\"oy\"", fr.oy ) &&
		     jsonFloat( line, "\"oz\"", fr.oz ) ) {
			fr.hasPosition = true;
			framesWithPos++;
		}

		m_frames.push_back( fr );
	}
	fclose( f );

	if ( !m_frames.empty() ) {
		m_maxSpeed     = 1.f;
		m_hasPositions = ( framesWithPos > (int)m_frames.size() / 2 );
		for ( auto& fr : m_frames )
			m_maxSpeed = std::max( m_maxSpeed, fr.actualSpeed );
		buildNodeStats();
	}
	return (int)m_frames.size();
}

void BotViz::buildNodeStats() {
	m_nodeStats.clear();
	for ( auto& fr : m_frames ) {
		int key = fr.nodeIndex;   // routeIndex = node order
		m_nodeStats[key].nodeOrder = key;
		m_nodeStats[key].addFrame( fr );
	}
	for ( auto& kv : m_nodeStats )
		kv.second.finalise();
}

void BotViz::clear() {
	m_frames.clear();
	m_points.clear();
	m_nodeStats.clear();
	m_filePath.clear();
	m_resolved     = false;
	m_hasPositions = false;
	playhead       = 0;
}

float BotViz::duration() const {
	if ( m_frames.size() < 2 ) return 0.f;
	return m_frames.back().time - m_frames.front().time;
}

const BotFrame* BotViz::frame( int i ) const {
	if ( i < 0 || i >= (int)m_frames.size() ) return nullptr;
	return &m_frames[i];
}

const NodeStats* BotViz::nodeStats( int order ) const {
	auto it = m_nodeStats.find( order );
	return ( it != m_nodeStats.end() ) ? &it->second : nullptr;
}

// ── Speed → color ─────────────────────────────────────────────────────────────

void BotViz::speedToColor( float speed, float& r, float& g, float& b ) const {
	float t = sqrtf( std::min( speed / m_maxSpeed, 1.f ) );
	if      ( t < 0.25f ) { float s=t/0.25f;       r=0;  g=s*.8f; b=1; }
	else if ( t < 0.50f ) { float s=(t-.25f)/.25f; r=0;  g=.8f;   b=1-s; }
	else if ( t < 0.75f ) { float s=(t-.50f)/.25f; r=s;  g=1;     b=0; }
	else                   { float s=(t-.75f)/.25f; r=1;  g=1-s;   b=0; }
}

// ── Resolve positions ─────────────────────────────────────────────────────────

void BotViz::resolvePositions( const std::map<int,Vector3>& nodePositions ) {
	m_points.clear();
	m_points.reserve( m_frames.size() );

	const float Z_OFFSET = 40.f;

	for ( auto& fr : m_frames ) {
		RoutePoint pt{};
		if ( fr.hasPosition ) {
			pt.pos = Vector3( fr.ox, fr.oy, fr.oz + Z_OFFSET );
		} else {
			auto it = nodePositions.find( fr.nodeIndex );
			if ( it != nodePositions.end() )
				pt.pos = it->second + Vector3( 0, 0, 120 );
		}
		speedToColor( fr.actualSpeed, pt.r, pt.g, pt.b );
		pt.isCollision = ( fr.collisionRisk > 0 );
		m_points.push_back( pt );
	}
	m_resolved = true;
}

// ── Rendering ─────────────────────────────────────────────────────────────────

void BotViz::renderWireframe() const {
	if ( !m_resolved || m_points.empty() ) return;

	if ( showRoute ) {
		gl().glLineWidth( 2.f );
		gl().glBegin( GL_LINE_STRIP );
		for ( auto& pt : m_points ) {
			gl().glColor3f( pt.r, pt.g, pt.b );
			gl().glVertex3fv( pt.pos.data() );
		}
		gl().glEnd();
	}

	if ( showCollisions ) {
		const float R = 80.f;
		gl().glLineWidth( 3.f );
		gl().glColor3f( 1.f, 0.1f, 0.f );
		gl().glBegin( GL_LINES );
		for ( auto& pt : m_points ) {
			if ( !pt.isCollision ) continue;
			gl().glVertex3f( pt.pos.x()-R, pt.pos.y(),   pt.pos.z() );
			gl().glVertex3f( pt.pos.x()+R, pt.pos.y(),   pt.pos.z() );
			gl().glVertex3f( pt.pos.x(),   pt.pos.y()-R, pt.pos.z() );
			gl().glVertex3f( pt.pos.x(),   pt.pos.y()+R, pt.pos.z() );
			gl().glVertex3f( pt.pos.x(),   pt.pos.y(),   pt.pos.z()-R );
			gl().glVertex3f( pt.pos.x(),   pt.pos.y(),   pt.pos.z()+R );
		}
		gl().glEnd();
	}

	{
		int fi = std::min( playhead, (int)m_points.size() - 1 );
		if ( fi >= 0 ) {
			const float R = 100.f;
			const auto& p = m_points[fi].pos;
			gl().glLineWidth( 4.f );
			gl().glColor3f( 1.f, 1.f, 1.f );
			gl().glBegin( GL_LINES );
			gl().glVertex3f( p.x()-R, p.y(),   p.z() ); gl().glVertex3f( p.x()+R, p.y(),   p.z() );
			gl().glVertex3f( p.x(),   p.y()-R, p.z() ); gl().glVertex3f( p.x(),   p.y()+R, p.z() );
			gl().glVertex3f( p.x(),   p.y(),   p.z()-R ); gl().glVertex3f( p.x(),   p.y(),   p.z()+R );
			gl().glEnd();
		}
	}
}

void BotViz::renderSolid() const {
	renderWireframe();
}
