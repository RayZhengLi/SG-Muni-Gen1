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

GPSData *gps_get_data();

#endif