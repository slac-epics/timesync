#include <iocsh.h>
#include <callback.h>
#include <dbScan.h>
#include <dbAccess.h>
#include <cantProceed.h>
#include <epicsThread.h>
#include <epicsExport.h>
#include <registryFunction.h>
#include <errlog.h>
#include <epicsVersion.h>
#include <unistd.h>
#include "timesync.h"
#include <stdint.h>
#include <timingFifoApi.h>

using namespace		std;

int sync_debug = 2;
int sync_cnt   = 200;
#define LCLS1_FID_MAX        0x1ffe0
#define LCLS1_FID_ROLL_LO    0x00200
#define LCLS1_FID_ROLL_HI    (LCLS1_FID_MAX-LCLS1_FID_ROLL_LO)
#define LCLS1_FID_ROLL(a,b)  ((b) < LCLS1_FID_ROLL_LO && (a) > LCLS1_FID_ROLL_HI)
#define LCLS1_FID_GT(a,b)    (LCLS1_FID_ROLL(b, a) || ((a) > (b) && !LCLS1_FID_ROLL(a, b)))
#define LCLS1_FID_DIFF(a,b)  ((LCLS1_FID_ROLL(b, a) ? LCLS1_FID_MAX : 0) + \
                              (int)(a) - (int)(b) - (LCLS1_FID_ROLL(a, b) ? LCLS1_FID_MAX : 0))
#define LCLS2_FID_DIFF(a,b)  ((int)(((a)<LCLS1_FID_MAX&&(b)<LCLS1_FID_MAX)?LCLS1_FID_DIFF(a,b):((a)-(b))))
#define SYNC_DEBUG(n)        (sync_debug > (n) && sync_cnt > 0 && --sync_cnt)
#define SYNC_DEBUG_ALWAYS(n) (sync_debug > (n))
#define SET_SYNC(v)                                   \
    do {                                              \
        in_sync = v;                                  \
        if (have_syncpv)                              \
            dbPutField(&addr, DBR_LONG, &in_sync, 1); \
    } while (0)
#define SYNC_ERROR(level, msg)     \
        if (SYNC_DEBUG(level)) {   \
            printf msg;            \
            DebugPrint(dobj);\
            fflush(stdout);        \
        }                          \
        SET_SYNC(0);               \
        continue

/* Parameters for LCLS-I */
static int sync_retry  = 0;    
static int sync_future = 0;
static int sync_far    = 0;
static int sync_vfar   = 0;
static int sync_notfar = 0;

static struct SyncGlobalParams globs = {
    3,    3,       /* retry  - How many "notfar" events to see before declaring sync. */
    1,    2580,    /* future - How many fiducials in the future a timestamp can match the delayed fiducial */
    2,    5160,    /* far    - How far off can we be for the initial sync? */
    3,    7740,    /* vfar   - How far off can we be before declaring that we aren't synced? */
    3,    7740,    /* notfar - How close do we have to be to declare a match? */
};

void SyncObject::SetGlobalParams(struct SyncGlobalParams *gnew)
{
    if (gnew)
        globs = *gnew;
    if (m_mode) {
        if (*m_mode) {
            printf("Setting LCLS2 Synchronization parameters.\n");
            sync_retry  = globs.l2_retry;
            sync_future = globs.l2_future;
            sync_far    = globs.l2_far;
            sync_vfar   = globs.l2_vfar;
            sync_notfar = globs.l2_notfar;
        } else {
            printf("Setting LCLS1 Synchronization parameters.\n");
            sync_retry  = globs.l1_retry;
            sync_future = globs.l1_future;
            sync_far    = globs.l1_far;
            sync_vfar   = globs.l1_vfar;
            sync_notfar = globs.l1_notfar;
        }
    }
}

