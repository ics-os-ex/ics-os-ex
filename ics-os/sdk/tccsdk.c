/*
  Name: tcc built-in DEX-OS SDK
  Copyright: 
  Author: Joseph Emmanuel DL Dayo
  Date: 12/04/04 05:38
  Description: A built-in C library for tcc aka "Tiny C Compiler".
  Compile in tcc using the -c option to create an object file and
  then link it with the main program.
  
  

    Copyright (C) 2004  Joseph Emmanuel Dayo
    
    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.
    
    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.
    
    You should have received a copy of the GNU Library General Public
    License along with this library; if not, write to the Free
    Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/
#include "dexsdk.h"
#include "time.h"

/* Forward decl (defined in posix.c). wait() below delegates to it. We do
 * NOT include <sys/wait.h> here because it pulls in <sys/types.h>, whose
 * clock_t typedef conflicts with dexsdk.h's own clock_t in this TU. */
int waitpid(int pid, int *status, int options);

//global ANSI variables
int errno;

FILE *stdout = (FILE*)1, *stdin =(FILE*)2, *stderr=(FILE*)1;

/*executes a dex32 systemcall (int 0x30) , implementation almost similar to linux*/
unsigned long dexsdk_systemcall(int function_number, long p1, long p2,
                  long p3, long p4, long p5){
   unsigned long return_value;
   __asm__ volatile ("int $0x30" \
         : "=a" (return_value) \
         : "0" ((long)(function_number)),"b" ((long)(p1)),"c" ((long)(p2)), \
         "d" ((long)(p3)),"S" ((long)(p4)),"D" ((long)(p5)) \
         : "memory" ); \
   return return_value;
};

int dex_exit(int val){
   dexsdk_systemcall(FXN_EXIT,val,0,0,0,0);
};

void exit (int status){
    printf ("SDK_EXIT enter status=%d\n", status);
    dex_exit(status);
    printf ("SDK_EXIT returned status=%d\n", status);
};

void getparameters(char *buf){
   dexsdk_systemcall(0x50,(long)buf,0,0,0,0);
};

void charputc(char c){
   dexsdk_systemcall(6,(long)(unsigned char)c,0,0,0,0);
};  

/*this strtok is still not thread safe, so be careful!*/
char *strtok(char *s, const char *delim){
   const char *spanp;
   int c, sc;
   char *tok;
   static char *last;


   if (s == NULL && (s = last) == NULL)
      return (NULL);

   /*
   * Skip (span) leading delimiters (s += strspn(s, delim), sort of).
   */
cont:
   c = *s++;
   for (spanp = delim; (sc = *spanp++) != 0;) {
      if (c == sc)
         goto cont;  
   }

   if (c == 0) {			/* no non-delimiter characters */
      last = NULL;
      return (NULL);
   }
   tok = s - 1;

   /*
   * Scan token (scan for delimiters: s += strcspn(s, delim), sort of).
   * Note that delim must have one NUL; we stop if we see that, too.
   */
   for (;;) {
      c = *s++;
      spanp = delim;
      do {
         if ((sc = *spanp++) == c) {
            if (c == 0)
	            s = NULL;
	         else
               s[-1] = 0;
	      last = s;
	      return (tok);
         }
      }while (sc != 0);
   }
  /* NOTREACHED */
};

size_t strlen(const char *str){
   const char *s;

   if (str == 0)
      return 0;
   for (s = str; *s; ++s)
      ;
   return s-str;
};


#define ZEROPAD	1		/* pad with zero */
#define SIGN	2		/* unsigned/signed long */
#define PLUS	4		/* show plus */
#define SPACE	8		/* space if plus */
#define LEFT	16		/* left justified */
#define SPECIAL	32		/* 0x */
#define LARGE	64		/* use 'ABCDEF' instead of 'abcdef' */

static char * number(char * buf, char * end, long long num, int base, int size, int precision, int type){
   char c,sign,tmp[66];
   const char *digits;
   const char small_digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
   const char large_digits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
   int i;

   digits = (type & LARGE) ? large_digits : small_digits;
	if (type & LEFT)
		type &= ~ZEROPAD;
	if (base < 2 || base > 36)
		return 0;
	c = (type & ZEROPAD) ? '0' : ' ';
	sign = 0;
	if (type & SIGN) {
		if (num < 0) {
			sign = '-';
			num = -num;
			size--;
		} else if (type & PLUS) {
			sign = '+';
			size--;
		} else if (type & SPACE) {
			sign = ' ';
			size--;
		}
	}
	if (type & SPECIAL) {
		if (base == 16)
			size -= 2;
		else if (base == 8)
			size--;
	}
	i = 0;
	if (num == 0)
		tmp[i++]='0';
	else while (num != 0)
		tmp[i++] = digits[num/base];
	if (i > precision)
		precision = i;
	size -= precision;
	if (!(type&(ZEROPAD+LEFT))) {
		while(size-->0) {
			if (buf <= end)
				*buf = ' ';
			++buf;
		}
	}
	if (sign) {
		if (buf <= end)
			*buf = sign;
		++buf;
	}
	if (type & SPECIAL) {
		if (base==8) {
			if (buf <= end)
				*buf = '0';
			++buf;
		} else if (base==16) {
			if (buf <= end)
				*buf = '0';
			++buf;
			if (buf <= end)
				*buf = digits[33];
			++buf;
		}
	}
	if (!(type & LEFT)) {
		while (size-- > 0) {
			if (buf <= end)
				*buf = c;
			++buf;
		}
	}
	while (i < precision--) {
		if (buf <= end)
			*buf = '0';
		++buf;
	}
	while (i-- > 0) {
		if (buf <= end)
			*buf = tmp[i];
		++buf;
	}
	while (size-- > 0) {
		if (buf <= end)
			*buf = ' ';
		++buf;
	}
	return buf;
}

int vprintf_help(unsigned c, void **ptr,FILE *f){
   charputc(c);
   return 0 ;
}

static int sdk_vprint(const char *fmt, va_list args);

int vprintf(const char *fmt, va_list args){
	return sdk_vprint(fmt, args);
}


