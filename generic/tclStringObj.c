/*
 * tclStringObj.c --
 *
 *	This file contains functions that implement string operations on Tcl
 *	objects. Some string operations work with UTF strings and others
 *	require Unicode format. Functions that require knowledge of the width
 *	of each character, such as indexing, operate on Unicode data.
 *
 *	A Unicode string is an internationalized string. Conceptually, a
 *	Unicode string is an array of 16-bit quantities organized as a
 *	sequence of properly formed UTF-8 characters. There is a one-to-one
 *	map between Unicode and UTF characters. Because Unicode characters
 *	have a fixed width, operations such as indexing operate on Unicode
 *	data. The String object is optimized for the case where each UTF char
 *	in a string is only one byte. In this case, we store the value of
 *	numChars, but we don't store the Unicode data (unless Tcl_GetUnicode
 *	is explicitly called).
 *
 *	The String object type stores one or both formats. The default
 *	behavior is to store UTF. Once Unicode is calculated by a function, it
 *	is stored in the internal rep for future access (without an additional
 *	O(n) cost).
 *
 *	To allow many appends to be done to an object without constantly
 *	reallocating the space for the string or Unicode representation, we
 *	allocate double the space for the string or Unicode and use the
 *	internal representation to keep track of how much space is used vs.
 *	allocated.
 *
 * Copyright © 1995-1997 Sun Microsystems, Inc.
 * Copyright © 1999 Scriptics Corporation.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tclInt.h"
#include "tclTomMath.h"
#include "tclStringRep.h"

#include "assert.h"
/*
 * Prototypes for functions defined later in this file:
 */

static void		AppendPrintfToObjVA(Tcl_Obj *objPtr,
			    const char *format, va_list argList);
static void		AppendUnicodeToUnicodeRep(Tcl_Obj *objPtr,
			    const Tcl_UniChar *unicode, size_t appendNumChars);
static void		AppendUnicodeToUtfRep(Tcl_Obj *objPtr,
			    const Tcl_UniChar *unicode, size_t numChars);
static void		AppendUtfToUnicodeRep(Tcl_Obj *objPtr,
			    const char *bytes, size_t numBytes);
static void		AppendUtfToUtfRep(Tcl_Obj *objPtr,
			    const char *bytes, size_t numBytes);
static void		DupStringInternalRep(Tcl_Obj *objPtr,
			    Tcl_Obj *copyPtr);
static size_t		ExtendStringRepWithUnicode(Tcl_Obj *objPtr,
			    const Tcl_UniChar *unicode, size_t numChars);
static void		ExtendUnicodeRepWithString(Tcl_Obj *objPtr,
			    const char *bytes, size_t numBytes,
			    size_t numAppendChars);
static void		FillUnicodeRep(Tcl_Obj *objPtr);
static void		FreeStringInternalRep(Tcl_Obj *objPtr);
static void		GrowStringBuffer(Tcl_Obj *objPtr, size_t needed, int flag);
static void		GrowUnicodeBuffer(Tcl_Obj *objPtr, size_t needed);
static int		SetStringFromAny(Tcl_Interp *interp, Tcl_Obj *objPtr);
static void		SetUnicodeObj(Tcl_Obj *objPtr,
			    const Tcl_UniChar *unicode, size_t numChars);
static size_t		UnicodeLength(const Tcl_UniChar *unicode);
static void		UpdateStringOfString(Tcl_Obj *objPtr);

#define ISCONTINUATION(bytes) (\
	((((bytes)[0] & 0xC0) == 0x80) || (((bytes)[0] == '\xED') \
	&& (((bytes)[1] & 0xF0) == 0xB0) && (((bytes)[2] & 0xC0) == 0x80))))


/*
 * The structure below defines the string Tcl object type by means of
 * functions that can be invoked by generic object code.
 */

const Tcl_ObjType tclStringType = {
    "string",			/* name */
    FreeStringInternalRep,	/* freeIntRepPro */
    DupStringInternalRep,	/* dupIntRepProc */
    UpdateStringOfString,	/* updateStringProc */
    SetStringFromAny		/* setFromAnyProc */
};

/*
 * TCL STRING GROWTH ALGORITHM
 *
 * When growing strings (during an append, for example), the following growth
 * algorithm is used:
 *
 *   Attempt to allocate 2 * (originalLength + appendLength)
 *   On failure:
 *	attempt to allocate originalLength + 2*appendLength + TCL_MIN_GROWTH
 *
 * This algorithm allows very good performance, as it rapidly increases the
 * memory allocated for a given string, which minimizes the number of
 * reallocations that must be performed. However, using only the doubling
 * algorithm can lead to a significant waste of memory. In particular, it may
 * fail even when there is sufficient memory available to complete the append
 * request (but there is not 2*totalLength memory available). So when the
 * doubling fails (because there is not enough memory available), the
 * algorithm requests a smaller amount of memory, which is still enough to
 * cover the request, but which hopefully will be less than the total
 * available memory.
 *
 * The addition of TCL_MIN_GROWTH allows for efficient handling of very
 * small appends. Without this extra slush factor, a sequence of several small
 * appends would cause several memory allocations. As long as
 * TCL_MIN_GROWTH is a reasonable size, we can avoid that behavior.
 *
 * The growth algorithm can be tuned by adjusting the following parameters:
 *
 * TCL_MIN_GROWTH		Additional space, in bytes, to allocate when
 *				the double allocation has failed. Default is
 *				1024 (1 kilobyte).  See tclInt.h.
 */

#ifndef TCL_MIN_UNICHAR_GROWTH
#define TCL_MIN_UNICHAR_GROWTH	TCL_MIN_GROWTH/sizeof(Tcl_UniChar)
#endif

static void
GrowStringBuffer(
    Tcl_Obj *objPtr,
    size_t needed,
    int flag)
{
    /*
     * Pre-conditions:
     *	objPtr->typePtr == &tclStringType
     *	needed > stringPtr->allocated
     *	flag || objPtr->bytes != NULL
     */

    String *stringPtr = GET_STRING(objPtr);
    char *ptr = NULL;
    size_t attempt;

    if (objPtr->bytes == &tclEmptyString) {
	objPtr->bytes = NULL;
    }
    if (flag == 0 || stringPtr->allocated > 0) {
	attempt = 2 * needed;
	ptr = (char *)Tcl_AttemptRealloc(objPtr->bytes, attempt + 1);
	if (ptr == NULL) {
	    /*
	     * Take care computing the amount of modest growth to avoid
	     * overflow into invalid argument values for attempt.
	     */

	    size_t limit = INT_MAX - needed;
	    size_t extra = needed - objPtr->length + TCL_MIN_GROWTH;
	    size_t growth = (extra > limit) ? limit : extra;

	    attempt = needed + growth;
	    ptr = (char *)Tcl_AttemptRealloc(objPtr->bytes, attempt + 1);
	}
    }
    if (ptr == NULL) {
	/*
	 * First allocation - just big enough; or last chance fallback.
	 */

	attempt = needed;
	ptr = (char *)Tcl_Realloc(objPtr->bytes, attempt + 1);
    }
    objPtr->bytes = ptr;
    stringPtr->allocated = attempt;
}

