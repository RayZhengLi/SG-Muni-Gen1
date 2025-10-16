#ifndef SG_GPS_H
#define SG_GPS_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    unsigned int tof;
    double lat;
    double lon;
    float alt;
    float speed;
    unsigned int heading;
    unsigned int hdop;
    unsigned int nsats;
    unsigned int fixstatus;
}GPSData;

// Update the internal static GPS data and return a pointer to it
GPSData *gps_get_data();

// Get current location and update the provided structure
bool gps_get_current_location(GPSData *location);

#endif