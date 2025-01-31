/*
 * This file is part of RGBDS.
 *
 * Copyright (c) 1997-2019, Carsten Sorensen and RGBDS contributors.
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Outputs an objectfile
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "asm/asm.h"
#include "asm/charmap.h"
#include "asm/fstack.h"
#include "asm/main.h"
#include "asm/output.h"
#include "asm/rpn.h"
#include "asm/symbol.h"

#include "extern/err.h"

#include "common.h"
#include "linkdefs.h"

void out_SetCurrentSection(struct Section *pSect);

struct Patch {
	char tzFilename[_MAX_PATH + 1];
	uint32_t nLine;
	uint32_t nOffset;
	uint8_t nType;
	uint32_t nRPNSize;
	uint8_t *pRPN;
	struct Patch *pNext;
};

struct PatchSymbol {
	uint32_t ID;
	struct sSymbol *pSymbol;
	struct PatchSymbol *pNext;
	struct PatchSymbol *pBucketNext; /* next symbol in hash table bucket */
};

struct SectionStackEntry {
	struct Section *pSection;
	struct sSymbol *pScope; /* Section's symbol scope */
	struct SectionStackEntry *pNext;
};

struct PatchSymbol *tHashedPatchSymbols[HASHSIZE];
struct Section *pSectionList, *pCurrentSection;
struct PatchSymbol *pPatchSymbols;
struct PatchSymbol **ppPatchSymbolsTail = &pPatchSymbols;
char *tzObjectname;
struct SectionStackEntry *pSectionStack;

/*
 * Section stack routines
 */
void out_PushSection(void)
{
	struct SectionStackEntry *pSect;

	pSect = malloc(sizeof(struct SectionStackEntry));
	if (pSect == NULL)
		fatalerror("No memory for section stack");

	pSect->pSection = pCurrentSection;
	pSect->pScope = sym_GetCurrentSymbolScope();
	pSect->pNext = pSectionStack;
	pSectionStack = pSect;
}

void out_PopSection(void)
{
	if (pSectionStack == NULL)
		fatalerror("No entries in the section stack");

	struct SectionStackEntry *pSect;

	pSect = pSectionStack;
	out_SetCurrentSection(pSect->pSection);
	sym_SetCurrentSymbolScope(pSect->pScope);
	pSectionStack = pSect->pNext;
	free(pSect);
}

static uint32_t getmaxsectionsize(uint32_t secttype, char *sectname)
{
	switch (secttype) {
	case SECT_ROM0:
		return 0x8000; /* If ROMX sections not used */
	case SECT_ROMX:
		return 0x4000;
	case SECT_VRAM:
		return 0x2000;
	case SECT_SRAM:
		return 0x2000;
	case SECT_WRAM0:
		return 0x2000; /* If WRAMX sections not used */
	case SECT_WRAMX:
		return 0x1000;
	case SECT_OAM:
		return 0xA0;
	case SECT_HRAM:
		return 0x7F;
	default:
		break;
	}
	errx(1, "Section \"%s\" has an invalid section type.", sectname);
}

/*
 * Count the number of symbols used in this object
 */
static uint32_t countsymbols(void)
{
	struct PatchSymbol *pSym;
	uint32_t count = 0;

	pSym = pPatchSymbols;

	while (pSym) {
		count += 1;
		pSym = pSym->pNext;
	}

	return (count);
}

/*
 * Count the number of sections used in this object
 */
static uint32_t countsections(void)
{
	struct Section *pSect;
	uint32_t count = 0;

	pSect = pSectionList;

	while (pSect) {
		count += 1;
		pSect = pSect->pNext;
	}

	return (count);
}

/*
 * Count the number of patches used in this object
 */
static uint32_t countpatches(struct Section *pSect)
{
	struct Patch *pPatch;
	uint32_t r = 0;

	pPatch = pSect->pPatches;
	while (pPatch) {
		r += 1;
		pPatch = pPatch->pNext;
	}

	return (r);
}

/*
 * Write a long to a file (little-endian)
 */
static void fputlong(uint32_t i, FILE *f)
{
	fputc(i, f);
	fputc(i >> 8, f);
	fputc(i >> 16, f);
	fputc(i >> 24, f);
}