/* flags used in processing format string */
#define		PR_LJ	0x01	/* left justify */
#define		PR_CA	0x02	/* use A-F instead of a-f for hex */
#define		PR_SG	0x04	/* signed numeric conversion (%d vs. %u) */
#define		PR_32	0x08	/* long (32-bit) numeric conversion */
#define		PR_16	0x10	/* short (16-bit) numeric conversion */
#define		PR_WS	0x20	/* PR_SG set and num was < 0 */
#define		PR_LZ	0x40	/* pad left with '0' instead of ' ' */
#define		PR_FP	0x80	/* pointers are far */
#define		PR_PTR	0x100	/* pointer conversion (%p) */

/* largest number handled is 2^64-1, lowest radix handled is 8.
2^64-1 in base 8 has 22 digits (add trailing NUL and slop) */
#define		PR_BUFLEN	32

int do_printf(const char *fmt, va_list args, fnptr_t fn,FILE *f, void *ptr){
   unsigned state, flags, radix, actual_wd, count, given_wd;
	unsigned char *where, buf[PR_BUFLEN];
	long num;

	state = flags = count = given_wd = 0;
/* begin scanning format specifier list */
	for(; *fmt; fmt++){
		switch(state){
/* STATE 0: AWAITING % */
		case 0:
			if(*fmt != '%')	/* not %... */
			{
				fn(*fmt, &ptr,f);	/* ...just echo it */
				count++;
				break;
			}
/* found %, get next char and advance state to check if next char is a flag */
			state++;
			fmt++;
			/* FALL THROUGH */
/* STATE 1: AWAITING FLAGS (%-0) */
		case 1:
     		if(*fmt == '%')	/* %% */
			{
				fn(*fmt, &ptr,f);
				count++;
				state = flags = given_wd = 0;
				break;
			}
			if(*fmt == '-')
			{
				if(flags & PR_LJ)/* %-- is illegal */
					state = flags = given_wd = 0;
				else
					flags |= PR_LJ;
				break;
			}
/* not a flag char: advance state to check if it's field width */
			state++;
/* check now for '%0...' */
			if(*fmt == '0')
			{
				flags |= PR_LZ;
				fmt++;
			}
			/* FALL THROUGH */
/* STATE 2: AWAITING (NUMERIC) FIELD WIDTH */
		case 2:
			if(*fmt >= '0' && *fmt <= '9')
			{
				given_wd = 10 * given_wd +
					(*fmt - '0');
				break;
			}
/* not field width: advance state to check if it's a modifier */
			state++;
			/* FALL THROUGH */
/* STATE 3: AWAITING MODIFIER CHARS (FNlh) */
		case 3:
			if(*fmt == 'F')
			{
				flags |= PR_FP;
				break;
			}
			if(*fmt == 'N')
				break;
			if(*fmt == 'l')
			{
				flags |= PR_32;
				break;
			}
			if(*fmt == 'h')
			{
				flags |= PR_16;
				break;
			}
/* not modifier: advance state to check if it's a conversion char */
			state++;
			/* FALL THROUGH */
/* STATE 4: AWAITING CONVERSION CHARS (Xxpndiuocs) */
		case 4:
			where = buf + PR_BUFLEN - 1;
			*where = '\0';
			switch(*fmt)
			{
			case 'X':
				flags |= PR_CA;
				/* FALL THROUGH */
/* xxx - far pointers (%Fp, %Fn) not yet supported */
			case 'x':
			case 'n':
				radix = 16;
				goto DO_NUM;
			case 'p':
				flags |= PR_PTR;
				radix = 16;
				goto DO_NUM;
			case 'd':
			case 'i':
				flags |= PR_SG;
				/* FALL THROUGH */
			case 'u':
				radix = 10;
				goto DO_NUM;
			case 'o':
				radix = 8;
/* load the value to be printed. l=long=32 bits: */
DO_NUM:				if(flags & PR_PTR)
					num = (unsigned long)va_arg(args, void *);
				else if(flags & PR_32)
					num = va_arg(args, unsigned long);
/* h=short=16 bits (signed or unsigned) */
				else if(flags & PR_16)
				{
					if(flags & PR_SG)
						num = va_arg(args, int );
					else
						num = va_arg(args, unsigned );
				}
/* no h nor l: sizeof(int) bits (signed or unsigned) */
				else
				{
					if(flags & PR_SG)
						num = va_arg(args, int);
					else
						num = va_arg(args, unsigned int);
				}
/* take care of sign */
				if(flags & PR_SG)
				{
					if(num < 0)
					{
						flags |= PR_WS;
						num = -num;
					}
				}
/* convert binary to octal/decimal/hex ASCII
OK, I found my mistake. The math here is _always_ unsigned */
				do
				{
					unsigned long temp;

					temp = (unsigned long)num % radix;
					where--;
					if(temp < 10)
						*where = temp + '0';
					else if(flags & PR_CA)
						*where = temp - 10 + 'A';
					else
						*where = temp - 10 + 'a';
					num = (unsigned long)num / radix;
				}
				while(num != 0);
				goto EMIT;
			case 'c':
/* disallow pad-left-with-zeroes for %c */
				flags &= ~PR_LZ;
				where--;
				*where = (unsigned char)va_arg(args,
					unsigned );
				actual_wd = 1;
				goto EMIT2;
			case 's':
/* disallow pad-left-with-zeroes for %s */
				flags &= ~PR_LZ;
				where = va_arg(args, unsigned char *);
EMIT:
				actual_wd = strlen((char*)where);
				if(flags & PR_WS)
					actual_wd++;
/* if we pad left with ZEROES, do the sign now */
				if((flags & (PR_WS | PR_LZ)) ==
					(PR_WS | PR_LZ))
				{
					fn('-', &ptr,f);
					count++;
				}
/* pad on left with spaces or zeroes (for right justify) */
EMIT2:				if((flags & PR_LJ) == 0)
				{
					while(given_wd > actual_wd)
					{
						fn(flags & PR_LZ ? '0' :
							' ', &ptr,f);
						count++;
						given_wd--;
					}
				}
/* if we pad left with SPACES, do the sign now */
				if((flags & (PR_WS | PR_LZ)) == PR_WS)
				{
					fn('-', &ptr,f);
					count++;
				}
/* emit string/char/converted number */
				while(*where != '\0')
				{
					fn(*where++,&ptr,f);
					count++;
				}
/* pad on right with spaces (for left justify) */
				if(given_wd < actual_wd)
					given_wd = 0;
				else given_wd -= actual_wd;
				for(; given_wd; given_wd--)
				{
					fn(' ', &ptr,f);
					count++;
				}
				break;
			default:
				break;
			}
		default:
			state = flags = given_wd = 0;
			break;
		}
	}
	return count;
}

