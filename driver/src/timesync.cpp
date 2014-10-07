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
#include "evrTime.h"

using namespace		std;

int sync_debug = 2;
int sync_cnt   = 200;
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

int SyncObject::poll(void)
{
    unsigned int gen;
    int trigevent;
    int eventvalid;
    DataObject *dobj = NULL;
    int delayfid, tsfid = -1, lastdatafid = -1;
    int in_sync;
    int do_print = 0;
    unsigned long long idx = -1;
    epicsTimeStamp evt_time;
    int status = 0;
    int attributes;
    double lastdelay = 0.0;
    DBADDR addr;
    int have_syncpv = 0;
    const char *syncpvname = m_syncpv.c_str();
    int lastdelayfid = -1, lasttsfid = -1;

    attributes = Attributes();
    lastdelay = *m_delay;

    if (syncpvname && syncpvname[0] && !dbNameToAddr(syncpvname, &addr))
        have_syncpv = 1;

    trigevent = m_gen ? *m_event : -1;
    gen       = m_gen ? *m_gen : 0;
    SET_SYNC(m_gen ? 0 : 1);
    eventvalid = trigevent > 0 && trigevent < 256;

    while(TRUE) {
        if (dobj)
            delete dobj;
        dobj = Acquire();
        delayfid = lastfid - (int)(lastdelay + 0.5);
        if (!dobj || CheckError(dobj)) {
            gen = -1;
            continue;
        }

        if (m_gen && (gen != *m_gen || lastdelay != *m_delay)) {
            /* The trigger event changed or the timing did, so force a resync! */
            trigevent = *m_event;
            gen = *m_gen;
            SET_SYNC(0);

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
                printf("%s resynchronizing at fiducial 0x%x (delay=%lg).\n",
                       Name(), lastfid, *m_delay);
            }

            if (gen != *m_gen) {
                printf("Generation change, restarting!\n");
                continue;   /* Ow... a reconfigure while reconfiguring.  Just start over. */
            }

            /* Get the current time! */
            status = evrTimeGetFifo(&evt_time, trigevent, &idx, MAX_TS_QUEUE);
            tsfid = evt_time.nsec & 0x1ffff;
            if (tsfid == 0x1ffff) { /* Sigh.  Restart if the fiducial is bad. */
                if (SYNC_DEBUG(0)) {
                    printf("%s has bad fiducial at time %08x:%08x.\n", Name(),
                           evt_time.secPastEpoch, evt_time.nsec);
                    fflush(stdout);
                }
                continue;
            }

            if (SYNC_DEBUG(1)) {
                printf("Got data: lastfid=%x delayfid=%x tsfid=%x\n", lastfid, delayfid, tsfid);
                fflush(stdout);
            }

            while (FID_DIFF(tsfid, delayfid) > 1) {
                /*
                 * The most recent event timestamp (tsfid) is significantly more recent 
                 * than the the most recent trigger fiducial.
                 *
                 * Go back until we get an event before this time.
                 */
                status = evrTimeGetFifo(&evt_time, trigevent, &idx, -1);
                tsfid = evt_time.nsec & 0x1ffff;
                if (tsfid == 0x1ffff) {
                    if (SYNC_DEBUG(0)) {
                        printf("%s resync sees a bad fiducial, restarting!\n", Name());
                        fflush(stdout);
                    }
                    break;
                }
                if (SYNC_DEBUG(0)) {
                    printf("%s is moving back to timestamp fiducial 0x%x at index %lld.\n",
                           Name(), tsfid, idx);
                    fflush(stdout);
                }
            }
            if (tsfid == 0x1ffff) {
                continue;
            }

            /*
             * Our trigger was close to delayfid.  However, sometimes we don't have the
             * timestamp yet.  Wait until we receive it!
             */
            if (FID_DIFF(delayfid, tsfid) > 2) {
                SYNC_ERROR(0, ("%s is still way off! delayfid = 0x%05x, tsfid = 0x%05x, restarting.\n",
                               Name(), delayfid, tsfid));
            }
#if 0
            while (FID_DIFF(delayfid, tsfid) > 2) {
                unsigned long long idx2;
                do
                    status = evrTimeGetFifo(&evt_time, trigevent, &idx2, MAX_TS_QUEUE);
                while (idx == idx2 || gen != *m_gen);
                idx = idx2;
                tsfid = evt_time.nsec & 0x1ffff;
                if (gen != *m_gen || tsfid == 0x1ffff)
                    break;
            }
#endif

            if (gen != *m_gen || tsfid == 0x1ffff) {
                /* This is just bad.  When in doubt, start over. */
                if (SYNC_DEBUG(0)) {
                    printf("%s resync failed with timestamp fiducial 0x%x, restarting!\n",
                           Name(), (evt_time.nsec & 0x1ffff));
                    fflush(stdout);
                }
                continue;
            }

            /*
             * We should probably check that we are, indeed, close.  What if tsfid skipped
             * *past* our real fiducial?  (Unlikely, I know.)
             */
            if (SYNC_DEBUG(0)) {
                printf("%s resync established with index %lld at timestamp fiducial 0x%x at delayed fiducial 0x%x.\n",
                       Name(), idx, tsfid, delayfid);
                DebugPrint(dobj);
                fflush(stdout);
            }
            SET_SYNC(1);
            do_print = 3; /* Double check the next few times through the loop! */
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
            status = evrTimeGetFifo(&evt_time, trigevent, &idx, incr);
            tsfid = evt_time.nsec & 0x1ffff;

            if (status || tsfid == 0x1ffff) {
                /* Sigh.  Maybe we're just early?  Wait a *little*... */
                struct timespec req = {0, 1000000}; /* 1 ms */
                nanosleep(&req, NULL);
                status = evrTimeGetFifo(&evt_time, trigevent, &idx, 0);
                tsfid = evt_time.nsec & 0x1ffff;
            }
            if (status) {
                SYNC_ERROR(0, ("%s has an invalid timestamp, resynching!\n", Name()));
            }
            if (tsfid == 0x1ffff) {
                SYNC_ERROR(0, ("Invalid fiducial! (delayed fid 0x%05x)\n", delayfid));
            }

            if (attributes & SyncObject::HasTime) {
                /* If we have a timestamp, use it to calculate the expected fiducial, and then
                   search for a timestamp in the queue close to this one. */
                int fiddiff = FidDiff(dobj);
                if (fiddiff < 0) {
                    SYNC_ERROR(0, ("Lost sync in Fiducial!\n"));
                }
                int fid = lastdatafid + fiddiff;
                while (fid > 0x1ffe0)
                    fid -= 0x1ffe0;
                while (!status && FID_DIFF(fid, tsfid) >= 3) {
                    status = evrTimeGetFifo(&evt_time, trigevent, &idx, 1);
                    tsfid = evt_time.nsec & 0x1ffff;
                }
                if (status) {
                    SYNC_ERROR(0, ("%s has an invalid timestamp, resynching!\n", Name()));
                }
                if (tsfid == 0x1ffff) {
                    SYNC_ERROR(0, ("Invalid fiducial! (expected fid 0x%05x)\n", fid));
                }
                /* We're keeping it tight here for the initial sync! */
                if (abs(FID_DIFF(fid, tsfid)) >= 2) {
                    SYNC_ERROR(0, ("Lost sync! (timestamp fid 0x%05x, expected fid 0x%05x)\n", tsfid, fid));
                }
            }

            if (attributes & SyncObject::CanSkip) {
                /*
                 * If we can skip data, we have to give up if we are delayed for any reason!
                 */
                if (FID_DIFF(delayfid, tsfid) >= 3) {
                    SYNC_ERROR(0, ("Lost sync! (timestamp fid 0x%05x, delayed fid 0x%05x, last tsfid = 0x%05x, last delayfid = 0x%05x)\n", tsfid, delayfid, lasttsfid, lastdelayfid));
                }
            }
            lasttsfid = tsfid;
            lastdelayfid = delayfid;

            if (do_print) {
                /*
                 * We think we're synched, but we're still in the checking phase.
                 */
                if (abs(FID_DIFF(tsfid, delayfid)) < 3) {
                    do_print--;
                    if (SYNC_DEBUG(0)) {
                        if (!do_print)
                            printf("%s is fully resynched with index %lld at timestamp fiducial 0x%x (0x%x - %lg = 0x%x).\n",
                                   Name(), idx, evt_time.nsec & 0x1ffff, lastfid,
                                   *m_delay, delayfid);
                        else
                            printf("%s has data at fiducial 0x%x (0x%x - %lg = 0x%x).\n",
                                   Name(), evt_time.nsec & 0x1ffff, lastfid,
                                   *m_delay, delayfid);
                        DebugPrint(dobj);
                        fflush(stdout);
                    }
                    lastdatafid = tsfid;
                    continue;
                } else {
                    SYNC_ERROR(0, ("%s has lost synchronization! (timestamp fid = 0x%x, delay fid = 0x%x, diff = %d)\n",
                                   Name(), evt_time.nsec & 0x1ffff, delayfid, abs((int)tsfid - (int)delayfid)));
                }
            } else {
                if (SYNC_DEBUG_ALWAYS(2)) {
                    printf("%s ts fid=%x, lastfid=%x\n", Name(), evt_time.nsec & 0x1ffff, lastfid);
                    fflush(stdout);
                }
            }
        } else {
            epicsTimeGetEvent(&evt_time, 0);
        }

        QueueData(dobj, evt_time);
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
