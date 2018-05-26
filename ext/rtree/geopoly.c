/*
** 2018-05-25
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
** This file implements an alternative R-Tree virtual table that
** uses polygons to express the boundaries of 2-dimensional objects.
**
** This file is #include-ed onto the end of "rtree.c" so that it has
** access to all of the R-Tree internals.
*/
#include <stdlib.h>

/* Enable -DGEOPOLY_ENABLE_DEBUG for debugging facilities */
#ifdef GEOPOLY_ENABLE_DEBUG
  static int geo_debug = 0;
# define GEODEBUG(X) if(geo_debug)printf X
#else
# define GEODEBUG(X)
#endif

#ifndef JSON_NULL   /* The following stuff repeats things found in json1 */
/*
** Versions of isspace(), isalnum() and isdigit() to which it is safe
** to pass signed char values.
*/
#ifdef sqlite3Isdigit
   /* Use the SQLite core versions if this routine is part of the
   ** SQLite amalgamation */
#  define safe_isdigit(x)  sqlite3Isdigit(x)
#  define safe_isalnum(x)  sqlite3Isalnum(x)
#  define safe_isxdigit(x) sqlite3Isxdigit(x)
#else
   /* Use the standard library for separate compilation */
#include <ctype.h>  /* amalgamator: keep */
#  define safe_isdigit(x)  isdigit((unsigned char)(x))
#  define safe_isalnum(x)  isalnum((unsigned char)(x))
#  define safe_isxdigit(x) isxdigit((unsigned char)(x))
#endif

/*
** Growing our own isspace() routine this way is twice as fast as
** the library isspace() function.
*/
static const char geopolyIsSpace[] = {
  0, 0, 0, 0, 0, 0, 0, 0,     0, 1, 1, 0, 0, 1, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,     0, 0, 0, 0, 0, 0, 0, 0,
  1, 0, 0, 0, 0, 0, 0, 0,     0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,     0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,     0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,     0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,     0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,     0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,     0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,     0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,     0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,     0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,     0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,     0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,     0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,     0, 0, 0, 0, 0, 0, 0, 0,
};
#define safe_isspace(x) (geopolyIsSpace[(unsigned char)x])
#endif /* JSON NULL - back to original code */

/* Compiler and version */
#ifndef GCC_VERSION
#if defined(__GNUC__) && !defined(SQLITE_DISABLE_INTRINSIC)
# define GCC_VERSION (__GNUC__*1000000+__GNUC_MINOR__*1000+__GNUC_PATCHLEVEL__)
#else
# define GCC_VERSION 0
#endif
#endif
#ifndef MSVC_VERSION
#if defined(_MSC_VER) && !defined(SQLITE_DISABLE_INTRINSIC)
# define MSVC_VERSION _MSC_VER
#else
# define MSVC_VERSION 0
#endif
#endif

/* Datatype for coordinates
*/
typedef float GeoCoord;

/*
** Internal representation of a polygon.
**
** The polygon consists of a sequence of vertexes.  There is a line
** segment between each pair of vertexes, and one final segment from
** the last vertex back to the first.  (This differs from the GeoJSON
** standard in which the final vertex is a repeat of the first.)
**
** The polygon follows the right-hand rule.  The area to the right of
** each segment is "outside" and the area to the left is "inside".
**
** The on-disk representation consists of a 4-byte header followed by
** the values.  The 4-byte header is:
**
**      encoding    (1 byte)   0=big-endian, 1=little-endian
**      nvertex     (3 bytes)  Number of vertexes as a big-endian integer
*/
typedef struct GeoPoly GeoPoly;
struct GeoPoly {
  int nVertex;          /* Number of vertexes */
  unsigned char hdr[4]; /* Header for on-disk representation */
  GeoCoord a[2];    /* 2*nVertex values. X (longitude) first, then Y */
};

/*
** State of a parse of a GeoJSON input.
*/
typedef struct GeoParse GeoParse;
struct GeoParse {
  const unsigned char *z;   /* Unparsed input */
  int nVertex;              /* Number of vertexes in a[] */
  int nAlloc;               /* Space allocated to a[] */
  int nErr;                 /* Number of errors encountered */
  GeoCoord *a;          /* Array of vertexes.  From sqlite3_malloc64() */
};

/* Do a 4-byte byte swap */
static void geopolySwab32(unsigned char *a){
  unsigned char t = a[0];
  a[0] = a[3];
  a[3] = t;
  t = a[1];
  a[1] = a[2];
  a[2] = t;
}

/* Skip whitespace.  Return the next non-whitespace character. */
static char geopolySkipSpace(GeoParse *p){
  while( p->z[0] && safe_isspace(p->z[0]) ) p->z++;
  return p->z[0];
}

/* Parse out a number.  Write the value into *pVal if pVal!=0.
** return non-zero on success and zero if the next token is not a number.
*/
static int geopolyParseNumber(GeoParse *p, GeoCoord *pVal){
  const unsigned char *z = p->z;
  char c = geopolySkipSpace(p);
  int j;
  int seenDP = 0;
  int seenE = 0;
  assert( '-' < '0' );
  if( c<='0' ){
    j = c=='-';
    if( z[j]=='0' && z[j+1]>='0' && z[j+1]<='9' ) return 0;
  }
  j = 1;
  for(;; j++){
    c = z[j];
    if( c>='0' && c<='9' ) continue;
    if( c=='.' ){
      if( z[j-1]=='-' ) return 0;
      if( seenDP ) return 0;
      seenDP = 1;
      continue;
    }
    if( c=='e' || c=='E' ){
      if( z[j-1]<'0' ) return 0;
      if( seenE ) return -1;
      seenDP = seenE = 1;
      c = z[j+1];
      if( c=='+' || c=='-' ){
        j++;
        c = z[j+1];
      }
      if( c<'0' || c>'9' ) return 0;
      continue;
    }
    break;
  }
  if( z[j-1]<'0' ) return 0;
  if( pVal ) *pVal = atof((const char*)p->z);
  p->z += j;
  return 1;
}