/* toupper/tolower live in posix.c */

int vsprintf(char *buffer, const char *fmt, va_list args);
int vsnprintf(char *buffer, size_t n, const char *fmt, va_list args);

/* Format into a local buffer and emit the WHOLE line in one non-require-ints
   syscall (0xB5 = console_puts). The int 0x30 entry leaves IF=0 for the entire
   kernel call, so a timer IRQ / scheduler context switch cannot fire between
   characters; the line cannot be torn by kernel teardown output. Lines longer
   than the stack buffer used to overflow and GPF on ret (make echoing `ar rcs`
   of libib.a); those allocate or stream instead. */
int printf(const char *fmt, ...)
{
	va_list args;
	int ret_val;

	va_start(args, fmt);
	ret_val = sdk_vprint(fmt, args);
	va_end(args);
	return ret_val;
}

int do_sprintf(const char *fmt, va_list args, sfnptr_t fn, void **ptr){
	unsigned state, flags, radix, actual_wd, count, given_wd;
	unsigned char *where, buf[PR_BUFLEN];
	long num;

	state = flags = count = given_wd = 0;
/* begin scanning format specifier list */
	for(; *fmt; fmt++)
	{
		switch(state)
		{
/* STATE 0: AWAITING % */
		case 0:
			if(*fmt != '%')	/* not %... */
			{
				fn(*fmt, ptr);	/* ...just echo it */
				count++;
				break;
			}
/* found %, get next char and advance state to check if next char is a flag */
			state++;
			fmt++;
			/* FALL THROUGH */
/* STATE 1: AWAITING FLAGS (%-0) */
		case 1:
         if (*fmt == '?')
               {
                fmt++;
                if (*fmt=='r') textcolor(RED);
                if (*fmt=='b') textcolor(BLUE);
                if (*fmt=='g') textcolor(GREEN);
                if (*fmt=='w') textcolor(WHITE);
                if (*fmt=='0') textcolor(BLACK);
                count++;
               	state = flags = given_wd = 0;
      				break;
               };
			if(*fmt == '%')	/* %% */
			{
				fn(*fmt, ptr);
				count++;
				state = flags = given_wd = 0;
				break;
			}
			if(*fmt == '-')
			{
				if(flags & PR_LJ)/* %-- is illegal */
					state = flags = given_wd = 0;
				else
					flags |= PR_LJ;
				break;
			}
/* not a flag char: advance state to check if it's field width */
			state++;
/* check now for '%0...' */
			if(*fmt == '0')
			{
				flags |= PR_LZ;
				fmt++;
			}
			/* FALL THROUGH */
/* STATE 2: AWAITING (NUMERIC) FIELD WIDTH */
		case 2:
			if(*fmt >= '0' && *fmt <= '9')
			{
				given_wd = 10 * given_wd +
					(*fmt - '0');
				break;
			}
/* not field width: advance state to check if it's a modifier */
			state++;
			/* FALL THROUGH */
/* STATE 3: AWAITING MODIFIER CHARS (FNlh) */
		case 3:
			if(*fmt == 'F')
			{
				flags |= PR_FP;
				break;
			}
			if(*fmt == 'N')
				break;
			if(*fmt == 'l')
			{
				flags |= PR_32;
				break;
			}
			if(*fmt == 'h')
			{
				flags |= PR_16;
				break;
			}
/* not modifier: advance state to check if it's a conversion char */
			state++;
			/* FALL THROUGH */
/* STATE 4: AWAITING CONVERSION CHARS (Xxpndiuocs) */
		case 4:
			where = buf + PR_BUFLEN - 1;
			*where = '\0';
			switch(*fmt)
			{
			case 'X':
				flags |= PR_CA;
				/* FALL THROUGH */
/* xxx - far pointers (%Fp, %Fn) not yet supported */
			case 'x':
			case 'n':
				radix = 16;
				goto DO_NUM;
			case 'p':
				flags |= PR_PTR;
				radix = 16;
				goto DO_NUM;
			case 'd':
			case 'i':
				flags |= PR_SG;
				/* FALL THROUGH */
			case 'u':
				radix = 10;
				goto DO_NUM;
			case 'o':
				radix = 8;
/* load the value to be printed. l=long=32 bits: */
DO_NUM:				if(flags & PR_PTR)
					num = (unsigned long)va_arg(args, void *);
				else if(flags & PR_32)
					num = va_arg(args, unsigned long);
/* h=short=16 bits (signed or unsigned) */
				else if(flags & PR_16)
				{
					if(flags & PR_SG)
						num = va_arg(args, int );
					else
						num = va_arg(args, unsigned );
				}
/* no h nor l: sizeof(int) bits (signed or unsigned) */
				else
				{
					if(flags & PR_SG)
						num = va_arg(args, int);
					else
						num = va_arg(args, unsigned int);
				}
/* take care of sign */
				if(flags & PR_SG)
				{
					if(num < 0)
					{
						flags |= PR_WS;
						num = -num;
					}
				}
/* convert binary to octal/decimal/hex ASCII
OK, I found my mistake. The math here is _always_ unsigned */
				do
				{
					unsigned long temp;

					temp = (unsigned long)num % radix;
					where--;
					if(temp < 10)
						*where = temp + '0';
					else if(flags & PR_CA)
						*where = temp - 10 + 'A';
					else
						*where = temp - 10 + 'a';
					num = (unsigned long)num / radix;
				}
				while(num != 0);
				goto EMIT;
			case 'c':
/* disallow pad-left-with-zeroes for %c */
				flags &= ~PR_LZ;
				where--;
				*where = (unsigned char)va_arg(args,
					unsigned );
				actual_wd = 1;
				goto EMIT2;
			case 's':
/* disallow pad-left-with-zeroes for %s */
				flags &= ~PR_LZ;
				where = va_arg(args, unsigned char *);
EMIT:
				actual_wd = strlen(where);
				if(flags & PR_WS)
					actual_wd++;
/* if we pad left with ZEROES, do the sign now */
				if((flags & (PR_WS | PR_LZ)) ==
					(PR_WS | PR_LZ))
				{
					fn('-', ptr);
					count++;
				}
/* pad on left with spaces or zeroes (for right justify) */
EMIT2:				if((flags & PR_LJ) == 0)
				{
					while(given_wd > actual_wd)
					{
						fn(flags & PR_LZ ? '0' :
							' ', ptr);
						count++;
						given_wd--;
					}
				}
/* if we pad left with SPACES, do the sign now */
				if((flags & (PR_WS | PR_LZ)) == PR_WS)
				{
					fn('-', ptr);
					count++;
				}
/* emit string/char/converted number */
				while(*where != '\0')
				{
					fn(*where++, ptr);
					count++;
				}
/* pad on right with spaces (for left justify) */
				if(given_wd < actual_wd)
					given_wd = 0;
				else given_wd -= actual_wd;
				for(; given_wd; given_wd--)
				{
					fn(' ', ptr);
					count++;
				}
				break;
			default:
				break;
			}
		default:
			state = flags = given_wd = 0;
			break;
		}
	}
	return count;
}