/*
 * Write a NULL-terminated string to a file
 */
static void fputstring(char *s, FILE *f)
{
	while (*s)
		fputc(*s++, f);
	fputc(0, f);
}

/*
 * Return a section's ID
 */
static uint32_t getsectid(struct Section *pSect)
{
	struct Section *sec;
	uint32_t ID = 0;

	sec = pSectionList;

	while (sec) {
		if (sec == pSect)
			return ID;
		ID += 1;
		sec = sec->pNext;
	}

	fatalerror("%s: Unknown section", __func__);
	return (uint32_t)(-1);
}

/*
 * Write a patch to a file
 */
static void writepatch(struct Patch *pPatch, FILE *f)
{
	fputstring(pPatch->tzFilename, f);
	fputlong(pPatch->nLine, f);
	fputlong(pPatch->nOffset, f);
	fputc(pPatch->nType, f);
	fputlong(pPatch->nRPNSize, f);
	fwrite(pPatch->pRPN, 1, pPatch->nRPNSize, f);
}

/*
 * Write a section to a file
 */
static void writesection(struct Section *pSect, FILE *f)
{
	fputstring(pSect->pzName, f);

	fputlong(pSect->nPC, f);

	fputc(pSect->nType, f);

	fputlong(pSect->nOrg, f);
	fputlong(pSect->nBank, f);
	fputlong(pSect->nAlign, f);

	if ((pSect->nType == SECT_ROM0) || (pSect->nType == SECT_ROMX)) {
		struct Patch *pPatch;

		fwrite(pSect->tData, 1, pSect->nPC, f);
		fputlong(countpatches(pSect), f);

		pPatch = pSect->pPatches;
		while (pPatch) {
			writepatch(pPatch, f);
			pPatch = pPatch->pNext;
		}
	}
}

/*
 * Write a symbol to a file
 */
static void writesymbol(struct sSymbol *pSym, FILE *f)
{
	uint32_t type;
	uint32_t offset;
	int32_t sectid;

	if (!(pSym->nType & SYMF_DEFINED)) {
		if (pSym->nType & SYMF_LOCAL) {
			char *name = pSym->tzName;
			char *localPtr = strchr(name, '.');

			if (localPtr)
				name = localPtr;
			errx(1, "%s(%u) : '%s' not defined",
			     pSym->tzFileName, pSym->nFileLine, name);
		}
		type = SYM_IMPORT;
	} else if (pSym->nType & SYMF_EXPORT) {
		type = SYM_EXPORT;
	} else {
		type = SYM_LOCAL;
	}

	switch (type) {
	case SYM_LOCAL:
		offset = pSym->nValue;
		sectid = getsectid(pSym->pSection);
		break;
	case SYM_IMPORT:
		offset = 0;
		sectid = -1;
		break;
	case SYM_EXPORT:
		offset = pSym->nValue;
		if (pSym->nType & SYMF_CONST)
			sectid = -1;
		else
			sectid = getsectid(pSym->pSection);
		break;
	}

	fputstring(pSym->tzName, f);
	fputc(type, f);

	if (type != SYM_IMPORT) {
		fputstring(pSym->tzFileName, f);
		fputlong(pSym->nFileLine, f);

		fputlong(sectid, f);
		fputlong(offset, f);
	}
}

/*
 * Add a symbol to the object
 */
static uint32_t nextID;

static uint32_t addsymbol(struct sSymbol *pSym)
{
	struct PatchSymbol *pPSym, **ppPSym;
	uint32_t hash;

	hash = sym_CalcHash(pSym->tzName);
	ppPSym = &(tHashedPatchSymbols[hash]);

	while ((*ppPSym) != NULL) {
		if (pSym == (*ppPSym)->pSymbol)
			return (*ppPSym)->ID;
		ppPSym = &((*ppPSym)->pBucketNext);
	}

	pPSym = malloc(sizeof(struct PatchSymbol));
	*ppPSym = pPSym;

	if (pPSym == NULL)
		fatalerror("No memory for patchsymbol");

	pPSym->pNext = NULL;
	pPSym->pBucketNext = NULL;
	pPSym->pSymbol = pSym;
	pPSym->ID = nextID++;

	*ppPatchSymbolsTail = pPSym;
	ppPatchSymbolsTail = &(pPSym->pNext);

	return pPSym->ID;
}