/*
** If the input is a well-formed JSON array of coordinates, where each
** coordinate is itself a two-value array, then convert the JSON into
** a GeoPoly object and return a pointer to that object.
**
** If any error occurs, return NULL.
*/
static GeoPoly *geopolyParseJson(const unsigned char *z, int *pRc){
  GeoParse s;
  int rc = SQLITE_OK;
  memset(&s, 0, sizeof(s));
  s.z = z;
  if( geopolySkipSpace(&s)=='[' ){
    s.z++;
    while( geopolySkipSpace(&s)=='[' ){
      int ii = 0;
      char c;
      s.z++;
      if( s.nVertex<=s.nAlloc ){
        GeoCoord *aNew;
        s.nAlloc = s.nAlloc*2 + 16;
        aNew = sqlite3_realloc64(s.a, s.nAlloc*sizeof(GeoCoord)*2 );
        if( aNew==0 ){
          rc = SQLITE_NOMEM;
          s.nErr++;
          break;
        }
        s.a = aNew;
      }
      while( geopolyParseNumber(&s, ii<=1 ? &s.a[s.nVertex*2+ii] : 0) ){
        ii++;
        if( ii==2 ) s.nVertex++;
        c = geopolySkipSpace(&s);
        s.z++;
        if( c==',' ) continue;
        if( c==']' ) break;
        s.nErr++;
        rc = SQLITE_ERROR;
        goto parse_json_err;
      }
      if( geopolySkipSpace(&s)==',' ){
        s.z++;
        continue;
      }
      break;
    }
    if( geopolySkipSpace(&s)==']' && s.nVertex>=4 ){
      int nByte;
      GeoPoly *pOut;
      int x = (s.nVertex-1)*2;
      if( s.a[x]==s.a[0] && s.a[x+1]==s.a[1] ) s.nVertex--;
      nByte = sizeof(GeoPoly) * (s.nVertex-1)*2*sizeof(GeoCoord);
      pOut = sqlite3_malloc64( nByte );
      x = 1;
      if( pOut==0 ) goto parse_json_err;
      pOut->nVertex = s.nVertex;
      memcpy(pOut->a, s.a, s.nVertex*2*sizeof(GeoCoord));
      pOut->hdr[0] = *(unsigned char*)&x;
      pOut->hdr[1] = (s.nVertex>>16)&0xff;
      pOut->hdr[2] = (s.nVertex>>8)&0xff;
      pOut->hdr[3] = s.nVertex&0xff;
      sqlite3_free(s.a);
      if( pRc ) *pRc = SQLITE_OK;
      return pOut;
    }else{
      s.nErr++;
      rc = SQLITE_ERROR;
    }
  }
parse_json_err:
  if( pRc ) *pRc = rc;
  sqlite3_free(s.a);
  return 0;
}

/*
** Given a function parameter, try to interpret it as a polygon, either
** in the binary format or JSON text.  Compute a GeoPoly object and
** return a pointer to that object.  Or if the input is not a well-formed
** polygon, put an error message in sqlite3_context and return NULL.
*/
static GeoPoly *geopolyFuncParam(
  sqlite3_context *pCtx,      /* Context for error messages */
  sqlite3_value *pVal,        /* The value to decode */
  int *pRc                    /* Write error here */
){
  GeoPoly *p = 0;
  int nByte;
  if( sqlite3_value_type(pVal)==SQLITE_BLOB
   && (nByte = sqlite3_value_bytes(pVal))>=(4+6*sizeof(GeoCoord))
  ){
    const unsigned char *a = sqlite3_value_blob(pVal);
    int nVertex;
    nVertex = (a[1]<<16) + (a[2]<<8) + a[3];
    if( (a[0]==0 || a[0]==1)
     && (nVertex*2*sizeof(GeoCoord) + 4)==nByte
    ){
      p = sqlite3_malloc64( sizeof(*p) + (nVertex-1)*2*sizeof(GeoCoord) );
      if( p==0 ){
        if( pRc ) *pRc = SQLITE_NOMEM;
        if( pCtx ) sqlite3_result_error_nomem(pCtx);
      }else{
        int x = 1;
        p->nVertex = nVertex;
        memcpy(p->hdr, a, nByte);
        if( a[0] != *(unsigned char*)&x ){
          int ii;
          for(ii=0; ii<nVertex*2; ii++){
            geopolySwab32((unsigned char*)&p->a[ii]);
          }
          p->hdr[0] ^= 1;
        }
      }
    }
    if( pRc ) *pRc = SQLITE_OK;
    return p;
  }else if( sqlite3_value_type(pVal)==SQLITE_TEXT ){
    return geopolyParseJson(sqlite3_value_text(pVal), pRc);
  }else{
    *pRc = SQLITE_ERROR;
    if( pCtx!=0 ) sqlite3_result_error(pCtx, "not a valid polygon", -1);
    return 0;
  }
}

/*
** Implementation of the geopoly_blob(X) function.
**
** If the input is a well-formed Geopoly BLOB or JSON string
** then return the BLOB representation of the polygon.  Otherwise
** return NULL.
*/
static void geopolyBlobFunc(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  GeoPoly *p = geopolyFuncParam(context, argv[0], 0);
  if( p ){
    sqlite3_result_blob(context, p->hdr, 
       4+8*p->nVertex, SQLITE_TRANSIENT);
    sqlite3_free(p);
  }
}

/*
** SQL function:     geopoly_json(X)
**
** Interpret X as a polygon and render it as a JSON array
** of coordinates.  Or, if X is not a valid polygon, return NULL.
*/
static void geopolyJsonFunc(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  GeoPoly *p = geopolyFuncParam(context, argv[0], 0);
  if( p ){
    sqlite3 *db = sqlite3_context_db_handle(context);
    sqlite3_str *x = sqlite3_str_new(db);
    int i;
    sqlite3_str_append(x, "[", 1);
    for(i=0; i<p->nVertex; i++){
      sqlite3_str_appendf(x, "[%!g,%!g],", p->a[i*2], p->a[i*2+1]);
    }
    sqlite3_str_appendf(x, "[%!g,%!g]]", p->a[0], p->a[1]);
    sqlite3_result_text(context, sqlite3_str_finish(x), -1, sqlite3_free);
    sqlite3_free(p);
  }
}

