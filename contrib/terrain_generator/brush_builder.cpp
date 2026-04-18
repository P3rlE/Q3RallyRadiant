#include "brush_builder.h"

#include "debugging/debugging.h"
#include "ibrush.h"
#include "ientity.h"
#include "ieclass.h"
#include "iscenegraph.h"
#include "iundo.h"
#include "qerplugin.h"
#include "scenelib.h"
#include <memory>
#include <cmath>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void fill_face( _QERFaceData& face,
                        double x0, double y0, double z0,
                        double x1, double y1, double z1,
                        double x2, double y2, double z2,
                        const char* shader ){
	face.m_p0[0] = x0; face.m_p0[1] = y0; face.m_p0[2] = z0;
	face.m_p1[0] = x1; face.m_p1[1] = y1; face.m_p1[2] = z1;
	face.m_p2[0] = x2; face.m_p2[1] = y2; face.m_p2[2] = z2;
	face.m_shader   = shader;
	face.m_texdef.scale[0] = 0.03125f;  // matches ( ( 0.03125 0 0 ) ( 0 0.03125 0 ) ) in .map
	face.m_texdef.scale[1] = 0.03125f;
	face.m_texdef.shift[0] = 0;
	face.m_texdef.shift[1] = 0;
	face.m_texdef.rotate   = 0;
	face.contents = 0;
	face.flags    = 0;
	face.value    = 0;
}

class UndoScope
{
	std::unique_ptr<UndoableCommand> m_command;
public:
	UndoScope( const char* command, bool enabled ){
		if ( enabled ) {
			m_command = std::make_unique<UndoableCommand>( command );
		}
	}
};

static scene::Node& create_func_group( const TerrainBuildOptions& options ){
	EntityClass* ec = GlobalEntityClassManager().findOrInsert( "func_group", true );
	NodeSmartReference entity( GlobalEntityCreator().createEntity( ec ) );
	Node_getTraversable( GlobalSceneGraph().root() )->insert( entity );
	if ( options.preview ) {
		if ( Entity* e = Node_getEntity( entity ) ) {
			e->setKeyValue( "_terrain_generator_preview", "1" );
			e->setKeyValue( "name", "terrain_generator_preview" );
		}
	}
	return entity;
}

