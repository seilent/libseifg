#ifndef IPOLLER_HEADER
#define IPOLLER_HEADER

#include "ibusiness_event.hh"

class IPoller {
public:
    IPoller(){};
    virtual ~IPoller(){};
    virtual bool AddSocket(intptr_t s, long eventflags) = 0;
    virtual bool ModSocket(intptr_t s, long eventflags) = 0;
    virtual bool RemoveSocket(intptr_t s) = 0;
    // while (IsRunning) {
    virtual void Poll() = 0;
    // }
    void SetBusiness(IBusinessEvent* op) { op_ = op; }

protected:
    IBusinessEvent* op_ = nullptr;
};

#endif // ipoller.hh