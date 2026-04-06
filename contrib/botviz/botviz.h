#pragma once

#include "math/vector.h"
#include <vector>
#include <string>
#include <map>

// ── Per-frame data from the .jsonl debug log ──────────────────────────────────

struct BotFrame {
	float time          = 0;
	int   nodeIndex     = 0;
	int   routeIndex    = 0;
	float actualSpeed   = 0;
	float routeDeviation= 0;
	int   collisionRisk = 0;
	char  decisionState[32] = {};
};

// ── Per-frame renderable point (position + color, resolved at load) ───────────

struct RoutePoint {
	Vector3 pos;
	float   r, g, b;
	bool    isCollision;
};

// ── Main state object ─────────────────────────────────────────────────────────

class BotViz {
public:
	static BotViz& instance();

	// Load .jsonl — returns frame count, -1 on error
	int  load( const char* path );
	void clear();

	bool isLoaded() const { return !m_frames.empty(); }
	int  frameCount() const { return (int)m_frames.size(); }
	float duration() const;
	const std::string& filePath() const { return m_filePath; }
	const BotFrame* frame( int i ) const;

	// Called once after load to bake positions from scene node table
	void resolvePositions( const std::map<int,Vector3>& nodePositions );

	// Render — called from OpenGLRenderable::render()
	void renderWireframe() const;
	void renderSolid()     const;

	// Playhead
	int  playhead = 0;

	// Toggles
	bool showRoute      = true;
	bool showCollisions = true;

private:
	BotViz() = default;

	void   speedToColor( float speed, float& r, float& g, float& b ) const;

	std::vector<BotFrame>   m_frames;
	std::vector<RoutePoint> m_points;   // resolved positions
	std::string             m_filePath;
	float                   m_maxSpeed  = 1.f;
	bool                    m_resolved  = false;
};
