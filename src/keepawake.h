/**********************************************************************
 *  RAII guard that asks Windows to keep the system + display awake.
 *  Used around long-running Write / Read / Verify so a multi-minute
 *  SD-card operation isn't cut off by idle-sleep and the user can
 *  still see progress without nudging the mouse. Released on scope
 *  exit (RAII), so normal sleep / blank-out timers resume the moment
 *  the operation finishes.
 **********************************************************************/

#ifndef KEEPAWAKE_H
#define KEEPAWAKE_H

#include <windows.h>

class KeepAwake
{
public:
    KeepAwake()  { SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED); }
    ~KeepAwake() { SetThreadExecutionState(ES_CONTINUOUS); }
    KeepAwake(const KeepAwake &) = delete;
    KeepAwake &operator=(const KeepAwake &) = delete;
};

#endif // KEEPAWAKE_H
