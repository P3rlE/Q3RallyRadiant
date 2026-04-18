#pragma once

#include "terrain_engine.h"
#include <vector>

namespace scene { class Node; }

struct TerrainBuildOptions
{
	bool undoable = true;
	bool preview  = false;
};

// Generates terrain brushes into func_group entities and inserts them into
// the scene graph. For standard terrain, one func_group is created.
// For tunnel terrain, four func_groups are created (floor, ceiling, left wall, right wall).
void build_terrain_brushes( const BrushData& target, double step_x, double step_y,
                             const HeightMap& height_map, const char* top_texture,
                             bool split_diagonally,
                             const TerrainBuildOptions& options,
                             std::vector<scene::Node*>* created_entities = nullptr );

void build_tunnel_brushes( const BrushData& target, double step_x, double step_y,
                            const TunnelMaps& maps, const char* top_texture,
                            double cave_height, double slope_height,
                            const TerrainBuildOptions& options,
                            std::vector<scene::Node*>* created_entities = nullptr );