/*****************************************************************************
SPRINTF
*****************************************************************************/
/*
 * FIXME
 */
int vsprintf_help(unsigned c, void **ptr ){
        char *dst;

        dst = *ptr;
        *dst++ = c;
        *ptr = dst;
        return 0 ;
}

typedef struct sdk_sn {
        char *p;
        unsigned long left;
} sdk_sn;

static int vsnprintf_help(unsigned c, void **ptr)
{
        sdk_sn *s = *(sdk_sn **)ptr;
        if (s->p && s->left > 1) {
                *s->p++ = (char)c;
                s->left--;
        }
        return 0;
}

int vsnprintf(char *buffer, size_t n, const char *fmt, va_list args)
{
        sdk_sn ctx, *pc = &ctx;
        int ret_val;
        ctx.p = buffer;
        ctx.left = (buffer && n) ? (unsigned long)n : 0;
        ret_val = do_sprintf(fmt, args, vsnprintf_help, (void *)&pc);
        if (buffer && n) {
                size_t term = (size_t)ret_val;
                if (term >= n)
                        term = n - 1;
                buffer[term] = 0;
        }
        return ret_val;
}

static int sdk_vprint(const char *fmt, va_list args)
{
        char buf[1024];
        va_list args2;
        int n;
        char *big;

        va_copy(args2, args);
        n = vsnprintf(buf, sizeof(buf), fmt, args);
        if (n < 0) {
                va_end(args2);
                return n;
        }
        if ((size_t)n < sizeof(buf)) {
                va_end(args2);
                dexsdk_systemcall(0xB5, (long)buf, (long)n, 0, 0, 0);
                return n;
        }
        big = (char *)malloc((size_t)n + 1);
        if (!big) {
                n = do_printf(fmt, args2, vprintf_help, 0, NULL);
                va_end(args2);
                return n;
        }
        vsnprintf(big, (size_t)n + 1, fmt, args2);
        va_end(args2);
        dexsdk_systemcall(0xB5, (long)big, (long)n, 0, 0, 0);
        free(big);
        return n;
}

int vsprintf(char *buffer, const char *fmt, va_list args){
        int ret_val;
        /* do_sprintf advances the pointer it is given (vsprintf_help writes
         * through it), so pass a SEPARATE cursor. Passing &buffer directly
         * leaves buffer pointing past the end and the NUL below lands at
         * buffer + 2*ret_val, leaving the real terminator uninitialized. */
        char *cursor = buffer;
        ret_val=do_sprintf(fmt, args, vsprintf_help, (void*)& cursor);
        buffer[ret_val] = '\0';
        return ret_val;
}
/*****************************************************************************
*****************************************************************************/
/*
 * FIXME
 */
int sprintf(char *buffer, const char *fmt, ...){
        va_list args;
        int ret_val;

        va_start(args, fmt);
        ret_val = vsprintf(buffer, fmt, args);
        buffer[ret_val] = '\0';
        va_end(args);
        return ret_val;
}


/********************CRT control functions******************************/
/* Clear the screen */
void  clrscr(){
    dexsdk_systemcall(8,0,0,0,0,0);
};

int wherex(void){
  return getx()+1;
};

int wherey(void){
 return gety()+1;
};

int getx(){
   return dexsdk_systemcall(0x35,0,0,0,0,0);
};

int gety(){
   return dexsdk_systemcall(0x36,0,0,0,0,0);
};

void setx(int x){
   dexsdk_systemcall(0x37,x,0,0,0,0);
};

void sety(int y){
   dexsdk_systemcall(0x38,y,0,0,0,0); 
};

void textcolor(char val){
   dexsdk_systemcall(13,val,0,0,0,0);
};

void textbackground(char val){
   dexsdk_systemcall(14,val,0,0,0,0);
};
  
void outc(char x){
    dexsdk_systemcall(0x51,x,0,0,0,0);
};

void update_cursor(int x, int y){
    dexsdk_systemcall(7,x,y,0,0,0);
};

void gotoxy(int x,int y){
   setx(x-1);
   sety(y-1);
   update_cursor(y-1,x-1);
;};

/************Heap management functions***/
void *sbrk(int amt){
   return (void*)dexsdk_systemcall(FXN_SBRK,amt,0,0,0,0);
};

char *buckets[32] = {0};
size_t bucket2size[32] = {0};
/* New blocks are carved from a shared page-aligned slab so many small blocks
   share pages instead of each taking its own sbrk page.  The previous
   one-page-per-block scheme ballooned the user heap past the 1 GiB VA cap on
   large GAS files (e.g. insn-attrtab, ~250k symbol/frag blocks). */
static char *slab_cur = 0, *slab_end = 0;

static int size2bucket(size_t size){
    int b;

    if (size < 16)
       size = 16;
    for (b=0; b<32; b++){
       if (bucket2size[b] >= size)
          return b;
    }
    return -1;
}

static void init_buckets(void){
    unsigned b;
    bucket2size[0] = 16;
    for (b=1; b<32; b++)
       bucket2size[b] = bucket2size[b-1] * 2;
}

/* 32-byte header before the user payload.  The free-list pointer lives in
   the header, not in the payload, so freeing a chunk can never leave a stale
   allocator pointer inside memory that has already been handed to the caller.
   The header size is a multiple of 16 so sbrk-aligned chunks keep the payload
   at a 16-byte boundary. */
