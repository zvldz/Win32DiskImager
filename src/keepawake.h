/**********************************************************************
 *  RAII guard that asks Windows to keep the system awake.
 *  Used around long-running Write / Read / Verify so a multi-minute
 *  SD-card operation isn't cut off by an idle-sleep timer. Display
 *  blank-out is allowed — the user only cares that the I/O finishes.
 **********************************************************************/

#ifndef KEEPAWAKE_H
#define KEEPAWAKE_H

#include <windows.h>

class KeepAwake
{
public:
    KeepAwake()  { SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED); }
    ~KeepAwake() { SetThreadExecutionState(ES_CONTINUOUS); }
    KeepAwake(const KeepAwake &) = delete;
    KeepAwake &operator=(const KeepAwake &) = delete;
};

#endif // KEEPAWAKE_H
