
#ifndef __STRUM_H
#define __STRUM_H
/* Used to define stringizing macro to stringize user defined macros
 * which can't be stringized in normal way */

#define _strum(um) #um

/* HEY!!! Use me, i.e., strum(um) to STRingize User-Macros.. 
 * not my small brother! */
#define strum(um) _strum (um)

#endif /*__STRUM_H*/

