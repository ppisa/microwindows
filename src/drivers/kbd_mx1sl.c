/*******************************************************************
  Components for embedded applications builded for
  laboratory and medical instruments firmware

  kbd_mx1sl.c - periodically scanned keyboard MicroWindows driver

  Copyright (C) 2001 by Pavel Pisa pisa@cmp.felk.cvut.cz
            (C) 2002 by PiKRON Ltd. http://www.pikron.com

 *******************************************************************/

#include <string.h>
#include <mwtypes.h>
#include "device.h"

#define WITH_RTEMS

#ifndef WITH_RTEMS

#include <types.h>
#include <cpu_def.h>
#include <system_def.h>
#include <mx1_reg.h>

#define KEY_TIMER msec_time
#define KEY_PUSH_T	20
#define KEY_RELEASE_T	10
#define KEY_REPFIRST_T	700
#define KEY_REPNEXT_T	200

typedef unsigned long kbdisr_lock_level_t;
#define kbdisr_lock   save_and_cli
#define	kbdisr_unlock restore_flags

#else /*WITH_RTEMS*/

#include <rtems.h>
#include <inttypes.h>
#include <mc9328mxl.h>

#define KEY_TIMER  ({rtems_interval ticks=0; \
	  rtems_clock_get(RTEMS_CLOCK_GET_TICKS_SINCE_BOOT,&ticks); \
	  ticks; \
	})

#define KEY_PUSH_T	20
#define KEY_RELEASE_T	10
#define KEY_REPFIRST_T	700
#define KEY_REPNEXT_T	200

#define _reg_PTA_DR MC9328MXL_GPIOA_DR
#define _reg_PTA_SSR MC9328MXL_GPIOA_SSR
#define _reg_PTA_DDIR MC9328MXL_GPIOA_DDIR
#define _reg_PTA_PUEN MC9328MXL_GPIOA_PUEN

#define _reg_PTD_DR MC9328MXL_GPIOD_DR
#define _reg_PTD_SSR MC9328MXL_GPIOD_SSR
#define _reg_PTD_DDIR MC9328MXL_GPIOD_DDIR
#define _reg_PTD_PUEN MC9328MXL_GPIOD_PUEN

typedef rtems_interrupt_level kbdisr_lock_level_t;
#define kbdisr_lock   rtems_interrupt_disable
#define	kbdisr_unlock rtems_interrupt_enable

#endif /*WITH_RTEMS*/


static int  mx1_kbd_Open(KBDDEVICE *pkd);
static void mx1_kbd_Close(void);
static void mx1_kbd_GetModifierInfo(MWKEYMOD *modifiers, MWKEYMOD *curmodifiers);
static int  mx1_kbd_Read(MWKEY *buf, MWKEYMOD *modifiers, MWSCANCODE *scancode);
static int  mx1_kbd_Poll(void);

KBDDEVICE kbddev = {
	mx1_kbd_Open,
	mx1_kbd_Close,
	mx1_kbd_GetModifierInfo,
	mx1_kbd_Read,
	mx1_kbd_Poll
};

int mx1_kbd_variat_config;

/*
 *-----------------------------------------------------------
 */


/*
 * MX_KBD matrix is 3 inputs x 8 outputs.
 * Outputs are PD23 to PD30
 * Inputs are PD20 to PD22.
 */

#define KBD_SCAN_CNT 8
#define KBD_RET_CNT 3

static inline
unsigned char mx1_kbd_onerow_variant_1(unsigned char scan)
{
	kbdisr_lock_level_t level;
	unsigned int scan_mask=(0xff<<23); /*PD*/
	unsigned int scan_val=(~scan<<23) & scan_mask; /*PD*/
	unsigned int ret;

	kbdisr_lock(level);

	_reg_PTD_DR|=scan_val;
	_reg_PTD_DR&=scan_val|~scan_mask;
	_reg_PTD_DR=_reg_PTD_DR; /*Some time to propagate signals*/
	ret=_reg_PTD_SSR;

	kbdisr_unlock(level);

	return (~ret>>20)&7;
}