/*
** SQL function:     geopoly_svg(X, ....)
**
** Interpret X as a polygon and render it as a SVG <polyline>.
** Additional arguments are added as attributes to the <polyline>.
*/
static void geopolySvgFunc(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  GeoPoly *p = geopolyFuncParam(context, argv[0], 0);
  if( p ){
    sqlite3 *db = sqlite3_context_db_handle(context);
    sqlite3_str *x = sqlite3_str_new(db);
    int i;
    char cSep = '\'';
    sqlite3_str_appendf(x, "<polyline points=");
    for(i=0; i<p->nVertex; i++){
      sqlite3_str_appendf(x, "%c%g,%g", cSep, p->a[i*2], p->a[i*2+1]);
      cSep = ' ';
    }
    sqlite3_str_appendf(x, " %g,%g'", p->a[0], p->a[1]);
    for(i=1; i<argc; i++){
      const char *z = (const char*)sqlite3_value_text(argv[i]);
      if( z && z[0] ){
        sqlite3_str_appendf(x, " %s", z);
      }
    }
    sqlite3_str_appendf(x, "></polyline>");
    sqlite3_result_text(context, sqlite3_str_finish(x), -1, sqlite3_free);
    sqlite3_free(p);
  }
}

/*
** Implementation of the geopoly_area(X) function.
**
** If the input is a well-formed Geopoly BLOB then return the area
** enclosed by the polygon.  If the polygon circulates clockwise instead
** of counterclockwise (as it should) then return the negative of the
** enclosed area.  Otherwise return NULL.
*/
static void geopolyAreaFunc(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  GeoPoly *p = geopolyFuncParam(context, argv[0], 0);
  if( p ){
    double rArea = 0.0;
    int ii;
    for(ii=0; ii<p->nVertex-1; ii++){
      rArea += (p->a[ii*2] - p->a[ii*2+2])           /* (x0 - x1) */
                * (p->a[ii*2+1] + p->a[ii*2+3])      /* (y0 + y1) */
                * 0.5;
    }
    rArea += (p->a[ii*2] - p->a[0])                  /* (xN - x0) */
             * (p->a[ii*2+1] + p->a[1])              /* (yN + y0) */
             * 0.5;
    sqlite3_result_double(context, rArea);
    sqlite3_free(p);
  }            
}

/*
** Compute a bound-box on a polygon.  Return a new GeoPoly object
** that describes the bounding box.  Or, if aCoord is not a NULL pointer
** fill it in with the bounding box instead.
*/
static GeoPoly *geopolyBBox(
  sqlite3_context *context,   /* For recording the error */
  sqlite3_value *pPoly,       /* The polygon */
  RtreeCoord *aCoord,         /* Results here */
  int *pRc                    /* Error code here */
){
  GeoPoly *p = geopolyFuncParam(context, pPoly, pRc);
  GeoPoly *pOut = 0;
  if( p ){
    int ii;
    float mnX, mxX, mnY, mxY;
    mnX = mxX = p->a[0];
    mnY = mxY = p->a[1];
    for(ii=1; ii<p->nVertex; ii++){
      double r = p->a[ii*2];
      if( r<mnX ) mnX = r;
      else if( r>mxX ) mxX = r;
      r = p->a[ii*2+1];
      if( r<mnY ) mnY = r;
      else if( r>mxY ) mxY = r;
    }
    if( pRc ) *pRc = SQLITE_OK;
    if( aCoord==0 ){
      pOut = sqlite3_realloc(p, sizeof(GeoPoly)+sizeof(GeoCoord)*6);
      if( pOut==0 ){
        sqlite3_free(p);
        if( context ) sqlite3_result_error_nomem(context);
        if( pRc ) *pRc = SQLITE_NOMEM;
        return 0;
      }
      pOut->nVertex = 4;
      pOut->hdr[1] = 0;
      pOut->hdr[2] = 0;
      pOut->hdr[3] = 4;
      pOut->a[0] = mnX;
      pOut->a[1] = mnY;
      pOut->a[2] = mxX;
      pOut->a[3] = mnY;
      pOut->a[4] = mxX;
      pOut->a[5] = mxY;
      pOut->a[6] = mnX;
      pOut->a[7] = mxY;
    }else{
      sqlite3_free(p);
      aCoord[0].f = mnX;
      aCoord[1].f = mxX;
      aCoord[2].f = mnY;
      aCoord[3].f = mxY;
    }
  }
  return pOut;
}

/*
** Implementation of the geopoly_bbox(X) SQL function.
*/
static void geopolyBBoxFunc(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  GeoPoly *p = geopolyBBox(context, argv[0], 0, 0);
  if( p ){
    sqlite3_result_blob(context, p->hdr, 
       4+8*p->nVertex, SQLITE_TRANSIENT);
    sqlite3_free(p);
  }
}

/*
** Determine if point (x0,y0) is beneath line segment (x1,y1)->(x2,y2).
** Returns:
**
**    +2  x0,y0 is on the line segement
**
**    +1  x0,y0 is beneath line segment
**
**    0   x0,y0 is not on or beneath the line segment or the line segment
**        is vertical and x0,y0 is not on the line segment
**
** The left-most coordinate min(x1,x2) is not considered to be part of
** the line segment for the purposes of this analysis.
*/
static int pointBeneathLine(
  double x0, double y0,
  double x1, double y1,
  double x2, double y2
){
  double y;
  if( x0==x1 && y0==y1 ) return 2;
  if( x1<x2 ){
    if( x0<=x1 || x0>x2 ) return 0;
  }else if( x1>x2 ){
    if( x0<=x2 || x0>x1 ) return 0;
  }else{
    /* Vertical line segment */
    if( x0!=x1 ) return 0;
    if( y0<y1 && y0<y2 ) return 0;
    if( y0>y1 && y0>y2 ) return 0;
    return 2;
  }
  y = y1 + (y2-y1)*(x0-x1)/(x2-x1);
  if( y0==y ) return 2;
  if( y0<y ) return 1;
  return 0;
}

