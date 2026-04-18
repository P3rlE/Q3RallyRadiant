#pragma once

#include "terrain_engine.h"
#include <vector>

namespace scene { class Node; }

struct TerrainBuildOptions
{
	bool undoable = true;
	bool preview  = false;
};

struct TerrainMaterialSlots
{
	const char* base  = "";
	const char* steep = nullptr;
	const char* peak  = nullptr;
	const char* dirt  = nullptr;
	const char* track = nullptr;
};

struct TerrainMaterialRules
{
	double steep_angle_deg = 45.0;
	double peak_min_percent = 85.0;
	double dirt_min_percent = 20.0;
	double dirt_max_percent = 55.0;
	double track_min_percent = 40.0;
	double track_max_percent = 48.0;
};

// Generates terrain brushes into func_group entities and inserts them into
// the scene graph. For standard terrain, one func_group is created.
// For tunnel terrain, four func_groups are created (floor, ceiling, left wall, right wall).
void build_terrain_brushes( const BrushData& target, double step_x, double step_y,
                             const HeightMap& height_map, const char* top_texture,
                             const TerrainMaterialSlots& material_slots,
                             const TerrainMaterialRules& material_rules,
                             bool split_diagonally,
                             const TerrainBuildOptions& options,
                             std::vector<scene::Node*>* created_entities = nullptr );

void build_tunnel_brushes( const BrushData& target, double step_x, double step_y,
                            const TunnelMaps& maps, const char* top_texture,
                            const TerrainMaterialSlots& material_slots,
                            const TerrainMaterialRules& material_rules,
                            double cave_height, double slope_height,
                            const TerrainBuildOptions& options,
                            std::vector<scene::Node*>* created_entities = nullptr );