static void insert_brush_into( scene::Node& entity,
                                double x,  double y,  double min_z,
                                double mx, double my, double base_max_z,
                                double z_bl, double z_tl, double z_br, double z_tr,
                                const char* top_tex_a, const char* top_tex_b, const char* caulk,
                                bool split_diagonally, bool alt_dir ){
	if ( !split_diagonally ) {
		NodeSmartReference brush( GlobalBrushCreator().createBrush() );
		_QERFaceData face;
		fill_face( face, x,  y,  base_max_z,   x,  my, base_max_z,   mx, y,  base_max_z,   top_tex_a );
		GlobalBrushCreator().Brush_addFace( brush, face );
		fill_face( face, x,  y,  min_z,        mx, y,  min_z,        x,  my, min_z,         caulk );
		GlobalBrushCreator().Brush_addFace( brush, face );
		fill_face( face, mx, y,  min_z,        mx, y,  base_max_z,   mx, my, min_z,         caulk );
		GlobalBrushCreator().Brush_addFace( brush, face );
		fill_face( face, x,  y,  min_z,        x,  my, min_z,        x,  y,  base_max_z,    caulk );
		GlobalBrushCreator().Brush_addFace( brush, face );
		fill_face( face, x,  my, min_z,        mx, my, min_z,        x,  my, base_max_z,    caulk );
		GlobalBrushCreator().Brush_addFace( brush, face );
		fill_face( face, x,  y,  min_z,        x,  y,  base_max_z,   mx, y,  min_z,         caulk );
		GlobalBrushCreator().Brush_addFace( brush, face );
		Node_getTraversable( entity )->insert( brush );
	}
	else if ( !alt_dir ) {
		// Standard diagonal: BL→TR split
		// Triangle 1: BL, TL, BR
		{
			NodeSmartReference brush( GlobalBrushCreator().createBrush() );
			_QERFaceData face;
			fill_face( face, x,  y,  z_bl,   x,  my, z_tl,   mx, y,  z_br,   top_tex_a );
			GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, x,  y,  min_z,  mx, y,  min_z,  x,  my, min_z,   caulk );
			GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, x,  y,  min_z,  x,  my, min_z,  x,  y,  base_max_z, caulk );
			GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, x,  y,  min_z,  x,  y,  base_max_z, mx, y, min_z, caulk );
			GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, mx, y,  min_z,  mx, y,  base_max_z, x,  my, min_z, caulk );
			GlobalBrushCreator().Brush_addFace( brush, face );
			Node_getTraversable( entity )->insert( brush );
		}
		// Triangle 2: TR, BR, TL
		{
			NodeSmartReference brush( GlobalBrushCreator().createBrush() );
			_QERFaceData face;
			fill_face( face, mx, my, z_tr,   mx, y,  z_br,   x,  my, z_tl,   top_tex_b );
			GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, mx, my, min_z,  x,  my, min_z,  mx, y,  min_z,   caulk );
			GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, mx, y,  min_z,  mx, y,  base_max_z, mx, my, min_z, caulk );
			GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, x,  my, min_z,  mx, my, min_z,  x,  my, base_max_z, caulk );
			GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, x,  my, min_z,  x,  my, base_max_z, mx, y, min_z, caulk );
			GlobalBrushCreator().Brush_addFace( brush, face );
			Node_getTraversable( entity )->insert( brush );
		}
	}
	else {
		// Alternate diagonal: TL→BR split (checkerboard pattern)
		// Triangle 1: TL, TR, BL
		{
			NodeSmartReference brush( GlobalBrushCreator().createBrush() );
			_QERFaceData face;
			fill_face( face, x,  my, z_tl,   mx, my, z_tr,   x,  y,  z_bl,   top_tex_a );
			GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, x,  my, min_z,  x,  y,  min_z,  mx, my, min_z,   caulk );
			GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, x,  y,  min_z,  x,  my, min_z,  x,  y,  base_max_z, caulk );
			GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, x,  my, min_z,  mx, my, min_z,  x,  my, base_max_z, caulk );
			GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, mx, my, min_z,  x,  y,  min_z,  x,  y,  base_max_z, caulk );
			GlobalBrushCreator().Brush_addFace( brush, face );
			Node_getTraversable( entity )->insert( brush );
		}
		// Triangle 2: TR, BR, BL
		{
			NodeSmartReference brush( GlobalBrushCreator().createBrush() );
			_QERFaceData face;
			fill_face( face, mx, my, z_tr,   mx, y,  z_br,   x,  y,  z_bl,   top_tex_b );
			GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, mx, my, min_z,  x,  y,  min_z,  mx, y,  min_z,   caulk );
			GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, mx, y,  min_z,  mx, y,  base_max_z, mx, my, min_z, caulk );
			GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, x,  y,  min_z,  x,  y,  base_max_z, mx, y,  min_z, caulk );
			GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, x,  y,  min_z,  mx, my, min_z,  x,  y,  base_max_z, caulk );
			GlobalBrushCreator().Brush_addFace( brush, face );
			Node_getTraversable( entity )->insert( brush );
		}
	}
}

static double clamp01( double v ){
	return std::max( 0.0, std::min( 1.0, v ) );
}

static double triangle_slope_deg( double x0, double y0, double z0,
                                  double x1, double y1, double z1,
                                  double x2, double y2, double z2 ){
	const double ux = x1 - x0, uy = y1 - y0, uz = z1 - z0;
	const double vx = x2 - x0, vy = y2 - y0, vz = z2 - z0;
	double nx = uy * vz - uz * vy;
	double ny = uz * vx - ux * vz;
	double nz = ux * vy - uy * vx;
	const double len = std::sqrt( nx * nx + ny * ny + nz * nz );
	if ( len < 1e-9 ) {
		return 0.0;
	}
	nx /= len;
	ny /= len;
	nz /= len;
	return std::acos( clamp01( std::abs( nz ) ) ) * 180.0 / 3.14159265358979323846;
}

static const char* choose_material( const TerrainMaterialSlots& slots, const TerrainMaterialRules& rules,
                                    double avg_height_percent, double slope_deg ){
	const bool has_steep = slots.steep != nullptr && slots.steep[0] != '\0';
	const bool has_peak  = slots.peak  != nullptr && slots.peak[0]  != '\0';
	const bool has_dirt  = slots.dirt  != nullptr && slots.dirt[0]  != '\0';
	const bool has_track = slots.track != nullptr && slots.track[0] != '\0';
	const bool multi = has_steep || has_peak || has_dirt || has_track;
	if ( !multi ) {
		return slots.base;
	}

	if ( has_track && avg_height_percent >= rules.track_min_percent && avg_height_percent <= rules.track_max_percent ) {
		return slots.track;
	}
	if ( has_dirt && avg_height_percent >= rules.dirt_min_percent && avg_height_percent <= rules.dirt_max_percent ) {
		return slots.dirt;
	}
	if ( has_peak && avg_height_percent >= rules.peak_min_percent ) {
		return slots.peak;
	}
	if ( has_steep && slope_deg >= rules.steep_angle_deg ) {
		return slots.steep;
	}
	return slots.base;
}

