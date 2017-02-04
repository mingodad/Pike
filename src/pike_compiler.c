/*
|| This file is part of Pike. For copyright information see COPYRIGHT.
|| Pike is distributed under GPL, LGPL and MPL. See the file COPYING
|| for more information.
*/

#include "global.h"
#include "program.h"
#include "object.h"
#include "buffer.h"
#include "pike_types.h"
#include "stralloc.h"
#include "las.h"
#include "lex.h"
#include "pike_macros.h"
#include "fsort.h"
#include "pike_error.h"
#include "docode.h"
#include "interpret.h"
#include "main.h"
#include "pike_memory.h"
#include "gc.h"
#include "threads.h"
#include "constants.h"
#include "operators.h"
#include "builtin_functions.h"
#include "mapping.h"
#include "cyclic.h"
#include "pike_types.h"
#include "opcodes.h"
#include "version.h"
#include "block_allocator.h"
#include "block_alloc.h"
#include "pikecode.h"
#include "pike_compiler.h"
#include "module_support.h"
#include "bitvector.h"
#include "sprintf.h"

#include <errno.h>
#include <fcntl.h>

#ifdef PIKE_THREADS
static COND_T Pike_compiler_cond;
static THREAD_T Pike_compiler_thread;
static int lock_depth = 0;

PMOD_EXPORT void lock_pike_compiler(void)
{
  if (lock_depth && (Pike_compiler_thread != th_self())) {
    SWAP_OUT_CURRENT_THREAD();
    while (lock_depth && (Pike_compiler_thread != th_self())) {
      co_wait_interpreter(&Pike_compiler_cond);
    }
    SWAP_IN_CURRENT_THREAD();
  }
  lock_depth++;
  Pike_compiler_thread = th_self();
}

PMOD_EXPORT void unlock_pike_compiler(void)
{
#ifdef PIKE_DEBUG
  if (lock_depth < 1) {
    Pike_fatal("Pike compiler running unlocked!\n");
  }
#endif
  lock_depth--;
  co_broadcast(&Pike_compiler_cond);
}
#else
PMOD_EXPORT void lock_pike_compiler(void)
{
}
PMOD_EXPORT void unlock_pike_compiler(void)
{
}
#endif

static void low_enter_compiler(struct object *ce, int inherit);

/* #define COMPILER_DEBUG */
/* #define PROGRAM_BUILD_DEBUG */

#ifdef COMPILER_DEBUG
#define CDFPRINTF(...)	fprintf(stderr, __VA_ARGS__)
#ifndef PIKE_THREADS
/* The CDFPRINTF lines wants to print lock_depth, so fake one of those */
static const int lock_depth = 1;
#endif
#else /* !COMPILER_DEBUG */
#define CDFPRINTF(...)
#endif /* COMPILER_DEBUG */

static struct program *reporter_program = NULL;
struct program *compilation_program = 0;
struct program *compilation_env_program = 0;
struct object *compilation_environment = NULL;

/*
 * Supporters.
 *
 * Supporters are used to register that a program being compiled depends on
 * another program that also is being compiled.
 *
 * Every program being compiled has a supporter (in the compilation
 * struct).
 */

struct Supporter *current_supporter=0;


#ifdef PIKE_DEBUG

struct supporter_marker
{
  struct supporter_marker *next;
  void *data;
  int level, verified;
};

#undef INIT_BLOCK
#define INIT_BLOCK(X) do { (X)->level = (X)->verified = 0; }while(0)
PTR_HASH_ALLOC(supporter_marker, 128);

static int supnum;

#define SNUM(X) (get_supporter_marker((X))->level)

static void mark_supporters(struct Supporter *s)
{
  struct supporter_marker *m;

  if(!s) return;
  debug_malloc_touch(s);
  m=get_supporter_marker(s);

  if(m->level) return;
  m->level = -1;

  if(s->magic != 0x500b0127)
  {
#ifdef DEBUG_MALLOC
    describe(s);
#endif
    Pike_fatal("This is not a supporter (addr=%p, magic=%x)!\n",s,s->magic);
  }

  mark_supporters(s->dependants);
  mark_supporters(s->next_dependant);

  m->level=supnum++;

  mark_supporters(s->previous);
  mark_supporters(s->depends_on);
}

static void low_verify_supporters(struct Supporter *s)
{
  struct Supporter *ss;
  struct supporter_marker *m;

  if(!s) return;
  debug_malloc_touch(s);
  m=get_supporter_marker(s);

  if(m->verified) return;
  m->verified = 1;

  low_verify_supporters(s->dependants);
  low_verify_supporters(s->next_dependant);

#if 0
  fprintf(stderr, "low_verify_supporters %p%s, level %d: "
	  "previous %p, depends_on %p, dependants %p, next_dependant %p\n",
	  s, s == current_supporter ? " == current_supporter" : "",
	  m->level, s->previous, s->depends_on, s->dependants, s->next_dependant);
#endif

  if(s->previous && SNUM(s->previous) <= m->level)
    Pike_fatal("Que, numbers out of whack1\n");

  if(s->depends_on && SNUM(s->depends_on) <= m->level)
    Pike_fatal("Que, numbers out of whack2\n");

  for(ss=s->dependants;ss;ss=ss->next_dependant) {
    if (ss->depends_on != s)
      Pike_fatal("Dependant hasn't got depends_on set properly.\n");
    if(SNUM(ss) >= m->level)
      Pike_fatal("Que, numbers out of whack3\n");
  }

  low_verify_supporters(s->previous);
  low_verify_supporters(s->depends_on);
}

void verify_supporters()
{
  if(d_flag)
  {
    supnum=1;
    init_supporter_marker_hash();

#if 0
    fprintf(stderr, "verify_supporters start\n");
#endif

    mark_supporters(current_supporter);
    low_verify_supporters(current_supporter);
#ifdef DO_PIKE_CLEANUP
    {
      size_t e=0;
      for(e=0;e<supporter_marker_hash_table_size;e++)
	while(supporter_marker_hash_table[e])
	  remove_supporter_marker(supporter_marker_hash_table[e]->data);
    }
#endif
    exit_supporter_marker_hash();

#if 0
    fprintf(stderr, "verify_supporters end\n");
#endif
  }
}
#else
#define verify_supporters();
#endif

void init_supporter(struct Supporter *s,
		    supporter_callback *fun,
		    void *data)
{
  CDFPRINTF("th(%ld) init_supporter() supporter=%p data=%p.\n",
            (long) th_self(), s, data);
  verify_supporters();
#ifdef PIKE_DEBUG
  s->magic = 0x500b0127;
#endif
  s->previous=current_supporter;
  current_supporter=s;

  s->depends_on=0;
  s->dependants=0;
  s->next_dependant=0;
  s->fun=fun;
  s->data=data;
  s->prog=0;
  verify_supporters();
}

int unlink_current_supporter(struct Supporter *c)
{
  int ret=0;
#ifdef PIKE_DEBUG
  if(c != current_supporter)
    Pike_fatal("Previous unlink failed.\n");
#endif
  debug_malloc_touch(c);
  verify_supporters();
  if(c->depends_on)
  {
#ifdef PIKE_DEBUG
    struct Supporter *s;
    for (s = c->depends_on->dependants; s; s = s->next_dependant)
      if (s == c) Pike_fatal("Dependant already linked in.\n");
#endif
    ret++;
    c->next_dependant = c->depends_on->dependants;
    c->depends_on->dependants=c;
    add_ref(c->self);
    CDFPRINTF("th(%ld) unlink_current_supporter() "
              "supporter=%p (prog %p) depends on %p (prog %p).\n",
              (long) th_self(), c, c->prog,
              c->depends_on, c->depends_on->prog);
  }
  current_supporter=c->previous;
  verify_supporters();
  return ret;
}

void free_supporter(struct Supporter *c)
{
  verify_supporters();
  if (c->depends_on) {
    struct Supporter **s;
    for (s = &c->depends_on->dependants; *s; s = &(*s)->next_dependant)
      if (*s == c) {*s = c->next_dependant; break;}
    c->depends_on = 0;
  }
  verify_supporters();
}

int call_dependants(struct Supporter *s, int finish)
{
  int ok = 1;
  struct Supporter *tmp;
  CDFPRINTF("th(%ld) call_dependants() supporter=%p (prog %p) "
            "finish=%d.\n", (long) th_self(), s, s->prog, finish);
  verify_supporters();
  while((tmp=s->dependants))
  {
    CDFPRINTF("th(%ld) dependant: %p (prog %p) (data:%p).\n",
              (long) th_self(), tmp, tmp->prog, tmp->data);
    s->dependants=tmp->next_dependant;
#ifdef PIKE_DEBUG
    tmp->next_dependant=0;
#endif
    verify_supporters();
    if (!tmp->fun(tmp->data, finish)) ok = 0;
    verify_supporters();
    free_object(tmp->self);
  }
  return ok;
}

int report_compiler_dependency(struct program *p)
{
  int ret=0;
  struct Supporter *c,*cc;

  if (p == Pike_compiler->new_program) {
    /* Depends on self... */
    return 0;
  }

  CDFPRINTF("th(%ld) compiler dependency on %p from %p\n",
            (long)th_self(), p, Pike_compiler->new_program);

  verify_supporters();
  if (Pike_compiler->flags & COMPILATION_FORCE_RESOLVE)
    return 0;
  for(cc=current_supporter;cc;cc=cc->previous)
  {
    if(cc->prog &&
       !(cc->prog->flags & PROGRAM_PASS_1_DONE))
    {
      c=cc->depends_on;
      if(!c) c=cc->previous;
      for(;c;c=c->previous)
      {
	if(c->prog == p)
	{
	  cc->depends_on=c;
          CDFPRINTF("th(%ld) supporter %p (prog %p) "
                    "now depends on %p (prog %p)\n",
                    (long) th_self(), cc, cc->prog, c, c->prog);
	  verify_supporters();
	  ret++; /* dependency registred */
	}
      }
    }
  }
  verify_supporters();
  return ret;
}

extern int yyparse(void);

static void do_yyparse(void)
{
  struct svalue *save_sp = Pike_sp;
  yyparse();  /* Parse da program */
  if (save_sp != Pike_sp) {
#ifdef PIKE_DEBUG
    if (!Pike_compiler->num_parse_error) {
      Pike_fatal("yyparse() left %"PRINTPTRDIFFT"d droppings on the stack!\n",
		 Pike_sp - save_sp);
    }
#endif
    pop_n_elems(Pike_sp - save_sp);
  }
}

/*! @class Reporter
 *!
 *!   API for reporting parse errors and similar.
 */