static inline
void mx1_kbd_setio_variant_1(void)
{
	kbdisr_lock_level_t level;
	unsigned int scan_mask=(0xff<<23); /*PD*/
	unsigned int ret_mask=(0x7<<20);   /*PD*/

	kbdisr_lock(level);

	_reg_PTD_DR|=scan_mask;
	_reg_PTD_DDIR|=scan_mask;
	_reg_PTD_DDIR&=~ret_mask;
	_reg_PTD_PUEN|=ret_mask;

	kbdisr_unlock(level);
}

static inline
unsigned char mx1_kbd_onerow_variant_4(unsigned char scan)
{
	kbdisr_lock_level_t level;
	unsigned int scan_mask=(0xff<<5); /*PA*/
	unsigned int scan_val=(~scan<<5) & scan_mask; /*PD*/
	unsigned int ret;

	kbdisr_lock(level);

	_reg_PTA_DR|=scan_val;
	_reg_PTA_DR&=scan_val|~scan_mask;
	_reg_PTA_DR=_reg_PTA_DR; /*Some time to propagate signals*/
	ret=_reg_PTD_SSR;

	kbdisr_unlock(level);

	return (~ret>>20)&7;
}

static inline
void mx1_kbd_setio_variant_4(void)
{
	kbdisr_lock_level_t level;
	unsigned int scan_mask=(0xff<<5);  /*PA*/
	unsigned int ret_mask=(0x7<<20);   /*PD*/

	kbdisr_lock(level);

	_reg_PTA_DR|=scan_mask;
	_reg_PTA_DDIR|=scan_mask;
	_reg_PTD_DDIR&=~ret_mask;
	_reg_PTD_PUEN|=ret_mask;

	kbdisr_unlock(level);
}

static inline
unsigned char mx1_kbd_onerow(unsigned char scan)
{
	if(mx1_kbd_variat_config == '1')
		return mx1_kbd_onerow_variant_1(scan);
	else if(mx1_kbd_variat_config == '4')
		return mx1_kbd_onerow_variant_4(scan);
	return 0;
}

static inline
void mx1_kbd_setio(void)
{
	if(!mx1_kbd_variat_config)
		mx1_kbd_variat_config='1';

	if(mx1_kbd_variat_config == '1')
		mx1_kbd_setio_variant_1();
	else if(mx1_kbd_variat_config == '4')
		mx1_kbd_setio_variant_4();
}

typedef struct {
	MWKEY bc;
	MWKEY sc;
} scan2mwkey_t;

typedef struct {
	int scan;
	int flag;
	MWKEYMOD is_mod;
	MWKEYMOD set_mod;
	MWKEYMOD xor_mod;
} scan2mwmod_t;

#define MWKMOD_SGM_SC	0x8000

#if 0

const scan2mwkey_t mx1_kbd_scan2mwkey_basic[]={
	[0x00]={0,0},
	[0x21]={MWKEY_F1,0},
	[0x22]={MWKEY_F2,0},
	[0x23]={MWKEY_F3,0},
	[0x24]={MWKEY_F4,0},
	[0x2c]={MWKEY_F5,0},
	[0x25]={MWKEY_F6,0},
	[0x26]={MWKEY_F7,0},
	[0x27]={MWKEY_F8,0},
	[0x28]={MWKEY_F9,0},
	[0x04]={MWKEY_KP0,MWKEY_INSERT},
	[0x03]={MWKEY_KP1,MWKEY_END},
	[0x0b]={MWKEY_KP2,MWKEY_DOWN},
	[0x13]={MWKEY_KP3,MWKEY_PAGEDOWN},
	[0x02]={MWKEY_KP4,MWKEY_LEFT},
	[0x0a]={MWKEY_KP5,MWKEY_BACKSPACE},
	[0x12]={MWKEY_KP6,MWKEY_RIGHT},
	[0x01]={MWKEY_KP7,MWKEY_HOME},
	[0x09]={MWKEY_KP8,MWKEY_UP},
	[0x11]={MWKEY_KP9,MWKEY_PAGEUP},
	[0x0c]={MWKEY_KP_PERIOD,MWKEY_DELETE},
	[0x14]={MWKEY_ENTER,MWKEY_ESCAPE},
	[0x16]={MWKEY_ENTER,MWKEY_ESCAPE},
};

