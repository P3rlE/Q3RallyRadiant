#include "terrain_engine.h"
#include "noise.h"

#include <cmath>
#include <algorithm>
#include <numbers>
#include <random>
#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static double round2( double v ){
	return std::round( v * 100.0 ) / 100.0;
}

class TerrainRng
{
	std::mt19937 m_engine;
	std::uniform_real_distribution<double> m_dist;
public:
	explicit TerrainRng( std::uint32_t seed ) : m_engine( seed ), m_dist( 0.0, 1.0 ){}

	double random_double(){
		return m_dist( m_engine );
	}
};

static std::uint32_t mix_seed( std::uint32_t base_seed, std::uint32_t salt ){
	std::uint32_t x = base_seed ^ salt;
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

static double sample_noise( NoiseType noise_type, double x, double y, TerrainRng& rng ){
	switch ( noise_type ) {
	case NoiseType::Perlin:  return Perlin::noise( x, y );
	case NoiseType::Simplex: return Simplex::noise( x, y );
	default:                 return rng.random_double() * 2.0 - 1.0;
	}
}

static double sample_mask_weight( const MaskMap& mask_map, double x, double y ){
	if ( mask_map.empty() ) {
		return 1.0;
	}
	const auto it = mask_map.find( { round2( x ), round2( y ) } );
	if ( it == mask_map.end() ) {
		return 1.0;
	}
	return std::clamp( it->second, 0.0, 1.0 );
}

// ---------------------------------------------------------------------------

BrushData make_manual_brush_data( double width, double length, double height ){
	BrushData b;
	b.min_x   = -width  / 2.0;
	b.max_x   =  width  / 2.0;
	b.min_y   = -length / 2.0;
	b.max_y   =  length / 2.0;
	b.min_z   = 0.0;
	b.max_z   = height;
	b.width_x  = width;
	b.length_y = length;
	b.height_z = height;
	return b;
}

void adjust_bounds_to_fit_grid( BrushData& target, double step_x, double step_y ){
	double new_width  = std::max( step_x, std::round( target.width_x  / step_x ) * step_x );
	double new_length = std::max( step_y, std::round( target.length_y / step_y ) * step_y );

	if ( std::abs( target.width_x  - new_width  ) > 0.001 ||
	     std::abs( target.length_y - new_length ) > 0.001 ) {
		double diff_x = new_width  - target.width_x;
		double diff_y = new_length - target.length_y;

		target.min_x   = std::round( target.min_x - diff_x / 2.0 );
		target.max_x   = target.min_x + new_width;
		target.min_y   = std::round( target.min_y - diff_y / 2.0 );
		target.max_y   = target.min_y + new_length;
		target.width_x  = new_width;
		target.length_y = new_length;
	}
}

static int clamp_iteration_count( int requested, std::size_t cell_count ){
	if ( requested <= 0 ) {
		return 0;
	}
	int max_iter = 6;
	if ( cell_count <= 4096 ) {
		max_iter = 24;
	}
	else if ( cell_count <= 16384 ) {
		max_iter = 16;
	}
	else if ( cell_count <= 65536 ) {
		max_iter = 10;
	}
	return std::min( requested, max_iter );
}

static std::size_t grid_index( int ix, int iy, int width ){
	return static_cast<std::size_t>( iy ) * static_cast<std::size_t>( width ) + static_cast<std::size_t>( ix );
}

static void quantize_terraces( std::vector<double>& values, double terrace_step ){
	if ( terrace_step <= 0.0 ) {
		return;
	}
	for ( double& v : values ) {
		v = std::floor( v / terrace_step ) * terrace_step;
	}
}

static bool same_terrace_band( double a, double b, double terrace_step ){
	if ( terrace_step <= 0.0 ) {
		return true;
	}
	const double ia = std::floor( a / terrace_step );
	const double ib = std::floor( b / terrace_step );
	return ia == ib;
}

static double laplacian_pass( std::vector<double>& values, int width, int height, double terrace_step ){
	std::vector<double> next = values;
	double max_delta = 0.0;
	for ( int iy = 0; iy < height; ++iy ) {
		for ( int ix = 0; ix < width; ++ix ) {
			const std::size_t idx = grid_index( ix, iy, width );
			const double center = values[idx];
			double sum = center;
			int count = 1;
			const int offsets[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
			for ( const auto& off : offsets ) {
				const int nx = ix + off[0];
				const int ny = iy + off[1];
				if ( nx < 0 || nx >= width || ny < 0 || ny >= height ) {
					continue;
				}
				const double neighbor = values[grid_index( nx, ny, width )];
				if ( same_terrace_band( center, neighbor, terrace_step ) ) {
					sum += neighbor;
					++count;
				}
			}
			const double blended = center * 0.6 + ( sum / static_cast<double>( count ) ) * 0.4;
			next[idx] = blended;
			max_delta = std::max( max_delta, std::abs( blended - center ) );
		}
	}
	values.swap( next );
	quantize_terraces( values, terrace_step );
	return max_delta;
}

static double thermal_pass( std::vector<double>& values, int width, int height, double terrace_step ){
	std::vector<double> delta( values.size(), 0.0 );
	double max_transfer = 0.0;
	for ( int iy = 0; iy < height; ++iy ) {
		for ( int ix = 0; ix < width; ++ix ) {
			const std::size_t idx = grid_index( ix, iy, width );
			const double center = values[idx];
			const int offsets[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
			for ( const auto& off : offsets ) {
				const int nx = ix + off[0];
				const int ny = iy + off[1];
				if ( nx < 0 || nx >= width || ny < 0 || ny >= height ) {
					continue;
				}
				const std::size_t nidx = grid_index( nx, ny, width );
				if ( idx >= nidx ) {
					continue;
				}
				const double neighbor = values[nidx];
				if ( !same_terrace_band( center, neighbor, terrace_step ) ) {
					continue;
				}
				const double diff = center - neighbor;
				const double threshold = 3.0;
				if ( std::abs( diff ) <= threshold ) {
					continue;
				}
				const double moved = std::min( std::abs( diff ) - threshold, 2.0 ) * 0.2;
				if ( diff > 0.0 ) {
					delta[idx] -= moved;
					delta[nidx] += moved;
				}
				else {
					delta[idx] += moved;
					delta[nidx] -= moved;
				}
				max_transfer = std::max( max_transfer, moved );
			}
		}
	}
	for ( std::size_t i = 0; i < values.size(); ++i ) {
		values[i] += delta[i];
	}
	quantize_terraces( values, terrace_step );
	return max_transfer;
}

static double hydraulic_pass( std::vector<double>& values, int width, int height, double terrace_step, TerrainRng& rng ){
	std::vector<double> delta( values.size(), 0.0 );
	double max_transfer = 0.0;
	for ( int iy = 1; iy < height - 1; ++iy ) {
		for ( int ix = 1; ix < width - 1; ++ix ) {
			const std::size_t idx = grid_index( ix, iy, width );
			const double center = values[idx];
			int best_x = ix;
			int best_y = iy;
			double best_drop = 0.0;
			for ( int oy = -1; oy <= 1; ++oy ) {
				for ( int ox = -1; ox <= 1; ++ox ) {
					if ( ox == 0 && oy == 0 ) {
						continue;
					}
					const int nx = ix + ox;
					const int ny = iy + oy;
					const double neighbor = values[grid_index( nx, ny, width )];
					if ( !same_terrace_band( center, neighbor, terrace_step ) ) {
						continue;
					}
					const double drop = center - neighbor;
					if ( drop > best_drop ) {
						best_drop = drop;
						best_x = nx;
						best_y = ny;
					}
				}
			}
			if ( best_drop <= 0.5 ) {
				continue;
			}
			const std::size_t nidx = grid_index( best_x, best_y, width );
			const double jitter = 0.75 + rng.random_double() * 0.5;
			const double moved = std::min( best_drop * 0.08, 1.2 ) * jitter;
			delta[idx] -= moved;
			delta[nidx] += moved;
			max_transfer = std::max( max_transfer, moved );
		}
	}
	for ( std::size_t i = 0; i < values.size(); ++i ) {
		values[i] += delta[i];
	}
	quantize_terraces( values, terrace_step );
	return max_transfer;
}

// ---------------------------------------------------------------------------
// Standard heightmap
// ---------------------------------------------------------------------------

HeightMap generate_height_map( const BrushData& target, double step_x, double step_y,
                                ShapeType shape_type, double shape_height,
                                double variance, double frequency,
                                const MaskMap& mask_map,
                                NoiseType noise_type, double terrace_step,
                                double curve_radius, double banking_angle_deg,
                                double ramp_length,
                                const PostProcessSettings& post_process,
                                int seed ){
	HeightMap height_map;
	TerrainRng rng( static_cast<std::uint32_t>( seed ) );
	const int width = static_cast<int>( std::round( target.width_x / step_x ) ) + 1;
	const int height = static_cast<int>( std::round( target.length_y / step_y ) ) + 1;
	std::vector<double> grid;
	grid.reserve( static_cast<std::size_t>( width ) * static_cast<std::size_t>( height ) );

	double seed_x = rng.random_double() * 10000.0;
	double seed_y = rng.random_double() * 10000.0;

	for ( double x = target.min_x; x <= target.max_x + 0.01; x += step_x ) {
		for ( double y = target.min_y; y <= target.max_y + 0.01; y += step_y ) {
			double nx = target.width_x  > 0 ? ( x - target.min_x ) / target.width_x  : 0.0;
			double ny = target.length_y > 0 ? ( y - target.min_y ) / target.length_y : 0.0;

			double center_dist = std::min( 1.0, std::sqrt(
				( nx - 0.5 ) * ( nx - 0.5 ) + ( ny - 0.5 ) * ( ny - 0.5 ) ) / 0.5 );

			double base_z = 0.0;
			switch ( shape_type ) {
			case ShapeType::Hill: {
				base_z = shape_height * 0.5 * ( 1.0 + std::cos( center_dist * std::numbers::pi ) );
				break;
			}
			case ShapeType::Crater: {
				base_z = shape_height * 0.5 * ( 1.0 - std::cos( center_dist * std::numbers::pi ) );
				break;
			}
			case ShapeType::Ridge: {
				double dist = std::min( 1.0, std::abs( nx - 0.5 ) / 0.5 );
				base_z = shape_height * 0.5 * ( 1.0 + std::cos( dist * std::numbers::pi ) );
				break;
			}
			case ShapeType::Slope: {
				base_z = shape_height * nx;
				break;
			}
			case ShapeType::Volcano: {
				double mountain   = shape_height * 0.5 * ( 1.0 + std::cos( center_dist * std::numbers::pi ) );
				double crater_dist = std::min( 1.0, center_dist / 0.35 );
				double crater     = ( shape_height * 0.7 ) * 0.5 * ( 1.0 + std::cos( crater_dist * std::numbers::pi ) );
				base_z = mountain - crater;
				break;
			}
			case ShapeType::Valley: {
				double dist = std::min( 1.0, std::abs( nx - 0.5 ) / 0.5 );
				base_z = shape_height * 0.5 * ( 1.0 - std::cos( dist * std::numbers::pi ) );
				break;
			}
			case ShapeType::BankedTurn: {
				const double safe_radius = std::max( curve_radius, step_x * 2.0 );
				const double turn_center_x = target.min_x + safe_radius;
				const double turn_center_y = target.min_y + safe_radius;
				const double dx = x - turn_center_x;
				const double dy = y - turn_center_y;
				const double r = std::sqrt( dx * dx + dy * dy );
				const double lane_half = std::max( 64.0, target.width_x * 0.30 );
				const double radial = std::clamp( ( r - safe_radius ) / lane_half, -1.0, 1.0 );
				const double bank_angle_rad = std::clamp( banking_angle_deg, 0.0, 45.0 ) * std::numbers::pi / 180.0;
				const double bank_z = radial * std::tan( bank_angle_rad ) * lane_half;
				const double lane_shape = std::max( 0.0, 1.0 - std::abs( radial ) );
				base_z = shape_height * lane_shape + bank_z;
				break;
			}
			case ShapeType::Berm: {
				const double berm_center = 0.82;
				const double berm_half_width = 0.20;
				const double dist = std::min( 1.0, std::abs( nx - berm_center ) / berm_half_width );
				const double along = std::sin( ny * std::numbers::pi );
				base_z = shape_height * 0.5 * ( 1.0 + std::cos( dist * std::numbers::pi ) ) * along;
				break;
			}
			case ShapeType::JumpRamp: {
				const double safe_ramp_len = std::max( ramp_length, step_x * 2.0 );
				const double t = std::clamp( ( x - target.min_x ) / safe_ramp_len, 0.0, 1.0 );
				const double eased = t * t * ( 3.0 - 2.0 * t );
				base_z = shape_height * eased;
				break;
			}
			case ShapeType::Whoops: {
				const double safe_spacing = std::max( ramp_length, step_y * 2.0 );
				const double wave = std::sin( ( y - target.min_y ) * ( 2.0 * std::numbers::pi / safe_spacing ) );
				const double lane_mask = std::sin( nx * std::numbers::pi );
				base_z = shape_height * 0.5 * ( wave + 1.0 ) * lane_mask;
				break;
			}
			case ShapeType::SCurve: {
				// Banking reverses halfway along Y — left side up in first half,
				// right side up in second half. Smooth crossover at ny = 0.5.
				const double lateral = nx - 0.5;
				base_z = shape_height * lateral * -std::cos( ny * std::numbers::pi );
				break;
			}
			case ShapeType::Hairpin: {
				// Tight 180° turn: apex at top-centre of the brush, outer edge banked up.
				const double turn_cx = ( target.min_x + target.max_x ) * 0.5;
				const double turn_cy = target.max_y - target.width_x * 0.3;
				const double dx = x - turn_cx;
				const double dy = y - turn_cy;
				const double r = std::sqrt( dx * dx + dy * dy );
				const double inner_r = target.width_x * 0.15;
				const double outer_r = target.width_x * 0.55;
				// 0 at inner edge → 1 at outer edge
				const double radial = std::clamp( ( r - inner_r ) / std::max( outer_r - inner_r, 1.0 ), 0.0, 1.0 );
				// Blend banking in gradually as the track approaches the hairpin apex
				const double blend = std::clamp( ( ny - 0.2 ) / 0.5, 0.0, 1.0 );
				base_z = shape_height * radial * blend;
				break;
			}
			case ShapeType::OffCamber: {
				// Like BankedTurn but the surface tilts away from the turn centre —
				// the outside drops instead of rising (technical, challenging).
				const double safe_radius = std::max( curve_radius, step_x * 2.0 );
				const double turn_center_x = target.min_x + safe_radius;
				const double turn_center_y = target.min_y + safe_radius;
				const double dx = x - turn_center_x;
				const double dy = y - turn_center_y;
				const double r = std::sqrt( dx * dx + dy * dy );
				const double lane_half = std::max( 64.0, target.width_x * 0.30 );
				const double radial = std::clamp( ( r - safe_radius ) / lane_half, -1.0, 1.0 );
				const double bank_angle_rad = std::clamp( banking_angle_deg, 0.0, 45.0 ) * std::numbers::pi / 180.0;
				const double bank_z = -radial * std::tan( bank_angle_rad ) * lane_half;
				const double lane_shape = std::max( 0.0, 1.0 - std::abs( radial ) );
				base_z = shape_height * lane_shape + bank_z;
				break;
			}
			default:
				break;
			}

			double noise_z = 0.0;
			if ( variance > 0.0 ) {
				const double mask_weight = sample_mask_weight( mask_map, x, y );
				const double local_variance = variance * mask_weight;
				const double local_frequency = frequency * mask_weight;
				if ( noise_type == NoiseType::Random ) {
					noise_z = ( rng.random_double() * ( local_variance * 2.0 ) ) - local_variance;
				} else {
					noise_z = sample_noise( noise_type,
					                        ( x + seed_x ) * local_frequency,
					                        ( y + seed_y ) * local_frequency, rng ) * local_variance;
				}
			}

			double final_z = target.max_z + base_z + noise_z;
			if ( shape_type != ShapeType::Flat && terrace_step > 0.0 ) {
				final_z = std::floor( final_z / terrace_step ) * terrace_step;
			}
			grid.push_back( final_z );
		}
	}

	if ( !grid.empty() ) {
		const std::size_t cell_count = grid.size();
		const int lap_iters = clamp_iteration_count( post_process.laplacian_iterations, cell_count );
		for ( int i = 0; i < lap_iters; ++i ) {
			if ( laplacian_pass( grid, width, height, terrace_step ) < 0.05 ) {
				break;
			}
		}

		const int thermal_iters = clamp_iteration_count( post_process.thermal_iterations, cell_count );
		for ( int i = 0; i < thermal_iters; ++i ) {
			if ( thermal_pass( grid, width, height, terrace_step ) < 0.02 ) {
				break;
			}
		}

		TerrainRng hydraulic_rng( mix_seed( static_cast<std::uint32_t>( seed ), 0x68796472U ) );
		const int hydraulic_iters = clamp_iteration_count( post_process.hydraulic_iterations, cell_count );
		for ( int i = 0; i < hydraulic_iters; ++i ) {
			if ( hydraulic_pass( grid, width, height, terrace_step, hydraulic_rng ) < 0.02 ) {
				break;
			}
		}
	}

	std::size_t index = 0;
	for ( double x = target.min_x; x <= target.max_x + 0.01; x += step_x ) {
		for ( double y = target.min_y; y <= target.max_y + 0.01; y += step_y ) {
			height_map[{ round2( x ), round2( y ) }] = std::round( grid[index++] );
		}
	}

	return height_map;
}

// ---------------------------------------------------------------------------
// Track section heightmap
// ---------------------------------------------------------------------------

static double smoothstep01( double v ){
	const double t = std::clamp( v, 0.0, 1.0 );
	return t * t * ( 3.0 - 2.0 * t );
}

static double normalize_angle_positive( double angle ){
	while ( angle < 0.0 ) {
		angle += 2.0 * std::numbers::pi;
	}
	while ( angle >= 2.0 * std::numbers::pi ) {
		angle -= 2.0 * std::numbers::pi;
	}
	return angle;
}

static double signed_arc_distance( const BrushData& target, double x, double y,
                                   double radius, double arc_degrees, bool left,
                                   double& progress ){
	const double safe_radius = std::max( radius, 64.0 );
	const double arc_rad = std::clamp( arc_degrees, 15.0, 180.0 ) * std::numbers::pi / 180.0;
	const double start_x = ( target.min_x + target.max_x ) * 0.5;
	const double start_y = target.min_y;
	const double center_x = start_x + ( left ? -safe_radius : safe_radius );
	const double center_y = start_y;
	const double start_angle = left ? 0.0 : std::numbers::pi;
	const double point_angle = std::atan2( y - center_y, x - center_x );
	const double raw_delta = left
	                       ? normalize_angle_positive( point_angle - start_angle )
	                       : normalize_angle_positive( start_angle - point_angle );
	const bool before_start = raw_delta > std::numbers::pi;
	const double arc_delta = before_start ? 0.0 : raw_delta;
	const double clamped_delta = std::clamp( arc_delta, 0.0, arc_rad );
	const double centerline_angle = left ? start_angle + clamped_delta : start_angle - clamped_delta;
	const double cx = center_x + safe_radius * std::cos( centerline_angle );
	const double cy = center_y + safe_radius * std::sin( centerline_angle );
	progress = arc_rad > 0.0 ? clamped_delta / arc_rad : 0.0;

	if ( !before_start && raw_delta <= arc_rad ) {
		return std::sqrt( ( x - center_x ) * ( x - center_x ) + ( y - center_y ) * ( y - center_y ) ) - safe_radius;
	}
	return std::sqrt( ( x - cx ) * ( x - cx ) + ( y - cy ) * ( y - cy ) );
}

static double signed_track_distance( const BrushData& target, TrackSectionType section_type,
                                     double x, double y, double half_track, double half_shoulder,
                                     double curve_radius, double curve_arc_degrees,
                                     double& progress ){
	const double ny = target.length_y > 0.0 ? ( y - target.min_y ) / target.length_y : 0.0;
	const double center_x = ( target.min_x + target.max_x ) * 0.5;
	progress = ny;

	switch ( section_type ) {
	case TrackSectionType::CurveLeft:
		return signed_arc_distance( target, x, y, curve_radius, curve_arc_degrees, true, progress );
	case TrackSectionType::CurveRight:
		return signed_arc_distance( target, x, y, curve_radius, curve_arc_degrees, false, progress );
	case TrackSectionType::BankedTurn: {
		const double safe_radius = std::max( curve_radius, half_track + half_shoulder + 64.0 );
		return signed_arc_distance( target, x, y, safe_radius, curve_arc_degrees, true, progress );
	}
	case TrackSectionType::SCurve: {
		const double amplitude = std::max( 0.0, target.width_x * 0.5 - half_track - half_shoulder );
		const double curve_center = center_x + amplitude * std::sin( ( ny - 0.5 ) * 2.0 * std::numbers::pi );
		return x - curve_center;
	}
	case TrackSectionType::Hairpin: {
		const double radius = std::max( half_track + half_shoulder + 64.0, curve_radius );
		const double cx = center_x;
		const double cy = target.max_y - radius;
		const double dx = x - cx;
		const double dy = y - cy;
		const double r = std::sqrt( dx * dx + dy * dy );
		const double angle = std::atan2( dy, dx );
		progress = std::clamp( ( angle + std::numbers::pi ) / std::numbers::pi, 0.0, 1.0 );
		return r - radius;
	}
	default:
		return x - center_x;
	}
}

static double track_feature_height( TrackSectionType section_type, double progress,
                                    double y, const BrushData& target,
                                    double feature_height, double feature_length ){
	switch ( section_type ) {
	case TrackSectionType::Jump: {
		const double safe_len = std::max( feature_length, 64.0 );
		const double distance = y - target.min_y;
		const double launch = smoothstep01( std::clamp( distance / safe_len, 0.0, 1.0 ) );
		const double landing = 1.0 - smoothstep01( std::clamp( ( distance - safe_len ) / safe_len, 0.0, 1.0 ) );
		return feature_height * std::min( launch, landing );
	}
	case TrackSectionType::Whoops: {
		const double safe_spacing = std::max( feature_length, 64.0 );
		const double wave = std::sin( ( y - target.min_y ) * ( 2.0 * std::numbers::pi / safe_spacing ) );
		return feature_height * 0.5 * ( wave + 1.0 );
	}
	case TrackSectionType::Hairpin:
		return feature_height * 0.2 * smoothstep01( progress );
	default:
		return 0.0;
	}
}

TrackSectionMaps generate_track_section_maps( const BrushData& target, double step_x, double step_y,
                                              const TrackSectionOptions& track_options,
                                              double variance, double frequency,
                                              const MaskMap& mask_map,
                                              NoiseType noise_type, double terrace_step,
                                              const PostProcessSettings& post_process,
                                              int seed ){
	TrackSectionMaps result;
	TerrainRng rng( static_cast<std::uint32_t>( seed ) );
	const int width = static_cast<int>( std::round( target.width_x / step_x ) ) + 1;
	const int height = static_cast<int>( std::round( target.length_y / step_y ) ) + 1;
	std::vector<double> grid;
	grid.reserve( static_cast<std::size_t>( width ) * static_cast<std::size_t>( height ) );

	const double half_track = std::max( step_x, track_options.track_width * 0.5 );
	const double shoulder = std::max( 0.0, track_options.shoulder_width );
	const double half_total = half_track + shoulder;
	const double bank_angle_rad = std::clamp( track_options.banking_angle_deg, -45.0, 45.0 ) * std::numbers::pi / 180.0;
	double seed_x = rng.random_double() * 10000.0;
	double seed_y = rng.random_double() * 10000.0;

	for ( double x = target.min_x; x <= target.max_x + 0.01; x += step_x ) {
		for ( double y = target.min_y; y <= target.max_y + 0.01; y += step_y ) {
			double progress = 0.0;
			const double signed_dist = signed_track_distance( target, track_options.type, x, y, half_track, shoulder,
			                                                  track_options.curve_radius, track_options.curve_arc_degrees, progress );
			const double abs_dist = std::abs( signed_dist );
			const bool on_track = abs_dist <= half_track;
			const bool on_shoulder = !on_track && abs_dist <= half_total;

			SurfaceKind surface = SurfaceKind::Terrain;
			if ( on_track ) {
				surface = SurfaceKind::Track;
			}
			else if ( on_shoulder ) {
				surface = SurfaceKind::Shoulder;
			}

			double lane_weight = 0.0;
			if ( abs_dist < half_total ) {
				lane_weight = 1.0 - smoothstep01( std::clamp( ( abs_dist - half_track ) / std::max( shoulder, 1.0 ), 0.0, 1.0 ) );
			}

			const double track_profile = track_feature_height( track_options.type, progress, y, target,
			                                                  track_options.feature_height, track_options.feature_length );
			double bank_z = 0.0;
			if ( track_options.type == TrackSectionType::CurveLeft
			  || track_options.type == TrackSectionType::CurveRight
			  || track_options.type == TrackSectionType::BankedTurn
			  || track_options.type == TrackSectionType::SCurve
			  || track_options.type == TrackSectionType::Hairpin ) {
				double direction = 1.0;
				if ( track_options.type == TrackSectionType::SCurve ) {
					direction = -std::cos( progress * std::numbers::pi * 2.0 );
				}
				bank_z = signed_dist / std::max( half_track, 1.0 ) * std::tan( bank_angle_rad ) * half_track * direction;
			}

			const double berm_center = half_track + shoulder * 0.45;
			const double berm_width = std::max( shoulder * 0.55, step_x );
			const double berm_dist = std::abs( abs_dist - berm_center ) / berm_width;
			const double berm_z = track_options.berm_height * std::max( 0.0, 1.0 - smoothstep01( berm_dist ) );

			double noise_z = 0.0;
			if ( variance > 0.0 ) {
				const double mask_weight = sample_mask_weight( mask_map, x, y );
				const double surface_weight = track_options.smooth_track
				                            ? ( on_track ? 0.0 : ( on_shoulder ? 0.35 : 1.0 ) )
				                            : ( on_track ? 0.25 : ( on_shoulder ? 0.55 : 1.0 ) );
				const double local_variance = variance * mask_weight * surface_weight;
				const double local_frequency = frequency * std::max( mask_weight, 0.1 );
				if ( noise_type == NoiseType::Random ) {
					noise_z = ( rng.random_double() * ( local_variance * 2.0 ) ) - local_variance;
				}
				else {
					noise_z = sample_noise( noise_type,
					                        ( x + seed_x ) * local_frequency,
					                        ( y + seed_y ) * local_frequency, rng ) * local_variance;
				}
			}

			double final_z = target.max_z + track_profile * lane_weight + bank_z * lane_weight + berm_z + noise_z;
			if ( terrace_step > 0.0 && !on_track ) {
				final_z = std::floor( final_z / terrace_step ) * terrace_step;
			}

			grid.push_back( final_z );
			result.surface_map[{ round2( x ), round2( y ) }] = surface;
		}
	}

	if ( !track_options.smooth_track && !grid.empty() ) {
		const std::size_t cell_count = grid.size();
		const int lap_iters = clamp_iteration_count( post_process.laplacian_iterations, cell_count );
		for ( int i = 0; i < lap_iters; ++i ) {
			if ( laplacian_pass( grid, width, height, terrace_step ) < 0.05 ) {
				break;
			}
		}

		const int thermal_iters = clamp_iteration_count( post_process.thermal_iterations, cell_count );
		for ( int i = 0; i < thermal_iters; ++i ) {
			if ( thermal_pass( grid, width, height, terrace_step ) < 0.02 ) {
				break;
			}
		}

		TerrainRng hydraulic_rng( mix_seed( static_cast<std::uint32_t>( seed ), 0x74726163U ) );
		const int hydraulic_iters = clamp_iteration_count( post_process.hydraulic_iterations, cell_count );
		for ( int i = 0; i < hydraulic_iters; ++i ) {
			if ( hydraulic_pass( grid, width, height, terrace_step, hydraulic_rng ) < 0.02 ) {
				break;
			}
		}
	}

	std::size_t index = 0;
	for ( double x = target.min_x; x <= target.max_x + 0.01; x += step_x ) {
		for ( double y = target.min_y; y <= target.max_y + 0.01; y += step_y ) {
			result.height_map[{ round2( x ), round2( y ) }] = std::round( grid[index++] );
		}
	}

	return result;
}

// ---------------------------------------------------------------------------
// Tunnel heightmaps
// ---------------------------------------------------------------------------

TunnelMaps generate_tunnel_height_maps( const BrushData& target, double step_x, double step_y,
                                        double cave_height, double slope_height,
                                        double variance, double frequency,
                                        const MaskMap& mask_map,
                                        NoiseType noise_type, double terrace_step,
                                        int seed ){
	TunnelMaps result;
	const std::uint32_t base_seed = static_cast<std::uint32_t>( seed );
	TerrainRng floor_rng( mix_seed( base_seed, 0x1f123bb5U ) );
	TerrainRng ceil_rng(  mix_seed( base_seed, 0xa8b7c421U ) );
	TerrainRng wall_l_rng( mix_seed( base_seed, 0x736f6c4cU ) );
	TerrainRng wall_r_rng( mix_seed( base_seed, 0x736f6c52U ) );

	double seed_floor_x = floor_rng.random_double() * 10000.0;
	double seed_floor_y = floor_rng.random_double() * 10000.0;
	double seed_ceil_x  = ceil_rng.random_double() * 10000.0;
	double seed_ceil_y  = ceil_rng.random_double() * 10000.0;
	double seed_wall_l  = wall_l_rng.random_double() * 10000.0;
	double seed_wall_r  = wall_r_rng.random_double() * 10000.0;

	double center_x  = ( target.min_x + target.max_x ) / 2.0;
	double half_width = target.width_x / 2.0;

	// Floor and ceiling
	for ( double x = target.min_x; x <= target.max_x + 0.01; x += step_x ) {
		for ( double y = target.min_y; y <= target.max_y + 0.01; y += step_y ) {
			double t = half_width > 0 ? std::min( 1.0, std::abs( x - center_x ) / half_width ) : 0.0;
			double blend = 1.0 - std::sqrt( std::max( 0.0, 1.0 - t * t ) );

			double floor_noise = 0.0, ceil_noise = 0.0;
			if ( variance > 0.0 ) {
				const double mask_weight = sample_mask_weight( mask_map, x, y );
				const double local_variance = variance * mask_weight;
				const double local_frequency = frequency * mask_weight;
				if ( noise_type == NoiseType::Random ) {
					floor_noise = floor_rng.random_double() * local_variance;
					ceil_noise  = ceil_rng.random_double() * local_variance;
				} else {
					floor_noise = std::abs( sample_noise( noise_type,
					    ( x + seed_floor_x ) * local_frequency, ( y + seed_floor_y ) * local_frequency, floor_rng ) ) * local_variance;
					ceil_noise  = std::abs( sample_noise( noise_type,
					    ( x + seed_ceil_x  ) * local_frequency, ( y + seed_ceil_y  ) * local_frequency, ceil_rng ) ) * local_variance;
				}
			}

			double ny    = target.length_y > 0 ? ( y - target.min_y ) / target.length_y : 0.0;
			double base_z = target.max_z + slope_height * ny;
			double floor_z = base_z + blend * ( cave_height * 0.25 ) + floor_noise;
			double ceil_z  = base_z + cave_height - blend * ( cave_height * 0.25 ) - ceil_noise;

			if ( floor_z > ceil_z ) {
				double mid = ( floor_z + ceil_z ) / 2.0;
				floor_z = mid;
				ceil_z  = mid;
			}

			if ( terrace_step > 0.0 ) {
				floor_z = std::floor( floor_z / terrace_step ) * terrace_step;
				ceil_z  = std::ceil(  ceil_z  / terrace_step ) * terrace_step;
			}

			result.floor_map[   { round2( x ), round2( y ) }] = std::round( floor_z );
			result.ceiling_map[ { round2( x ), round2( y ) }] = std::round( ceil_z  );
		}
	}

	// Wall step in Z — walls must cover the full slope range regardless of direction.
	// slope_height may be negative (downward slope), so use abs for the span.
	double total_wall_height = cave_height + std::abs( slope_height );
	int    num_z_steps = std::max( 1, (int)std::round( total_wall_height / step_x ) );
	double step_z      = total_wall_height / num_z_steps;
	double wall_min_z  = target.max_z + std::min( 0.0, slope_height );
	double wall_max_z  = wall_min_z + total_wall_height;
	result.step_z      = step_z;

	// Left and right walls — grid over (Y, Z)
	for ( double y = target.min_y; y <= target.max_y + 0.01; y += step_y ) {
		for ( double z = wall_min_z; z <= wall_max_z + 0.01; z += step_z ) {
			double ry = round2( y );
			double rz = round2( z );

			double wall_noise = 0.0;
			if ( variance > 0.0 ) {
				const double left_mask_weight = sample_mask_weight( mask_map, target.min_x, y );
				const double left_local_variance = variance * left_mask_weight;
				const double left_local_frequency = frequency * left_mask_weight;
				if ( noise_type == NoiseType::Random ) {
					wall_noise = wall_l_rng.random_double() * left_local_variance;
				} else {
					wall_noise = std::abs( sample_noise( noise_type,
					    ( y + seed_wall_l ) * left_local_frequency, ( z + seed_wall_l ) * left_local_frequency, wall_l_rng ) ) * left_local_variance;
				}
			}
			result.left_wall_map[{ ry, rz }] = std::round( target.min_x + wall_noise );

			wall_noise = 0.0;
			if ( variance > 0.0 ) {
				const double right_mask_weight = sample_mask_weight( mask_map, target.max_x, y );
				const double right_local_variance = variance * right_mask_weight;
				const double right_local_frequency = frequency * right_mask_weight;
				if ( noise_type == NoiseType::Random ) {
					wall_noise = wall_r_rng.random_double() * right_local_variance;
				} else {
					wall_noise = std::abs( sample_noise( noise_type,
					    ( y + seed_wall_r ) * right_local_frequency, ( z + seed_wall_r ) * right_local_frequency, wall_r_rng ) ) * right_local_variance;
				}
			}
			result.right_wall_map[{ ry, rz }] = std::round( target.max_x - wall_noise );
		}
	}

	return result;
}
