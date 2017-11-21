/***********************************************************************
 *
 * Project:  PgSQL HTTP
 * Purpose:  Main file.
 *
 ***********************************************************************
 * Copyright 2015 Paul Ramsey <pramsey@cleverelephant.ca>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 ***********************************************************************/

/* Constants */
#define HTTP_VERSION "1.2.1"
#define HTTP_ENCODING "gzip"
#define CURL_MIN_VERSION 0x071400 /* 7.20.0 */

/* System */
#include <regex.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>	/* INT_MAX */
#include <signal.h> /* SIGINT */

/* PostgreSQL */
#include <postgres.h>
#include <fmgr.h>
#include <funcapi.h>
#include <access/htup.h>
#include <catalog/namespace.h>
#include <catalog/pg_type.h>
#include <catalog/dependency.h>
#include <commands/extension.h>
#include <lib/stringinfo.h>
#include <mb/pg_wchar.h>
#include <utils/array.h>
#include <utils/builtins.h>
#include <utils/lsyscache.h>
#include <utils/syscache.h>
#include <utils/typcache.h>
#include <utils/guc.h>

#if PG_VERSION_NUM >= 100000
#include <utils/varlena.h>
#endif

#if PG_VERSION_NUM >= 90300
#include <access/htup_details.h>
#endif

/* CURL */
#include <curl/curl.h>

/* Set up PgSQL */
PG_MODULE_MAGIC;


/* HTTP request methods we support */
typedef enum {
	HTTP_GET,
	HTTP_POST,
	HTTP_DELETE,
	HTTP_PUT,
	HTTP_HEAD
} http_method;

/* Components (and postitions) of the http_request tuple type */
enum {
	REQ_METHOD = 0,
	REQ_URI = 1,
	REQ_HEADERS = 2,
	REQ_CONTENT_TYPE = 3,
	REQ_CONTENT = 4
} http_request_type;

/* Components (and postitions) of the http_response tuple type */
enum {
	RESP_STATUS = 0,
	RESP_CONTENT_TYPE = 1,
	RESP_HEADERS = 2,
	RESP_CONTENT = 3
} http_response_type;

/* Components (and postitions) of the http_header tuple type */
enum {
	HEADER_FIELD = 0,
	HEADER_VALUE = 1
} http_header_type;

typedef enum {
	CURLOPT_STRING,
	CURLOPT_LONG
} http_curlopt_type;

/* CURLOPT string/enum value mapping */
typedef struct {
	char *curlopt_str;
	CURLoption curlopt;
	http_curlopt_type curlopt_type;
	bool superuser_only;
} http_curlopt;

/* CURLOPT values we allow user to set at run-time */
/* Be careful adding these, as they can be a security risk */
static http_curlopt settable_curlopts[] = {
	{ "CURLOPT_CAINFO", CURLOPT_CAINFO, CURLOPT_STRING, false },
	{ "CURLOPT_TIMEOUT", CURLOPT_TIMEOUT, CURLOPT_LONG, false },
	{ "CURLOPT_TIMEOUT_MS", CURLOPT_TIMEOUT_MS, CURLOPT_LONG, false },
	{ "CURLOPT_CONNECTTIMEOUT", CURLOPT_CONNECTTIMEOUT, CURLOPT_LONG, false },
#if LIBCURL_VERSION_NUM >= 0x070e01 /* 7.14.1 */
	{ "CURLOPT_PROXY", CURLOPT_PROXY, CURLOPT_STRING, false },
	{ "CURLOPT_PROXYPORT", CURLOPT_PROXYPORT, CURLOPT_LONG, false },
#endif
#if LIBCURL_VERSION_NUM >= 0x071301 /* 7.19.1 */
	{ "CURLOPT_PROXYUSERNAME", CURLOPT_PROXYUSERNAME, CURLOPT_STRING, false },
	{ "CURLOPT_PROXYPASSWORD", CURLOPT_PROXYPASSWORD, CURLOPT_STRING, false },
#endif
// #if LIBCURL_VERSION_NUM >= 0x071304 /* 7.19.4 */
// 	{ "CURLOPT_PROTOCOLS", CURLOPT_PROTOCOLS, CURLOPT_LONG, true },
// #endif
#if LIBCURL_VERSION_NUM >= 0x071504 /* 7.21.4 */
	{ "CURLOPT_TLSAUTH_USERNAME", CURLOPT_TLSAUTH_USERNAME, CURLOPT_STRING, false },
	{ "CURLOPT_TLSAUTH_PASSWORD", CURLOPT_TLSAUTH_PASSWORD, CURLOPT_STRING, false },
	{ "CURLOPT_TLSAUTH_TYPE", CURLOPT_TLSAUTH_TYPE, CURLOPT_STRING, false },
#endif
#if LIBCURL_VERSION_NUM >= 0x071900 /* 7.25.0 */
	{ "CURLOPT_TCP_KEEPALIVE", CURLOPT_TCP_KEEPALIVE, CURLOPT_LONG, false },
	{ "CURLOPT_TCP_KEEPIDLE", CURLOPT_TCP_KEEPIDLE, CURLOPT_LONG, false },
#endif
#if LIBCURL_VERSION_NUM >= 0x073400  /* 7.52.0 */
	{ "CURLOPT_PRE_PROXY", CURLOPT_PRE_PROXY, CURLOPT_STRING, false },
	{ "CURLOPT_PROXY_CAINFO", CURLOPT_PROXY_TLSAUTH_USERNAME, CURLOPT_STRING, false },
	{ "CURLOPT_PROXY_TLSAUTH_USERNAME", CURLOPT_PROXY_TLSAUTH_USERNAME, CURLOPT_STRING, false },
	{ "CURLOPT_PROXY_TLSAUTH_PASSWORD", CURLOPT_PROXY_TLSAUTH_PASSWORD, CURLOPT_STRING, false },
	{ "CURLOPT_PROXY_TLSAUTH_TYPE", CURLOPT_PROXY_TLSAUTH_TYPE, CURLOPT_STRING, false },
#endif
	{ NULL, 0, 0, false } /* Array null terminator */
};