/*! @decl enum SeverityLevel
 *!   Message severity level.
 *! { NOTICE, WARNING, ERROR, FATAL }
 *!
 *! @constant NOTICE
 *! @constant WARNING
 *! @constant ERROR
 *! @constant FATAL
 *!
 *! @seealso
 *!   @[report()]
 */

/*! @decl void report(SeverityLevel severity, @
 *!                   string filename, int(1..) linenumber, @
 *!                   string subsystem, @
 *!                   string message, mixed ... extra_args)
 *!
 *!   Report a diagnostic from the compiler.
 *!
 *! @param severity
 *!   The severity of the diagnostic.
 *!
 *! @param filename
 *! @param linenumber
 *!   Location which triggered the diagnostic.
 *!
 *! @param subsystem
 *!   Compiler subsystem that generated the diagnostic.
 *!
 *! @param message
 *!   @[sprintf()]-style formatting string with the diagnostic message.
 *!
 *! @param extra_args
 *!   Extra arguments to @[sprintf()].
 *!
 *!   The default implementation does the following:
 *!
 *!   @ul
 *!     @item
 *!       If there's a @[MasterObject()->report()], call it
 *!       with the same arguments as ourselves.
 *!     @item
 *!       Otherwise depending on @[severity]:
 *!       @int
 *!         @value NOTICE
 *!           Ignored.
 *!         @value WARNING
 *!           Calls @[MasterObject()->compile_warning()].
 *!         @value ERROR
 *!         @value FATAL
 *!           Calls @[MasterObject()->compile_error()].
 *!       @endint
 *!   @endul
 *!
 *!   If there's no master object yet, the diagnostic is output to
 *!   @[Stdio.stderr].
 *!
 *! @note
 *!   In Pike 7.8 and earlier @[MasterObject()->report()] was not called.
 *!
 *! @seealso
 *!   @[PikeCompiler()->report()]
 */
/* NOTE: This function MUST NOT use any storage in the Reporter program! */
static void f_reporter_report(INT32 args)
{
  int level;
  struct pike_string *filename;
  INT_TYPE linenumber;
  struct pike_string *subsystem;
  struct pike_string *message;
  struct object *master_ob;

  if ((master_ob = get_master()) && master_ob->prog) {
    int fun = find_identifier("report", master_ob->prog);
    if (fun >= 0) {
      apply_low(master_ob, fun, args);
      return;
    }
  }

  if (args > 5) {
    f_sprintf(args - 4);
    args = 5;
  }
  get_all_args("report", args, "%d%W%+%W%W",
	       &level, &filename, &linenumber, &subsystem, &message);

  /* Ignore informational level messages */
  if (level >= REPORT_WARNING) {
    if (master_ob && master_ob->prog) {
      ref_push_string(filename);
      push_int(linenumber);
      ref_push_string(message);
      if (level >= REPORT_ERROR) {
	APPLY_MASTER("compile_error", 3);
	args++;
      } else {
	APPLY_MASTER("compile_warning", 3);
	args++;
      }
    } else {
      /* We hope that errors from compiling the master
       * won't contain wide-strings... */
      if (level >= REPORT_ERROR) {
	fprintf(stderr, "%s:%ld: %s\n",
		filename->str, (long)linenumber, message->str);
      } else {
	fprintf(stderr, "%s:%ld: Warning: %s\n",
		filename->str, (long)linenumber, message->str);
      }
      fflush(stderr);
    }
  }
  pop_n_elems(args);
  push_int(0);
}

/*! @endclass
 */

/*! @module DefaultCompilerEnvironment
 *!
 *!   The @[CompilerEnvironment] object that is used
 *!   for loading C-modules and by @[predef::compile()].
 *!
 *! @note
 *!   @[predef::compile()] is essentially an alias for the
 *!   @[CompilerEnvironment()->compile()] in this object.
 *!
 *! @seealso
 *!   @[CompilerEnvironment], @[predef::compile()]
 */

/*! @decl inherit CompilerEnvironment
 */

/*! @endmodule
 */

/*! @class CompilerEnvironment
 *!
 *!   The compiler environment.
 *!
 *!   By inheriting this class and overloading the functions,
 *!   it is possible to make a custom Pike compiler.
 *!
 *! @note
 *!   Prior to Pike 7.8 this sort of customization has to be done
 *!   either via custom master objects, or via @[CompilationHandler]s.
 *!
 *! @seealso
 *!   @[CompilationHandler], @[MasterObject], @[master()], @[replace_master()]
 */

/*! @decl inherit Reporter
 *!
 *! Implements the @[Reporter] API.
 *!
 *! @seealso
 *!   @[Reporter()->report()], @[Reporter()->SeverityLevel]
 */

/*! @class lock
 *!
 *! This class acts as a lock against other threads accessing the compiler.
 *!
 *! The lock is released when the object is destructed.
 */

static void compiler_environment_lock_event_handler(int e)
{
  switch(e) {
  case PROG_EVENT_INIT:
    lock_pike_compiler();
    break;
  case PROG_EVENT_EXIT:
    unlock_pike_compiler();
    break;
  }
}

/*! @endclass
 */

/*! @decl program compile(string source, CompilationHandler|void handler, @
 *!                       int|void major, int|void minor,@
 *!                       program|void target, object|void placeholder)
 *!
 *!   Compile a string to a program.
 *!
 *!   This function takes a piece of Pike code as a string and
 *!   compiles it into a clonable program.
 *!
 *!   The optional argument @[handler] is used to specify an alternative
 *!   error handler. If it is not specified the current master object will
 *!   be used.
 *!
 *!   The optional arguments @[major] and @[minor] are used to tell the
 *!   compiler to attempt to be compatible with Pike @[major].@[minor].
 *!
 *! @note
 *!   This function essentially performs
 *!   @code
 *!     program compile(mixed ... args)
 *!     {
 *!       return PikeCompiler(@@args)->compile();
 *!     }
 *!   @endcode
 *!
 *! @note
 *!   Note that @[source] must contain the complete source for a program.
 *!   It is not possible to compile a single expression or statement.
 *!
 *!   Also note that @[compile()] does not preprocess the program.
 *!   To preprocess the program you can use @[compile_string()] or
 *!   call the preprocessor manually by calling @[cpp()].
 *!
 *! @seealso
 *!   @[compile_string()], @[compile_file()], @[cpp()], @[master()],
 *!   @[CompilationHandler]
 */
static void f_compilation_env_compile(INT32 args)
{
  apply_current(CE_PIKE_COMPILER_FUN_NUM, args);
  args = 1;
  if (TYPEOF(Pike_sp[-1]) != T_OBJECT) {
    Pike_error("Bad return value from PikeCompiler().\n");
  }
  apply(Pike_sp[-1].u.object, "compile", 0);
  stack_pop_n_elems_keep_top(args);
}

/*! @decl mixed resolv(string identifier, string filename, @
 *!		       object|void handler)
 *!
 *!   Look up @[identifier] in the current context.
 *!
 *!   The default implementation calls the corresponding
 *!   function in the master object.
 */
static void f_compilation_env_resolv(INT32 args)
{
  struct pike_string *ident;
  struct pike_string *filename;
  struct object *handler = NULL;

  get_all_args("resolv", args, "%W%W.%O",
	       &ident, &filename, &handler);

  if(get_master())
  {
    DECLARE_CYCLIC();
    if(BEGIN_CYCLIC(ident, filename))
    {
      my_yyerror("Recursive module dependency in %S.", ident);
    }else{
      SET_CYCLIC_RET(1);

      APPLY_MASTER("resolv", args);
    }
    END_CYCLIC();
  } else {
    pop_n_elems(args);
    push_undefined();
  }
}

/*! @decl object get_compilation_handler(int major, int minor)
 *!
 *!   Get compatibility handler for Pike @[major].@[minor].
 *!
 *!   The default implementation calls the corresponding
 *!   function in the master object.
 *!
 *! @note
 *!   This function is typically called by
 *!   @[PikeCompiler()->get_compilation_handler()].
 *!
 *! @seealso
 *!   @[MasterObject()->get_compilation_handler()].
 */
static void f_compilation_env_get_compilation_handler(INT32 args)
{
  if(get_master())
  {
    APPLY_MASTER("get_compilation_handler", args);
  } else {
    pop_n_elems(args);
    push_undefined();
  }
}

/*! @decl mapping(string:mixed)|object get_default_module()
 *!
 *!   Get the default module for the current compatibility level
 *!   (ie typically the value returned by @[predef::all_constants()]).
 *!
 *!   The default implementation calls the corresponding function
 *!   in the master object.
 *!
 *! @returns
 *!   @mixed
 *!     @type mapping(string:mixed)|object
 *!       Constant table to use.
 *!
 *!     @type int(0..0)
 *!       Use the builtin constant table.
 *!   @endmixed
 *!
 *! @note
 *!   This function is typically called by
 *!   @[Pike_compiler()->get_default_module()].
 *!
 *! @seealso
 *!   @[MasterObject()->get_default_module()].
 */
static void f_compilation_env_get_default_module(INT32 args)
{
  if(get_master())
  {
    APPLY_MASTER("get_default_module", args);
  } else {
    pop_n_elems(args);
    push_undefined();
  }
}

/*! @decl program handle_inherit(string inh, string current_file, @
 *!                              object|void handler)
 *!
 *!   Look up an inherit @[inh].
 *!
 *!   The default implementation calls the corresponding function
 *!   in the master object.
 *!
 *! @seealso
 *!   @[MasterObject()->handle_inherit()].
 */
static void f_compilation_env_handle_inherit(INT32 args)
{
  if(get_master())
  {
    APPLY_MASTER("handle_inherit", args);
  } else {
    pop_n_elems(args);
    push_undefined();
  }
}

#if 0
/* @decl int filter_exception(SeverityLevel level, mixed err)
 *
 *   The default implementation calls
 *   @[MasterObject()->compile_exception()] for @[level] @[ERROR]
 *   and @[FATAL].
 *
 * @note
 *   This function is not implemented in Pike 7.8.
 *
 * @seealso
 *   @[MasterObject()->compile_exception()].
 */
static void f_compilation_env_filter_exception(INT32 args)
{
  int level;
  struct svalue *err;

  get_all_args("filter_exception", args, "%d%*", &level, &err);
  if (args > 2) {
    pop_n_elems(args-2);
    args = 2;
  }

#if 0
  if (level >= REPORT_WARNING) {
    if (level >= REPORT_ERROR) {
      APPLY_MASTER("compile_exception", 1);
      /* FIXME! */
    } else {
      push_int(level);
      push_string(format_exception_for_error_msg(err));
      /* FIXME! */
    }
  }
#endif

  pop_n_elems(args);
  push_undefined();
  return;
}
#endif

/*! @class PikeCompiler
 *!
 *!   The Pike compiler.
 *!
 *!   An object of this class compiles a single string
 *!   of Pike code.
 */

