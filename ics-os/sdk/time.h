//DEX32-time.h
//Description: Time.h defines the functions related to obtaining the
//              system time and date
//             programmed by Josph Emmanuel Dayo


//the structue used to define date and time
#ifndef DEX_TIME_H

#define DEX_TIME_H

#ifndef DEX_TIME_T_DEFINED
#define DEX_TIME_T_DEFINED
typedef long int time_t;
#endif

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

typedef struct _dex32_datetime {
    int month,year,day,hour,min,sec,ms,adj;
} dex32_datetime;

void get_date_time(dex32_datetime *); //gets the date and time
time_t time(time_t *t);
struct tm *localtime(const time_t *t);
struct tm *gmtime(const time_t *t);
time_t mktime(struct tm *tm);
void delay(unsigned int ms);
#endif
