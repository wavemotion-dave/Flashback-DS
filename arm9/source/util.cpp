/* REminiscence - Flashback interpreter
 * Copyright (C) 2005-2011 Gregory Montoir
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <cstdarg>
#include "util.h"

#ifdef __NDS__        
#include <nds.h>
#endif

uint16 g_debugMask;

void debug(uint16 cm, const char *msg, ...) {
	char buf[256];
	if (cm & g_debugMask) {
		va_list va;
		va_start(va, msg);
		vsprintf(buf, msg, va);
		va_end(va);
#ifdef __NDS__        
        nocashMessage(buf);
#else        
		printf("%s\n", buf);
		fflush(stdout);
#endif        
	}
}

void error(const char *msg, ...) {
	char buf[256];
	va_list va;
	va_start(va, msg);
	vsnprintf(buf, sizeof(buf), msg, va);
	va_end(va);
#ifdef __NDS__
    REG_BLDCNT_SUB = 0; REG_BLDY_SUB = 0;
    consoleDemoInit();
    printf("ERROR: %s!\n", buf);
    while (keysCurrent())  {asm("nop");}
    while (!keysCurrent()) {asm("nop");}
    while (keysCurrent())  {asm("nop");}
#else
	fprintf(stderr, "ERROR: %s!\n", buf);
#endif        
	exit(-1);
}

void warning(const char *msg, ...) {
#ifdef __NDS__
	char buf[256],buf1[256+16];
#else
	char buf[256+16];
#endif
	va_list va;
	va_start(va, msg);
	vsnprintf(buf, sizeof(buf), msg, va);
	va_end(va);
#ifdef __NDS__
    sprintf(buf1,"WARNING: %s!", buf);
    nocashMessage(buf1);
#else
	fprintf(stderr, "WARNING: %s!\n", buf);
#endif    
}