#define MALLOC_HDR 32
#define MALLOC_MAGIC 0xA11CC8EDu
struct malloc_hdr {
   int bucket;
   unsigned int magic;
   int pad[2];
   size_t size;
   void *next;
};

void free(void *ptr){
    struct malloc_hdr *h;
    int b;
    if (ptr == 0)
       return;
    h = (struct malloc_hdr *)((char *)ptr - MALLOC_HDR);
    b = h->bucket;
    if (b < 0 || b >= 32 || h->magic != MALLOC_MAGIC)
       return;
    h->magic = 0;
    h->next = buckets[b];
    buckets[b] = (char *)h;
}

void *realloc(void *ptr, size_t size) {
    char *newptr;
    struct malloc_hdr *h;
    size_t oldsize;
    if (ptr == 0)
       return malloc(size);
    h = (struct malloc_hdr *)((char *)ptr - MALLOC_HDR);
    if (h->bucket < 0 || h->bucket >= 32 || h->magic != MALLOC_MAGIC)
       return 0;
    oldsize = h->size;
    if (size == 0){
       free(ptr);
       return 0;
    }
    if (size <= oldsize)
       return ptr;
    newptr = (char *)malloc(size);
    if (newptr == 0)
       return 0;
    memcpy(newptr, ptr, oldsize);
    free(ptr);
    return newptr;
}

void *malloc(size_t size){
    char *rv;
    struct malloc_hdr *h;
    int b;

    if (bucket2size[0] == 0)
       init_buckets();

    b = size2bucket(size);
    if (b < 0)
       return 0;
    if (buckets[b]){
       h = (struct malloc_hdr *)buckets[b];
       buckets[b] = (char *)h->next;
       if (h->bucket != b) {
          buckets[b] = (char *)h;
          return 0;
       }
       h->magic = MALLOC_MAGIC;
       h->next = 0;
       return (char *)h + MALLOC_HDR;
    }

   size = bucket2size[b] + MALLOC_HDR;
    if (size > 0x7fffffff)
        return 0;
    if (size <= 0x100000) {
        /* Carve from a shared slab (>= 64 KiB) so small blocks share pages. */
        if ((unsigned long)(slab_end - slab_cur) < (unsigned long)size) {
            unsigned long slab = (size > 0x10000) ? size : 0x10000;
            slab = (slab + 0xfff) & ~(unsigned long)0xfff;
            slab_cur = (char *)sbrk((int)slab);
            if (slab_cur == 0 || slab_cur == (char *)-1)
                return 0;
            slab_end = slab_cur + slab;
        }
        rv = slab_cur;
        slab_cur += size;
    } else {
        /* Large block: its own page-aligned sbrk region. */
        size = (size + 0xfff) & ~(unsigned)0xfff;
        rv = (char *)sbrk((int)size);
        if (rv == 0 || rv == (char *)-1)
            return 0;
    }

    h = (struct malloc_hdr *)rv;
    h->bucket = b;
    h->magic = MALLOC_MAGIC;
    h->pad[0] = h->pad[1] = 0;
    h->size = bucket2size[b];
    h->next = 0;
    rv += MALLOC_HDR;
    return rv;
}


/************String management***********/
void *memmove (void *dst, const void *src, size_t count){
   void *ret = dst;

   if (dst <= src || (char*)dst >= ((char*)src + count)) {
      while (count--) {
         *(char*)dst = *(char*)src;
         dst = (char*)dst + 1;
         src = (char*)src + 1;
      }
   }else {
      /*
      * Overlapping Buffers
      * copy from higher addresses to lower addresses
      */
      dst = (char*)dst + count - 1;
      src = (char*)src + count - 1;

      while (count--) {
         *(char*)dst = *(char*)src;
         dst = (char*)dst - 1;
         src = (char*)src - 1;
      }
   }
   return(ret);
};

void *memset (void *dst,int val, size_t count){
   unsigned char *p = dst;
   /* Word-fill large blocks — TinyCC ONE_SOURCE does huge zeroing. */
   if (count >= 16 && ((unsigned long)p & 7) == 0 && (val & 0xff) == 0) {
      unsigned long *q = (unsigned long *)p;
      while (count >= 8) {
         *q++ = 0;
         count -= 8;
      }
      p = (unsigned char *)q;
   }
   while (count--)
      *p++ = (unsigned char)val;
   return dst;
};

void *memchr(const void *s, int c, size_t n){
   if (n){
      const char *p = s;
      char cc = c;
      do {
         if (*p == cc)
	         return unconst(p, void *);
         p++;
      } while (--n != 0);
   }
   return 0;
}

void *memcpy (void * dst, const void * src, size_t count){
   unsigned char *d = dst;
   const unsigned char *s = src;
   if (count >= 16 &&
       (((unsigned long)d | (unsigned long)s) & 7) == 0) {
      unsigned long *ld = (unsigned long *)d;
      const unsigned long *ls = (const unsigned long *)s;
      while (count >= 8) {
         *ld++ = *ls++;
         count -= 8;
      }
      d = (unsigned char *)ld;
      s = (const unsigned char *)ls;
   }
   while (count--)
      *d++ = *s++;
   return dst;
};

int memcmp(const void *s1, const void *s2, size_t n){
   if (n != 0){
      const unsigned char *p1 = s1, *p2 = s2;

      do {
         if (*p1++ != *p2++)
	         return (*--p1 - *--p2);
      } while (--n != 0);
   }
   return 0;
}

char *strcpy(char *to, const char *from){
   char *save = to;

   for (; (*to = *from); ++from, ++to)
      ;
   return save;
};

char *strchr(const char *s, int c){
   char cc = c;
   while (*s){
      if (*s == cc)
         return unconst(s, char *);
      s++;
   }
   if (cc == 0)
      return unconst(s, char *);
   return 0;
}

char *strrchr(const char *s, int c){
   char cc = c;
   const char *sp=(char *)0;
   while (*s){
      if (*s == cc)
         sp = s;
      s++;
   }
   if (cc == 0)
      sp = s;
   return unconst(sp, char *);
}

int strcmp(const char *s1, const char *s2){
   while (*s1 == *s2){
      if (*s1 == 0)
         return 0;
      s1++;
      s2++;
   }
   return *(unsigned const char *)s1 - *(unsigned const char *)(s2);
}