static void free_compilation(struct compilation *c)
{
  debug_malloc_touch(c);
  if (c->prog) {
    free_string(c->prog);
    c->prog = NULL;
  }
  if(c->handler) {
    free_object(c->handler);
    c->handler = NULL;
  }
  if(c->compat_handler) {
    free_object(c->compat_handler);
    c->compat_handler = NULL;
  }
  if(c->target) {
    free_program(c->target);
    c->target = NULL;
  }
  if(c->p) {
    free_program(c->p);
    c->p = NULL;
  }
  if(c->placeholder) {
    free_object(c->placeholder);
    c->placeholder = NULL;
  }
  if(c->lex.current_file) {
    free_string(c->lex.current_file);
    c->lex.current_file = NULL;
  }
  if(c->lex.attributes) {
    free_node(c->lex.attributes);
    c->lex.attributes = NULL;
  }
  if (c->resolve_cache) {
    free_mapping(c->resolve_cache);
    c->resolve_cache = NULL;
  }
  free_svalue(& c->default_module);
  SET_SVAL(c->default_module, T_INT, NUMBER_NUMBER, integer, 0);
  free_supporter(&c->supporter);
  verify_supporters();
}

static void run_init(struct compilation *c)
{
  debug_malloc_touch(c);

  if (c->compat_handler) free_object(c->compat_handler);
  c->compat_handler=0;

  if (c->resolve_cache) {
    free_mapping(c->resolve_cache);
    c->resolve_cache = 0;
  }

  c->lex.current_line=1;
  free_string(c->lex.current_file);
  c->lex.current_file=make_shared_string("-");

  c->lex.attributes = NULL;

  if (runtime_options & RUNTIME_STRICT_TYPES)
  {
    c->lex.pragmas = ID_STRICT_TYPES;
  } else {
    c->lex.pragmas = 0;
  }

  c->lex.end = c->prog->str + (c->prog->len << c->prog->size_shift);

  switch(c->prog->size_shift)
  {
    case 0: c->lex.current_lexer = yylex0; break;
    case 1: c->lex.current_lexer = yylex1; break;
    case 2: c->lex.current_lexer = yylex2; break;
  }

  c->lex.pos=c->prog->str;
}

static void run_init2(struct compilation *c)
{
#if 0
  int i;
  struct program *p;
  struct reference *refs;
#endif /* 0 */
  debug_malloc_touch(c);
  Pike_compiler->compiler = c;

  /* Get the proper default module. */
  safe_apply_current2(PC_GET_DEFAULT_MODULE_FUN_NUM, 0, NULL);
  if(TYPEOF(Pike_sp[-1]) == T_INT)
  {
    pop_stack();
    ref_push_mapping(get_builtin_constants());
  }
  assign_svalue(&c->default_module, Pike_sp-1);
  pop_stack();

  use_module(& c->default_module);

  Pike_compiler->compat_major=PIKE_MAJOR_VERSION;
  Pike_compiler->compat_minor=PIKE_MINOR_VERSION;

  if(c->major>=0)
    change_compiler_compatibility(c->major, c->minor);

#if 0
  /* Make all inherited private symbols that weren't overloaded
   * in the first pass local.
   */
  p = c->new_program;
  i = p->num_identifier_references;
  refs = p->identifier_references;
  while (i--) {
    if (refs[i].id_flags & ID_PRIVATE) refs[i].id_flags |= ID_INLINE;
  }
#endif /* 0 */
}

static void run_exit(struct compilation *c)
{
  debug_malloc_touch(c);

#ifdef PIKE_DEBUG
  if(c->num_used_modules)
    Pike_fatal("Failed to pop modules properly.\n");
#endif

#ifdef PIKE_DEBUG
  if (c->compilation_depth != -1) {
    fprintf(stderr, "compile(): compilation_depth is %d\n",
	    c->compilation_depth);
  }
#endif /* PIKE_DEBUG */

  if (c->resolve_cache) {
    free_mapping(c->resolve_cache);
    c->resolve_cache = NULL;
  }

  verify_supporters();
}

static void zap_placeholder(struct compilation *c)
{
  /* fprintf(stderr, "Destructing placeholder.\n"); */
  if (c->placeholder->storage) {
    yyerror("Placeholder already has storage!");
#if 0
    fprintf(stderr, "Placeholder already has storage!\n"
	    "placeholder: %p, storage: %p, prog: %p\n",
	    c->placeholder, c->placeholder->storage, c->placeholder->prog);
#endif
    debug_malloc_touch(c->placeholder);
    destruct(c->placeholder);
  } else {
    /* FIXME: Is this correct? */
    /* It would probably be nicer if it was possible to just call
     * destruct on the object, but this works too. -Hubbe
     */
    free_program(c->placeholder->prog);
    c->placeholder->prog = NULL;
    debug_malloc_touch(c->placeholder);
  }
  free_object(c->placeholder);
  c->placeholder=0;
  verify_supporters();
}

/* NOTE: Must not throw errors! */
static int run_pass1(struct compilation *c)
{
  int ret=0;

  debug_malloc_touch(c);
  run_init(c);

#if 0
  CDFPRINTF("th(%ld) compile() starting compilation_depth=%d\n",
            (long)th_self(), c->compilation_depth);
#endif

  if(c->placeholder && c->placeholder->prog != null_program) {
    yyerror("Placeholder object is not a null_program clone!");
    return 0;
  }
  debug_malloc_touch(c->placeholder);

  if(c->target && !(c->target->flags & PROGRAM_VIRGIN)) {
    yyerror("Placeholder program is not virgin!");
    return 0;
  }

  low_start_new_program(c->target,1,0,0,0);
  c->supporter.prog = Pike_compiler->new_program;

  CDFPRINTF("th(%ld) %p run_pass1() start: "
            "lock_depth:%d, compilation_depth:%d\n",
            (long)th_self(), Pike_compiler->new_program,
            lock_depth, c->compilation_depth);

  run_init2(c);

  if(c->placeholder)
  {
    if(c->placeholder->prog != null_program)
    {
      yyerror("Placeholder argument is not a null_program clone!");
      c->placeholder=0;
      debug_malloc_touch(c->placeholder);
    }else{
      free_program(c->placeholder->prog);
      add_ref(c->placeholder->prog=Pike_compiler->new_program);
      debug_malloc_touch(c->placeholder);
    }
  }

#if 0
  CDFPRINTF("th(%ld) %p compile(): First pass\n",
            (long)th_self(), Pike_compiler->new_program);
#endif

  do_yyparse();  /* Parse da program */

  if (!Pike_compiler->new_program->num_linenumbers) {
    /* The lexer didn't write an initial entry. */
    store_linenumber(0, c->lex.current_file);
#ifdef DEBUG_MALLOC
    if(strcmp(c->lex.current_file->str,"-"))
      debug_malloc_name(Pike_compiler->new_program, c->lex.current_file->str, 0);
#endif
  }

  CDFPRINTF("th(%ld) %p run_pass1() done for %s\n",
            (long)th_self(), Pike_compiler->new_program,
            c->lex.current_file->str);

  ret=unlink_current_supporter(& c->supporter);

  c->p=debug_malloc_pass(end_first_pass(0));

  run_exit(c);

  if(c->placeholder)
  {
    if(!c->p || (c->placeholder->storage))
    {
      debug_malloc_touch(c->placeholder);
      zap_placeholder(c);
    } else {
#ifdef PIKE_DEBUG
      if (c->placeholder->prog != c->p)
	Pike_fatal("Placeholder object got wrong program after first pass.\n");
#endif
      debug_malloc_touch(c->placeholder);
      c->placeholder->storage=c->p->storage_needed ?
	(char *)xcalloc(c->p->storage_needed, 1) :
	(char *)NULL;
      call_c_initializers(c->placeholder);
    }
  }

  verify_supporters();
  return ret;
}

void run_pass2(struct compilation *c)
{
  debug_malloc_touch(c);
  debug_malloc_touch(c->placeholder);

  if (!c->p) {
    c->flags &= ~(COMPILER_BUSY);
    c->flags |= COMPILER_DONE;
    return;
  }

  run_init(c);
  low_start_new_program(c->p,2,0,0,0);
  free_program(c->p);
  c->p=0;

  run_init2(c);

  CDFPRINTF("th(%ld) %p run_pass2() start: "
            "lock_depth:%d, compilation_depth:%d\n",
            (long)th_self(), Pike_compiler->new_program,
            lock_depth, c->compilation_depth);

  verify_supporters();

  do_yyparse();  /* Parse da program */

  CDFPRINTF("th(%ld) %p run_pass2() done for %s\n",
            (long)th_self(), Pike_compiler->new_program,
            c->lex.current_file->str);

  verify_supporters();

  c->p=debug_malloc_pass(end_program());

  run_exit(c);
}

static void run_cleanup(struct compilation *c, int delayed)
{
  debug_malloc_touch(c);
  debug_malloc_touch(c->placeholder);
#if 0 /* FIXME */
#ifdef PIKE_THREADS
  if (lock_depth != c->saved_lock_depth) {
    Pike_fatal("compile(): lock_depth:%d saved_lock_depth:%d\n",
	       lock_depth, c->saved_lock_depth);
  }
#endif
#endif /* PIKE_DEBUG */

  unlock_pike_compiler();

  CDFPRINTF("th(%ld) %p run_cleanup(): "
            "lock_depth:%d, compilation_depth:%d\n",
            (long)th_self(), c->target,
            lock_depth, c->compilation_depth);
  if (!c->p)
  {
    /* fprintf(stderr, "Destructing placeholder.\n"); */
    if(c->placeholder) {
      debug_malloc_touch(c->placeholder);
      zap_placeholder(c);
    }

    if(delayed && c->target)
    {
      struct program *p = c->target;

      /* Free the constants in the failed program, to untangle the
       * cyclic references we might have to this program, typically
       * in parent pointers in nested classes. */
      if (p->constants) {
	int i;
	for (i = 0; i < p->num_constants; i++) {
	  free_svalue(&p->constants[i].sval);
	  SET_SVAL(p->constants[i].sval, T_INT, NUMBER_NUMBER,
		   integer, 0);
	}
      }

      /* We have to notify the master object that
       * a previous compile() actually failed, even
       * if we did not know it at the time
       */
      CDFPRINTF("th(%ld) %p unregistering failed delayed compile.\n",
                (long) th_self(), p);
      ref_push_program(p);
      /* FIXME: Shouldn't the compilation handler be used here? */
      SAFE_APPLY_MASTER("unregister",1);
      pop_stack();

      {
#ifdef PIKE_DEBUG
	int refs = p->refs;
#endif

	/* Free the target here to avoid false alarms in the debug
	 * check below. */
	free_program (c->target);
	c->target = NULL;

#ifdef PIKE_DEBUG
	if (refs > 1) {
	  /* Other programs can have indexed out constants from p, which
	   * might be broken themselves and/or keep references to p
	   * through the parent pointer. We should find all those other
	   * programs and invalidate them too, but how can that be done?
	   * The whole delayed compilation thingie is icky icky icky... :P
	   * /mast */
	  fprintf(stderr, "Warning: Program %p still got %d "
		  "external refs after unregister:\n", p, p->refs);
	  locate_references(p);
	  fprintf (stderr, "Describing program:\n");
	  describe_something (p, T_PROGRAM, 0, 0, 0, NULL);
	}
#endif
      }
    }
  }
  else
  {
    if (c->placeholder)
    {
      if (c->target->flags & PROGRAM_FINISHED) {
	JMP_BUF rec;
	/* Initialize the placeholder. */
#ifdef PIKE_DEBUG
	if (c->placeholder->prog != c->p)
	  Pike_fatal("Placeholder object got wrong program after second pass.\n");
#endif
	if(SETJMP(rec))
	{
	  handle_compile_exception (NULL);
	  debug_malloc_touch(c->placeholder);
	  zap_placeholder(c);
	}else{
	  debug_malloc_touch(c->placeholder);
	  call_pike_initializers(c->placeholder,0);
	}
	UNSETJMP(rec);
      }
      else {
	debug_malloc_touch(c->placeholder);
	zap_placeholder(c);
      }
    }
  }
  verify_supporters();
  c->flags &= ~(COMPILER_BUSY);
  c->flags |= COMPILER_DONE;
}

