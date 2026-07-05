#pragma once

#include "terrain_math.h"

#include <map>
#include <utility>
#include <tuple>
#include <vector>

using HeightMap   = std::map<std::pair<double, double>, double>;
using WallMap     = std::map<std::pair<double, double>, double>;
using MaskMap     = std::map<std::pair<double, double>, double>;

enum class SurfaceKind {
	Auto = 0,
	Track = 1,
	Shoulder = 2,
	Terrain = 3
};

using SurfaceMap = std::map<std::pair<double, double>, SurfaceKind>;

struct TunnelMaps
{
	HeightMap floor_map;
	HeightMap ceiling_map;
	WallMap   left_wall_map;
	WallMap   right_wall_map;
	double    step_z;
};

enum class ShapeType {
	Flat        = 0,
	Hill        = 1,
	Crater      = 2,
	Ridge       = 3,
	Slope       = 4,
	Volcano     = 5,
	Valley      = 6,
	Tunnel      = 7,
	SlopeTunnel = 8,
	BankedTurn  = 9,
	Berm        = 10,
	JumpRamp    = 11,
	Whoops      = 12,
	SCurve      = 13,
	Hairpin     = 14,
	OffCamber   = 15
};

enum class NoiseType {
	Perlin  = 0,
	Simplex = 1,
	Random  = 2
};

enum class TrackSectionType {
	Straight = 0,
	CurveLeft = 1,
	CurveRight = 2,
	BankedTurn = 3,
	SCurve = 4,
	Hairpin = 5,
	Jump = 6,
	Whoops = 7
};

struct PostProcessSettings
{
	int laplacian_iterations = 0;
	int thermal_iterations = 0;
	int hydraulic_iterations = 0;
};

struct TrackSectionOptions
{
	TrackSectionType type = TrackSectionType::Straight;
	double track_width = 384.0;
	double shoulder_width = 128.0;
	double berm_height = 64.0;
	double banking_angle_deg = 10.0;
	double feature_height = 128.0;
	double feature_length = 512.0;
	double curve_radius = 768.0;
	double curve_arc_degrees = 90.0;
	bool smooth_track = true;
};

struct TrackPort
{
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
	double heading_degrees = 90.0;
	double track_width = 384.0;
	double banking_angle_degrees = 0.0;
};

struct TrackSegmentSpec
{
	TrackSectionOptions options;
	TrackPort start_port;
	TrackPort end_port;
};

struct TrackChainSpec
{
	TrackPort start_port;
	std::vector<TrackSectionOptions> segments;
};

struct TrackSectionMaps
{
	HeightMap height_map;
	SurfaceMap surface_map;
	TrackPort start_port;
	TrackPort end_port;
};

BrushData make_manual_brush_data( double width, double length, double height );

void adjust_bounds_to_fit_grid( BrushData& target, double step_x, double step_y );

HeightMap generate_height_map( const BrushData& target, double step_x, double step_y,
                                ShapeType shape_type, double shape_height,
                                double variance, double frequency,
                                const MaskMap& mask_map,
                                NoiseType noise_type, double terrace_step,
                                double curve_radius, double banking_angle_deg,
                                double ramp_length,
                                const PostProcessSettings& post_process,
                                int seed );

TunnelMaps generate_tunnel_height_maps( const BrushData& target, double step_x, double step_y,
                                        double cave_height, double slope_height,
                                        double variance, double frequency,
                                        const MaskMap& mask_map,
                                        NoiseType noise_type, double terrace_step,
                                        int seed );

TrackSectionMaps generate_track_section_maps( const BrushData& target, double step_x, double step_y,
                                              const TrackSectionOptions& track_options,
                                              double variance, double frequency,
                                              const MaskMap& mask_map,
                                              NoiseType noise_type, double terrace_step,
                                              const PostProcessSettings& post_process,
                                              int seed );

TrackPort make_track_start_port( const BrushData& target, const TrackSectionOptions& track_options );

TrackPort compute_track_end_port( const BrushData& target, const TrackSectionOptions& track_options,
                                  const TrackPort& start_port );

std::vector<TrackSegmentSpec> build_track_chain_segments( const BrushData& target,
                                                          const TrackChainSpec& chain_spec );