size_t strcspn(const char *s1, const char *s2){
   const char *p, *spanp;
   char c, sc;

   for (p = s1;;){
      c = *p++;
      spanp = s2;
      do {
         if ((sc = *spanp++) == c)
	         return p - 1 - s1;
      } while (sc != 0);
   }
  /* NOTREACHED */
}

int strcoll(const char *s1, const char *s2){
   return strcmp(s1, s2);
}

size_t strspn(const char *s1, const char *s2){
   const char *p = s1, *spanp;
   char c, sc;

 cont:
   c = *p++;
   for (spanp = s2; (sc = *spanp++) != 0;)
      if (sc == c)
         goto cont;
   return (p - 1 - s1);
}

/* Return a pointer to the first occurrence in s1 of any character in s2,
 * or NULL if none. (Used by GNU ld's ldlang.c for option parsing.) */
char *strpbrk(const char *s1, const char *s2){
   const char *p;
   char c;

   for (p = s1; *p; p++){
      c = *p;
      if (c != 0 && strchr(s2, c) != 0)
         return (char *)p;
   }
   return 0;
}

char *strcat(char *s, const char *append){
   char *save = s;

   for (; *s; ++s);
   while ((*s++ = *append++))
      ;
   return save;
}

char *strstr(const char *s, const char *find){
   char c, sc;
   size_t len;

   if ((c = *find++) != 0){
      len = strlen(find);
      do {
         do {
	         if ((sc = *s++) == 0)
	            return 0;
         } while (sc != c);
      } while (strncmp(s, find, len) != 0);
      s--;
   }
   return unconst(s, char *);
}

char *strncat(char *dst, const char *src, size_t n){
   if (n != 0){
      char *d = dst;
      const char *s = src;
      while (*d != 0)
         d++;
      do {
         if ((*d = *s++) == 0)
	         break;
         d++;
      } while (--n != 0);
      *d = 0;
   }
   return dst;
}

char *strncpy(char *dst, const char *src, size_t n){
   if (n != 0) {
      char *d = dst;
      const char *s = src;

      do {
         if ((*d++ = *s++) == 0){
	         while (--n != 0)
	            *d++ = 0;
	         break;
         }
      } while (--n != 0);
   }
   return dst;
}

int strncmp(const char *s1, const char *s2, size_t n){
   if (n == 0)
      return 0;
   do {
      if (*s1 != *s2++)
         return *(unsigned const char *)s1 - *(unsigned const char *)--s2;
      if (*s1++ == 0)
         break;
   } while (--n != 0);
   return 0;
}

/************Input functions*************/

int kb_deq(int *code){
   return dexsdk_systemcall(1,(long)code,0,0,0,0);
};
  
char getch(){
   int code,c;
   do{
      c=kb_deq(&code);
   }while (c==-1);
   return ((char)code);
};

int getchar(){
   char c;
   long n = dexsdk_systemcall(FXN_SYSREAD, 0, (long)&c, 1, 0, 0);
   if (n == 1)
      return (unsigned char)c;
   return (int)getch();
};

/******** Files ********/
FILE *openfile(const char  *filename,int mode){
   return (FILE*)dexsdk_systemcall(4,(long)filename,mode,0,0,0);
};

int feof(FILE *f){
   extern int ics_fdopen_fd(FILE *) __attribute__((weak));
   int fd=ics_fdopen_fd ? ics_fdopen_fd(f) : -1;
   if (fd >= 0) {
      long cur=lseek(fd,0,SEEK_CUR);
      long end=lseek(fd,0,SEEK_END);
      lseek(fd,cur,SEEK_SET);
      return cur >= end;
   }
   return dexsdk_systemcall(0x52,(long)f,0,0,0,0);
};

int clearerr(FILE *f){
   /* FILE is an opaque fd; no user-visible error/EOF flags to clear. */
   (void)f;
   return 0;
};

/* POSIX fstat() lives in posix.c */

FILE *fopen(const char *filename,const char *s){
   int read=0,write=0,append=0;
   int i;
   for (i=0;s[i];i++){
      if (s[i]=='a') 
         append=1;
      if (s[i]=='w') 
         write=1;
      if (s[i]=='r') 
         read=1;
   };
   if (append) 
      return openfile(filename,FILE_APPEND);
   if (read&&write) 
      return openfile(filename,FILE_READWRITE);
   if (read) 
      return openfile(filename,FILE_READ);
   if (write) 
      return openfile(filename,FILE_WRITE);
   return 0;
};

/* One-deep pushback slot for stdin (the SDK FILE type is an opaque fd with
   no user-side buffer). Empty when == -1. */
static int sdk_stdin_pushback = -1;

int fgetc (FILE *stream){
   unsigned char ch;
   if (stream==stdin) {
      if (sdk_stdin_pushback != -1) {
         int c = sdk_stdin_pushback;
         sdk_stdin_pushback = -1;
         return c;
      }
      return getchar();
   }
   if (fread(&ch,1,1,stream) != 1)
      return EOF;
   return ch;
};

/* ungetc: push back one character (POSIX guarantees one char).
   The SDK FILE is an opaque fd handle (no user-side buffering), so a regular
   file is pushed back by seeking one byte earlier; stdin uses the pushback
   slot above. This is all GAS needs for its backtracking lexer. */
int ungetc(int c, FILE *stream){
   if (c == EOF)
      return EOF;
   if (stream == stdin) {
      if (sdk_stdin_pushback != -1)
         return EOF;               /* pushback slot already full */
      sdk_stdin_pushback = (unsigned char)c;
      return (unsigned char)c;
   }
   long pos = ftell(stream);
   if (pos < 0)
      return EOF;
   if (fseek(stream, pos - 1, SEEK_SET) != 0)
      return EOF;
   return (unsigned char)c;
};

char *gets(char *buf){
   unsigned int i=0;
   char c;
   do{
      c=getch();
      if (c=='\r'||c=='\n'||c==0xa) 
         break;
      if (c=='\b'){
         if(i>0){
            i--;

            if (getx()==0){
               setx(79);
               if (gety()>0) 
                  sety(gety()-1);
            }else{
               setx(getx()-1);
            }
            outc(' ');
         };
      }else{
         if (i<256){  //maximum command line is only 255 characters
            charputc(buf[i]=c);
            i++;
           // setx(getx()+1);
            if (getx()>80) 
               printf("\n");
         };
      };
      outc(' ');
      update_cursor(gety(),getx());
   }while (c!='\r');
   setx(0);
   printf("\n");
   buf[i]=0;
   return buf;
};

