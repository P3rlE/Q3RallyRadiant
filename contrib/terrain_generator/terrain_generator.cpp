#include "terrain_generator.h"

#include <algorithm>
#include "debugging/debugging.h"
#include "iplugin.h"
#include "string/string.h"
#include "modulesystem/singletonmodule.h"
#include "typesystem.h"

#include <QWidget>
#include <QDialog>
#include <QEventLoop>
#include <QTimer>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QCheckBox>
#include <QLineEdit>
#include <QPushButton>
#include <QFileDialog>
#include <QImage>
#include "gtkutil/spinbox.h"
#include "gtkutil/combobox.h"

#include <random>
#include <limits>
#include <vector>

#include "scenelib.h"

#include "terrain_math.h"
#include "noise.h"
#include "terrain_engine.h"
#include "brush_builder.h"

#include "ibrush.h"
#include "ientity.h"
#include "ieclass.h"
#include "iscenegraph.h"
#include "iundo.h"
#include "iselection.h"
#include "qerplugin.h"
#include "modulesystem/moduleregistry.h"

namespace terrain_generator
{

QWidget* main_window;
bool g_has_last_seed = false;
int  g_last_seed = 0;

enum class MaskPreset {
	None = 0,
	Radial = 1,
	LinearFalloff = 2,
	Centerline = 3,
	ImageImport = 4
};

enum class PostProcessPreset {
	Custom = 0,
	Subtle = 1,
	Medium = 2,
	Aggressive = 3
};

static int make_auto_seed(){
	std::random_device rd;
	return static_cast<int>( rd() );
}

static double clamp01( double v ){
	return std::max( 0.0, std::min( 1.0, v ) );
}

static MaskMap build_mask_map( const BrushData& target, double step_x, double step_y, MaskPreset preset, const QString& image_path ){
	MaskMap mask_map;
	if ( preset == MaskPreset::None ) {
		return mask_map;
	}

	QImage mask_image;
	if ( preset == MaskPreset::ImageImport && !image_path.trimmed().isEmpty() ) {
		mask_image = QImage( image_path );
	}

	for ( double x = target.min_x; x <= target.max_x + 0.01; x += step_x ) {
		for ( double y = target.min_y; y <= target.max_y + 0.01; y += step_y ) {
			const double nx = target.width_x > 0.0 ? ( x - target.min_x ) / target.width_x : 0.0;
			const double ny = target.length_y > 0.0 ? ( y - target.min_y ) / target.length_y : 0.0;
			double weight = 1.0;
			switch ( preset ) {
			case MaskPreset::Radial: {
				const double dx = nx - 0.5;
				const double dy = ny - 0.5;
				const double radial = std::sqrt( dx * dx + dy * dy ) / 0.5;
				weight = 1.0 - clamp01( radial );
				break;
			}
			case MaskPreset::LinearFalloff:
				weight = 1.0 - clamp01( nx );
				break;
			case MaskPreset::Centerline:
				weight = 1.0 - clamp01( std::abs( nx - 0.5 ) / 0.5 );
				break;
			case MaskPreset::ImageImport: {
				if ( mask_image.isNull() ) {
					weight = 1.0;
					break;
				}
				const int px = std::clamp( (int)std::round( nx * ( mask_image.width() - 1 ) ), 0, mask_image.width() - 1 );
				const int py = std::clamp( (int)std::round( ny * ( mask_image.height() - 1 ) ), 0, mask_image.height() - 1 );
				weight = qGray( mask_image.pixel( px, py ) ) / 255.0;
				break;
			}
			default:
				break;
			}
			mask_map[{ std::round( x * 100.0 ) / 100.0, std::round( y * 100.0 ) / 100.0 }] = clamp01( weight );
		}
	}

	return mask_map;
}

const char* init( void* hApp, void* pMainWidget ){
	main_window = static_cast<QWidget*>( pMainWidget );
	return "";
}

const char* getName(){
	return "TerrainGenerator";
}

const char* getCommandList(){
	return "About;Generate Terrain";
}

const char* getCommandTitleList(){
	return "";
}

void dispatch( const char* command, float* vMin, float* vMax, bool bSingleBrush ){
	if ( string_equal( command, "About" ) ) {
		const QString seed_text = g_has_last_seed ? QString::number( g_last_seed ) : "N/A";
		const QString about_text =
			"Terrain Generator\n\n"
			"Procedural CSG generation tool for id Tech 3 engines.\n\n"
			"Developed by vallz and vld\n\n"
			"Last used seed: " + seed_text;
		const QByteArray about_utf8 = about_text.toUtf8();
		GlobalRadiant().m_pfnMessageBox( main_window,
			about_utf8.constData(),
			"About Terrain Generator",
			EMessageBoxType::Info, 0 );
		return;
	}
	if ( string_equal( command, "Generate Terrain" ) ) {
		// Query the live selection bounds from the selection system.
		// Called at dialog-open time, on mode switch, and on OK — so the
		// user can make a selection while the dialog is open and retry.
		struct SelBounds { double x0, x1, y0, y1, z0, z1; bool valid; };
		auto query_sel = [&]( double min_size = 64.0 ) -> SelBounds {
			const AABB& b = GlobalSelectionSystem().getBoundsSelected();
			SelBounds s;
			s.x0 = b.origin[0] - b.extents[0];
			s.x1 = b.origin[0] + b.extents[0];
			s.y0 = b.origin[1] - b.extents[1];
			s.y1 = b.origin[1] + b.extents[1];
			s.z0 = b.origin[2] - b.extents[2];
			s.z1 = b.origin[2] + b.extents[2];
			s.valid = ( s.x1 - s.x0 >= min_size && s.y1 - s.y0 >= min_size );
			return s;
		};
		const SelBounds init_sel = query_sel();

		// --- Build dialog ---
		QDialog dialog( main_window, Qt::Dialog | Qt::WindowCloseButtonHint );
		dialog.setWindowTitle( "Terrain Generator" );

		dialog.setMinimumWidth( 420 );
		auto *form = new QFormLayout( &dialog );

		// Target mode
		auto *target_combo = new ComboBox;
		target_combo->addItem( "Use Selection", 0 );
		target_combo->addItem( "Manual Size",   1 );
		target_combo->setCurrentIndex( init_sel.valid ? 0 : 1 );
		form->addRow( "Target:", target_combo );

		// Selection size — live labels (left) + frozen reference labels (right).
		// Reference is captured on the first Generate click and stays fixed.
		// Helper: build one inline row widget with [current | ref: value]
		auto make_sel_row = []( QLabel*& cur_lbl, QLabel*& ref_lbl ) -> QWidget* {
			auto *w      = new QWidget;
			auto *layout = new QHBoxLayout( w );
			layout->setContentsMargins( 0, 0, 0, 0 );
			layout->setSpacing( 6 );
			cur_lbl = new QLabel;
			ref_lbl = new QLabel;
			ref_lbl->setStyleSheet( "color: gray;" );
			ref_lbl->hide();
			layout->addWidget( cur_lbl );
			layout->addWidget( ref_lbl );
			layout->addStretch();
			return w;
		};

		QLabel *sel_w_label, *ref_w_label;
		QLabel *sel_l_label, *ref_l_label;
		QLabel *sel_h_label, *ref_h_label;
		auto *sel_w_row = make_sel_row( sel_w_label, ref_w_label );
		auto *sel_l_row = make_sel_row( sel_l_label, ref_l_label );
		auto *sel_h_row = make_sel_row( sel_h_label, ref_h_label );

		auto refresh_sel_labels = [&](){
			const SelBounds s = query_sel();
			sel_w_label->setText( s.valid ? QString::number( (int)( s.x1 - s.x0 ) )               : "—" );
			sel_l_label->setText( s.valid ? QString::number( (int)( s.y1 - s.y0 ) )               : "—" );
			sel_h_label->setText( s.valid ? QString::number( (int)std::max( s.z1 - s.z0, 64.0 ) ) : "—" );
		};
		refresh_sel_labels();

		// Reference state — captured once on first Generate, frozen after that.
		SelBounds ref_bounds{};
		bool      ref_captured = false;

		// "Use reference" checkbox — hidden until first Generate populates it.
		auto *use_ref_cb = new QCheckBox( "Use reference for regeneration" );
		use_ref_cb->setChecked( true );
		use_ref_cb->hide();

		form->addRow( "Width (X):",  sel_w_row );
		form->addRow( "Length (Y):", sel_l_row );
		form->addRow( "Height (Z):", sel_h_row );
		form->addRow( "",            use_ref_cb );

		// Manual size inputs (shown in Manual Size mode)
		auto *manual_w_spin = new SpinBox( 64, 131072, 1024, 0, 64 );
		auto *manual_l_spin = new SpinBox( 64, 131072, 1024, 0, 64 );
		auto *manual_h_spin = new SpinBox( 64, 131072,   64, 0, 64 );
		form->addRow( "Width (X):",  manual_w_spin );
		form->addRow( "Length (Y):", manual_l_spin );
		form->addRow( "Height (Z):", manual_h_spin );

		// Sub-square size: preset combo + Advanced checkbox in one row
		auto *sq_widget  = new QWidget;
		auto *sq_hbox    = new QHBoxLayout( sq_widget );
		sq_hbox->setContentsMargins( 0, 0, 0, 0 );
		auto *sq_combo   = new ComboBox;
		for ( int v : { 8, 16, 32, 64, 128, 256, 512, 1024 } )
			sq_combo->addItem( QString::number( v ), v );
		sq_combo->setCurrentIndex( 3 ); // 64
		auto *sq_advanced = new QCheckBox( "Advanced" );
		sq_hbox->addWidget( sq_combo );
		sq_hbox->addWidget( sq_advanced );
		form->addRow( "Sub-square Size:", sq_widget );

		// Advanced step X/Y (hidden until Advanced is checked)
		auto *step_x_spin = new SpinBox( 8, 512, 64, 0, 8 );
		auto *step_y_spin = new SpinBox( 8, 512, 64, 0, 8 );
		form->addRow( "Step X:", step_x_spin );
		form->addRow( "Step Y:", step_y_spin );

		// Base shape
		auto *shape_combo = new ComboBox;
		shape_combo->addItem( "Flat (None)",      (int)ShapeType::Flat );
		shape_combo->addItem( "Hill",             (int)ShapeType::Hill );
		shape_combo->addItem( "Crater",           (int)ShapeType::Crater );
		shape_combo->addItem( "Ridge",            (int)ShapeType::Ridge );
		shape_combo->addItem( "Slope",            (int)ShapeType::Slope );
		shape_combo->addItem( "Volcano",          (int)ShapeType::Volcano );
		shape_combo->addItem( "Valley",           (int)ShapeType::Valley );
		shape_combo->addItem( "Tunnel",           (int)ShapeType::Tunnel );
		shape_combo->addItem( "Slope Tunnel",     (int)ShapeType::SlopeTunnel );
		shape_combo->addItem( "Banked Turn",      (int)ShapeType::BankedTurn );
		shape_combo->addItem( "Berm",             (int)ShapeType::Berm );
		shape_combo->addItem( "Jump Ramp",        (int)ShapeType::JumpRamp );
		shape_combo->addItem( "Whoops",           (int)ShapeType::Whoops );
		shape_combo->setCurrentIndex( 0 ); // Flat
		form->addRow( "Base Shape:", shape_combo );

		// Shape height — editable spinbox (manual mode or non-slope shapes).
		// For Slope / Slope Tunnel in Use Selection mode, replaced by a
		// read-only label derived from the selection brush's Z extent.
		auto *shape_height_spin = new DoubleSpinBox( 0, 4096, 160, 2, 8 );
		form->addRow( "Peak Height:", shape_height_spin );

		// Tunnel height — only visible for Slope Tunnel
		auto *tunnel_height_spin = new DoubleSpinBox( 0, 4096, 192, 2, 8 );
		form->addRow( "Tunnel Height:", tunnel_height_spin );

		auto *curve_radius_spin = new DoubleSpinBox( 64, 8192, 640, 2, 16 );
		form->addRow( "Curve Radius:", curve_radius_spin );

		auto *banking_angle_spin = new DoubleSpinBox( 0, 45, 12, 1, 1 );
		form->addRow( "Bank Angle (°):", banking_angle_spin );

		auto *ramp_length_spin = new DoubleSpinBox( 64, 8192, 512, 2, 16 );
		form->addRow( "Ramp Length:", ramp_length_spin );

		// Terrace step — hidden for Flat
		auto *terrace_spin = new DoubleSpinBox( 0, 512, 0, 2, 8 );
		form->addRow( "Terrace Step:", terrace_spin );

		// Noise type
		auto *noise_combo = new ComboBox;
		noise_combo->addItem( "Perlin Noise",     (int)NoiseType::Perlin );
		noise_combo->addItem( "Simplex Noise",    (int)NoiseType::Simplex );
		noise_combo->addItem( "Regular (Random)", (int)NoiseType::Random );
		form->addRow( "Noise Type:", noise_combo );

		// Variance / Frequency
		auto *variance_spin  = new DoubleSpinBox( 0, 1024, 16, 2, 1 );
		form->addRow( "Variance:", variance_spin );
		auto *frequency_spin = new DoubleSpinBox( 0.0001, 1.0, 0.005, 4, 0.001 );
		form->addRow( "Frequency:", frequency_spin );

		auto *post_process_combo = new ComboBox;
		post_process_combo->addItem( "Custom", (int)PostProcessPreset::Custom );
		post_process_combo->addItem( "Subtle", (int)PostProcessPreset::Subtle );
		post_process_combo->addItem( "Medium", (int)PostProcessPreset::Medium );
		post_process_combo->addItem( "Aggressive", (int)PostProcessPreset::Aggressive );
		post_process_combo->setCurrentIndex( 1 );
		form->addRow( "Post-process Preset:", post_process_combo );

		auto *laplacian_spin = new SpinBox( 0, 64, 1, 0, 1 );
		auto *thermal_spin = new SpinBox( 0, 64, 2, 0, 1 );
		auto *hydraulic_spin = new SpinBox( 0, 64, 1, 0, 1 );
		form->addRow( "Laplacian Iterations:", laplacian_spin );
		form->addRow( "Thermal Iterations:", thermal_spin );
		form->addRow( "Hydraulic Iterations:", hydraulic_spin );

		auto *mask_preset_combo = new ComboBox;
		mask_preset_combo->addItem( "None",                  (int)MaskPreset::None );
		mask_preset_combo->addItem( "Radial Mask",           (int)MaskPreset::Radial );
		mask_preset_combo->addItem( "Linear Falloff",        (int)MaskPreset::LinearFalloff );
		mask_preset_combo->addItem( "Centerline Favoring",   (int)MaskPreset::Centerline );
		mask_preset_combo->addItem( "Image Import (PNG)",    (int)MaskPreset::ImageImport );
		form->addRow( "Mask Preset:", mask_preset_combo );

		auto *mask_image_widget = new QWidget;
		auto *mask_image_hbox   = new QHBoxLayout( mask_image_widget );
		mask_image_hbox->setContentsMargins( 0, 0, 0, 0 );
		auto *mask_image_edit   = new QLineEdit;
		auto *mask_image_pick   = new QPushButton( "Browse" );
		mask_image_pick->setFixedWidth( 70 );
		mask_image_hbox->addWidget( mask_image_edit );
		mask_image_hbox->addWidget( mask_image_pick );
		form->addRow( "Mask PNG:", mask_image_widget );

		auto *seed_widget = new QWidget;
		auto *seed_hbox   = new QHBoxLayout( seed_widget );
		seed_hbox->setContentsMargins( 0, 0, 0, 0 );
		auto *seed_spin   = new SpinBox( std::numeric_limits<int>::min(), std::numeric_limits<int>::max(), make_auto_seed(), 0, 1 );
		auto *auto_seed_cb = new QCheckBox( "Auto Seed" );
		auto_seed_cb->setChecked( true );
		seed_spin->setEnabled( false );
		seed_hbox->addWidget( seed_spin );
		seed_hbox->addWidget( auto_seed_cb );
		seed_hbox->addStretch();
		form->addRow( "Seed:", seed_widget );

		// Texture slots — base + optional material slots
		auto *tex_widget = new QWidget;
		auto *tex_hbox   = new QHBoxLayout( tex_widget );
		tex_hbox->setContentsMargins( 0, 0, 0, 0 );
		auto *texture_edit = new QLineEdit( GlobalRadiant().TextureBrowser_getSelectedShader() );
		auto *tex_pick     = new QPushButton( "Pick" );
		tex_pick->setFixedWidth( 48 );
		tex_hbox->addWidget( texture_edit );
		tex_hbox->addWidget( tex_pick );

		// Poll the selection bounds every 250ms so the W/L/H labels stay
		// current if the user changes their selection while the dialog is open.
		auto *sel_timer = new QTimer( &dialog );
		sel_timer->setInterval( 250 );
		QObject::connect( sel_timer, &QTimer::timeout, [&](){
			if ( target_combo->currentIndex() == 0 )
				refresh_sel_labels();
		} );
		sel_timer->start();

		// Poll the texture browser every 100ms; auto-update the field and
		// (if the browser was closed before Pick opened it) auto-close it.
		QString last_polled_shader = texture_edit->text();
		bool close_browser_on_pick = false;

		auto *pick_timer = new QTimer( &dialog );
		pick_timer->setInterval( 100 );
		QObject::connect( pick_timer, &QTimer::timeout, [&](){
			const QString current = GlobalRadiant().TextureBrowser_getSelectedShader();
			if ( current != last_polled_shader ) {
				last_polled_shader = current;
				texture_edit->setText( current );
				if ( close_browser_on_pick ) {
					close_browser_on_pick = false;
					GlobalRadiant().TextureBrowser_close();
				}
			}
		} );
		pick_timer->start();

		QObject::connect( tex_pick, &QPushButton::clicked, [&](){
			close_browser_on_pick = !GlobalRadiant().TextureBrowser_isShown();
			GlobalRadiant().TextureBrowser_show();
		} );
		form->addRow( "Base Material:", tex_widget );

		auto *steep_material_edit = new QLineEdit;
		auto *peak_material_edit  = new QLineEdit;
		auto *dirt_material_edit  = new QLineEdit;
		auto *track_material_edit = new QLineEdit;
		form->addRow( "Steep Material:", steep_material_edit );
		form->addRow( "Peak Material:",  peak_material_edit );
		form->addRow( "Dirt Material:",  dirt_material_edit );
		form->addRow( "Track Material:", track_material_edit );

		auto *steep_angle_spin = new DoubleSpinBox( 0.0, 89.0, 45.0, 1, 1 );
		auto *peak_min_spin    = new DoubleSpinBox( 0.0, 100.0, 85.0, 1, 1 );
		auto *dirt_min_spin    = new DoubleSpinBox( 0.0, 100.0, 20.0, 1, 1 );
		auto *dirt_max_spin    = new DoubleSpinBox( 0.0, 100.0, 55.0, 1, 1 );
		auto *track_min_spin   = new DoubleSpinBox( 0.0, 100.0, 40.0, 1, 1 );
		auto *track_max_spin   = new DoubleSpinBox( 0.0, 100.0, 48.0, 1, 1 );
		form->addRow( "Steep Angle (°):", steep_angle_spin );
		form->addRow( "Peak Min Height (%):", peak_min_spin );
		form->addRow( "Dirt Min Height (%):", dirt_min_spin );
		form->addRow( "Dirt Max Height (%):", dirt_max_spin );
		form->addRow( "Track Min Height (%):", track_min_spin );
		form->addRow( "Track Max Height (%):", track_max_spin );

		// Generate (generate without closing) + Close
		// Buttons: Generate (left) — Close (right), explicit layout so the
		// order is platform-independent.
		auto *btn_widget   = new QWidget;
		auto *btn_layout   = new QHBoxLayout( btn_widget );
		btn_layout->setContentsMargins( 0, 12, 0, 0 ); // top spacing from fields
		auto *preview_btn  = new QPushButton( "Preview" );
		auto *generate_btn = new QPushButton( "Generate" );
		auto *close_btn    = new QPushButton( "Close" );
		btn_layout->addStretch();
		btn_layout->addWidget( preview_btn );
		btn_layout->addSpacing( 8 );
		btn_layout->addWidget( generate_btn );
		btn_layout->addSpacing( 8 );
		btn_layout->addWidget( close_btn );
		btn_layout->addStretch();
		form->addRow( btn_widget );

		// Helper: show/hide a form row (widget + its label)
		auto set_row_visible = [&]( QWidget *w, bool visible ){
			w->setVisible( visible );
			if ( auto *lbl = form->labelForField( w ) )
				lbl->setVisible( visible );
		};

		std::vector<scene::Node*> preview_entities;
		bool preview_active = false;

		auto clear_preview = [&](){
			bool changed = false;
			for ( scene::Node* node : preview_entities ) {
				if ( node != nullptr ) {
					Node_getTraversable( GlobalSceneGraph().root() )->erase( *node );
					changed = true;
				}
			}
			preview_entities.clear();
			preview_active = false;
			if ( changed )
				SceneChangeNotify();
		};

		// Shared validation + generation — returns true on success.
		auto do_generate = [&]( bool preview_mode ) -> bool {
			if ( target_combo->currentIndex() == 0 && !query_sel().valid ) {
				GlobalRadiant().m_pfnMessageBox( main_window,
					"No valid brush is selected.\n\n"
					"Please select a brush in the viewport first,\n"
					"or switch the Target to \"Manual Size\".",
					"Terrain Generator — No Selection",
					EMessageBoxType::Error, 0 );
				return false;
			}
			if ( texture_edit->text().trimmed().isEmpty() ) {
				GlobalRadiant().m_pfnMessageBox( main_window,
					"No texture specified.\n\n"
					"Please enter a texture path or use the Pick button\n"
					"to select one from the texture browser.",
					"Terrain Generator — No Texture",
					EMessageBoxType::Error, 0 );
				return false;
			}

			// --- Read parameters ---
			const bool use_manual  = ( target_combo->currentIndex() == 1 );
			const bool advanced    = sq_advanced->isChecked();
			const double step_x    = advanced ? step_x_spin->value() : sq_combo->currentData().toInt();
			const double step_y    = advanced ? step_y_spin->value() : sq_combo->currentData().toInt();
			const ShapeType shape  = (ShapeType)shape_combo->currentData().toInt();
			const NoiseType noise  = (NoiseType)noise_combo->currentData().toInt();
			const double tun_height = tunnel_height_spin->value();
			const double variance     = variance_spin->value();
			const double frequency    = frequency_spin->value();
			const PostProcessSettings post_process{
				laplacian_spin->value(),
				thermal_spin->value(),
				hydraulic_spin->value()
			};
			const MaskPreset mask_preset = (MaskPreset)mask_preset_combo->currentData().toInt();
			const double terrace      = terrace_spin->value();
			const double curve_radius = curve_radius_spin->value();
			const double bank_angle   = banking_angle_spin->value();
			const double ramp_length  = ramp_length_spin->value();
			int seed = seed_spin->value();
			if ( auto_seed_cb->isChecked() ) {
				seed = make_auto_seed();
				seed_spin->setValue( seed );
			}
			const std::string texture_str = texture_edit->text().toStdString();
			const char* texture       = texture_str.c_str();
			const std::string steep_texture_str = steep_material_edit->text().trimmed().toStdString();
			const std::string peak_texture_str  = peak_material_edit->text().trimmed().toStdString();
			const std::string dirt_texture_str  = dirt_material_edit->text().trimmed().toStdString();
			const std::string track_texture_str = track_material_edit->text().trimmed().toStdString();
			const TerrainMaterialSlots material_slots{
				texture,
				steep_texture_str.empty() ? nullptr : steep_texture_str.c_str(),
				peak_texture_str.empty()  ? nullptr : peak_texture_str.c_str(),
				dirt_texture_str.empty()  ? nullptr : dirt_texture_str.c_str(),
				track_texture_str.empty() ? nullptr : track_texture_str.c_str()
			};
			const TerrainMaterialRules material_rules{
				steep_angle_spin->value(),
				peak_min_spin->value(),
				dirt_min_spin->value(),
				dirt_max_spin->value(),
				track_min_spin->value(),
				track_max_spin->value()
			};
			if ( mask_preset == MaskPreset::ImageImport ) {
				const QString mask_path = mask_image_edit->text().trimmed();
				if ( mask_path.isEmpty() || QImage( mask_path ).isNull() ) {
					GlobalRadiant().m_pfnMessageBox( main_window,
						"Mask import is enabled, but the PNG file is missing or invalid.\n\n"
						"Please select a valid grayscale PNG or switch the mask preset.",
						"Terrain Generator — Invalid Mask PNG",
						EMessageBoxType::Error, 0 );
					return false;
				}
			}

			// --- Determine bounds (must happen before deleting the selection brush) ---
			BrushData target;

			if ( use_manual ) {
				target = make_manual_brush_data( manual_w_spin->value(), manual_l_spin->value(), manual_h_spin->value() );
			}
			else {
				// Use the frozen reference bounds if captured and checkbox is on,
				// otherwise sample the live selection.
				const bool  use_ref = ref_captured && use_ref_cb->isChecked();
				const SelBounds s   = use_ref ? ref_bounds : query_sel( step_x < step_y ? step_x : step_y );
				if ( s.valid ) {
					target.min_x = s.x0;
					target.max_x = s.x1;
					target.min_y = s.y0;
					target.max_y = s.y1;
					target.min_z = s.z0;
					target.max_z    = s.z0 + std::max( s.z1 - s.z0, 64.0 );
					target.width_x  = target.max_x - target.min_x;
					target.length_y = target.max_y - target.min_y;
					target.height_z = target.max_z - target.min_z;

					// Capture reference on first generation (live bounds only, not ref replay).
					if ( !ref_captured && !use_ref ) {
						ref_bounds   = s;
						ref_captured = true;
						ref_w_label->setText( "  Ref (X): " + QString::number( (int)( s.x1 - s.x0 ) ) );
						ref_l_label->setText( "  Ref (Y): " + QString::number( (int)( s.y1 - s.y0 ) ) );
						ref_h_label->setText( "  Ref (Z): " + QString::number( (int)std::max( s.z1 - s.z0, 64.0 ) ) );
						ref_w_label->show();
						ref_l_label->show();
						ref_h_label->show();
						use_ref_cb->show();
						if ( target_combo->currentIndex() == 0 )
							set_row_visible( use_ref_cb, true );
					}
				}
				else {
					target = make_manual_brush_data( manual_w_spin->value(), manual_l_spin->value(), manual_h_spin->value() );
				}
			}

			// For Slope / Slope Tunnel in Use Selection mode, derive the slope
			// height from the brush's Z extent so the terrain descends from the
			// top of the brush down to a minimum height of 64 units.
			// A negative value flips the engine's formula (base_z = height * nx)
			// so it slopes downward instead of upward.
			// For Slope/SlopeTunnel in Use Selection mode the spinbox holds the
			// drop amount (auto-filled from brush Z − 64). Negate it so the
			// engine formula (base_z = shape_height * nx) slopes downward.
			const bool   slope_from_sel = !use_manual
			                           && ( shape == ShapeType::Slope || shape == ShapeType::SlopeTunnel );
			// Spinbox shows the full brush Z height. For the downward slope the
			// engine needs the drop amount (full_z − 64), negated so the
			// formula base_z = shape_height*nx descends to min_z + 64.
			const double shape_height   = slope_from_sel
			                           ? -( shape_height_spin->value() - 64.0 )
			                           : shape_height_spin->value();

			// --- Delete the selection brush now that its bounds are captured ---
			if ( !use_manual && !preview_mode ) {
				const int sel_count = (int)GlobalSelectionSystem().countSelected();
				for ( int i = 0; i < sel_count && GlobalSelectionSystem().countSelected() > 0; ++i )
					Path_deleteTop( GlobalSelectionSystem().ultimateSelected().path() );
				SceneChangeNotify();
			}

			if ( preview_mode ) {
				clear_preview();
			}

			adjust_bounds_to_fit_grid( target, step_x, step_y );
			const MaskMap mask_map = build_mask_map( target, step_x, step_y, mask_preset, mask_image_edit->text() );

			// --- Generate ---
			const bool is_tunnel = ( shape == ShapeType::Tunnel || shape == ShapeType::SlopeTunnel );

			globalOutputStream() << "TerrainGenerator: generating "
			                     << ( is_tunnel ? "tunnel" : "terrain" )
			                     << " — bounds ("
			                     << target.width_x << " x " << target.length_y << " x " << target.height_z
			                     << "), step (" << step_x << " x " << step_y << ")"
			                     << ", seed: " << seed
			                     << ", texture: " << texture << "\n";

			TerrainBuildOptions build_options;
			build_options.preview  = preview_mode;
			build_options.undoable = !preview_mode;

			if ( is_tunnel ) {
				const double cave_height    = ( shape == ShapeType::SlopeTunnel ) ? tun_height   : shape_height;
				const double slope_height   = ( shape == ShapeType::SlopeTunnel ) ? shape_height : 0;
				const double tunnel_terrace = ( shape == ShapeType::SlopeTunnel ) ? terrace      : 0.0;
				auto maps = generate_tunnel_height_maps( target, step_x, step_y, cave_height, slope_height, variance, frequency, mask_map, noise, tunnel_terrace, seed );
				build_tunnel_brushes( target, step_x, step_y, maps, texture, material_slots, material_rules, cave_height, slope_height,
				                     build_options, preview_mode ? &preview_entities : nullptr );
			}
			else {
				bool split_diagonally = ( variance > 0 || shape != ShapeType::Flat );
				auto height_map = generate_height_map( target, step_x, step_y, shape, shape_height, variance, frequency, mask_map,
				                                      noise, terrace, curve_radius, bank_angle, ramp_length, post_process, seed );
				build_terrain_brushes( target, step_x, step_y, height_map, texture, material_slots, material_rules, split_diagonally,
				                      build_options, preview_mode ? &preview_entities : nullptr );
			}

			g_last_seed = seed;
			g_has_last_seed = true;
			preview_active = preview_mode;
			globalOutputStream() << "TerrainGenerator: generation complete\n";
			return true;
		};

		auto rerender_preview_if_active = [&](){
			if ( preview_active ) {
				do_generate( true );
			}
		};

		QObject::connect( preview_btn,  &QPushButton::clicked, [&](){ do_generate( true ); } );
		QObject::connect( generate_btn, &QPushButton::clicked, [&](){
			clear_preview();
			do_generate( false );
		} );
		QObject::connect( close_btn,    &QPushButton::clicked, [&](){
			clear_preview();
			dialog.reject();
		} );

		// Shape height label per shape type (index matches ShapeType enum value)
		static const char* shape_height_label[] = {
			nullptr,           // Flat — row hidden
			"Peak Height:",    // Hill
			"Crater Depth:",   // Crater
			"Ridge Height:",   // Ridge
			"Slope Height:",   // Slope
			"Volcano Height:", // Volcano
			"Valley Depth:",   // Valley
			"Tunnel Height:",  // Tunnel
			"Slope Height:",   // SlopeTunnel
			"Turn Height:",    // BankedTurn
			"Berm Height:",    // Berm
			"Ramp Height:",    // JumpRamp
			"Whoop Height:",   // Whoops
		};

		// Returns true when slope height should be derived from the selection
		// (Use Selection mode + Slope or Slope Tunnel shape).
		auto slope_derived = [&]() -> bool {
			const ShapeType st = (ShapeType)shape_combo->currentData().toInt();
			return target_combo->currentIndex() == 0
			    && ( st == ShapeType::Slope || st == ShapeType::SlopeTunnel );
		};

		// Target mode toggle: read-only selection labels vs editable spinboxes.
		// Refresh labels each time the user switches to "Use Selection" so they
		// reflect whatever is selected at that moment.
		auto update_target_mode = [&]( int idx ){
			const bool use_sel = ( idx == 0 );
			if ( use_sel ) {
				refresh_sel_labels();
				if ( slope_derived() ) {
					const SelBounds s = query_sel();
					if ( s.valid )
						shape_height_spin->setValue( s.z1 - s.z0 );
				}
			}
			set_row_visible( sel_w_row,     use_sel );
			set_row_visible( sel_l_row,     use_sel );
			set_row_visible( sel_h_row,     use_sel );
			set_row_visible( manual_w_spin, !use_sel );
			set_row_visible( manual_l_spin, !use_sel );
			set_row_visible( manual_h_spin, !use_sel );
			// Only toggle the checkbox row once a reference has been captured
			// (it must never appear in Manual Size mode).
			if ( ref_captured )
				set_row_visible( use_ref_cb, use_sel );
		};

		// Advanced sub-square toggle
		auto update_advanced = [&]( bool advanced ){
			sq_combo->setVisible( !advanced );
			set_row_visible( step_x_spin, advanced );
			set_row_visible( step_y_spin, advanced );
		};

		// Shape type toggle: labels + visibility
		auto update_shape = [&]( int idx ){
			const ShapeType st         = (ShapeType)shape_combo->itemData( idx ).toInt();
			const bool is_flat         = ( st == ShapeType::Flat );
			const bool is_slope_tunnel = ( st == ShapeType::SlopeTunnel );
			const bool is_banked_turn  = ( st == ShapeType::BankedTurn );
			const bool uses_ramp_len   = ( st == ShapeType::JumpRamp || st == ShapeType::Whoops );

			set_row_visible( shape_height_spin, !is_flat );
			if ( !is_flat ) {
				if ( auto *lbl = qobject_cast<QLabel*>( form->labelForField( shape_height_spin ) ) )
					lbl->setText( shape_height_label[idx] );
				// Auto-fill slope height from selection when applicable
				if ( slope_derived() ) {
					const SelBounds s = query_sel();
					if ( s.valid )
						shape_height_spin->setValue( s.z1 - s.z0 );
				}
			}
			set_row_visible( tunnel_height_spin, is_slope_tunnel );
			set_row_visible( curve_radius_spin, is_banked_turn );
			set_row_visible( banking_angle_spin, is_banked_turn );
			set_row_visible( ramp_length_spin, uses_ramp_len );
			// Terrace not applicable to flat tunnels (no slope to step),
			// but valid for slope tunnels where the floor descends along Y
			set_row_visible( terrace_spin, !is_flat && st != ShapeType::Tunnel );
		};

		auto update_mask_controls = [&]( int idx ){
			const MaskPreset preset = (MaskPreset)mask_preset_combo->itemData( idx ).toInt();
			set_row_visible( mask_image_widget, preset == MaskPreset::ImageImport );
		};

		auto apply_post_process_preset = [&](){
			const PostProcessPreset preset = (PostProcessPreset)post_process_combo->currentData().toInt();
			switch ( preset ) {
			case PostProcessPreset::Subtle:
				laplacian_spin->setValue( 1 );
				thermal_spin->setValue( 2 );
				hydraulic_spin->setValue( 1 );
				break;
			case PostProcessPreset::Medium:
				laplacian_spin->setValue( 2 );
				thermal_spin->setValue( 4 );
				hydraulic_spin->setValue( 3 );
				break;
			case PostProcessPreset::Aggressive:
				laplacian_spin->setValue( 4 );
				thermal_spin->setValue( 8 );
				hydraulic_spin->setValue( 6 );
				break;
			default:
				break;
			}
		};

		auto sync_post_process_preset = [&](){
			const int lap = laplacian_spin->value();
			const int thr = thermal_spin->value();
			const int hyd = hydraulic_spin->value();
			PostProcessPreset matched = PostProcessPreset::Custom;
			if ( lap == 1 && thr == 2 && hyd == 1 ) {
				matched = PostProcessPreset::Subtle;
			}
			else if ( lap == 2 && thr == 4 && hyd == 3 ) {
				matched = PostProcessPreset::Medium;
			}
			else if ( lap == 4 && thr == 8 && hyd == 6 ) {
				matched = PostProcessPreset::Aggressive;
			}

			const int idx = post_process_combo->findData( (int)matched );
			if ( idx >= 0 && post_process_combo->currentIndex() != idx ) {
				post_process_combo->setCurrentIndex( idx );
			}
		};

		// Wire signals
		QObject::connect( target_combo, QOverload<int>::of( &QComboBox::currentIndexChanged ), update_target_mode );
		QObject::connect( sq_advanced,  &QCheckBox::toggled,                                   update_advanced );
		QObject::connect( shape_combo,  QOverload<int>::of( &QComboBox::currentIndexChanged ), update_shape );
		QObject::connect( mask_preset_combo, QOverload<int>::of( &QComboBox::currentIndexChanged ), update_mask_controls );
		QObject::connect( target_combo, QOverload<int>::of( &QComboBox::currentIndexChanged ), [&]( int ){ rerender_preview_if_active(); } );
		QObject::connect( sq_advanced,  &QCheckBox::toggled,                                   [&]( bool ){ rerender_preview_if_active(); } );
		QObject::connect( shape_combo,  QOverload<int>::of( &QComboBox::currentIndexChanged ), [&]( int ){ rerender_preview_if_active(); } );
		QObject::connect( noise_combo,  QOverload<int>::of( &QComboBox::currentIndexChanged ), [&]( int ){ rerender_preview_if_active(); } );
		QObject::connect( post_process_combo,  QOverload<int>::of( &QComboBox::currentIndexChanged ), [&]( int ){
			apply_post_process_preset();
			rerender_preview_if_active();
		} );
		QObject::connect( use_ref_cb,   &QCheckBox::toggled,                                   [&]( bool ){ rerender_preview_if_active(); } );
		QObject::connect( auto_seed_cb, &QCheckBox::toggled,                                   [&]( bool ){ rerender_preview_if_active(); } );
		QObject::connect( manual_w_spin, QOverload<int>::of( &QSpinBox::valueChanged ), [&]( int ){ rerender_preview_if_active(); } );
		QObject::connect( manual_l_spin, QOverload<int>::of( &QSpinBox::valueChanged ), [&]( int ){ rerender_preview_if_active(); } );
		QObject::connect( manual_h_spin, QOverload<int>::of( &QSpinBox::valueChanged ), [&]( int ){ rerender_preview_if_active(); } );
		QObject::connect( step_x_spin, QOverload<int>::of( &QSpinBox::valueChanged ), [&]( int ){ rerender_preview_if_active(); } );
		QObject::connect( step_y_spin, QOverload<int>::of( &QSpinBox::valueChanged ), [&]( int ){ rerender_preview_if_active(); } );
		QObject::connect( shape_height_spin, QOverload<double>::of( &QDoubleSpinBox::valueChanged ), [&]( double ){ rerender_preview_if_active(); } );
		QObject::connect( tunnel_height_spin, QOverload<double>::of( &QDoubleSpinBox::valueChanged ), [&]( double ){ rerender_preview_if_active(); } );
		QObject::connect( curve_radius_spin, QOverload<double>::of( &QDoubleSpinBox::valueChanged ), [&]( double ){ rerender_preview_if_active(); } );
		QObject::connect( banking_angle_spin, QOverload<double>::of( &QDoubleSpinBox::valueChanged ), [&]( double ){ rerender_preview_if_active(); } );
		QObject::connect( ramp_length_spin, QOverload<double>::of( &QDoubleSpinBox::valueChanged ), [&]( double ){ rerender_preview_if_active(); } );
		QObject::connect( terrace_spin, QOverload<double>::of( &QDoubleSpinBox::valueChanged ), [&]( double ){ rerender_preview_if_active(); } );
		QObject::connect( laplacian_spin, QOverload<int>::of( &QSpinBox::valueChanged ), [&]( int ){
			sync_post_process_preset();
			rerender_preview_if_active();
		} );
		QObject::connect( thermal_spin, QOverload<int>::of( &QSpinBox::valueChanged ), [&]( int ){
			sync_post_process_preset();
			rerender_preview_if_active();
		} );
		QObject::connect( hydraulic_spin, QOverload<int>::of( &QSpinBox::valueChanged ), [&]( int ){
			sync_post_process_preset();
			rerender_preview_if_active();
		} );
		QObject::connect( variance_spin, QOverload<double>::of( &QDoubleSpinBox::valueChanged ), [&]( double ){ rerender_preview_if_active(); } );
		QObject::connect( frequency_spin, QOverload<double>::of( &QDoubleSpinBox::valueChanged ), [&]( double ){ rerender_preview_if_active(); } );
		QObject::connect( mask_preset_combo, QOverload<int>::of( &QComboBox::currentIndexChanged ), [&]( int ){ rerender_preview_if_active(); } );
		QObject::connect( mask_image_edit, &QLineEdit::textChanged, [&]( const QString& ){ rerender_preview_if_active(); } );
		QObject::connect( seed_spin, QOverload<int>::of( &QSpinBox::valueChanged ), [&]( int ){ rerender_preview_if_active(); } );
		QObject::connect( texture_edit, &QLineEdit::textChanged, [&]( const QString& ){ rerender_preview_if_active(); } );
		QObject::connect( steep_material_edit, &QLineEdit::textChanged, [&]( const QString& ){ rerender_preview_if_active(); } );
		QObject::connect( peak_material_edit, &QLineEdit::textChanged, [&]( const QString& ){ rerender_preview_if_active(); } );
		QObject::connect( dirt_material_edit, &QLineEdit::textChanged, [&]( const QString& ){ rerender_preview_if_active(); } );
		QObject::connect( track_material_edit, &QLineEdit::textChanged, [&]( const QString& ){ rerender_preview_if_active(); } );
		QObject::connect( steep_angle_spin, QOverload<double>::of( &QDoubleSpinBox::valueChanged ), [&]( double ){ rerender_preview_if_active(); } );
		QObject::connect( peak_min_spin, QOverload<double>::of( &QDoubleSpinBox::valueChanged ), [&]( double ){ rerender_preview_if_active(); } );
		QObject::connect( dirt_min_spin, QOverload<double>::of( &QDoubleSpinBox::valueChanged ), [&]( double ){ rerender_preview_if_active(); } );
		QObject::connect( dirt_max_spin, QOverload<double>::of( &QDoubleSpinBox::valueChanged ), [&]( double ){ rerender_preview_if_active(); } );
		QObject::connect( track_min_spin, QOverload<double>::of( &QDoubleSpinBox::valueChanged ), [&]( double ){ rerender_preview_if_active(); } );
		QObject::connect( track_max_spin, QOverload<double>::of( &QDoubleSpinBox::valueChanged ), [&]( double ){ rerender_preview_if_active(); } );

		QObject::connect( auto_seed_cb, &QCheckBox::toggled, [&]( bool checked ){
			seed_spin->setEnabled( !checked );
			if ( checked )
				seed_spin->setValue( make_auto_seed() );
		} );

		QObject::connect( mask_image_pick, &QPushButton::clicked, [&](){
			const QString path = QFileDialog::getOpenFileName( &dialog, "Select Grayscale Mask (PNG)", QString(), "PNG Files (*.png)" );
			if ( !path.isEmpty() )
				mask_image_edit->setText( path );
		} );

		// Set initial visibility
		update_target_mode( target_combo->currentIndex() );
		update_advanced( false );
		update_shape( shape_combo->currentIndex() );
		update_mask_controls( mask_preset_combo->currentIndex() );
		apply_post_process_preset();

		// show() instead of exec() so the dialog is non-modal — the texture
		// browser panel (and all other Radiant windows) remain interactive
		dialog.show();
		dialog.raise();
		{
			QEventLoop loop;
			QObject::connect( &dialog, &QDialog::finished, &loop, &QEventLoop::quit );
			loop.exec();
		}
		clear_preview();
	}
}

} // namespace terrain_generator

