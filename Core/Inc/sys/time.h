#ifndef _SYS_TIME_H_
#define _SYS_TIME_H_

#ifdef __cplusplus
extern "C" {
#endif

struct timeval {
  long tv_sec;
  long tv_usec;
};

struct timezone {
  int tz_minuteswest;
  int tz_dsttime;
};

#ifdef __cplusplus
}
#endif

#endif
