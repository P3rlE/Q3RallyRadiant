#include "units.h"

#include <cstdio>


double meters_per_quake_unit = 0.0254;

namespace {
DisplayUnit g_displayUnit = DisplayUnit::QuakeUnits;
constexpr double meters_per_inch = 0.0254;

DisplayUnit sanitizeDisplayUnit( DisplayUnit displayUnit ){
	switch ( displayUnit ) {
	case DisplayUnit::QuakeUnits:
	case DisplayUnit::Meters:
	case DisplayUnit::Inches:
		return displayUnit;
	default:
		return DisplayUnit::QuakeUnits;
	}
}

CopiedString formatFixed( double value, int decimals ){
	if ( decimals < 0 ) {
		decimals = 0;
	}

	char buffer[64];
	std::snprintf( buffer, sizeof( buffer ), "%.*f", decimals, value );
	return buffer;
}
}

DisplayUnit getDisplayUnit(){
	return g_displayUnit;
}

void setDisplayUnit( DisplayUnit displayUnit ){
	g_displayUnit = sanitizeDisplayUnit( displayUnit );
}

DisplayUnit displayUnitFromInt( int value ){
	switch ( value ) {
	case 1:
		return DisplayUnit::Meters;
	case 2:
		return DisplayUnit::Inches;
	case 0:
	default:
		return DisplayUnit::QuakeUnits;
	}
}

int displayUnitToInt( DisplayUnit displayUnit ){
	switch ( displayUnit ) {
	case DisplayUnit::Meters:
		return 1;
	case DisplayUnit::Inches:
		return 2;
	case DisplayUnit::QuakeUnits:
	default:
		return 0;
	}
}

void setMetersPerQuakeUnit( double metersPerQuakeUnit ){
	if ( metersPerQuakeUnit > 0.0 ) {
		meters_per_quake_unit = metersPerQuakeUnit;
	}
}

double quakeToDisplay( double qu ){
	switch ( g_displayUnit ) {
	case DisplayUnit::Meters:
		return qu * meters_per_quake_unit;
	case DisplayUnit::Inches:
		return qu * meters_per_quake_unit / meters_per_inch;
	case DisplayUnit::QuakeUnits:
	default:
		return qu;
	}
}

const char* displayUnitSuffix(){
	switch ( g_displayUnit ) {
	case DisplayUnit::Meters:
		return "m";
	case DisplayUnit::Inches:
		return "in";
	case DisplayUnit::QuakeUnits:
	default:
		return "qu";
	}
}

CopiedString formatDisplayValue( double qu, int decimals ){
	return formatFixed( quakeToDisplay( qu ), decimals );
}

CopiedString formatDisplayTriplet( const Vector3& qu, int decimals ){
	const CopiedString x = formatDisplayValue( qu[0], decimals );
	const CopiedString y = formatDisplayValue( qu[1], decimals );
	const CopiedString z = formatDisplayValue( qu[2], decimals );

	char buffer[256];
	std::snprintf( buffer, sizeof( buffer ), "%s %s %s %s", x.c_str(), y.c_str(), z.c_str(), displayUnitSuffix() );
	return buffer;
}

int displayUnitDefaultDecimals(){
	switch ( g_displayUnit ) {
	case DisplayUnit::Meters:
		return 2;
	case DisplayUnit::Inches:
		return 1;
	case DisplayUnit::QuakeUnits:
	default:
		return 1;
	}
}
