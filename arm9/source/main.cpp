/* REminiscence - Flashback interpreter
 * Copyright (C) 2005-2007 Gregory Montoir
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */
#ifdef __NDS__
#include <nds.h>
#include <fat.h>  
//#include <filesystem.h>
#endif

#include "file.h"
#include "fs.h"
#include "game.h"
#include "systemstub.h"

#ifdef __NDS__
#include "backscreen.h"
#include "backcredit.h"
#include "topscreen.h"

#define REM_MENUINIT 0x01
#define REM_MENUSHOW 0x02
#define REM_PLAYGAME 0x03
#define REM_PLAYSGAM 0x04 
#define REM_MENUCRED 0x05
#define REM_QUITSTDS 0x06

int bg0, bg1, bg0b, bg1b;

bool gameToLoad=false;

extern u8 *fake_heap_end;     // current heap start
extern u8 *fake_heap_start;   // current heap end

u8* getHeapStart() {return fake_heap_start;}
u8* getHeapEnd()   {return (u8*)sbrk(0);}
u8* getHeapLimit() {return fake_heap_end;}

int getMemUsed() { // returns the amount of used memory in bytes
   struct mallinfo mi = mallinfo();
   return mi.uordblks;
}

int getMemFree() { // returns the amount of free memory in bytes
   struct mallinfo mi = mallinfo();
   return mi.fordblks + (getHeapLimit() - getHeapEnd());
}


bool dsSaveIndicator(void) 
{
  unsigned short dmaval = *(bgGetMapPtr(bg0b)+29*32);
  for (int i=0;i<10;i++) dmaCopy((void *) (bgGetMapPtr(bg0b)+(24+i)*32),(void *) (bgGetMapPtr(bg1b)+(9+i)*32+12),10*2);
		
  for (int i=0; i<60; i++)
  {
      swiWaitForVBlank();
  }
  
  dmaFillWords(dmaval | (dmaval<<16),bgGetMapPtr(bg1b),24*32*2);
  return 1;
}

unsigned int dsWaitOnMenu(void) {
  // Palette index 179, 180, 181
  unsigned int uState=REM_MENUINIT;
  unsigned int keys_pressed;
  bool bDone=false;
  int iUp=0, iDown=0, iA=0, iFlp=0, iIncPal = 1, iSensPal=0;
  static int iPal=222;
  
  while (!bDone) {
    // wait for stylus
    keys_pressed = keysCurrent();
    if (keys_pressed & KEY_UP) {
      if (!iUp) {
        iUp = 1;
        BG_PALETTE_SUB[iPal] = RGB15(31,31,31);
        iPal = iPal == 222 ? 224 : iPal-1;
      }
    }
    else iUp = 0;
    if (keys_pressed & KEY_DOWN) {
      if (!iDown) {
        iDown = 1;
        BG_PALETTE_SUB[iPal] = RGB15(31,31,31);
        iPal = iPal == 224 ? 222 : iPal+1;
      }
    }
    else iDown = 0;
    if (keys_pressed & (KEY_A | KEY_START)) {
      if (!iA) {
        iA = 1; bDone = true;
        if (iPal == 222) uState=REM_PLAYGAME;
        if (iPal == 223) uState=REM_PLAYSGAM;
        if (iPal == 224) uState=REM_MENUCRED;
      }
    }
    else iA = 0;
    iFlp++;
    if (iFlp >= 10) { 
      iFlp=0; BG_PALETTE_SUB[iPal] = RGB15(iIncPal,iIncPal,iIncPal); 
      iIncPal += (iSensPal ? 2 : -2); if ( (iIncPal >= 31) || (iIncPal <= 1)) { iSensPal = 1 - iSensPal; }
    }
    swiWaitForVBlank();
  }
  
  return uState;
}
#else

static const char *USAGE =
	"REminiscence - Flashback Interpreter\n"
	"Usage: %s [OPTIONS]...\n"
	"  --datapath=PATH   Path to data files (default 'DATA')\n"
	"  --savepath=PATH   Path to save files (default '.')\n"
	"  --levelnum=NUM    Starting level (default '0')";

static bool parseOption(const char *arg, const char *longCmd, const char **opt) {
	bool handled = false;
	if (arg[0] == '-' && arg[1] == '-') {
		if (strncmp(arg + 2, longCmd, strlen(longCmd)) == 0) {
			*opt = arg + 2 + strlen(longCmd);
			handled = true;
		}
	}
	return handled;
}
#endif

static int detectVersion(FileSystem *fs) {
	static const struct {
		const char *filename;
		int type;
		const char *name;
	} table[] = {
		{ "LEVEL1.MAP", kResourceTypePC, "PC" },
		{ "LEVEL1.LEV", kResourceTypeAmiga, "Amiga" },
		{ 0, -1 }
	};
	for (int i = 0; table[i].filename; ++i) {
		File f;
		if (f.open(table[i].filename, "rb", fs)) {
			debug(DBG_INFO, "Detected %s version", table[i].name);
			return table[i].type;
		}
	}
	return -1;
}

static Language detectLanguage(FileSystem *fs) {
	static const struct {
		const char *filename;
		Language language;
	} table[] = {
		// PC
		{ "ENGCINE.TXT", LANG_EN },
		{ "FR_CINE.TXT", LANG_FR },
		{ "GERCINE.TXT", LANG_DE },
		{ "SPACINE.TXT", LANG_SP },
		// Amiga
		{ "FRCINE.TXT", LANG_FR },
		{ 0, LANG_EN }
	};
	for (int i = 0; table[i].filename; ++i) {
		File f;
		if (f.open(table[i].filename, "rb", fs)) {
			return table[i].language;
		}
	}
	return LANG_EN;
}