/*
 * Add all exported symbols to the object
 */
static void addexports(void)
{
	int32_t i;

	for (i = 0; i < HASHSIZE; i += 1) {
		struct sSymbol *pSym;

		pSym = tHashedSymbols[i];
		while (pSym) {
			if (pSym->nType & SYMF_EXPORT)
				addsymbol(pSym);
			pSym = pSym->pNext;
		}
	}
}

/*
 * Allocate a new patchstructure and link it into the list
 */
struct Patch *allocpatch(void)
{
	struct Patch *pPatch;

	pPatch = malloc(sizeof(struct Patch));

	if (pPatch == NULL)
		fatalerror("No memory for patch");

	pPatch->pNext = pCurrentSection->pPatches;
	pPatch->nRPNSize = 0;
	pPatch->pRPN = NULL;
	pCurrentSection->pPatches = pPatch;

	return pPatch;
}

/*
 * Create a new patch (includes the rpn expr)
 */
void createpatch(uint32_t type, struct Expression *expr)
{
	struct Patch *pPatch;
	uint16_t rpndata;
	uint8_t *rpnexpr;
	char tzSym[512];
	uint32_t rpnptr = 0, symptr;

	rpnexpr = malloc(expr->nRPNPatchSize);

	if (rpnexpr == NULL)
		fatalerror("No memory for patch RPN expression");

	pPatch = allocpatch();
	pPatch->nType = type;
	strcpy(pPatch->tzFilename, tzCurrentFileName);
	pPatch->nLine = nLineNo;
	pPatch->nOffset = nPC;

	while ((rpndata = rpn_PopByte(expr)) != 0xDEAD) {
		switch (rpndata) {
		case RPN_CONST:
			rpnexpr[rpnptr++] = RPN_CONST;
			rpnexpr[rpnptr++] = rpn_PopByte(expr);
			rpnexpr[rpnptr++] = rpn_PopByte(expr);
			rpnexpr[rpnptr++] = rpn_PopByte(expr);
			rpnexpr[rpnptr++] = rpn_PopByte(expr);
			break;
		case RPN_SYM:
			symptr = 0;
			while ((tzSym[symptr++] = rpn_PopByte(expr)) != 0)
				;

			if (sym_isConstant(tzSym)) {
				uint32_t value;

				value = sym_GetConstantValue(tzSym);
				rpnexpr[rpnptr++] = RPN_CONST;
				rpnexpr[rpnptr++] = value & 0xFF;
				rpnexpr[rpnptr++] = value >> 8;
				rpnexpr[rpnptr++] = value >> 16;
				rpnexpr[rpnptr++] = value >> 24;
			} else {
				struct sSymbol *sym = sym_FindSymbol(tzSym);

				if (sym == NULL)
					break;

				symptr = addsymbol(sym);
				rpnexpr[rpnptr++] = RPN_SYM;
				rpnexpr[rpnptr++] = symptr & 0xFF;
				rpnexpr[rpnptr++] = symptr >> 8;
				rpnexpr[rpnptr++] = symptr >> 16;
				rpnexpr[rpnptr++] = symptr >> 24;
			}
			break;
		case RPN_BANK_SYM:
		{
			struct sSymbol *sym;

			symptr = 0;
			while ((tzSym[symptr++] = rpn_PopByte(expr)) != 0)
				;

			sym = sym_FindSymbol(tzSym);
			if (sym == NULL)
				break;

			symptr = addsymbol(sym);
			rpnexpr[rpnptr++] = RPN_BANK_SYM;
			rpnexpr[rpnptr++] = symptr & 0xFF;
			rpnexpr[rpnptr++] = symptr >> 8;
			rpnexpr[rpnptr++] = symptr >> 16;
			rpnexpr[rpnptr++] = symptr >> 24;
			break;
		}
		case RPN_BANK_SECT:
		{
			uint16_t b;

			rpnexpr[rpnptr++] = RPN_BANK_SECT;

			do {
				b = rpn_PopByte(expr);
				rpnexpr[rpnptr++] = b & 0xFF;
			} while (b != 0);
			break;
		}
		default:
			rpnexpr[rpnptr++] = rpndata;
			break;
		}
	}

	assert(rpnptr == expr->nRPNPatchSize);

	pPatch->pRPN = rpnexpr;
	pPatch->nRPNSize = rpnptr;
}