char *fgets(char *s, int n, FILE* f){
   extern int ics_fdopen_fd(FILE *) __attribute__((weak));
   int fd=ics_fdopen_fd ? ics_fdopen_fd(f) : -1;
   if (f==stdin){
      int x;
      gets(s);
      x=strlen(s);
      s[x]='\n';
      s[x+1]= 0;
      return s;
   };
   if (fd >= 0) {
      int i=0;
      while (i<n-1 && read(fd,&s[i],1)==1) {
         if (s[i++]=='\n')
            break;
      }
      s[i]=0;
      return i ? s : 0;
   }
   return (char*)dexsdk_systemcall(0x40,(long)s,n,(long)f,0,0);
};

int fread(void *buf,int itemsize,int noitems,FILE* fhandle){
   extern int ics_fdopen_fd(FILE *) __attribute__((weak));
   int fd=ics_fdopen_fd ? ics_fdopen_fd(fhandle) : -1;
   if (fd >= 0) {
      int bytes;
      if (itemsize<=0 || noitems<=0)
         return 0;
      bytes=read(fd,buf,(unsigned long)itemsize*(unsigned long)noitems);
      return bytes > 0 ? bytes/itemsize : 0;
   }
   return dexsdk_systemcall(0x39,(long)buf,itemsize,noitems,(long)fhandle,0);
};

/* FILE is an opaque kernel handle, so retain a small user-side write cache
   keyed by that handle. Compiler output commonly arrives through putc();
   issuing int 0x30 once per byte makes native compilation prohibitively slow. */
#define SDK_WRITE_SLOTS 8
#define SDK_WRITE_BUFSIZE 4096
struct sdk_write_buffer {
   FILE *stream;
   unsigned int used;
   char data[SDK_WRITE_BUFSIZE];
};
static struct sdk_write_buffer sdk_write_buffers[SDK_WRITE_SLOTS];

static int sdk_raw_fwrite(const void *buf, int itemsize, int noitems,
                          FILE *fhandle){
   extern int ics_fdopen_fd(FILE *) __attribute__((weak));
   int fd=ics_fdopen_fd ? ics_fdopen_fd(fhandle) : -1;
   if (fd >= 0) {
      int bytes;
      if (itemsize<=0 || noitems<=0)
         return 0;
      bytes=write(fd,buf,(unsigned long)itemsize*(unsigned long)noitems);
      return bytes > 0 ? bytes/itemsize : 0;
   }
   return dexsdk_systemcall(0x45,(long)buf,itemsize,noitems,(long)fhandle,0);
}

static struct sdk_write_buffer *sdk_write_buffer(FILE *stream, int create){
   int i;
   struct sdk_write_buffer *free_slot = 0;
   for (i = 0; i < SDK_WRITE_SLOTS; i++) {
      if (sdk_write_buffers[i].stream == stream)
         return &sdk_write_buffers[i];
      if (!sdk_write_buffers[i].stream && !free_slot)
         free_slot = &sdk_write_buffers[i];
   }
   if (create && free_slot) {
      free_slot->stream = stream;
      free_slot->used = 0;
      return free_slot;
   }
   return 0;
}

static int sdk_flush_write_buffer(FILE *stream, int release){
   struct sdk_write_buffer *b = sdk_write_buffer(stream, 0);
   int r = 0;
   if (b && b->used) {
      r = sdk_raw_fwrite(b->data, 1, b->used, stream);
      b->used = 0;
   }
   if (b && release)
      b->stream = 0;
   return r;
}

static unsigned int sdk_buffered_write_bytes(FILE *stream){
   struct sdk_write_buffer *b = sdk_write_buffer(stream, 0);
   return b ? b->used : 0;
}

int fwrite(const void *buf,int itemsize,int noitems,FILE* fhandle){
   const char *p = (const char *)buf;
   unsigned int total;
   unsigned int left;
   struct sdk_write_buffer *b;
   if (itemsize <= 0 || noitems <= 0)
      return 0;
   if (fhandle == stdout || fhandle == stderr)
      return sdk_raw_fwrite(buf,itemsize,noitems,fhandle);
   total = (unsigned int)itemsize * (unsigned int)noitems;
   if (total >= SDK_WRITE_BUFSIZE) {
      sdk_flush_write_buffer(fhandle, 0);
      return sdk_raw_fwrite(buf,itemsize,noitems,fhandle);
   }
   b = sdk_write_buffer(fhandle, 1);
   if (!b)
      return sdk_raw_fwrite(buf,itemsize,noitems,fhandle);
   left = total;
   while (left) {
      unsigned int room = SDK_WRITE_BUFSIZE - b->used;
      unsigned int chunk = left < room ? left : room;
      memcpy(b->data + b->used, p, chunk);
      b->used += chunk;
      p += chunk;
      left -= chunk;
      if (b->used == SDK_WRITE_BUFSIZE)
         sdk_flush_write_buffer(fhandle, 0);
   }
   return noitems;
};
 
int fputc(int c,FILE *f){
   struct sdk_write_buffer *b;
   char ch = (char)c;
   if ( f ==  stdout || f == stderr) //stdout
      charputc(ch);
   else {
      b = sdk_write_buffer(f, 1);
      if (!b)
         sdk_raw_fwrite(&ch,1,1,f);
      else {
         b->data[b->used++] = ch;
         if (b->used == SDK_WRITE_BUFSIZE)
            sdk_flush_write_buffer(f, 0);
      }
   }
   return (unsigned char)c;
};

int fputs(const char *s, FILE *stream){
   int n = strlen(s);
   if (stream == stdout || stream == stderr) {
      int i;
      for (i = 0; i < n; i++)
         charputc(s[i]);
      return n;
   }
   return fwrite(s, 1, n, stream);
}

 
int fclose(FILE *stream){
   extern int ics_fdopen_close(FILE *) __attribute__((weak));
   if (stream==stdout || stream==stderr || stream==stdin)
      return 0;
   sdk_flush_write_buffer(stream, 1);
   if (ics_fdopen_close && ics_fdopen_close(stream))
      return 0;
   closefile(stream);
   return 0;
};