#undef main
int main(int argc, char *argv[]) {
#ifdef __NDS__
  const char *dataPath = "/flashback";
  const char *savePath = "/flashback";
  const char *levelNum = "0";
  
  unsigned int etatEmu = REM_MENUINIT;
  // Init sound
  consoleDemoInit();
  soundEnable();
  lcdMainOnTop();

  // Init Fat
	if (!fatInitDefault()) {
		iprintf("Unable to initialize libfat!\n");
		return -1;
	}

  while(etatEmu != REM_QUITSTDS) {
    switch (etatEmu) {
      case REM_MENUINIT:
	    {
#if 1
        // Init BG mode for 8 bits colors , tiles mode
        videoSetMode(MODE_0_2D | DISPLAY_BG0_ACTIVE );
        videoSetModeSub(MODE_0_2D | DISPLAY_BG0_ACTIVE | DISPLAY_BG1_ACTIVE);
        vramSetBankA(VRAM_A_MAIN_BG); vramSetBankC(VRAM_C_SUB_BG);
        bg0 = bgInit(0, BgType_Text8bpp, BgSize_T_256x256, 31,0);
        bg0b = bgInitSub(0, BgType_Text8bpp, BgSize_T_256x256, 31,0);
        bg1b = bgInitSub(1, BgType_Text8bpp, BgSize_T_256x256, 30,0);
#endif
        memset((u8 *)0x06000000, 0x00, 0x20000); // Ensure screen is clear...
		unsigned short dmaval = *(bgGetMapPtr(bg0b)+29*32);
        decompress(backscreenTiles, bgGetGfxPtr(bg0b), LZ77Vram);
        decompress(backscreenMap, (void*) bgGetMapPtr(bg0b), LZ77Vram);
        dmaCopy((void *) backscreenPal,(u16*) BG_PALETTE_SUB,256*2);
		dmaFillWords(dmaval | (dmaval<<16),bgGetMapPtr(bg1b),24*32*2);
        bgSetPriority(bg0b,1);bgSetPriority(bg1b,0);
        
        decompress(topscreenTiles, bgGetGfxPtr(bg0), LZ77Vram);
        decompress(topscreenMap, (void*) bgGetMapPtr(bg0), LZ77Vram);
        dmaCopy((void *) topscreenPal,(u16*) BG_PALETTE,256*2);
                
        etatEmu = REM_MENUSHOW;
		}
        break;
        
      case REM_MENUSHOW:
        etatEmu =  dsWaitOnMenu();
        break;
      
      case REM_MENUCRED:
        decompress(backcreditTiles, bgGetGfxPtr(bg0b), LZ77Vram);
        decompress(backcreditMap, (void*) bgGetMapPtr(bg0b), LZ77Vram);
        dmaCopy((void *) backcreditPal,(u16*) BG_PALETTE_SUB,256*2);
        while (keysCurrent())  {asm("nop");}
        while (!keysCurrent()) {asm("nop");}
        while (keysCurrent())  {asm("nop");}
        etatEmu = REM_MENUINIT;
        break;
       
      case REM_PLAYSGAM:
      case REM_PLAYGAME:
        {
          memset((u8 *)0x06000000, 0x00, 0x20000); // Ensure screen is clear...
          g_debugMask = 0; //DBG_INFO; // DBG_CUT | DBG_VIDEO | DBG_RES | DBG_MENU | DBG_PGE | DBG_GAME | DBG_UNPACK | DBG_COL | DBG_MOD | DBG_SFX | DBG_FILE;
          FileSystem fs(dataPath);
          const int version = detectVersion(&fs);
          if (version == -1) {
            error("Unable to find data files, check that all required files are present");
            return -1;
          }
          REG_BLDCNT_SUB = BLEND_FADE_BLACK | BLEND_SRC_BG0 | BLEND_DST_BG0; REG_BLDY_SUB = 14;
          Language language = detectLanguage(&fs);
          SystemStub *stub = SystemStub_SDL_create();
          if (etatEmu == REM_PLAYSGAM) gameToLoad = true;
          Game *g = new Game(stub, &fs, savePath, atoi(levelNum), (ResourceType)version, language);
          g->run();
          delete g;
          delete stub;
          REG_BLDCNT_SUB = 0; REG_BLDY_SUB = 0;
          etatEmu = REM_MENUINIT;
        }
        break;
    }
	}
#else
	const char *dataPath = "DATA";
	const char *savePath = ".";
	const char *levelNum = "0";
	for (int i = 1; i < argc; ++i) {
		bool opt = false;
		if (strlen(argv[i]) >= 2) {
			opt |= parseOption(argv[i], "datapath=", &dataPath);
			opt |= parseOption(argv[i], "savepath=", &savePath);
			opt |= parseOption(argv[i], "levelnum=", &levelNum);
		}
		if (!opt) {
			printf(USAGE, argv[0]);
			return 0;
		}
	}
	g_debugMask = 0; //DBG_INFO; // DBG_CUT | DBG_VIDEO | DBG_RES | DBG_MENU | DBG_PGE | DBG_GAME | DBG_UNPACK | DBG_COL | DBG_MOD | DBG_SFX | DBG_FILE;
	FileSystem fs(dataPath);
	const int version = detectVersion(&fs);
	if (version == -1) {
		error("Unable to find data files, check that all required files are present");
		return -1;
	}
	Language language = detectLanguage(&fs);
	SystemStub *stub = SystemStub_SDL_create();
	Game *g = new Game(stub, &fs, savePath, atoi(levelNum), (ResourceType)version, language);
	g->run();
	delete g;
	delete stub;
#endif
  
	return 0;
}