const scan2mwmod_t mx1_kbd_scan2mwmod_basic[]={
	{0x28,0,0,MWKMOD_LSHIFT|MWKMOD_SGM_SC,0},
	{0,0,0,0}
};

#endif

const scan2mwkey_t mx1_kbd_scan2mwkey_basic[]={
	[0x00]={0,0},
	[0x0b]={'0',0},
	[0x01]={'1',0},
	[0x02]={'2',0},
	[0x03]={'3',0},
	[0x04]={'4',0},
	[0x05]={'5',0},
	[0x06]={'6',0},
	[0x07]={'7',0},
	[0x08]={'8',0},
	[0x09]={'9',0},
	[0x0a]={'.',0},
	[0x0c]={MWKEY_ESCAPE,0},

	[0x0f]={MWKEY_UP,0},
	[0x13]={MWKEY_DOWN,0},
	[0x11]={MWKEY_ENTER,0},

	[0x0d]={MWKEY_F1,0},
	[0x0e]={MWKEY_F2,0},
	[0x10]={MWKEY_F3,0},	/*MWKEY_LEFT*/
	[0x12]={MWKEY_F4,0},	/*MWKEY_RIGHT*/
	[0x14]={MWKEY_F5,0},
	[0x15]={MWKEY_F6,0},

	[0x16]={MWKEY_SUSPEND,0},
};

const scan2mwmod_t mx1_kbd_scan2mwmod_basic[]={
	{0,0,0,0}
};

scan2mwkey_t *mx1_kbd_scan2mwkey_tab=(scan2mwkey_t*)mx1_kbd_scan2mwkey_basic;
scan2mwmod_t *mx1_kbd_scan2mwmod_tab=(scan2mwmod_t*)mx1_kbd_scan2mwmod_basic;

/* State of keyboard matrix and key press reporting */

static unsigned char key_down_arr[KBD_SCAN_CNT];
static unsigned char key_chng_arr[KBD_SCAN_CNT];
static unsigned char key_hit;

static MWKEYMOD key_mod;

static int key_last_changed;

/* Internal state for repeat processing */

static short key_use_timer;
static long  key_time;
static short key_state;

#define KEY_STATE_IDLE     0
#define KEY_STATE_PUSH     1
#define KEY_STATE_RELEASE  2
#define KEY_STATE_REPEAT   4
#define KEY_STATE_NOISE    8
#define KEY_STATE_BUSY     (KEY_STATE_PUSH|KEY_STATE_RELEASE)


/**
 * mx1_kbd_scan - Scan keyboard matrix and report requests for state change
 *
 * Scans keyboard matrix connected row by row by calling function
 * mx1_kbd_onerow(). Number of scanned output lines is defined
 * by %KBD_SCAN_CNT. Checks read keyboard state against @key_down_arr
 * and updates @key_change_arr array. The @key_down_arr state is
 * left unchanged. It is changed later by mx1_kbd_down() function.
 * Returns 0, if no keyboard activity is found. Returns 1
 * if at least one key is pressed. Returns 2 or 3 in case
 * of detected change.
 */
int
mx1_kbd_scan()
{
	int i, ret=0;
	unsigned char mask, val, chng;
	for(i=0,mask=1;i<KBD_SCAN_CNT;i++,mask<<=1) {
		val=mx1_kbd_onerow(mask);
		chng=val^key_down_arr[i];
		key_chng_arr[i]=chng;
		if(val) ret|=1;
		if(chng) ret|=2;
	}
	mx1_kbd_onerow(~0);
	return ret;
}


