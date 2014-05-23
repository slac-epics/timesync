#ifndef SYNC_H
#define SYNC_H

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
    virtual int Init(void)                   { m_event = NULL; m_gen = NULL; m_delay = NULL; return 0; };
    virtual DataObject *Acquire(void)        { return new DataObject(NULL); };
    virtual int CheckError(DataObject *dobj) { return 0; };
    virtual const char *Name(void)           { return ""; };
    virtual void QueueData(DataObject *dobj, epicsTimeStamp &evt_time) { delete dobj; };
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
    Synchronizer(SyncObject *_sobj) { sobj = _sobj; };

    // Routine to do synchronization (only return on error!)
    int poll(void);

 private:
    // The object to be synchronized.
    SyncObject *sobj;
};


#endif // SYNC_H
