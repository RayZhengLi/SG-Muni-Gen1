#include "sg-gps.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <sys/types.h>
#include <sys/shm.h>

#include <calamp/lmu_gps_shm.h>

static GPS_SHM_DATA gps_raw_data;
static GPSData gps_data;


static int read_gps(GPS_SHM_DATA *p_gps)
{
    int32_t shmkey = 0;
    int32_t shmid = 0;
    GPS_SHM_DATA *shmData = NULL;

    if (p_gps == NULL)
        return 0;

    // get access to modem_emulator's shared memory...
    shmkey = ftok(GPS_SHM_FILE, 1);

    if (shmkey == -1)
    {
        perror("ftok");
        return -1;
    }

    shmid = shmget(shmkey, 0, 0);

    if (shmid == -1)
    {
        perror("shmget");
        return -2;
    }

    shmData = (GPS_SHM_DATA *)shmat(shmid, NULL, SHM_RDONLY);

    if (shmData == (void *)-1)
    {
        perror("shmat");
        return -3;
    }

    // copy shared memory information to local data
    memcpy(p_gps, shmData, sizeof(GPS_SHM_DATA));
    shmdt(shmData);

    return 1;
}

GPSData *gps_get_data(){
    if(read_gps(&gps_raw_data)!=1){
        return NULL;
    }

    gps_data.tof = gps_raw_data.tof;
    gps_data.lat = (double)(gps_raw_data.lat)/10000000.0;
    gps_data.lat = round(gps_data.lat * 10000000) / 10000000.0;
    gps_data.lon = (double)(gps_raw_data.lon)/10000000.0;
    gps_data.lon = round(gps_data.lon * 10000000) / 10000000.0;
    gps_data.alt = (float)(gps_raw_data.alt)/100.0f;
    gps_data.alt = roundf(gps_data.alt * 100) / 100.0f;
    gps_data.speed = (float)(gps_raw_data.spd_cmps)*0.036f;
    gps_data.hdop = gps_raw_data.hdop;
    gps_data.heading = gps_raw_data.heading;
    gps_data.nsats = gps_raw_data.nsats;
    gps_data.fixstatus = gps_raw_data.fixstatus;

    return &gps_data;
}

bool gps_get_current_location(GPSData *location){
    GPS_SHM_DATA gps_raw;
    if(read_gps(&gps_raw)!=1){
        return false;
    }else{
        location->tof = gps_raw.tof;
        location->lat = (double)(gps_raw.lat) / 10000000.0;
        location->lat = round(location->lat * 10000000) / 10000000.0;
    }
}