/*
** SQL function:    geopoly_within(P,X,Y)
**
** Return +2 if point X,Y is within polygon P.
** Return +1 if point X,Y is on the polygon boundary.
** Return 0 if point X,Y is outside the polygon
*/
static void geopolyWithinFunc(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  GeoPoly *p = geopolyFuncParam(context, argv[0], 0);
  double x0 = sqlite3_value_double(argv[1]);
  double y0 = sqlite3_value_double(argv[2]);
  if( p ){
    int v = 0;
    int cnt = 0;
    int ii;
    for(ii=0; ii<p->nVertex-1; ii++){
      v = pointBeneathLine(x0,y0,p->a[ii*2],p->a[ii*2+1],
                                 p->a[ii*2+2],p->a[ii*2+3]);
      if( v==2 ) break;
      cnt += v;
    }
    if( v!=2 ){
      v = pointBeneathLine(x0,y0,p->a[ii*2],p->a[ii*2+1],
                                 p->a[0],p->a[1]);
    }
    if( v==2 ){
      sqlite3_result_int(context, 1);
    }else if( ((v+cnt)&1)==0 ){
      sqlite3_result_int(context, 0);
    }else{
      sqlite3_result_int(context, 2);
    }
    sqlite3_free(p);
  }            
}

/* Objects used by the overlap algorihm. */
typedef struct GeoEvent GeoEvent;
typedef struct GeoSegment GeoSegment;
typedef struct GeoOverlap GeoOverlap;
struct GeoEvent {
  double x;              /* X coordinate at which event occurs */
  int eType;             /* 0 for ADD, 1 for REMOVE */
  GeoSegment *pSeg;      /* The segment to be added or removed */
  GeoEvent *pNext;       /* Next event in the sorted list */
};
struct GeoSegment {
  double C, B;           /* y = C*x + B */
  double y;              /* Current y value */
  float y0;              /* Initial y value */
  unsigned char side;    /* 1 for p1, 2 for p2 */
  unsigned int idx;      /* Which segment within the side */
  GeoSegment *pNext;     /* Next segment in a list sorted by y */
};
struct GeoOverlap {
  GeoEvent *aEvent;          /* Array of all events */
  GeoSegment *aSegment;      /* Array of all segments */
  int nEvent;                /* Number of events */
  int nSegment;              /* Number of segments */
};

/*
** Add a single segment and its associated events.
*/
static void geopolyAddOneSegment(
  GeoOverlap *p,
  GeoCoord x0,
  GeoCoord y0,
  GeoCoord x1,
  GeoCoord y1,
  unsigned char side,
  unsigned int idx
){
  GeoSegment *pSeg;
  GeoEvent *pEvent;
  if( x0==x1 ) return;  /* Ignore vertical segments */
  if( x0>x1 ){
    GeoCoord t = x0;
    x0 = x1;
    x1 = t;
    t = y0;
    y0 = y1;
    y1 = t;
  }
  pSeg = p->aSegment + p->nSegment;
  p->nSegment++;
  pSeg->C = (y1-y0)/(x1-x0);
  pSeg->B = y1 - x1*pSeg->C;
  pSeg->y0 = y0;
  pSeg->side = side;
  pSeg->idx = idx;
  pEvent = p->aEvent + p->nEvent;
  p->nEvent++;
  pEvent->x = x0;
  pEvent->eType = 0;
  pEvent->pSeg = pSeg;
  pEvent = p->aEvent + p->nEvent;
  p->nEvent++;
  pEvent->x = x1;
  pEvent->eType = 1;
  pEvent->pSeg = pSeg;
}
  


/*
** Insert all segments and events for polygon pPoly.
*/
static void geopolyAddSegments(
  GeoOverlap *p,          /* Add segments to this Overlap object */
  GeoPoly *pPoly,         /* Take all segments from this polygon */
  unsigned char side      /* The side of pPoly */
){
  unsigned int i;
  GeoCoord *x;
  for(i=0; i<pPoly->nVertex-1; i++){
    x = pPoly->a + (i*2);
    geopolyAddOneSegment(p, x[0], x[1], x[2], x[3], side, i);
  }
  x = pPoly->a + (i*2);
  geopolyAddOneSegment(p, x[0], x[1], pPoly->a[0], pPoly->a[1], side, i);
}

/*
** Merge two lists of sorted events by X coordinate
*/
static GeoEvent *geopolyEventMerge(GeoEvent *pLeft, GeoEvent *pRight){
  GeoEvent head, *pLast;
  head.pNext = 0;
  pLast = &head;
  while( pRight && pLeft ){
    if( pRight->x <= pLeft->x ){
      pLast->pNext = pRight;
      pLast = pRight;
      pRight = pRight->pNext;
    }else{
      pLast->pNext = pLeft;
      pLast = pLeft;
      pLeft = pLeft->pNext;
    }
  }
  pLast->pNext = pRight ? pRight : pLeft;
  return head.pNext;  
}

/*
** Sort an array of nEvent event objects into a list.
*/
static GeoEvent *geopolySortEventsByX(GeoEvent *aEvent, int nEvent){
  int mx = 0;
  int i, j;
  GeoEvent *p;
  GeoEvent *a[50];
  for(i=0; i<nEvent; i++){
    p = &aEvent[i];
    p->pNext = 0;
    for(j=0; j<mx && a[j]; j++){
      p = geopolyEventMerge(a[j], p);
      a[j] = 0;
    }
    a[j] = p;
    if( j>=mx ) mx = j+1;
  }
  p = 0;
  for(i=0; i<mx; i++){
    p = geopolyEventMerge(a[i], p);
  }
  return p;
}