/* Function signatures */
void _PG_init(void);
void _PG_fini(void);
static size_t http_writeback(void *contents, size_t size, size_t nmemb, void *userp);
static size_t http_readback(void *buffer, size_t size, size_t nitems, void *instream);

/* Global variables */
bool g_use_keepalive;
int g_timeout_msec;

CURL * g_http_handle = NULL;
pqsigfunc pgsql_interrupt_handler = NULL;
int http_interrupt_requested = 0;


static int
http_progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
#ifdef WIN32
	if (UNBLOCKED_SIGNAL_QUEUE())
	{
		pgwin32_dispatch_queued_signals();
	}
#endif
	/* elog(DEBUG3, "http_interrupt_requested = %d", http_interrupt_requested); */
	return http_interrupt_requested;
}


static void
http_interrupt_handler(int sig)
{
	/* Handle the signal here */
	elog(DEBUG2, "http_interrupt_handler: sig=%d", sig);
	http_interrupt_requested = sig;
	return;
}


/* Startup */
void _PG_init(void)
{
	DefineCustomBoolVariable("http.keepalive",
							 "reuse existing connections with keepalive",
							 NULL,
							 &g_use_keepalive,
							 false,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomIntVariable("http.timeout_msec",
							"request completion timeout in milliseconds",
							NULL,
							&g_timeout_msec,
							5000,
							0,
							INT_MAX,
							PGC_USERSET,
							GUC_NOT_IN_SAMPLE | GUC_UNIT_MS,
							NULL,
							NULL,
							NULL);

	/* Set up Curl! */
	curl_global_init(CURL_GLOBAL_ALL);

	/* Register our interrupt handler (http_handle_interrupt) */
	/* and store the existing one so we can call it when we're */
	/* through with our work */
	pgsql_interrupt_handler = pqsignal(SIGINT, http_interrupt_handler);
	http_interrupt_requested = 0;
}

/* Tear-down */
void _PG_fini(void)
{
	/* Re-register the original signal handler */
	pqsignal(SIGINT, pgsql_interrupt_handler);

	if (g_http_handle)
	{
		curl_easy_cleanup(g_http_handle);
		g_http_handle = NULL;
	}

	curl_global_cleanup();
	elog(NOTICE, "Goodbye from HTTP %s", HTTP_VERSION);
}

/**
* This function is passed into CURL as the CURLOPT_WRITEFUNCTION,
* this allows the  return values to be held in memory, in our case in a string.
*/
static size_t
http_writeback(void *contents, size_t size, size_t nmemb, void *userp)
{
	size_t realsize = size * nmemb;
	StringInfo si = (StringInfo)userp;
	appendBinaryStringInfo(si, (const char*)contents, (int)realsize);
	return realsize;
}

/**
* This function is passed into CURL as the CURLOPT_READFUNCTION,
* this allows the PUT operation to read the data it needs.
*/
static size_t
http_readback(void *buffer, size_t size, size_t nitems, void *instream)
{
	size_t reqsize = size * nitems;
	StringInfo si = (StringInfo)instream;
	size_t remaining = si->len - si->cursor;
	size_t readsize = reqsize < remaining ? reqsize : remaining;
	memcpy(buffer, si->data + si->cursor, readsize);
	si->cursor += readsize;
	return readsize;
}

static void
http_error(CURLcode err, const char *error_buffer)
{
	if ( strlen(error_buffer) > 0 )
		ereport(ERROR, (errmsg("%s", error_buffer)));
	else
		ereport(ERROR, (errmsg("%s", curl_easy_strerror(err))));
}

/* Utility macro to try a setopt and catch an error */
#define CURL_SETOPT(handle, opt, value) do { \
	err = curl_easy_setopt((handle), (opt), (value)); \
	if ( err != CURLE_OK ) \
	{ \
		http_error(err, http_error_buffer); \
		PG_RETURN_NULL(); \
	} \
	} while (0);


