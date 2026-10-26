#pragma once
#include "simCore/Calc/Angle.h"
#include "simCore/Calc/Coordinate.h"
#include "simCore/Calc/CoordinateConverter.h"
#include <stdexcept>
namespace hezarfen {
inline simCore::Coordinate toEcef(double lat, double lon, double altitude, double heading) {
    simCore::Coordinate lla;
    lla.setCoordinateSystem(simCore::COORD_SYS_LLA);
    lla.setPositionLLA(lat * simCore::DEG2RAD,lon * simCore::DEG2RAD,altitude);
    lla.setOrientation(heading * simCore::DEG2RAD,0,0);
    simCore::Coordinate ecef;
    if (simCore::CoordinateConverter::convertGeodeticToEcef(lla,ecef))
        throw std::runtime_error("SIMDIS WGS84 to ECEF conversion failed");
    return ecef;
}
}
