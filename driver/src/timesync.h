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

class SyncObject
{
 public:
    enum AttributeMask { CanSkip = 1, HasCount = 2, HasTime = 4 };
    virtual int Init(void)                     { m_event = NULL; m_gen = NULL; m_delay = NULL; return 0; };
    virtual DataObject *Acquire(void)          { return new DataObject(NULL); };
    virtual int CheckError(DataObject *dobj)   { return 0; };
    virtual const char *Name(void)             { return ""; };
    virtual int CountIncr(DataObject *dobj)    { return -1; }
    virtual int Fiducial(DataObject *dobj, int lastdatafid) { return -1; }
    virtual int Attributes(void)               { return CanSkip; }
    virtual void QueueData(DataObject *dobj, epicsTimeStamp &evt_time) { delete dobj; };
    virtual void DebugPrint(DataObject *dobj) {};
    virtual ~SyncObject()                    {};
 public:
    epicsUInt32    *m_event;                // Event of interest.
    epicsUInt32    *m_gen;                  // Generation of event, if triggered.
    double         *m_delay;                // Expected delay between trigger and reception.
};

class Synchronizer
{
 public:
    // Constructor
    Synchronizer(SyncObject *_sobj, std::string _syncpv) { sobj = _sobj; syncpv = _syncpv; };

    // Routine to do synchronization (only return on error!)
    int poll(void);

 private:
    // The object to be synchronized.
    SyncObject *sobj;
    std::string syncpv;
};


#endif // TIMESYNC_H