// ---------------------------------------------------------------------------
// Standard terrain
// ---------------------------------------------------------------------------

void build_terrain_brushes( const BrushData& target, double step_x, double step_y,
                             const HeightMap& height_map, const char* top_texture,
                             const TerrainMaterialSlots& material_slots,
                             const TerrainMaterialRules& material_rules,
                             bool split_diagonally, const TerrainBuildOptions& options,
                             std::vector<scene::Node*>* created_entities ){
	UndoScope undo( "terrainGenerator.generateTerrain", options.undoable );

	scene::Node& entity = create_func_group( options );
	if ( created_entities != nullptr ) {
		created_entities->push_back( &entity );
	}

	const char* caulk = "textures/common/caulk";
	TerrainMaterialSlots effective_slots = material_slots;
	if ( effective_slots.base == nullptr || effective_slots.base[0] == '\0' ) {
		effective_slots.base = top_texture;
	}
	double min_z      = target.min_z;
	double base_max_z = target.max_z;

	auto r2 = []( double v ) { return std::round( v * 100.0 ) / 100.0; };

	int x_index = 0;
	for ( double x = target.min_x; x < target.max_x - 0.01; x += step_x, ++x_index ) {
		int y_index = 0;
		for ( double y = target.min_y; y < target.max_y - 0.01; y += step_y, ++y_index ) {
			double mx = x + step_x < target.max_x ? x + step_x : target.max_x;
			double my = y + step_y < target.max_y ? y + step_y : target.max_y;

			auto lookup = [&]( double kx, double ky ) -> double {
				auto it = height_map.find({ r2( kx ), r2( ky ) });
				return it != height_map.end() ? it->second : min_z;
			};

			double z_bl = lookup( x,  y  );
			double z_tl = lookup( x,  my );
			double z_br = lookup( mx, y  );
			double z_tr = lookup( mx, my );

			bool alt_dir = ( ( x_index + y_index ) % 2 ) != 0;
			const double min_h = target.min_z;
			const double max_h = target.max_z;
			const double denom = std::max( 1.0, max_h - min_h );

			const double avg_a = !alt_dir ? ( z_bl + z_tl + z_br ) / 3.0 : ( z_tl + z_tr + z_bl ) / 3.0;
			const double avg_b = !alt_dir ? ( z_tr + z_br + z_tl ) / 3.0 : ( z_tr + z_br + z_bl ) / 3.0;
			const double avg_a_pct = clamp01( ( avg_a - min_h ) / denom ) * 100.0;
			const double avg_b_pct = clamp01( ( avg_b - min_h ) / denom ) * 100.0;

			const double slope_a = !alt_dir
				? triangle_slope_deg( x, y, z_bl, x, my, z_tl, mx, y, z_br )
				: triangle_slope_deg( x, my, z_tl, mx, my, z_tr, x, y, z_bl );
			const double slope_b = !alt_dir
				? triangle_slope_deg( mx, my, z_tr, mx, y, z_br, x, my, z_tl )
				: triangle_slope_deg( mx, my, z_tr, mx, y, z_br, x, y, z_bl );

			const char* top_tex_a = choose_material( effective_slots, material_rules, avg_a_pct, slope_a );
			const char* top_tex_b = choose_material( effective_slots, material_rules, avg_b_pct, slope_b );

			insert_brush_into( entity, x, y, min_z, mx, my, base_max_z,
			                   z_bl, z_tl, z_br, z_tr,
			                   top_tex_a, top_tex_b, caulk, split_diagonally, alt_dir );
		}
	}

	SceneChangeNotify();
}

// ---------------------------------------------------------------------------
// Tunnel terrain
// ---------------------------------------------------------------------------

