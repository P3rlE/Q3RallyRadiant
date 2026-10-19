#include "botviz.h"

#include "iglrender.h"
#include "igl.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <cerrno>
#include <cctype>
#include <algorithm>
#include <unordered_map>
#include <string>

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
	while ( *p && ( *p == '"' || *p == ':' || std::isspace( static_cast<unsigned char>( *p ) ) ) ) p++;
	char* end = nullptr;
	out = std::strtof( p, &end );
	if ( end == p || !std::isfinite( out ) ) return false;
	return true;
}
static bool jsonInt( const char* line, const char* key, int& out ) {
	const char* p = strstr( line, key );
	if ( !p ) return false;
	p += strlen( key );
	while ( *p && ( *p == '"' || *p == ':' || std::isspace( static_cast<unsigned char>( *p ) ) ) ) p++;
	char* end = nullptr;
	errno = 0;
	const long parsed = std::strtol( p, &end, 10 );
	if ( end == p || errno == ERANGE || parsed < -2147483647L - 1L || parsed > 2147483647L ) return false;
	if ( *end && *end != ',' && *end != '}' && !std::isspace( static_cast<unsigned char>( *end ) ) ) return false;
	out = static_cast<int>( parsed );
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

static bool parseGhostFrame( const char* line, int& timeMs, float& x, float& y, float& z ) {
	const char* p = line;
	char* end = nullptr;
	while ( *p == ' ' || *p == '\t' ) ++p;
	if ( *p < '0' || *p > '9' ) return false;

	errno = 0;
	const long parsedTime = std::strtol( p, &end, 10 );
	if ( end == p || errno == ERANGE || parsedTime < 0 || parsedTime > 2147483647L ) return false;
	timeMs = static_cast<int>( parsedTime );
	p = end;

	auto parseCoordinate = [&]( float& value ) {
		if ( *p != ' ' && *p != '\t' ) return false;
		while ( *p == ' ' || *p == '\t' ) ++p;
		value = std::strtof( p, &end );
		if ( end == p || !std::isfinite( value ) ) return false;
		p = end;
		return true;
	};

	if ( !parseCoordinate( x ) || !parseCoordinate( y ) || !parseCoordinate( z ) ) return false;
	return *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == '\0';
}

static bool parseGhostInteger( const char* text, int& value ) {
	if ( !text || !text[0] ) return false;
	char* end = nullptr;
	errno = 0;
	const long parsed = std::strtol( text, &end, 10 );
	while ( end && *end && std::isspace( static_cast<unsigned char>( *end ) ) ) ++end;
	if ( end == text || !end || *end != '\0' || errno == ERANGE || parsed < -2147483647L - 1L || parsed > 2147483647L ) return false;
	value = static_cast<int>( parsed );
	return true;
}

// ── Load ──────────────────────────────────────────────────────────────────────

int BotViz::load( const char* path ) {
	FILE* f = fopen( path, "r" );
	if ( !f ) return -1;

	clear();
	m_filePath     = path;
	m_sourceDescription = "Bot JSONL telemetry";

	char line[1024];
	int  framesWithPos = 0;

	while ( fgets( line, sizeof(line), f ) ) {
		if ( line[0] != '{' ) continue;

		BotFrame fr{};
		if ( !jsonFloat( line, "\"time\"", fr.time ) ) continue;
		jsonInt(   line, "\"nodeIndex\"",       fr.nodeIndex );
		jsonInt(   line, "\"routeIndex\"",      fr.routeIndex );
		jsonInt(   line, "\"pathId\"",          fr.pathId );
		jsonFloat( line, "\"actualSpeed\"",     fr.actualSpeed );
		jsonFloat( line, "\"routeDeviation\"",  fr.routeDeviation );
		jsonInt(   line, "\"collisionRisk\"",   fr.collisionRisk );
		jsonStr(   line, "\"decisionState\"",   fr.decisionState, 32 );
		jsonStr(   line, "\"routeSource\"",     fr.routeSource, 16 );
		jsonStr(   line, "\"activeRoutePath\"", fr.activeRoutePath, 128 );

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
		m_isGhostRoute = std::any_of( m_frames.begin(), m_frames.end(), []( const BotFrame& fr ) {
			return strcmp( fr.routeSource, "ghost" ) == 0;
		} );
		m_sourceDescription = m_isGhostRoute
			? "Bot JSONL telemetry · includes Ghost route samples"
			: "Bot JSONL telemetry";
		for ( auto& fr : m_frames )
			m_maxSpeed = std::max( m_maxSpeed, fr.actualSpeed );
		buildNodeStats();
	}
	return (int)m_frames.size();
}

int BotViz::loadGhostRoute( const char* path ) {
	FILE* f = fopen( path, "r" );
	if ( !f ) return -1;
	if ( fseek( f, 0, SEEK_END ) != 0 ) {
		fclose( f );
		return -2;
	}
	const long fileSize = ftell( f );
	if ( fileSize <= 0 || fileSize > 2 * 1024 * 1024 || fseek( f, 0, SEEK_SET ) != 0 ) {
		fclose( f );
		return -2;
	}

	clear();
	m_filePath = path;
	m_isGhostRoute = true;
	char line[2048];
	char mapName[128] = {};
	int trackLength = -1;
	int trackReversed = -1;
	int bestTimeMs = 0;
	int clean = 0;
	bool inFrames = false;
	int previousTimeMs = -1;
	int framesWithPosition = 0;

	while ( fgets( line, sizeof( line ), f ) ) {
		if ( !inFrames ) {
			if ( strncmp( line, "map ", 4 ) == 0 ) {
				sscanf( line + 4, "%127s", mapName );
			} else if ( strncmp( line, "track_length ", 13 ) == 0 ) {
				parseGhostInteger( line + 13, trackLength );
			} else if ( strncmp( line, "track_reversed ", 15 ) == 0 ) {
				parseGhostInteger( line + 15, trackReversed );
			} else if ( strncmp( line, "best_time_ms ", 13 ) == 0 ) {
				parseGhostInteger( line + 13, bestTimeMs );
			} else if ( strncmp( line, "clean ", 6 ) == 0 ) {
				parseGhostInteger( line + 6, clean );
			} else if ( strncmp( line, "frames", 6 ) == 0 && ( line[6] == '\0' || line[6] == ' ' || line[6] == '\t' || line[6] == '\r' || line[6] == '\n' ) ) {
				inFrames = true;
			}
			continue;
		}

		const char* frameLine = line;
		while ( *frameLine == ' ' || *frameLine == '\t' || *frameLine == '\r' || *frameLine == '\n' ) ++frameLine;
		if ( *frameLine == '#' || *frameLine == '\0' ) continue;
		int timeMs = 0;
		float x = 0.f, y = 0.f, z = 0.f;
		if ( !parseGhostFrame( frameLine, timeMs, x, y, z ) || ( previousTimeMs >= 0 && timeMs < previousTimeMs ) ) {
			fclose( f );
			clear();
			return -2;
		}
		if ( m_frames.size() >= 65536 ) {
			fclose( f );
			clear();
			return -2;
		}

		BotFrame fr{};
		fr.time = timeMs * 0.001f;
		fr.nodeIndex = static_cast<int>( m_frames.size() );
		fr.routeIndex = fr.nodeIndex;
		fr.pathId = -1;
		strncpy( fr.routeSource, "ghost", sizeof( fr.routeSource ) - 1 );
		fr.ox = x;
		fr.oy = y;
		fr.oz = z;
		fr.hasPosition = true;
		m_frames.push_back( fr );
		previousTimeMs = timeMs;
		framesWithPosition++;
	}
	const bool readFailed = ferror( f ) != 0;
	fclose( f );

	if ( readFailed || !inFrames || m_frames.size() < 2 ) {
		clear();
		return -2;
	}

	for ( std::size_t i = 1; i < m_frames.size(); ++i ) {
		const float elapsed = m_frames[i].time - m_frames[i - 1].time;
		if ( elapsed > 0.f ) {
			const float dx = m_frames[i].ox - m_frames[i - 1].ox;
			const float dy = m_frames[i].oy - m_frames[i - 1].oy;
			const float dz = m_frames[i].oz - m_frames[i - 1].oz;
			m_frames[i].actualSpeed = std::sqrt( dx * dx + dy * dy + dz * dz ) / elapsed;
		}
	}

	m_filePath = path;
	m_hasPositions = framesWithPosition > (int)m_frames.size() / 2;
	m_maxSpeed = 1.f;
	for ( const BotFrame& fr : m_frames ) m_maxSpeed = std::max( m_maxSpeed, fr.actualSpeed );

	char description[384];
	const char* mapLabel = mapName[0] ? mapName : "unknown map";
	const char* direction = trackReversed == 1 ? "reverse" : trackReversed == 0 ? "normal" : "direction unknown";
	const char* cleanLabel = clean ? "clean · " : "";
	if ( trackLength >= 0 ) {
		std::snprintf( description, sizeof( description ), "Ghost: %s · length %d · %s · %s%.2f s · %d frames",
			mapLabel, trackLength, direction, cleanLabel, bestTimeMs * 0.001, static_cast<int>( m_frames.size() ) );
	} else {
		std::snprintf( description, sizeof( description ), "Ghost: %s · %s%.2f s · %d frames",
			mapLabel, cleanLabel, bestTimeMs * 0.001, static_cast<int>( m_frames.size() ) );
	}
	m_sourceDescription = description;
	buildNodeStats();
	return static_cast<int>( m_frames.size() );
}

void BotViz::buildNodeStats() {
	m_nodeStats.clear();
	for ( auto& fr : m_frames ) {
		if ( strcmp( fr.routeSource, "ghost" ) == 0 ) continue;
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
	m_hasRenderablePositions = false;
	m_isGhostRoute = false;
	m_sourceDescription.clear();
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
	m_hasRenderablePositions = false;

	const float Z_OFFSET = 40.f;

	for ( auto& fr : m_frames ) {
		RoutePoint pt{};
		if ( fr.hasPosition ) {
			pt.pos = Vector3( fr.ox, fr.oy, fr.oz + Z_OFFSET );
			pt.hasPosition = true;
		} else {
			auto it = nodePositions.find( fr.nodeIndex );
			if ( it != nodePositions.end() ) {
				pt.pos = it->second + Vector3( 0, 0, 120 );
				pt.hasPosition = true;
			}
		}
		m_hasRenderablePositions = m_hasRenderablePositions || pt.hasPosition;
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
	std::unordered_map<std::string, RunningBucket> buckets;

	const float Z_OFFSET = 40.f;

	for ( size_t i = 0; i + 1 < m_frames.size(); ++i ) {
		const BotFrame& fr = m_frames[i];
		const BotFrame& next = m_frames[i + 1];

		const int fromNode = fr.nodeIndex;
		const int toNode = next.nodeIndex;
		const char* routeSource = fr.routeSource[0] ? fr.routeSource : "map";
		const int pathId = fr.pathId;
		const std::string key = std::string( routeSource ) + ":" + std::to_string( pathId ) + ":" +
			std::to_string( fromNode ) + ":" + std::to_string( toNode );

		auto& bucket = buckets[key];
		bucket.out.fromNode = fromNode;
		bucket.out.toNode = toNode;
		bucket.out.pathId = pathId;
		strncpy( bucket.out.routeSource, routeSource, sizeof( bucket.out.routeSource ) - 1 );
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
			const int sourceOrder = strcmp( a.routeSource, b.routeSource );
			if ( sourceOrder != 0 ) return sourceOrder < 0;
			if ( a.pathId != b.pathId ) return a.pathId < b.pathId;
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
			gl().glBegin( GL_LINES );
			for ( std::size_t i = 0; i + 1 < m_points.size(); ++i ) {
				const RoutePoint& from = m_points[i];
				const RoutePoint& to = m_points[i + 1];
				if ( !from.hasPosition || !to.hasPosition ) continue;
				gl().glColor3f( from.r, from.g, from.b );
				gl().glVertex3fv( from.pos.data() );
				gl().glColor3f( to.r, to.g, to.b );
				gl().glVertex3fv( to.pos.data() );
			}
			gl().glEnd();
		}
		else {
			gl().glLineWidth( 4.f );
			gl().glBegin( GL_LINES );
			for ( auto& b : m_routeBuckets ) {
				if ( !b.hasFromPos || !b.hasToPos ) continue;
				float r = 0.f, g = 0.f, bb = 1.f;
				if ( strcmp( b.routeSource, "ghost" ) == 0 ) {
					speedToColor( b.avgSpeed, r, g, bb );
				} else {
					heatToColor( b.collisionRate, r, g, bb );
				}
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
			if ( !pt.isCollision || !pt.hasPosition ) continue;
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
		if ( fi >= 0 && m_points[fi].hasPosition ) {
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
	fprintf( f, "routeSource,pathId,fromNode,toNode,sampleCount,avgSpeed,collisionRate,avgRouteDeviation,fromX,fromY,fromZ,toX,toY,toZ\n" );
	for ( auto& b : m_routeBuckets ) {
		fprintf( f, "%s,%d,%d,%d,%d,%.4f,%.4f,%.4f",
			b.routeSource, b.pathId, b.fromNode, b.toNode, b.sampleCount, b.avgSpeed, b.collisionRate, b.avgRouteDeviation );
		if ( b.hasFromPos ) fprintf( f, ",%.3f,%.3f,%.3f", b.fromPos.x(), b.fromPos.y(), b.fromPos.z() );
		else fprintf( f, ",,," );
		if ( b.hasToPos ) fprintf( f, ",%.3f,%.3f,%.3f", b.toPos.x(), b.toPos.y(), b.toPos.z() );
		else fprintf( f, ",,," );
		fprintf( f, "\n" );
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
			"  {\"routeSource\":\"%s\",\"pathId\":%d,\"fromNode\":%d,\"toNode\":%d,\"sampleCount\":%d,\"avgSpeed\":%.4f,"
			"\"collisionRate\":%.4f,\"avgRouteDeviation\":%.4f,\"fromPos\":",
			b.routeSource, b.pathId, b.fromNode, b.toNode, b.sampleCount, b.avgSpeed, b.collisionRate, b.avgRouteDeviation );
		if ( b.hasFromPos ) {
			fprintf( f, "[%.3f,%.3f,%.3f]", b.fromPos.x(), b.fromPos.y(), b.fromPos.z() );
		} else {
			fprintf( f, "null" );
		}
		fprintf( f, ",\"toPos\":" );
		if ( b.hasToPos ) {
			fprintf( f, "[%.3f,%.3f,%.3f]", b.toPos.x(), b.toPos.y(), b.toPos.z() );
		} else {
			fprintf( f, "null" );
		}
		fprintf( f, "}%s\n", (i + 1 < m_routeBuckets.size()) ? "," : "" );
	}
	fprintf( f, "]\n" );
	fclose( f );
	return true;
}

void BotViz::renderSolid() const {
	renderWireframe();
}