/*
** Merge two lists of sorted segments by Y, and then by C.
*/
static GeoSegment *geopolySegmentMerge(GeoSegment *pLeft, GeoSegment *pRight){
  GeoSegment head, *pLast;
  head.pNext = 0;
  pLast = &head;
  while( pRight && pLeft ){
    double r = pRight->y - pLeft->y;
    if( r==0.0 ) r = pRight->C - pLeft->C;
    if( r<0.0 ){
      pLast->pNext = pRight;
      pLast = pRight;
      pRight = pRight->pNext;
    }else{
      pLast->pNext = pLeft;
      pLast = pLeft;
      pLeft = pLeft->pNext;
    }
  }
  pLast->pNext = pRight ? pRight : pLeft;
  return head.pNext;  
}

/*
** Sort a list of GeoSegments in order of increasing Y and in the event of
** a tie, increasing C (slope).
*/
static GeoSegment *geopolySortSegmentsByYAndC(GeoSegment *pList){
  int mx = 0;
  int i;
  GeoSegment *p;
  GeoSegment *a[50];
  while( pList ){
    p = pList;
    pList = pList->pNext;
    p->pNext = 0;
    for(i=0; i<mx && a[i]; i++){
      p = geopolySegmentMerge(a[i], p);
      a[i] = 0;
    }
    a[i] = p;
    if( i>=mx ) mx = i+1;
  }
  p = 0;
  for(i=0; i<mx; i++){
    p = geopolySegmentMerge(a[i], p);
  }
  return p;
}

/*
** Determine the overlap between two polygons
*/
static int geopolyOverlap(GeoPoly *p1, GeoPoly *p2){
  int nVertex = p1->nVertex + p2->nVertex + 2;
  GeoOverlap *p;
  int nByte;
  GeoEvent *pThisEvent;
  double rX;
  int rc = 0;
  int needSort = 0;
  GeoSegment *pActive = 0;
  GeoSegment *pSeg;
  unsigned char aOverlap[4];

  nByte = sizeof(GeoEvent)*nVertex*2 
           + sizeof(GeoSegment)*nVertex 
           + sizeof(GeoOverlap);
  p = sqlite3_malloc( nByte );
  if( p==0 ) return -1;
  p->aEvent = (GeoEvent*)&p[1];
  p->aSegment = (GeoSegment*)&p->aEvent[nVertex*2];
  p->nEvent = p->nSegment = 0;
  geopolyAddSegments(p, p1, 1);
  geopolyAddSegments(p, p2, 2);
  pThisEvent = geopolySortEventsByX(p->aEvent, p->nEvent);
  rX = pThisEvent->x==0.0 ? -1.0 : 0.0;
  memset(aOverlap, 0, sizeof(aOverlap));
  while( pThisEvent ){
    if( pThisEvent->x!=rX ){
      GeoSegment *pPrev = 0;
      int iMask = 0;
      GEODEBUG(("Distinct X: %g\n", pThisEvent->x));
      rX = pThisEvent->x;
      if( needSort ){
        GEODEBUG(("SORT\n"));
        pActive = geopolySortSegmentsByYAndC(pActive);
        needSort = 0;
      }
      for(pSeg=pActive; pSeg; pSeg=pSeg->pNext){
        if( pPrev ){
          if( pPrev->y!=pSeg->y ){
            GEODEBUG(("MASK: %d\n", iMask));
            aOverlap[iMask] = 1;
          }
        }
        iMask ^= pSeg->side;
        pPrev = pSeg;
      }
      pPrev = 0;
      for(pSeg=pActive; pSeg; pSeg=pSeg->pNext){
        double y = pSeg->C*rX + pSeg->B;
        GEODEBUG(("Segment %d.%d %g->%g\n", pSeg->side, pSeg->idx, pSeg->y, y));
        pSeg->y = y;
        if( pPrev ){
          if( pPrev->y>pSeg->y && pPrev->side!=pSeg->side ){
            rc = 1;
            GEODEBUG(("Crossing: %d.%d and %d.%d\n",
                    pPrev->side, pPrev->idx,
                    pSeg->side, pSeg->idx));
            goto geopolyOverlapDone;
          }else if( pPrev->y!=pSeg->y ){
            GEODEBUG(("MASK: %d\n", iMask));
            aOverlap[iMask] = 1;
          }
        }
        iMask ^= pSeg->side;
        pPrev = pSeg;
      }
    }
    GEODEBUG(("%s %d.%d C=%g B=%g\n",
      pThisEvent->eType ? "RM " : "ADD",
      pThisEvent->pSeg->side, pThisEvent->pSeg->idx,
      pThisEvent->pSeg->C,
      pThisEvent->pSeg->B));
    if( pThisEvent->eType==0 ){
      /* Add a segment */
      pSeg = pThisEvent->pSeg;
      pSeg->y = pSeg->y0;
      pSeg->pNext = pActive;
      pActive = pSeg;
      needSort = 1;
    }else{
      /* Remove a segment */
      if( pActive==pThisEvent->pSeg ){
        pActive = pActive->pNext;
      }else{
        for(pSeg=pActive; pSeg; pSeg=pSeg->pNext){
          if( pSeg->pNext==pThisEvent->pSeg ){
            pSeg->pNext = pSeg->pNext->pNext;
            break;
          }
        }
      }
    }
    pThisEvent = pThisEvent->pNext;
  }
  if( aOverlap[3]==0 ){
    rc = 0;
  }else if( aOverlap[1]!=0 && aOverlap[2]==0 ){
    rc = 3;
  }else if( aOverlap[1]==0 && aOverlap[2]!=0 ){
    rc = 2;
  }else if( aOverlap[1]==0 && aOverlap[2]==0 ){
    rc = 4;
  }else{
    rc = 1;
  }

geopolyOverlapDone:
  sqlite3_free(p);
  return rc;
}

