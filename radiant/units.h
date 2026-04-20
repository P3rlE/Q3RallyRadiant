#pragma once

#include "math/vector.h"
#include "string/string.h"

enum class DisplayUnit {
	QuakeUnits,
	Meters,
	Miles,
};

extern double meters_per_quake_unit;

DisplayUnit getDisplayUnit();
void setDisplayUnit( DisplayUnit displayUnit );
void setMetersPerQuakeUnit( double metersPerQuakeUnit );

double quakeToDisplay( double qu );
const char* displayUnitSuffix();
CopiedString formatDisplayValue( double qu, int decimals );
CopiedString formatDisplayTriplet( const Vector3& qu, int decimals = 1 );
