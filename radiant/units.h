#pragma once

#include "math/vector.h"
#include "string/string.h"

enum class DisplayUnit {
	QuakeUnits = 0,
	Meters = 1,
	Miles = 2,
};

extern double meters_per_quake_unit;

DisplayUnit getDisplayUnit();
void setDisplayUnit( DisplayUnit displayUnit );
DisplayUnit displayUnitFromInt( int value );
int displayUnitToInt( DisplayUnit displayUnit );
void setMetersPerQuakeUnit( double metersPerQuakeUnit );

double quakeToDisplay( double qu );
const char* displayUnitSuffix();
CopiedString formatDisplayValue( double qu, int decimals );
CopiedString formatDisplayTriplet( const Vector3& qu, int decimals = 1 );
int displayUnitDefaultDecimals();