/**
 * mx1_kbd_scan2mod - Propagate keyboard matrix changes between modifiers
 * @scan_code:		Scan code of last detected key change
 *
 * Functions check keyboard matrix state in @key_down_arr.
 * It updates @key_mod according to @key_down_arr and
 * modifiers transformations table @mx1_kbd_scan2mwmod_tab.
 */
void
mx1_kbd_scan2mod(int scan_code)
{
	unsigned char val, chng;
	int s;
	scan2mwmod_t *mt=mx1_kbd_scan2mwmod_tab;

	for(;(s=mt->scan);mt++) {
		chng=(s==scan_code);
		s--;
		val=key_down_arr[s/KBD_RET_CNT]&(1<<(s%KBD_RET_CNT));
		if(val) {
			key_mod|=mt->set_mod;
			if(chng){
				key_mod^=mt->xor_mod;
			}
		} else {
			key_mod&=~mt->set_mod;
		}
	}
}

/**
 * mx1_kbd_down - Detects changed key scancode and applies changes to matrix state
 *
 * Functions check @key_chng_arr and process changes.
 * It updates its internal state @key_state, does
 * noise cancellation and repeat timing, then updates
 * @key_down_arr, stores detected scancode to @key_last_changed
 * and calls modifiers processing mx1_kbd_scan2mod().
 * Return value is zero if no change is detected.
 * In other case evaluated scancode is returned.
 * Variable @key_hit signals by value 1 pressed key, by value
 * 2 key release.
 */
int
mx1_kbd_down()
{
	int i, j=0;
	unsigned char val;

        if(!(key_state&KEY_STATE_BUSY)){
		for(i=0;i<KBD_SCAN_CNT;i++) {
			if(!(val=key_chng_arr[i])) continue;
			for(j=0;!(val&1);j++) val>>=1;
			key_last_changed=i*KBD_RET_CNT+j+1;
			if(key_down_arr[i]&(1<<j)){
				key_time=KEY_TIMER+KEY_PUSH_T;
				key_state=KEY_STATE_RELEASE;
			}else{
				key_time=KEY_TIMER+KEY_RELEASE_T;
				key_state=KEY_STATE_PUSH;
			}
			break;
		}
		if(key_state==KEY_STATE_IDLE)
			return 0;
	} else {
		if(!key_last_changed){
			key_state=KEY_STATE_IDLE;
	  		return 0;
		}
		i=(key_last_changed-1)/KBD_RET_CNT;
		j=(key_last_changed-1)%KBD_RET_CNT;
		if(!(key_chng_arr[i]&(1<<j))){
			/* Noise detected */
			if(!(key_state&KEY_STATE_NOISE)){
			        key_time=KEY_TIMER+KEY_RELEASE_T;
			        key_state|=KEY_STATE_NOISE;
			}
		}
	}

	if(!key_use_timer){
		if(KEY_TIMER) key_use_timer=1;
		if(key_state&KEY_STATE_REPEAT) return 0;
	}else{
		if((long)(KEY_TIMER-key_time)<0) return 0;
	}

	if(key_state==KEY_STATE_PUSH) {
		key_down_arr[i]|=1<<j;
		mx1_kbd_scan2mod(key_last_changed);
		key_state=KEY_STATE_REPEAT;
		key_time=KEY_TIMER+KEY_REPFIRST_T;
		key_hit=1;
		return key_last_changed;
	} else if(key_state==KEY_STATE_REPEAT) {
		key_time=KEY_TIMER+KEY_REPNEXT_T;
		key_hit=1;
		return key_last_changed;
	} else if(key_state==KEY_STATE_RELEASE) {
		key_down_arr[i]&=~(1<<j);
		mx1_kbd_scan2mod(key_last_changed);
	        key_state=KEY_STATE_IDLE;
		key_hit=2;
		return key_last_changed;
	}
	key_state=KEY_STATE_IDLE;
	return 0;
}