int SyncObject::poll(void)
{
    unsigned int gen;
    int trigevent;
    int eventvalid;
    DataObject *dobj = NULL;
    TimingPulseId delayfid, tsfid = -1, lastdatafid = -1;
    int in_sync;
    int do_print = 0;
    uint64_t idx = 0;
    EventTimingData evt_info;
    int status = 0;
    int attributes;
    double lastdelay = 0.0;
    DBADDR addr;
    int have_syncpv = 0;
    const char *syncpvname = m_syncpv.c_str();
    TimingPulseId lastdelayfid = -1, lasttsfid = -1;
    unsigned int mode;

    attributes = Attributes();
    lastdelay = *m_delay;
    mode = *m_mode;
    SetGlobalParams(NULL);

    if (syncpvname && syncpvname[0] && !dbNameToAddr(syncpvname, &addr))
        have_syncpv = 1;

    trigevent = m_gen ? *m_event : -1;
    gen       = m_gen ? *m_gen : 0;
    printf("Initial Gen = %d\n", gen);
    SET_SYNC(m_gen ? 0 : 1);
    eventvalid = trigevent > 0 && trigevent < 256;

    while(TRUE) {
        if (dobj)
            delete dobj;
        dobj = Acquire();
        delayfid = timingGetLastFiducial() - (int)(lastdelay + 0.5);
        while (delayfid == lastdatafid) {
            struct timespec req = {0, 1000000}; /* 1 ms */
            nanosleep(&req, NULL);
            delayfid = timingGetLastFiducial() - (int)(lastdelay + 0.5);
        }
        if (!dobj || CheckError(dobj)) {
            printf("Timesync found error?\n");
            gen = -1;
            continue;
        }

        if ((m_gen && (gen != *m_gen || lastdelay != *m_delay)) || (mode != *m_mode)) {
            /* Either the timing info changed or the delay calculation changed
               or the timing mode changed, so force a resync! */
            printf("gen = %d, *m_gen = %d, lastdelay = %lf, *m_delay = %lf, mode = %d, *m_mode = %d\n",
                   gen, *m_gen, lastdelay, *m_delay, mode, *m_mode);
            trigevent = *m_event;
            gen = *m_gen;
            printf("Timesync Gen = %d\n", gen);
            SET_SYNC(0);
            if (mode != *m_mode) {
                printf("Switching to LCLS-%s mode!\n", *m_mode ? "II" : "I");
                mode = *m_mode;
                SetGlobalParams(NULL);
            }
            if (eventvalid) {
                eventvalid = trigevent > 0 && trigevent < 256;
                if (eventvalid)
                    printf("%s is setting event trigger to %d.\n", Name(), trigevent);
                else
                    printf("%s has invalid event trigger %d!\n", Name(), trigevent);
                DebugPrint(dobj);
                fflush(stdout);
            } else {
                eventvalid = trigevent > 0 && trigevent < 256;
                if (eventvalid) {
                    printf("%s is setting event trigger to %d.\n", Name(), trigevent);
                    DebugPrint(dobj);
                    fflush(stdout);
                }
            }
            lastdelay = *m_delay;
            continue;
        }

        if (!in_sync) {     /* Try to resynchronize! */
            if (!eventvalid)
                continue;

            if (SYNC_DEBUG(0)) {
                printf("%s resynchronizing at fiducial 0x%llx (delay=%lg).\n",
                       Name(), timingGetLastFiducial(), *m_delay);
            }

            if (gen != *m_gen) {
                printf("Generation change, restarting!\n");
                continue;   /* Ow... a reconfigure while reconfiguring.  Just start over. */
            }

            /* Get the current time! */
            status = timingFifoRead(trigevent, TS_INDEX_INIT, &idx, &evt_info);
            tsfid = evt_info.fifo_fid;
            if (tsfid == TIMING_PULSEID_INVALID) { /* Sigh.  Restart if the fiducial is bad. */
                if (SYNC_DEBUG(0)) {
                    printf("%s has bad fiducial at time %08x:%08x.\n", Name(),
                           evt_info.fifo_time.secPastEpoch, evt_info.fifo_time.nsec);
                    fflush(stdout);
                }
                continue;
            }

            if (SYNC_DEBUG(1)) {
                printf("Got data: lastfid=%llx delayfid=%llx tsfid=%llx\n", timingGetLastFiducial(), delayfid, tsfid);
                fflush(stdout);
            }

            while (LCLS2_FID_DIFF(tsfid, delayfid) > sync_future) {
                /*
                 * The most recent event timestamp (tsfid) is significantly more recent 
                 * than the the most recent trigger fiducial.
                 *
                 * Go back until we get an event before this time.
                 */
                status = timingFifoRead(trigevent, -1, &idx, &evt_info);
                tsfid = evt_info.fifo_fid;
                if (tsfid == TIMING_PULSEID_INVALID) {
                    if (SYNC_DEBUG(0)) {
                        printf("%s resync sees a bad fiducial, restarting!\n", Name());
                        fflush(stdout);
                    }
                    break;
                }
                if (SYNC_DEBUG(0)) {
                    printf("%s is moving back to timestamp fiducial 0x%llx at index %llu.\n",
                           Name(), tsfid, idx);
                    fflush(stdout);
                }
            }
            if (tsfid == TIMING_PULSEID_INVALID) {
                continue;
            }

            /*
             * Our trigger was close to delayfid.  However, sometimes we don't have the
             * timestamp yet.  Wait until we receive it!
             */
            if (LCLS2_FID_DIFF(delayfid, tsfid) > sync_far) {
                SYNC_ERROR(0, ("%s is still way off! delayfid = 0x%llx, tsfid = 0x%llx, restarting.\n",
                               Name(), delayfid, tsfid));
            }

            if (gen != *m_gen || tsfid == TIMING_PULSEID_INVALID) {
                /* This is just bad.  When in doubt, start over. */
                if (SYNC_DEBUG(0)) {
                    printf("%s resync failed with timestamp fiducial 0x%llx, restarting!\n",
                           Name(), tsfid);
                    fflush(stdout);
                }
                continue;
            }

            /*
             * We should probably check that we are, indeed, close.  What if tsfid skipped
             * *past* our real fiducial?  (Unlikely, I know.)
             */
            if (SYNC_DEBUG(0)) {
                printf("%s resync established with index %llu at timestamp fiducial 0x%llx at delayed fiducial 0x%llx.\n",
                       Name(), idx, tsfid, delayfid);
                DebugPrint(dobj);
                fflush(stdout);
            }
            SET_SYNC(1);
            do_print = sync_retry; /* Double check the next few times through the loop! */
            lastdatafid = tsfid;
            continue;
        }

        assert(in_sync == 1);

        if (m_gen) {
            int incr;

            if (attributes & SyncObject::HasCount) {
                /* If we have a counter, use it to figure out how many timestamps to skip. */
                incr = CountIncr(dobj);
                if (incr < 0) {
                    SYNC_ERROR(0, ("Lost sync in CountIncr!\n"));
                }
            } else
                incr = 1;
            status = timingFifoRead(trigevent, incr, &idx, &evt_info);
            tsfid = evt_info.fifo_fid;

            if (status || tsfid == TIMING_PULSEID_INVALID) {
                uint64_t now;
                timingFifoRead(trigevent, TS_INDEX_INIT, &now, &evt_info); /* Where are we? */
                tsfid = evt_info.fifo_fid;
                if (now + 1 == idx) {
                    /* OK, we seem to be a tad early?!?  Just wait for it! */
                    do {
                        struct timespec req = {0, 1000000}; /* 1 ms */
                        nanosleep(&req, NULL);
                        timingFifoRead(trigevent, TS_INDEX_INIT, &now, &evt_info);
                        tsfid = evt_info.fifo_fid;
                    } while (now + 1 == idx);
                    status = timingFifoRead(trigevent, 0, &idx, &evt_info);
                    tsfid = evt_info.fifo_fid;
                }
            }
            if (status) {
                SYNC_ERROR(0, ("%s has an invalid timestamp, resynching!\n", Name()));
            }
            if (tsfid == TIMING_PULSEID_INVALID) {
                SYNC_ERROR(0, ("Invalid fiducial! (delayed fid 0x%llx)\n", delayfid));
            }

            if (attributes & SyncObject::HasTime) {
                /* If we have a timestamp, use it to calculate the expected fiducial, and then
                   search for a timestamp in the queue close to this one. */
                int fiddiff = FidDiff(dobj);
                if (fiddiff < 0) {
                    SYNC_ERROR(0, ("Lost sync in Fiducial!\n"));
                }
                TimingPulseId fid = lastdatafid + fiddiff;
                if (tsfid < LCLS1_FID_MAX) { /* LCLS1 - Rollaround! */
                    while (fid >= LCLS1_FID_MAX)
                        fid -= LCLS1_FID_MAX;
                }
                while (!status && LCLS2_FID_DIFF(fid, tsfid) >= sync_vfar) {
                    status = timingFifoRead(trigevent, 1, &idx, &evt_info);
                    tsfid = evt_info.fifo_fid;
                }
                if (status) {
                    SYNC_ERROR(0, ("%s has an invalid timestamp, resynching (lastdata=0x%llx, fd=%d)!\n",
                                   Name(), lastdatafid, fiddiff));
                }
                if (tsfid == TIMING_PULSEID_INVALID) {
                    SYNC_ERROR(0, ("Invalid fiducial! (expected fid 0x%llx)\n", fid));
                }
                /* We're keeping it tight here for the initial sync! */
                if (abs(LCLS2_FID_DIFF(fid, tsfid)) >= sync_far) {
                    SYNC_ERROR(0, ("Lost sync! (timestamp fid 0x%llx, expected fid 0x%llx)\n", tsfid, fid));
                }
            }

            if (attributes & SyncObject::CanSkip) {
                /*
                 * If we can skip data, we have to give up if we are delayed for any reason!
                 */
                if (LCLS2_FID_DIFF(delayfid, tsfid) >= sync_vfar) {
                    SYNC_ERROR(0, ("Lost sync! (timestamp fid 0x%llx, delayed fid 0x%llx, last tsfid = 0x%llx, last delayfid = 0x%llx)\n", tsfid, delayfid, lasttsfid, lastdelayfid));
                }
            }
            lasttsfid = tsfid;
            lastdelayfid = delayfid;

            if (do_print) {
                /*
                 * We think we're synched, but we're still in the checking phase.
                 */
                if (abs(LCLS2_FID_DIFF(tsfid, delayfid)) < sync_notfar) {
                    do_print--;
                    if (SYNC_DEBUG(0)) {
                        if (!do_print)
                            printf("%s is fully resynched with index %llu at timestamp fiducial 0x%llx (0x%llx - %lg = 0x%llx).\n",
                                   Name(), idx, tsfid, timingGetLastFiducial(),
                                   *m_delay, delayfid);
                        else
                            printf("%s has data at fiducial 0x%llx (0x%llx - %lg = 0x%llx).\n",
                                   Name(), tsfid, timingGetLastFiducial(),
                                   *m_delay, delayfid);
                        DebugPrint(dobj);
                        fflush(stdout);
                    }
                    lastdatafid = tsfid;
                    continue;
                } else {
                    SYNC_ERROR(0, ("%s has lost synchronization! (timestamp fid = 0x%llx, delay fid = 0x%llx, diff = %d)\n",
                                   Name(), tsfid, delayfid, abs((int)(tsfid - delayfid))));
                }
            } else {
                if (SYNC_DEBUG_ALWAYS(2)) {
                    printf("%s ts fid=%llx, lastfid=%llx\n", Name(), tsfid, timingGetLastFiducial() );
                    fflush(stdout);
                }
            }
        } else {
            epicsTimeGetEvent(&evt_info.fifo_time, 0);
        }

        QueueData(dobj, evt_info.fifo_time);
        lastdatafid = tsfid;
    }
    return 0;
}


// Debug stuff. 
static const iocshArg	   syncdebugArg0	= { "level",	iocshArgInt };
static const iocshArg	   syncdebugArg1	= { "count",	iocshArgInt };
static const iocshArg	  *syncdebugArgs[2]	= { &syncdebugArg0, &syncdebugArg1 };
static const iocshFuncDef  syncdebugFuncDef	= { "syncdebug", 2, syncdebugArgs };
static int  syncdebugCallFunc(const iocshArgBuf * args)
{
    sync_debug = args[0].ival;
    if (!args[1].ival)
        sync_cnt = 1000;
    else
        sync_cnt = args[1].ival;
    printf("Setting syncdebug to %d (count = %d)\n", sync_debug, sync_cnt);
    return 0;
}

void timesyncRegister(void)
{
    iocshRegister(&syncdebugFuncDef, reinterpret_cast<iocshCallFunc>(syncdebugCallFunc));
}

extern "C"
{
    epicsExportRegistrar(timesyncRegister);
}