static void insert_floor_ceil_brush( scene::Node& entity,
                                      double x,  double y,  double mx, double my,
                                      double z_bl, double z_tl, double z_br, double z_tr,
                                      double min_z, double solid_top,
                                      const char* top_tex_a, const char* top_tex_b, const char* caulk,
                                      bool is_ceiling, bool alt_dir ){
	_QERFaceData face;

	if ( !alt_dir ) {
		// BL→TR split
		// Triangle 1: BL, TL, BR
		{
			NodeSmartReference brush( GlobalBrushCreator().createBrush() );
			if ( !is_ceiling ) {
				fill_face( face, x,  y,  z_bl,      x,  my, z_tl,      mx, y,  z_br,      top_tex_a ); GlobalBrushCreator().Brush_addFace( brush, face );
				fill_face( face, x,  y,  min_z,     mx, y,  min_z,     x,  my, min_z,     caulk );   GlobalBrushCreator().Brush_addFace( brush, face );
			} else {
				fill_face( face, x,  y,  z_bl,      mx, y,  z_br,      x,  my, z_tl,      top_tex_a ); GlobalBrushCreator().Brush_addFace( brush, face );
				fill_face( face, x,  y,  solid_top, x,  my, solid_top, mx, y,  solid_top, caulk );   GlobalBrushCreator().Brush_addFace( brush, face );
			}
			fill_face( face, x,  y,  min_z, x,  my, min_z,    x,  y,  solid_top, caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, x,  y,  min_z, x,  y,  solid_top, mx, y,  min_z,    caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, mx, y,  min_z, mx, y,  solid_top, x,  my, min_z,    caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			Node_getTraversable( entity )->insert( brush );
		}
		// Triangle 2: TR, BR, TL
		{
			NodeSmartReference brush( GlobalBrushCreator().createBrush() );
			if ( !is_ceiling ) {
				fill_face( face, mx, my, z_tr,      mx, y,  z_br,      x,  my, z_tl,      top_tex_b ); GlobalBrushCreator().Brush_addFace( brush, face );
				fill_face( face, mx, my, min_z,     x,  my, min_z,     mx, y,  min_z,     caulk );   GlobalBrushCreator().Brush_addFace( brush, face );
			} else {
				fill_face( face, mx, my, z_tr,      x,  my, z_tl,      mx, y,  z_br,      top_tex_b ); GlobalBrushCreator().Brush_addFace( brush, face );
				fill_face( face, mx, my, solid_top, mx, y,  solid_top, x,  my, solid_top, caulk );   GlobalBrushCreator().Brush_addFace( brush, face );
			}
			fill_face( face, mx, y,  min_z, mx, y,  solid_top, mx, my, min_z,     caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, x,  my, min_z, mx, my, min_z,     x,  my, solid_top, caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, x,  my, min_z, x,  my, solid_top, mx, y,  min_z,     caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			Node_getTraversable( entity )->insert( brush );
		}
	}
	else {
		// TL→BR split
		// Triangle 1: TL, TR, BL
		{
			NodeSmartReference brush( GlobalBrushCreator().createBrush() );
			if ( !is_ceiling ) {
				fill_face( face, x,  my, z_tl,      mx, my, z_tr,      x,  y,  z_bl,      top_tex_a ); GlobalBrushCreator().Brush_addFace( brush, face );
				fill_face( face, x,  my, min_z,     x,  y,  min_z,     mx, my, min_z,     caulk );   GlobalBrushCreator().Brush_addFace( brush, face );
			} else {
				fill_face( face, x,  my, z_tl,      x,  y,  z_bl,      mx, my, z_tr,      top_tex_a ); GlobalBrushCreator().Brush_addFace( brush, face );
				fill_face( face, x,  my, solid_top, mx, my, solid_top, x,  y,  solid_top, caulk );   GlobalBrushCreator().Brush_addFace( brush, face );
			}
			fill_face( face, x,  y,  min_z, x,  my, min_z,    x,  y,  solid_top, caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, x,  my, min_z, mx, my, min_z,    x,  my, solid_top, caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, mx, my, min_z, x,  y,  min_z,    x,  y,  solid_top, caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			Node_getTraversable( entity )->insert( brush );
		}
		// Triangle 2: TR, BR, BL
		{
			NodeSmartReference brush( GlobalBrushCreator().createBrush() );
			if ( !is_ceiling ) {
				fill_face( face, mx, my, z_tr,      mx, y,  z_br,      x,  y,  z_bl,      top_tex_b ); GlobalBrushCreator().Brush_addFace( brush, face );
				fill_face( face, mx, my, min_z,     x,  y,  min_z,     mx, y,  min_z,     caulk );   GlobalBrushCreator().Brush_addFace( brush, face );
			} else {
				fill_face( face, mx, my, z_tr,      x,  y,  z_bl,      mx, y,  z_br,      top_tex_b ); GlobalBrushCreator().Brush_addFace( brush, face );
				fill_face( face, mx, my, solid_top, mx, y,  solid_top, x,  y,  solid_top, caulk );   GlobalBrushCreator().Brush_addFace( brush, face );
			}
			fill_face( face, mx, y,  min_z, mx, y,  solid_top, mx, my, min_z,    caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, x,  y,  min_z, x,  y,  solid_top, mx, y,  min_z,    caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			fill_face( face, x,  y,  min_z, mx, my, min_z,     x,  y,  solid_top, caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			Node_getTraversable( entity )->insert( brush );
		}
	}
}

static void insert_wall_brush( scene::Node& entity,
                                double x_bl, double x_tl, double x_br, double x_tr,
                                double gy, double gmx_y, double gz, double gmx_z,
                                double outer_x, double limit_x,
                                const char* top_tex_a, const char* top_tex_b, const char* caulk,
                                bool is_left, bool alt_dir ){
	// Wall geometry is floor/ceiling rotated 90°.
	// Coordinate mapping: floor(fx,fy,fz) → world(fz, fx, fy)
	//   gy→fx, gz→fy, gmx_y→fmx, gmx_z→fmy, x_*→fz_*, outer_x/limit_x→fmin_z/fsolid_top
	// Left wall: inner surface faces +X (like floor), cap at outer_x (min_z side).
	// Right wall: inner surface faces −X (like ceiling), cap at outer_x (solid_top side).
	_QERFaceData face;
	const bool   is_ceiling = !is_left;
	const double min_z      = is_left ? outer_x : limit_x;
	const double solid_top  = is_left ? limit_x  : outer_x;

	// Aliases to match insert_floor_ceil_brush variable names exactly.
	const double x = gy, y = gz, mx = gmx_y, my = gmx_z;
	const double z_bl = x_bl, z_tl = x_tl, z_br = x_br, z_tr = x_tr;

	// fw: emit a face using floor coordinate order but with world axes permuted.
	auto fw = [&]( double fx0, double fy0, double fz0,
	               double fx1, double fy1, double fz1,
	               double fx2, double fy2, double fz2,
	               const char* shader ){
		fill_face( face, fz0, fx0, fy0, fz1, fx1, fy1, fz2, fx2, fy2, shader );
	};

	if ( !alt_dir ) {
		// BL→TR split
		{
			NodeSmartReference brush( GlobalBrushCreator().createBrush() );
			if ( !is_ceiling ) {
				fw( x,  y,  z_bl,      x,  my, z_tl,      mx, y,  z_br,      top_tex_a ); GlobalBrushCreator().Brush_addFace( brush, face );
				fw( x,  y,  min_z,     mx, y,  min_z,     x,  my, min_z,     caulk );   GlobalBrushCreator().Brush_addFace( brush, face );
			} else {
				fw( x,  y,  z_bl,      mx, y,  z_br,      x,  my, z_tl,      top_tex_a ); GlobalBrushCreator().Brush_addFace( brush, face );
				fw( x,  y,  solid_top, x,  my, solid_top, mx, y,  solid_top, caulk );   GlobalBrushCreator().Brush_addFace( brush, face );
			}
			fw( x,  y,  min_z, x,  my, min_z,    x,  y,  solid_top, caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			fw( x,  y,  min_z, x,  y,  solid_top, mx, y,  min_z,    caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			fw( mx, y,  min_z, mx, y,  solid_top, x,  my, min_z,    caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			Node_getTraversable( entity )->insert( brush );
		}
		{
			NodeSmartReference brush( GlobalBrushCreator().createBrush() );
			if ( !is_ceiling ) {
				fw( mx, my, z_tr,      mx, y,  z_br,      x,  my, z_tl,      top_tex_b ); GlobalBrushCreator().Brush_addFace( brush, face );
				fw( mx, my, min_z,     x,  my, min_z,     mx, y,  min_z,     caulk );   GlobalBrushCreator().Brush_addFace( brush, face );
			} else {
				fw( mx, my, z_tr,      x,  my, z_tl,      mx, y,  z_br,      top_tex_b ); GlobalBrushCreator().Brush_addFace( brush, face );
				fw( mx, my, solid_top, mx, y,  solid_top, x,  my, solid_top, caulk );   GlobalBrushCreator().Brush_addFace( brush, face );
			}
			fw( mx, y,  min_z, mx, y,  solid_top, mx, my, min_z,     caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			fw( x,  my, min_z, mx, my, min_z,     x,  my, solid_top, caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			fw( x,  my, min_z, x,  my, solid_top, mx, y,  min_z,     caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			Node_getTraversable( entity )->insert( brush );
		}
	}
	else {
		// TL→BR split
		{
			NodeSmartReference brush( GlobalBrushCreator().createBrush() );
			if ( !is_ceiling ) {
				fw( x,  my, z_tl,      mx, my, z_tr,      x,  y,  z_bl,      top_tex_a ); GlobalBrushCreator().Brush_addFace( brush, face );
				fw( x,  my, min_z,     x,  y,  min_z,     mx, my, min_z,     caulk );   GlobalBrushCreator().Brush_addFace( brush, face );
			} else {
				fw( x,  my, z_tl,      x,  y,  z_bl,      mx, my, z_tr,      top_tex_a ); GlobalBrushCreator().Brush_addFace( brush, face );
				fw( x,  my, solid_top, mx, my, solid_top, x,  y,  solid_top, caulk );   GlobalBrushCreator().Brush_addFace( brush, face );
			}
			fw( x,  y,  min_z, x,  my, min_z,    x,  y,  solid_top, caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			fw( x,  my, min_z, mx, my, min_z,    x,  my, solid_top, caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			fw( mx, my, min_z, x,  y,  min_z,    x,  y,  solid_top, caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			Node_getTraversable( entity )->insert( brush );
		}
		{
			NodeSmartReference brush( GlobalBrushCreator().createBrush() );
			if ( !is_ceiling ) {
				fw( mx, my, z_tr,      mx, y,  z_br,      x,  y,  z_bl,      top_tex_b ); GlobalBrushCreator().Brush_addFace( brush, face );
				fw( mx, my, min_z,     x,  y,  min_z,     mx, y,  min_z,     caulk );   GlobalBrushCreator().Brush_addFace( brush, face );
			} else {
				fw( mx, my, z_tr,      x,  y,  z_bl,      mx, y,  z_br,      top_tex_b ); GlobalBrushCreator().Brush_addFace( brush, face );
				fw( mx, my, solid_top, mx, y,  solid_top, x,  y,  solid_top, caulk );   GlobalBrushCreator().Brush_addFace( brush, face );
			}
			fw( mx, y,  min_z, mx, y,  solid_top, mx, my, min_z,    caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			fw( x,  y,  min_z, x,  y,  solid_top, mx, y,  min_z,    caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			fw( x,  y,  min_z, mx, my, min_z,     x,  y,  solid_top, caulk ); GlobalBrushCreator().Brush_addFace( brush, face );
			Node_getTraversable( entity )->insert( brush );
		}
	}
}

void build_tunnel_brushes( const BrushData& target, double step_x, double step_y,
                            const TunnelMaps& maps, const char* top_texture,
                            const TerrainMaterialSlots& material_slots,
                            const TerrainMaterialRules& material_rules,
                            double cave_height, double slope_height,
                            const TerrainBuildOptions& options,
                            std::vector<scene::Node*>* created_entities ){
	UndoScope undo( "terrainGenerator.generateTunnel", options.undoable );

	scene::Node& floor_entity  = create_func_group( options );
	scene::Node& ceil_entity   = create_func_group( options );
	scene::Node& lwall_entity  = create_func_group( options );
	scene::Node& rwall_entity  = create_func_group( options );
	if ( created_entities != nullptr ) {
		created_entities->push_back( &floor_entity );
		created_entities->push_back( &ceil_entity );
		created_entities->push_back( &lwall_entity );
		created_entities->push_back( &rwall_entity );
	}

	const char* caulk    = "textures/common/caulk";
	TerrainMaterialSlots effective_slots = material_slots;
	if ( effective_slots.base == nullptr || effective_slots.base[0] == '\0' ) {
		effective_slots.base = top_texture;
	}
	double min_z         = target.min_z;
	// Highest ceiling point — slope_height may be negative (downward slope),
	// so the ceiling peak is always at the high end of the slope.
	double max_ceil_z    = target.max_z + cave_height + std::max( 0.0, slope_height );
	double ceil_solid_top = max_ceil_z + target.height_z;

	auto r2 = []( double v ) { return std::round( v * 100.0 ) / 100.0; };

	// Floor and ceiling
	int x_index = 0;
	for ( double x = target.min_x; x < target.max_x - 0.01; x += step_x, ++x_index ) {
		int y_index = 0;
		for ( double y = target.min_y; y < target.max_y - 0.01; y += step_y, ++y_index ) {
			double mx = x + step_x < target.max_x ? x + step_x : target.max_x;
			double my = y + step_y < target.max_y ? y + step_y : target.max_y;

			auto safe_at = [&]( const HeightMap& m, double kx, double ky, double fallback ) -> double {
				auto it = m.find( { r2( kx ), r2( ky ) } );
				if ( it == m.end() ) {
					globalErrorStream() << "TerrainGenerator: height map missing key ("
					                    << kx << ", " << ky << ") — using fallback\n";
					return fallback;
				}
				return it->second;
			};

			double f_bl = safe_at( maps.floor_map,   x,  y,  min_z );
			double f_tl = safe_at( maps.floor_map,   x,  my, min_z );
			double f_br = safe_at( maps.floor_map,   mx, y,  min_z );
			double f_tr = safe_at( maps.floor_map,   mx, my, min_z );
			double c_bl = safe_at( maps.ceiling_map, x,  y,  max_ceil_z );
			double c_tl = safe_at( maps.ceiling_map, x,  my, max_ceil_z );
			double c_br = safe_at( maps.ceiling_map, mx, y,  max_ceil_z );
			double c_tr = safe_at( maps.ceiling_map, mx, my, max_ceil_z );

			bool alt_dir = ( ( x_index + y_index ) % 2 ) != 0;
			const double h_denom = std::max( 1.0, target.max_z - target.min_z );
			const double f_avg_a = !alt_dir ? ( f_bl + f_tl + f_br ) / 3.0 : ( f_tl + f_tr + f_bl ) / 3.0;
			const double f_avg_b = !alt_dir ? ( f_tr + f_br + f_tl ) / 3.0 : ( f_tr + f_br + f_bl ) / 3.0;
			const double c_avg_a = !alt_dir ? ( c_bl + c_tl + c_br ) / 3.0 : ( c_tl + c_tr + c_bl ) / 3.0;
			const double c_avg_b = !alt_dir ? ( c_tr + c_br + c_tl ) / 3.0 : ( c_tr + c_br + c_bl ) / 3.0;

			const double f_slope_a = !alt_dir
				? triangle_slope_deg( x, y, f_bl, x, my, f_tl, mx, y, f_br )
				: triangle_slope_deg( x, my, f_tl, mx, my, f_tr, x, y, f_bl );
			const double f_slope_b = !alt_dir
				? triangle_slope_deg( mx, my, f_tr, mx, y, f_br, x, my, f_tl )
				: triangle_slope_deg( mx, my, f_tr, mx, y, f_br, x, y, f_bl );
			const double c_slope_a = !alt_dir
				? triangle_slope_deg( x, y, c_bl, x, my, c_tl, mx, y, c_br )
				: triangle_slope_deg( x, my, c_tl, mx, my, c_tr, x, y, c_bl );
			const double c_slope_b = !alt_dir
				? triangle_slope_deg( mx, my, c_tr, mx, y, c_br, x, my, c_tl )
				: triangle_slope_deg( mx, my, c_tr, mx, y, c_br, x, y, c_bl );

			const char* floor_tex_a = choose_material( effective_slots, material_rules, clamp01( ( f_avg_a - target.min_z ) / h_denom ) * 100.0, f_slope_a );
			const char* floor_tex_b = choose_material( effective_slots, material_rules, clamp01( ( f_avg_b - target.min_z ) / h_denom ) * 100.0, f_slope_b );
			const char* ceil_tex_a  = choose_material( effective_slots, material_rules, clamp01( ( c_avg_a - target.min_z ) / h_denom ) * 100.0, c_slope_a );
			const char* ceil_tex_b  = choose_material( effective_slots, material_rules, clamp01( ( c_avg_b - target.min_z ) / h_denom ) * 100.0, c_slope_b );

			insert_floor_ceil_brush( floor_entity, x, y, mx, my,
			                         f_bl, f_tl, f_br, f_tr,
			                         min_z, max_ceil_z, floor_tex_a, floor_tex_b, caulk, false, alt_dir );
			insert_floor_ceil_brush( ceil_entity, x, y, mx, my,
			                         c_bl, c_tl, c_br, c_tr,
			                         min_z, ceil_solid_top, ceil_tex_a, ceil_tex_b, caulk, true, alt_dir );
		}
	}

	// Walls — must match the range generated by terrain_engine.
	// slope_height may be negative (downward slope), so clamp accordingly.
	double wall_min_z   = target.max_z + std::min( 0.0, slope_height );
	double wall_max_z   = wall_min_z + cave_height + std::abs( slope_height );
	double step_z       = maps.step_z;
	double outer_left   = target.min_x - step_x;
	double outer_right  = target.max_x + step_x;
	double limit_x      = ( target.min_x + target.max_x ) / 2.0;

	int gy_index = 0;
	for ( double gy = target.min_y; gy < target.max_y - 0.01; gy += step_y, ++gy_index ) {
		int gz_index = 0;
		for ( double gz = wall_min_z; gz < wall_max_z - 0.01; gz += step_z, ++gz_index ) {
			double gmx_y = gy + step_y < target.max_y ? gy + step_y : target.max_y;
			double gmx_z = gz + step_z < wall_max_z   ? gz + step_z : wall_max_z;

			auto safe_wall = [&]( const HeightMap& m, double kx, double ky, double fallback ) -> double {
				auto it = m.find( { r2( kx ), r2( ky ) } );
				if ( it == m.end() ) {
					globalErrorStream() << "TerrainGenerator: wall map missing key ("
					                    << kx << ", " << ky << ") — using fallback\n";
					return fallback;
				}
				return it->second;
			};

			double lx_bl = safe_wall( maps.left_wall_map,  gy,     gz,     limit_x );
			double lx_tl = safe_wall( maps.left_wall_map,  gy,     gmx_z,  limit_x );
			double lx_br = safe_wall( maps.left_wall_map,  gmx_y,  gz,     limit_x );
			double lx_tr = safe_wall( maps.left_wall_map,  gmx_y,  gmx_z,  limit_x );

			double rx_bl = safe_wall( maps.right_wall_map, gy,     gz,     limit_x );
			double rx_tl = safe_wall( maps.right_wall_map, gy,     gmx_z,  limit_x );
			double rx_br = safe_wall( maps.right_wall_map, gmx_y,  gz,     limit_x );
			double rx_tr = safe_wall( maps.right_wall_map, gmx_y,  gmx_z,  limit_x );

			bool alt_dir = ( ( gy_index + gz_index ) % 2 ) != 0;
			const double w_denom = std::max( 1.0, target.max_z - target.min_z );
			const double l_avg_a = !alt_dir ? ( gz + gmx_z + gz ) / 3.0 : ( gmx_z + gmx_z + gz ) / 3.0;
			const double l_avg_b = !alt_dir ? ( gmx_z + gz + gmx_z ) / 3.0 : ( gmx_z + gz + gz ) / 3.0;
			const double r_avg_a = l_avg_a;
			const double r_avg_b = l_avg_b;
			const double l_slope_a = !alt_dir
				? triangle_slope_deg( lx_bl, gy, gz, lx_tl, gy, gmx_z, lx_br, gmx_y, gz )
				: triangle_slope_deg( lx_tl, gy, gmx_z, lx_tr, gmx_y, gmx_z, lx_bl, gy, gz );
			const double l_slope_b = !alt_dir
				? triangle_slope_deg( lx_tr, gmx_y, gmx_z, lx_br, gmx_y, gz, lx_tl, gy, gmx_z )
				: triangle_slope_deg( lx_tr, gmx_y, gmx_z, lx_br, gmx_y, gz, lx_bl, gy, gz );
			const double r_slope_a = !alt_dir
				? triangle_slope_deg( rx_bl, gy, gz, rx_tl, gy, gmx_z, rx_br, gmx_y, gz )
				: triangle_slope_deg( rx_tl, gy, gmx_z, rx_tr, gmx_y, gmx_z, rx_bl, gy, gz );
			const double r_slope_b = !alt_dir
				? triangle_slope_deg( rx_tr, gmx_y, gmx_z, rx_br, gmx_y, gz, rx_tl, gy, gmx_z )
				: triangle_slope_deg( rx_tr, gmx_y, gmx_z, rx_br, gmx_y, gz, rx_bl, gy, gz );
			const char* l_tex_a = choose_material( effective_slots, material_rules, clamp01( ( l_avg_a - target.min_z ) / w_denom ) * 100.0, l_slope_a );
			const char* l_tex_b = choose_material( effective_slots, material_rules, clamp01( ( l_avg_b - target.min_z ) / w_denom ) * 100.0, l_slope_b );
			const char* r_tex_a = choose_material( effective_slots, material_rules, clamp01( ( r_avg_a - target.min_z ) / w_denom ) * 100.0, r_slope_a );
			const char* r_tex_b = choose_material( effective_slots, material_rules, clamp01( ( r_avg_b - target.min_z ) / w_denom ) * 100.0, r_slope_b );

			insert_wall_brush( lwall_entity, lx_bl, lx_tl, lx_br, lx_tr,
			                   gy, gmx_y, gz, gmx_z, outer_left,  limit_x, l_tex_a, l_tex_b, caulk, true,  alt_dir );
			insert_wall_brush( rwall_entity, rx_bl, rx_tl, rx_br, rx_tr,
			                   gy, gmx_y, gz, gmx_z, outer_right, limit_x, r_tex_a, r_tex_b, caulk, false, alt_dir );
		}
	}

	SceneChangeNotify();
}