/*
 * A quick check to see if we have an initialized section
 */
static void checksection(void)
{
	if (pCurrentSection == NULL)
		fatalerror("Code generation before SECTION directive");
}

/*
 * A quick check to see if we have an initialized section that can contain
 * this much initialized data
 */
static void checkcodesection(void)
{
	checksection();
	if (pCurrentSection->nType != SECT_ROM0 &&
	    pCurrentSection->nType != SECT_ROMX) {
		fatalerror("Section '%s' cannot contain code or data (not ROM0 or ROMX)",
			   pCurrentSection->pzName);
	} else if (nUnionDepth > 0) {
		fatalerror("UNIONs cannot contain code or data");
	}
}

/*
 * Check if the section has grown too much.
 */
static void checksectionoverflow(uint32_t delta_size)
{
	uint32_t maxsize = getmaxsectionsize(pCurrentSection->nType,
					  pCurrentSection->pzName);
	uint32_t new_size = pCurrentSection->nPC + delta_size;

	if (new_size > maxsize) {
		/*
		 * This check is here to trap broken code that generates
		 * sections that are too big and to prevent the assembler from
		 * generating huge object files or trying to allocate too much
		 * memory.
		 * The real check must be done at the linking stage.
		 */
		fatalerror("Section '%s' is too big (max size = 0x%X bytes, reached 0x%X).",
			   pCurrentSection->pzName, maxsize, new_size);
	}
}

/*
 * Write an objectfile
 */
void out_WriteObject(void)
{
	FILE *f;

	addexports();

	/* If no path specified, don't write file */
	if (tzObjectname == NULL)
		return;

	f = fopen(tzObjectname, "wb");
	if (f == NULL)
		fatalerror("Couldn't write file '%s'\n", tzObjectname);

	struct PatchSymbol *pSym;
	struct Section *pSect;

	fwrite(RGBDS_OBJECT_VERSION_STRING, 1,
	       strlen(RGBDS_OBJECT_VERSION_STRING), f);

	fputlong(countsymbols(), f);
	fputlong(countsections(), f);

	pSym = pPatchSymbols;
	while (pSym) {
		writesymbol(pSym->pSymbol, f);
		pSym = pSym->pNext;
	}

	pSect = pSectionList;
	while (pSect) {
		writesection(pSect, f);
		pSect = pSect->pNext;
	}

	fclose(f);
}

/*
 * Set the objectfilename
 */
void out_SetFileName(char *s)
{
	tzObjectname = s;
	if (CurrentOptions.verbose)
		printf("Output filename %s\n", s);
}

/*
 * Find a section by name and type. If it doesn't exist, create it
 */
struct Section *out_FindSection(char *pzName, uint32_t secttype, int32_t org,
				int32_t bank, int32_t alignment)
{
	struct Section *pSect, **ppSect;

	ppSect = &pSectionList;
	pSect = pSectionList;

	while (pSect) {
		if (strcmp(pzName, pSect->pzName) == 0) {
			if (secttype == pSect->nType
			    && ((uint32_t)org) == pSect->nOrg
			    && ((uint32_t)bank) == pSect->nBank
			    && ((uint32_t)alignment == pSect->nAlign)) {
				return pSect;
			}

			fatalerror("Section already exists but with a different type");
		}
		ppSect = &(pSect->pNext);
		pSect = pSect->pNext;
	}

	pSect = malloc(sizeof(struct Section));
	*ppSect = pSect;
	if (pSect == NULL)
		fatalerror("Not enough memory for section");

	pSect->pzName = malloc(strlen(pzName) + 1);
	if (pSect->pzName == NULL)
		fatalerror("Not enough memory for sectionname");

	strcpy(pSect->pzName, pzName);
	pSect->nType = secttype;
	pSect->nPC = 0;
	pSect->nOrg = org;
	pSect->nBank = bank;
	pSect->nAlign = alignment;
	pSect->pNext = NULL;
	pSect->pPatches = NULL;
	pSect->charmap = NULL;

	/* It is only needed to allocate memory for ROM sections. */
	if (secttype == SECT_ROM0 || secttype == SECT_ROMX) {
		uint32_t sectsize;

		sectsize = getmaxsectionsize(secttype, pzName);
		pSect->tData = malloc(sectsize);
		if (pSect->tData == NULL)
			fatalerror("Not enough memory for section");
	} else {
		pSect->tData = NULL;
	}

	return (pSect);
}