static void
GrowUnicodeBuffer(
    Tcl_Obj *objPtr,
    size_t needed)
{
    /*
     * Pre-conditions:
     *	objPtr->typePtr == &tclStringType
     *	needed > stringPtr->maxChars
     */

    String *ptr = NULL, *stringPtr = GET_STRING(objPtr);
    size_t attempt;

    if (stringPtr->maxChars > 0) {
	/*
	 * Subsequent appends - apply the growth algorithm.
	 */

	attempt = 2 * needed;
	ptr = stringAttemptRealloc(stringPtr, attempt);
	if (ptr == NULL) {
	    /*
	     * Take care computing the amount of modest growth to avoid
	     * overflow into invalid argument values for attempt.
	     */

	    size_t extra = needed - stringPtr->numChars
		    + TCL_MIN_UNICHAR_GROWTH;

	    attempt = needed + extra;
	    ptr = stringAttemptRealloc(stringPtr, attempt);
	}
    }
    if (ptr == NULL) {
	/*
	 * First allocation - just big enough; or last chance fallback.
	 */

	attempt = needed;
	ptr = stringRealloc(stringPtr, attempt);
    }
    stringPtr = ptr;
    stringPtr->maxChars = attempt;
    SET_STRING(objPtr, stringPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_NewStringObj --
 *
 *	This function is normally called when not debugging: i.e., when
 *	TCL_MEM_DEBUG is not defined. It creates a new string object and
 *	initializes it from the byte pointer and length arguments.
 *
 *	When TCL_MEM_DEBUG is defined, this function just returns the result
 *	of calling the debugging version Tcl_DbNewStringObj.
 *
 * Results:
 *	A newly created string object is returned that has ref count zero.
 *
 * Side effects:
 *	The new object's internal string representation will be set to a copy
 *	of the length bytes starting at "bytes". If "length" is negative, use
 *	bytes up to the first NUL byte; i.e., assume "bytes" points to a
 *	C-style NUL-terminated string. The object's type is set to NULL. An
 *	extra NUL is added to the end of the new object's byte array.
 *
 *----------------------------------------------------------------------
 */

#ifdef TCL_MEM_DEBUG
#undef Tcl_NewStringObj
Tcl_Obj *
Tcl_NewStringObj(
    const char *bytes,		/* Points to the first of the length bytes
				 * used to initialize the new object. */
    size_t length)			/* The number of bytes to copy from "bytes"
				 * when initializing the new object. If
				 * negative, use bytes up to the first NUL
				 * byte. */
{
    return Tcl_DbNewStringObj(bytes, length, "unknown", 0);
}
#else /* if not TCL_MEM_DEBUG */
Tcl_Obj *
Tcl_NewStringObj(
    const char *bytes,		/* Points to the first of the length bytes
				 * used to initialize the new object. */
    size_t length)		/* The number of bytes to copy from "bytes"
				 * when initializing the new object. If -1,
				 * use bytes up to the first NUL byte. */
{
    Tcl_Obj *objPtr;

    if (length == TCL_INDEX_NONE) {
	length = (bytes? strlen(bytes) : 0);
    }
    TclNewStringObj(objPtr, bytes, length);
    return objPtr;
}
#endif /* TCL_MEM_DEBUG */

/*
 *----------------------------------------------------------------------
 *
 * Tcl_DbNewStringObj --
 *
 *	This function is normally called when debugging: i.e., when
 *	TCL_MEM_DEBUG is defined. It creates new string objects. It is the
 *	same as the Tcl_NewStringObj function above except that it calls
 *	Tcl_DbCkalloc directly with the file name and line number from its
 *	caller. This simplifies debugging since then the [memory active]
 *	command will report the correct file name and line number when
 *	reporting objects that haven't been freed.
 *
 *	When TCL_MEM_DEBUG is not defined, this function just returns the
 *	result of calling Tcl_NewStringObj.
 *
 * Results:
 *	A newly created string object is returned that has ref count zero.
 *
 * Side effects:
 *	The new object's internal string representation will be set to a copy
 *	of the length bytes starting at "bytes". If "length" is negative, use
 *	bytes up to the first NUL byte; i.e., assume "bytes" points to a
 *	C-style NUL-terminated string. The object's type is set to NULL. An
 *	extra NUL is added to the end of the new object's byte array.
 *
 *----------------------------------------------------------------------
 */

#ifdef TCL_MEM_DEBUG
Tcl_Obj *
Tcl_DbNewStringObj(
    const char *bytes,		/* Points to the first of the length bytes
				 * used to initialize the new object. */
    size_t length,		/* The number of bytes to copy from "bytes"
				 * when initializing the new object. If -1,
				 * use bytes up to the first NUL byte. */
    const char *file,		/* The name of the source file calling this
				 * function; used for debugging. */
    int line)			/* Line number in the source file; used for
				 * debugging. */
{
    Tcl_Obj *objPtr;

    if (length == TCL_INDEX_NONE) {
	length = (bytes? strlen(bytes) : 0);
    }
    TclDbNewObj(objPtr, file, line);
    TclInitStringRep(objPtr, bytes, length);
    return objPtr;
}
#else /* if not TCL_MEM_DEBUG */
Tcl_Obj *
Tcl_DbNewStringObj(
    const char *bytes,		/* Points to the first of the length bytes
				 * used to initialize the new object. */
    size_t length,		/* The number of bytes to copy from "bytes"
				 * when initializing the new object. If -1,
				 * use bytes up to the first NUL byte. */
    TCL_UNUSED(const char *) /*file*/,
    TCL_UNUSED(int) /*line*/)
{
    return Tcl_NewStringObj(bytes, length);
}
#endif /* TCL_MEM_DEBUG */

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_NewUnicodeObj --
 *
 *	This function is creates a new String object and initializes it from
 *	the given Unicode String. If the Utf String is the same size as the
 *	Unicode string, don't duplicate the data.
 *
 * Results:
 *	The newly created object is returned. This object will have no initial
 *	string representation. The returned object has a ref count of 0.
 *
 * Side effects:
 *	Memory allocated for new object and copy of Unicode argument.
 *
 *---------------------------------------------------------------------------
 */

Tcl_Obj *
Tcl_NewUnicodeObj(
    const Tcl_UniChar *unicode,	/* The unicode string used to initialize the
				 * new object. */
    size_t numChars)		/* Number of characters in the unicode
				 * string. */
{
    Tcl_Obj *objPtr;

    TclNewObj(objPtr);
    SetUnicodeObj(objPtr, unicode, numChars);
    return objPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetCharLength --
 *
 *	Get the length of the Unicode string from the Tcl object.
 *
 * Results:
 *	Pointer to unicode string representing the unicode object.
 *
 * Side effects:
 *	Frees old internal rep. Allocates memory for new "String" internal
 *	rep.
 *
 *----------------------------------------------------------------------
 */

size_t
Tcl_GetCharLength(
    Tcl_Obj *objPtr)		/* The String object to get the num chars
				 * of. */
{
    String *stringPtr;
    size_t numChars = 0;

    /*
     * Quick, no-shimmer return for short string reps.
     */

    if ((objPtr->bytes) && (objPtr->length < 2)) {
	/* 0 bytes -> 0 chars; 1 byte -> 1 char */
	return objPtr->length;
    }

    /*
     * Optimize the case where we're really dealing with a bytearray object;
     * we don't need to convert to a string to perform the get-length operation.
     *
     * Starting in Tcl 8.7, we check for a "pure" bytearray, because the
     * machinery behind that test is using a proper bytearray ObjType.  We
     * could also compute length of an improper bytearray without shimmering
     * but there's no value in that. We *want* to shimmer an improper bytearray
     * because improper bytearrays have worthless internal reps.
     */

    if (TclIsPureByteArray(objPtr)) {
	(void) Tcl_GetByteArrayFromObj(objPtr, &numChars);
	return numChars;
    }

    /*
     * OK, need to work with the object as a string.
     */

    SetStringFromAny(NULL, objPtr);
    stringPtr = GET_STRING(objPtr);
    numChars = stringPtr->numChars;

    /*
     * If numChars is unknown, compute it.
     */

    if (numChars == TCL_INDEX_NONE) {
	TclNumUtfChars(numChars, objPtr->bytes, objPtr->length);
	stringPtr->numChars = numChars;
    }
    return numChars;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCheckEmptyString --
 *
 *	Determine whether the string value of an object is or would be the
 *	empty string, without generating a string representation.
 *
 * Results:
 *	Returns 1 if empty, 0 if not, and -1 if unknown.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
int
TclCheckEmptyString(
    Tcl_Obj *objPtr)
{
    int length = -1;

    if (objPtr->bytes == &tclEmptyString) {
	return TCL_EMPTYSTRING_YES;
    }

    if (TclListObjIsCanonical(objPtr)) {
	Tcl_ListObjLength(NULL, objPtr, &length);
	return length == 0;
    }

    if (TclIsPureDict(objPtr)) {
	Tcl_DictObjSize(NULL, objPtr, &length);
	return length == 0;
    }

    if (objPtr->bytes == NULL) {
	return TCL_EMPTYSTRING_UNKNOWN;
    }
    return objPtr->length == 0;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetUniChar --
 *
 *	Get the index'th Unicode character from the String object. If index
 *	is out of range or it references a low surrogate preceded by a high
 *	surrogate, the result = -1;
 *
 * Results:
 *	Returns the index'th Unicode character in the Object.
 *
 * Side effects:
 *	Fills unichar with the index'th Unicode character.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_GetUniChar(
    Tcl_Obj *objPtr,		/* The object to get the Unicode charater
				 * from. */
    size_t index)		/* Get the index'th Unicode character. */
{
    String *stringPtr;
    int ch;

    /*
     * Optimize the case where we're really dealing with a bytearray object
     * we don't need to convert to a string to perform the indexing operation.
     */

    if (TclIsPureByteArray(objPtr)) {
	size_t length = 0;
	unsigned char *bytes = Tcl_GetByteArrayFromObj(objPtr, &length);
	if (index >= length) {
		return -1;
	}

	return bytes[index];
    }

    /*
     * OK, need to work with the object as a string.
     */

    SetStringFromAny(NULL, objPtr);
    stringPtr = GET_STRING(objPtr);

    if (stringPtr->hasUnicode == 0) {
	/*
	 * If numChars is unknown, compute it.
	 */

	if (stringPtr->numChars == TCL_INDEX_NONE) {
	    TclNumUtfChars(stringPtr->numChars, objPtr->bytes, objPtr->length);
	}
	if (stringPtr->numChars == objPtr->length) {
	    return (Tcl_UniChar) objPtr->bytes[index];
	}
	FillUnicodeRep(objPtr);
	stringPtr = GET_STRING(objPtr);
    }

    if (index >= stringPtr->numChars) {
	return -1;
    }
    ch = stringPtr->unicode[index];
#if TCL_UTF_MAX <= 3
    /* See: bug [11ae2be95dac9417] */
    if ((ch & 0xF800) == 0xD800) {
	if (ch & 0x400) {
	    if ((index > 0)
		    && ((stringPtr->unicode[index-1] & 0xFC00) == 0xD800)) {
		ch = -1; /* low surrogate preceded by high surrogate */
	    }
	} else if ((++index < stringPtr->numChars)
		&& ((stringPtr->unicode[index] & 0xFC00) == 0xDC00)) {
	    /* high surrogate followed by low surrogate */
	    ch = (((ch & 0x3FF) << 10) |
			(stringPtr->unicode[index] & 0x3FF)) + 0x10000;
	}
    }
#endif
    return ch;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetUnicodeFromObj/TclGetUnicodeFromObj --
 *
 *	Get the Unicode form of the String object with length. If the object
 *	is not already a String object, it will be converted to one. If the
 *	String object does not have a Unicode rep, then one is create from the
 *	UTF string format.
 *
 * Results:
 *	Returns a pointer to the object's internal Unicode string.
 *
 * Side effects:
 *	Converts the object to have the String internal rep.
 *
 *----------------------------------------------------------------------
 */

#undef Tcl_GetUnicodeFromObj
Tcl_UniChar *
TclGetUnicodeFromObj(
    Tcl_Obj *objPtr,		/* The object to find the unicode string
				 * for. */
    int *lengthPtr)		/* If non-NULL, the location where the string
				 * rep's unichar length should be stored. If
				 * NULL, no length is stored. */
{
    String *stringPtr;

    SetStringFromAny(NULL, objPtr);
    stringPtr = GET_STRING(objPtr);

    if (stringPtr->hasUnicode == 0) {
	FillUnicodeRep(objPtr);
	stringPtr = GET_STRING(objPtr);
    }

    if (lengthPtr != NULL) {
	*lengthPtr = stringPtr->numChars;
    }
    return stringPtr->unicode;
}

Tcl_UniChar *
Tcl_GetUnicodeFromObj(
    Tcl_Obj *objPtr,		/* The object to find the unicode string
				 * for. */
    size_t *lengthPtr)		/* If non-NULL, the location where the string
				 * rep's unichar length should be stored. If
				 * NULL, no length is stored. */
{
    String *stringPtr;

    SetStringFromAny(NULL, objPtr);
    stringPtr = GET_STRING(objPtr);

    if (stringPtr->hasUnicode == 0) {
	FillUnicodeRep(objPtr);
	stringPtr = GET_STRING(objPtr);
    }

    if (lengthPtr != NULL) {
	*lengthPtr = stringPtr->numChars;
    }
    return stringPtr->unicode;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetRange --
 *
 *	Create a Tcl Object that contains the chars between first and last of
 *	the object indicated by "objPtr". If the object is not already a
 *	String object, convert it to one. The first and last indices are
 *	assumed to be in the appropriate range.
 *
 * Results:
 *	Returns a new Tcl Object of the String type.
 *
 * Side effects:
 *	Changes the internal rep of "objPtr" to the String type.
 *
 *----------------------------------------------------------------------
 */

Tcl_Obj *
Tcl_GetRange(
    Tcl_Obj *objPtr,		/* The Tcl object to find the range of. */
    size_t first,			/* First index of the range. */
    size_t last)			/* Last index of the range. */
{
    Tcl_Obj *newObjPtr;		/* The Tcl object to find the range of. */
    String *stringPtr;
    size_t length = 0;

    if (first == TCL_INDEX_NONE) {
	first = TCL_INDEX_START;
    }
    if (last + 2 <= first + 1) {
	return Tcl_NewObj();
    }

    /*
     * Optimize the case where we're really dealing with a bytearray object
     * we don't need to convert to a string to perform the substring operation.
     */

    if (TclIsPureByteArray(objPtr)) {
	unsigned char *bytes = Tcl_GetByteArrayFromObj(objPtr, &length);

	if (last >= length) {
	    last = length - 1;
	}
	if (last < first) {
	    return Tcl_NewObj();
	}
	return Tcl_NewByteArrayObj(bytes + first, last - first + 1);
    }

    /*
     * OK, need to work with the object as a string.
     */

    SetStringFromAny(NULL, objPtr);
    stringPtr = GET_STRING(objPtr);

    if (stringPtr->hasUnicode == 0) {
	/*
	 * If numChars is unknown, compute it.
	 */

	if (stringPtr->numChars == TCL_INDEX_NONE) {
	    TclNumUtfChars(stringPtr->numChars, objPtr->bytes, objPtr->length);
	}
	if (stringPtr->numChars == objPtr->length) {
	    if (last >= stringPtr->numChars) {
		last = stringPtr->numChars - 1;
	    }
	    if (last < first) {
		return Tcl_NewObj();
	    }
	    newObjPtr = Tcl_NewStringObj(objPtr->bytes + first, last-first+1);

	    /*
	     * Since we know the char length of the result, store it.
	     */

	    SetStringFromAny(NULL, newObjPtr);
	    stringPtr = GET_STRING(newObjPtr);
	    stringPtr->numChars = newObjPtr->length;
	    return newObjPtr;
	}
	FillUnicodeRep(objPtr);
	stringPtr = GET_STRING(objPtr);
    }
    if (last > stringPtr->numChars) {
	last = stringPtr->numChars;
    }
    if (last < first) {
	return Tcl_NewObj();
    }
#if TCL_UTF_MAX <= 3
    /* See: bug [11ae2be95dac9417] */
    if ((first + 1 > 1) && ((stringPtr->unicode[first] & 0xFC00) == 0xDC00)
	    && ((stringPtr->unicode[first-1] & 0xFC00) == 0xD800)) {
	++first;
    }
    if ((last + 2 < stringPtr->numChars + 1)
	    && ((stringPtr->unicode[last+1] & 0xFC00) == 0xDC00)
	    && ((stringPtr->unicode[last] & 0xFC00) == 0xD800)) {
	++last;
    }
#endif
    return Tcl_NewUnicodeObj(stringPtr->unicode + first, last - first + 1);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_SetStringObj --
 *
 *	Modify an object to hold a string that is a copy of the bytes
 *	indicated by the byte pointer and length arguments.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The object's string representation will be set to a copy of the
 *	"length" bytes starting at "bytes". If "length" is negative, use bytes
 *	up to the first NUL byte; i.e., assume "bytes" points to a C-style
 *	NUL-terminated string. The object's old string and internal
 *	representations are freed and the object's type is set NULL.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_SetStringObj(
    Tcl_Obj *objPtr,		/* Object whose internal rep to init. */
    const char *bytes,		/* Points to the first of the length bytes
				 * used to initialize the object. */
    size_t length)		/* The number of bytes to copy from "bytes"
				 * when initializing the object. If -1,
				 * use bytes up to the first NUL byte.*/
{
    if (Tcl_IsShared(objPtr)) {
	Tcl_Panic("%s called with shared object", "Tcl_SetStringObj");
    }

    /*
     * Set the type to NULL and free any internal rep for the old type.
     */

    TclFreeInternalRep(objPtr);

    /*
     * Free any old string rep, then set the string rep to a copy of the
     * length bytes starting at "bytes".
     */

    TclInvalidateStringRep(objPtr);
    if (length == TCL_INDEX_NONE) {
	length = (bytes? strlen(bytes) : 0);
    }
    TclInitStringRep(objPtr, bytes, length);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_SetObjLength --
 *
 *	This function changes the length of the string representation of an
 *	object.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	If the size of objPtr's string representation is greater than length,
 *	then it is reduced to length and a new terminating null byte is stored
 *	in the strength. If the length of the string representation is greater
 *	than length, the storage space is reallocated to the given length; a
 *	null byte is stored at the end, but other bytes past the end of the
 *	original string representation are undefined. The object's internal
 *	representation is changed to "expendable string".
 *
 *----------------------------------------------------------------------
 */

void
Tcl_SetObjLength(
    Tcl_Obj *objPtr,		/* Pointer to object. This object must not
				 * currently be shared. */
    size_t length)		/* Number of bytes desired for string
				 * representation of object, not including
				 * terminating null byte. */
{
    String *stringPtr;

    if (Tcl_IsShared(objPtr)) {
	Tcl_Panic("%s called with shared object", "Tcl_SetObjLength");
    }

    if (objPtr->bytes && objPtr->length == length) {
	return;
    }

    SetStringFromAny(NULL, objPtr);
    stringPtr = GET_STRING(objPtr);

    if (objPtr->bytes != NULL) {
	/*
	 * Change length of an existing string rep.
	 */
	if (length > stringPtr->allocated) {
	    /*
	     * Need to enlarge the buffer.
	     */
	    if (objPtr->bytes == &tclEmptyString) {
		objPtr->bytes = (char *)Tcl_Alloc(length + 1);
	    } else {
		objPtr->bytes = (char *)Tcl_Realloc(objPtr->bytes, length + 1);
	    }
	    stringPtr->allocated = length;
	}

	objPtr->length = length;
	objPtr->bytes[length] = 0;

	/*
	 * Invalidate the unicode data.
	 */

	stringPtr->numChars = TCL_INDEX_NONE;
	stringPtr->hasUnicode = 0;
    } else {
	if (length > stringPtr->maxChars) {
	    stringPtr = stringRealloc(stringPtr, length);
	    SET_STRING(objPtr, stringPtr);
	    stringPtr->maxChars = length;
	}

	/*
	 * Mark the new end of the unicode string
	 */

	stringPtr->numChars = length;
	stringPtr->unicode[length] = 0;
	stringPtr->hasUnicode = 1;

	/*
	 * Can only get here when objPtr->bytes == NULL. No need to invalidate
	 * the string rep.
	 */
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_AttemptSetObjLength --
 *
 *	This function changes the length of the string representation of an
 *	object. It uses the attempt* (non-panic'ing) memory allocators.
 *
 * Results:
 *	1 if the requested memory was allocated, 0 otherwise.
 *
 * Side effects:
 *	If the size of objPtr's string representation is greater than length,
 *	then it is reduced to length and a new terminating null byte is stored
 *	in the strength. If the length of the string representation is greater
 *	than length, the storage space is reallocated to the given length; a
 *	null byte is stored at the end, but other bytes past the end of the
 *	original string representation are undefined. The object's internal
 *	representation is changed to "expendable string".
 *
 *----------------------------------------------------------------------
 */

int
Tcl_AttemptSetObjLength(
    Tcl_Obj *objPtr,		/* Pointer to object. This object must not
				 * currently be shared. */
    size_t length)		/* Number of bytes desired for string
				 * representation of object, not including
				 * terminating null byte. */
{
    String *stringPtr;

    if (Tcl_IsShared(objPtr)) {
	Tcl_Panic("%s called with shared object", "Tcl_AttemptSetObjLength");
    }
    if (objPtr->bytes && objPtr->length == length) {
	return 1;
    }

    SetStringFromAny(NULL, objPtr);
    stringPtr = GET_STRING(objPtr);

    if (objPtr->bytes != NULL) {
	/*
	 * Change length of an existing string rep.
	 */
	if (length > stringPtr->allocated) {
	    /*
	     * Need to enlarge the buffer.
	     */

	    char *newBytes;

	    if (objPtr->bytes == &tclEmptyString) {
		newBytes = (char *)Tcl_AttemptAlloc(length + 1);
	    } else {
		newBytes = (char *)Tcl_AttemptRealloc(objPtr->bytes, length + 1);
	    }
	    if (newBytes == NULL) {
		return 0;
	    }
	    objPtr->bytes = newBytes;
	    stringPtr->allocated = length;
	}

	objPtr->length = length;
	objPtr->bytes[length] = 0;

	/*
	 * Invalidate the unicode data.
	 */

	stringPtr->numChars = TCL_INDEX_NONE;
	stringPtr->hasUnicode = 0;
    } else {
	/*
	 * Changing length of pure unicode string.
	 */

	if (length > stringPtr->maxChars) {
	    stringPtr = stringAttemptRealloc(stringPtr, length);
	    if (stringPtr == NULL) {
		return 0;
	    }
	    SET_STRING(objPtr, stringPtr);
	    stringPtr->maxChars = length;
	}

	/*
	 * Mark the new end of the unicode string.
	 */

	stringPtr->unicode[length] = 0;
	stringPtr->numChars = length;
	stringPtr->hasUnicode = 1;

	/*
	 * Can only get here when objPtr->bytes == NULL. No need to invalidate
	 * the string rep.
	 */
    }
    return 1;
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_SetUnicodeObj --
 *
 *	Modify an object to hold the Unicode string indicated by "unicode".
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Memory allocated for new "String" internal rep.
 *
 *---------------------------------------------------------------------------
 */

void
Tcl_SetUnicodeObj(
    Tcl_Obj *objPtr,		/* The object to set the string of. */
    const Tcl_UniChar *unicode,	/* The unicode string used to initialize the
				 * object. */
    size_t numChars)		/* Number of characters in the unicode
				 * string. */
{
    if (Tcl_IsShared(objPtr)) {
	Tcl_Panic("%s called with shared object", "Tcl_SetUnicodeObj");
    }
    TclFreeInternalRep(objPtr);
    SetUnicodeObj(objPtr, unicode, numChars);
}

static size_t
UnicodeLength(
    const Tcl_UniChar *unicode)
{
    size_t numChars = 0;

    if (unicode) {
	while ((numChars != TCL_INDEX_NONE) && (unicode[numChars] != 0)) {
	    numChars++;
	}
    }
    return numChars;
}

static void
SetUnicodeObj(
    Tcl_Obj *objPtr,		/* The object to set the string of. */
    const Tcl_UniChar *unicode,	/* The unicode string used to initialize the
				 * object. */
    size_t numChars)		/* Number of characters in the unicode
				 * string. */
{
    String *stringPtr;

    if (numChars == TCL_INDEX_NONE) {
	numChars = UnicodeLength(unicode);
    }

    /*
     * Allocate enough space for the String structure + Unicode string.
     */

    stringPtr = stringAlloc(numChars);
    SET_STRING(objPtr, stringPtr);
    objPtr->typePtr = &tclStringType;

    stringPtr->maxChars = numChars;
    memcpy(stringPtr->unicode, unicode, numChars * sizeof(Tcl_UniChar));
    stringPtr->unicode[numChars] = 0;
    stringPtr->numChars = numChars;
    stringPtr->hasUnicode = 1;

    TclInvalidateStringRep(objPtr);
    stringPtr->allocated = 0;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_AppendLimitedToObj --
 *
 *	This function appends a limited number of bytes from a sequence of
 *	bytes to an object, marking any limitation with an ellipsis.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The bytes at *bytes are appended to the string representation of
 *	objPtr.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_AppendLimitedToObj(
    Tcl_Obj *objPtr,		/* Points to the object to append to. */
    const char *bytes,		/* Points to the bytes to append to the
				 * object. */
    size_t length,		/* The number of bytes available to be
				 * appended from "bytes". If -1, then
				 * all bytes up to a NUL byte are available. */
    size_t limit,		/* The maximum number of bytes to append to
				 * the object. */
    const char *ellipsis)	/* Ellipsis marker string, appended to the
				 * object to indicate not all available bytes
				 * at "bytes" were appended. */
{
    String *stringPtr;
    size_t toCopy = 0;
    size_t eLen = 0;

    if (length == TCL_INDEX_NONE) {
	length = (bytes ? strlen(bytes) : 0);
    }
    if (length == 0) {
	return;
    }
    if (limit <= 0) {
	return;
    }

    if (length <= limit) {
	toCopy = length;
    } else {
	if (ellipsis == NULL) {
	    ellipsis = "...";
	}
	eLen = strlen(ellipsis);
	while (eLen > limit) {
	    eLen = Tcl_UtfPrev(ellipsis+eLen, ellipsis) - ellipsis;
	}

	toCopy = Tcl_UtfPrev(bytes+limit+1-eLen, bytes) - bytes;
    }

    /*
     * If objPtr has a valid Unicode rep, then append the Unicode conversion
     * of "bytes" to the objPtr's Unicode rep, otherwise append "bytes" to
     * objPtr's string rep.
     */

    if (Tcl_IsShared(objPtr)) {
	Tcl_Panic("%s called with shared object", "Tcl_AppendLimitedToObj");
    }

    SetStringFromAny(NULL, objPtr);
    stringPtr = GET_STRING(objPtr);

    /* If appended string starts with a continuation byte or a lower surrogate,
     * force objPtr to unicode representation. See [7f1162a867] */
    if (bytes && ISCONTINUATION(bytes)) {
	Tcl_GetUnicode(objPtr);
	stringPtr = GET_STRING(objPtr);
    }
    if (stringPtr->hasUnicode && (stringPtr->numChars+1) > 1) {
	AppendUtfToUnicodeRep(objPtr, bytes, toCopy);
    } else {
	AppendUtfToUtfRep(objPtr, bytes, toCopy);
    }

    if (length <= limit) {
	return;
    }

    stringPtr = GET_STRING(objPtr);
    if (stringPtr->hasUnicode && (stringPtr->numChars+1) > 1) {
	AppendUtfToUnicodeRep(objPtr, ellipsis, eLen);
    } else {
	AppendUtfToUtfRep(objPtr, ellipsis, eLen);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_AppendToObj --
 *
 *	This function appends a sequence of bytes to an object.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The bytes at *bytes are appended to the string representation of
 *	objPtr.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_AppendToObj(
    Tcl_Obj *objPtr,		/* Points to the object to append to. */
    const char *bytes,		/* Points to the bytes to append to the
				 * object. */
    size_t length)		/* The number of bytes to append from "bytes".
				 * If TCL_INDEX_NONE, then append all bytes up to NUL
				 * byte. */
{
    Tcl_AppendLimitedToObj(objPtr, bytes, length, TCL_INDEX_NONE, NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * TclAppendUnicodeToObj --
 *
 *	This function appends a Unicode string to an object in the most
 *	efficient manner possible. Length must be >= 0.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Invalidates the string rep and creates a new Unicode string.
 *
 *----------------------------------------------------------------------
 */

void
TclAppendUnicodeToObj(
    Tcl_Obj *objPtr,		/* Points to the object to append to. */
    const Tcl_UniChar *unicode,	/* The unicode string to append to the
				 * object. */
    size_t length)		/* Number of chars in "unicode". */
{
    String *stringPtr;

    if (Tcl_IsShared(objPtr)) {
	Tcl_Panic("%s called with shared object", "TclAppendUnicodeToObj");
    }

    if (length == 0) {
	return;
    }

    SetStringFromAny(NULL, objPtr);
    stringPtr = GET_STRING(objPtr);

    /*
     * If objPtr has a valid Unicode rep, then append the "unicode" to the
     * objPtr's Unicode rep, otherwise the UTF conversion of "unicode" to
     * objPtr's string rep.
     */

    if (stringPtr->hasUnicode) {
	AppendUnicodeToUnicodeRep(objPtr, unicode, length);
    } else {
	AppendUnicodeToUtfRep(objPtr, unicode, length);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_AppendObjToObj --
 *
 *	This function appends the string rep of one object to another.
 *	"objPtr" cannot be a shared object.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The string rep of appendObjPtr is appended to the string
 *	representation of objPtr.
 *	IMPORTANT: This routine does not and MUST NOT shimmer appendObjPtr.
 *	Callers are counting on that.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_AppendObjToObj(
    Tcl_Obj *objPtr,		/* Points to the object to append to. */
    Tcl_Obj *appendObjPtr)	/* Object to append. */
{
    String *stringPtr;
    size_t length = 0, numChars;
    size_t appendNumChars = TCL_INDEX_NONE;
    const char *bytes;

    /*
     * Special case: second object is standard-empty is fast case. We know
     * that appending nothing to anything leaves that starting anything...
     */

    if (appendObjPtr->bytes == &tclEmptyString) {
	return;
    }

    /*
     * Handle append of one bytearray object to another as a special case.
     * Note that we only do this when the objects are pure so that the
     * bytearray faithfully represent the true value; Otherwise appending the
     * byte arrays together could lose information;
     */

    if ((TclIsPureByteArray(objPtr) || objPtr->bytes == &tclEmptyString)
	    && TclIsPureByteArray(appendObjPtr)) {
	/*
	 * You might expect the code here to be
	 *
	 *  bytes = Tcl_GetByteArrayFromObj(appendObjPtr, &length);
	 *  TclAppendBytesToByteArray(objPtr, bytes, length);
	 *
	 * and essentially all of the time that would be fine. However, it
	 * would run into trouble in the case where objPtr and appendObjPtr
	 * point to the same thing. That may never be a good idea. It seems to
	 * violate Copy On Write, and we don't have any tests for the
	 * situation, since making any Tcl commands that call
	 * Tcl_AppendObjToObj() do that appears impossible (They honor Copy On
	 * Write!). For the sake of extensions that go off into that realm,
	 * though, here's a more complex approach that can handle all the
	 * cases.
	 *
	 * First, get the lengths.
	 */

	size_t lengthSrc = 0;

	(void) Tcl_GetByteArrayFromObj(objPtr, &length);
	(void) Tcl_GetByteArrayFromObj(appendObjPtr, &lengthSrc);

	/*
	 * Grow buffer enough for the append.
	 */

	TclAppendBytesToByteArray(objPtr, NULL, lengthSrc);

	/*
	 * Reset objPtr back to the original value.
	 */

	Tcl_SetByteArrayLength(objPtr, length);

	/*
	 * Now do the append knowing that buffer growth cannot cause any
	 * trouble.
	 */

	TclAppendBytesToByteArray(objPtr,
		Tcl_GetByteArrayFromObj(appendObjPtr, (size_t *)NULL), lengthSrc);
	return;
    }

    /*
     * Must append as strings.
     */

    SetStringFromAny(NULL, objPtr);
    stringPtr = GET_STRING(objPtr);

    /* If appended string starts with a continuation byte or a lower surrogate,
     * force objPtr to unicode representation. See [7f1162a867]
     * This fixes append-3.4, append-3.7 and utf-1.18 testcases. */
    if (ISCONTINUATION(TclGetString(appendObjPtr))) {
	Tcl_GetUnicode(objPtr);
	stringPtr = GET_STRING(objPtr);
    }
    /*
     * If objPtr has a valid Unicode rep, then get a Unicode string from
     * appendObjPtr and append it.
     */

    if (stringPtr->hasUnicode) {
	/*
	 * If appendObjPtr is not of the "String" type, don't convert it.
	 */

	if (TclHasInternalRep(appendObjPtr, &tclStringType)) {
	    Tcl_UniChar *unicode =
		    Tcl_GetUnicodeFromObj(appendObjPtr, &numChars);

	    AppendUnicodeToUnicodeRep(objPtr, unicode, numChars);
	} else {
	    bytes = Tcl_GetStringFromObj(appendObjPtr, &length);
	    AppendUtfToUnicodeRep(objPtr, bytes, length);
	}
	return;
    }

    /*
     * Append to objPtr's UTF string rep. If we know the number of characters
     * in both objects before appending, then set the combined number of
     * characters in the final (appended-to) object.
     */

    bytes = Tcl_GetStringFromObj(appendObjPtr, &length);

    numChars = stringPtr->numChars;
    if ((numChars != TCL_INDEX_NONE) && TclHasInternalRep(appendObjPtr, &tclStringType)) {
	String *appendStringPtr = GET_STRING(appendObjPtr);

	appendNumChars = appendStringPtr->numChars;
    }

    AppendUtfToUtfRep(objPtr, bytes, length);

    if ((numChars != TCL_INDEX_NONE) && (appendNumChars != TCL_INDEX_NONE)) {
	stringPtr->numChars = numChars + appendNumChars;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * AppendUnicodeToUnicodeRep --
 *
 *	This function appends the contents of "unicode" to the Unicode rep of
 *	"objPtr". objPtr must already have a valid Unicode rep.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	objPtr's internal rep is reallocated.
 *
 *----------------------------------------------------------------------
 */

static void
AppendUnicodeToUnicodeRep(
    Tcl_Obj *objPtr,		/* Points to the object to append to. */
    const Tcl_UniChar *unicode,	/* String to append. */
    size_t appendNumChars)		/* Number of chars of "unicode" to append. */
{
    String *stringPtr;
    size_t numChars;

    if (appendNumChars == TCL_INDEX_NONE) {
	appendNumChars = UnicodeLength(unicode);
    }
    if (appendNumChars == 0) {
	return;
    }

    SetStringFromAny(NULL, objPtr);
    stringPtr = GET_STRING(objPtr);

    /*
     * If not enough space has been allocated for the unicode rep, reallocate
     * the internal rep object with additional space. First try to double the
     * required allocation; if that fails, try a more modest increase. See the
     * "TCL STRING GROWTH ALGORITHM" comment at the top of this file for an
     * explanation of this growth algorithm.
     */

    numChars = stringPtr->numChars + appendNumChars;

    if (numChars > stringPtr->maxChars) {
	size_t index = TCL_INDEX_NONE;

	/*
	 * Protect against case where unicode points into the existing
	 * stringPtr->unicode array. Force it to follow any relocations due to
	 * the reallocs below.
	 */

	if (unicode && unicode >= stringPtr->unicode
		&& unicode <= stringPtr->unicode + stringPtr->maxChars) {
	    index = unicode - stringPtr->unicode;
	}

	GrowUnicodeBuffer(objPtr, numChars);
	stringPtr = GET_STRING(objPtr);

	/*
	 * Relocate unicode if needed; see above.
	 */

	if (index != TCL_INDEX_NONE) {
	    unicode = stringPtr->unicode + index;
	}
    }

    /*
     * Copy the new string onto the end of the old string, then add the
     * trailing null.
     */

    if (unicode) {
	memmove(stringPtr->unicode + stringPtr->numChars, unicode,
		appendNumChars * sizeof(Tcl_UniChar));
    }
    stringPtr->unicode[numChars] = 0;
    stringPtr->numChars = numChars;
    stringPtr->allocated = 0;

    TclInvalidateStringRep(objPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * AppendUnicodeToUtfRep --
 *
 *	This function converts the contents of "unicode" to UTF and appends
 *	the UTF to the string rep of "objPtr".
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	objPtr's internal rep is reallocated.
 *
 *----------------------------------------------------------------------
 */

static void
AppendUnicodeToUtfRep(
    Tcl_Obj *objPtr,		/* Points to the object to append to. */
    const Tcl_UniChar *unicode,	/* String to convert to UTF. */
    size_t numChars)		/* Number of chars of "unicode" to convert. */
{
    String *stringPtr = GET_STRING(objPtr);

    numChars = ExtendStringRepWithUnicode(objPtr, unicode, numChars);

    if (stringPtr->numChars != TCL_INDEX_NONE) {
	stringPtr->numChars += numChars;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * AppendUtfToUnicodeRep --
 *
 *	This function converts the contents of "bytes" to Unicode and appends
 *	the Unicode to the Unicode rep of "objPtr". objPtr must already have a
 *	valid Unicode rep. numBytes must be non-negative.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	objPtr's internal rep is reallocated.
 *
 *----------------------------------------------------------------------
 */

static void
AppendUtfToUnicodeRep(
    Tcl_Obj *objPtr,		/* Points to the object to append to. */
    const char *bytes,		/* String to convert to Unicode. */
    size_t numBytes)		/* Number of bytes of "bytes" to convert. */
{
    String *stringPtr;

    if (numBytes == 0) {
	return;
    }

    ExtendUnicodeRepWithString(objPtr, bytes, numBytes, -1);
    TclInvalidateStringRep(objPtr);
    stringPtr = GET_STRING(objPtr);
    stringPtr->allocated = 0;
}

/*
 *----------------------------------------------------------------------
 *
 * AppendUtfToUtfRep --
 *
 *	This function appends "numBytes" bytes of "bytes" to the UTF string
 *	rep of "objPtr". objPtr must already have a valid String rep.
 *	numBytes must be non-negative.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	objPtr's internal rep is reallocated.
 *
 *----------------------------------------------------------------------
 */

static void
AppendUtfToUtfRep(
    Tcl_Obj *objPtr,		/* Points to the object to append to. */
    const char *bytes,		/* String to append. */
    size_t numBytes)		/* Number of bytes of "bytes" to append. */
{
    String *stringPtr;
    size_t newLength, oldLength;

    if (numBytes == 0) {
	return;
    }

    /*
     * Copy the new string onto the end of the old string, then add the
     * trailing null.
     */

    if (objPtr->bytes == NULL) {
	objPtr->length = 0;
    }
    oldLength = objPtr->length;
    newLength = numBytes + oldLength;

    stringPtr = GET_STRING(objPtr);
    if (newLength > stringPtr->allocated) {
	size_t offset = TCL_INDEX_NONE;

	/*
	 * Protect against case where unicode points into the existing
	 * stringPtr->unicode array. Force it to follow any relocations due to
	 * the reallocs below.
	 */

	if (bytes && bytes >= objPtr->bytes
		&& bytes <= objPtr->bytes + objPtr->length) {
	    offset = bytes - objPtr->bytes;
	}

	/*
	 * TODO: consider passing flag=1: no overalloc on first append. This
	 * would make test stringObj-8.1 fail.
	 */

	GrowStringBuffer(objPtr, newLength, 0);

	/*
	 * Relocate bytes if needed; see above.
	 */

	if (offset != TCL_INDEX_NONE) {
	    bytes = objPtr->bytes + offset;
	}
    }

    /*
     * Invalidate the unicode data.
     */

    stringPtr->numChars = TCL_INDEX_NONE;
    stringPtr->hasUnicode = 0;

    if (bytes) {
	memmove(objPtr->bytes + oldLength, bytes, numBytes);
    }
    objPtr->bytes[newLength] = 0;
    objPtr->length = newLength;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_AppendStringsToObj --
 *
 *	This function appends one or more null-terminated strings to an
 *	object.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The contents of all the string arguments are appended to the string
 *	representation of objPtr.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_AppendStringsToObj(
    Tcl_Obj *objPtr,
    ...)
{
    va_list argList;

    va_start(argList, objPtr);
    if (Tcl_IsShared(objPtr)) {
	Tcl_Panic("%s called with shared object", "Tcl_AppendStringsToObj");
    }

    while (1) {
	const char *bytes = va_arg(argList, char *);

	if (bytes == NULL) {
	    break;
	}
	Tcl_AppendToObj(objPtr, bytes, -1);
    }
    va_end(argList);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_AppendFormatToObj --
 *
 *	This function appends a list of Tcl_Obj's to a Tcl_Obj according to
 *	the formatting instructions embedded in the format string. The
 *	formatting instructions are inspired by sprintf(). Returns TCL_OK when
 *	successful. If there's an error in the arguments, TCL_ERROR is
 *	returned, and an error message is written to the interp, if non-NULL.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_AppendFormatToObj(
    Tcl_Interp *interp,
    Tcl_Obj *appendObj,
    const char *format,
    int objc,
    Tcl_Obj *const objv[])
{
    const char *span = format, *msg, *errCode;
    int objIndex = 0, gotXpg = 0, gotSequential = 0;
    size_t originalLength, limit, numBytes = 0;
    Tcl_UniChar ch = 0;
    static const char *mixedXPG =
	    "cannot mix \"%\" and \"%n$\" conversion specifiers";
    static const char *const badIndex[2] = {
	"not enough arguments for all format specifiers",
	"\"%n$\" argument index out of range"
    };
    static const char *overflow = "max size for a Tcl value exceeded";

    if (Tcl_IsShared(appendObj)) {
	Tcl_Panic("%s called with shared object", "Tcl_AppendFormatToObj");
    }
    (void)Tcl_GetStringFromObj(appendObj, &originalLength);
    limit = (size_t)INT_MAX - originalLength;

    /*
     * Format string is NUL-terminated.
     */

    while (*format != '\0') {
	char *end;
	int gotMinus = 0, gotHash = 0, gotZero = 0, gotSpace = 0, gotPlus = 0;
	int width, gotPrecision, precision, sawFlag, useShort = 0, useBig = 0;
#ifndef TCL_WIDE_INT_IS_LONG
	int useWide = 0;
#endif
	int newXpg, numChars, allocSegment = 0, segmentLimit;
	size_t segmentNumBytes;
	Tcl_Obj *segment;
	int step = TclUtfToUniChar(format, &ch);

	format += step;
	if (ch != '%') {
	    numBytes += step;
	    continue;
	}
	if (numBytes) {
	    if (numBytes > limit) {
		msg = overflow;
		errCode = "OVERFLOW";
		goto errorMsg;
	    }
	    Tcl_AppendToObj(appendObj, span, numBytes);
	    limit -= numBytes;
	    numBytes = 0;
	}

	/*
	 * Saw a % : process the format specifier.
	 *
	 * Step 0. Handle special case of escaped format marker (i.e., %%).
	 */

	step = TclUtfToUniChar(format, &ch);
	if (ch == '%') {
	    span = format;
	    numBytes = step;
	    format += step;
	    continue;
	}

	/*
	 * Step 1. XPG3 position specifier
	 */

	newXpg = 0;
	if (isdigit(UCHAR(ch))) {
	    int position = strtoul(format, &end, 10);

	    if (*end == '$') {
		newXpg = 1;
		objIndex = position - 1;
		format = end + 1;
		step = TclUtfToUniChar(format, &ch);
	    }
	}
	if (newXpg) {
	    if (gotSequential) {
		msg = mixedXPG;
		errCode = "MIXEDSPECTYPES";
		goto errorMsg;
	    }
	    gotXpg = 1;
	} else {
	    if (gotXpg) {
		msg = mixedXPG;
		errCode = "MIXEDSPECTYPES";
		goto errorMsg;
	    }
	    gotSequential = 1;
	}
	if ((objIndex < 0) || (objIndex >= objc)) {
	    msg = badIndex[gotXpg];
	    errCode = gotXpg ? "INDEXRANGE" : "FIELDVARMISMATCH";
	    goto errorMsg;
	}

	/*
	 * Step 2. Set of flags.
	 */

	sawFlag = 1;
	do {
	    switch (ch) {
	    case '-':
		gotMinus = 1;
		break;
	    case '#':
		gotHash = 1;
		break;
	    case '0':
		gotZero = 1;
		break;
	    case ' ':
		gotSpace = 1;
		break;
	    case '+':
		gotPlus = 1;
		break;
	    default:
		sawFlag = 0;
	    }
	    if (sawFlag) {
		format += step;
		step = TclUtfToUniChar(format, &ch);
	    }
	} while (sawFlag);

	/*
	 * Step 3. Minimum field width.
	 */

	width = 0;
	if (isdigit(UCHAR(ch))) {
	    width = strtoul(format, &end, 10);
	    if (width < 0) {
		msg = overflow;
		errCode = "OVERFLOW";
		goto errorMsg;
	    }
	    format = end;
	    step = TclUtfToUniChar(format, &ch);
	} else if (ch == '*') {
	    if (objIndex >= objc - 1) {
		msg = badIndex[gotXpg];
		errCode = gotXpg ? "INDEXRANGE" : "FIELDVARMISMATCH";
		goto errorMsg;
	    }
	    if (TclGetIntFromObj(interp, objv[objIndex], &width) != TCL_OK) {
		goto error;
	    }
	    if (width < 0) {
		width = -width;
		gotMinus = 1;
	    }
	    objIndex++;
	    format += step;
	    step = TclUtfToUniChar(format, &ch);
	}
	if (width > (int) limit) {
	    msg = overflow;
	    errCode = "OVERFLOW";
	    goto errorMsg;
	}

	/*
	 * Step 4. Precision.
	 */

	gotPrecision = precision = 0;
	if (ch == '.') {
	    gotPrecision = 1;
	    format += step;
	    step = TclUtfToUniChar(format, &ch);
	}
	if (isdigit(UCHAR(ch))) {
	    precision = strtoul(format, &end, 10);
	    format = end;
	    step = TclUtfToUniChar(format, &ch);
	} else if (ch == '*') {
	    if (objIndex >= objc - 1) {
		msg = badIndex[gotXpg];
		errCode = gotXpg ? "INDEXRANGE" : "FIELDVARMISMATCH";
		goto errorMsg;
	    }
	    if (TclGetIntFromObj(interp, objv[objIndex], &precision)
		    != TCL_OK) {
		goto error;
	    }

	    /*
	     * TODO: Check this truncation logic.
	     */

	    if (precision < 0) {
		precision = 0;
	    }
	    objIndex++;
	    format += step;
	    step = TclUtfToUniChar(format, &ch);
	}

	/*
	 * Step 5. Length modifier.
	 */

	if (ch == 'h') {
	    useShort = 1;
	    format += step;
	    step = TclUtfToUniChar(format, &ch);
	} else if (ch == 'l') {
	    format += step;
	    step = TclUtfToUniChar(format, &ch);
	    if (ch == 'l') {
		useBig = 1;
		format += step;
		step = TclUtfToUniChar(format, &ch);
#ifndef TCL_WIDE_INT_IS_LONG
	    } else {
		useWide = 1;
#endif
	    }
	} else if (ch == 'I') {
	    if ((format[1] == '6') && (format[2] == '4')) {
		format += (step + 2);
		step = TclUtfToUniChar(format, &ch);
#ifndef TCL_WIDE_INT_IS_LONG
		useWide = 1;
#endif
	    } else if ((format[1] == '3') && (format[2] == '2')) {
		format += (step + 2);
		step = TclUtfToUniChar(format, &ch);
	    } else {
		format += step;
		step = TclUtfToUniChar(format, &ch);
	    }
	} else if ((ch == 't') || (ch == 'z') || (ch == 'q') || (ch == 'j')
		|| (ch == 'L')) {
	    format += step;
	    step = TclUtfToUniChar(format, &ch);
	    useBig = 1;
	}

	format += step;
	span = format;

	/*
	 * Step 6. The actual conversion character.
	 */

	segment = objv[objIndex];
	numChars = -1;
	if (ch == 'i') {
	    ch = 'd';
	}
	switch (ch) {
	case '\0':
	    msg = "format string ended in middle of field specifier";
	    errCode = "INCOMPLETE";
	    goto errorMsg;
	case 's':
	    if (gotPrecision) {
		numChars = Tcl_GetCharLength(segment);
		if (precision < numChars) {
		    segment = Tcl_GetRange(segment, 0, precision - 1);
		    numChars = precision;
		    Tcl_IncrRefCount(segment);
		    allocSegment = 1;
		}
	    }
	    break;
	case 'c': {
	    char buf[4] = "";
	    int code, length;

	    if (TclGetIntFromObj(interp, segment, &code) != TCL_OK) {
		goto error;
	    }
	    length = Tcl_UniCharToUtf(code, buf);
	    if ((code >= 0xD800) && (length < 3)) {
		/* Special case for handling high surrogates. */
		length += Tcl_UniCharToUtf(-1, buf + length);
	    }
	    segment = Tcl_NewStringObj(buf, length);
	    Tcl_IncrRefCount(segment);
	    allocSegment = 1;
	    break;
	}

	case 'u':
	    /* FALLTHRU */
	case 'd':
	case 'o':
	case 'p':
	case 'x':
	case 'X':
	case 'b': {
	    short s = 0;	/* Silence compiler warning; only defined and
				 * used when useShort is true. */
	    long l;
	    Tcl_WideInt w;
	    mp_int big;
	    int toAppend, isNegative = 0;

#ifndef TCL_WIDE_INT_IS_LONG
	    if (ch == 'p') {
		useWide = 1;
	    }
#endif
	    if (useBig) {
		int cmpResult;
		if (Tcl_GetBignumFromObj(interp, segment, &big) != TCL_OK) {
		    goto error;
		}
		cmpResult = mp_cmp_d(&big, 0);
		isNegative = (cmpResult == MP_LT);
		if (cmpResult == MP_EQ) gotHash = 0;
		if (ch == 'u') {
		    if (isNegative) {
			mp_clear(&big);
			msg = "unsigned bignum format is invalid";
			errCode = "BADUNSIGNED";
			goto errorMsg;
		    } else {
			ch = 'd';
		    }
		}
#ifndef TCL_WIDE_INT_IS_LONG
	    } else if (useWide) {
		if (TclGetWideBitsFromObj(interp, segment, &w) != TCL_OK) {
		    goto error;
		}
		isNegative = (w < (Tcl_WideInt) 0);
		if (w == (Tcl_WideInt) 0) gotHash = 0;
#endif
	    } else if (TclGetLongFromObj(NULL, segment, &l) != TCL_OK) {
		if (TclGetWideBitsFromObj(interp, segment, &w) != TCL_OK) {
		    goto error;
		} else {
		    l = (long) w;
		}
		if (useShort) {
		    s = (short) l;
		    isNegative = (s < (short) 0);
		    if (s == (short) 0) gotHash = 0;
		} else {
		    isNegative = (l < (long) 0);
		    if (l == (long) 0) gotHash = 0;
		}
	    } else if (useShort) {
		s = (short) l;
		isNegative = (s < (short) 0);
		if (s == (short) 0) gotHash = 0;
	    } else {
		isNegative = (l < (long) 0);
		if (l == (long) 0) gotHash = 0;
	    }

	    TclNewObj(segment);
	    allocSegment = 1;
	    segmentLimit = INT_MAX;
	    Tcl_IncrRefCount(segment);

	    if ((isNegative || gotPlus || gotSpace) && (useBig || ch=='d')) {
		Tcl_AppendToObj(segment,
			(isNegative ? "-" : gotPlus ? "+" : " "), 1);
		segmentLimit -= 1;
	    }

	    if (gotHash || (ch == 'p')) {
		switch (ch) {
		case 'o':
		    Tcl_AppendToObj(segment, "0o", 2);
		    segmentLimit -= 2;
		    break;
		case 'p':
		case 'x':
		case 'X':
		    Tcl_AppendToObj(segment, "0x", 2);
		    segmentLimit -= 2;
		    break;
		case 'b':
		    Tcl_AppendToObj(segment, "0b", 2);
		    segmentLimit -= 2;
		    break;
		}
	    }

	    switch (ch) {
	    case 'd': {
		size_t length;
		Tcl_Obj *pure;
		const char *bytes;

		if (useShort) {
		    TclNewIntObj(pure, s);
#ifndef TCL_WIDE_INT_IS_LONG
		} else if (useWide) {
		    TclNewIntObj(pure, w);
#endif
		} else if (useBig) {
		    pure = Tcl_NewBignumObj(&big);
		} else {
		    TclNewIntObj(pure, l);
		}
		Tcl_IncrRefCount(pure);
		bytes = Tcl_GetStringFromObj(pure, &length);

		/*
		 * Already did the sign above.
		 */

		if (*bytes == '-') {
		    length--;
		    bytes++;
		}
		toAppend = length;

		/*
		 * Canonical decimal string reps for integers are composed
		 * entirely of one-byte encoded characters, so "length" is the
		 * number of chars.
		 */

		if (gotPrecision) {
		    if (length < (size_t)precision) {
			segmentLimit -= precision - length;
		    }
		    while (length < (size_t)precision) {
			Tcl_AppendToObj(segment, "0", 1);
			length++;
		    }
		    gotZero = 0;
		}
		if (gotZero) {
		    length += Tcl_GetCharLength(segment);
		    if (length < (size_t)width) {
			segmentLimit -= width - length;
		    }
		    while (length < (size_t)width) {
			Tcl_AppendToObj(segment, "0", 1);
			length++;
		    }
		}
		if (toAppend > segmentLimit) {
		    msg = overflow;
		    errCode = "OVERFLOW";
		    goto errorMsg;
		}
		Tcl_AppendToObj(segment, bytes, toAppend);
		Tcl_DecrRefCount(pure);
		break;
	    }

	    case 'u':
	    case 'o':
	    case 'p':
	    case 'x':
	    case 'X':
	    case 'b': {
		Tcl_WideUInt bits = 0;
		Tcl_WideInt numDigits = 0;
		int numBits = 4, base = 16, index = 0, shift = 0;
		size_t length;
		Tcl_Obj *pure;
		char *bytes;

		if (ch == 'u') {
		    base = 10;
		} else if (ch == 'o') {
		    base = 8;
		    numBits = 3;
		} else if (ch == 'b') {
		    base = 2;
		    numBits = 1;
		}
		if (useShort) {
		    unsigned short us = (unsigned short) s;

		    bits = (Tcl_WideUInt) us;
		    while (us) {
			numDigits++;
			us /= base;
		    }
#ifndef TCL_WIDE_INT_IS_LONG
		} else if (useWide) {
		    Tcl_WideUInt uw = (Tcl_WideUInt) w;

		    bits = uw;
		    while (uw) {
			numDigits++;
			uw /= base;
		    }
#endif
		} else if (useBig && !mp_iszero(&big)) {
		    int leftover = (big.used * MP_DIGIT_BIT) % numBits;
		    mp_digit mask = (~(mp_digit)0) << (MP_DIGIT_BIT-leftover);

		    numDigits = 1 +
			    (((Tcl_WideInt) big.used * MP_DIGIT_BIT) / numBits);
		    while ((mask & big.dp[big.used-1]) == 0) {
			numDigits--;
			mask >>= numBits;
		    }
		    if (numDigits > INT_MAX) {
			msg = overflow;
			errCode = "OVERFLOW";
			goto errorMsg;
		    }
		} else if (!useBig) {
		    unsigned long ul = (unsigned long) l;

		    bits = (Tcl_WideUInt) ul;
		    while (ul) {
			numDigits++;
			ul /= base;
		    }
		}

		/*
		 * Need to be sure zero becomes "0", not "".
		 */

		if (numDigits == 0) {
		    numDigits = 1;
		}
		TclNewObj(pure);
		Tcl_SetObjLength(pure, numDigits);
		bytes = TclGetString(pure);
		toAppend = length = numDigits;
		while (numDigits--) {
		    int digitOffset;

		    if (useBig && !mp_iszero(&big)) {
			if (index < big.used && (size_t) shift <
				CHAR_BIT*sizeof(Tcl_WideUInt) - MP_DIGIT_BIT) {
			    bits |= ((Tcl_WideUInt) big.dp[index++]) << shift;
			    shift += MP_DIGIT_BIT;
			}
			shift -= numBits;
		    }
		    digitOffset = bits % base;
		    if (digitOffset > 9) {
			if (ch == 'X') {
			    bytes[numDigits] = 'A' + digitOffset - 10;
			} else {
			    bytes[numDigits] = 'a' + digitOffset - 10;
			}
		    } else {
			bytes[numDigits] = '0' + digitOffset;
		    }
		    bits /= base;
		}
		if (useBig) {
		    mp_clear(&big);
		}
		if (gotPrecision) {
		    if (length < (size_t)precision) {
			segmentLimit -= precision - length;
		    }
		    while (length < (size_t)precision) {
			Tcl_AppendToObj(segment, "0", 1);
			length++;
		    }
		    gotZero = 0;
		}
		if (gotZero) {
		    length += Tcl_GetCharLength(segment);
		    if (length < (size_t)width) {
			segmentLimit -= width - length;
		    }
		    while (length < (size_t)width) {
			Tcl_AppendToObj(segment, "0", 1);
			length++;
		    }
		}
		if (toAppend > segmentLimit) {
		    msg = overflow;
		    errCode = "OVERFLOW";
		    goto errorMsg;
		}
		Tcl_AppendObjToObj(segment, pure);
		Tcl_DecrRefCount(pure);
		break;
	    }

	    }
	    break;
	}

	case 'a':
	case 'A':
	case 'e':
	case 'E':
	case 'f':
	case 'g':
	case 'G': {
#define MAX_FLOAT_SIZE 320
	    char spec[2*TCL_INTEGER_SPACE + 9], *p = spec;
	    double d;
	    int length = MAX_FLOAT_SIZE;
	    char *bytes;

	    if (Tcl_GetDoubleFromObj(interp, segment, &d) != TCL_OK) {
		/* TODO: Figure out ACCEPT_NAN here */
		goto error;
	    }
	    *p++ = '%';
	    if (gotMinus) {
		*p++ = '-';
	    }
	    if (gotHash) {
		*p++ = '#';
	    }
	    if (gotZero) {
		*p++ = '0';
	    }
	    if (gotSpace) {
		*p++ = ' ';
	    }
	    if (gotPlus) {
		*p++ = '+';
	    }
	    if (width) {
		p += sprintf(p, "%d", width);
		if (width > length) {
		    length = width;
		}
	    }
	    if (gotPrecision) {
		*p++ = '.';
		p += sprintf(p, "%d", precision);
		if (precision > INT_MAX - length) {
		    msg = overflow;
		    errCode = "OVERFLOW";
		    goto errorMsg;
		}
		length += precision;
	    }

	    /*
	     * Don't pass length modifiers!
	     */

	    *p++ = (char) ch;
	    *p = '\0';

	    TclNewObj(segment);
	    allocSegment = 1;
	    if (!Tcl_AttemptSetObjLength(segment, length)) {
		msg = overflow;
		errCode = "OVERFLOW";
		goto errorMsg;
	    }
	    bytes = TclGetString(segment);
	    if (!Tcl_AttemptSetObjLength(segment, sprintf(bytes, spec, d))) {
		msg = overflow;
		errCode = "OVERFLOW";
		goto errorMsg;
	    }
	    if (ch == 'A') {
		char *q = TclGetString(segment) + 1;
		*q = 'x';
		q = strchr(q, 'P');
		if (q) *q = 'p';
	    }
	    break;
	}
	default:
	    if (interp != NULL) {
		Tcl_SetObjResult(interp,
			Tcl_ObjPrintf("bad field specifier \"%c\"", ch));
		Tcl_SetErrorCode(interp, "TCL", "FORMAT", "BADTYPE", NULL);
	    }
	    goto error;
	}

	if (width>0 && numChars<0) {
	    numChars = Tcl_GetCharLength(segment);
	}
	if (!gotMinus && width>0) {
	    if (numChars < width) {
		limit -= width - numChars;
	    }
	    while (numChars < width) {
		Tcl_AppendToObj(appendObj, (gotZero ? "0" : " "), 1);
		numChars++;
	    }
	}

	(void)Tcl_GetStringFromObj(segment, &segmentNumBytes);
	if (segmentNumBytes > limit) {
	    if (allocSegment) {
		Tcl_DecrRefCount(segment);
	    }
	    msg = overflow;
	    errCode = "OVERFLOW";
	    goto errorMsg;
	}
	Tcl_AppendObjToObj(appendObj, segment);
	limit -= segmentNumBytes;
	if (allocSegment) {
	    Tcl_DecrRefCount(segment);
	}
	if (width > 0) {
	    if (numChars < width) {
		limit -= width-numChars;
	    }
	    while (numChars < width) {
		Tcl_AppendToObj(appendObj, (gotZero ? "0" : " "), 1);
		numChars++;
	    }
	}

	objIndex += gotSequential;
    }
    if (numBytes) {
	if (numBytes > limit) {
	    msg = overflow;
	    errCode = "OVERFLOW";
	    goto errorMsg;
	}
	Tcl_AppendToObj(appendObj, span, numBytes);
	limit -= numBytes;
	numBytes = 0;
    }

    return TCL_OK;

  errorMsg:
    if (interp != NULL) {
	Tcl_SetObjResult(interp, Tcl_NewStringObj(msg, -1));
	Tcl_SetErrorCode(interp, "TCL", "FORMAT", errCode, NULL);
    }
  error:
    Tcl_SetObjLength(appendObj, originalLength);
    return TCL_ERROR;
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_Format --
 *
 * Results:
 *	A refcount zero Tcl_Obj.
 *
 * Side effects:
 *	None.
 *
 *---------------------------------------------------------------------------
 */

Tcl_Obj *
Tcl_Format(
    Tcl_Interp *interp,
    const char *format,
    int objc,
    Tcl_Obj *const objv[])
{
    int result;
    Tcl_Obj *objPtr;

    TclNewObj(objPtr);
    result = Tcl_AppendFormatToObj(interp, objPtr, format, objc, objv);
    if (result != TCL_OK) {
	Tcl_DecrRefCount(objPtr);
	return NULL;
    }
    return objPtr;
}

/*
 *---------------------------------------------------------------------------
 *
 * AppendPrintfToObjVA --
 *
 * Results:
 *
 * Side effects:
 *
 *---------------------------------------------------------------------------
 */

static void
AppendPrintfToObjVA(
    Tcl_Obj *objPtr,
    const char *format,
    va_list argList)
{
    int code, objc;
    Tcl_Obj **objv, *list;
    const char *p;

    TclNewObj(list);
    p = format;
    Tcl_IncrRefCount(list);
    while (*p != '\0') {
	int size = 0, seekingConversion = 1, gotPrecision = 0;
	int lastNum = -1;

	if (*p++ != '%') {
	    continue;
	}
	if (*p == '%') {
	    p++;
	    continue;
	}
	do {
	    switch (*p) {
	    case '\0':
		seekingConversion = 0;
		break;
	    case 's': {
		const char *q, *end, *bytes = va_arg(argList, char *);
		seekingConversion = 0;

		/*
		 * The buffer to copy characters from starts at bytes and ends
		 * at either the first NUL byte, or after lastNum bytes, when
		 * caller has indicated a limit.
		 */

		end = bytes;
		while ((!gotPrecision || lastNum--) && (*end != '\0')) {
		    end++;
		}

		/*
		 * Within that buffer, we trim both ends if needed so that we
		 * copy only whole characters, and avoid copying any partial
		 * multi-byte characters.
		 */

		q = Tcl_UtfPrev(end, bytes);
		if (!Tcl_UtfCharComplete(q, (end - q))) {
		    end = q;
		}

		q = bytes + 4;
		while ((bytes < end) && (bytes < q)
			&& ((*bytes & 0xC0) == 0x80)) {
		    bytes++;
		}

		Tcl_ListObjAppendElement(NULL, list,
			Tcl_NewStringObj(bytes , (end - bytes)));

		break;
	    }
	    case 'c':
	    case 'i':
	    case 'u':
	    case 'd':
	    case 'o':
	    case 'p':
	    case 'x':
	    case 'X':
		seekingConversion = 0;
		switch (size) {
		case -1:
		case 0:
		    Tcl_ListObjAppendElement(NULL, list, Tcl_NewWideIntObj(
			    va_arg(argList, int)));
		    break;
		case 1:
		    Tcl_ListObjAppendElement(NULL, list, Tcl_NewWideIntObj(
			    va_arg(argList, long)));
		    break;
		case 2:
		    Tcl_ListObjAppendElement(NULL, list, Tcl_NewWideIntObj(
			    va_arg(argList, Tcl_WideInt)));
		    break;
		case 3:
		    Tcl_ListObjAppendElement(NULL, list, Tcl_NewBignumObj(
			    va_arg(argList, mp_int *)));
		    break;
		}
		break;
	    case 'a':
	    case 'A':
	    case 'e':
	    case 'E':
	    case 'f':
	    case 'g':
	    case 'G':
		if (size > 0) {
		Tcl_ListObjAppendElement(NULL, list, Tcl_NewDoubleObj(
			(double)va_arg(argList, long double)));
		} else {
			Tcl_ListObjAppendElement(NULL, list, Tcl_NewDoubleObj(
				va_arg(argList, double)));
		}
		seekingConversion = 0;
		break;
	    case '*':
		lastNum = va_arg(argList, int);
		Tcl_ListObjAppendElement(NULL, list, Tcl_NewWideIntObj(lastNum));
		p++;
		break;
	    case '0': case '1': case '2': case '3': case '4':
	    case '5': case '6': case '7': case '8': case '9': {
		char *end;

		lastNum = strtoul(p, &end, 10);
		p = end;
		break;
	    }
	    case '.':
		gotPrecision = 1;
		p++;
		break;
	    case 'l':
		++size;
		p++;
		break;
	    case 't':
	    case 'z':
		if (sizeof(size_t) == sizeof(Tcl_WideInt)) {
		    size = 2;
		}
		p++;
		break;
	    case 'j':
	    case 'q':
		size = 2;
		p++;
		break;
	    case 'I':
		if (p[1]=='6' && p[2]=='4') {
		    p += 2;
		    size = 2;
		} else if (p[1]=='3' && p[2]=='2') {
		    p += 2;
		} else if (sizeof(size_t) == sizeof(Tcl_WideInt)) {
		    size = 2;
		}
		p++;
		break;
	    case 'L':
		size = 3;
		p++;
		break;
	    case 'h':
		size = -1;
		/* FALLTHRU */
	    default:
		p++;
	    }
	} while (seekingConversion);
    }
    TclListObjGetElements(NULL, list, &objc, &objv);
    code = Tcl_AppendFormatToObj(NULL, objPtr, format, objc, objv);
    if (code != TCL_OK) {
	Tcl_AppendPrintfToObj(objPtr,
		"Unable to format \"%s\" with supplied arguments: %s",
		format, TclGetString(list));
    }
    Tcl_DecrRefCount(list);
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_AppendPrintfToObj --
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	None.
 *
 *---------------------------------------------------------------------------
 */

void
Tcl_AppendPrintfToObj(
    Tcl_Obj *objPtr,
    const char *format,
    ...)
{
    va_list argList;

    va_start(argList, format);
    AppendPrintfToObjVA(objPtr, format, argList);
    va_end(argList);
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_ObjPrintf --
 *
 * Results:
 *	A refcount zero Tcl_Obj.
 *
 * Side effects:
 *	None.
 *
 *---------------------------------------------------------------------------
 */

Tcl_Obj *
Tcl_ObjPrintf(
    const char *format,
    ...)
{
    va_list argList;
    Tcl_Obj *objPtr;

    TclNewObj(objPtr);
    va_start(argList, format);
    AppendPrintfToObjVA(objPtr, format, argList);
    va_end(argList);
    return objPtr;
}

/*
 *---------------------------------------------------------------------------
 *
 * TclGetStringStorage --
 *
 *	Returns the string storage space of a Tcl_Obj.
 *
 * Results:
 *	The pointer value objPtr->bytes is returned and the number of bytes
 *	allocated there is written to *sizePtr (if known).
 *
 * Side effects:
 *	May set objPtr->bytes.
 *
 *---------------------------------------------------------------------------
 */

char *
TclGetStringStorage(
    Tcl_Obj *objPtr,
    size_t *sizePtr)
{
    String *stringPtr;

    if (!TclHasInternalRep(objPtr, &tclStringType) || objPtr->bytes == NULL) {
	return Tcl_GetStringFromObj(objPtr, sizePtr);
    }

    stringPtr = GET_STRING(objPtr);
    *sizePtr = stringPtr->allocated;
    return objPtr->bytes;
}

/*
 *---------------------------------------------------------------------------
 *
 * TclStringRepeat --
 *
 *	Performs the [string repeat] function.
 *
 * Results:
 * 	A (Tcl_Obj *) pointing to the result value, or NULL in case of an
 * 	error.
 *
 * Side effects:
 * 	On error, when interp is not NULL, error information is left in it.
 *
 *---------------------------------------------------------------------------
 */

Tcl_Obj *
TclStringRepeat(
    Tcl_Interp *interp,
    Tcl_Obj *objPtr,
    size_t count,
    int flags)
{
    Tcl_Obj *objResultPtr;
    int inPlace = flags & TCL_STRING_IN_PLACE;
    size_t length = 0, unichar = 0, done = 1;
    int binary = TclIsPureByteArray(objPtr);

    /* assert (count >= 2) */

    /*
     * Analyze to determine what representation result should be.
     * GOALS:	Avoid shimmering & string rep generation.
     * 		Produce pure bytearray when possible.
     * 		Error on overflow.
     */

    if (!binary) {
	if (TclHasInternalRep(objPtr, &tclStringType)) {
	    String *stringPtr = GET_STRING(objPtr);
	    if (stringPtr->hasUnicode) {
		unichar = 1;
	    }
	}
    }

    if (binary) {
	/* Result will be pure byte array. Pre-size it */
	(void)Tcl_GetByteArrayFromObj(objPtr, &length);
    } else if (unichar) {
	/* Result will be pure Tcl_UniChar array. Pre-size it. */
	(void)Tcl_GetUnicodeFromObj(objPtr, &length);
    } else {
	/* Result will be concat of string reps. Pre-size it. */
	(void)Tcl_GetStringFromObj(objPtr, &length);
    }

    if (length == 0) {
	/* Any repeats of empty is empty. */
	return objPtr;
    }

    if (count > INT_MAX/length) {
	if (interp) {
	    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		    "max size for a Tcl value (%d bytes) exceeded", INT_MAX));
	    Tcl_SetErrorCode(interp, "TCL", "MEMORY", NULL);
	}
	return NULL;
    }

    if (binary) {
	/* Efficiently produce a pure byte array result */
	objResultPtr = (!inPlace || Tcl_IsShared(objPtr)) ?
		Tcl_DuplicateObj(objPtr) : objPtr;

	Tcl_SetByteArrayLength(objResultPtr, count*length); /* PANIC? */
	Tcl_SetByteArrayLength(objResultPtr, length);
	while (count - done > done) {
	    Tcl_AppendObjToObj(objResultPtr, objResultPtr);
	    done *= 2;
	}
	TclAppendBytesToByteArray(objResultPtr,
		Tcl_GetByteArrayFromObj(objResultPtr, (size_t *)NULL),
		(count - done) * length);
    } else if (unichar) {
	/*
	 * Efficiently produce a pure Tcl_UniChar array result.
	 */

	if (!inPlace || Tcl_IsShared(objPtr)) {
	    objResultPtr = Tcl_NewUnicodeObj(Tcl_GetUnicode(objPtr), length);
	} else {
	    TclInvalidateStringRep(objPtr);
	    objResultPtr = objPtr;
	}

        if (0 == Tcl_AttemptSetObjLength(objResultPtr, count*length)) {
	    if (interp) {
		Tcl_SetObjResult(interp, Tcl_ObjPrintf(
			"string size overflow: unable to alloc %"
			TCL_Z_MODIFIER "u bytes",
			STRING_SIZE(count*length)));
		Tcl_SetErrorCode(interp, "TCL", "MEMORY", NULL);
	    }
	    return NULL;
	}
	Tcl_SetObjLength(objResultPtr, length);
	while (count - done > done) {
	    Tcl_AppendObjToObj(objResultPtr, objResultPtr);
	    done *= 2;
	}
	TclAppendUnicodeToObj(objResultPtr, Tcl_GetUnicode(objResultPtr),
		(count - done) * length);
    } else {
	/*
	 * Efficiently concatenate string reps.
	 */

	if (!inPlace || Tcl_IsShared(objPtr)) {
	    objResultPtr = Tcl_NewStringObj(TclGetString(objPtr), length);
	} else {
	    TclFreeInternalRep(objPtr);
	    objResultPtr = objPtr;
	}
        if (0 == Tcl_AttemptSetObjLength(objResultPtr, count*length)) {
	    if (interp) {
		Tcl_SetObjResult(interp, Tcl_ObjPrintf(
			"string size overflow: unable to alloc %" TCL_Z_MODIFIER "u bytes",
			count*length));
		Tcl_SetErrorCode(interp, "TCL", "MEMORY", NULL);
	    }
	    return NULL;
	}
	Tcl_SetObjLength(objResultPtr, length);
	while (count - done > done) {
	    Tcl_AppendObjToObj(objResultPtr, objResultPtr);
	    done *= 2;
	}
	Tcl_AppendToObj(objResultPtr, TclGetString(objResultPtr),
		(count - done) * length);
    }
    return objResultPtr;
}

/*
 *---------------------------------------------------------------------------
 *
 * TclStringCat --
 *
 *	Performs the [string cat] function.
 *
 * Results:
 * 	A (Tcl_Obj *) pointing to the result value, or NULL in case of an
 * 	error.
 *
 * Side effects:
 * 	On error, when interp is not NULL, error information is left in it.
 *
 *---------------------------------------------------------------------------
 */

Tcl_Obj *
TclStringCat(
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj * const objv[],
    int flags)
{
    Tcl_Obj *objResultPtr, * const *ov;
    int oc, binary = 1;
	size_t length = 0;
    int allowUniChar = 1, requestUniChar = 0, forceUniChar = 0;
    int first = objc - 1;	/* Index of first value possibly not empty */
    int last = 0;		/* Index of last value possibly not empty */
    int inPlace = flags & TCL_STRING_IN_PLACE;

    /* assert ( objc >= 0 ) */

    if (objc <= 1) {
	/* Only one or no objects; return first or empty */
	return objc ? objv[0] : Tcl_NewObj();
    }

    /* assert ( objc >= 2 ) */

    /*
     * Analyze to determine what representation result should be.
     * GOALS:	Avoid shimmering & string rep generation.
     * 		Produce pure bytearray when possible.
     * 		Error on overflow.
     */

    ov = objv, oc = objc;
    do {
	Tcl_Obj *objPtr = *ov++;

	if (TclIsPureByteArray(objPtr)) {
	    allowUniChar = 0;
	} else if (objPtr->bytes) {
	    /* Value has a string rep. */
	    if (objPtr->length) {
		/*
		 * Non-empty string rep. Not a pure bytearray, so we won't
		 * create a pure bytearray.
		 */

	 	binary = 0;
	 	if (ov > objv+1 && ISCONTINUATION(TclGetString(objPtr))) {
	 	    forceUniChar = 1;
	 	} else if ((objPtr->typePtr) && (objPtr->typePtr != &tclStringType)) {
		    /* Prevent shimmer of non-string types. */
		    allowUniChar = 0;
		}
	    }
	} else {
	    /* assert (objPtr->typePtr != NULL) -- stork! */
	    binary = 0;
	    if (TclHasInternalRep(objPtr, &tclStringType)) {
		/* Have a pure Unicode value; ask to preserve it */
		requestUniChar = 1;
	    } else {
		/* Have another type; prevent shimmer */
		allowUniChar = 0;
	    }
	}
    } while (--oc && (binary || allowUniChar));

    if (binary) {
	/*
	 * Result will be pure byte array. Pre-size it
	 */

	size_t numBytes = 0;
	ov = objv;
	oc = objc;
	do {
	    Tcl_Obj *objPtr = *ov++;

	    /*
	     * Every argument is either a bytearray with a ("pure")
	     * value we know we can safely use, or it is an empty string.
	     * We don't need to count bytes for the empty strings.
	     */

	    if (TclIsPureByteArray(objPtr)) {
		(void)Tcl_GetByteArrayFromObj(objPtr, &numBytes); /* PANIC? */

		if (numBytes) {
		    last = objc - oc;
		    if (length == 0) {
			first = last;
		    }
		    length += numBytes;
		}
	    }
	} while (--oc);
    } else if ((allowUniChar && requestUniChar) || forceUniChar) {
	/*
	 * Result will be pure Tcl_UniChar array. Pre-size it.
	 */

	ov = objv;
	oc = objc;
	do {
	    Tcl_Obj *objPtr = *ov++;

	    if ((objPtr->bytes == NULL) || (objPtr->length)) {
		size_t numChars;

		(void)Tcl_GetUnicodeFromObj(objPtr, &numChars); /* PANIC? */
		if (numChars) {
		    last = objc - oc;
		    if (length == 0) {
			first = last;
		    }
		    length += numChars;
		}
	    }
	} while (--oc);
    } else {
	/* Result will be concat of string reps. Pre-size it. */
	ov = objv; oc = objc;
	do {
	    Tcl_Obj *pendingPtr = NULL;

	    /*
	     * Loop until a possibly non-empty value is reached.
	     * Keep string rep generation pending when possible.
	     */

	    do {
		/* assert ( pendingPtr == NULL ) */
		/* assert ( length == 0 ) */

		Tcl_Obj *objPtr = *ov++;

		if (objPtr->bytes == NULL) {
		    /* No string rep; Take the chance we can avoid making it */
		    pendingPtr = objPtr;
		} else {
		    (void)Tcl_GetStringFromObj(objPtr, &length); /* PANIC? */
		}
	    } while (--oc && (length == 0) && (pendingPtr == NULL));

	    /*
 	     * Either we found a possibly non-empty value, and we remember
 	     * this index as the first and last such value so far seen,
	     * or (oc == 0) and all values are known empty,
 	     * so first = last = objc - 1 signals the right quick return.
 	     */

	    first = last = objc - oc - 1;

	    if (oc && (length == 0)) {
		size_t numBytes;

		/* assert ( pendingPtr != NULL ) */

		/*
		 * There's a pending value followed by more values.  Loop over
		 * remaining values generating strings until a non-empty value
		 * is found, or the pending value gets its string generated.
		 */

		do {
		    Tcl_Obj *objPtr = *ov++;
		    (void)Tcl_GetStringFromObj(objPtr, &numBytes); /* PANIC? */
		} while (--oc && numBytes == 0 && pendingPtr->bytes == NULL);

		if (numBytes) {
		    last = objc -oc -1;
		}
		if (oc || numBytes) {
		    (void)Tcl_GetStringFromObj(pendingPtr, &length);
		}
		if (length == 0) {
		    if (numBytes) {
			first = last;
		    }
		} else if (numBytes + length > (size_t)INT_MAX) {
		    goto overflow;
		}
		length += numBytes;
	    }
	} while (oc && (length == 0));

	while (oc) {
	    size_t numBytes;
	    Tcl_Obj *objPtr = *ov++;

	    /* assert ( length > 0 && pendingPtr == NULL )  */

	    TclGetString(objPtr); /* PANIC? */
	    numBytes = objPtr->length;
	    if (numBytes) {
		last = objc - oc;
		if (numBytes + length > (size_t)INT_MAX) {
		    goto overflow;
		}
		length += numBytes;
	    }
	    --oc;
	}
    }

    if (last <= first /*|| length == 0 */) {
	/* Only one non-empty value or zero length; return first */
	/* NOTE: (length == 0) implies (last <= first) */
	return objv[first];
    }

    objv += first; objc = (last - first + 1);

    if (binary) {
	/* Efficiently produce a pure byte array result */
	unsigned char *dst;

	/*
	 * Broken interface! Byte array value routines offer no way to handle
	 * failure to allocate enough space. Following stanza may panic.
	 */

	if (inPlace && !Tcl_IsShared(*objv)) {
	    size_t start = 0;

	    objResultPtr = *objv++; objc--;
	    (void)Tcl_GetByteArrayFromObj(objResultPtr, &start);
	    dst = Tcl_SetByteArrayLength(objResultPtr, length) + start;
	} else {
	    objResultPtr = Tcl_NewByteArrayObj(NULL, length);
	    dst = Tcl_SetByteArrayLength(objResultPtr, length);
	}
	while (objc--) {
	    Tcl_Obj *objPtr = *objv++;

	    /*
	     * Every argument is either a bytearray with a ("pure")
	     * value we know we can safely use, or it is an empty string.
	     * We don't need to copy bytes from the empty strings.
	     */

	    if (TclIsPureByteArray(objPtr)) {
		size_t more = 0;
		unsigned char *src = Tcl_GetByteArrayFromObj(objPtr, &more);
		memcpy(dst, src, more);
		dst += more;
	    }
	}
    } else if ((allowUniChar && requestUniChar) || forceUniChar) {
	/* Efficiently produce a pure Tcl_UniChar array result */
	Tcl_UniChar *dst;

	if (inPlace && !Tcl_IsShared(*objv)) {
	    size_t start;

	    objResultPtr = *objv++; objc--;

	    /* Ugly interface! Force resize of the unicode array. */
	    (void)Tcl_GetUnicodeFromObj(objResultPtr, &start);
	    Tcl_InvalidateStringRep(objResultPtr);
	    if (0 == Tcl_AttemptSetObjLength(objResultPtr, length)) {
		if (interp) {
		    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		    	"concatenation failed: unable to alloc %"
			TCL_Z_MODIFIER "u bytes",
			STRING_SIZE(length)));
		    Tcl_SetErrorCode(interp, "TCL", "MEMORY", NULL);
		}
		return NULL;
	    }
	    dst = Tcl_GetUnicode(objResultPtr) + start;
	} else {
	    Tcl_UniChar ch = 0;

	    /* Ugly interface! No scheme to init array size. */
	    objResultPtr = Tcl_NewUnicodeObj(&ch, 0);	/* PANIC? */
	    if (0 == Tcl_AttemptSetObjLength(objResultPtr, length)) {
		Tcl_DecrRefCount(objResultPtr);
		if (interp) {
		    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		    	"concatenation failed: unable to alloc %"
			TCL_Z_MODIFIER "u bytes",
			STRING_SIZE(length)));
		    Tcl_SetErrorCode(interp, "TCL", "MEMORY", NULL);
		}
		return NULL;
	    }
	    dst = Tcl_GetUnicode(objResultPtr);
	}
	while (objc--) {
	    Tcl_Obj *objPtr = *objv++;

	    if ((objPtr->bytes == NULL) || (objPtr->length)) {
		size_t more;
		Tcl_UniChar *src = Tcl_GetUnicodeFromObj(objPtr, &more);
		memcpy(dst, src, more * sizeof(Tcl_UniChar));
		dst += more;
	    }
	}
    } else {
	/* Efficiently concatenate string reps */
	char *dst;

	if (inPlace && !Tcl_IsShared(*objv)) {
	    size_t start;

	    objResultPtr = *objv++; objc--;

	    (void)Tcl_GetStringFromObj(objResultPtr, &start);
	    if (0 == Tcl_AttemptSetObjLength(objResultPtr, length)) {
		if (interp) {
		    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		    	"concatenation failed: unable to alloc %" TCL_Z_MODIFIER "u bytes",
			length));
		    Tcl_SetErrorCode(interp, "TCL", "MEMORY", NULL);
		}
		return NULL;
	    }
	    dst = TclGetString(objResultPtr) + start;

	    /* assert ( length > start ) */
	    TclFreeInternalRep(objResultPtr);
	} else {
	    TclNewObj(objResultPtr);	/* PANIC? */
	    if (0 == Tcl_AttemptSetObjLength(objResultPtr, length)) {
		Tcl_DecrRefCount(objResultPtr);
		if (interp) {
		    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		    	"concatenation failed: unable to alloc %" TCL_Z_MODIFIER "u bytes",
			length));
		    Tcl_SetErrorCode(interp, "TCL", "MEMORY", NULL);
		}
		return NULL;
	    }
	    dst = TclGetString(objResultPtr);
	}
	while (objc--) {
	    Tcl_Obj *objPtr = *objv++;

	    if ((objPtr->bytes == NULL) || (objPtr->length)) {
		size_t more;
		char *src = Tcl_GetStringFromObj(objPtr, &more);

		memcpy(dst, src, more);
		dst += more;
	    }
	}
	/* Must NUL-terminate! */
	*dst = '\0';
    }
    return objResultPtr;

  overflow:
    if (interp) {
	Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		    "max size for a Tcl value (%d bytes) exceeded", INT_MAX));
	Tcl_SetErrorCode(interp, "TCL", "MEMORY", NULL);
    }
    return NULL;
}

/*
 *---------------------------------------------------------------------------
 *
 * TclStringCmp --
 *	Compare two Tcl_Obj values as strings.
 *
 * Results:
 *	Like memcmp, return -1, 0, or 1.
 *
 * Side effects:
 *	String representations may be generated.  Internal representation may
 *	be changed.
 *
 *---------------------------------------------------------------------------
 */

int
TclStringCmp(
    Tcl_Obj *value1Ptr,
    Tcl_Obj *value2Ptr,
    int checkEq,		/* comparison is only for equality */
    int nocase,			/* comparison is not case sensitive */
    size_t reqlength)		/* requested length */
{
    char *s1, *s2;
    int empty, match;
    size_t length, s1len = 0, s2len = 0;
    memCmpFn_t memCmpFn;

    if ((reqlength == 0) || (value1Ptr == value2Ptr)) {
	/*
	 * Always match at 0 chars of if it is the same obj.
	 */
	match = 0;
    } else {
	if (!nocase && TclIsPureByteArray(value1Ptr)
		&& TclIsPureByteArray(value2Ptr)) {
	    /*
	     * Use binary versions of comparisons since that won't cause undue
	     * type conversions and it is much faster. Only do this if we're
	     * case-sensitive (which is all that really makes sense with byte
	     * arrays anyway, and we have no memcasecmp() for some reason... :^)
	     */

	    s1 = (char *) Tcl_GetByteArrayFromObj(value1Ptr, &s1len);
	    s2 = (char *) Tcl_GetByteArrayFromObj(value2Ptr, &s2len);
	    memCmpFn = memcmp;
	} else if (TclHasInternalRep(value1Ptr, &tclStringType)
		&& TclHasInternalRep(value2Ptr, &tclStringType)) {
	    /*
	     * Do a unicode-specific comparison if both of the args are of
	     * String type. If the char length == byte length, we can do a
	     * memcmp. In benchmark testing this proved the most efficient
	     * check between the unicode and string comparison operations.
	     */

	    if (nocase) {
		s1 = (char *) Tcl_GetUnicodeFromObj(value1Ptr, &s1len);
		s2 = (char *) Tcl_GetUnicodeFromObj(value2Ptr, &s2len);
		memCmpFn = (memCmpFn_t)TclUniCharNcasecmp;
	    } else {
		s1len = Tcl_GetCharLength(value1Ptr);
		s2len = Tcl_GetCharLength(value2Ptr);
		if ((s1len == value1Ptr->length)
			&& (value1Ptr->bytes != NULL)
			&& (s2len == value2Ptr->length)
			&& (value2Ptr->bytes != NULL)) {
		    s1 = value1Ptr->bytes;
		    s2 = value2Ptr->bytes;
		    memCmpFn = memcmp;
		} else {
		    s1 = (char *) Tcl_GetUnicode(value1Ptr);
		    s2 = (char *) Tcl_GetUnicode(value2Ptr);
		    if (
#if defined(WORDS_BIGENDIAN) && (TCL_UTF_MAX > 3)
			    1
#else
			    checkEq
#endif
			    ) {
			memCmpFn = memcmp;
			s1len *= sizeof(Tcl_UniChar);
			s2len *= sizeof(Tcl_UniChar);
		    } else {
			memCmpFn = (memCmpFn_t) TclUniCharNcmp;
		    }
		}
	    }
	} else {
	    empty = TclCheckEmptyString(value1Ptr);
	    if (empty > 0) {
		switch (TclCheckEmptyString(value2Ptr)) {
		case -1:
		    s1 = 0;
		    s1len = 0;
		    s2 = Tcl_GetStringFromObj(value2Ptr, &s2len);
		    break;
		case 0:
		    match = -1;
		    goto matchdone;
		case 1:
		default: /* avoid warn: `s2` may be used uninitialized */
		    match = 0;
		    goto matchdone;
		}
	    } else if (TclCheckEmptyString(value2Ptr) > 0) {
		switch (empty) {
		case -1:
		    s2 = 0;
		    s2len = 0;
		    s1 = Tcl_GetStringFromObj(value1Ptr, &s1len);
		    break;
		case 0:
		    match = 1;
		    goto matchdone;
		case 1:
		default: /* avoid warn: `s1` may be used uninitialized */
		    match = 0;
		    goto matchdone;
		}
	    } else {
		s1 = Tcl_GetStringFromObj(value1Ptr, &s1len);
		s2 = Tcl_GetStringFromObj(value2Ptr, &s2len);
	    }
	    if (!nocase && checkEq) {
		/*
		 * When we have equal-length we can check only for
		 * (in)equality. We can use memcmp in all (n)eq cases because
		 * we don't need to worry about lexical LE/BE variance.
		 */

		memCmpFn = memcmp;
	    } else {
		/*
		 * As a catch-all we will work with UTF-8. We cannot use
		 * memcmp() as that is unsafe with any string containing NUL
		 * (\xC0\x80 in Tcl's utf rep). We can use the more efficient
		 * TclpUtfNcmp2 if we are case-sensitive and no specific
		 * length was requested.
		 */

		if ((reqlength == TCL_INDEX_NONE) && !nocase) {
		    memCmpFn = (memCmpFn_t) TclpUtfNcmp2;
		} else {
		    s1len = Tcl_NumUtfChars(s1, s1len);
		    s2len = Tcl_NumUtfChars(s2, s2len);
		    memCmpFn = (memCmpFn_t)
			    (nocase ? Tcl_UtfNcasecmp : Tcl_UtfNcmp);
		}
	    }
	}

	length = (s1len < s2len) ? s1len : s2len;
	if (reqlength == TCL_INDEX_NONE) {
	    /*
	     * The requested length is negative, so we ignore it by setting it
	     * to length + 1 so we correct the match var.
	     */

	    reqlength = length + 1;
	} else if (reqlength > 0 && reqlength < length) {
	    length = reqlength;
	}

	if (checkEq && (s1len != s2len)) {
	    match = 1;		/* This will be reversed below. */
	} else {
	    /*
	     * The comparison function should compare up to the minimum byte
	     * length only.
	     */

	    match = memCmpFn(s1, s2, length);
	}
	if ((match == 0) && (reqlength > length)) {
	    match = s1len - s2len;
	}
	match = (match > 0) ? 1 : (match < 0) ? -1 : 0;
    }
  matchdone:
    return match;
}

/*
 *---------------------------------------------------------------------------
 *
 * TclStringFirst --
 *
 *	Implements the [string first] operation.
 *
 * Results:
 *	If needle is found as a substring of haystack, the index of the
 *	first instance of such a find is returned.  If needle is not present
 *	as a substring of haystack, -1 is returned.
 *
 * Side effects:
 *	needle and haystack may have their Tcl_ObjType changed.
 *
 *---------------------------------------------------------------------------
 */

Tcl_Obj *
TclStringFirst(
    Tcl_Obj *needle,
    Tcl_Obj *haystack,
    size_t start)
{
    size_t lh = 0, ln = Tcl_GetCharLength(needle);
    size_t value = TCL_INDEX_NONE;
    Tcl_UniChar *checkStr, *endStr, *uh, *un;
    Tcl_Obj *obj;

    if (start == TCL_INDEX_NONE) {
	start = 0;
    }
    if (ln == 0) {
	/* We don't find empty substrings.  Bizarre!
	 * Whenever this routine is turned into a proper substring
	 * finder, change to `return start` after limits imposed. */
	goto firstEnd;
    }

    if (TclIsPureByteArray(needle) && TclIsPureByteArray(haystack)) {
	unsigned char *end, *check, *bh;
	unsigned char *bn = Tcl_GetByteArrayFromObj(needle, &ln);

	/* Find bytes in bytes */
	bh = Tcl_GetByteArrayFromObj(haystack, &lh);
	if ((lh < ln) || (start > lh - ln)) {
	    /* Don't start the loop if there cannot be a valid answer */
	    goto firstEnd;
	}
	end = bh + lh;

	check = bh + start;
	while (check + ln <= end) {
	    /*
	     * Look for the leading byte of the needle in the haystack
	     * starting at check and stopping when there's not enough room
	     * for the needle left.
	     */
	    check = (unsigned char *)memchr(check, bn[0], (end + 1 - ln) - check);
	    if (check == NULL) {
		/* Leading byte not found -> needle cannot be found. */
		goto firstEnd;
	    }
	    /* Leading byte found, check rest of needle. */
	    if (0 == memcmp(check+1, bn+1, ln-1)) {
		/* Checks! Return the successful index. */
		value = (check - bh);
		goto firstEnd;
	    }
	    /* Rest of needle match failed; Iterate to continue search. */
	    check++;
	}
	goto firstEnd;
    }

    /*
     * TODO: It might be nice to support some cases where it is not
     * necessary to shimmer to &tclStringType to compute the result,
     * and instead operate just on the objPtr->bytes values directly.
     * However, we also do not want the answer to change based on the
     * code pathway, or if it does we want that to be for some values
     * we explicitly decline to support.  Getting there will involve
     * locking down in practice more firmly just what encodings produce
     * what supported results for the objPtr->bytes values.  For now,
     * do only the well-defined Tcl_UniChar array search.
     */

    un = Tcl_GetUnicodeFromObj(needle, &ln);
    uh = Tcl_GetUnicodeFromObj(haystack, &lh);
    if ((lh < ln) || (start > lh - ln)) {
	/* Don't start the loop if there cannot be a valid answer */
	goto firstEnd;
    }
    endStr = uh + lh;

    for (checkStr = uh + start; checkStr + ln <= endStr; checkStr++) {
	if ((*checkStr == *un) && (0 ==
		memcmp(checkStr + 1, un + 1, (ln-1) * sizeof(Tcl_UniChar)))) {
	    value =  (checkStr - uh);
	    goto firstEnd;
	}
    }
  firstEnd:
    TclNewIndexObj(obj, value);
    return obj;
}

/*
 *---------------------------------------------------------------------------
 *
 * TclStringLast --
 *
 *	Implements the [string last] operation.
 *
 * Results:
 *	If needle is found as a substring of haystack, the index of the
 *	last instance of such a find is returned.  If needle is not present
 *	as a substring of haystack, -1 is returned.
 *
 * Side effects:
 *	needle and haystack may have their Tcl_ObjType changed.
 *
 *---------------------------------------------------------------------------
 */

Tcl_Obj *
TclStringLast(
    Tcl_Obj *needle,
    Tcl_Obj *haystack,
    size_t last)
{
    size_t lh = 0, ln = Tcl_GetCharLength(needle);
    size_t value = TCL_INDEX_NONE;
    Tcl_UniChar *checkStr, *uh, *un;
    Tcl_Obj *obj;

    if (ln == 0) {
	/*
	 * 	We don't find empty substrings.  Bizarre!
	 *
	 * 	TODO: When we one day make this a true substring
	 * 	finder, change this to "return last", after limitation.
	 */
	goto lastEnd;
    }

    if (TclIsPureByteArray(needle) && TclIsPureByteArray(haystack)) {
	unsigned char *check, *bh = Tcl_GetByteArrayFromObj(haystack, &lh);
	unsigned char *bn = Tcl_GetByteArrayFromObj(needle, &ln);

	if (last + 1 >= lh + 1) {
	    last = lh - 1;
	}
	if (last + 1 < ln) {
	    /* Don't start the loop if there cannot be a valid answer */
	    goto lastEnd;
	}
	check = bh + last + 1 - ln;

	while (check >= bh) {
	    if ((*check == bn[0])
		    && (0 == memcmp(check+1, bn+1, ln-1))) {
		value = (check - bh);
		goto lastEnd;
	    }
	    check--;
	}
	goto lastEnd;
    }

    uh = Tcl_GetUnicodeFromObj(haystack, &lh);
    un = Tcl_GetUnicodeFromObj(needle, &ln);

    if (last + 1 >= lh + 1) {
	last = lh - 1;
    }
    if (last + 1 < ln) {
	/* Don't start the loop if there cannot be a valid answer */
	goto lastEnd;
    }
    checkStr = uh + last + 1 - ln;
    while (checkStr >= uh) {
	if ((*checkStr == un[0])
		&& (0 == memcmp(checkStr+1, un+1, (ln-1)*sizeof(Tcl_UniChar)))) {
	    value = (checkStr - uh);
	    goto lastEnd;
	}
	checkStr--;
    }
  lastEnd:
    TclNewIndexObj(obj, value);
    return obj;
}

/*
 *---------------------------------------------------------------------------
 *
 * TclStringReverse --
 *
 *	Implements the [string reverse] operation.
 *
 * Results:
 *	A Tcl value which is the [string reverse] of the argument supplied.
 *	When sharing rules permit and the caller requests, the returned value
 *	might be the argument with modifications done in place.
 *
 * Side effects:
 *	May allocate a new Tcl_Obj.
 *
 *---------------------------------------------------------------------------
 */

static void
ReverseBytes(
    unsigned char *to,		/* Copy bytes into here... */
    unsigned char *from,	/* ...from here... */
    size_t count)		/* Until this many are copied, */
				/* reversing as you go. */
{
    unsigned char *src = from + count;

    if (to == from) {
	/* Reversing in place */
	while (--src > to) {
	    unsigned char c = *src;

	    *src = *to;
	    *to++ = c;
	}
    } else {
	while (--src >= from) {
	    *to++ = *src;
	}
    }
}

Tcl_Obj *
TclStringReverse(
    Tcl_Obj *objPtr,
    int flags)
{
    String *stringPtr;
    Tcl_UniChar ch = 0;
    int inPlace = flags & TCL_STRING_IN_PLACE;
#if TCL_UTF_MAX < 4
    int needFlip = 0;
#endif

    if (TclIsPureByteArray(objPtr)) {
	size_t numBytes = 0;
	unsigned char *from = Tcl_GetByteArrayFromObj(objPtr, &numBytes);

	if (!inPlace || Tcl_IsShared(objPtr)) {
	    objPtr = Tcl_NewByteArrayObj(NULL, numBytes);
	}
	ReverseBytes(Tcl_GetByteArrayFromObj(objPtr, (size_t *)NULL), from, numBytes);
	return objPtr;
    }

    SetStringFromAny(NULL, objPtr);
    stringPtr = GET_STRING(objPtr);

    if (stringPtr->hasUnicode) {
	Tcl_UniChar *from = Tcl_GetUnicode(objPtr);
	stringPtr = GET_STRING(objPtr);
	Tcl_UniChar *src = from + stringPtr->numChars;
	Tcl_UniChar *to;

	if (!inPlace || Tcl_IsShared(objPtr)) {
	    /*
	     * Create a non-empty, pure unicode value, so we can coax
	     * Tcl_SetObjLength into growing the unicode rep buffer.
	     */

	    objPtr = Tcl_NewUnicodeObj(&ch, 1);
	    Tcl_SetObjLength(objPtr, stringPtr->numChars);
	    to = Tcl_GetUnicode(objPtr);
	    stringPtr = GET_STRING(objPtr);
	    while (--src >= from) {
#if TCL_UTF_MAX < 4
		ch = *src;
		if ((ch & 0xF800) == 0xD800) {
		    needFlip = 1;
		}
		*to++ = ch;
#else
		*to++ = *src;
#endif
	    }
	} else {
	    /*
	     * Reversing in place.
	     */

#if TCL_UTF_MAX < 4
	    to = src;
#endif
	    while (--src > from) {
		ch = *src;
#if TCL_UTF_MAX < 4
		if ((ch & 0xF800) == 0xD800) {
		    needFlip = 1;
		}
#endif
		*src = *from;
		*from++ = ch;
	    }
	}
#if TCL_UTF_MAX < 4
	if (needFlip) {
	    /*
	     * Flip back surrogate pairs.
	     */

	    from = to - stringPtr->numChars;
	    while (--to >= from) {
		ch = *to;
		if ((ch & 0xFC00) == 0xD800) {
		    if ((to-1 >= from) && ((to[-1] & 0xFC00) == 0xDC00)) {
			to[0] = to[-1];
			to[-1] = ch;
			--to;
		    }
		}
	    }
	}
#endif
    }

    if (objPtr->bytes) {
	size_t numChars = stringPtr->numChars;
	size_t numBytes = objPtr->length;
	char *to, *from = objPtr->bytes;

	if (!inPlace || Tcl_IsShared(objPtr)) {
	    TclNewObj(objPtr);
	    Tcl_SetObjLength(objPtr, numBytes);
	}
	to = objPtr->bytes;

	if ((numChars == TCL_INDEX_NONE) || (numChars < numBytes)) {
	    /*
	     * Either numChars == -1 and we don't know how many chars are
	     * represented by objPtr->bytes and we need Pass 1 just in case,
	     * or numChars >= 0 and we know we have fewer chars than bytes, so
	     * we know there's a multibyte character needing Pass 1.
	     *
	     * Pass 1. Reverse the bytes of each multi-byte character.
	     */

	    size_t bytesLeft = numBytes;
	    int chw;

	    while (bytesLeft) {
		/*
		 * NOTE: We know that the from buffer is NUL-terminated. It's
		 * part of the contract for objPtr->bytes values. Thus, we can
		 * skip calling Tcl_UtfCharComplete() here.
		 */

		size_t bytesInChar = TclUtfToUCS4(from, &chw);

		ReverseBytes((unsigned char *)to, (unsigned char *)from,
			bytesInChar);
		to += bytesInChar;
		from += bytesInChar;
		bytesLeft -= bytesInChar;
	    }

	    from = to = objPtr->bytes;
	}
	/* Pass 2. Reverse all the bytes. */
	ReverseBytes((unsigned char *)to, (unsigned char *)from, numBytes);
    }

    return objPtr;
}

/*
 *---------------------------------------------------------------------------
 *
 * TclStringReplace --
 *
 *	Implements the inner engine of the [string replace] and
 *	[string insert] commands.
 *
 *	The result is a concatenation of a prefix from objPtr, characters
 *	0 through first-1, the insertPtr string value, and a suffix from
 *	objPtr, characters from first + count to the end. The effect is as if
 *	the inner substring of characters first through first+count-1 are
 *	removed and replaced with insertPtr. If insertPtr is NULL, it is
 *	treated as an empty string. When passed the flag TCL_STRING_IN_PLACE,
 *	this routine will try to do the work within objPtr, so long as no
 *	sharing forbids it. Without that request, or as needed, a new Tcl
 *	value will be allocated to be the result.
 *
 * Results:
 *	A Tcl value that is the result of the substring replacement. May
 *	return NULL in case of an error. When NULL is returned and interp is
 *	non-NULL, error information is left in interp
 *
 *---------------------------------------------------------------------------
 */

Tcl_Obj *
TclStringReplace(
    Tcl_Interp *interp,		/* For error reporting, may be NULL */
    Tcl_Obj *objPtr,		/* String to act upon */
    size_t first,		/* First index to replace */
    size_t count,		/* How many chars to replace */
    Tcl_Obj *insertPtr,		/* Replacement string, may be NULL */
    int flags)			/* TCL_STRING_IN_PLACE => attempt in-place */
{
    int inPlace = flags & TCL_STRING_IN_PLACE;
    Tcl_Obj *result;

    /* Replace nothing with nothing */
    if ((insertPtr == NULL) && (count == 0)) {
	if (inPlace) {
	    return objPtr;
	} else {
	    return Tcl_DuplicateObj(objPtr);
	}
    }

    /*
     * The caller very likely had to call Tcl_GetCharLength() or similar
     * to be able to process index values.  This means it is likely that
     * objPtr is either a proper "bytearray" or a "string" or else it has
     * a known and short string rep.
     */

    if (TclIsPureByteArray(objPtr)) {
	size_t numBytes = 0;
	unsigned char *bytes = Tcl_GetByteArrayFromObj(objPtr, &numBytes);

	if (insertPtr == NULL) {
	    /* Replace something with nothing. */

	    assert ( first <= numBytes ) ;
	    assert ( count <= numBytes ) ;
	    assert ( first + count <= numBytes ) ;

	    result = Tcl_NewByteArrayObj(NULL, numBytes - count);/* PANIC? */
	    TclAppendBytesToByteArray(result, bytes, first);
	    TclAppendBytesToByteArray(result, bytes + first + count,
		    numBytes - count - first);
	    return result;
	}

	/* Replace everything */
	if ((first == 0) && (count == numBytes)) {
	    return insertPtr;
	}

	if (TclIsPureByteArray(insertPtr)) {
	    size_t newBytes = 0;
	    unsigned char *iBytes
		    = Tcl_GetByteArrayFromObj(insertPtr, &newBytes);

	    if (count == newBytes && inPlace && !Tcl_IsShared(objPtr)) {
		/*
		 * Removal count and replacement count are equal.
		 * Other conditions permit. Do in-place splice.
		 */

		memcpy(bytes + first, iBytes, count);
		Tcl_InvalidateStringRep(objPtr);
		return objPtr;
	    }

	    if ((size_t)newBytes > INT_MAX - (numBytes - count)) {
		if (interp) {
		    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
			    "max size for a Tcl value (%d bytes) exceeded",
			    INT_MAX));
		    Tcl_SetErrorCode(interp, "TCL", "MEMORY", NULL);
		}
		return NULL;
	    }
	    result = Tcl_NewByteArrayObj(NULL, numBytes - count + newBytes);
								/* PANIC? */
	    Tcl_SetByteArrayLength(result, 0);
	    TclAppendBytesToByteArray(result, bytes, first);
	    TclAppendBytesToByteArray(result, iBytes, newBytes);
	    TclAppendBytesToByteArray(result, bytes + first + count,
		    numBytes - count - first);
	    return result;
	}

	/* Flow through to try other approaches below */
    }

    /*
     * TODO: Figure out how not to generate a Tcl_UniChar array rep
     * when it can be determined objPtr->bytes points to a string of
     * all single-byte characters so we can index it directly.
     */

    /* The traditional implementation... */
    {
	size_t numChars;
	Tcl_UniChar *ustring = Tcl_GetUnicodeFromObj(objPtr, &numChars);

	/* TODO: Is there an in-place option worth pursuing here? */

	result = Tcl_NewUnicodeObj(ustring, first);
	if (insertPtr) {
	    Tcl_AppendObjToObj(result, insertPtr);
	}
	if (first + count < (size_t)numChars) {
	    TclAppendUnicodeToObj(result, ustring + first + count,
		    numChars - first - count);
	}

	return result;
    }
}

/*
 *---------------------------------------------------------------------------
 *
 * FillUnicodeRep --
 *
 *	Populate the Unicode internal rep with the Unicode form of its string
 *	rep. The object must alread have a "String" internal rep.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Reallocates the String internal rep.
 *
 *---------------------------------------------------------------------------
 */

static void
FillUnicodeRep(
    Tcl_Obj *objPtr)		/* The object in which to fill the unicode
				 * rep. */
{
    String *stringPtr = GET_STRING(objPtr);

    ExtendUnicodeRepWithString(objPtr, objPtr->bytes, objPtr->length,
	    stringPtr->numChars);
}

static void
ExtendUnicodeRepWithString(
    Tcl_Obj *objPtr,
    const char *bytes,
    size_t numBytes,
    size_t numAppendChars)
{
    String *stringPtr = GET_STRING(objPtr);
    size_t needed, numOrigChars = 0;
    Tcl_UniChar *dst, unichar = 0;

    if (stringPtr->hasUnicode) {
	numOrigChars = stringPtr->numChars;
    }
    if (numAppendChars == TCL_INDEX_NONE) {
	TclNumUtfChars(numAppendChars, bytes, numBytes);
    }
    needed = numOrigChars + numAppendChars;

    if (needed > stringPtr->maxChars) {
	GrowUnicodeBuffer(objPtr, needed);
	stringPtr = GET_STRING(objPtr);
    }

    stringPtr->hasUnicode = 1;
    if (bytes) {
	stringPtr->numChars = needed;
    } else {
	numAppendChars = 0;
    }
    dst = stringPtr->unicode + numOrigChars;
    if (numAppendChars-- > 0) {
	bytes += TclUtfToUniChar(bytes, &unichar);
#if TCL_UTF_MAX > 3
	/* join upper/lower surrogate */
	if (bytes && (stringPtr->unicode[numOrigChars - 1] | 0x3FF) == 0xDBFF && (unichar | 0x3FF) == 0xDFFF) {
		stringPtr->numChars--;
		unichar = ((stringPtr->unicode[numOrigChars - 1] & 0x3FF) << 10) + (unichar & 0x3FF) + 0x10000;
		dst--;
	}
#endif
	*dst++ = unichar;
	while (numAppendChars-- > 0) {
	    bytes += TclUtfToUniChar(bytes, &unichar);
	    *dst++ = unichar;
	}
    }
    *dst = 0;
}

/*
 *----------------------------------------------------------------------
 *
 * DupStringInternalRep --
 *
 *	Initialize the internal representation of a new Tcl_Obj to a copy of
 *	the internal representation of an existing string object.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	copyPtr's internal rep is set to a copy of srcPtr's internal
 *	representation.
 *
 *----------------------------------------------------------------------
 */

static void
DupStringInternalRep(
    Tcl_Obj *srcPtr,		/* Object with internal rep to copy. Must have
				 * an internal rep of type "String". */
    Tcl_Obj *copyPtr)		/* Object with internal rep to set. Must not
				 * currently have an internal rep.*/
{
    String *srcStringPtr = GET_STRING(srcPtr);
    String *copyStringPtr = NULL;

    if (srcStringPtr->numChars == TCL_INDEX_NONE) {
	/*
	 * The String struct in the source value holds zero useful data. Don't
	 * bother copying it. Don't even bother allocating space in which to
	 * copy it. Just let the copy be untyped.
	 */

	return;
    }

    if (srcStringPtr->hasUnicode) {
	int copyMaxChars;

	if (srcStringPtr->maxChars / 2 >= srcStringPtr->numChars) {
	    copyMaxChars = 2 * srcStringPtr->numChars;
	} else {
	    copyMaxChars = srcStringPtr->maxChars;
	}
	copyStringPtr = stringAttemptAlloc(copyMaxChars);
	if (copyStringPtr == NULL) {
	    copyMaxChars = srcStringPtr->numChars;
	    copyStringPtr = stringAlloc(copyMaxChars);
	}
	copyStringPtr->maxChars = copyMaxChars;
	memcpy(copyStringPtr->unicode, srcStringPtr->unicode,
		srcStringPtr->numChars * sizeof(Tcl_UniChar));
	copyStringPtr->unicode[srcStringPtr->numChars] = 0;
    } else {
	copyStringPtr = stringAlloc(0);
	copyStringPtr->maxChars = 0;
	copyStringPtr->unicode[0] = 0;
    }
    copyStringPtr->hasUnicode = srcStringPtr->hasUnicode;
    copyStringPtr->numChars = srcStringPtr->numChars;

    /*
     * Tricky point: the string value was copied by generic object management
     * code, so it doesn't contain any extra bytes that might exist in the
     * source object.
     */

    copyStringPtr->allocated = copyPtr->bytes ? copyPtr->length : 0;

    SET_STRING(copyPtr, copyStringPtr);
    copyPtr->typePtr = &tclStringType;
}

/*
 *----------------------------------------------------------------------
 *
 * SetStringFromAny --
 *
 *	Create an internal representation of type "String" for an object.
 *
 * Results:
 *	This operation always succeeds and returns TCL_OK.
 *
 * Side effects:
 *	Any old internal reputation for objPtr is freed and the internal
 *	representation is set to "String".
 *
 *----------------------------------------------------------------------
 */

static int
SetStringFromAny(
    TCL_UNUSED(Tcl_Interp *),
    Tcl_Obj *objPtr)		/* The object to convert. */
{
    if (!TclHasInternalRep(objPtr, &tclStringType)) {
	String *stringPtr = stringAlloc(0);

	/*
	 * Convert whatever we have into an untyped value. Just A String.
	 */

	(void) TclGetString(objPtr);
	TclFreeInternalRep(objPtr);

	/*
	 * Create a basic String internalrep that just points to the UTF-8 string
	 * already in place at objPtr->bytes.
	 */

	stringPtr->numChars = -1;
	stringPtr->allocated = objPtr->length;
	stringPtr->maxChars = 0;
	stringPtr->hasUnicode = 0;
	SET_STRING(objPtr, stringPtr);
	objPtr->typePtr = &tclStringType;
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * UpdateStringOfString --
 *
 *	Update the string representation for an object whose internal
 *	representation is "String".
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The object's string may be set by converting its Unicode represention
 *	to UTF format.
 *
 *----------------------------------------------------------------------
 */

static void
UpdateStringOfString(
    Tcl_Obj *objPtr)		/* Object with string rep to update. */
{
    String *stringPtr = GET_STRING(objPtr);

    /*
     * This routine is only called when we need to generate the
     * string rep objPtr->bytes because it does not exist -- it is NULL.
     * In that circumstance, any lingering claim about the size of
     * memory pointed to by that NULL pointer is clearly bogus, and
     * needs a reset.
     */

    stringPtr->allocated = 0;

    if (stringPtr->numChars == 0) {
	TclInitStringRep(objPtr, NULL, 0);
    } else {
	(void) ExtendStringRepWithUnicode(objPtr, stringPtr->unicode,
		stringPtr->numChars);
    }
}

static size_t
ExtendStringRepWithUnicode(
    Tcl_Obj *objPtr,
    const Tcl_UniChar *unicode,
    size_t numChars)
{
    /*
     * Pre-condition: this is the "string" Tcl_ObjType.
     */

    size_t i, origLength, size = 0;
    char *dst;
    String *stringPtr = GET_STRING(objPtr);

    if (numChars == TCL_INDEX_NONE) {
	numChars = UnicodeLength(unicode);
    }

    if (numChars == 0) {
	return 0;
    }

    if (objPtr->bytes == NULL) {
	objPtr->length = 0;
    }
    size = origLength = objPtr->length;

    /*
     * Quick cheap check in case we have more than enough room.
     */

    if (numChars <= (INT_MAX - size)/TCL_UTF_MAX
	    && stringPtr->allocated >= size + numChars * TCL_UTF_MAX) {
	goto copyBytes;
    }

    for (i = 0; i < numChars; i++) {
	size += TclUtfCount(unicode[i]);
    }

    /*
     * Grow space if needed.
     */

    if (size > stringPtr->allocated) {
	GrowStringBuffer(objPtr, size, 1);
    }

  copyBytes:
    dst = objPtr->bytes + origLength;
    for (i = 0; i < numChars; i++) {
	dst += Tcl_UniCharToUtf(unicode[i], dst);
    }
    *dst = '\0';
    objPtr->length = dst - objPtr->bytes;
    return numChars;
}

/*
 *----------------------------------------------------------------------
 *
 * FreeStringInternalRep --
 *
 *	Deallocate the storage associated with a String data object's internal
 *	representation.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Frees memory.
 *
 *----------------------------------------------------------------------
 */

static void
FreeStringInternalRep(
    Tcl_Obj *objPtr)		/* Object with internal rep to free. */
{
    Tcl_Free(GET_STRING(objPtr));
    objPtr->typePtr = NULL;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