/*
** SQL function:    geopoly_overlap(P1,P2)
**
** Determine whether or not P1 and P2 overlap. Return value:
**
**   0     The two polygons are disjoint
**   1     They overlap
**   2     P1 is completely contained within P2
**   3     P2 is completely contained within P1
**   4     P1 and P2 are the same polygon
**   NULL  Either P1 or P2 or both are not valid polygons
*/
static void geopolyOverlapFunc(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  GeoPoly *p1 = geopolyFuncParam(context, argv[0], 0);
  GeoPoly *p2 = geopolyFuncParam(context, argv[1], 0);
  if( p1 && p2 ){
    int x = geopolyOverlap(p1, p2);
    if( x<0 ){
      sqlite3_result_error_nomem(context);
    }else{
      sqlite3_result_int(context, x);
    }
  }
  sqlite3_free(p1);
  sqlite3_free(p2);
}

/*
** Enable or disable debugging output
*/
static void geopolyDebugFunc(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
#ifdef GEOPOLY_ENABLE_DEBUG
  geo_debug = sqlite3_value_int(argv[0]);
#endif
}

/* 
** This function is the implementation of both the xConnect and xCreate
** methods of the geopoly virtual table.
**
**   argv[0]   -> module name
**   argv[1]   -> database name
**   argv[2]   -> table name
**   argv[...] -> column names...
*/
static int geopolyInit(
  sqlite3 *db,                        /* Database connection */
  void *pAux,                         /* One of the RTREE_COORD_* constants */
  int argc, const char *const*argv,   /* Parameters to CREATE TABLE statement */
  sqlite3_vtab **ppVtab,              /* OUT: New virtual table */
  char **pzErr,                       /* OUT: Error message, if any */
  int isCreate                        /* True for xCreate, false for xConnect */
){
  int rc = SQLITE_OK;
  Rtree *pRtree;
  int nDb;              /* Length of string argv[1] */
  int nName;            /* Length of string argv[2] */
  sqlite3_str *pSql;
  char *zSql;
  int ii;
  char cSep;

  sqlite3_vtab_config(db, SQLITE_VTAB_CONSTRAINT_SUPPORT, 1);

  /* Allocate the sqlite3_vtab structure */
  nDb = (int)strlen(argv[1]);
  nName = (int)strlen(argv[2]);
  pRtree = (Rtree *)sqlite3_malloc(sizeof(Rtree)+nDb+nName+2);
  if( !pRtree ){
    return SQLITE_NOMEM;
  }
  memset(pRtree, 0, sizeof(Rtree)+nDb+nName+2);
  pRtree->nBusy = 1;
  pRtree->base.pModule = &rtreeModule;
  pRtree->zDb = (char *)&pRtree[1];
  pRtree->zName = &pRtree->zDb[nDb+1];
  pRtree->eCoordType = RTREE_COORD_REAL32;
  pRtree->nDim = 2;
  pRtree->nDim2 = 4;
  memcpy(pRtree->zDb, argv[1], nDb);
  memcpy(pRtree->zName, argv[2], nName);


  /* Create/Connect to the underlying relational database schema. If
  ** that is successful, call sqlite3_declare_vtab() to configure
  ** the r-tree table schema.
  */
  pSql = sqlite3_str_new(db);
  sqlite3_str_appendf(pSql, "CREATE TABLE x");
  cSep = '(';
  pRtree->nAux = 1;   /* Add one for _shape */
  for(ii=3; ii<argc; ii++){
    pRtree->nAux++;
    sqlite3_str_appendf(pSql, "%c%s", cSep, argv[ii]+1);
    cSep = ',';
  }
  sqlite3_str_appendf(pSql, "%c _shape, _bbox HIDDEN);", cSep);
  zSql = sqlite3_str_finish(pSql);
  if( !zSql ){
    rc = SQLITE_NOMEM;
  }else if( SQLITE_OK!=(rc = sqlite3_declare_vtab(db, zSql)) ){
    *pzErr = sqlite3_mprintf("%s", sqlite3_errmsg(db));
  }
  sqlite3_free(zSql);
  if( rc ) goto geopolyInit_fail;
  pRtree->nBytesPerCell = 8 + pRtree->nDim2*4;

  /* Figure out the node size to use. */
  rc = getNodeSize(db, pRtree, isCreate, pzErr);
  if( rc ) goto geopolyInit_fail;
  rc = rtreeSqlInit(pRtree, db, argv[1], argv[2], isCreate);
  if( rc ){
    *pzErr = sqlite3_mprintf("%s", sqlite3_errmsg(db));
    goto geopolyInit_fail;
  }

  *ppVtab = (sqlite3_vtab *)pRtree;
  return SQLITE_OK;

geopolyInit_fail:
  if( rc==SQLITE_OK ) rc = SQLITE_ERROR;
  assert( *ppVtab==0 );
  assert( pRtree->nBusy==1 );
  rtreeRelease(pRtree);
  return rc;
}


/* 
** GEOPOLY virtual table module xCreate method.
*/
static int geopolyCreate(
  sqlite3 *db,
  void *pAux,
  int argc, const char *const*argv,
  sqlite3_vtab **ppVtab,
  char **pzErr
){
  return geopolyInit(db, pAux, argc, argv, ppVtab, pzErr, 1);
}

/* 
** GEOPOLY virtual table module xConnect method.
*/
static int geopolyConnect(
  sqlite3 *db,
  void *pAux,
  int argc, const char *const*argv,
  sqlite3_vtab **ppVtab,
  char **pzErr
){
  return geopolyInit(db, pAux, argc, argv, ppVtab, pzErr, 0);
}


