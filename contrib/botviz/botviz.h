#pragma once

#include "math/vector.h"
#include <vector>
#include <string>
#include <map>
#include <unordered_map>
#include <cstring>

// ── Per-frame data from the .jsonl debug log ──────────────────────────────────

struct BotFrame {
	float time           = 0;
	int   nodeIndex      = 0;
	int   routeIndex     = 0;
	float actualSpeed    = 0;
	float routeDeviation = 0;
	int   collisionRisk  = 0;
	char  decisionState[32] = {};

	// Direct 3D position (ox/oy/oz) — present in updated log format
	float ox = 0, oy = 0, oz = 0;
	bool  hasPosition = false;
};

// ── Aggregated stats per bot_path_node (by order key) ────────────────────────

struct NodeStats {
	int   nodeOrder     = 0;
	int   frameCount    = 0;
	float avgSpeed      = 0;
	float maxSpeed      = 0;
	float minSpeed      = 9999;
	int   collisions    = 0;
	char  topState[32]  = {};   // most frequent decisionState

	// For computing topState
	std::unordered_map<std::string,int> stateCounts;

	void addFrame( const BotFrame& fr ) {
		frameCount++;
		avgSpeed   = avgSpeed + ( fr.actualSpeed - avgSpeed ) / frameCount; // running mean
		if ( fr.actualSpeed > maxSpeed ) maxSpeed = fr.actualSpeed;
		if ( fr.actualSpeed < minSpeed ) minSpeed = fr.actualSpeed;
		if ( fr.collisionRisk ) collisions++;
		stateCounts[fr.decisionState]++;
	}

	void finalise() {
		int best = 0;
		for ( auto& kv : stateCounts ) {
			if ( kv.second > best ) {
				best = kv.second;
				strncpy( topState, kv.first.c_str(), 31 );
				topState[31] = '\0';
			}
		}
	}
};

// ── Per-frame renderable point ────────────────────────────────────────────────

struct RoutePoint {
	Vector3 pos;
	float   r, g, b;
	bool    isCollision;
};

// ── Aggregated route segment buckets (nodeIndex -> next nodeIndex) ──────────

struct RouteBucketStats {
	int   fromNode = 0;
	int   toNode = 0;
	int   sampleCount = 0;
	float avgSpeed = 0.f;
	float collisionRate = 0.f;
	float avgRouteDeviation = 0.f;
	Vector3 fromPos;
	Vector3 toPos;
	bool    hasFromPos = false;
	bool    hasToPos = false;
};

enum class BotVizRenderMode {
	Route = 0,
	Heatmap = 1
};

// ── Main state object ─────────────────────────────────────────────────────────

class BotViz {
public:
	static BotViz& instance();

	int  load( const char* path );
	void clear();

	bool  isLoaded()      const { return !m_frames.empty(); }
	int   frameCount()    const { return (int)m_frames.size(); }
	bool  hasPositions()  const { return m_hasPositions; }
	float duration()      const;
	float startTime()     const;
	float endTime()       const;
	const std::string&  filePath()  const { return m_filePath; }
	const BotFrame*     frame( int i ) const;
	int frameIndexForTime( float t ) const;
	const RoutePoint* point( int i ) const;

	// Node stats — keyed by bot_path_node order value
	const NodeStats* nodeStats( int order ) const;
	bool             hasNodeStats() const { return !m_nodeStats.empty(); }
	const std::vector<RouteBucketStats>& routeBuckets() const { return m_routeBuckets; }

	void resolvePositions( const std::map<int,Vector3>& nodePositions );
	bool exportBucketsCsv( const char* path ) const;
	bool exportBucketsJson( const char* path ) const;

	void renderWireframe() const;
	void renderSolid()     const;

	int  playhead       = 0;
	bool showRoute      = true;
	bool showCollisions = true;
	BotVizRenderMode renderMode = BotVizRenderMode::Route;

private:
	BotViz() = default;

	void speedToColor( float speed, float& r, float& g, float& b ) const;
	void heatToColor( float heat, float& r, float& g, float& b ) const;
	void buildNodeStats();
	void buildRouteBuckets( const std::map<int,Vector3>& nodePositions );

	std::vector<BotFrame>            m_frames;
	std::vector<RoutePoint>          m_points;
	std::map<int,NodeStats>          m_nodeStats;   // key = node order
	std::vector<RouteBucketStats>    m_routeBuckets;
	std::string                      m_filePath;
	float                            m_maxSpeed     = 1.f;
	bool                             m_resolved     = false;
	bool                             m_hasPositions = false;
};
