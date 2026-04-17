#include "botviz.h"

#include "iglrender.h"
#include "igl.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <unordered_map>

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
	m_routeBuckets.clear();
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
	m_routeBuckets.clear();
	m_filePath.clear();
	m_resolved     = false;
	m_hasPositions = false;
	playhead       = 0;
}

float BotViz::duration() const {
	if ( m_frames.size() < 2 ) return 0.f;
	return m_frames.back().time - m_frames.front().time;
}

float BotViz::startTime() const {
	return m_frames.empty() ? 0.f : m_frames.front().time;
}

float BotViz::endTime() const {
	return m_frames.empty() ? 0.f : m_frames.back().time;
}

const BotFrame* BotViz::frame( int i ) const {
	if ( i < 0 || i >= (int)m_frames.size() ) return nullptr;
	return &m_frames[i];
}

int BotViz::frameIndexForTime( float t ) const {
	if ( m_frames.empty() ) return -1;
	if ( t <= m_frames.front().time ) return 0;
	if ( t >= m_frames.back().time ) return (int)m_frames.size() - 1;

	auto it = std::lower_bound(
		m_frames.begin(), m_frames.end(), t,
		[]( const BotFrame& fr, float value ){ return fr.time < value; } );
	if ( it == m_frames.end() ) return (int)m_frames.size() - 1;
	if ( it == m_frames.begin() ) return 0;

	const int idx = (int)( it - m_frames.begin() );
	const float a = m_frames[idx - 1].time;
	const float b = m_frames[idx].time;
	return ( std::fabs( t - a ) <= std::fabs( b - t ) ) ? idx - 1 : idx;
}

