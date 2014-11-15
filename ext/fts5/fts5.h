/*
** 2014 May 31
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** Interfaces to extend FTS5. Using the interfaces defined in this file, 
** FTS5 may be extended with:
**
**     * custom tokenizers, and
**     * custom auxiliary functions.
*/


#ifndef _FTS5_H
#define _FTS5_H

#include "sqlite3.h"

/*************************************************************************
** CUSTOM AUXILIARY FUNCTIONS
**
** Virtual table implementations may overload SQL functions by implementing
** the sqlite3_module.xFindFunction() method.
*/

typedef struct Fts5ExtensionApi Fts5ExtensionApi;
typedef struct Fts5Context Fts5Context;

typedef void (*fts5_extension_function)(
  const Fts5ExtensionApi *pApi,   /* API offered by current FTS version */
  Fts5Context *pFts,              /* First arg to pass to pApi functions */
  sqlite3_context *pCtx,          /* Context for returning result/error */
  int nVal,                       /* Number of values in apVal[] array */
  sqlite3_value **apVal           /* Array of trailing arguments */
);

/*
** EXTENSION API FUNCTIONS
**
** xUserData(pFts):
**   Return a copy of the context pointer the extension function was 
**   registered with.
**
**
** xColumnTotalSize(pFts, iCol, pnToken):
**   Returns the total number of tokens in column iCol, considering all
**   rows in the FTS5 table.
**
**
** xColumnCount:
**   Returns the number of columns in the FTS5 table.
**
** xColumnSize:
**   Reports the size in tokens of a column value from the current row.
**
** xColumnText:
**   Reports the size in tokens of a column value from the current row.
**
** xPhraseCount:
**   Returns the number of phrases in the current query expression.
**
** xPhraseSize:
**   Returns the number of tokens in phrase iPhrase of the query. Phrases
**   are numbered starting from zero.
**
** xRowid:
**   Returns the rowid of the current row.
**
** xPoslist:
**   Iterate through instances of phrase iPhrase in the current row. 
**
**   At EOF, a non-zero value is returned and output variable iPos set to -1.
**
** xTokenize:
**   Tokenize text using the tokenizer belonging to the FTS5 table.
**
**
** xQueryPhrase(pFts5, iPhrase, pUserData, xCallback):
**   This API function is used to query the FTS table for phrase iPhrase
**   of the current query. Specifically, a query equivalent to:
**
**       ... FROM ftstable WHERE ftstable MATCH $p ORDER BY DESC
**
**   with $p set to a phrase equivalent to the phrase iPhrase of the
**   current query is executed. For each row visited, the callback function
**   passed as the fourth argument is invoked. The context and API objects 
**   passed to the callback function may be used to access the properties of
**   each matched row. Invoking Api.xUserData() returns a copy of the pointer
**   passed as the third argument to pUserData.
**
**   If the callback function returns any value other than SQLITE_OK, the
**   query is abandoned and the xQueryPhrase function returns immediately.
**   If the returned value is SQLITE_DONE, xQueryPhrase returns SQLITE_OK.
**   Otherwise, the error code is propagated upwards.
**
**   If the query runs to completion without incident, SQLITE_OK is returned.
**   Or, if some error occurs before the query completes or is aborted by
**   the callback, an SQLite error code is returned.
**
**
** xSetAuxdata(pFts5, pAux, xDelete)
**
**   Save the pointer passed as the second argument as the extension functions 
**   "auxiliary data". The pointer may then be retrieved by the current or any
**   future invocation of the same fts5 extension function made as part of
**   of the same MATCH query using the xGetAuxdata() API.
**
**   Each extension function is allocated a single auxiliary data slot per
**   query. If the extension function is invoked more than once by the SQL
**   query, then all invocations share a single auxiliary data context.
**
**   If there is already an auxiliary data pointer when this function is
**   invoked, then it is replaced by the new pointer. If an xDelete callback
**   was specified along with the original pointer, it is invoked at this
**   point.
**
**   The xDelete callback, if one is specified, is also invoked on the
**   auxiliary data pointer after the FTS5 query has finished.
**
**
** xGetAuxdata(pFts5, bClear)
**
**   Returns the current auxiliary data pointer for the fts5 extension 
**   function. See the xSetAuxdata() method for details.
**
**   If the bClear argument is non-zero, then the auxiliary data is cleared
**   (set to NULL) before this function returns. In this case the xDelete,
**   if any, is not invoked.
**
**
** xRowCount(pFts5, pnRow)
**
**   This function is used to retrieve the total number of rows in the table.
**   In other words, the same value that would be returned by:
**
**        SELECT count(*) FROM ftstable;
*/
struct Fts5ExtensionApi {
  int iVersion;                   /* Currently always set to 1 */

  void *(*xUserData)(Fts5Context*);

  int (*xColumnCount)(Fts5Context*);
  int (*xRowCount)(Fts5Context*, sqlite3_int64 *pnRow);
  int (*xColumnTotalSize)(Fts5Context*, int iCol, sqlite3_int64 *pnToken);

  int (*xTokenize)(Fts5Context*, 
    const char *pText, int nText, /* Text to tokenize */
    void *pCtx,                   /* Context passed to xToken() */
    int (*xToken)(void*, const char*, int, int, int, int)    /* Callback */
  );

  int (*xPhraseCount)(Fts5Context*);
  int (*xPhraseSize)(Fts5Context*, int iPhrase);