/*
 * Set the current section
 */
void out_SetCurrentSection(struct Section *pSect)
{
	if (nUnionDepth > 0)
		fatalerror("Cannot change the section within a UNION");

	pCurrentSection = pSect;
	nPC = (pSect != NULL) ? pSect->nPC : 0;

	pPCSymbol->nValue = nPC;
	pPCSymbol->pSection = pCurrentSection;
}

/*
 * Set the current section by name and type
 */
void out_NewSection(char *pzName, uint32_t secttype)
{
	out_SetCurrentSection(out_FindSection(pzName, secttype, -1, -1, 1));
}

/*
 * Set the current section by name and type
 */
void out_NewAbsSection(char *pzName, uint32_t secttype, int32_t org,
		       int32_t bank)
{
	out_SetCurrentSection(out_FindSection(pzName, secttype, org, bank, 1));
}

/*
 * Set the current section by name and type, using a given byte alignment
 */
void out_NewAlignedSection(char *pzName, uint32_t secttype, int32_t alignment,
			   int32_t bank)
{
	if (alignment < 0 || alignment > 16)
		yyerror("Alignment must be between 0-16 bits.");

	out_SetCurrentSection(out_FindSection(pzName, secttype, -1, bank,
					      1 << alignment));
}

/*
 * Output an absolute byte (bypassing ROM/union checks)
 */
void out_AbsByteBypassCheck(int32_t b)
{
	checksectionoverflow(1);
	b &= 0xFF;
	pCurrentSection->tData[nPC] = b;
	pCurrentSection->nPC += 1;
	nPC += 1;
	pPCSymbol->nValue += 1;
}

/*
 * Output an absolute byte
 */
void out_AbsByte(int32_t b)
{
	checkcodesection();
	out_AbsByteBypassCheck(b);
}

void out_AbsByteGroup(char *s, int32_t length)
{
	checkcodesection();
	checksectionoverflow(length);
	while (length--)
		out_AbsByte(*s++);
}

/*
 * Skip this many bytes
 */
void out_Skip(int32_t skip)
{
	checksection();
	checksectionoverflow(skip);
	if (!((pCurrentSection->nType == SECT_ROM0)
		|| (pCurrentSection->nType == SECT_ROMX))) {
		pCurrentSection->nPC += skip;
		nPC += skip;
		pPCSymbol->nValue += skip;
	} else if (nUnionDepth > 0) {
		while (skip--)
			out_AbsByteBypassCheck(CurrentOptions.fillchar);
	} else {
		checkcodesection();
		while (skip--)
			out_AbsByte(CurrentOptions.fillchar);
	}
}

/*
 * Output a NULL terminated string (excluding the NULL-character)
 */
void out_String(char *s)
{
	checkcodesection();
	checksectionoverflow(strlen(s));
	while (*s)
		out_AbsByte(*s++);
}

/*
 * Output a relocatable byte. Checking will be done to see if it
 * is an absolute value in disguise.
 */
void out_RelByte(struct Expression *expr)
{
	checkcodesection();
	checksectionoverflow(1);
	if (rpn_isReloc(expr)) {
		pCurrentSection->tData[nPC] = 0;
		createpatch(PATCH_BYTE, expr);
		pCurrentSection->nPC += 1;
		nPC += 1;
		pPCSymbol->nValue += 1;
	} else {
		out_AbsByte(expr->nVal);
	}
	rpn_Free(expr);
}

/*
 * Output an absolute word
 */
void out_AbsWord(int32_t b)
{
	checkcodesection();
	checksectionoverflow(2);
	b &= 0xFFFF;
	pCurrentSection->tData[nPC] = b & 0xFF;
	pCurrentSection->tData[nPC + 1] = b >> 8;
	pCurrentSection->nPC += 2;
	nPC += 2;
	pPCSymbol->nValue += 2;
}