const RoutePoint* BotViz::point( int i ) const {
	if ( i < 0 || i >= (int)m_points.size() ) return nullptr;
	return &m_points[i];
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

void BotViz::heatToColor( float heat, float& r, float& g, float& b ) const {
	float t = std::max( 0.f, std::min( heat, 1.f ) );
	if      ( t < 0.5f ) { float s = t / 0.5f; r = 0.1f; g = 0.3f + 0.5f * s; b = 1.f - 0.8f * s; }
	else                 { float s = ( t - 0.5f ) / 0.5f; r = 0.1f + 0.9f * s; g = 0.8f - 0.7f * s; b = 0.2f - 0.2f * s; }
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
	buildRouteBuckets( nodePositions );
	m_resolved = true;
}

void BotViz::buildRouteBuckets( const std::map<int,Vector3>& nodePositions ) {
	m_routeBuckets.clear();
	if ( m_frames.size() < 2 ) return;

	struct RunningBucket {
		RouteBucketStats out;
		float speedAccum = 0.f;
		float collisionAccum = 0.f;
		float deviationAccum = 0.f;
	};
	std::unordered_map<long long, RunningBucket> buckets;

	const float Z_OFFSET = 40.f;

	for ( size_t i = 0; i + 1 < m_frames.size(); ++i ) {
		const BotFrame& fr = m_frames[i];
		const BotFrame& next = m_frames[i + 1];

		const int fromNode = fr.nodeIndex;
		const int toNode = next.nodeIndex;
		const long long key = ( (long long)fromNode << 32 ) | (unsigned int)toNode;

		auto& bucket = buckets[key];
		bucket.out.fromNode = fromNode;
		bucket.out.toNode = toNode;
		bucket.out.sampleCount++;
		bucket.speedAccum += fr.actualSpeed;
		bucket.collisionAccum += ( fr.collisionRisk > 0 ) ? 1.f : 0.f;
		bucket.deviationAccum += fr.routeDeviation;

		if ( fr.hasPosition ) {
			bucket.out.fromPos = Vector3( fr.ox, fr.oy, fr.oz + Z_OFFSET );
			bucket.out.hasFromPos = true;
		}
		else {
			auto it = nodePositions.find( fromNode );
			if ( it != nodePositions.end() ) {
				bucket.out.fromPos = it->second + Vector3( 0, 0, 120 );
				bucket.out.hasFromPos = true;
			}
		}

		if ( next.hasPosition ) {
			bucket.out.toPos = Vector3( next.ox, next.oy, next.oz + Z_OFFSET );
			bucket.out.hasToPos = true;
		}
		else {
			auto it = nodePositions.find( toNode );
			if ( it != nodePositions.end() ) {
				bucket.out.toPos = it->second + Vector3( 0, 0, 120 );
				bucket.out.hasToPos = true;
			}
		}
	}

	m_routeBuckets.reserve( buckets.size() );
	for ( auto& kv : buckets ) {
		RunningBucket& b = kv.second;
		if ( b.out.sampleCount > 0 ) {
			b.out.avgSpeed = b.speedAccum / b.out.sampleCount;
			b.out.collisionRate = b.collisionAccum / b.out.sampleCount;
			b.out.avgRouteDeviation = b.deviationAccum / b.out.sampleCount;
		}
		m_routeBuckets.push_back( b.out );
	}

	std::sort( m_routeBuckets.begin(), m_routeBuckets.end(),
		[]( const RouteBucketStats& a, const RouteBucketStats& b ) {
			if ( a.fromNode != b.fromNode ) return a.fromNode < b.fromNode;
			return a.toNode < b.toNode;
		} );
}

// ── Rendering ─────────────────────────────────────────────────────────────────

void BotViz::renderWireframe() const {
	if ( !m_resolved || m_points.empty() ) return;

	if ( showRoute ) {
		if ( renderMode == BotVizRenderMode::Route ) {
			gl().glLineWidth( 2.f );
			gl().glBegin( GL_LINE_STRIP );
			for ( auto& pt : m_points ) {
				gl().glColor3f( pt.r, pt.g, pt.b );
				gl().glVertex3fv( pt.pos.data() );
			}
			gl().glEnd();
		}
		else {
			gl().glLineWidth( 4.f );
			gl().glBegin( GL_LINES );
			for ( auto& b : m_routeBuckets ) {
				if ( !b.hasFromPos || !b.hasToPos ) continue;
				float r = 0.f, g = 0.f, bb = 1.f;
				heatToColor( b.collisionRate, r, g, bb );
				gl().glColor3f( r, g, bb );
				gl().glVertex3fv( b.fromPos.data() );
				gl().glVertex3fv( b.toPos.data() );
			}
			gl().glEnd();
		}
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

bool BotViz::exportBucketsCsv( const char* path ) const {
	if ( m_routeBuckets.empty() ) return false;
	FILE* f = fopen( path, "w" );
	if ( !f ) return false;
	fprintf( f, "fromNode,toNode,sampleCount,avgSpeed,collisionRate,avgRouteDeviation,fromX,fromY,fromZ,toX,toY,toZ\n" );
	for ( auto& b : m_routeBuckets ) {
		fprintf( f, "%d,%d,%d,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
			b.fromNode, b.toNode, b.sampleCount, b.avgSpeed, b.collisionRate, b.avgRouteDeviation,
			b.fromPos.x(), b.fromPos.y(), b.fromPos.z(), b.toPos.x(), b.toPos.y(), b.toPos.z() );
	}
	fclose( f );
	return true;
}

bool BotViz::exportBucketsJson( const char* path ) const {
	if ( m_routeBuckets.empty() ) return false;
	FILE* f = fopen( path, "w" );
	if ( !f ) return false;
	fprintf( f, "[\n" );
	for ( size_t i = 0; i < m_routeBuckets.size(); ++i ) {
		auto& b = m_routeBuckets[i];
		fprintf( f,
			"  {\"fromNode\":%d,\"toNode\":%d,\"sampleCount\":%d,\"avgSpeed\":%.4f,"
			"\"collisionRate\":%.4f,\"avgRouteDeviation\":%.4f,"
			"\"fromPos\":[%.3f,%.3f,%.3f],\"toPos\":[%.3f,%.3f,%.3f]}%s\n",
			b.fromNode, b.toNode, b.sampleCount, b.avgSpeed, b.collisionRate, b.avgRouteDeviation,
			b.fromPos.x(), b.fromPos.y(), b.fromPos.z(), b.toPos.x(), b.toPos.y(), b.toPos.z(),
			(i + 1 < m_routeBuckets.size()) ? "," : "" );
	}
	fprintf( f, "]\n" );
	fclose( f );
	return true;
}

void BotViz::renderSolid() const {
	renderWireframe();
}