/**
*  Convert a request type string into the appropriate enumeration value.
*/
static http_method
request_type(const char *method)
{
	if ( strcasecmp(method, "GET") == 0 )
		return HTTP_GET;
	else if ( strcasecmp(method, "POST") == 0 )
		return HTTP_POST;
	else if ( strcasecmp(method, "PUT") == 0 )
		return HTTP_PUT;
	else if ( strcasecmp(method, "DELETE") == 0 )
		return HTTP_DELETE;
	else if ( strcasecmp(method, "HEAD") == 0 )
		return HTTP_HEAD;
	else
		return HTTP_GET;
}

/**
* Given a field name and value, output a http_header tuple.
*/
static Datum
header_tuple(TupleDesc header_tuple_desc, const char *field, const char *value)
{
	HeapTuple header_tuple;
	int ncolumns;
	Datum *header_values;
	bool *header_nulls;

	/* Prepare our return object */
	ncolumns = header_tuple_desc->natts;
	header_values = palloc0(sizeof(Datum)*ncolumns);
	header_nulls = palloc0(sizeof(bool)*ncolumns);

	header_values[HEADER_FIELD] = CStringGetTextDatum(field);
	header_nulls[HEADER_FIELD] = false;
	header_values[HEADER_VALUE] = CStringGetTextDatum(value);
	header_nulls[HEADER_VALUE] = false;

	/* Build up a tuple from values/nulls lists */
	header_tuple = heap_form_tuple(header_tuple_desc, header_values, header_nulls);
	return HeapTupleGetDatum(header_tuple);
}

/**
* Our own implementation of strcasestr.
*/
static char *http_strcasestr(const char *s, const char *find)
{
	char c, sc;
	size_t len;

	if ((c = *find++) != 0)
	{
		c = tolower((unsigned char)c);
		len = strlen(find);
		do
		{
			do
			{
				if ((sc = *s++) == 0)
				return (NULL);
			}
			while ((char)tolower((unsigned char)sc) != c);
		}
		while (strncasecmp(s, find, len) != 0);
		s--;
	}
	return ((char *)s);
}

/**
* Quick and dirty, remove all \r from a StringInfo.
*/
static void
string_info_remove_cr(StringInfo si)
{
	int i = 0, j = 0;
	while ( si->data[i] )
	{
		if ( si->data[i] != '\r' )
			si->data[j++] = si->data[i++];
		else
			i++;
	}
	si->data[j] = '\0';
	si->len -= i-j;
	return;
}

/**
* Add an array of http_header tuples into a Curl string list.
*/
static struct curl_slist *
header_array_to_slist(ArrayType *array, struct curl_slist *headers)
{
	ArrayIterator iterator;
	Datum value;
	bool isnull;

#if PG_VERSION_NUM >= 90500
	iterator = array_create_iterator(array, 0, NULL);
#else
	iterator = array_create_iterator(array, 0);
#endif

	while( array_iterate(iterator, &value, &isnull) )
	{
		HeapTupleHeader rec;
		HeapTupleData tuple;
		Oid tup_type;
		int32 tup_typmod, ncolumns;
		TupleDesc tup_desc;
		size_t tup_len;
		Datum *values;
		bool *nulls;

		/* Skip null array items */
		if ( isnull )
			continue;

		rec = DatumGetHeapTupleHeader(value);
		tup_type = HeapTupleHeaderGetTypeId(rec);
		tup_typmod = HeapTupleHeaderGetTypMod(rec);
		tup_len = HeapTupleHeaderGetDatumLength(rec);
		tup_desc = lookup_rowtype_tupdesc(tup_type, tup_typmod);
		ncolumns = tup_desc->natts;

		/* Prepare for values / nulls to hold the data */
		values = (Datum *) palloc0(ncolumns * sizeof(Datum));
		nulls = (bool *) palloc0(ncolumns * sizeof(bool));

		/* Build a temporary HeapTuple control structure */
		tuple.t_len = tup_len;
		ItemPointerSetInvalid(&(tuple.t_self));
		tuple.t_tableOid = InvalidOid;
		tuple.t_data = rec;

		/* Break down the tuple into values/nulls lists */
		heap_deform_tuple(&tuple, tup_desc, values, nulls);

		/* Convert the data into a header */
		/* TODO: Ensure the header list is unique? Or leave that to the */
		/* server to deal with. */
		if ( ! nulls[HEADER_FIELD] )
		{
			char buffer[1024];
			char *header_val;
			char *header_fld = TextDatumGetCString(values[HEADER_FIELD]);

			/* Don't process "content-type" in the optional headers */
			if ( strlen(header_fld) <= 0 || strncasecmp(header_fld, "Content-Type", 12) == 0 )
			{
				elog(NOTICE, "'Content-Type' is not supported as an optional header");
				continue;
			}

			if ( nulls[HEADER_VALUE] )
				header_val = pstrdup("");
			else
				header_val = TextDatumGetCString(values[HEADER_VALUE]);

			snprintf(buffer, sizeof(buffer), "%s: %s", header_fld, header_val);
			elog(DEBUG2, "pgsql-http: optional request header '%s'", buffer);
			headers = curl_slist_append(headers, buffer);
			pfree(header_fld);
			pfree(header_val);
		}

		/* Free all the temporary structures */
		ReleaseTupleDesc(tup_desc);
		pfree(values);
		pfree(nulls);
	}
	array_free_iterator(iterator);

	return headers;
}

