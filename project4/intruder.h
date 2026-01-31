//intruder.h

#ifndef INTRUDER_H
#define INTRUDER_H

#include <stdint.h>

/* Intruder configuration */
#define INTRUDER_LENGTH 10
#define INTRUDER_SPEED 40

/* Intruder state flag */
extern int intruder_flag;  // 0 = no intruder, 1 = intruder active

/*
@Function: 
    *Thread function for keyboard input to generate an intruder event
*/
void* keyboard_listener(void* arg); 

#endif