static int call_delayed_pass2(struct compilation *cc, int finish)
{
  int ok = 0;
  debug_malloc_touch(cc);

  debug_malloc_touch(cc->p);

  CDFPRINTF("th(%ld) %p %s delayed compile.\n",
            (long) th_self(), cc->p, finish ? "continuing" : "cleaning up");

  /* Reenter the delayed compilation. */
  add_ref(cc->supporter.self);
  low_enter_compiler(cc->supporter.self, cc->compilation_inherit);

  if(finish && cc->p) run_pass2(cc);
  run_cleanup(cc,1);

  exit_compiler();

  debug_malloc_touch(cc);

#ifdef PIKE_DEBUG
  if(cc->supporter.dependants)
    Pike_fatal("Que???\n");
#endif
  if(cc->p) {
    ok = finish;
    free_program(cc->p); /* later */
    cc->p = NULL;
  }

  CDFPRINTF("th(%ld) %p delayed compile %s.\n",
            (long) th_self(), cc->target, ok ? "done" : "failed");

  verify_supporters();

  return ok;
}

static void compilation_event_handler(int e)
{
  struct compilation *c = THIS_COMPILATION;

  switch (e) {
  case PROG_EVENT_INIT:
    CDFPRINTF("th(%ld) compilation: INIT(%p).\n", (long) th_self(), c);
    memset(c, 0, sizeof(*c));
    c->supporter.self = Pike_fp->current_object; /* NOTE: Not ref-counted! */
    c->compilation_inherit =
      Pike_fp->context - Pike_fp->current_object->prog->inherits;
    buffer_init(&c->used_modules);
    SET_SVAL(c->default_module, T_MAPPING, 0, mapping, get_builtin_constants());
    add_ref(c->default_module.u.mapping);
    c->major = -1;
    c->minor = -1;
    c->lex.current_line = 1;
    c->lex.current_file = make_shared_string("-");
    c->compilation_depth = -1;
    break;
  case PROG_EVENT_EXIT:
    CDFPRINTF("th(%ld) compilation: EXIT(%p).\n", (long) th_self(), c);
    buffer_free(&c->used_modules);
    free_compilation(c);
    break;
  }
}

/*! @decl void report(SeverityLevel severity, @
 *!                   string filename, int linenumber, @
 *!                   string subsystem, @
 *!                   string message, mixed ... extra_args)
 *!
 *!   Report a diagnostic from the compiler.
 *!
 *!   The default implementation attempts to call the first
 *!   corresponding function in the active handlers in priority order:
 *!
 *!   @ol
 *!     @item
 *!       Call handler->report().
 *!     @item
 *!       Call handler->compile_warning() or handler->compile_error()
 *!       depending on @[severity].
 *!     @item
 *!       Call compat->report().
 *!     @item
 *!       Call compat->compile_warning() or compat->compile_error()
 *!       depending on @[severity].
 *!     @item
 *!       Fallback: Call @[CompilerEnvironment()->report()]
 *!       in the parent object.
 *!   @endol
 *!
 *!   The arguments will be as follows:
 *!   @dl
 *!     @item report()
 *!       The report() function will be called with the same arguments
 *!       as this function.
 *!     @item compile_warning()/compile_error()
 *!       Depending on the @[severity] either compile_warning()
 *!       or compile_error() will be called.
 *!
 *!       They will be called with the @[filename], @[linenumber]
 *!       and formatted @[message] as arguments.
 *!
 *!       Note that these will not be called for the @[NOTICE] severity,
 *!       and that compile_error() will be used for both @[ERROR] and
 *!       @[FATAL].
 *!   @enddl
 *!
 *! @note
 *!   In Pike 7.8 and earlier the report() function was not called
 *!   in the handlers.
 *!
 *! @seealso
 *!   @[CompilerEnvironment()->report()]
 */
static void f_compilation_report(INT32 args)
{
  struct compilation *c = THIS_COMPILATION;
  int level;
  struct pike_string *filename;
  INT_TYPE linenumber;
  struct pike_string *subsystem;
  struct pike_string *message;
  struct object *handler = NULL;
  int fun = -1;

  /* FIXME: get_all_args() ought to have a marker
   *        indicating that we accept more arguments...
   */
  get_all_args("report", args, "%d", &level);

  if ((c->handler || c->compat_handler)) {
    const char *fun_name = "compile_warning";

    if (level >= REPORT_ERROR) fun_name = "compile_error";

    if((handler = c->handler) && handler->prog) {
      if ((fun = find_identifier("report", handler->prog)) != -1) {
	apply_low(handler, fun, args);
	return;
      }
      if ((fun = find_identifier(fun_name, handler->prog)) != -1) {
	goto apply_handler;
      }
    }
    if ((handler = c->compat_handler) && handler->prog) {
      if ((fun = find_identifier("report", handler->prog)) != -1) {
	apply_low(handler, fun, args);
	return;
      }
      if ((fun = find_identifier(fun_name, handler->prog)) != -1) {
	goto apply_handler;
      }
    }
  }
  /* Nothing apropriate in any handlers.
   * Call the report() in our parent.
   */
  apply_external(1, CE_REPORT_FUN_NUM, args);
  return;

 apply_handler:
  /* Ignore informational level messages */
  if (level < REPORT_WARNING) return;
  if (args > 5) {
    f_sprintf(args - 4);
    args = 5;
  }
  get_all_args("report", args, "%d%W%+%W%W",
	       &level, &filename, &linenumber,
	       &subsystem, &message);

  ref_push_string(filename);
  push_int(linenumber);
  ref_push_string(message);
  apply_low(handler, fun, 3);
  stack_pop_n_elems_keep_top(args);
}

/*! @decl void create(string|void source, @
 *!                   CompilationHandler|void handler, @
 *!                   int|void major, int|void minor,@
 *!                   program|void target, object|void placeholder)
 *!
 *!   Create a PikeCompiler object for a source string.
 *!
 *!   This function takes a piece of Pike code as a string and
 *!   initializes a compiler object accordingly.
 *!
 *! @param source
 *!   Source code to compile.
 *!
 *! @param handler
 *!   The optional argument @[handler] is used to specify an alternative
 *!   error handler. If it is not specified the current master object
 *!   at compile time will be used.
 *!
 *! @param major
 *! @param minor
 *!   The optional arguments @[major] and @[minor] are used to tell the
 *!   compiler to attempt to be compatible with Pike @[major].@[minor].
 *!
 *! @param target
 *!   @[__empty_program()] program to fill in. The virgin program
 *!   returned by @[__empty_program()] will be modified and returned
 *!   by @[compile()] on success.
 *!
 *! @param placeholder
 *!   @[__null_program()] placeholder object to fill in. The object
 *!   will be modified into an instance of the resulting program
 *!   on successfull compile. Note that @[lfun::create()] in the
 *!   program will be called without any arguments.
 *!
 *! @note
 *!   Note that @[source] must contain the complete source for a program.
 *!   It is not possible to compile a single expression or statement.
 *!
 *!   Also note that no preprocessing is performed.
 *!   To preprocess the program you can use @[compile_string()] or
 *!   call the preprocessor manually by calling @[cpp()].
 *!
 *! @note
 *!   Note that all references to @[target] and @[placeholder] should
 *!   removed if @[compile()] failes. On failure the @[placeholder]
 *!   object will be destructed.
 *!
 *! @seealso
 *!   @[compile_string()], @[compile_file()], @[cpp()], @[master()],
 *!   @[CompilationHandler]
 */
static void f_compilation_create(INT32 args)
{
  struct pike_string *aprog = NULL;
  struct object *ahandler = NULL;/* error handler */
  int amajor = -1;
  int aminor = -1;
  struct program *atarget = NULL;
  struct object *aplaceholder = NULL;
  int dependants_ok = 1;
  struct compilation *c = THIS_COMPILATION;

  if (c->flags & COMPILER_BUSY) {
    Pike_error("PikeCompiler object is in use.\n");
  }

  STACK_LEVEL_START(args);

  get_all_args("create", args, ".%W%O%d%d%P%O",
	       &aprog, &ahandler,
	       &amajor, &aminor,
	       &atarget, &aplaceholder);

  if (args == 3) {
    SIMPLE_ARG_TYPE_ERROR("create", 4, "int");
  }

  check_c_stack(65536);

  CDFPRINTF("th(%ld) %p compilation create() enter, placeholder=%p\n",
            (long) th_self(), atarget, aplaceholder);

  debug_malloc_touch(c);

  verify_supporters();

  c->flags &= ~COMPILER_DONE;

  if (c->p) free_program(c->p);
  c->p = NULL;

  if (c->prog) free_string(c->prog);
  if ((c->prog=aprog)) add_ref(aprog);

  if (c->handler) free_object(c->handler);
  if ((c->handler=ahandler)) add_ref(ahandler);

  if (c->target) free_program(c->target);
  if ((c->target=atarget)) add_ref(atarget);

  if (c->placeholder) free_object(c->placeholder);
  if ((c->placeholder=aplaceholder)) add_ref(aplaceholder);

  c->major = amajor?amajor:-1;
  c->minor = aminor?aminor:-1;

  STACK_LEVEL_DONE(args);
  pop_n_elems(args);

  push_int(0);
}