  sqlite3_int64 (*xRowid)(Fts5Context*);
  int (*xColumnText)(Fts5Context*, int iCol, const char **pz, int *pn);
  int (*xColumnSize)(Fts5Context*, int iCol, int *pnToken);
  int (*xPoslist)(Fts5Context*, int iPhrase, int *pi, sqlite3_int64 *piPos);

  int (*xQueryPhrase)(Fts5Context*, int iPhrase, void *pUserData,
    int(*)(const Fts5ExtensionApi*,Fts5Context*,void*)
  );
  int (*xSetAuxdata)(Fts5Context*, void *pAux, void(*xDelete)(void*));
  void *(*xGetAuxdata)(Fts5Context*, int bClear);
};

#define FTS5_POS2COLUMN(iPos) (int)(iPos >> 32)
#define FTS5_POS2OFFSET(iPos) (int)(iPos & 0xFFFFFFFF)

/* 
** CUSTOM AUXILIARY FUNCTIONS
*************************************************************************/

/*************************************************************************
** CUSTOM TOKENIZERS
**
** Applications may also register custom tokenizer types. A tokenizer 
** is registered by providing fts5 with a populated instance of the 
** following structure. The structure methods are expected to function
** as follows:
**
** xCreate:
**   This function is used to allocate and inititalize a tokenizer instance.
**   A tokenizer instance is required to actually tokenize text.
**
**   The first argument passed to this function is a copy of the (void*)
**   pointer provided by the application when the fts5_tokenizer object
**   was registered with SQLite. The second and third arguments are an
**   array of nul-terminated strings containing the tokenizer arguments,
**   if any, specified as part of the CREATE VIRTUAL TABLE statement used
**   to create the fts5 table.
**
**   The final argument is an output variable. If successful, (*ppOut) 
**   should be set to point to the new tokenizer handle and SQLITE_OK
**   returned. If an error occurs, some value other than SQLITE_OK should
**   be returned. In this case, fts5 assumes that the final value of *ppOut 
**   is undefined.
**
** xDelete:
**   This function is invoked to delete a tokenizer handle previously
**   allocated using xCreate(). Fts5 guarantees that this function will
**   be invoked exactly once for each successful call to xCreate().
**
** xTokenize:
**   This function is expected to tokenize the nText byte string indicated 
**   by argument pText. pText may not be nul-terminated. The first argument
**   passed to this function is a pointer to an Fts5Tokenizer object returned 
**   by an earlier call to xCreate().
**
**   For each token in the input string, the supplied callback xToken() must
**   be invoked. The first argument to it should be a copy of the pointer
**   passed as the second argument to xTokenize(). The next two arguments
**   are a pointer to a buffer containing the token text, and the size of
**   the token in bytes. The 4th and 5th arguments are the byte offsets of
**   the first byte of and first byte immediately following the text from 
**   which the token is derived within the input. The final argument is the
**   token position - the total number of tokens that appear before this one 
**   in the input buffer.
**
**   The xToken() callback must be invoked with non-decreasing values of
**   the iPos parameter.
**
**   If an xToken() callback returns any value other than SQLITE_OK, then
**   the tokenization should be abandoned and the xTokenize() method should
**   immediately return a copy of the xToken() return value. Or, if the
**   input buffer is exhausted, xTokenize() should return SQLITE_OK. Finally,
**   if an error occurs with the xTokenize() implementation itself, it
**   may abandon the tokenization and return any error code other than
**   SQLITE_OK or SQLITE_DONE.
**
*/
typedef struct fts5_tokenizer fts5_tokenizer;
typedef struct Fts5Tokenizer Fts5Tokenizer;

struct fts5_tokenizer {
  int (*xCreate)(void*, const char **azArg, int nArg, Fts5Tokenizer **ppOut);
  void (*xDelete)(Fts5Tokenizer*);
  int (*xTokenize)(Fts5Tokenizer*, 
      void *pCtx,
      const char *pText, int nText, 
      int (*xToken)(
        void *pCtx,         /* Copy of 2nd argument to xTokenize() */
        const char *pToken, /* Pointer to buffer containing token */
        int nToken,         /* Size of token in bytes */
        int iStart,         /* Byte offset of token within input text */
        int iEnd,           /* Byte offset of end of token within input text */
        int iPos            /* Position of token in input (first token is 0) */
      )
  );
};

/*
** END OF CUSTOM TOKENIZERS
*************************************************************************/

/*************************************************************************
** FTS5 EXTENSION REGISTRATION API
*/
typedef struct fts5_api fts5_api;
struct fts5_api {
  int iVersion;                   /* Currently always set to 1 */

  /* Create a new tokenizer */
  int (*xCreateTokenizer)(
    fts5_api *pApi,
    const char *zName,
    void *pContext,
    fts5_tokenizer *pTokenizer,
    void (*xDestroy)(void*)
  );

  /* Find an existing tokenizer */
  int (*xFindTokenizer)(
    fts5_api *pApi,
    const char *zName,
    fts5_tokenizer *pTokenizer
  );

  /* Create a new auxiliary function */
  int (*xCreateFunction)(
    fts5_api *pApi,
    const char *zName,
    void *pContext,
    fts5_extension_function xFunction,
    void (*xDestroy)(void*)
  );
};

/*
** END OF REGISTRATION API
*************************************************************************/

#endif /* _FTS5_H */