class TerrainGeneratorDependencies :
	public GlobalRadiantModuleRef,
	public GlobalUndoModuleRef,
	public GlobalSceneGraphModuleRef,
	public GlobalSelectionModuleRef,
	public GlobalEntityModuleRef,
	public GlobalEntityClassManagerModuleRef,
	public GlobalBrushModuleRef
{
public:
	TerrainGeneratorDependencies() :
		GlobalEntityModuleRef( GlobalRadiant().getRequiredGameDescriptionKeyValue( "entities" ) ),
		GlobalEntityClassManagerModuleRef( GlobalRadiant().getRequiredGameDescriptionKeyValue( "entityclass" ) ),
		GlobalBrushModuleRef( GlobalRadiant().getRequiredGameDescriptionKeyValue( "brushtypes" ) ){
	}
};

class TerrainGeneratorModule : public TypeSystemRef
{
	_QERPluginTable m_plugin;
public:
	typedef _QERPluginTable Type;
	STRING_CONSTANT( Name, "TerrainGenerator" );

	TerrainGeneratorModule(){
		m_plugin.m_pfnQERPlug_Init                = &terrain_generator::init;
		m_plugin.m_pfnQERPlug_GetName             = &terrain_generator::getName;
		m_plugin.m_pfnQERPlug_GetCommandList      = &terrain_generator::getCommandList;
		m_plugin.m_pfnQERPlug_GetCommandTitleList = &terrain_generator::getCommandTitleList;
		m_plugin.m_pfnQERPlug_Dispatch            = &terrain_generator::dispatch;
	}

	_QERPluginTable* getTable(){
		return &m_plugin;
	}
};

typedef SingletonModule<TerrainGeneratorModule, TerrainGeneratorDependencies> SingletonTerrainGeneratorModule;
SingletonTerrainGeneratorModule g_TerrainGeneratorModule;

extern "C" void RADIANT_DLLEXPORT Radiant_RegisterModules( ModuleServer& server ){
	initialiseModule( server );
	g_TerrainGeneratorModule.selfRegister();
}