/*! @decl program compile()
 *!
 *!   Compile the current source into a program.
 *!
 *!   This function compiles the current Pike source code
 *!   into a clonable program.
 *!
 *! @seealso
 *!   @[compile_string()], @[compile_file()], @[cpp()], @[master()],
 *!   @[CompilationHandler], @[create()]
 */
static void f_compilation_compile(INT32 args)
{
  int delay, dependants_ok = 1;
  struct program *ret;
#ifdef PIKE_DEBUG
  ONERROR tmp;
#endif
  struct compilation *c = THIS_COMPILATION;

  if (c->flags & COMPILER_BUSY) {
    Pike_error("PikeCompiler in use.\n");
  }

  get_all_args("compile", args, "");

  check_c_stack(65536);

  CDFPRINTF("th(%ld) %p f_compilation_compile() enter, "
            "placeholder=%p\n", (long) th_self(), c->target, c->placeholder);

  debug_malloc_touch(c);

  verify_supporters();

  if (c->flags & COMPILER_DONE) {
    /* Already compiled. */
    pop_n_elems(args);
    if (c->p) ref_push_program(c->p);
    else push_int(0);
    return;
  }

  if (!c->prog) {
    /* No program text. */
    low_start_new_program(c->target, 1, NULL, 0, NULL);
    c->p = end_program();
    c->flags |= COMPILER_DONE;
    pop_n_elems(args);
    ref_push_program(c->p);
    return;
  }

#ifdef PIKE_DEBUG
  SET_ONERROR(tmp, fatal_on_error,"Compiler exited with longjump!\n");
#endif

  c->flags |= COMPILER_BUSY;

  lock_pike_compiler();
#ifdef PIKE_THREADS
  c->saved_lock_depth = lock_depth;
#endif

  init_supporter(& c->supporter,
		 (supporter_callback *) call_delayed_pass2,
		 (void *)c);

  delay=run_pass1(c);
  dependants_ok = call_dependants(& c->supporter, !!c->p );
#ifdef PIKE_DEBUG
  /* FIXME */
  UNSET_ONERROR(tmp);
#endif

  if(delay)
  {
    CDFPRINTF("th(%ld) %p f_compilation_compile() finish later, "
              "placeholder=%p.\n",
              (long) th_self(), c->target, c->placeholder);
    /* finish later */
    verify_supporters();
    /* We're hanging in the supporter. */
    ret = debug_malloc_pass(c->p);
  }else{
    CDFPRINTF("th(%ld) %p f_compilation_compile() finish now.\n",
              (long) th_self(), c->target);
    /* finish now */
    run_pass2(c);
    debug_malloc_touch(c);
    run_cleanup(c,0);

    ret = debug_malloc_pass(c->p);

    debug_malloc_touch(c);

    if (!dependants_ok) {
      CDFPRINTF("th(%ld) %p f_compilation_compile() reporting failure "
                "since a dependant failed.\n",
                (long) th_self(), c->target);
      throw_error_object(fast_clone_object(compilation_error_program), 0, 0, 0,
			 "Compilation failed.\n");
    }
    if(!ret) {
      CDFPRINTF("th(%ld) %p f_compilation_compile() failed.\n",
                (long) th_self(), c->target);
      throw_error_object(fast_clone_object(compilation_error_program), 0, 0, 0,
			 "Compilation failed.\n");
    }
    debug_malloc_touch(ret);
#ifdef PIKE_DEBUG
    if (a_flag > 2) {
      dump_program_tables(ret, 0);
    }
#endif /* PIKE_DEBUG */
    verify_supporters();
  }
  pop_n_elems(args);
  if (ret)
    ref_push_program(ret);
  else
    push_int(0);
}

/*! @decl mixed resolv(string identifier, string filename, @
 *!                    object handler)
 *!
 *!   Resolve the symbol @[identifier].
 *!
 *!   The default implementation calls the corresponding function
 *!   in any active handler, and otherwise falls back to
 *!   @[CompilerEnvironment()->resolv()] in the parent object.
 */
static void f_compilation_resolv(INT32 args)
{
  struct compilation *c = THIS_COMPILATION;
  struct object *handler;
  int fun = -1;

  if (((handler = c->handler) && handler->prog &&
       ((fun = find_identifier("resolv", handler->prog)) != -1)) ||
      ((handler = c->compat_handler) && handler->prog &&
       ((fun = find_identifier("resolv", handler->prog)) != -1))) {
    apply_low(handler, fun, args);
  } else {
    apply_external(1, CE_RESOLV_FUN_NUM, args);
  }
}

/*! @decl object get_compilation_handler(int major, int minor)
 *!
 *!   Get compatibility handler for Pike @[major].@[minor].
 *!
 *! @note
 *!   This function is called by @[change_compiler_compatibility()].
 */
static void f_compilation_get_compilation_handler(INT32 args)
{
  struct compilation *c = THIS_COMPILATION;
  struct object *handler;
  int fun = -1;

  if (((handler = c->handler) && handler->prog &&
       ((fun = find_identifier("get_compilation_handler", handler->prog)) != -1)) ||
      ((handler = c->compat_handler) && handler->prog &&
       ((fun = find_identifier("get_compilation_handler", handler->prog)) != -1))) {
    apply_low(handler, fun, args);
  } else {
    apply_external(1, CE_GET_COMPILATION_HANDLER_FUN_NUM, args);
  }
}

/*! @decl mapping(string:mixed)|object get_default_module()
 *!
 *!   Get the default module for the current compatibility level
 *!   (ie typically the value returned by @[predef::all_constants()]).
 *!
 *!   The default implementation calls the corresponding function
 *!   in the current handler, the current compatibility handler
 *!   or in the parent @[CompilerEnvironment] in that order.
 *!
 *! @returns
 *!   @mixed
 *!     @type mapping(string:mixed)|object
 *!       Constant table to use.
 *!
 *!     @type int(0..0)
 *!       Use the builtin constant table.
 *!   @endmixed
 *!
 *! @note
 *!   This function is called by @[change_compiler_compatibility()].
 */
static void f_compilation_get_default_module(INT32 args)
{
  struct compilation *c = THIS_COMPILATION;
  struct object *handler;
  int fun = -1;

  if (((handler = c->handler) && handler->prog &&
       ((fun = find_identifier("get_default_module", handler->prog)) != -1)) ||
      ((handler = c->compat_handler) && handler->prog &&
       ((fun = find_identifier("get_default_module", handler->prog)) != -1))) {
    apply_low(handler, fun, args);
  } else {
    apply_external(1, CE_GET_DEFAULT_MODULE_FUN_NUM, args);
  }
}

/*! @decl void change_compiler_compatibility(int major, int minor)
 *!
 *!   Change compiler to attempt to be compatible with Pike @[major].@[minor].
 */
static void f_compilation_change_compiler_compatibility(INT32 args)
{
  struct compilation *c = THIS_COMPILATION;
  int major = -1;
  int minor = -1;

  STACK_LEVEL_START(args);

  get_all_args("change_compiler_compatibility", args, "%d%d",
	       &major, &minor);

  if ((major == -1) && (minor == -1)) {
    major = PIKE_MAJOR_VERSION;
    minor = PIKE_MINOR_VERSION;
  }

  if ((major == Pike_compiler->compat_major) &&
      (minor == Pike_compiler->compat_minor)) {
    /* Optimization: Already at this compat level. */
    pop_n_elems(args);
    push_int(0);
    return;
  }

  Pike_compiler->compat_major=major;
  Pike_compiler->compat_minor=minor;

  /* Optimization: The up to date compiler shouldn't need a compat handler. */
  if((major != PIKE_MAJOR_VERSION) || (minor != PIKE_MINOR_VERSION))
  {
    apply_current(PC_GET_COMPILATION_HANDLER_FUN_NUM, args);

    if((TYPEOF(Pike_sp[-1]) == T_OBJECT) && (Pike_sp[-1].u.object->prog))
    {
      if (SUBTYPEOF(Pike_sp[-1])) {
	/* FIXME: */
	Pike_error("Subtyped compat handlers are not supported yet.\n");
      }
      if (c->compat_handler == Pike_sp[-1].u.object) {
	/* Still at the same compat level. */
	pop_stack();
	push_int(0);
	return;
      } else {
	if(c->compat_handler) free_object(c->compat_handler);
	c->compat_handler = Pike_sp[-1].u.object;
	dmalloc_touch_svalue(Pike_sp-1);
	Pike_sp--;
      }
    } else {
      pop_stack();
      if(c->compat_handler) {
	free_object(c->compat_handler);
	c->compat_handler = NULL;
      } else {
	/* No change in compat handler. */
	push_int(0);
	return;
      }
    }
  } else {
    pop_n_elems(args);
    if (c->compat_handler) {
      free_object(c->compat_handler);
      c->compat_handler = NULL;
    } else {
      /* No change in compat handler. */
      push_int(0);
      return;
    }
  }

  STACK_LEVEL_CHECK(0);

  Pike_fp->args = 0;	/* Clean up the stack frame. */

  apply_current(PC_GET_DEFAULT_MODULE_FUN_NUM, 0);

  if(TYPEOF(Pike_sp[-1]) == T_INT)
  {
    pop_stack();
    ref_push_mapping(get_builtin_constants());
  }

  STACK_LEVEL_CHECK(1);

  assign_svalue(&c->default_module, Pike_sp-1);

  /* Replace the implicit import of all_constants() with
   * the new value.
   */
  if(c->num_used_modules)
  {
    struct svalue *dst = buffer_ptr(&c->used_modules);
    free_svalue( dst );
    dst[0] = Pike_sp[-1];
    Pike_sp--;
    dmalloc_touch_svalue(Pike_sp);
    if(Pike_compiler->module_index_cache)
    {
      free_mapping(Pike_compiler->module_index_cache);
      Pike_compiler->module_index_cache=0;
    }
  }else{
    use_module(Pike_sp-1);
    pop_stack();
  }

  STACK_LEVEL_DONE(0);
  push_int(0);
}

/*! @decl program handle_inherit(string inh)
 *!
 *!   Look up an inherit @[inh] in the current program.
 */
static void f_compilation_handle_inherit(INT32 args)
{
  struct compilation *c = THIS_COMPILATION;
  struct object *handler;
  int fun = -1;

  if (args > 1) pop_n_elems(args-1);

  ref_push_string(c->lex.current_file);
  if (c->handler && c->handler->prog) {
    ref_push_object(c->handler);
    args = 3;
  }
  else args = 2;

  if (((handler = c->handler) && handler->prog &&
       ((fun = find_identifier("handle_inherit", handler->prog)) != -1)) ||
      ((handler = c->compat_handler) && handler->prog &&
       ((fun = find_identifier("handle_inherit", handler->prog)) != -1))) {
    apply_low(handler, fun, args);
  } else {
    apply_external(1, CE_HANDLE_INHERIT_FUN_NUM, args);
  }
}