/*
** GEOPOLY virtual table module xBestIndex method. There are three
** table scan strategies to choose from (in order from most to 
** least desirable):
**
**   idxNum     idxStr        Strategy
**   ------------------------------------------------
**     1        Unused        Direct lookup by rowid.
**     2        'Fx'           shape query
**     2        ''            full-table scan.
**   ------------------------------------------------
**
** If strategy 1 is used, then idxStr is not meaningful. If strategy
** 2 is used, idxStr is either the two-byte string 'Fx' or an empty
** string.
*/
static int geopolyBestIndex(sqlite3_vtab *tab, sqlite3_index_info *pIdxInfo){
  Rtree *pRtree = (Rtree*)tab;
  int rc = SQLITE_OK;
  int ii;
  int bMatch = 0;                 /* True if there exists a MATCH constraint */
  i64 nRow;                       /* Estimated rows returned by this scan */

  int iIdx = 0;
  char zIdxStr[3];
  memset(zIdxStr, 0, sizeof(zIdxStr));

  /* Check if there exists a MATCH constraint - even an unusable one. If there
  ** is, do not consider the lookup-by-rowid plan as using such a plan would
  ** require the VDBE to evaluate the MATCH constraint, which is not currently
  ** possible. */
  for(ii=0; ii<pIdxInfo->nConstraint; ii++){
    if( pIdxInfo->aConstraint[ii].op==SQLITE_INDEX_CONSTRAINT_MATCH ){
      bMatch = 1;
    }
  }

  assert( pIdxInfo->idxStr==0 );
  for(ii=0; ii<pIdxInfo->nConstraint && iIdx<(int)(sizeof(zIdxStr)-1); ii++){
    struct sqlite3_index_constraint *p = &pIdxInfo->aConstraint[ii];

    if( bMatch==0
     && p->usable 
     && p->iColumn<0
     && p->op==SQLITE_INDEX_CONSTRAINT_EQ 
    ){
      /* We have an equality constraint on the rowid. Use strategy 1. */
      int jj;
      for(jj=0; jj<ii; jj++){
        pIdxInfo->aConstraintUsage[jj].argvIndex = 0;
        pIdxInfo->aConstraintUsage[jj].omit = 0;
      }
      pIdxInfo->idxNum = 1;
      pIdxInfo->aConstraintUsage[ii].argvIndex = 1;
      pIdxInfo->aConstraintUsage[jj].omit = 1;

      /* This strategy involves a two rowid lookups on an B-Tree structures
      ** and then a linear search of an R-Tree node. This should be 
      ** considered almost as quick as a direct rowid lookup (for which 
      ** sqlite uses an internal cost of 0.0). It is expected to return
      ** a single row.
      */ 
      pIdxInfo->estimatedCost = 30.0;
      pIdxInfo->estimatedRows = 1;
      pIdxInfo->idxFlags = SQLITE_INDEX_SCAN_UNIQUE;
      return SQLITE_OK;
    }

    /* A MATCH operator against the _shape column */
    if( p->usable
     && p->iColumn==pRtree->nAux
     && p->op==SQLITE_INDEX_CONSTRAINT_MATCH
    ){
      zIdxStr[0] = RTREE_QUERY;
      zIdxStr[1] = 'x';
      zIdxStr[2] = 0;
      pIdxInfo->aConstraintUsage[ii].argvIndex = 0;
      pIdxInfo->aConstraintUsage[ii].omit = 1;
    }
  }

  pIdxInfo->idxNum = 2;
  pIdxInfo->needToFreeIdxStr = 1;
  if( iIdx>0 && 0==(pIdxInfo->idxStr = sqlite3_mprintf("%s", zIdxStr)) ){
    return SQLITE_NOMEM;
  }

  nRow = pRtree->nRowEst/100 + 5;
  pIdxInfo->estimatedCost = (double)6.0 * (double)nRow;
  pIdxInfo->estimatedRows = nRow;

  return rc;
}


/* 
** GEOPOLY virtual table module xColumn method.
*/
static int geopolyColumn(sqlite3_vtab_cursor *cur, sqlite3_context *ctx, int i){
  Rtree *pRtree = (Rtree *)cur->pVtab;
  RtreeCursor *pCsr = (RtreeCursor *)cur;
  RtreeSearchPoint *p = rtreeSearchPointFirst(pCsr);
  int rc = SQLITE_OK;
  RtreeNode *pNode = rtreeNodeOfFirstSearchPoint(pCsr, &rc);

  if( rc ) return rc;
  if( p==0 ) return SQLITE_OK;
  if( i<=pRtree->nAux ){
    if( !pCsr->bAuxValid ){
      if( pCsr->pReadAux==0 ){
        rc = sqlite3_prepare_v3(pRtree->db, pRtree->zReadAuxSql, -1, 0,
                                &pCsr->pReadAux, 0);
        if( rc ) return rc;
      }
      sqlite3_bind_int64(pCsr->pReadAux, 1, 
          nodeGetRowid(pRtree, pNode, p->iCell));
      rc = sqlite3_step(pCsr->pReadAux);
      if( rc==SQLITE_ROW ){
        pCsr->bAuxValid = 1;
      }else{
        sqlite3_reset(pCsr->pReadAux);
        if( rc==SQLITE_DONE ) rc = SQLITE_OK;
        return rc;
      }
    }
    sqlite3_result_value(ctx,
         sqlite3_column_value(pCsr->pReadAux, i - pRtree->nDim2 + 1));
  }else{
    /* Must be the _bbox column */
  }
  return SQLITE_OK;
}


/*
** The xUpdate method for GEOPOLY module virtual tables.
*/
static int geopolyUpdate(
  sqlite3_vtab *pVtab, 
  int nData, 
  sqlite3_value **aData, 
  sqlite_int64 *pRowid
){
  Rtree *pRtree = (Rtree *)pVtab;
  int rc = SQLITE_OK;
  RtreeCell cell;                 /* New cell to insert if nData>1 */
  int iShapeCol;                  /* Index of the _shape column */
  i64 oldRowid;                   /* The old rowid */
  int oldRowidValid;              /* True if oldRowid is valid */
  i64 newRowid;                   /* The new rowid */
  int newRowidValid;              /* True if newRowid is valid */
  int coordChange = 0;            /* Change in coordinates */

  if( pRtree->nNodeRef ){
    /* Unable to write to the btree while another cursor is reading from it,
    ** since the write might do a rebalance which would disrupt the read
    ** cursor. */
    return SQLITE_LOCKED_VTAB;
  }
  rtreeReference(pRtree);
  assert(nData>=1);

  iShapeCol = pRtree->nAux;
  rc = SQLITE_ERROR;
  oldRowidValid = sqlite3_value_type(aData[0])!=SQLITE_NULL;;
  oldRowid = oldRowidValid ? sqlite3_value_int64(aData[0]) : 0;
  newRowidValid = nData>1 && sqlite3_value_type(aData[1])!=SQLITE_NULL;
  newRowid = newRowidValid ? sqlite3_value_int64(aData[1]) : 0;
  cell.iRowid = newRowid;

  if( nData>1                                           /* not a DELETE */
   && (!oldRowidValid                                   /* INSERT */
        || !sqlite3_value_nochange(aData[iShapeCol+2])  /* UPDATE _shape */
        || oldRowid!=newRowid)                          /* Rowid change */
  ){
    geopolyBBox(0, aData[iShapeCol+2], cell.aCoord, &rc);
    if( rc ){
      if( rc==SQLITE_ERROR ){
        pVtab->zErrMsg =
          sqlite3_mprintf("_shape does not contain a valid polygon");
      }
      return rc;
    }
    coordChange = 1;

    /* If a rowid value was supplied, check if it is already present in 
    ** the table. If so, the constraint has failed. */
    if( oldRowidValid && oldRowid!=newRowid ){
      int steprc;
      sqlite3_bind_int64(pRtree->pReadRowid, 1, cell.iRowid);
      steprc = sqlite3_step(pRtree->pReadRowid);
      rc = sqlite3_reset(pRtree->pReadRowid);
      if( SQLITE_ROW==steprc ){
        if( sqlite3_vtab_on_conflict(pRtree->db)==SQLITE_REPLACE ){
          rc = rtreeDeleteRowid(pRtree, cell.iRowid);
        }else{
          rc = rtreeConstraintError(pRtree, 0);
        }
      }
    }
  }

  /* If aData[0] is not an SQL NULL value, it is the rowid of a
  ** record to delete from the r-tree table. The following block does
  ** just that.
  */
  if( rc==SQLITE_OK && (nData==1 || coordChange) ){
    rc = rtreeDeleteRowid(pRtree, oldRowid);
  }

  /* If the aData[] array contains more than one element, elements
  ** (aData[2]..aData[argc-1]) contain a new record to insert into
  ** the r-tree structure.
  */
  if( rc==SQLITE_OK && nData>1 && coordChange ){
    /* Insert the new record into the r-tree */
    RtreeNode *pLeaf = 0;
    *pRowid = cell.iRowid;
    if( rc==SQLITE_OK ){
      rc = ChooseLeaf(pRtree, &cell, 0, &pLeaf);
    }
    if( rc==SQLITE_OK ){
      int rc2;
      pRtree->iReinsertHeight = -1;
      rc = rtreeInsertCell(pRtree, pLeaf, &cell, 0);
      rc2 = nodeRelease(pRtree, pLeaf);
      if( rc==SQLITE_OK ){
        rc = rc2;
      }
    }
  }

  /* Change the data */
  if( rc==SQLITE_OK && pRtree->nAux>0 ){
    sqlite3_stmt *pUp = pRtree->pWriteAux;
    int jj;
    int nChange = 0;
    sqlite3_bind_int64(pUp, 1, newRowid);
    for(jj=0; jj<pRtree->nAux; jj++){
      if( !sqlite3_value_nochange(aData[jj+2]) ) nChange++;
      sqlite3_bind_value(pUp, jj+2, aData[jj+2]);
    }
    if( nChange ){
      sqlite3_step(pUp);
      rc = sqlite3_reset(pUp);
    }
  }

  rtreeRelease(pRtree);
  return rc;
}

static sqlite3_module geopolyModule = {
  2,                          /* iVersion */
  geopolyCreate,              /* xCreate - create a table */
  geopolyConnect,             /* xConnect - connect to an existing table */
  geopolyBestIndex,           /* xBestIndex - Determine search strategy */
  rtreeDisconnect,            /* xDisconnect - Disconnect from a table */
  rtreeDestroy,               /* xDestroy - Drop a table */
  rtreeOpen,                  /* xOpen - open a cursor */
  rtreeClose,                 /* xClose - close a cursor */
  rtreeFilter,                /* xFilter - configure scan constraints */
  rtreeNext,                  /* xNext - advance a cursor */
  rtreeEof,                   /* xEof */
  geopolyColumn,              /* xColumn - read data */
  rtreeRowid,                 /* xRowid - read data */
  geopolyUpdate,              /* xUpdate - write data */
  rtreeBeginTransaction,      /* xBegin - begin transaction */
  rtreeEndTransaction,        /* xSync - sync transaction */
  rtreeEndTransaction,        /* xCommit - commit transaction */
  rtreeEndTransaction,        /* xRollback - rollback transaction */
  0,                          /* xFindFunction - function overloading */
  rtreeRename,                /* xRename - rename the table */
  rtreeSavepoint,             /* xSavepoint */
  0,                          /* xRelease */
  0,                          /* xRollbackTo */
};

static int sqlite3_geopoly_init(sqlite3 *db){
  int rc = SQLITE_OK;
  static const struct {
    void (*xFunc)(sqlite3_context*,int,sqlite3_value**);
    int nArg;
    const char *zName;
  } aFunc[] = {
     { geopolyAreaFunc,          1,    "geopoly_area"     },
     { geopolyBlobFunc,          1,    "geopoly_blob"     },
     { geopolyJsonFunc,          1,    "geopoly_json"     },
     { geopolySvgFunc,          -1,    "geopoly_svg"      },
     { geopolyWithinFunc,        3,    "geopoly_within"   },
     { geopolyOverlapFunc,       2,    "geopoly_overlap"  },
     { geopolyDebugFunc,         1,    "geopoly_debug"    },
     { geopolyBBoxFunc,          1,    "geopoly_bbox"     },
  };
  int i;
  for(i=0; i<sizeof(aFunc)/sizeof(aFunc[0]) && rc==SQLITE_OK; i++){
    rc = sqlite3_create_function(db, aFunc[i].zName, aFunc[i].nArg,
                                 SQLITE_UTF8, 0,
                                 aFunc[i].xFunc, 0, 0);
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3_create_module_v2(db, "geopoly", &geopolyModule, 0, 0);
  }
  return rc;
}