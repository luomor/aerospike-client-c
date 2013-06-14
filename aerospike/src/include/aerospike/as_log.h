/******************************************************************************
 *	Copyright 2008-2013 by Aerospike.
 *
 *	Permission is hereby granted, free of charge, to any person obtaining a copy 
 *	of this software and associated documentation files (the "Software"), to 
 *	deal in the Software without restriction, including without limitation the 
 *	rights to use, copy, modify, merge, publish, distribute, sublicense, and/or 
 *	sell copies of the Software, and to permit persons to whom the Software is 
 *	furnished to do so, subject to the following conditions:
 *	
 *	The above copyright notice and this permission notice shall be included in 
 *	all copies or substantial portions of the Software.
 *	
 *	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *	IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *	FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *	AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *	LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
 *	FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *	IN THE SOFTWARE.
 *****************************************************************************/

#pragma once 

#include <aerospike/as_status.h>

#include <citrusleaf/cf_atomic.h>

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/******************************************************************************
 *	TYPES
 *****************************************************************************/

/**
 *	Log Level
 */
typedef enum as_log_level_e {
	AS_LOG_LEVEL_OFF 	= -1,
	AS_LOG_LEVEL_ERROR	= 0,
	AS_LOG_LEVEL_WARN	= 1,
	AS_LOG_LEVEL_INFO	= 2,
	AS_LOG_LEVEL_DEBUG	= 3,
	AS_LOG_LEVEL_TRACE 	= 4
} as_log_level;

/**
 *	Callback function for as_log related logging calls.
 *
 *	@param level 		The log level of the message.
 *	@param func 			The function where the message was logged.
 *	@param file 			The file where the message was logged.
 *	@param line 			The line where the message was logged.
 *	@param fmt 			The format string used.
 *	@param ... 			The format argument.
 *
 *	@return true if the message was logged. Otherwise false.
 */
typedef bool (* as_log_callback)(
	as_log_level level, const char * func, const char * file, uint32_t line,
	const char * fmt, ...);

/**
 *	Logging Context
 */
typedef struct as_log_s {

	/**
	 *	Log Level
	 */
	cf_atomic32 level;

	/**
	 *	Logging Callback
	 */
	cf_atomic_p callback;

} as_log;

/******************************************************************************
 *	FUNCTIONS
 *****************************************************************************/

/**
 *	Initialize Log Context 
 */
as_log * as_log_init(as_log * log);

/**
 *	Set the level for the given log.
 *
 *	@param log 		The log context.
 *	@param level 	The log level.
 *
 *	@return true on success. Otherwise false.
 */
bool as_log_set_level(as_log * log, as_log_level level);

/**
 *	Set the callback for the given log
 *
 *	@param log 		The log context.
 *	@param callback 	The log callback.
 *
 *	@return true on success. Otherwise false.
 */
bool as_log_set_callback(as_log * log, as_log_callback callback);