/*! @decl int(0..1) pop_type_attribute(string attribute, type a, type b)
 *!
 *!   Type attribute handler.
 *!
 *!   Called during type checking when @expr{a <= b@} and
 *!   @[a] had the type attribute @[attribute] before the
 *!   comparison.
 *!
 *!   The default implementation implements the "deprecated"
 *!   attribute.
 *!
 *! @returns
 *!   Returns @expr{1@} if the type check should be allowed
 *!   (ie @expr{__attribute__(attribute, a) <= b@}), and
 *!   @expr{0@} (zero) otherwise.
 *!
 *! @seealso
 *!   @[push_type_attribute()]
 */
static void f_compilation_pop_type_attribute(INT32 args)
{
  struct pike_string *attr;
  struct svalue *a, *b;
  struct compilation *c = THIS_COMPILATION;
  struct pike_string *deprecated_string;

  get_all_args("pop_type_attribute", args, "%W%*%*", &attr, &a, &b);

  if (Pike_compiler->compiler_pass == 2) {
    MAKE_CONST_STRING(deprecated_string, "deprecated");
    if ((attr == deprecated_string) &&
	!(c->lex.pragmas & ID_NO_DEPRECATION_WARNINGS)) {
      push_svalue(a);
      yytype_report(REPORT_WARNING, NULL, 0, NULL,
		    NULL, 0, NULL,
		    1, "Using deprecated %O value.");
    }
  }
  pop_n_elems(args);
  push_int(1);
}

/*! @decl int(0..1) push_type_attribute(string attribute, type a, type b)
 *!
 *!   Type attribute handler.
 *!
 *!   Called during type checking when @expr{a <= b@} and
 *!   @[b] had the type attribute @[attribute] before the
 *!   comparison.
 *!
 *!   The default implementation implements the "deprecated"
 *!   attribute.
 *!
 *! @returns
 *!   Returns @expr{1@} if the type check should be allowed
 *!   (ie @expr{a <= __attribute__(attribute, b)@}), and
 *!   @expr{0@} (zero) otherwise.
 *!
 *! @seealso
 *!   @[pop_type_attribute()]
 */
static void f_compilation_push_type_attribute(INT32 args)
{
  struct pike_string *attr;
  struct svalue *a, *b;
  struct compilation *c = THIS_COMPILATION;
  struct pike_string *deprecated_string;

  get_all_args("push_type_attribute", args, "%W%*%*", &attr, &a, &b);

  if (Pike_compiler->compiler_pass == 2) {
    MAKE_CONST_STRING(deprecated_string, "deprecated");
    if ((attr == deprecated_string) &&
	!(c->lex.pragmas & ID_NO_DEPRECATION_WARNINGS) &&
	!((TYPEOF(*a) == PIKE_T_TYPE) && (a->u.type == zero_type_string))) {
      /* Don't warn about setting deprecated values to zero. */
      push_svalue(b);
      yytype_report(REPORT_WARNING, NULL, 0, NULL,
		    NULL, 0, NULL,
		    1, "Using deprecated %O value.");
    }
  }
  pop_n_elems(args);
  push_int(1);
}

/*! @decl int(0..1) apply_type_attribute(string attribute, @
 *!                                      type a, type|void b)
 *!
 *!   Type attribute handler.
 *!
 *! @param attribute
 *!   Attribute that @[a] had.
 *!
 *! @param a
 *!   Type of the value being called.
 *!
 *! @param b
 *!   Type of the first argument in the call, or
 *!   @[UNDEFINED] if no more arguments.
 *!
 *!   Called during type checking when @[a] has been successfully
 *!   had a partial evaluation with the argument @[b] and
 *!   @[a] had the type attribute @[attribute] before the
 *!   evaluation.
 *!
 *!   The default implementation implements the "deprecated"
 *!   attribute.
 *!
 *! @returns
 *!   Returns @expr{1@} if the type check should be allowed
 *!   (ie @expr{__attribute__(attribute, a)(b)@}) is valid,
 *!   and @expr{0@} (zero) otherwise.
 *!
 *! @seealso
 *!   @[pop_type_attribute()], @[push_type_attribute()]
 */
static void f_compilation_apply_type_attribute(INT32 args)
{
  struct pike_string *attr;
  struct svalue *a, *b = NULL;
  struct compilation *c = THIS_COMPILATION;
  struct pike_string *deprecated_string;

  get_all_args("apply_type_attribute", args, "%W%*.%*", &attr, &a, &b);

  if (Pike_compiler->compiler_pass == 2) {
    MAKE_CONST_STRING(deprecated_string, "deprecated");
    if ((attr == deprecated_string) &&
	!(c->lex.pragmas & ID_NO_DEPRECATION_WARNINGS) &&
	(!b ||
	 ((TYPEOF(*b) == T_INT) && (SUBTYPEOF(*b) == NUMBER_UNDEFINED) &&
	  (!b->u.integer)))) {
      /* push_svalue(a); */
      yytype_report(REPORT_WARNING, NULL, 0, NULL,
		    NULL, 0, NULL,
		    0, "Calling a deprecated value.");
    }
  }
  pop_n_elems(args);
  push_int(1);
}

/*! @decl type(mixed) apply_attribute_constant(string attr, @
 *!                                            mixed value, @
 *!                                            type arg_type, @
 *!                                            void cont_type)
 *!
 *!   Handle constant arguments to attributed function argument types.
 *!
 *! @param attr
 *!   Attribute that @[arg_type] had.
 *!
 *! @param value
 *!   Constant value sent as parameter.
 *!
 *! @param arg_type
 *!   Declared type of the function argument.
 *!
 *! @param cont_type
 *!   Continuation function type after the current argument.
 *!
 *!   This function is called when a function is called
 *!   with the constant value @[value] and it has been
 *!   successfully matched against @[arg_type],
 *!   and @[arg_type] had the type attribute @[attr].
 *!
 *!   This function is typically used to perform specialized
 *!   argument checking and to allow for a strengthening
 *!   of the function type based on @[value].
 *!
 *!   The default implementation implements the @expr{"sprintf_format"@},
 *!   @expr{"sscanf_format"@} and @expr{"sscanf_76_format"@} attributes.
 *!
 *! @returns
 *!   Returns a continuation type if it succeeded in strengthening the type.
 *!
 *!   Returns @tt{UNDEFINED@} otherwise (this is not an error indication).
 *!
 *! @seealso
 *!   @[pop_type_attribute()], @[push_type_attribute()]
 */
static void f_compilation_apply_attribute_constant(INT32 args)
{
  struct compilation *c = THIS_COMPILATION;
  struct pike_string *attribute;
  struct pike_string *test;
  struct svalue *sval;
  get_all_args("apply_attribute_constant", args, "%S%*", &attribute, &sval);

  if ((TYPEOF(*sval) == T_INT) && !sval->u.integer) {
    pop_n_elems(args);
    push_undefined();
    return;
  }

  MAKE_CONST_STRING(test, "sprintf_format");
  if (attribute == test) {
    f___handle_sprintf_format(args);
    return;
  }
  MAKE_CONST_STRING(test, "strict_sprintf_format");
  if (attribute == test) {
    f___handle_sprintf_format(args);
    return;
  }
  MAKE_CONST_STRING(test, "sscanf_format");
  if (attribute == test) {
    f___handle_sscanf_format(args);
    return;
  }
  MAKE_CONST_STRING(test, "sscanf_76_format");
  if (attribute == test) {
    f___handle_sscanf_format(args);
    return;
  }
  pop_n_elems(args);
  push_undefined();
}

static void f_compilation__sprintf(INT32 args)
{
  struct compilation *c = THIS_COMPILATION;
  struct string_builder buf;
  init_string_builder_alloc(&buf, 50, 0);
  string_builder_strcat(&buf, "PikeCompiler(");
  if (c->prog) {
    string_builder_strcat(&buf, "\"\", ");
  } else {
    string_builder_strcat(&buf, "UNDEFINED, ");
  }
  if (c->handler) {
    ref_push_object(c->handler);
    string_builder_sprintf(&buf, "%O, ", Pike_sp-1);
    pop_stack();
  } else {
    string_builder_strcat(&buf, "UNDEFINED, ");
  }
  string_builder_sprintf(&buf, "%d, %d, %s, %s)",
			 c->major, c->minor,
			 c->target?"target":"UNDEFINED",
			 c->placeholder?"placeholder":"UNDEFINED");
  pop_n_elems(args);
  push_string(finish_string_builder(&buf));
}

/**
 * Fake being called via PikeCompiler()->compile()
 *
 * This function is used to set up the environment for
 * compiling C efuns and modules.
 *
 * Note: Since this is a stack frame, it will be cleaned up
 *       automatically on error, so no need to use ONERROR().
 *
 * Note: Steals a reference from ce.
 */
static void low_enter_compiler(struct object *ce, int inherit)
{
  struct pike_frame *new_frame = alloc_pike_frame();
#ifdef PROFILING
  new_frame->children_base = Pike_interpreter.accounted_time;
  new_frame->start_time = get_cpu_time() - Pike_interpreter.unlocked_time;
  new_frame->ident = PC_COMPILE_FUN_NUM;	/* Fake call of compile(). */
#endif /* PROFILING */
  new_frame->next = Pike_fp;
  new_frame->current_object = ce;
  /* Note: The compilation environment object hangs on this frame,
   *       so that it will be freed when the frame dies.
   */
  new_frame->current_program = ce->prog;
  add_ref(new_frame->current_program);
  new_frame->context = compilation_program->inherits + inherit;
  new_frame->current_storage = ce->storage + new_frame->context->storage_offset;
#ifdef PIKE_DEBUG
  if (new_frame->context->prog != compilation_program) {
    Pike_fatal("Invalid inherit for compilation context (%p != %p).\n",
	       new_frame->context->prog, compilation_program);
  }
#endif /* PIKE_DEBUG */
  new_frame->fun = new_frame->context->identifier_level + PC_COMPILE_FUN_NUM;
  new_frame->locals = Pike_sp;
  new_frame->expendible_offset = 0;
  new_frame->save_sp_offset = 0;
  new_frame->save_mark_sp = Pike_mark_sp;
  new_frame->args = 0;
  new_frame->num_args = 0;
  new_frame->num_locals = 0;
  new_frame->pc = 0;
  new_frame->return_addr = 0;
  new_frame->scope = 0;
  Pike_fp = new_frame;
}