/*
 * Output a relocatable word. Checking will be done to see if
 * it's an absolute value in disguise.
 */
void out_RelWord(struct Expression *expr)
{
	checkcodesection();
	checksectionoverflow(2);
	if (rpn_isReloc(expr)) {
		pCurrentSection->tData[nPC] = 0;
		pCurrentSection->tData[nPC + 1] = 0;
		createpatch(PATCH_WORD_L, expr);
		pCurrentSection->nPC += 2;
		nPC += 2;
		pPCSymbol->nValue += 2;
	} else {
		out_AbsWord(expr->nVal);
	}
	rpn_Free(expr);
}

/*
 * Output an absolute longword
 */
void out_AbsLong(int32_t b)
{
	checkcodesection();
	checksectionoverflow(sizeof(int32_t));
	pCurrentSection->tData[nPC] = b & 0xFF;
	pCurrentSection->tData[nPC + 1] = b >> 8;
	pCurrentSection->tData[nPC + 2] = b >> 16;
	pCurrentSection->tData[nPC + 3] = b >> 24;
	pCurrentSection->nPC += 4;
	nPC += 4;
	pPCSymbol->nValue += 4;
}

/*
 * Output a relocatable longword. Checking will be done to see if
 * is an absolute value in disguise.
 */
void out_RelLong(struct Expression *expr)
{
	checkcodesection();
	checksectionoverflow(4);
	if (rpn_isReloc(expr)) {
		pCurrentSection->tData[nPC] = 0;
		pCurrentSection->tData[nPC + 1] = 0;
		pCurrentSection->tData[nPC + 2] = 0;
		pCurrentSection->tData[nPC + 3] = 0;
		createpatch(PATCH_LONG_L, expr);
		pCurrentSection->nPC += 4;
		nPC += 4;
		pPCSymbol->nValue += 4;
	} else {
		out_AbsLong(expr->nVal);
	}
	rpn_Free(expr);
}

/*
 * Output a PC-relative relocatable byte. Checking will be done to see if it
 * is an absolute value in disguise.
 */
void out_PCRelByte(struct Expression *expr)
{
	checkcodesection();
	checksectionoverflow(1);

	/* Always let the linker calculate the offset. */
	pCurrentSection->tData[nPC] = 0;
	createpatch(PATCH_BYTE_JR, expr);
	pCurrentSection->nPC += 1;
	nPC += 1;
	pPCSymbol->nValue += 1;

	rpn_Free(expr);
}

/*
 * Output a binary file
 */
void out_BinaryFile(char *s)
{
	FILE *f;

	f = fstk_FindFile(s, NULL);
	if (f == NULL)
		err(1, "Unable to open incbin file '%s'", s);

	int32_t fsize;

	fseek(f, 0, SEEK_END);
	fsize = ftell(f);
	fseek(f, 0, SEEK_SET);

	checkcodesection();
	checksectionoverflow(fsize);

	int32_t dest = nPC;
	int32_t todo = fsize;

	while (todo--)
		pCurrentSection->tData[dest++] = fgetc(f);

	pCurrentSection->nPC += fsize;
	nPC += fsize;
	pPCSymbol->nValue += fsize;
	fclose(f);
}

void out_BinaryFileSlice(char *s, int32_t start_pos, int32_t length)
{
	FILE *f;

	if (start_pos < 0)
		fatalerror("Start position cannot be negative");

	if (length < 0)
		fatalerror("Number of bytes to read must be greater than zero");

	f = fstk_FindFile(s, NULL);
	if (f == NULL)
		err(1, "Unable to open included file '%s'", s);

	int32_t fsize;

	fseek(f, 0, SEEK_END);
	fsize = ftell(f);

	if (start_pos >= fsize)
		fatalerror("Specified start position is greater than length of file");

	if ((start_pos + length) > fsize)
		fatalerror("Specified range in INCBIN is out of bounds");

	fseek(f, start_pos, SEEK_SET);

	checkcodesection();
	checksectionoverflow(length);

	int32_t dest = nPC;
	int32_t todo = length;

	while (todo--)
		pCurrentSection->tData[dest++] = fgetc(f);

	pCurrentSection->nPC += length;
	nPC += length;
	pPCSymbol->nValue += length;

	fclose(f);
}