int fflush (FILE *stream){
   extern int ics_fdopen_fd(FILE *) __attribute__((weak));
   int fd;
   int i;
   if (!stream) {
      for (i = 0; i < SDK_WRITE_SLOTS; i++)
         if (sdk_write_buffers[i].stream)
            sdk_flush_write_buffer(sdk_write_buffers[i].stream, 0);
      return 0;
   }
   if (stream == stdout || stream==stderr || stream == stdin) 
      return 0;
   sdk_flush_write_buffer(stream, 0);
   fd=ics_fdopen_fd ? ics_fdopen_fd(stream) : -1;
   if (fd >= 0)
      return fsync(fd);
   return dexsdk_systemcall(0x59,(long)stream,0,0,0,0);
};

/* fseek lives in posix.c (POSIX int return) */

long int ftell(FILE *stream){
   extern int ics_fdopen_fd(FILE *) __attribute__((weak));
   int fd=ics_fdopen_fd ? ics_fdopen_fd(stream) : -1;
   long pos = fd >= 0 ? lseek(fd,0,SEEK_CUR) :
                        dexsdk_systemcall(0x47,(long)stream,0,0,0,0);
   if (pos >= 0)
      pos += sdk_buffered_write_bytes(stream);
   return pos;
};

int closefile(FILE* fhandle){
   return dexsdk_systemcall(5,(long)fhandle,0,0,0,0);
};

int remove(char *filename){
   return dexsdk_systemcall(0x49,(long)filename,0,0,0,0);
};

int mkdir (const char *filename, mode_t mode){
    return dexsdk_systemcall(0x4A,(long)filename,0,0,0,0);
};

int copyfile(const char *src, const char *dest){
    return dexsdk_systemcall(0x97,(long)src,(long)dest,0,0,0);
};

//------------------------------------------
int atoi(const char *str){
    int i = strlen(str) - 1 , i2 = 1;
    int num = 0;
    for (;i>=0; i--)
      {
        if ( (str[i]>='0') && (str[i]<='9') )
        {
            num += (str[i]-'0') * i2;
            i2*=10;
        };
      };
    return num;
};


//VGA system calls
void set_graphics(int mode){
    dexsdk_systemcall(0x60,(int)mode,0,0,0,0);
}

void write_pixel(int x, int y, char color){
    dexsdk_systemcall(0x5F,(int)x,(int)y,(char)color,0,0);
}

void write_text(char *str, int x,int y, char color,int size){
    dexsdk_systemcall(0x9D,(long)str,(int)x,(int)y,(char)color,(int)size);
}

void write_char(unsigned char ch, int x,int y, char color,int size){
    dexsdk_systemcall(0x9E,ch,(int)x,(int)y,(char)color,(int)size);
}

void read_palette(char *r, char *g, char *b, char index){
    dexsdk_systemcall(0x62,*r,*g,*b,index,0);
}

void write_palette(char r, char g, char b, char index){
    dexsdk_systemcall(0x61,r,g,b,index,0);
}



//------------------------time
void delay(unsigned int ms){
    dexsdk_systemcall(0x9B,ms,0,0,0,0);
}

/* time() lives in posix.c */

void get_date_time(dex32_datetime *datetime){ 
    dexsdk_systemcall(0x53,(long)datetime,0,0,0,0);
}

//keypressed
void kb_ready(){
    dexsdk_systemcall(0x9C,0,0,0,0,0);
}

//random
/* Named rand_seed (not 'next'): with ONE_SOURCE=0 the TinyCC preprocessor's
   global function next() (tccpp.c) collides with this global symbol at link. */
static unsigned long int rand_seed = 1;

/* rand: return pseudo-random integer on 0..32767 */
int rand(void){
   rand_seed = rand_seed * 1103515245 + 12345;
   return (unsigned int)(rand_seed/65536) % 32768;
}

/* srand: set seed for rand() */
void srand(unsigned int seed){
   rand_seed = seed;
}

/*Process control functions */
int fork(){
   return dexsdk_systemcall(0x90,0,0,0,0,0);   
}

int getpid(){
   return dexsdk_systemcall(0x2,0,0,0,0,0);
};

int exec(char *fname,unsigned short  mode, char *params){
   return dexsdk_systemcall(0x5C,(long)fname,mode,(long)params,0,0);     
}

int execp(char *fname,unsigned short mode, char *params){
   return dexsdk_systemcall(0x5B,(long)fname, mode,(long)params,0,0);     
}

void sleep(unsigned int ms){
    dexsdk_systemcall(0x9B,ms,0,0,0,0);
}

/* POSIX wait(): wait for any child to terminate, return its pid and
 * store the exit status. Delegates to the robust waitpid(-1) path, which
 * directly switches the CPU to the child and uses the parent's waitq
 * (the legacy 0xC spin-wait never yielded to the child and returned a
 * bogus status, so GNU make's blocking wait() spun forever). */
int wait(int *status){
   return (int)waitpid(-1, status, 0);
}

/* Terminates the calling thread. The kernel exit() path is
   thread-aware: kill_process() routes PCBs flagged PS_ATTB_THREAD
   to kill_thread(), so the ordinary exit syscall does the job. */
void thread_exit(){
   dexsdk_systemcall(3,0,0,0,0,0);
   while (1);   /* not reached */
}

/* Creates and starts a thread. Returns the thread id.
   The kernel sets the new thread's ESP to stk+stacksize-4 and its
   EIP to f, but places no return address on the stack. We seed the
   top-of-stack slot with the address of thread_exit, so a thread
   function that simply returns lands in thread_exit and terminates
   cleanly instead of jumping to a garbage address.
   Note: the thread's stack is heap memory of this process; it is
   reclaimed when the process exits. */
int thread_create(void *f){
   unsigned char *stk = (unsigned char *)malloc(10240);
   *((unsigned int*)(stk + 10240 - 4)) = (unsigned int)thread_exit;
   return dexsdk_systemcall(0xB,(long)f,(long)stk,10240,0,0);
}

/* Waits until the given thread of the calling process terminates.
   Returns 0 on success (or if the thread already exited), -1 if
   tid is not a thread of this process. */
int thread_join(int tid){
   return dexsdk_systemcall(0xA1,tid,0,0,0,0);
}

/* getenv() lives in posix.c (POSIX 1-arg) */

int setenv(const char *name, const char *value, int replace){
   return dexsdk_systemcall(0xA0,(long)name,(long)value,(int)replace,0,0);
}



