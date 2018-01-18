#ifndef TIMESYNC_H
#define TIMESYNC_H
#include<string>

class DataObject
{
 public:
    DataObject(void *_data) { data = _data; }
 public:
    void *data;
};

struct SyncGlobalParams {
    int l1_retry;
    int l2_retry;
    int l1_future;
    int l2_future;
    int l1_far;
    int l2_far;
    int l1_vfar;
    int l2_vfar;
    int l1_notfar;
    int l2_notfar;
};

class SyncObject
{
 public:
    enum AttributeMask { CanSkip = 1, HasCount = 2, HasTime = 4 };
    SyncObject()                               { m_mode = NULL; }
    virtual ~SyncObject()                      {}
    void SetParams(epicsUInt32 *mode, epicsUInt32 *ev, epicsUInt32 *gen, double *delay, char *syncpv) {
        m_mode = mode;
        m_event = ev;
        m_gen = gen;
        m_delay = delay;
        m_syncpv = syncpv;
        SetGlobalParams(NULL);
    };
    void SetGlobalParams(struct SyncGlobalParams *globs);
    virtual DataObject *Acquire(void)          { return new DataObject(NULL); };
    virtual int CheckError(DataObject *dobj)   { return 0; };
    virtual const char *Name(void)             { return ""; };
    virtual int CountIncr(DataObject *dobj)    { return -1; }
    virtual int FidDiff(DataObject *dobj)      { return -1; }
    virtual int Attributes(void)               { return CanSkip; }
    virtual void QueueData(DataObject *dobj, epicsTimeStamp &evt_time) { delete dobj; };
    virtual void DebugPrint(DataObject *dobj)  {};
    int poll(void);                         // Routine to do synchronization (never returns!)
 private:
    epicsUInt32    *m_mode;                 // Timing mode, 0=LCLS1, 1=LCLS2.
    epicsUInt32    *m_event;                // Event of interest.
    epicsUInt32    *m_gen;                  // Generation of event, if triggered.
    double         *m_delay;                // Expected delay between trigger and reception.
    std::string     m_syncpv;               // Name of the synchronization status PV
};

#endif // TIMESYNC_H
