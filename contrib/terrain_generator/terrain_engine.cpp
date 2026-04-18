#include "terrain_engine.h"
#include "noise.h"

#include <cmath>
#include <algorithm>
#include <numbers>
#include <random>
#include <cstdint>

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
                                int seed ){
	HeightMap height_map;
	TerrainRng rng( static_cast<std::uint32_t>( seed ) );

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
			if ( shape_type != ShapeType::Flat && terrace_step > 0.0 )
				final_z = std::floor( final_z / terrace_step ) * terrace_step;

			height_map[{ round2( x ), round2( y ) }] = std::round( final_z );
		}
	}

	return height_map;
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