PMOD_EXPORT void enter_compiler(struct pike_string *filename,
				INT_TYPE linenumber)
{
  struct object *ce = parent_clone_object(compilation_program,
					  compilation_environment,
					  CE_PIKE_COMPILER_FUN_NUM, 0);
  struct compilation *c;

  low_enter_compiler(ce, 0);

  c = THIS_COMPILATION;
  if (filename) {
    free_string(c->lex.current_file);
    copy_shared_string(c->lex.current_file, filename);
  }
  if (linenumber) {
    c->lex.current_line = linenumber;
  }
}

/**
 * Reverse the effect of enter_compiler().
 */
PMOD_EXPORT void exit_compiler(void)
{
#ifdef PIKE_DEBUG
  if ((Pike_fp->current_program != compilation_program) ||
      (Pike_fp->fun != PC_COMPILE_FUN_NUM)) {
    Pike_fatal("exit_compiler(): Frame stack out of whack!\n");
  }
#endif /* PIKE_DEBUG */
  POP_PIKE_FRAME();
}

/*! @class CompilerState
 *!
 *!   Keeps the state of a single program/class during compilation.
 *!
 *! @note
 *!   Not in use yet!
 */

#define THIS_PROGRAM_STATE  ((struct program_state *)(Pike_fp->current_storage))

static void program_state_event_handler(int UNUSED(event))
{
#if 0
  struct program_state *c = THIS_PROGRAM_STATE;
  switch (event) {
  case PROG_EVENT_INIT:
#define INIT
#include "compilation.h"
    break;
  case PROG_EVENT_EXIT:
#define EXIT
#include "compilation.h"
    break;
  }
#endif /* 0 */
}

/*! @endclass
 */

/*! @endclass
 */

/*! @endclass
 */

/**
 * Strap the compiler by creating the compilation program by hand.
 */
static void compile_compiler(void)
{
  struct program *p = low_allocate_program();
  struct program *p2 = compilation_program = low_allocate_program();
  struct object *co;
  struct inherit *inh;

  p->parent_info_storage = -1;
  /* p->event_handler = compilation_env_event_handler; */
  p->flags |= PROGRAM_HAS_C_METHODS;

#if 0
  /* ADD_STORAGE(struct compilation_env); */
  p->alignment_needed = ALIGNOF(struct compilation_env);
  p->storage_needed = p->xstorage + sizeof(struct compilation_env);
#endif /* 0 */

  /* Add the initial inherit, this is needed for clone_object()
   * to actually call the event handler, and for low_enter_compiler()
   * to find the storage and context. */
  p->inherits = inh = xalloc(sizeof(struct inherit));
  inh->prog = p;
  inh->inherit_level = 0;
  inh->identifier_level = 0;
  inh->parent_identifier = -1;
  inh->parent_offset = OBJECT_PARENT;
  inh->identifier_ref_offset = 0;
  inh->storage_offset = p->xstorage;
  inh->parent = NULL;
  inh->name = NULL;
  p->num_inherits = 1;

  /* Force clone_object() to accept the program...
   */
  p->flags |= PROGRAM_PASS_1_DONE;
  compilation_environment = clone_object(p, 0);
  p->flags &= ~PROGRAM_PASS_1_DONE;

  /* Once more, this time for p2...
   */

  p2->parent_info_storage = 0;
  p2->xstorage = sizeof(struct parent_info);
  p2->event_handler = compilation_event_handler;
  p2->flags |= PROGRAM_NEEDS_PARENT|PROGRAM_USES_PARENT|PROGRAM_HAS_C_METHODS;

  /* ADD_STORAGE(struct compilation); */
  p2->alignment_needed = ALIGNOF(struct compilation);
  p2->storage_needed = p2->xstorage + sizeof(struct compilation);

  p2->inherits = inh = xalloc(sizeof(struct inherit));
  inh->prog = p2;
  inh->inherit_level = 0;
  inh->identifier_level = 0;
  inh->parent_identifier = CE_PIKE_COMPILER_FUN_NUM;
  inh->parent_offset = OBJECT_PARENT;
  inh->identifier_ref_offset = 0;
  inh->storage_offset = p2->xstorage;
  inh->parent = NULL;
  inh->name = NULL;
  p2->num_inherits = 1;

  p2->flags |= PROGRAM_PASS_1_DONE;
  co = parent_clone_object(p2, compilation_environment,
			   CE_PIKE_COMPILER_FUN_NUM, 0);
  p2->flags &= ~PROGRAM_PASS_1_DONE;

  low_enter_compiler(co, 0);

  low_start_new_program(p, 1, NULL, 0, NULL);
  free_program(p);	/* Remove the extra ref we just got... */

  /* NOTE: The order of these identifiers is hard-coded in
   *       the CE_*_FUN_NUM definitions in "pike_compiler.h".
   */

  /* NB: Overloaded properly by inherit of Reporter later on. */
  ADD_FUNCTION("report", f_reporter_report,
	       tFuncV(tName("SeverityLevel", tInt03) tStr tIntPos
		      tStr tStr, tMix, tVoid),0);

  ADD_FUNCTION("compile", f_compilation_env_compile,
	       tFunc(tOr(tStr, tVoid) tOr(tObj, tVoid)
		     tOr(tInt, tVoid) tOr(tInt, tVoid)
		     tOr(tPrg(tObj), tVoid) tOr(tObj, tVoid),
		     tPrg(tObj)), 0);

  ADD_FUNCTION("resolv", f_compilation_env_resolv,
	       tFunc(tStr tStr tObj, tMix), 0);

  low_start_new_program(p2, 1, NULL, 0, NULL);

  /* low_start_new_program() has zapped the inherit we created
   * for p2 above, so we need to repair the frame pointer.
   */
  Pike_fp->context = p2->inherits;

  /* MAGIC! We're now executing inside the object being compiled,
   * and have done sufficient stuff to be able to call and use
   * the normal program building functions.
   */

  /* NOTE: The order of these identifiers is hard-coded in
   *       the PC_*_FUN_NUM definitions in "pike_compiler.h".
   */

  ADD_FUNCTION("report", f_compilation_report,
	       tFuncV(tName("SeverityLevel", tInt03) tStr tIntPos
		      tStr tStr, tMix, tVoid),0);

  ADD_FUNCTION("compile", f_compilation_compile,
	       tFunc(tNone, tPrg(tObj)), 0);

  ADD_FUNCTION("resolv", f_compilation_resolv,
	       tFunc(tStr tStr tObj, tMix), 0);

  ADD_FUNCTION("create", f_compilation_create,
	       tFunc(tOr(tStr, tVoid) tOr(tObj, tVoid)
		     tOr(tInt, tVoid) tOr(tInt, tVoid)
		     tOr(tPrg(tObj), tVoid) tOr(tObj, tVoid), tVoid),
	       ID_PROTECTED);

  ADD_FUNCTION("get_compilation_handler",
	       f_compilation_get_compilation_handler,
	       tFunc(tInt tInt, tObj), 0);

  ADD_FUNCTION("get_default_module", f_compilation_get_default_module,
	       tFunc(tNone, tOr(tMap(tStr, tMix), tObj)), 0);

  ADD_FUNCTION("change_compiler_compatibility",
	       f_compilation_change_compiler_compatibility,
	       tFunc(tInt tInt, tVoid), 0);

  ADD_FUNCTION("handle_inherit", f_compilation_handle_inherit,
	       tFunc(tStr, tPrg(tObj)), 0);

  ADD_FUNCTION("pop_type_attribute", f_compilation_pop_type_attribute,
	       tFunc(tStr tType(tMix) tType(tMix), tInt01), 0);

  ADD_FUNCTION("push_type_attribute", f_compilation_push_type_attribute,
	       tFunc(tStr tType(tMix) tType(tMix), tInt01), 0);

  ADD_FUNCTION("apply_type_attribute", f_compilation_apply_type_attribute,
	       tFunc(tStr tType(tMix) tOr(tType(tMix), tVoid), tInt01), 0);

  ADD_FUNCTION("apply_attribute_constant",
	       f_compilation_apply_attribute_constant,
	       tFunc(tStr tMix tType(tMix) tType(tFunction),
		     tType(tFunction)), 0);

  ADD_FUNCTION("_sprintf", f_compilation__sprintf,
	       tFunc(tInt tOr(tMap(tStr, tMix), tVoid), tStr), ID_PROTECTED);

  start_new_program();

  ADD_STORAGE(struct program_state);
  Pike_compiler->new_program->event_handler = program_state_event_handler;
  Pike_compiler->new_program->flags |=
    PROGRAM_NEEDS_PARENT|PROGRAM_USES_PARENT|PROGRAM_HAS_C_METHODS;

  /* Alias for report above. */
  low_define_alias(NULL, NULL, 0, 1, PC_REPORT_FUN_NUM);

  end_class("CompilerState", 0);

  /* Map some of our variables so that the gc can find them. */
  PIKE_MAP_VARIABLE("prog", OFFSETOF(compilation, prog),
		    tStr, PIKE_T_STRING, ID_HIDDEN);
  PIKE_MAP_VARIABLE("handler", OFFSETOF(compilation, handler),
		    tObj, PIKE_T_OBJECT, 0);
  PIKE_MAP_VARIABLE("compat_handler", OFFSETOF(compilation, compat_handler),
		    tObj, PIKE_T_OBJECT, 0);
  PIKE_MAP_VARIABLE("target", OFFSETOF(compilation, target),
		    tPrg(tObj), PIKE_T_PROGRAM, ID_HIDDEN);
  PIKE_MAP_VARIABLE("placeholder", OFFSETOF(compilation, placeholder),
		    tObj, PIKE_T_OBJECT, ID_HIDDEN);
  PIKE_MAP_VARIABLE("p", OFFSETOF(compilation, p),
		    tPrg(tObj), PIKE_T_PROGRAM, ID_HIDDEN);
  PIKE_MAP_VARIABLE("current_file", OFFSETOF(compilation, lex.current_file),
		    tStr, PIKE_T_STRING, ID_HIDDEN);
  PIKE_MAP_VARIABLE("default_module", OFFSETOF(compilation, default_module),
		    tOr(tMap(tStr,tMix),tObj), PIKE_T_MIXED, 0);

  /* end_class()/end_program() adds the parent_info storage once more.
   * Remove the one we added above, so that we don't get it double.
   */
  p2->xstorage = 0;

  end_class("PikeCompiler", 0);
  /* end_class()/end_program() has zapped the inherit once again,
   * so we need to repair the frame pointer.
   */
  Pike_fp->context = compilation_program->inherits;

  ADD_FUNCTION("get_compilation_handler",
	       f_compilation_env_get_compilation_handler,
	       tFunc(tInt tInt, tObj), 0);

  ADD_FUNCTION("get_default_module",
	       f_compilation_env_get_default_module,
	       tFunc(tNone, tOr(tMap(tStr, tMix), tObj)), 0);

  ADD_FUNCTION("handle_inherit", f_compilation_env_handle_inherit,
	       tFunc(tStr tStr tOr(tObj, tVoid), tPrg(tObj)), 0);

  /* Reporter */
  start_new_program();
  {
    struct svalue type_value;

    ADD_FUNCTION("report", f_reporter_report,
		 tFuncV(tName("SeverityLevel", tInt03) tStr tIntPos
			tStr tStr, tMix, tVoid),0);

    /* enum SeverityLevel { NOTICE, WARNING, ERROR, FATAL } */
    SET_SVAL(type_value, PIKE_T_TYPE, 0, type,
	     CONSTTYPE(tName("SeverityLevel", tInt03)));
    simple_add_constant("SeverityLevel", &type_value, 0);
    free_svalue(&type_value);

    add_integer_constant("NOTICE",  REPORT_NOTICE, 0);
    add_integer_constant("WARNING", REPORT_WARNING, 0);
    add_integer_constant("ERROR",   REPORT_ERROR, 0);
    add_integer_constant("FATAL",   REPORT_FATAL, 0);

    reporter_program = end_program();
  }
  add_global_program("Reporter", reporter_program);

  low_inherit(reporter_program, NULL, -1, 0, 0, 0);

  start_new_program();
  Pike_compiler->new_program->event_handler =
    compiler_environment_lock_event_handler;
  Pike_compiler->new_program->flags |= PROGRAM_DESTRUCT_IMMEDIATE;
  end_class("lock", 0);

  compilation_env_program = end_program();

  add_global_program("CompilerEnvironment", compilation_env_program);

  exit_compiler();

  ref_push_object(compilation_environment);
  low_add_constant("DefaultCompilerEnvironment", Pike_sp-1);
  pop_stack();
}

