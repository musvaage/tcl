/*
 * tclStringRep.h --
 *
 *	This file contains the definition of the Unicode string internal
 *	representation and macros to access it.
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
 * Copyright (c) 1995-1997 Sun Microsystems, Inc.
 * Copyright (c) 1999 by Scriptics Corporation.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

/*
 * The following structure is the internal rep for a String object. It keeps
 * track of how much memory has been used and how much has been allocated for
 * the Unicode and UTF string to enable growing and shrinking of the UTF and
 * Unicode reps of the String object with fewer mallocs. To optimize string
 * length and indexing operations, this structure also stores the number of
 * characters (same of UTF and Unicode!) once that value has been computed.
 */

typedef struct {
    int numChars;		/* The number of chars in the string. -1 means
				 * this value has not been calculated. >= 0
				 * means that there is a valid Unicode rep, or
				 * that the number of UTF bytes == the number
				 * of chars. */
    int allocated;		/* The amount of space actually allocated for
				 * the UTF string (minus 1 byte for the
				 * termination char). */
    int maxChars;		/* Max number of chars that can fit in the
				 * space allocated for the unicode array. */
    int hasUnicode;		/* Boolean determining whether the string has
				 * a Unicode representation. */
    unsigned short unicode[TCLFLEXARRAY];	/* The array of Unicode chars. The actual size
				 * of this field depends on the 'maxChars'
				 * field above. */
} String;

#define STRING_MAXCHARS \
    (int)(((size_t)UINT_MAX - 1 - offsetof(String, unicode))/sizeof(unsigned short))
#define STRING_SIZE(numChars) \
    (offsetof(String, unicode) + (((numChars) + 1U) * sizeof(unsigned short)))
#define stringCheckLimits(numChars) \
    do {								\
	if ((numChars) < 0 || (numChars) > STRING_MAXCHARS) {		\
	    Tcl_Panic("max length for a Tcl unicode value (%d chars) exceeded", \
		      (int)STRING_MAXCHARS);					\
	}								\
    } while (0)
#define stringAttemptAlloc(numChars) \
    (String *) attemptckalloc(STRING_SIZE(numChars))
#define stringAlloc(numChars) \
    (String *) ckalloc(STRING_SIZE(numChars))
#define stringRealloc(ptr, numChars) \
    (String *) ckrealloc((ptr), STRING_SIZE(numChars))
#define stringAttemptRealloc(ptr, numChars) \
    (String *) attemptckrealloc((ptr), STRING_SIZE(numChars))
#define GET_STRING(objPtr) \
    ((String *) (objPtr)->internalRep.twoPtrValue.ptr1)
#define SET_STRING(objPtr, stringPtr) \
    ((objPtr)->internalRep.twoPtrValue.ptr2 = NULL),			\
    ((objPtr)->internalRep.twoPtrValue.ptr1 = (void *) (stringPtr))

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