/**
* Look up the tuple description for a extension-defined type,
* avoiding the pitfalls of using relations that are not part
* of the extension, but share the same name as the relation
* of interest.
*/
static TupleDesc typname_get_tupledesc(const char *extname, const char *typname)
{
	Oid extoid = get_extension_oid(extname, true);
	ListCell *l;

	if ( ! OidIsValid(extoid) )
		elog(ERROR, "could not lookup '%s' extension oid", extname);

	foreach(l, fetch_search_path(true))
	{
		Oid typnamespace = lfirst_oid(l);
		Oid typoid = GetSysCacheOid2(TYPENAMENSP,
		                PointerGetDatum(typname),
		                ObjectIdGetDatum(typnamespace));

		if ( OidIsValid(typoid) )
		{
			// Oid typ_oid = get_typ_typrelid(rel_oid);
			Oid relextoid = getExtensionOfObject(TypeRelationId, typoid);
			if ( relextoid == extoid )
			{
				return TypeGetTupleDesc(typoid, NIL);
			}
		}
	}

	elog(ERROR, "could not lookup '%s' tuple desc", typname);
}

/**
* Convert a string of headers separated by newlines/CRs into an
* array of http_header tuples.
*/
static ArrayType *
header_string_to_array(StringInfo si)
{
	/* Array building */
	int arr_nelems = 0;
	int arr_elems_size = 8;
	Datum *arr_elems = palloc0(arr_elems_size*sizeof(Datum));
	Oid elem_type;
	int16 elem_len;
	bool elem_byval;
	char elem_align;

	/* Header handling */
	TupleDesc header_tuple_desc = NULL;

	/* Regex support */
	const char *regex_pattern = "^([^ \t\r\n\v\f]+): ?([^ \t\r\n\v\f]+.*)$";
	regex_t regex;
	regmatch_t pmatch[3];
	int reti;
	static int rvsz = 256;
	char rv1[rvsz];
	char rv2[rvsz];

	/* Compile the regular expression */
	reti = regcomp(&regex, regex_pattern, REG_ICASE | REG_EXTENDED | REG_NEWLINE );
	if ( reti )
		elog(ERROR, "Unable to compile regex pattern '%s'", regex_pattern);

	/* Lookup the tuple defn */
	header_tuple_desc = typname_get_tupledesc("http", "http_header");

	/* Prepare array building metadata */
	elem_type = header_tuple_desc->tdtypeid;
	get_typlenbyvalalign(elem_type, &elem_len, &elem_byval, &elem_align);

	/* Loop through string, matching regex pattern */
	si->cursor = 0;
	while ( ! regexec(&regex, si->data+si->cursor, 3, pmatch, 0) )
	{
		/* Read the regex match results */
		int eo0 = pmatch[0].rm_eo;
		int so1 = pmatch[1].rm_so;
		int eo1 = pmatch[1].rm_eo;
		int so2 = pmatch[2].rm_so;
		int eo2 = pmatch[2].rm_eo;

		/* Copy the matched portions out of the string */
		memcpy(rv1, si->data+si->cursor+so1, eo1-so1 < rvsz ? eo1-so1 : rvsz);
		rv1[eo1-so1] = '\0';
		memcpy(rv2, si->data+si->cursor+so2, eo2-so2 < rvsz ? eo2-so2 : rvsz);
		rv2[eo2-so2] = '\0';

		/* Move forward for next match */
		si->cursor += eo0;

		/* Increase elements array size if necessary */
		if ( arr_nelems >= arr_elems_size )
		{
			arr_elems_size *= 2;
			arr_elems = repalloc(arr_elems, arr_elems_size*sizeof(Datum));
		}
		arr_elems[arr_nelems] = header_tuple(header_tuple_desc, rv1, rv2);
		arr_nelems++;
	}

	ReleaseTupleDesc(header_tuple_desc);
	return construct_array(arr_elems, arr_nelems, elem_type, elem_len, elem_byval, elem_align);
}

