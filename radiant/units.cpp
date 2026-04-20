#include "units.h"

#include <cstdio>

#include "stream/stringstream.h"

double meters_per_quake_unit = 0.0254;

namespace {
DisplayUnit g_displayUnit = DisplayUnit::QuakeUnits;
constexpr double meters_per_mile = 1609.344;

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
	g_displayUnit = displayUnit;
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
	case DisplayUnit::Miles:
		return qu * meters_per_quake_unit / meters_per_mile;
	case DisplayUnit::QuakeUnits:
	default:
		return qu;
	}
}

const char* displayUnitSuffix(){
	switch ( g_displayUnit ) {
	case DisplayUnit::Meters:
		return "m";
	case DisplayUnit::Miles:
		return "mi";
	case DisplayUnit::QuakeUnits:
	default:
		return "qu";
	}
}

CopiedString formatDisplayValue( double qu, int decimals ){
	return formatFixed( quakeToDisplay( qu ), decimals );
}

CopiedString formatDisplayTriplet( const Vector3& qu, int decimals ){
	return StringStream<128>(
		formatDisplayValue( qu[0], decimals ).c_str(), ' ',
		formatDisplayValue( qu[1], decimals ).c_str(), ' ',
		formatDisplayValue( qu[2], decimals ).c_str(), ' ',
		displayUnitSuffix()
	).c_str();
}
