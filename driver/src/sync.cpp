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
#include "sync.h"
#include "evrTime.h"

using namespace		std;

int sync_debug = 2;
int sync_cnt   = 200;
#define SYNC_DEBUG(n)        (sync_debug > (n) && sync_cnt > 0 && --sync_cnt)
#define SYNC_DEBUG_ALWAYS(n) (sync_debug > (n))

int Synchronizer::poll(void)
{
    unsigned int gen;
    int trigevent;
    int eventvalid;
    DataObject *dobj = NULL;
    int delayfid, tsfid;
    int in_sync;
    int do_print = 0;
    unsigned long long idx = -1;
    epicsTimeStamp evt_time;
    int status = 0;

    sobj->Init();

    trigevent = sobj->m_gen ? *sobj->m_event : -1;
    gen       = sobj->m_gen ? *sobj->m_gen : 0;
    in_sync   = sobj->m_gen ? 0 : 1;
    eventvalid = trigevent > 0 && trigevent < 256;

    while(TRUE) {
        if (dobj)
            delete dobj;
        dobj = sobj->Acquire();
        delayfid = lastfid - (int)(*sobj->m_delay + 0.5);
        if (sobj->CheckError(dobj)) {
            gen = -1;
            continue;
        }

        if (sobj->m_gen && gen != *sobj->m_gen) {
            /* The trigger event changed, so force a resync! */
            trigevent = *sobj->m_event;
            gen = *sobj->m_gen;
            in_sync = 0;

            if (eventvalid) {
                eventvalid = trigevent > 0 && trigevent < 256;
                if (eventvalid)
                    printf("%s is setting event trigger to %d.\n", sobj->Name(), trigevent);
                else
                    printf("%s has invalid event trigger %d!\n", sobj->Name(), trigevent);
                fflush(stdout);
            } else {
                eventvalid = trigevent > 0 && trigevent < 256;
                if (eventvalid) {
                    printf("%s is setting event trigger to %d.\n", sobj->Name(), trigevent);
                    fflush(stdout);
                }
            }
            continue;
        }

        if (!in_sync) {     /* Try to resynchronize! */
            if (!eventvalid)
                continue;

            if (SYNC_DEBUG(0)) {
                printf("%s resynchronizing at fiducial 0x%x (delay=%lg).\n",
                       sobj->Name(), lastfid, *sobj->m_delay);
            }

            if (gen != *sobj->m_gen) {
                printf("Generation change, restarting!\n");
                continue;   /* Ow... a reconfigure while reconfiguring.  Just start over. */
            }

            /* Get the current time! */
            status = evrTimeGetFifo(&evt_time, trigevent, &idx, MAX_TS_QUEUE);
            tsfid = evt_time.nsec & 0x1ffff;
            if (tsfid == 0x1ffff) { /* Sigh.  Restart if the fiducial is bad. */
                if (SYNC_DEBUG(0)) {
                    printf("%s has bad fiducial at time %08x:%08x.\n", sobj->Name(),
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
                        printf("%s resync sees a bad fiducial, restarting!\n", sobj->Name());
                        fflush(stdout);
                    }
                    break;
                }
                if (SYNC_DEBUG(0)) {
                    printf("%s is moving back to timestamp fiducial 0x%x at index %lld.\n",
                           sobj->Name(), tsfid, idx);
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
            while (FID_DIFF(delayfid, tsfid) > 3) {
                unsigned long long idx2;
                do
                    status = evrTimeGetFifo(&evt_time, trigevent, &idx2, MAX_TS_QUEUE);
                while (idx == idx2 || gen != *sobj->m_gen);
                idx = idx2;
                tsfid = evt_time.nsec & 0x1ffff;
                if (gen != *sobj->m_gen || tsfid == 0x1ffff)
                    break;
            }
            if (gen != *sobj->m_gen || tsfid == 0x1ffff) {
                /* This is just bad.  When in doubt, start over. */
                if (SYNC_DEBUG(0)) {
                    printf("%s resync failed with timestamp fiducial 0x%x, restarting!\n",
                           sobj->Name(), (evt_time.nsec & 0x1ffff));
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
                       sobj->Name(), idx, tsfid, delayfid);
                fflush(stdout);
            }
            in_sync = 1;
            do_print = 2; /* Double check the next time through the loop! */
            continue;
        }

        assert(in_sync == 1);

        if (sobj->m_gen) {
            status = evrTimeGetFifo(&evt_time, trigevent, &idx, 1);
            tsfid = evt_time.nsec & 0x1ffff;

            if (status || FID_DIFF(delayfid, tsfid) > 2) {
                if (SYNC_DEBUG(0)) {
                    if (status) 
                        printf("%s has an invalid timestamp, resynching!\n", sobj->Name());
                    else
                        printf("Lost sync! (timestamp fid 0x%05x, delayed fid 0x%05x)\n",
                               tsfid, delayfid);
                    fflush(stdout);
                }
                in_sync = 0;
                continue;
            }

            if (do_print) {
                if (abs((int)tsfid - (int)delayfid) <= 2) {
                    do_print--;
                    if (SYNC_DEBUG(0)) {
                        if (!do_print)
                            printf("%s is fully resynched with index %lld at timestamp fiducial 0x%x (0x%x - %lg = 0x%x).\n",
                                   sobj->Name(), idx, evt_time.nsec & 0x1ffff, lastfid,
                                   *sobj->m_delay, delayfid);
                        else
                            printf("%s has image at fiducial 0x%x (0x%x - %lg = 0x%x).\n",
                                   sobj->Name(), evt_time.nsec & 0x1ffff, lastfid,
                                   *sobj->m_delay, delayfid);
                        fflush(stdout);
                    }
                } else {
                    if (SYNC_DEBUG(0)) {
                        printf("%s has lost synchronization! (timestamp fid = 0x%x, delay fid = 0x%x, diff = %d)\n",
                               sobj->Name(), evt_time.nsec & 0x1ffff, delayfid, abs((int)tsfid - (int)delayfid));
                        fflush(stdout);
                    }                    
                    in_sync = 0;
                }
                continue;
            } else {
                if (SYNC_DEBUG_ALWAYS(2)) {
                    printf("%s ts fid=%x, lastfid=%x\n", sobj->Name(), evt_time.nsec & 0x1ffff, lastfid);
                    fflush(stdout);
                }
            }
        } else {
            epicsTimeGetEvent(&evt_time, *sobj->m_event);
        }

        sobj->QueueData(dobj, evt_time);
    }
    return 0;
}


// Debug stuff. 
static const iocshArg	   syncdebugArg0	= { "level",	iocshArgInt };
static const iocshArg	  *syncdebugArgs[1]	= { &syncdebugArg0 };
static const iocshFuncDef  syncdebugFuncDef	= { "syncdebug", 1, syncdebugArgs };
static int  syncdebugCallFunc(const iocshArgBuf * args)
{
    sync_debug = args[0].ival;
    sync_cnt   = 1000;
    return 0;
}

void syncRegister(void)
{
    iocshRegister(&syncdebugFuncDef, reinterpret_cast<iocshCallFunc>(syncdebugCallFunc));
}

extern "C"
{
    epicsExportRegistrar(syncRegister);
}
