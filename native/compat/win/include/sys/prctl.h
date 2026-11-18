/* xbased on Windows: prctl, with nothing to set. */
#ifndef NC_WIN_PRCTL_H
#define NC_WIN_PRCTL_H
#define PR_SET_NAME 15
#define PR_SET_TIMERSLACK 29
static inline int prctl(int option, ...)
{
    (void)option;
    return 0;
}
#endif