/**
 * mx1_kbd_scan2mwkey - Converts scancode to MWKEY keyboard values
 * @scan:	Detected scancode
 *
 * Computes MWKEY value for detected scancode.
 * Uses @mx1_kbd_scan2mwkey_tab transformation table
 * and @key_mod modifiers information.
 */
MWKEY mx1_kbd_scan2mwkey(int scan)
{
	if((key_mod&MWKMOD_SGM_SC)&&mx1_kbd_scan2mwkey_tab[scan].sc)
		return mx1_kbd_scan2mwkey_tab[scan].sc;
	return mx1_kbd_scan2mwkey_tab[scan].bc;
}

int mx1_kbd_check_key_pressed(MWKEY key)
{
  int ret;
  int scan;
  scan2mwkey_t *p=mx1_kbd_scan2mwkey_tab+1;

  for(scan=0;scan<KBD_SCAN_CNT;scan++){
    for(ret=0;ret<KBD_RET_CNT;ret++){
      if((p->bc==key) || (p->sc==key)){
        return mx1_kbd_onerow(1<<scan)&(1<<ret);
      }
      p++;
    }
  }
  return 0;
}


/*
 *-----------------------------------------------------------
 */


/**
 * mx1_kbd_scan2mwkey - Open the keyboard
 * @pkd:	Pointer to keyboard device
 */
static int
mx1_kbd_Open(KBDDEVICE *pkd)
{
	key_last_changed=0;
	key_mod=0;
	key_hit=0;
	key_use_timer=0;
	memset(key_down_arr,0,sizeof(key_down_arr));
	mx1_kbd_setio();
	return 0;
}

/**
 * mx1_kbd_Close - Closes keyboard
 */
static void
mx1_kbd_Close(void)
{
}

/**
 * mx1_kbd_Poll - Polls for keyboard events
 *
 * Returns non-zero value if change is detected.
 */
static int
mx1_kbd_Poll(void)
{
	if(key_hit)
		return 1;
	if(mx1_kbd_scan())
		mx1_kbd_down();
	return key_hit?1:0;
}

/**
 * mx1_kbd_GetModifierInfo - Returns the possible modifiers for the keyboard
 * @modifiers:		If non-NULL, ones in defined modifiers bits are returned.
 * @curmodifiers:	If non-NULL, ones in actually active modifiers
 *			bits are returned.
 */
static  void
mx1_kbd_GetModifierInfo(MWKEYMOD *modifiers, MWKEYMOD *curmodifiers)
{
	if (modifiers)
		*modifiers = 0;		/* no modifiers available */
	if (curmodifiers)
		*curmodifiers = key_mod&~MWKMOD_SGM_SC;
}

/**
 * mx1_kbd_Read - Reads resolved MWKEY value, modifiers and scancode
 * @buf:		If non-NULL, resolved MWKEY is stored here
 * @modifiers:		If non-NULL, ones in actually active modifiers
 *			bits are returned
 * @scancode:		If non-NULL, scancode of resolved key is stored
 *			here
 *
 * This function reads one keystroke from the keyboard, and the current state
 * of the modifier keys (ALT, SHIFT, etc).  Returns -1 on error, 0 if no data
 * is ready, 1 on a keypress, and 2 on keyrelease.
 * This is a non-blocking call.
 */
static int
mx1_kbd_Read(MWKEY *buf, MWKEYMOD *modifiers, MWSCANCODE *scancode)
{
        int ret;
	if(!key_hit) {
		if(mx1_kbd_scan()){
			mx1_kbd_down();
		}
	}
	if(modifiers)
		*modifiers = key_mod&~MWKMOD_SGM_SC;
	if(!key_hit)
		return 0;
	if(scancode)
		*scancode = key_last_changed;
	if(buf)
		*buf = mx1_kbd_scan2mwkey(key_last_changed);
	ret=key_hit;
	key_hit=0;
	return ret;
}