/* Check/log version info */
static void
http_check_curl_version(const curl_version_info_data *version_info)
{
	elog(DEBUG2, "pgsql-http: curl version %s", version_info->version);
	elog(DEBUG2, "pgsql-http: curl version number 0x%x", version_info->version_num);
	elog(DEBUG2, "pgsql-http: ssl version %s", version_info->ssl_version);

	if ( version_info->version_num < CURL_MIN_VERSION )
	{
		elog(ERROR, "pgsql-http requires Curl version 0.7.20 or higher");
	}
}

/* Check/create the global CURL* handle */
static CURL*
http_get_handle()
{
	CURL *handle = g_http_handle;

	/* Initialize the global handle if needed */
	if (!handle)
	{
		handle = curl_easy_init();
		/* Always want a default fast (1 second) connection timeout */
		/* User can over-ride with http_set_curlopt() if they wish */
		curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 1);
	}

	if (!handle)
		ereport(ERROR, (errmsg("Unable to initialize CURL")));

	g_http_handle = handle;
	return handle;
}


/**
* User-defined Curl option reset.
*/
Datum http_reset_curlopt(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(http_reset_curlopt);
Datum http_reset_curlopt(PG_FUNCTION_ARGS)
{
	/* Set up global HTTP handle */
	g_http_handle = http_get_handle();
	curl_easy_reset(g_http_handle);
	PG_RETURN_BOOL(true);
}

/**
* User-defined Curl option handling.
*/
Datum http_set_curlopt(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(http_set_curlopt);
Datum http_set_curlopt(PG_FUNCTION_ARGS)
{
	int i = 0;
	char *curlopt, *value;
	text *curlopt_txt, *value_txt;

	/* Version check */
	http_check_curl_version(curl_version_info(CURLVERSION_NOW));

	/* We cannot handle null arguments */
	if ( PG_ARGISNULL(0) || PG_ARGISNULL(1) )
		PG_RETURN_BOOL(false);

	/* Set up global HTTP handle */
	g_http_handle = http_get_handle();

	/* Read arguments */
	curlopt_txt = PG_GETARG_TEXT_P(0);
	value_txt = PG_GETARG_TEXT_P(1);
	curlopt = text_to_cstring(curlopt_txt);
	value = text_to_cstring(value_txt);

	while (1)
	{
		http_curlopt opt = settable_curlopts[i++];
		if (!opt.curlopt_str) /* Terminate at end of array */
			break;
		if (strcasecmp(opt.curlopt_str, curlopt) == 0)
		{
			CURLcode err;
			char http_error_buffer[CURL_ERROR_SIZE];

			/* Argument is a string */
			if (opt.curlopt_type == CURLOPT_STRING)
			{
				err = curl_easy_setopt(g_http_handle, opt.curlopt, value);
				elog(DEBUG2, "pgsql-http: set '%s' to value '%s', return value = %d", opt.curlopt_str, value, err);
			}
			/* Argument is a long */
			else if (opt.curlopt_type == CURLOPT_LONG)
			{
				long value_long = strtol(value, NULL, 10);
				if ( errno == EINVAL || errno == ERANGE )
					elog(ERROR, "invalid integer provided for '%s'", opt.curlopt_str);

				err = curl_easy_setopt(g_http_handle, opt.curlopt, value_long);
				elog(DEBUG2, "pgsql-http: set '%s' to value '%ld', return value = %d", opt.curlopt_str, value_long, err);
			}
			else
			{
				elog(ERROR, "invalid curlopt_type");
			}

			if ( err != CURLE_OK )
			{
				http_error(err, http_error_buffer);
				PG_RETURN_BOOL(false);
			}
			PG_RETURN_BOOL(true);
		}
	}
	elog(ERROR, "curl option '%s' is not available for run-time configuration", curlopt);
	PG_RETURN_BOOL(false);
}


/**
* Master HTTP request function, takes in an http_request tuple and outputs
* an http_response tuple.
*/
Datum http_request(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(http_request);
Datum http_request(PG_FUNCTION_ARGS)
{
	/* Input */
	HeapTupleHeader rec;
	HeapTupleData tuple;
	Oid tup_type;
	int32 tup_typmod;
	TupleDesc tup_desc;
	int ncolumns;
	Datum *values;
	bool *nulls;

	char *uri;
	char *method_str;
	http_method method;

	/* Processing */
	CURLcode err;
	char http_error_buffer[CURL_ERROR_SIZE];

	struct curl_slist *headers = NULL;
	StringInfoData si_data;
	StringInfoData si_headers;
	StringInfoData si_read;

	int http_return;
	long long_status;
	int status;
	char *content_type = NULL;
	int content_charset = -1;

	/* Output */
	HeapTuple tuple_out;

	/* Set up the interrupt flag */
	http_interrupt_requested = 0;

	/* Version check */
	http_check_curl_version(curl_version_info(CURLVERSION_NOW));

	/* We cannot handle a null request */
	if ( ! PG_ARGISNULL(0) )
		rec = PG_GETARG_HEAPTUPLEHEADER(0);
	else
	{
		elog(ERROR, "An http_request must be provided");
		PG_RETURN_NULL();
	}

	/*************************************************************************
	* Build and run a curl request from the http_request argument
	*************************************************************************/

	/* Zero out static memory */
	memset(http_error_buffer, 0, sizeof(http_error_buffer));

	/* Extract type info from the tuple itself */
	tup_type = HeapTupleHeaderGetTypeId(rec);
	tup_typmod = HeapTupleHeaderGetTypMod(rec);
	tup_desc = lookup_rowtype_tupdesc(tup_type, tup_typmod);
	ncolumns = tup_desc->natts;

	/* Build a temporary HeapTuple control structure */
	tuple.t_len = HeapTupleHeaderGetDatumLength(rec);
	ItemPointerSetInvalid(&(tuple.t_self));
	tuple.t_tableOid = InvalidOid;
	tuple.t_data = rec;

	/* Prepare for values / nulls */
	values = (Datum *) palloc0(ncolumns * sizeof(Datum));
	nulls = (bool *) palloc0(ncolumns * sizeof(bool));

	/* Break down the tuple into values/nulls lists */
	heap_deform_tuple(&tuple, tup_desc, values, nulls);

	/* Read the URI */
	if ( nulls[REQ_URI] )
		elog(ERROR, "http_request.uri is NULL");
	uri = TextDatumGetCString(values[REQ_URI]);

	/* Read the method */
	if ( nulls[REQ_METHOD] )
		elog(ERROR, "http_request.method is NULL");
	method_str = TextDatumGetCString(values[REQ_METHOD]);
	method = request_type(method_str);
	elog(DEBUG2, "pgsql-http: method '%s'", method_str);
	pfree(method_str);

	/* Set up global HTTP handle */
	g_http_handle = http_get_handle();

	/* Set the target URL */
	CURL_SETOPT(g_http_handle, CURLOPT_URL, uri);

	/* Set the user agent */
	CURL_SETOPT(g_http_handle, CURLOPT_USERAGENT, PG_VERSION_STR);

	/* Restrict to just http/https. Leaving unrestricted */
	/* opens possibility of users requesting file:/// urls */
	/* locally */
	CURL_SETOPT(g_http_handle, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);

	if ( g_use_keepalive )
	{
		/* Keep sockets held open */
		CURL_SETOPT(g_http_handle, CURLOPT_FORBID_REUSE, 0);
	}
	else
	{
		/* Keep sockets from being held open */
		CURL_SETOPT(g_http_handle, CURLOPT_FORBID_REUSE, 1);
	}

	/* Set up the error buffer */
	CURL_SETOPT(g_http_handle, CURLOPT_ERRORBUFFER, http_error_buffer);

	/* Set up the write-back function */
	CURL_SETOPT(g_http_handle, CURLOPT_WRITEFUNCTION, http_writeback);

	/* Set up the write-back buffer */
	initStringInfo(&si_data);
	initStringInfo(&si_headers);
	CURL_SETOPT(g_http_handle, CURLOPT_WRITEDATA, (void*)(&si_data));
	CURL_SETOPT(g_http_handle, CURLOPT_WRITEHEADER, (void*)(&si_headers));

	/* Connect the progress callback for interrupt support */
	CURL_SETOPT(g_http_handle, CURLOPT_XFERINFOFUNCTION, http_progress_callback);
	CURL_SETOPT(g_http_handle, CURLOPT_NOPROGRESS, 0);

	/* Set up the HTTP timeout */
	CURL_SETOPT(g_http_handle, CURLOPT_TIMEOUT_MS, g_timeout_msec);

	/* Set the HTTP content encoding to all curl supports */
	CURL_SETOPT(g_http_handle, CURLOPT_ACCEPT_ENCODING, "");

	if ( method != HTTP_HEAD )
	{
		/* Follow redirects, as many as 5 */
		CURL_SETOPT(g_http_handle, CURLOPT_FOLLOWLOCATION, 1);
		CURL_SETOPT(g_http_handle, CURLOPT_MAXREDIRS, 5);
	}

	if ( g_use_keepalive )
	{
		/* Add a keep alive option to the headers to reuse network sockets */
		headers = curl_slist_append(headers, "Connection: Keep-Alive");
	}
	else
	{
		/* Add a close option to the headers to avoid open network sockets */
		headers = curl_slist_append(headers, "Connection: close");
	}

	/* Let our charset preference be known */
	headers = curl_slist_append(headers, "Charsets: utf-8");

	/* Handle optional headers */
	if ( ! nulls[REQ_HEADERS] )
	{
		ArrayType *array = DatumGetArrayTypeP(values[REQ_HEADERS]);
		headers = header_array_to_slist(array, headers);
	}

	/* Specific handling for methods that send a content payload */
	if ( method == HTTP_POST || method == HTTP_PUT )
	{
		text *content_text;
		long content_size;
		char *content_type;
		char buffer[1024];

		/* Read the content type */
		if ( nulls[REQ_CONTENT_TYPE] || ! values[REQ_CONTENT_TYPE] )
			elog(ERROR, "http_request.content_type is NULL");
		content_type = TextDatumGetCString(values[REQ_CONTENT_TYPE]);

		/* Add content type to the headers */
		snprintf(buffer, sizeof(buffer), "Content-Type: %s", content_type);
		headers = curl_slist_append(headers, buffer);
		pfree(content_type);

		/* Read the content */
		if ( nulls[REQ_CONTENT] || ! values[REQ_CONTENT] )
			elog(ERROR, "http_request.content is NULL");
		content_text = DatumGetTextP(values[REQ_CONTENT]);
		content_size = VARSIZE(content_text) - VARHDRSZ;

		if ( method == HTTP_POST )
		{
			/* Add the content to the payload */
			CURL_SETOPT(g_http_handle, CURLOPT_POST, 1);
			CURL_SETOPT(g_http_handle, CURLOPT_POSTFIELDS, text_to_cstring(content_text));
		}
		else if ( method == HTTP_PUT )
		{
			initStringInfo(&si_read);
			appendBinaryStringInfo(&si_read, VARDATA(content_text), content_size);
			CURL_SETOPT(g_http_handle, CURLOPT_UPLOAD, 1);
			CURL_SETOPT(g_http_handle, CURLOPT_READFUNCTION, http_readback);
			CURL_SETOPT(g_http_handle, CURLOPT_READDATA, &si_read);
			CURL_SETOPT(g_http_handle, CURLOPT_INFILESIZE, content_size);
		}
		else
		{
			/* Never get here */
			elog(ERROR, "illegal HTTP method");
		}
	}
	else if ( method == HTTP_DELETE )
	{
		CURL_SETOPT(g_http_handle, CURLOPT_CUSTOMREQUEST, "DELETE");
	}
	else if ( method == HTTP_HEAD )
	{
		CURL_SETOPT(g_http_handle, CURLOPT_NOBODY, 1);
	}

	/* Set the headers */
	CURL_SETOPT(g_http_handle, CURLOPT_HTTPHEADER, headers);

	/*************************************************************************
	* PERFORM THE REQUEST!
	**************************************************************************/
	http_return = curl_easy_perform(g_http_handle);
	elog(DEBUG2, "pgsql-http: queried '%s'", uri);
	elog(DEBUG2, "pgsql-http: http_return '%d'", http_return);

	/* Clean up some input things we don't need anymore */
	ReleaseTupleDesc(tup_desc);
	pfree(values);
	pfree(nulls);

	/*************************************************************************
	* Create an http_response object from the curl results
	*************************************************************************/

	/* Write out an error on failure */
	if ( http_return != CURLE_OK )
	{
		curl_slist_free_all(headers);
		curl_easy_cleanup(g_http_handle);
		g_http_handle = NULL;

		/* Propogate signal to the next handler */
		if (http_return == CURLE_ABORTED_BY_CALLBACK &&
			pgsql_interrupt_handler &&
			http_interrupt_requested)
		{
			elog(DEBUG2, "calling pgsql_interrupt_handler");
			(*pgsql_interrupt_handler)(http_interrupt_requested);
			http_interrupt_requested = 0;
			elog(ERROR, "HTTP request cancelled");
		}

		http_error(http_return, http_error_buffer);
	}

	/* Read the metadata from the handle directly */
	if ( (CURLE_OK != curl_easy_getinfo(g_http_handle, CURLINFO_RESPONSE_CODE, &long_status)) ||
		 (CURLE_OK != curl_easy_getinfo(g_http_handle, CURLINFO_CONTENT_TYPE, &content_type)) )
	{
		curl_slist_free_all(headers);
		curl_easy_cleanup(g_http_handle);
		g_http_handle = NULL;
		ereport(ERROR, (errmsg("CURL: Error in curl_easy_getinfo")));
	}

	/* Prepare our return object */
	tup_desc = RelationNameGetTupleDesc("http_response");
	ncolumns = tup_desc->natts;
	values = palloc0(sizeof(Datum)*ncolumns);
	nulls = palloc0(sizeof(bool)*ncolumns);

	/* Status code */
	status = long_status;
	values[RESP_STATUS] = Int32GetDatum(status);
	nulls[RESP_STATUS] = false;

	/* Content type */
	if ( content_type )
	{
		List *ctl;
		ListCell *lc;

		values[RESP_CONTENT_TYPE] = CStringGetTextDatum(content_type);
		nulls[RESP_CONTENT_TYPE] = false;

		/* Read the character set name out of the content type */
		/* if there is one in there */
		/* text/html; charset=iso-8859-1 */
		if ( SplitIdentifierString(pstrdup(content_type), ';', &ctl) )
		{
			foreach(lc, ctl)
			{
				/* charset=iso-8859-1 */
				const char *param = (const char *) lfirst(lc);
				const char *paramtype = "charset=";
				if ( http_strcasestr(param, paramtype) )
				{
					/* iso-8859-1 */
					const char *charset = param + strlen(paramtype);
					content_charset = pg_char_to_encoding(charset);
					break;
				}
			}
		}
	}
	else
	{
		values[RESP_CONTENT_TYPE] = (Datum)0;
		nulls[RESP_CONTENT_TYPE] = true;
	}

	/* Headers array */
	if ( si_headers.len )
	{
		/* Strip the carriage-returns, because who cares? */
		string_info_remove_cr(&si_headers);
		values[RESP_HEADERS] = PointerGetDatum(header_string_to_array(&si_headers));
		nulls[RESP_HEADERS] = false;
	}
	else
	{
		values[RESP_HEADERS] = (Datum)0;
		nulls[RESP_HEADERS] = true;
	}

	/* Content */
	if ( si_data.len )
	{
		char *content_str;
		size_t content_len;
		elog(DEBUG2, "pgsql-http: content_charset = %d", content_charset);

		/* Apply character transcoding if necessary */
		if ( content_charset < 0 )
		{
			content_str = si_data.data;
			content_len = si_data.len;
		}
		else
		{
			content_str = pg_any_to_server(si_data.data, si_data.len, content_charset);
			content_len = strlen(content_str);
		}

		values[RESP_CONTENT] = PointerGetDatum(cstring_to_text_with_len(content_str, content_len));
		nulls[RESP_CONTENT] = false;
	}
	else
	{
		values[RESP_CONTENT] = (Datum)0;
		nulls[RESP_CONTENT] = true;
	}

	/* Build up a tuple from values/nulls lists */
	tuple_out = heap_form_tuple(tup_desc, values, nulls);

	/* Clean up */
	ReleaseTupleDesc(tup_desc);
	if ( !g_use_keepalive )
	{
		curl_easy_cleanup(g_http_handle);
		g_http_handle = NULL;
	}
	curl_slist_free_all(headers);
	pfree(si_headers.data);
	pfree(si_data.data);
	pfree(values);
	pfree(nulls);

	/* Return */
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple_out));
}




/* URL Encode Escape Chars */
/* 48-57 (0-9) 65-90 (A-Z) 97-122 (a-z) 95 (_) 45 (-) */

static int chars_to_not_encode[] = {
	0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,1,0,0,1,1,
	1,1,1,1,1,1,1,1,0,0,
	0,0,0,0,0,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,
	1,0,0,0,0,1,0,1,1,1,
	1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,
	1,1,1,0,0,0,0,0
};



/**
* Utility function for users building URL encoded requests, applies
* standard URL encoding to an input string.
*/
Datum urlencode(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(urlencode);
Datum urlencode(PG_FUNCTION_ARGS)
{
	text *txt = PG_GETARG_TEXT_P(0); /* Declare strict, so no test for NULL input */
	size_t txt_size = VARSIZE(txt) - VARHDRSZ;
	char *str_in, *str_out, *ptr;
	int i, rv;

	/* Point into the string */
	str_in = VARDATA(txt);

	/* Prepare the output string */
	str_out = palloc0(txt_size * 4);
	ptr = str_out;

	for ( i = 0; i < txt_size; i++ )
	{
		unsigned char c = str_in[i];

		/* Break on NULL */
		if ( c == '\0' )
			break;

		/* Replace ' ' with '+' */
		if ( c  == ' ' )
		{
			*ptr = '+';
			ptr++;
			continue;
		}

		/* Pass basic characters through */
		if ( (c < 127) && chars_to_not_encode[(int)(str_in[i])] )
		{
			*ptr = str_in[i];
			ptr++;
			continue;
		}

		/* Encode the remaining chars */
		rv = snprintf(ptr, 4, "%%%02X", c);
		if ( rv < 0 )
			PG_RETURN_NULL();

		/* Move pointer forward */
		ptr += 3;
	}
	*ptr = '\0';

	PG_RETURN_TEXT_P(cstring_to_text(str_out));
}

// Local Variables:
// mode: C++
// tab-width: 4
// c-basic-offset: 4
// indent-tabs-mode: t
// End:
