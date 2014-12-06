/*
|| This file is part of Pike. For copyright information see COPYRIGHT.
|| Pike is distributed under GPL, LGPL and MPL. See the file COPYING
|| for more information.
*/

#include "global.h"
#include "zlib_machine.h"
#include "module.h"
#include "program.h"
#include "mapping.h"
#include "module_support.h"

#if !defined(HAVE_LIBZ) && !defined(HAVE_LIBGZ)
#undef HAVE_ZLIB_H
#endif

#ifdef HAVE_ZLIB_H

#include "interpret.h"
#include "svalue.h"
#include "stralloc.h"
#include "array.h"
#include "pike_macros.h"
#include "stralloc.h"
#include "object.h"
#include "pike_types.h"
#include "threads.h"
#include "dynamic_buffer.h"
#include "operators.h"

#include <zlib.h>


#define sp Pike_sp

struct zipper
{
  int  level;
  int  state;
  struct z_stream_s gz;
  struct pike_string *epilogue, *dict;
#ifdef _REENTRANT
  DEFINE_MUTEX(lock);
#endif /* _REENTRANT */
};

#define BUF 32768
#define MAX_BUF	(64*BUF)

#undef THIS
#define THIS ((struct zipper *)(Pike_fp->current_storage))

/*! @module Gz
 *!
 *! The Gz module contains functions to compress and uncompress strings using
 *! the same algorithm as the program @tt{gzip@}. Compressing can be done in
 *! streaming mode or all at once.
 *!
 *! The Gz module consists of two classes; Gz.deflate and Gz.inflate.
 *! Gz.deflate is used to pack data
 *! and Gz.inflate is used to unpack data. (Think "inflatable boat")
 *!
 *! @note
 *!   Note that this module is only available if the gzip library was
 *!   available when Pike was compiled.
 *!
 *!   Note that although these functions use the same @i{algorithm@} as
 *!   @tt{gzip@}, they do not use the exact same format, so you cannot directly
 *!   unzip gzipped files with these routines. Support for this will be
 *!   added in the future.
 */

/*! @class deflate
 *!
 *! This class interfaces with the compression routines in the libz
 *! library.
 *!
 *! @note
 *! This class is only available if libz was available and found when
 *! Pike was compiled.
 *!
 *! @seealso
 *! @[Gz.inflate()], @[Gz.compress()], @[Gz.uncompress()]
 */

/*! @decl void create(int(-9..9)|void level, int|void strategy,@
 *!                   int(8..15)|void window_size)
 *! @decl void create(mapping options)
 *!
 *! This function can also be used to re-initialize a Gz.deflate object
 *! so it can be re-used.
 *!
 *! If a mapping is passed as the only argument, it will accept the
 *! parameters described below as indices, and additionally it accepts
 *! a @expr{string@} as @expr{dictionary@}.
 *!
 *! @param level
 *!   Indicates the level of effort spent to make the data compress
 *!   well. Zero means no packing, 2-3 is considered 'fast', 6 is
 *!   default and higher is considered 'slow' but gives better
 *!   packing.
 *!
 *!   If the argument is negative, no headers will be emitted. This is
 *!   needed to produce ZIP-files, as an example. The negative value
 *!   is then negated, and handled as a positive value.
 *!
 *! @param strategy
 *!   The strategy to be used when compressing the data. One of the
 *!   following.
 *! @int
 *!   @value DEFAULT_STRATEGY
 *!     The default strategy as selected in the zlib library.
 *!   @value FILTERED
 *!     This strategy is intented for data created by a filter or
 *!     predictor and will put more emphasis on huffman encoding and
 *!     less on LZ string matching. This is between DEFAULT_STRATEGY
 *!     and HUFFMAN_ONLY.
 *!   @value RLE
 *!     This strategy is even closer to the HUFFMAN_ONLY in that it
 *!     only looks at the latest byte in the window, i.e. a window
 *!     size of 1 byte is sufficient for decompression. This mode is
 *!     not available in all zlib versions.
 *!   @value HUFFMAN_ONLY
 *!     This strategy will turn of string matching completely, only
 *!     doing huffman encoding. Window size doesn't matter in this
 *!     mode and the data can be decompressed with a zero size window.
 *!   @value FIXED
 *!     In this mode dynamic huffman codes are disabled, allowing for
 *!     a simpler decoder for special applications. This mode is not
 *!     available in all zlib versions.
 *! @endint
 *!
 *! @param window_size
 *!   Defines the size of the LZ77 window from 256 bytes to 32768
 *!   bytes, expressed as 2^x.
 */
static void gz_deflate_create(INT32 args)
{
  int tmp, wbits = 15;
  int strategy = Z_DEFAULT_STRATEGY;
  THIS->level=Z_DEFAULT_COMPRESSION;

  if(THIS->gz.state)
  {
/*     mt_lock(& THIS->lock); */
    deflateEnd(&THIS->gz);
/*     mt_unlock(& THIS->lock); */
  }

  do_free_string(THIS->dict);
  THIS->dict = NULL;

  if(args>2)
  {
    if(TYPEOF(sp[2-args]) != T_INT)
      Pike_error("Bad argument 3 to gz->create()\n");
    wbits = sp[2-args].u.integer;
    if( wbits == 0 ) wbits = 15;
    if( wbits < 8 || wbits > 15 )
      Pike_error("Invalid window size for gz_deflate->create().\n");
  }

#define TTS(type)	(((type) == PIKE_T_STRING && "string")	\
		      || ((type) == PIKE_T_MAPPING && "mapping")\
		      || ((type) == PIKE_T_ARRAY && "array")	\
		      || ((type) == PIKE_T_FLOAT && "float")	\
		      || ((type) == PIKE_T_INT && "int")	\
		      || ((type) == PIKE_T_OBJECT && "object")	\
		      || "mixed")
#define GET_TYPE(type, name)	((tmp = simple_mapping_string_lookup(m, name)) \
   && (TYPEOF(*(tmp)) == PIKE_T_##type || (Pike_error("Expected type %s,"\
       "got type %s for " name ".", TTS(PIKE_T_##type), TTS(TYPEOF(*tmp))), 0)))
  if(args)
  {
    if (TYPEOF(sp[-args]) == T_MAPPING && args == 1) {
      struct mapping *m = sp[-args].u.mapping;
      struct svalue *tmp;

      if (GET_TYPE(INT, "strategy")) strategy = tmp->u.integer;
      if (GET_TYPE(INT, "window_size"))
      {
	wbits = tmp->u.integer;
	if (wbits == 0) wbits = 15;
	if (wbits < 8 || wbits > 15)
	  Pike_error("Invalid window size for gz_deflate->create().\n");
      }
      if (GET_TYPE(STRING, "dictionary"))
      {
	if (tmp->u.string->size_shift)
	  Pike_error("dictionary cannot be a wide string in "
		     "gz_deflate->create().\n");
	THIS->dict = tmp->u.string;
	add_ref(THIS->dict);
      }
      if (GET_TYPE(INT, "level"))
      {
	  THIS->level = tmp->u.integer;
	  goto LVL_CHECK;
      }
    } else {
      if(TYPEOF(sp[-args]) != T_INT)
	Pike_error("Bad argument 1 to gz->create()\n");
      THIS->level=sp[-args].u.integer;
LVL_CHECK:
      if( THIS->level < 0 )
      {
	wbits = -wbits;
	THIS->level = -THIS->level;
      }
      if(THIS->level < Z_NO_COMPRESSION ||
	 THIS->level > Z_BEST_COMPRESSION)
      {
	Pike_error("Compression level out of range for "
		   "gz_deflate->create()\n");
      }
    }
  }

  if(args>1)
  {
    if(TYPEOF(sp[1-args]) != T_INT)
      Pike_error("Bad argument 2 to gz->create()\n");
    strategy=sp[1-args].u.integer;
    if(strategy != Z_DEFAULT_STRATEGY &&
       strategy != Z_FILTERED &&
#ifdef Z_RLE
       strategy != Z_RLE &&
#endif
#ifdef Z_FIXED
       strategy != Z_FIXED &&
#endif
       strategy != Z_HUFFMAN_ONLY)
    {
      Pike_error("Invalid compression strategy for gz_deflate->create()\n");
    }
  }

  THIS->gz.zalloc=Z_NULL;
  THIS->gz.zfree=Z_NULL;
  THIS->gz.opaque=(void *)THIS;

  pop_n_elems(args);
/*   mt_lock(& THIS->lock); */
  do {
    tmp=deflateInit2(&THIS->gz, THIS->level, Z_DEFLATED, wbits, 9, strategy );
    if (tmp == Z_STREAM_ERROR) {
      /* zlib 1.1.4's deflateInit2() only supports wbits 9..15 (not 8). */
      if (wbits == -8) wbits = -9;
      else if (wbits == 8) wbits = 9;
      else break;
      continue;
    }
    break;
  } while(1);
/*   mt_unlock(& THIS->lock); */
  switch(tmp)
  {
  case Z_OK:
    if (THIS->dict) {
      int err;
      err = deflateSetDictionary(&THIS->gz, (const Bytef*)THIS->dict->str,
				 THIS->dict->len);
      if (err != Z_OK) {
	Pike_error("failed to set dictionary in deflate init.\n");
      }
    }
    return;

  case Z_VERSION_ERROR:
    Pike_error("libz not compatible with zlib.h!!!\n");
    break;

  case Z_MEM_ERROR:
    Pike_error ("Out of memory while initializing Gz.deflate.\n");
    break;

  default:
    if(THIS->gz.msg)
      Pike_error("Failed to initialize Gz.deflate: %s\n",THIS->gz.msg);
    else
      Pike_error("Failed to initialize Gz.deflate (%d).\n", tmp);
  }
}


#ifdef _REENTRANT
static void do_mt_unlock (PIKE_MUTEX_T *lock)
{
  mt_unlock (lock);
}
#endif

static int do_deflate(dynamic_buffer *buf,
		      struct zipper *this,
		      int flush)
{
   int ret=0;

#ifdef _REENTRANT
   ONERROR uwp;
   THREADS_ALLOW();
   mt_lock(& this->lock);
   THREADS_DISALLOW();
   SET_ONERROR (uwp, do_mt_unlock, &this->lock);
#endif

   if(!this->gz.state)
      ret=Z_STREAM_ERROR;
   else
      do
      {
	 this->gz.next_out=(Bytef *)low_make_buf_space(
	    /* recommended by the zlib people */
	    (this->gz.avail_out =
	     this->gz.avail_in ?
	     this->gz.avail_in+this->gz.avail_in/1000+42 :
	      4096),
	    buf);

	 THREADS_ALLOW();
	 ret=deflate(& this->gz, flush);
	 THREADS_DISALLOW();

	 /* Absorb any unused space /Hubbe */
	 low_make_buf_space(-((ptrdiff_t)this->gz.avail_out), buf);

	 if(ret == Z_BUF_ERROR) ret=Z_OK;
      }
      while (ret==Z_OK && (this->gz.avail_in || !this->gz.avail_out));

#ifdef _REENTRANT
   CALL_AND_UNSET_ONERROR (uwp);
#endif
   return ret;
}

void zlibmod_pack(struct pike_string *data, dynamic_buffer *buf,
		  int level, int strategy, int wbits)
{
  struct zipper z;
  int ret;

  if(level < Z_NO_COMPRESSION ||
     level > Z_BEST_COMPRESSION)
    Pike_error("Compression level out of range for pack. %d %d %d\n",
               Z_DEFAULT_COMPRESSION, Z_NO_COMPRESSION, Z_BEST_COMPRESSION);

  if(strategy != Z_DEFAULT_STRATEGY &&
     strategy != Z_FILTERED &&
#ifdef Z_RLE
     strategy != Z_RLE &&
#endif
#ifdef Z_FIXED
     strategy != Z_FIXED &&
#endif
     strategy != Z_HUFFMAN_ONLY)
    Pike_error("Invalid compression strategy %d for pack.\n", strategy);

  if( wbits<0 ? (wbits<-15 || wbits>-8) : (wbits<8 || wbits>15 ) )
    Pike_error("Invalid window size value %d for pack.\n", wbits);

  memset(&z, 0, sizeof(z));
  z.gz.zalloc = Z_NULL;
  z.gz.zfree = Z_NULL;

  z.gz.next_in = (Bytef *)data->str;
  z.gz.avail_in = (unsigned INT32)(data->len);

  do {
    ret = deflateInit2(&z.gz, level, Z_DEFLATED, wbits, 9, strategy);
    if (ret == Z_STREAM_ERROR) {
      /* zlib 1.1.4's deflateInit2() only supports wbits 9..15 (not 8). */
      if (wbits == -8) wbits = -9;
      else if (wbits == 8) wbits = 9;
      else break;
      continue;
    }
    break;
  } while(1);

  switch(ret)
  {
  case Z_OK:
    break;

  case Z_VERSION_ERROR:
    Pike_error("libz not compatible with zlib.h!!!\n");
    break;

  case Z_MEM_ERROR:
    Pike_error ("Out of memory while initializing Gz.compress.\n");
    break;

  default:
    deflateEnd(&z.gz);
    if(z.gz.msg)
      Pike_error("Failed to initialize Gz.compress: %s\n", z.gz.msg);
    else
      Pike_error("Failed to initialize Gz.compress (%d).\n", ret);
  }

  mt_init(&z.lock);

  ret = do_deflate(buf, &z, Z_FINISH);

  deflateEnd(&z.gz);
  mt_destroy(&z.lock);

  if(ret != Z_STREAM_END)
    Pike_error("Error while deflating data (%d).\n",ret);
}

/*! @endclass
 */

/*! @decl string(8bit) compress(string(8bit) data, void|int(0..1) raw, @
 *!                             void|int(0..9) level, void|int strategy, @
 *!                             void|int(8..15) window_size)
 *!
 *! Encodes and returns the input @[data] according to the deflate
 *! format defined in RFC 1951.
 *!
 *! @param data
 *!   The data to be encoded.
 *!
 *! @param raw
 *!   If set, the data is encoded without the header and footer
 *!   defined in RFC 1950. Example of uses is the ZIP container
 *!   format.
 *!
 *! @param level
 *!   Indicates the level of effort spent to make the data compress
 *!   well. Zero means no packing, 2-3 is considered 'fast', 6 is
 *!   default and higher is considered 'slow' but gives better
 *!   packing.
 *!
 *! @param strategy
 *!   The strategy to be used when compressing the data. One of the
 *!   following.
 *! @int
 *!   @value DEFAULT_STRATEGY
 *!     The default strategy as selected in the zlib library.
 *!   @value FILTERED
 *!     This strategy is intented for data created by a filter or
 *!     predictor and will put more emphasis on huffman encoding and
 *!     less on LZ string matching. This is between DEFAULT_STRATEGY
 *!     and HUFFMAN_ONLY.
 *!   @value RLE
 *!     This strategy is even closer to the HUFFMAN_ONLY in that it
 *!     only looks at the latest byte in the window, i.e. a window
 *!     size of 1 byte is sufficient for decompression. This mode is
 *!     not available in all zlib versions.
 *!   @value HUFFMAN_ONLY
 *!     This strategy will turn of string matching completely, only
 *!     doing huffman encoding. Window size doesn't matter in this
 *!     mode and the data can be decompressed with a zero size window.
 *!   @value FIXED
 *!     In this mode dynamic huffman codes are disabled, allowing for
 *!     a simpler decoder for special applications. This mode is not
 *!     available in all zlib versions.
 *! @endint
 *!
 *! @param window_size
 *!   Defines the size of the LZ77 window from 256 bytes to 32768
 *!   bytes, expressed as 2^x.
 *!
 *! @seealso
 *!   @[deflate], @[inflate], @[uncompress]
 */
static void gz_compress(INT32 args)
{
  struct pike_string *data;
  dynamic_buffer buf;
  ONERROR err;

  int wbits = 15;
  int raw = 0;
  int level = 8;
  int strategy = Z_DEFAULT_STRATEGY;

  get_all_args("compress", args, "%n.%d%d%d%d", &data, &raw, &level, &strategy,
               &wbits);

  if( !wbits )
    wbits = 15;

  if( raw )
    wbits = -wbits;

  initialize_buf(&buf);
  SET_ONERROR(err, toss_buffer, &buf);
  zlibmod_pack(data, &buf, level, strategy, wbits);
  UNSET_ONERROR(err);

  pop_n_elems(args);
  push_string(low_free_buf(&buf));
}

/*! @class deflate
 */

/*! @decl string(8bit) deflate(string(8bit) data, int|void flush)
 *!
 *! This function performs gzip style compression on a string @[data] and
 *! returns the packed data. Streaming can be done by calling this
 *! function several times and concatenating the returned data.
 *!
 *! The optional argument @[flush] should be one of the following:
 *! @int
 *!   @value Gz.NO_FLUSH
 *!     Only data that doesn't fit in the internal buffers is returned.
 *!   @value Gz.PARTIAL_FLUSH
 *!     All input is packed and returned.
 *!   @value Gz.SYNC_FLUSH
 *!     All input is packed and returned.
 *!   @value Gz.FINISH
 *!     All input is packed and an 'end of data' marker is appended.
 *! @endint
 *!
 *! @seealso
 *! @[Gz.inflate->inflate()]
 */
static void gz_deflate(INT32 args)
{
  struct pike_string *data;
  int flush, fail;
  struct zipper *this=THIS;
  dynamic_buffer buf;
  ONERROR err;

  if(THIS->state == 1)
  {
    deflateEnd(& THIS->gz);
    deflateInit(& THIS->gz, THIS->level);
    THIS->state=0;
  }

  if(!THIS->gz.state)
    Pike_error("gz_deflate not initialized or destructed\n");

  if(args<1)
    Pike_error("Too few arguments to gz_deflate->deflate()\n");

  if(TYPEOF(sp[-args]) != T_STRING)
    Pike_error("Bad argument 1 to gz_deflate->deflate()\n");

  data=sp[-args].u.string;
  if (data->size_shift)
    Pike_error("Cannot input wide string to gz_deflate->deflate()\n");
  
  if(args>1)
  {
    if(TYPEOF(sp[1-args]) != T_INT)
      Pike_error("Bad argument 2 to gz_deflate->deflate()\n");
    
    flush=sp[1-args].u.integer;

    switch(flush)
    {
    case Z_PARTIAL_FLUSH:
    case Z_FINISH:
    case Z_SYNC_FLUSH:
    case Z_NO_FLUSH:
      break;

    default:
      Pike_error("Argument 2 to gz_deflate->deflate() out of range.\n");
    }
  }else{
    flush=Z_FINISH;
  }

  this->gz.next_in=(Bytef *)data->str;
  this->gz.avail_in = DO_NOT_WARN((unsigned INT32)(data->len));

  initialize_buf(&buf);

  SET_ONERROR(err,toss_buffer,&buf);
  fail=do_deflate(&buf,this,flush);
  UNSET_ONERROR(err);
  
  if(fail != Z_OK && fail != Z_STREAM_END)
  {
    toss_buffer(&buf);
    if(THIS->gz.msg)
      Pike_error("Error in gz_deflate->deflate(): %s\n",THIS->gz.msg);
    else
      Pike_error("Error in gz_deflate->deflate(): %d\n",fail);
  }

  if(fail == Z_STREAM_END)
    THIS->state=1;

  pop_n_elems(args);

  push_string(low_free_buf(&buf));
}


static void init_gz_deflate(struct object *UNUSED(o))
{
  mt_init(& THIS->lock);
  memset(& THIS->gz, 0, sizeof(THIS->gz));
  THIS->gz.zalloc=Z_NULL;
  THIS->gz.zfree=Z_NULL;
  THIS->gz.opaque=(void *)THIS;
  THIS->state=0;
  deflateInit(& THIS->gz, THIS->level = Z_DEFAULT_COMPRESSION);
  THIS->epilogue = NULL;
}

static void exit_gz_deflate(struct object *UNUSED(o))
{
/*   mt_lock(& THIS->lock); */
  deflateEnd(&THIS->gz);
  do_free_string(THIS->epilogue);
  do_free_string(THIS->dict);
  THIS->dict = NULL;
/*   mt_unlock(& THIS->lock); */
  mt_destroy( & THIS->lock );
}

/*! @endclass
 */

/*******************************************************************/

/*! @class inflate
 *!
 *! This class interfaces with the uncompression routines in the libz library.
 *!
 *! @note
 *! This program is only available if libz was available and found when
 *! Pike was compiled.
 *!
 *! @seealso
 *!   @[deflate], @[compress], @[uncompress]
 */

/*! @decl void create(int|void window_size)
 *! @decl void create(mapping options)
 *!
 *! If called with a mapping as only argument, @expr{create@} accepts
 *! the entries @expr{window_size@} (described below) and
 *! @expr{dictionary@}, which is a string to be set as dictionary.
 *!
 *! The window_size value is passed down to inflateInit2 in zlib.
 *!
 *! If the argument is negative, no header checks are done, and no
 *! verification of the data will be done either. This is needed for
 *! uncompressing ZIP-files, as an example. The negative value is then
 *! negated, and handled as a positive value.
 *!
 *! Positive arguments set the maximum dictionary size to an exponent
 *! of 2, such that 8 (the minimum) will cause the window size to be
 *! 256, and 15 (the maximum, and default value) will cause it to be
 *! 32Kb. Setting this to anything except 15 is rather pointless in
 *! Pike.
 *!
 *! It can be used to limit the amount of memory that is used to
 *! uncompress files, but 32Kb is not all that much in the great
 *! scheme of things.
 *!
 *! To decompress files compressed with level 9 compression, a 32Kb
 *! window size is needed. level 1 compression only requires a 256
 *! byte window.
 *!
 *! If the @[options] version is used you can specify your own dictionary in addition to the window size.
 *!
 *! @mapping options
 *! @member string dictionary
 *! @member int window_size
 *! @endmapping
 */
static void gz_inflate_create(INT32 args)
{
  int tmp, *tmp_p = &tmp;
  if(THIS->gz.state)
  {
/*     mt_lock(& THIS->lock); */
    inflateEnd(&THIS->gz);
/*     mt_unlock(& THIS->lock); */
  }


  THIS->gz.zalloc=Z_NULL;
  THIS->gz.zfree=Z_NULL;
  THIS->gz.opaque=(void *)THIS;
  if( args  && TYPEOF(Pike_sp[-1]) == PIKE_T_MAPPING)
  {
    struct mapping *m = Pike_sp[-1].u.mapping;
    struct svalue *tmp;

    if (GET_TYPE(STRING, "dictionary")) {
      if (tmp->u.string->size_shift)
	Pike_error("dictionary cannot be a wide string in "
		   "gz_inflate->create().\n");
      THIS->dict = tmp->u.string;
      add_ref(THIS->dict);
    }
    if (GET_TYPE(INT, "window_size"))
      *tmp_p=inflateInit2(& THIS->gz, tmp->u.integer);
    else
      *tmp_p=inflateInit( &THIS->gz );
  }
  else
  {
    if( args  && TYPEOF(Pike_sp[-1]) == PIKE_T_INT )
    {
      tmp=inflateInit2(& THIS->gz, Pike_sp[-1].u.integer);
    }
    else
    {
      tmp=inflateInit( &THIS->gz );
    }
  }
  pop_n_elems(args);

/*    mt_lock(& THIS->lock);  */
/*    mt_unlock(& THIS->lock); */
  switch(tmp)
  {
  case Z_OK:
#if 0 /* this apparently works with newer zlibs only. */
    if (THIS->dict) {
      int err;

      err = inflateSetDict(&THIS->gz, (const Bytef*)THIS->dict->str,
			   THIS->dict->len);

      if (err != Z_OK)
	    Pike_error("inflateSetDict on startup failed.\n");
    }
#endif
    return;

  case Z_VERSION_ERROR:
    Pike_error("libz not compatible with zlib.h!!!\n");
    break;

  case Z_MEM_ERROR:
    Pike_error ("Out of memory while initializing Gz.inflate.\n");
    break;

  default:
    if(THIS->gz.msg)
      Pike_error("Failed to initialize Gz.inflate: %s\n",THIS->gz.msg);
    else
      Pike_error("Failed to initialize Gz.inflate (%d).\n", tmp);
  }
}

static int do_inflate(dynamic_buffer *buf,
		      struct zipper *this,
		      int flush)
{
  int fail=0;

#ifdef _REENTRANT
  ONERROR uwp;
  THREADS_ALLOW();
  mt_lock(& this->lock);
  THREADS_DISALLOW();
  SET_ONERROR (uwp, do_mt_unlock, &this->lock);
#endif

  if(!this->gz.state)
  {
    fail=Z_STREAM_ERROR;
  }else{
#if 0
  static int fnord=0;
  fnord++;
#endif

    do
    {
      char *loc;
      int ret;
      loc=low_make_buf_space(BUF,buf);
      THREADS_ALLOW();
      this->gz.next_out=(Bytef *)loc;
      this->gz.avail_out=BUF;
#if 0
      fprintf(stderr,"INFLATE[%d]: avail_out=%7d  avail_in=%7d flush=%d\n",
	      fnord,
	      this->gz.avail_out,
	      this->gz.avail_in,
	      flush);
      fprintf(stderr,"INFLATE[%d]: mode=%d\n",fnord,
	      this->gz.state ? *(int *)(this->gz.state) : -1);
#endif
	      
      ret=inflate(& this->gz, flush);
#if 0
      fprintf(stderr,"Result [%d]: avail_out=%7d  avail_in=%7d  ret=%d\n",
	      fnord,
	      this->gz.avail_out,
	      this->gz.avail_in,
	      ret);
#endif

      THREADS_DISALLOW();
      low_make_buf_space(-((ptrdiff_t)this->gz.avail_out), buf);

      if(ret == Z_BUF_ERROR) ret=Z_OK;

      if (ret == Z_NEED_DICT && this->dict)
	ret = inflateSetDictionary(&this->gz,
				   (const Bytef*)this->dict->str,
				   this->dict->len);

      if(ret != Z_OK)
      {
	fail=ret;
	break;
      }
    } while(!this->gz.avail_out || flush==Z_FINISH || this->gz.avail_in);
  }

#ifdef _REENTRANT
  CALL_AND_UNSET_ONERROR (uwp);
#endif
  return fail;
}

void zlibmod_unpack(struct pike_string *data, dynamic_buffer *buf, int raw)
{
  struct zipper z;
  int ret;

  memset(&z, 0, sizeof(z));
  z.gz.zalloc = Z_NULL;
  z.gz.zfree = Z_NULL;

  z.gz.next_in=(Bytef *)data->str;
  z.gz.avail_in = DO_NOT_WARN((unsigned INT32)(data->len));

  if( raw )
    ret = inflateInit2(&z.gz, -15);
  else
    ret = inflateInit( &z.gz );

  switch(ret)
  {
  case Z_OK:
    break;

  case Z_VERSION_ERROR:
    Pike_error("libz not compatible with zlib.h!!!\n");
    break;

  case Z_MEM_ERROR:
    Pike_error ("Out of memory while initializing Gz.uncompress.\n");
    inflateEnd(&z.gz);
    break;

  default:
    inflateEnd( &z.gz );
    if(z.gz.msg)
      Pike_error("Failed to initialize Gz.uncompress: %s\n", z.gz.msg);
    else
      Pike_error("Failed to initialize Gz.uncompress (%d).\n", ret);
  }

  mt_init(&z.lock);
  ret = do_inflate(buf, &z, Z_SYNC_FLUSH);
  mt_destroy(&z.lock);
  inflateEnd( &z.gz );

  if(ret==Z_OK)
    Pike_error("Compressed data is truncated.\n");
  if(ret!=Z_STREAM_END)
    Pike_error("Failed to inflate data (%d).\n", ret);
}

/*! @endclass
*/

/*! @decl string(8bit) uncompress(string(8bit) data, void|int(0..1) raw)
 *!
 *! Uncompresses the @[data] and returns it. The @[raw] parameter
 *! tells the decoder that the indata lacks the data header and footer
 *! defined in RFC 1950.
 */
static void gz_uncompress(INT32 args)
{
  dynamic_buffer buf;
  ONERROR err;
  int raw = 0;

  if(args<1)
    SIMPLE_TOO_FEW_ARGS_ERROR("uncompress", 1);
  if(TYPEOF(Pike_sp[-args]) != PIKE_T_STRING)
    SIMPLE_BAD_ARG_ERROR("uncompress", 1, "string");
  if (Pike_sp[-args].u.string->size_shift)
    Pike_error("Cannot input wide string to uncompress\n");
  if(args>1)
  {
    if(TYPEOF(Pike_sp[1-args]) == PIKE_T_INT)
      raw = Pike_sp[1-args].u.integer;
    else
      SIMPLE_BAD_ARG_ERROR("uncompress", 2, "int");
  }

  initialize_buf(&buf);
  SET_ONERROR(err, toss_buffer, &buf);
  zlibmod_unpack(Pike_sp[-args].u.string, &buf, raw);
  UNSET_ONERROR(err);

  pop_n_elems(args);
  push_string(low_free_buf(&buf));

}

/*! @class inflate
 */

/*! @decl string(8bit) inflate(string(8bit) data)
 *!
 *! This function performs gzip style decompression. It can inflate
 *! a whole file at once or in blocks.
 *!
 *! @example
 *! // whole file
 *! @code
 *! write(Gz.inflate()->inflate(stdin->read(0x7fffffff));
 *!
 *! // streaming (blocks)
 *! function inflate=Gz.inflate()->inflate;
 *! while(string s=stdin->read(8192))
 *!   write(inflate(s));
 *! @endcode
 *!
 *! @seealso
 *! @[Gz.deflate->deflate()], @[Gz.decompress]
 */
static void gz_inflate(INT32 args)
{
  struct pike_string *data;
  int fail;
  struct zipper *this=THIS;
  dynamic_buffer buf;
  ONERROR err;

  if(!THIS->gz.state)
    Pike_error("gz_inflate not initialized or destructed\n");

  if(args<1)
    Pike_error("Too few arguments to gz_inflate->inflate()\n");

  if(TYPEOF(sp[-args]) != T_STRING)
    Pike_error("Bad argument 1 to gz_inflate->inflate()\n");

  data=sp[-args].u.string;
  if (data->size_shift)
    Pike_error("Cannot input wide string to gz_inflate->inflate()\n");

  this->gz.next_in=(Bytef *)data->str;
  this->gz.avail_in = DO_NOT_WARN((unsigned INT32)(data->len));

  initialize_buf(&buf);

  SET_ONERROR(err,toss_buffer,&buf);
  fail=do_inflate(&buf,this,Z_SYNC_FLUSH);
  UNSET_ONERROR(err);

  if(fail != Z_OK && fail != Z_STREAM_END)
  {
    toss_buffer(&buf);
    if(THIS->gz.msg)
      Pike_error("Error in gz_inflate->inflate(): %s\n",THIS->gz.msg);
    else
      Pike_error("Error in gz_inflate->inflate(): %d\n",fail);
  }

  pop_n_elems(args);

  push_string(low_free_buf(&buf));

  if(fail == Z_STREAM_END)
  {
    struct pike_string *old_epilogue = this->epilogue;
    if(old_epilogue) {
      push_string(old_epilogue);
      this->epilogue = NULL;
    }
    push_string(make_shared_binary_string((const char *)this->gz.next_in,
					  this->gz.avail_in));
    if(old_epilogue)
      f_add(2);
    if(TYPEOF(sp[-1]) == PIKE_T_STRING)
      this->epilogue = (--sp)->u.string;
    else
      pop_stack();
  }

  if(fail != Z_STREAM_END && fail!=Z_OK && !sp[-1].u.string->len)
  {
    pop_stack();
    push_int(0);
  }
}

/*! @decl string(8bit) end_of_stream()
 *!
 *! This function returns 0 if the end of stream marker has not yet
 *! been encountered, or a string (possibly empty) containg any extra data
 *! received following the end of stream marker if the marker has been
 *! encountered.  If the extra data is not needed, the result of this
 *! function can be treated as a logical value.
 */
static void gz_end_of_stream(INT32 args)
{
  struct zipper *this=THIS;
  pop_n_elems(args);
  if(this->epilogue)
    ref_push_string(this->epilogue);
  else
    push_int(0);
}

static void init_gz_inflate(struct object *UNUSED(o))
{
  mt_init(& THIS->lock);
  memset(& THIS->gz, 0, sizeof(THIS->gz));
  THIS->gz.zalloc=Z_NULL;
  THIS->gz.zfree=Z_NULL;
  THIS->gz.opaque=(void *)THIS;
  inflateInit(&THIS->gz);
  inflateEnd(&THIS->gz);
  THIS->epilogue = NULL;
}

static void exit_gz_inflate(struct object *UNUSED(o))
{
/*   mt_lock(& THIS->lock); */
  inflateEnd(& THIS->gz);
  do_free_string(THIS->epilogue);
  do_free_string(THIS->dict);
  THIS->dict = NULL;
/*   mt_unlock(& THIS->lock); */
  mt_destroy( & THIS->lock );
}

/*! @endclass
 */

/*! @decl int crc32(string(8bit) data, void|int start_value)
 *!
 *!   This function calculates the standard ISO3309 Cyclic Redundancy Check.
 */
static void gz_crc32(INT32 args)
{
   unsigned INT32 crc;
   if (!args || TYPEOF(sp[-args]) != T_STRING)
      Pike_error("Gz.crc32: illegal or missing argument 1 (expected string)\n");
  if (sp[-args].u.string->size_shift)
    Pike_error("Cannot input wide string to Gz.crc32\n");

   if (args>1) {
      if (TYPEOF(sp[1-args]) != T_INT)
	 Pike_error("Gz.crc32: illegal argument 2 (expected integer)\n");
      else
	 crc=(unsigned INT32)sp[1-args].u.integer;
   } else
      crc=0;
	 
   crc=crc32(crc,
	     (unsigned char*)sp[-args].u.string->str,
	     DO_NOT_WARN((unsigned INT32)(sp[-args].u.string->len)));

   pop_n_elems(args);
   push_int((INT32)crc);
}

/*! @endmodule
 */
#endif

PIKE_MODULE_EXIT {}

static void gz_deflate_size( INT32 args )
{
#define L_CODES (256 + 29 + 1)
#define HEAP_SIZE (2*L_CODES+1)

    pop_n_elems(args);
    /* estimation, from reading deflate.h. We could get a rather exact
     * value if we saved memlevel + windowbits somewhere.
     */
    push_int(
        /* internal_state structure, estimating alignment */
        (47 * 4) + sizeof(void*)*16 + (4*HEAP_SIZE) + (4*121) + (4*(2*19+1)) + 16*2 + 
        (2*L_CODES+1)*5 +
        /* d_buf <memlevel+6>, l_buf<memlevel+6>, window<windowbits>, hash<memlevel+7> */
        16384*2 + 16384*2 + 65536*2 + 32768*2);
}

static void gz_inflate_size( INT32 args )
{
    pop_n_elems(args);
    /* low-order estimation, from reading inflate.h */
    push_int(
        /* inflate_state structure. */
        ((28 * sizeof(void*)) + (320 * 2) + (288 * 2)) +
        /* codes[] */
        (2048 * 4) +
        /* window, really 1<<wbits, max wbits is 16. */
        65536);
}

PIKE_MODULE_INIT
{
#ifdef HAVE_ZLIB_H
  struct z_stream_s z;	/* Used to detect support for extensions. */
  int have_rle = 0;
  int have_fixed = 0;

  start_new_program();
  ADD_STORAGE(struct zipper);
  
  /* function(int|void,int|void:void) */
  ADD_FUNCTION("create",gz_deflate_create,tFunc(tOr(tMapping, tOr(tInt,tVoid)) tOr(tInt,tVoid),tVoid),0);
  /* function(string(8bit),int|void:string(8bit)) */
  ADD_FUNCTION("deflate",gz_deflate,tFunc(tStr8 tOr(tInt,tVoid),tStr8),0);
  ADD_FUNCTION("_size_object", gz_deflate_size, tFunc(tVoid,tInt), 0);

  add_integer_constant("NO_FLUSH",Z_NO_FLUSH,0);
  add_integer_constant("PARTIAL_FLUSH",Z_PARTIAL_FLUSH,0);
  add_integer_constant("SYNC_FLUSH",Z_SYNC_FLUSH,0);
  add_integer_constant("FINISH",Z_FINISH,0);
  add_integer_constant("DEFAULT_STRATEGY", Z_DEFAULT_STRATEGY,0);
  add_integer_constant("FILTERED", Z_FILTERED,0);
  add_integer_constant("HUFFMAN_ONLY", Z_HUFFMAN_ONLY,0);

  memset(&z, 0, sizeof(z));
#ifdef Z_RLE
  if (deflateInit2(&z, 8, Z_DEFLATED, 9, 9, Z_RLE) == Z_OK) {
    have_rle = 1;
    deflateEnd(&z);
    add_integer_constant("RLE", Z_RLE,0);
  }
#endif
#ifdef Z_FIXED
  if (deflateInit2(&z, 8, Z_DEFLATED, 9, 9, Z_FIXED) == Z_OK) {
    have_fixed = 1;
    deflateEnd(&z);
    add_integer_constant("FIXED", Z_FIXED,0);
  }
#endif

  set_init_callback(init_gz_deflate);
  set_exit_callback(exit_gz_deflate);

  end_class("deflate",0);

  start_new_program();
  ADD_STORAGE(struct zipper);
  
  /* function(int|void:void) */
  ADD_FUNCTION("create",gz_inflate_create,tFunc(tOr(tMapping,tOr(tInt,tVoid)),tVoid),0);
  /* function(string(8bit):string(8bit)) */
  ADD_FUNCTION("inflate",gz_inflate,tFunc(tStr8,tStr8),0);
  /* function(:string(8bit)) */
  ADD_FUNCTION("end_of_stream",gz_end_of_stream,tFunc(tNone,tStr8),0);
  ADD_FUNCTION("_size_object", gz_inflate_size, tFunc(tVoid,tInt), 0);

  add_integer_constant("NO_FLUSH",Z_NO_FLUSH,0);
  add_integer_constant("PARTIAL_FLUSH",Z_PARTIAL_FLUSH,0);
  add_integer_constant("SYNC_FLUSH",Z_SYNC_FLUSH,0);
  add_integer_constant("FINISH",Z_FINISH,0);

  set_init_callback(init_gz_inflate);
  set_exit_callback(exit_gz_inflate);

  end_class("inflate",0);

  add_integer_constant("NO_FLUSH",Z_NO_FLUSH,0);
  add_integer_constant("PARTIAL_FLUSH",Z_PARTIAL_FLUSH,0);
  add_integer_constant("SYNC_FLUSH",Z_SYNC_FLUSH,0);
  add_integer_constant("FINISH",Z_FINISH,0);
  add_integer_constant("DEFAULT_STRATEGY", Z_DEFAULT_STRATEGY,0);
  add_integer_constant("FILTERED", Z_FILTERED,0);
  add_integer_constant("HUFFMAN_ONLY", Z_HUFFMAN_ONLY,0);
#ifdef Z_RLE
  if (have_rle) {
    add_integer_constant("RLE", Z_RLE,0);
  }
#endif
#ifdef Z_FIXED
  if (have_fixed) {
    add_integer_constant("FIXED", Z_FIXED,0);
  }
#endif

  /* function(string(8bit),void|int:int) */
  ADD_FUNCTION("crc32",gz_crc32,tFunc(tStr8 tOr(tVoid,tInt),tInt),0);

  /* function(string(8bit),void|int(0..1),void|int,void|int:string(8bit)) */
  ADD_FUNCTION("compress",gz_compress,tFunc(tStr8 tOr(tVoid,tInt01) tOr(tVoid,tInt09) tOr(tVoid,tInt) tOr(tVoid,tInt),tStr8),0);

  /* function(string(8bit),void|int(0..1):string(8bit)) */
  ADD_FUNCTION("uncompress",gz_uncompress,tFunc(tStr8 tOr(tVoid,tInt01),tStr8),0);

  PIKE_MODULE_EXPORT(Gz, crc32);
  PIKE_MODULE_EXPORT(Gz, zlibmod_pack);
  PIKE_MODULE_EXPORT(Gz, zlibmod_unpack);
#else
  if(!TEST_COMPAT(7,6))
    HIDE_MODULE();
#endif
}

#if defined(HAVE___VTBL__9TYPE_INFO) || defined(HAVE___T_9__NOTHROW)
/* Super-special kluge for IRIX 6.3 */
#ifdef HAVE___VTBL__9TYPE_INFO
extern void __vtbl__9type_info(void);
#endif /* HAVE___VTBL__9TYPE_INFO */
#ifdef HAVE___T_9__NOTHROW
extern void __T_9__nothrow(void);
#endif /* HAVE___T_9__NOTHROW */
/* Don't even think of calling this one
 * Not static, so the compiler can't optimize it away totally.
 */
void zlibmod_strap_kluge(void)
{
#ifdef HAVE___VTBL__9TYPE_INFO
  __vtbl__9type_info();
#endif /* HAVE___VTBL__9TYPE_INFO */
#ifdef HAVE___T_9__NOTHROW
  __T_9__nothrow();
#endif /* HAVE___T_9__NOTHROW */
}
#endif /* HAVE___VTBL__9TYPE_INFO || HAVE___T_9__NOTHROW */