struct program *compile(struct pike_string *aprog,
			struct object *ahandler,/* error handler */
			int amajor, int aminor,
			struct program *atarget,
			struct object *aplaceholder)
{
  int delay, dependants_ok = 1;
  struct program *ret;
#ifdef PIKE_DEBUG
  ONERROR tmp;
#endif
  struct object *ce;
  struct compilation *c;

  /* FIXME! */

  Pike_fatal("Old C-level compile() function called!\n");

  CDFPRINTF("th(%ld) %p compile() enter, placeholder=%p\n",
            (long) th_self(), atarget, aplaceholder);

  ce = clone_object(compilation_program, 0);
  c = (struct compilation *)ce->storage;

  debug_malloc_touch(c);

  verify_supporters();

  c->p = NULL;
  add_ref(c->prog=aprog);
  if((c->handler=ahandler)) add_ref(ahandler);
  c->major=amajor;
  c->minor=aminor;
  if((c->target=atarget)) add_ref(atarget);
  if((c->placeholder=aplaceholder)) add_ref(aplaceholder);
  SET_SVAL(c->default_module, T_INT, NUMBER_NUMBER, integer, 0);

  if (c->handler)
  {
    if (safe_apply_handler ("get_default_module", c->handler, NULL,
			    0, BIT_MAPPING|BIT_OBJECT|BIT_ZERO)) {
      if(SAFE_IS_ZERO(Pike_sp-1))
      {
	pop_stack();
	ref_push_mapping(get_builtin_constants());
      }
    } else {
      ref_push_mapping(get_builtin_constants());
    }
  }else{
    ref_push_mapping(get_builtin_constants());
  }
  free_svalue(& c->default_module);
  move_svalue (&c->default_module, --Pike_sp);

#ifdef PIKE_DEBUG
  SET_ONERROR(tmp, fatal_on_error,"Compiler exited with longjump!\n");
#endif

  lock_pike_compiler();
#ifdef PIKE_THREADS
  c->saved_lock_depth = lock_depth;
#endif

  init_supporter(& c->supporter,
		 (supporter_callback *) call_delayed_pass2,
		 (void *)c);

  delay=run_pass1(c);
  dependants_ok = call_dependants(& c->supporter, !!c->p );
#ifdef PIKE_DEBUG
  /* FIXME */
  UNSET_ONERROR(tmp);
#endif

  if(delay)
  {
    CDFPRINTF("th(%ld) %p compile() finish later, placeholder=%p.\n",
              (long) th_self(), c->target, c->placeholder);
    /* finish later */
    add_ref(c->p);
    verify_supporters();
    return c->p; /* freed later */
  }else{
    CDFPRINTF("th(%ld) %p compile() finish now\n",
              (long) th_self(), c->target);
    /* finish now */
    if(c->p) run_pass2(c);
    debug_malloc_touch(c);
    run_cleanup(c,0);

    ret=c->p;
    /* FIXME: Looks like ret should get an extra ref here, but I'm not
     * sure. Besides, this function isn't used anymore. /mast */

    debug_malloc_touch(c);
    free_object(ce);

    if (!dependants_ok) {
      CDFPRINTF("th(%ld) %p compile() reporting failure "
                "since a dependant failed.\n",
                (long) th_self(), c->target);
      if (ret) free_program(ret);
      throw_error_object(fast_clone_object(compilation_error_program), 0, 0, 0,
			 "Compilation failed.\n");
    }
    if(!ret) {
      CDFPRINTF("th(%ld) %p compile() failed.\n",
                (long) th_self(), c->target);
      throw_error_object(fast_clone_object(compilation_error_program), 0, 0, 0,
			 "Compilation failed.\n");
    }
    debug_malloc_touch(ret);
#ifdef PIKE_DEBUG
    if (a_flag > 2) {
      dump_program_tables(ret, 0);
    }
#endif /* PIKE_DEBUG */
    verify_supporters();
    return ret;
  }
}


void push_compiler_frame(int lexical_scope)
{
  struct compiler_frame *f;
  f=ALLOC_STRUCT(compiler_frame);
  f->previous=Pike_compiler->compiler_frame;
  f->lexical_scope=lexical_scope;
  f->current_type=0;
  f->current_return_type=0;

  f->current_number_of_locals=0;
  f->max_number_of_locals=0;
  f->min_number_of_locals=0;
  f->last_block_level=-1;

  f->current_function_number=-2; /* no function */
  f->recur_label=-1;
  f->is_inline=0;
  f->num_args=-1;
  f->opt_flags = OPT_SIDE_EFFECT|OPT_EXTERNAL_DEPEND; /* FIXME: Should be 0. */
  Pike_compiler->compiler_frame=f;
}

node *low_pop_local_variables(int level, node *block)
{
  struct compilation *c = THIS_COMPILATION;
  while(Pike_compiler->compiler_frame->current_number_of_locals > level)
  {
    int e;
    e=--(Pike_compiler->compiler_frame->current_number_of_locals);
    if (block) {
      block = mknode(F_COMMA_EXPR, block,
		     mknode(F_POP_VALUE,
			    mknode(F_ASSIGN,
				   mkintnode(0),
				   mklocalnode(e, 0)),
			    NULL));
    }
    if ((Pike_compiler->compiler_pass == 2) &&
	!(Pike_compiler->compiler_frame->variable[e].flags &
	  LOCAL_VAR_IS_USED)) {
      ref_push_string(Pike_compiler->compiler_frame->variable[e].name);
      low_yyreport(REPORT_WARNING,
		   Pike_compiler->compiler_frame->variable[e].file,
		   Pike_compiler->compiler_frame->variable[e].line,
		   parser_system_string,
		   1, "Unused local variable %s.");
    }
    free_string(Pike_compiler->compiler_frame->variable[e].name);
    free_type(Pike_compiler->compiler_frame->variable[e].type);
    if(Pike_compiler->compiler_frame->variable[e].def)
      free_node(Pike_compiler->compiler_frame->variable[e].def);

    free_string(Pike_compiler->compiler_frame->variable[e].file);
  }
  return block;
}

node *pop_local_variables(int level, node *block)
{
#if 1
  struct compilation *c = THIS_COMPILATION;
  /* We need to save the variables Kuppo (but not their names) */
  if(level < Pike_compiler->compiler_frame->min_number_of_locals)
  {
    /* FIXME: Consider using flags to indicate whether a local variable
     *        actually is used from a nested scope. */
    for(;level<Pike_compiler->compiler_frame->min_number_of_locals;level++)
    {
      if ((Pike_compiler->compiler_pass == 2) &&
	  !(Pike_compiler->compiler_frame->variable[level].flags &
	    LOCAL_VAR_IS_USED)) {
	ref_push_string(Pike_compiler->compiler_frame->variable[level].name);
	low_yyreport(REPORT_WARNING,
		     Pike_compiler->compiler_frame->variable[level].file,
		     Pike_compiler->compiler_frame->variable[level].line,
		     parser_system_string,
		     1, "Unused local variable %s.");
	/* Make sure we only warn once... */
	Pike_compiler->compiler_frame->variable[level].flags |=
	  LOCAL_VAR_IS_USED;
      }
      free_string(Pike_compiler->compiler_frame->variable[level].name);
      copy_shared_string(Pike_compiler->compiler_frame->variable[level].name,
			 empty_pike_string);
      /* FIXME: Do we need to keep the filenames? */
    }
  }
#endif
  return low_pop_local_variables(level, block);
}


void pop_compiler_frame(void)
{
  struct compiler_frame *f;

  f=Pike_compiler->compiler_frame;
#ifdef PIKE_DEBUG
  if(!f)
    Pike_fatal("Popping out of compiler frames\n");
#endif

  low_pop_local_variables(0, NULL);
  if(f->current_type)
    free_type(f->current_type);

  if(f->current_return_type)
    free_type(f->current_return_type);

  Pike_compiler->compiler_frame=f->previous;
  dmfree((char *)f);
}


PMOD_EXPORT void change_compiler_compatibility(int major, int minor)
{
  CHECK_COMPILER();

  push_int(major);
  push_int(minor);

  safe_apply_current2(PC_CHANGE_COMPILER_COMPATIBILITY_FUN_NUM, 2,
		      "change_compiler_compatibility");
  pop_stack();
}

void init_pike_compiler(void)
{
#ifdef PIKE_THREADS
  co_init(&Pike_compiler_cond);
#endif

  compile_compiler();
}

void cleanup_pike_compiler(void)
{
  if (compilation_program) {
    free_program(compilation_program);
    compilation_program = 0;
  }
  if (compilation_environment) {
    free_object(compilation_environment);
    compilation_environment = 0;
  }
  if (compilation_env_program) {
    free_program(compilation_env_program);
    compilation_env_program = 0;
  }
  if (reporter_program) {
    free_program(reporter_program);
    reporter_program = 0;
  }

#ifdef PIKE_THREADS
  co_destroy(&Pike_compiler_cond);
#endif
}
